#pragma once

#include <stdint.h>
// #include <array>
#include <cpu/cereal/cereal.hpp>
#include <cpu/idt/interrupt.hpp>
#include <cpu/process/scheduler.hpp>
#include <cpu/syscall/syscall.hpp>
#include <common/ports.hpp>
#include <graphics/console.hpp>
#include <ipc/ipc.hpp>

class Keyboard : public Interrupt {
public:
    void initialize() override {
        if (initialized) {
            return;
        }

        Cereal::get().write("[kbd:init] begin\n");
        shiftPressed = false;
        ctrlPressed = false;
        altPressed = false;
        capsLock = false;
        bufferHead = 0;
        bufferTail = 0;

        flushOutputBuffer();

        waitForInputReady();
        outb(kCommandPort, 0xAD);
        waitForInputReady();
        outb(kCommandPort, 0xA7);
        flushOutputBuffer();

        const uint8_t originalConfig = readControllerConfig();
        writeStatus("[kbd:init] cfg-old=", originalConfig);

        uint8_t initConfig = originalConfig;
        initConfig &= static_cast<uint8_t>(~(kConfigKeyboardIrq | kConfigMouseIrq));
        initConfig &= static_cast<uint8_t>(~(kConfigKeyboardDisabled | kConfigMouseDisabled));
        initConfig |= kConfigSystemFlag;
        initConfig |= kConfigTranslation;
        writeControllerConfig(initConfig);
        writeStatus("[kbd:init] cfg-init=", initConfig);

        waitForInputReady();
        outb(kCommandPort, 0xAE);

        const uint8_t resetAck = sendDeviceCommand(0xFF);
        writeStatus("[kbd:init] reset-ack=", resetAck);
        if (resetAck == kDeviceAck) {
            const uint8_t resetBat = readDataWithTimeout();
            writeStatus("[kbd:init] reset-bat=", resetBat);
        }

        const uint8_t scanSetCommandAck = sendDeviceCommand(0xF0);
        writeStatus("[kbd:init] scan-set-cmd=", scanSetCommandAck);
        if (scanSetCommandAck == kDeviceAck) {
            const uint8_t scanSetAck = sendDeviceCommand(0x02);
            writeStatus("[kbd:init] scan-set-2=", scanSetAck);
        }

        const uint8_t scanAck = sendDeviceCommand(0xF4);
        writeStatus("[kbd:init] scan-ack=", scanAck);

        waitForInputReady();
        outb(kCommandPort, 0xA8);
        const uint8_t mouseDefaultAck = sendAuxDeviceCommand(0xF6);
        writeStatus("[kbd:init] mouse-defaults=", mouseDefaultAck);
        const uint8_t mouseSampleAck = sendAuxDeviceCommandWithData(0xF3, kMouseSampleRate);
        writeStatus("[kbd:init] mouse-sample=", mouseSampleAck);
        const uint8_t mouseResolutionAck = sendAuxDeviceCommandWithData(0xE8, kMouseResolution);
        writeStatus("[kbd:init] mouse-resolution=", mouseResolutionAck);
        const uint8_t mouseStreamAck = sendAuxDeviceCommand(0xF4);
        writeStatus("[kbd:init] mouse-stream=", mouseStreamAck);
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
        writeStatus("[kbd:init] cfg-new=", runtimeConfig);

        iFramebuffer* framebuffer = Console::get().getFramebuffer();
        if (framebuffer) {
            mouseX = static_cast<int32_t>(framebuffer->getWidth() / 2);
            mouseY = static_cast<int32_t>(framebuffer->getHeight() / 2);
        }
        mouseButtons = 0;
        mousePacketIndex = 0;
        initialized = true;
        Cereal::get().write("[kbd:init] ready\n");
    }

    void Run(InterruptFrame* frame) override {
        (void)frame;
        const bool handled = drainController(true);

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
    }

    bool drainController(bool traceIrq = false) {
        uint8_t status = inb(kStatusPort);
        bool handled = false;

        if (traceIrq && kTraceInputHotPath) {
            Cereal::get().write("[kbd:irq] status=");
            writeHex(status);
            Cereal::get().write("\n");
        }

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

    bool injectChar(char c, const char* prefix = "[kbd:serial]") {
        if (c == 0) {
            return false;
        }

        if (c == '\r') {
            c = '\n';
        }

        const Event event = makeKeyEvent(c, KeyModifierNone);
        const bool windowPosted = IPCManager::get().postKeyEventToFocusedWindow(event);
        const bool inputPosted = postInputManagerEvent(event);
        const bool buffered = appendToBuffer(c, prefix);
        if (kTraceInputHotPath) {
            Cereal::get().write(prefix);
            Cereal::get().write(" inject dispatch window=");
            Cereal::get().write(windowPosted ? "ok" : "fail");
            Cereal::get().write(" input=");
            Cereal::get().write(inputPosted ? "ok" : "fail");
            Cereal::get().write(" buffered=");
            Cereal::get().write(buffered ? "ok" : "full");
            Cereal::get().write(" pending=");
            writeDec(bufferCount());
            Cereal::get().write("\n");
        }
        Scheduler::get().wakeAllBlockedProcesses();
        return true;
    }

    void publishBufferedInputToInputManager() {
        if (kTraceInputHotPath) {
            Cereal::get().write("[kbd] publish buffered to input.manager count=");
            writeDec(bufferCount());
            Cereal::get().write("\n");
        }
        while (hasKey()) {
            const char c = buffer[bufferTail];
            const Event event = makeKeyEvent(c, KeyModifierNone);
            if (!postInputManagerEvent(event)) {
                if (kTraceInputHotPath) {
                    Cereal::get().write("[kbd] publish buffered failed char=");
                    writePrintableChar(c);
                    Cereal::get().write(" remaining=");
                    writeDec(bufferCount());
                    Cereal::get().write("\n");
                }
                break;
            }
            bufferTail = (bufferTail + 1) % BUFFER_SIZE;
            if (kTraceInputHotPath) {
                Cereal::get().write("[kbd] publish buffered ok char=");
                writePrintableChar(c);
                Cereal::get().write(" remaining=");
                writeDec(bufferCount());
                Cereal::get().write("\n");
            }
        }
    }

    static Keyboard& get() {
        static Keyboard instance;
        return instance;
    }
    
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
    static constexpr int32_t kMouseSensitivityNumerator = 2;
    static constexpr int32_t kMouseSensitivityDenominator = 1;
    static constexpr int32_t kMouseAccelerationThresholdLow = 4;
    static constexpr int32_t kMouseAccelerationThresholdHigh = 10;
    static constexpr int32_t kMouseAccelerationFactorLow = 2;
    static constexpr int32_t kMouseAccelerationFactorHigh = 3;

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
        Cereal::get().write("[kbd:init] wait input timeout\n");
        return false;
    }

    bool waitForOutputReady() {
        for (int i = 0; i < kIoWaitIterations; ++i) {
            if ((inb(kStatusPort) & kStatusOutputFull) != 0) {
                return true;
            }
            ioDelay();
        }
        Cereal::get().write("[kbd:init] wait output timeout\n");
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
            writeStatus("[kbd:init] flush=", byte);
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

    void writeStatus(const char* prefix, uint8_t value) {
        Cereal::get().write(prefix);
        writeHex(value);
        Cereal::get().write("\n");
    }

    Event makeKeyEvent(char c, uint16_t modifiers) {
        Event event = {};
        event.type = EventType::Key;
        event.key.action = KeyEventAction::Press;
        event.key.modifiers = modifiers;
        event.key.keycode = static_cast<uint16_t>(static_cast<unsigned char>(c));
        if (c != '\n' && c != '\r' && c != '\b') {
            event.key.text[0] = c;
        }
        return event;
    }

    bool appendToBuffer(char c, const char* prefix) {
        int nextHead = (bufferHead + 1) % BUFFER_SIZE;
        if (nextHead == bufferTail) {
            if (kTraceInputHotPath) {
                Cereal::get().write(prefix);
                Cereal::get().write(" buffer full dropping char=");
                writePrintableChar(c);
                Cereal::get().write("\n");
            }
            return false;
        }

        buffer[bufferHead] = c;
        bufferHead = nextHead;
        if (kTraceInputHotPath) {
            Cereal::get().write(prefix);
            Cereal::get().write(" char=");
            writePrintableChar(c);
            Cereal::get().write(" pending=");
            writeDec(bufferCount());
            Cereal::get().write("\n");
        }
        return true;
    }

    void handleDataByte(uint8_t status, uint8_t scancode, const char* prefix) {
        if (status & kStatusAuxData) {
            if (kTraceInputHotPath) {
                Cereal::get().write(prefix);
                Cereal::get().write(" mouse-byte=");
                writeHex(scancode);
                Cereal::get().write("\n");
            }
            handleMouseByte(scancode);
            return;
        }

        if (kTraceInputHotPath) {
            Cereal::get().write(prefix);
            Cereal::get().write(" scancode=");
            writeHex(scancode);
            Cereal::get().write("\n");
        }

        if (scancode == kDeviceAck || scancode == kKeyboardBatOk) {
            return;
        }

        if (scancode & 0x80) {
            scancode &= 0x7F;
            if (scancode == 0x2A || scancode == 0x36) shiftPressed = false;
            else if (scancode == 0x1D) ctrlPressed = false;
            else if (scancode == 0x38) altPressed = false;
            return;
        }

        if (scancode == 0x2A || scancode == 0x36) shiftPressed = true;
        else if (scancode == 0x1D) ctrlPressed = true;
        else if (scancode == 0x38) altPressed = true;
        else if (scancode == 0x3A) capsLock = !capsLock;
        else if (scancode < 58) {
            char c = (shiftPressed ^ capsLock) ? scancodeToAsciiShift[scancode] : scancodeToAscii[scancode];
            if (c != 0) {
                uint16_t modifiers = KeyModifierNone;
                if (shiftPressed) modifiers |= KeyModifierShift;
                if (ctrlPressed) modifiers |= KeyModifierControl;
                if (altPressed) modifiers |= KeyModifierAlt;
                if (capsLock) modifiers |= KeyModifierCapsLock;
                Event event = makeKeyEvent(c, modifiers);
                const bool windowPosted = IPCManager::get().postKeyEventToFocusedWindow(event);
                const bool inputPosted = postInputManagerEvent(event);
                if (kTraceInputHotPath) {
                    Cereal::get().write(prefix);
                    Cereal::get().write(" dispatch char=");
                    writePrintableChar(c);
                    Cereal::get().write(" window=");
                    Cereal::get().write(windowPosted ? "ok" : "fail");
                    Cereal::get().write(" input=");
                    Cereal::get().write(inputPosted ? "ok" : "fail");
                    Cereal::get().write(" modifiers=");
                    writeHex(static_cast<uint8_t>(modifiers & 0xFF));
                    Cereal::get().write("\n");
                }

                if (appendToBuffer(c, prefix)) {
                    Scheduler::get().wakeAllBlockedProcesses();
                }
            }
        }
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
        mouseY -= scaledDy;
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
        event.pointer.deltaY = -scaledDy;
        event.pointer.action = (buttons != mouseButtons) ? PointerEventAction::Button : PointerEventAction::Move;
        mouseButtons = buttons;

        IPCManager::get().postServiceEvent("graphics.compositor", &event, sizeof(event));
    }

    int32_t scaleMouseDelta(int32_t delta) const {
        const int32_t absDelta = absMouseDelta(delta);
        int32_t scaled = delta * kMouseSensitivityNumerator;

        if (absDelta >= kMouseAccelerationThresholdHigh) {
            scaled *= kMouseAccelerationFactorHigh;
        } else if (absDelta >= kMouseAccelerationThresholdLow) {
            scaled *= kMouseAccelerationFactorLow;
        }

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
        if (!posted) {
            Cereal::get().write("[kbd] input.manager post failed keycode=");
            writeHex(static_cast<uint8_t>(event.key.keycode & 0xFF));
            Cereal::get().write("\n");
        }
        return posted;
    }

    void writeHex(uint8_t value) {
        static constexpr char digits[] = "0123456789abcdef";
        char buf[5] = { '0', 'x', digits[(value >> 4) & 0xF], digits[value & 0xF], '\0' };
        Cereal::get().write(buf);
    }

    void writeDec(int value) {
        char buf[11];
        int pos = 0;

        if (value == 0) {
            Cereal::get().write('0');
            return;
        }

        while (value > 0 && pos < static_cast<int>(sizeof(buf))) {
            buf[pos++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }

        while (pos > 0) {
            Cereal::get().write(buf[--pos]);
        }
    }

    void writePrintableChar(char c) {
        if (c == '\n') {
            Cereal::get().write("\\n");
        } else if (c == '\r') {
            Cereal::get().write("\\r");
        } else if (c == '\b') {
            Cereal::get().write("\\b");
        } else if (c == '\t') {
            Cereal::get().write("\\t");
        } else {
            Cereal::get().write(c);
        }
    }

    int bufferCount() const {
        if (bufferHead >= bufferTail) {
            return bufferHead - bufferTail;
        }
        return BUFFER_SIZE - bufferTail + bufferHead;
    }

    char scancodeToAscii[58] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
        0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
        '*', 0, ' '
    };
    char scancodeToAsciiShift[58] = {
        0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
        '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
        0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
        0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
        '*', 0, ' '
    };
    
    static const int BUFFER_SIZE = 256;
    char buffer[BUFFER_SIZE];
    volatile int bufferHead = 0;
    volatile int bufferTail = 0;
    
    bool initialized = false;
    bool shiftPressed;
    bool ctrlPressed;
    bool altPressed;
    bool capsLock;
    uint8_t mousePacket[3] = {0, 0, 0};
    int mousePacketIndex = 0;
    int32_t mouseX = 0;
    int32_t mouseY = 0;
    uint16_t mouseButtons = 0;
    bool mouseEnabled = false;
};
