#pragma once

#include <stdint.h>
#include <common/spinlock.hpp>
// #include <array>
#include <cpu/cereal/cereal.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/syscall/syscall.hpp>
#include <common/ports.hpp>
#include <graphics/console.hpp>
#include <interrupts/keymap.hpp>
#include <ipc/ipc.hpp>
#include <time/tsc_timer.hpp>

class Keyboard : public Interrupt {
public:
    void initialize() override {
        if (initialized) {
            return;
        }

        keyboardPresent = false;
        extendedPrefix = false;
        pauseBytes = 0;
        bufferHead = 0;
        bufferTail = 0;

        flushOutputBuffer();

        waitForInputReady();
        outb(kCommandPort, 0xAD);
        waitForInputReady();
        outb(kCommandPort, 0xA7);
        flushOutputBuffer();

        const uint8_t originalConfig = readControllerConfig();

        uint8_t initConfig = originalConfig;
        initConfig &= static_cast<uint8_t>(~(kConfigKeyboardIrq | kConfigMouseIrq));
        initConfig &= static_cast<uint8_t>(~(kConfigKeyboardDisabled | kConfigMouseDisabled));
        initConfig |= kConfigSystemFlag;
        initConfig |= kConfigTranslation;
        writeControllerConfig(initConfig);

        waitForInputReady();
        outb(kCommandPort, 0xAE);

        const uint8_t resetAck = sendDeviceCommand(0xFF);
        if (resetAck == kDeviceAck) {
            const uint8_t resetBat = readDataWithTimeout();
            keyboardPresent = resetBat == kKeyboardBatOk;
        }

        const uint8_t scanSetCommandAck = sendDeviceCommand(0xF0);
        if (scanSetCommandAck == kDeviceAck) {
            const uint8_t scanSetAck = sendDeviceCommand(0x02);
            keyboardPresent = keyboardPresent || scanSetAck == kDeviceAck;
        }

        const uint8_t scanAck = sendDeviceCommand(0xF4);
        keyboardPresent = keyboardPresent || scanAck == kDeviceAck;

        // The lights to match the lock state we start with (Num Lock on).
        if (keyboardPresent && sendDeviceCommand(0xED) == kDeviceAck &&
            sendDeviceCommand(ps2LedByte(lockLeds())) == kDeviceAck) {
            ps2LedsSent = lockLeds();
        }

        waitForInputReady();
        outb(kCommandPort, 0xA8);
        const uint8_t mouseDefaultAck = sendAuxDeviceCommand(0xF6);
        const uint8_t mouseSampleAck = sendAuxDeviceCommandWithData(0xF3, kMouseSampleRate);
        const uint8_t mouseResolutionAck = sendAuxDeviceCommandWithData(0xE8, kMouseResolution);
        const uint8_t mouseStreamAck = sendAuxDeviceCommand(0xF4);
        mouseEnabled = mouseDefaultAck == kDeviceAck &&
            mouseSampleAck == kDeviceAck &&
            mouseResolutionAck == kDeviceAck &&
            mouseStreamAck == kDeviceAck;

        uint8_t runtimeConfig = initConfig;
        runtimeConfig |= kConfigKeyboardIrq;
        if (mouseEnabled) {
            runtimeConfig |= kConfigMouseIrq;
            runtimeConfig &= static_cast<uint8_t>(~kConfigMouseDisabled);
        } else {
            runtimeConfig &= static_cast<uint8_t>(~kConfigMouseIrq);
            runtimeConfig |= kConfigMouseDisabled;
            waitForInputReady();
            outb(kCommandPort, 0xA7);
        }
        runtimeConfig &= static_cast<uint8_t>(~kConfigKeyboardDisabled);
        writeControllerConfig(runtimeConfig);

        iFramebuffer* framebuffer = Console::get().getFramebuffer();
        if (framebuffer) {
            mouseX = static_cast<int32_t>(framebuffer->getWidth() / 2);
            mouseY = static_cast<int32_t>(framebuffer->getHeight() / 2);
        }
        mouseButtons = 0;
        mousePacketIndex = 0;
        initialized = true;
    }

    void Run(InterruptFrame* frame) override {
        (void)frame;
        if (!polling_lock.tryLock()) {
            sendEOI();
            return;
        }

        const bool handled = drainController(true);

        polling_lock.unlock();

        sendEOI();

        Process* current = Scheduler::get().getCurrentProcess();
        const bool interruptedUser = frame && frame->cs == 0x23;
        const bool runningIdle = current && current->getPriority() == ProcessPriority::Idle;
        if (handled && frame && (interruptedUser || runningIdle)) {
            Scheduler::get().schedule(frame);
        }
    }
    
    bool hasKey() { return bufferHead != bufferTail; }
    
    char getKey() {
        if (!hasKey()) return 0;
    
        char c = buffer[bufferTail];
        bufferTail = (bufferTail + 1) % BUFFER_SIZE;
        return c;
    }

    // Refactored poll to safely use the buffer instead of raw port reads
    char poll() {
        return getKey();
    }

    void servicePendingInput() {
        if (!polling_lock.tryLock()) {
            return;
        }

        drainController(false);
        while (hasPendingControllerData()) {
            if (!drainController(false)) {
                break;
            }
        }

        while (Cereal::get().hasInput()) {
            if (!injectChar(Cereal::get().read())) {
                break;
            }
        }

        const uint64_t now = time_get_uptime_ms();
        repeatTick(now);
        if (ps2LedState != LedState::Idle && now >= ps2LedDeadlineMs) {
            // No ACK: give up on this update; after three, on the lights.
            ps2LedState = LedState::Idle;
            ++ps2LedFailures;
        }
        polling_lock.unlock();
    }

    bool drainController(bool traceIrq = false) {
        uint8_t status = inb(kStatusPort);
        bool handled = false;

        if ((status & kStatusOutputFull) == 0) {
            return false;
        }

        handleDataByte(status, inb(kDataPort), traceIrq ? "[kbd:irq]" : "[kbd:poll]");
        handled = true;

        while ((status = inb(kStatusPort)) & kStatusOutputFull) {
            handleDataByte(status, inb(kDataPort), traceIrq ? "[kbd:irq]" : "[kbd:poll]");
        }

        return handled;
    }

    bool hasPendingControllerData() {
        return (inb(kStatusPort) & kStatusOutputFull) != 0;
    }

    // A character from the serial console: typed as the key that makes it
    // (pressed and released), with Shift or, for control characters 1-26,
    // Control around it.
    bool injectChar(char c, const char* prefix = "[kbd:serial]") {
        uint16_t modifier = KeyNone;
        if (c >= 1 && c <= 26 && c != '\b' && c != '\t' && c != '\n' && c != '\r') {
            c = static_cast<char>('a' + c - 1);
            modifier = KeyLeftControl;
        }
        bool shift = false;
        const uint16_t key = keymap::forUsChar(c, &shift);
        if (key == KeyNone) {
            return false;
        }
        if (shift) {
            modifier = KeyLeftShift;
        }
        if (modifier != KeyNone) keyChanged(modifier, true, prefix);
        keyChanged(key, true, prefix);
        keyChanged(key, false, prefix);
        if (modifier != KeyNone) keyChanged(modifier, false, prefix);
        return true;
    }

    // A key went down or up on some keyboard; keycode is a KeyCode (the HID
    // usage). The modifiers come from the keys held, Caps and Num Lock are
    // kept here, the text comes from the US layout. Held keys repeat from
    // repeatTick; a keyboard's own repeats (PS/2 typematic) are dropped, so
    // every keyboard repeats alike.
    bool keyChanged(uint16_t keycode, bool down, const char* prefix = "[kbd]") {
        if (keycode == KeyNone || keycode > 0xFF) {
            return false;
        }
        const bool wasDown = isDown(keycode);
        if (down == wasDown) {
            return false;
        }
        setDown(keycode, down);
        if (down && (keycode == KeyCapsLock || keycode == KeyNumLock || keycode == KeyScrollLock)) {
            if (keycode == KeyCapsLock) capsLock = !capsLock;
            if (keycode == KeyNumLock) numLock = !numLock;
            if (keycode == KeyScrollLock) scrollLock = !scrollLock;
            syncPs2Leds();
        }
        emitKey(down ? KeyEventAction::Press : KeyEventAction::Release, keycode, prefix);
        if (down && keymap::repeats(keycode)) {
            repeatKey = keycode;
            nextRepeatMs = time_get_uptime_ms() + kRepeatDelayMs;
        } else if (!down && keycode == repeatKey) {
            repeatKey = KeyNone;
        }
        return true;
    }

    // The lock lights as a HID output report has them: bit 0 Num Lock, bit 1
    // Caps Lock, bit 2 Scroll Lock. The USB controllers set their keyboards'
    // from their poll; PS/2's are set here.
    uint8_t lockLeds() const {
        return static_cast<uint8_t>((numLock ? 0x01 : 0) | (capsLock ? 0x02 : 0) | (scrollLock ? 0x04 : 0));
    }

    // One HID boot-style keyboard report (USB or I2C) against the previous
    // one: the modifier byte's bits are the keys 0xE0-0xE7, the arrays the
    // other keys held. Returns false for a report to ignore: the keyboard
    // signalling rollover (more keys than it can report) with usage 0x01.
    bool hidKeyboardReport(uint8_t oldModifiers, uint8_t newModifiers,
                           const uint8_t* oldKeys, const uint8_t* newKeys, uint8_t count,
                           const char* prefix) {
        for (uint8_t i = 0; i < count; ++i) {
            if (newKeys[i] == 0x01) {
                return false;
            }
        }
        auto has = [count](const uint8_t* keys, uint8_t usage) {
            for (uint8_t i = 0; i < count; ++i) {
                if (keys[i] == usage) return true;
            }
            return false;
        };
        // Modifiers down first and up last, so a report that brings Shift and
        // a letter together types the capital.
        for (uint8_t bit = 0; bit < 8; ++bit) {
            if ((newModifiers & ~oldModifiers) & (1u << bit)) keyChanged(KeyLeftControl + bit, true, prefix);
        }
        for (uint8_t i = 0; i < count; ++i) {
            if (oldKeys[i] >= KeyA && !has(newKeys, oldKeys[i])) keyChanged(oldKeys[i], false, prefix);
        }
        for (uint8_t i = 0; i < count; ++i) {
            if (newKeys[i] >= KeyA && !has(oldKeys, newKeys[i])) keyChanged(newKeys[i], true, prefix);
        }
        for (uint8_t bit = 0; bit < 8; ++bit) {
            if ((oldModifiers & ~newModifiers) & (1u << bit)) keyChanged(KeyLeftControl + bit, false, prefix);
        }
        return true;
    }

    // One mouse report. Returns false (and posts nothing) when it changes
    // nothing: no motion, no wheel, the same buttons. A report that only
    // releases a button is not empty.
    bool injectPointerDelta(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel = 0, const char* prefix = "[mouse]") {
        if (dx == 0 && dy == 0 && wheel == 0 && static_cast<uint16_t>(buttons & 0x07) == mouseButtons) {
            return false;
        }
        // USB and I2C reports are framed, so unlike a PS/2 byte stream that
        // slipped (handleMouseByte) a large delta is real motion: a fast
        // flick, or movement the device folded together while unpolled.
        const int32_t rawDx = static_cast<int32_t>(dx);
        const int32_t rawDy = static_cast<int32_t>(dy);

        const int32_t scaledDx = scaleMouseDelta(rawDx);
        const int32_t scaledDy = scaleMouseDelta(rawDy);

        iFramebuffer* framebuffer = Console::get().getFramebuffer();
        int32_t maxX = framebuffer ? static_cast<int32_t>(framebuffer->getWidth()) - 1 : 1023;
        int32_t maxY = framebuffer ? static_cast<int32_t>(framebuffer->getHeight()) - 1 : 767;
        if (maxX < 0) maxX = 0;
        if (maxY < 0) maxY = 0;

        mouseX += scaledDx;
        mouseY += scaledDy;
        if (mouseX < 0) mouseX = 0;
        if (mouseY < 0) mouseY = 0;
        if (mouseX > maxX) mouseX = maxX;
        if (mouseY > maxY) mouseY = maxY;

        Event event = {};
        event.type = EventType::Pointer;
        event.pointer.buttons = static_cast<uint16_t>(buttons & 0x07);
        event.pointer.x = mouseX;
        event.pointer.y = mouseY;
        event.pointer.deltaX = scaledDx;
        event.pointer.deltaY = scaledDy;
        event.pointer.action = (event.pointer.buttons != mouseButtons) ? PointerEventAction::Button : PointerEventAction::Move;
        mouseButtons = event.pointer.buttons;

        IPCManager::get().postServiceEvent("graphics.compositor", &event, sizeof(event));
        postInputManagerEvent(event);

        if (wheel != 0) {
            Event scroll = {};
            scroll.type = EventType::Pointer;
            scroll.pointer.buttons = mouseButtons;
            scroll.pointer.x = mouseX;
            scroll.pointer.y = mouseY;
            scroll.pointer.deltaY = static_cast<int32_t>(wheel);
            scroll.pointer.action = PointerEventAction::Scroll;
            IPCManager::get().postServiceEvent("graphics.compositor", &scroll, sizeof(scroll));
            postInputManagerEvent(scroll);
        }

        Scheduler::get().wakeAllBlockedProcesses();
        return true;
    }

    void publishBufferedInputToInputManager() {
        while (hasKey()) {
            const char c = buffer[bufferTail];
            // An escape sequence (an arrow, F-key, ...) has no single key to
            // stand for it here: drop it, ESC through its final byte.
            const int next = (bufferTail + 1) % BUFFER_SIZE;
            if (c == 0x1B && next != bufferHead && (buffer[next] == '[' || buffer[next] == 'O')) {
                bufferTail = (next + 1) % BUFFER_SIZE;
                while (hasKey()) {
                    const char b = buffer[bufferTail];
                    bufferTail = (bufferTail + 1) % BUFFER_SIZE;
                    if (b >= 0x40 && b <= 0x7E) {
                        break;
                    }
                }
                continue;
            }
            bool shift = false;
            const uint16_t key = keymap::forUsChar(c, &shift);
            const char text = keymap::usText(key, shift, false);
            const Event event = makeKeyEvent(KeyEventAction::Press, key, text, shift ? KeyModifierShift : KeyModifierNone);
            if (!postInputManagerEvent(event)) {
                break;
            }
            bufferTail = (bufferTail + 1) % BUFFER_SIZE;
        }
    }

    static Keyboard& get() {
        static Keyboard instance;
        return instance;
    }

    bool isInitialized() const { return initialized; }
    bool isKeyboardPresent() const { return keyboardPresent; }
    bool isMouseEnabled() const { return mouseEnabled; }
    
private:
    static constexpr uint16_t kDataPort = 0x60;
    static constexpr uint16_t kStatusPort = 0x64;
    static constexpr uint16_t kCommandPort = 0x64;

    static constexpr uint8_t kStatusOutputFull = 0x01;
    static constexpr uint8_t kStatusInputFull = 0x02;
    static constexpr uint8_t kStatusAuxData = 0x20;

    static constexpr uint8_t kConfigKeyboardIrq = 0x01;
    static constexpr uint8_t kConfigMouseIrq = 0x02;
    static constexpr uint8_t kConfigSystemFlag = 0x04;
    static constexpr uint8_t kConfigKeyboardDisabled = 0x10;
    static constexpr uint8_t kConfigMouseDisabled = 0x20;
    static constexpr uint8_t kConfigTranslation = 0x40;

    static constexpr uint8_t kDeviceAck = 0xFA;
    static constexpr uint8_t kDeviceResend = 0xFE;
    static constexpr uint8_t kKeyboardBatOk = 0xAA;
    static constexpr uint8_t kMouseXOverflow = 1 << 6;
    static constexpr uint8_t kMouseYOverflow = 1 << 7;
    static constexpr uint8_t kMouseXSign = 1 << 4;
    static constexpr uint8_t kMouseYSign = 1 << 5;
    static constexpr uint8_t kMouseSampleRate = 200;
    static constexpr uint8_t kMouseResolution = 3;
    static constexpr int kIoWaitIterations = 100000;
    static constexpr bool kTraceInputHotPath = false;
    static constexpr int32_t kMouseMaxRawDelta = 80;
    static constexpr int32_t kMouseSensitivityNumerator = 1;
    static constexpr int32_t kMouseSensitivityDenominator = 1;

    void ioDelay() {
        asm volatile("pause");
    }

    bool waitForInputReady() {
        for (int i = 0; i < kIoWaitIterations; ++i) {
            if ((inb(kStatusPort) & kStatusInputFull) == 0) {
                return true;
            }
            ioDelay();
        }
        return false;
    }

    bool waitForOutputReady() {
        for (int i = 0; i < kIoWaitIterations; ++i) {
            if ((inb(kStatusPort) & kStatusOutputFull) != 0) {
                return true;
            }
            ioDelay();
        }
        return false;
    }

    uint8_t readDataWithTimeout() {
        if (!waitForOutputReady()) {
            return 0xFF;
        }
        return inb(kDataPort);
    }

    void flushOutputBuffer() {
        for (int i = 0; i < 32 && (inb(kStatusPort) & kStatusOutputFull); ++i) {
            uint8_t byte = inb(kDataPort);
        }
    }

    uint8_t sendDeviceCommand(uint8_t command) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            waitForInputReady();
            outb(kDataPort, command);
            const uint8_t response = readDataWithTimeout();
            if (response != kDeviceResend) {
                return response;
            }
        }
        return kDeviceResend;
    }

    uint8_t sendAuxDeviceCommand(uint8_t command) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            waitForInputReady();
            outb(kCommandPort, 0xD4);
            waitForInputReady();
            outb(kDataPort, command);
            const uint8_t response = readDataWithTimeout();
            if (response != kDeviceResend) {
                return response;
            }
        }
        return kDeviceResend;
    }

    uint8_t sendAuxDeviceCommandWithData(uint8_t command, uint8_t data) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const uint8_t commandAck = sendAuxDeviceCommand(command);
            if (commandAck == kDeviceResend) {
                continue;
            }
            if (commandAck != kDeviceAck) {
                return commandAck;
            }

            waitForInputReady();
            outb(kCommandPort, 0xD4);
            waitForInputReady();
            outb(kDataPort, data);
            const uint8_t dataAck = readDataWithTimeout();
            if (dataAck != kDeviceResend) {
                return dataAck;
            }
        }
        return kDeviceResend;
    }

    uint8_t readControllerConfig() {
        waitForInputReady();
        outb(kCommandPort, 0x20);
        return readDataWithTimeout();
    }

    void writeControllerConfig(uint8_t config) {
        waitForInputReady();
        outb(kCommandPort, 0x60);
        waitForInputReady();
        outb(kDataPort, config);
    }

    Event makeKeyEvent(KeyEventAction action, uint16_t keycode, char text, uint16_t modifiers) {
        Event event = {};
        event.type = EventType::Key;
        event.key.action = action;
        event.key.modifiers = modifiers;
        event.key.keycode = keycode;
        event.key.text[0] = text;
        return event;
    }

    bool isDown(uint16_t key) const { return (keysDown[key >> 3] & (1u << (key & 7))) != 0; }

    void setDown(uint16_t key, bool down) {
        if (down) keysDown[key >> 3] |= static_cast<uint8_t>(1u << (key & 7));
        else keysDown[key >> 3] &= static_cast<uint8_t>(~(1u << (key & 7)));
    }

    uint16_t currentModifiers() const {
        uint16_t modifiers = KeyModifierNone;
        if (isDown(KeyLeftShift) || isDown(KeyRightShift)) modifiers |= KeyModifierShift;
        if (isDown(KeyLeftControl) || isDown(KeyRightControl)) modifiers |= KeyModifierControl;
        if (isDown(KeyLeftAlt) || isDown(KeyRightAlt)) modifiers |= KeyModifierAlt;
        if (isDown(KeyLeftSuper) || isDown(KeyRightSuper)) modifiers |= KeyModifierSuper;
        if (capsLock) modifiers |= KeyModifierCapsLock;
        if (numLock) modifiers |= KeyModifierNumLock;
        return modifiers;
    }

    // Posts the event for a physical key. The keypad becomes its navigation
    // keys with Num Lock off; releases carry no text and type nothing.
    void emitKey(KeyEventAction action, uint16_t physical, const char* prefix) {
        const uint16_t modifiers = currentModifiers();
        uint16_t keycode = physical;
        char text = 0;
        if (physical >= KeypadSlash && physical <= KeypadPeriod) {
            const bool navigation = !numLock && keymap::keypadNavigation(physical) != physical;
            if (navigation) {
                keycode = keymap::keypadNavigation(physical);
            } else {
                text = keymap::keypadText(physical);
            }
        } else {
            text = keymap::usText(physical, (modifiers & KeyModifierShift) != 0, capsLock);
        }
        if (action == KeyEventAction::Release) {
            text = 0;
        }
        const Event event = makeKeyEvent(action, keycode, text, modifiers);
        IPCManager::get().postKeyEventToFocusedWindow(event);
        IPCManager::get().postServiceEvent("graphics.compositor", &event, sizeof(event));
        postInputManagerEvent(event);
        if (action != KeyEventAction::Release) {
            char bytes[8];
            appendToBuffer(bytes, keymap::consoleBytes(keycode, text, modifiers, bytes));
        }
        Scheduler::get().wakeAllBlockedProcesses();
    }

    // PS/2 orders the lights Scroll, Num, Caps from bit 0.
    static uint8_t ps2LedByte(uint8_t hidLeds) {
        return static_cast<uint8_t>(((hidLeds & 0x04) ? 0x01 : 0) | ((hidLeds & 0x01) ? 0x02 : 0) |
                                    ((hidLeds & 0x02) ? 0x04 : 0));
    }

    // Sets the PS/2 lights without waiting: the command (0xED) goes out
    // here, the light byte when its ACK arrives with the scancodes
    // (ps2LedAck), and a change made meanwhile follows the second ACK.
    void syncPs2Leds() {
        if (!keyboardPresent || ps2LedState != LedState::Idle || ps2LedFailures >= 3 ||
            lockLeds() == ps2LedsSent) {
            return;
        }
        ps2LedsInFlight = lockLeds();
        waitForInputReady();
        outb(kDataPort, 0xED);
        ps2LedState = LedState::WaitCommandAck;
        ps2LedDeadlineMs = time_get_uptime_ms() + kLedAckTimeoutMs;
    }

    void ps2LedAck() {
        if (ps2LedState == LedState::WaitCommandAck) {
            waitForInputReady();
            outb(kDataPort, ps2LedByte(ps2LedsInFlight));
            ps2LedState = LedState::WaitDataAck;
            ps2LedDeadlineMs = time_get_uptime_ms() + kLedAckTimeoutMs;
        } else if (ps2LedState == LedState::WaitDataAck) {
            ps2LedsSent = ps2LedsInFlight;
            ps2LedState = LedState::Idle;
            ps2LedFailures = 0;
            syncPs2Leds();
        }
    }

    // Auto-repeat of the last key pressed while it stays down: after
    // kRepeatDelayMs, then every kRepeatIntervalMs. From the boot CPU's tick.
    void repeatTick(uint64_t nowMs) {
        if (repeatKey == KeyNone || nowMs < nextRepeatMs) {
            return;
        }
        if (!isDown(repeatKey)) {
            repeatKey = KeyNone;
            return;
        }
        emitKey(KeyEventAction::Repeat, repeatKey, "[kbd:repeat]");
        // Behind (the tick was late): carry on from now rather than burst.
        nextRepeatMs = nextRepeatMs + kRepeatIntervalMs > nowMs ? nextRepeatMs + kRepeatIntervalMs
                                                                : nowMs + kRepeatIntervalMs;
    }

    // Queues what a read of the console (stdin without a terminal) gets for
    // a key, all of it or, when the buffer is full, none: a reader never sees
    // half an escape sequence.
    bool appendToBuffer(const char* bytes, uint32_t n) {
        const int used = (bufferHead - bufferTail + BUFFER_SIZE) % BUFFER_SIZE;
        if (n == 0 || used + static_cast<int>(n) >= BUFFER_SIZE) {
            return false;
        }
        for (uint32_t i = 0; i < n; ++i) {
            buffer[bufferHead] = bytes[i];
            bufferHead = (bufferHead + 1) % BUFFER_SIZE;
        }
        return true;
    }

    void handleDataByte(uint8_t status, uint8_t scancode, const char* prefix) {
        if (status & kStatusAuxData) {
            handleMouseByte(scancode);
            return;
        }

        // 0xAA is not filtered like the ACK: besides "self-test passed" (read
        // during initialize) it is left Shift's break code, and dropping it
        // left Shift stuck down.
        if (scancode == kDeviceAck) {
            ps2LedAck();
            return;
        }
        if (scancode == kDeviceResend && ps2LedState != LedState::Idle) {
            ps2LedState = LedState::Idle;
            ++ps2LedFailures;
            return;
        }

        if (scancode == 0xE0) {
            extendedPrefix = true;
            return;
        }
        // Pause sends E1 1D 45 E1 9D C5 at once and nothing on release.
        if (scancode == 0xE1) {
            pauseBytes = 2;
            return;
        }
        if (pauseBytes != 0) {
            if (--pauseBytes == 0 && scancode == 0x45) {
                keyChanged(KeyPause, true, prefix);
                keyChanged(KeyPause, false, prefix);
            }
            return;
        }

        const bool extended = extendedPrefix;
        extendedPrefix = false;
        const bool release = (scancode & 0x80) != 0;
        scancode &= 0x7F;
        const uint16_t key = extended ? keymap::fromPs2Set1Extended(scancode) : keymap::fromPs2Set1(scancode);
        keyChanged(key, !release, prefix);
    }

    void handleMouseByte(uint8_t byte) {
        if (byte == kDeviceAck || byte == kKeyboardBatOk) {
            return;
        }

        if (mousePacketIndex == 0 && (byte & 0x08) == 0) {
            return;
        }

        mousePacket[mousePacketIndex++] = byte;
        if (mousePacketIndex < 3) {
            return;
        }

        mousePacketIndex = 0;

        const uint8_t status = mousePacket[0];
        if ((status & (kMouseXOverflow | kMouseYOverflow)) != 0) {
            return;
        }

        const int32_t dx = static_cast<int8_t>(mousePacket[1]);
        const int32_t dy = static_cast<int8_t>(mousePacket[2]);
        if (((status & kMouseXSign) != 0) != (dx < 0) ||
            ((status & kMouseYSign) != 0) != (dy < 0)) {
            return;
        }
        if (absMouseDelta(dx) > kMouseMaxRawDelta || absMouseDelta(dy) > kMouseMaxRawDelta) {
            return;
        }
        const uint16_t buttons = static_cast<uint16_t>(status & 0x07);
        const int32_t scaledDx = scaleMouseDelta(dx);
        const int32_t scaledDy = scaleMouseDelta(dy);

        iFramebuffer* framebuffer = Console::get().getFramebuffer();
        int32_t maxX = framebuffer ? static_cast<int32_t>(framebuffer->getWidth()) - 1 : 1023;
        int32_t maxY = framebuffer ? static_cast<int32_t>(framebuffer->getHeight()) - 1 : 767;
        if (maxX < 0) maxX = 0;
        if (maxY < 0) maxY = 0;

        mouseX += scaledDx;
        mouseY += scaledDy;
        if (mouseX < 0) mouseX = 0;
        if (mouseY < 0) mouseY = 0;
        if (mouseX > maxX) mouseX = maxX;
        if (mouseY > maxY) mouseY = maxY;

        Event event = {};
        event.type = EventType::Pointer;
        event.pointer.buttons = buttons;
        event.pointer.x = mouseX;
        event.pointer.y = mouseY;
        event.pointer.deltaX = scaledDx;
        event.pointer.deltaY = scaledDy;
        event.pointer.action = (buttons != mouseButtons) ? PointerEventAction::Button : PointerEventAction::Move;
        mouseButtons = buttons;

        IPCManager::get().postServiceEvent("graphics.compositor", &event, sizeof(event));
    }

    int32_t scaleMouseDelta(int32_t delta) const {
        int32_t scaled = delta * kMouseSensitivityNumerator;
        scaled /= kMouseSensitivityDenominator;
        if (scaled == 0 && delta != 0) {
            scaled = delta > 0 ? 1 : -1;
        }
        return scaled;
    }

    int32_t absMouseDelta(int32_t delta) const {
        return delta < 0 ? -delta : delta;
    }

    bool postInputManagerEvent(const Event& event) {
        const bool posted = IPCManager::get().postServiceEvent("input.manager", &event, sizeof(event));
        return posted;
    }

    int bufferCount() const {
        if (bufferHead >= bufferTail) {
            return bufferHead - bufferTail;
        }
        return BUFFER_SIZE - bufferTail + bufferHead;
    }

    
    static const int BUFFER_SIZE = 256;
    char buffer[BUFFER_SIZE];
    volatile int bufferHead = 0;
    volatile int bufferTail = 0;
    
    static constexpr uint64_t kRepeatDelayMs = 500;
    static constexpr uint64_t kRepeatIntervalMs = 33;
    static constexpr uint64_t kLedAckTimeoutMs = 100;

    enum class LedState : uint8_t { Idle, WaitCommandAck, WaitDataAck };

    bool initialized = false;
    bool keyboardPresent = false;
    uint8_t keysDown[32] = {};   // one bit per KeyCode held, any keyboard
    bool capsLock = false;
    bool numLock = true;         // as most PCs boot
    bool scrollLock = false;
    LedState ps2LedState = LedState::Idle;
    uint8_t ps2LedsSent = 0xFF;  // as lockLeds(); 0xFF: unknown
    uint8_t ps2LedsInFlight = 0;
    uint8_t ps2LedFailures = 0;
    uint64_t ps2LedDeadlineMs = 0;
    uint16_t repeatKey = KeyNone;
    uint64_t nextRepeatMs = 0;
    bool extendedPrefix;
    uint8_t pauseBytes = 0;
    uint8_t mousePacket[3] = {0, 0, 0};
    int mousePacketIndex = 0;
    int32_t mouseX = 0;
    int32_t mouseY = 0;
    uint16_t mouseButtons = 0;
    bool mouseEnabled = false;
    Spinlock polling_lock{"kbd-poll"};
};
