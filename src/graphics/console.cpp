#include <graphics/console.hpp>
#include <graphics/font.hpp>
#include <cpu/cereal/cereal.hpp>
#include <graphics/virtio_gpu.hpp>
#include <common/string.hpp>

inline constexpr Color Color::Rosewater(245, 224, 220);
inline constexpr Color Color::Flamingo(242, 205, 205);
inline constexpr Color Color::Pink(245, 194, 231);
inline constexpr Color Color::Mauve(203, 166, 247);
inline constexpr Color Color::Red(243, 139, 168);
inline constexpr Color Color::Maroon(235, 160, 172);
inline constexpr Color Color::Peach(250, 179, 135);
inline constexpr Color Color::Yellow(249, 226, 175);
inline constexpr Color Color::Green(166, 227, 161);
inline constexpr Color Color::Teal(148, 226, 213);
inline constexpr Color Color::Sky(137, 220, 235);
inline constexpr Color Color::Sapphire(116, 199, 236);
inline constexpr Color Color::Blue(140, 170, 238);
inline constexpr Color Color::Lavender(180, 190, 254);
inline constexpr Color Color::Text(205, 214, 244);
inline constexpr Color Color::Subtext1(186, 194, 222);
inline constexpr Color Color::Subtext0(166, 173, 200);
inline constexpr Color Color::Overlay2(147, 153, 178);
inline constexpr Color Color::Overlay1(127, 132, 156);
inline constexpr Color Color::Overlay0(108, 112, 134);
inline constexpr Color Color::Surface2(88, 91, 112);
inline constexpr Color Color::Surface1(69, 71, 90);
inline constexpr Color Color::Surface0(49, 50, 68);
inline constexpr Color Color::Base(30, 30, 46);
inline constexpr Color Color::Mantle(24, 24, 37);
inline constexpr Color Color::Crust(17, 17, 27);

inline constexpr Color Color::Black(0, 0, 0);
inline constexpr Color Color::White(255, 255, 255);

Console::Console(){
    framebuffer = nullptr;
    posX = 0;
    posY = 0;
    drawColor = 0xcad3f5;
    backgroundColor = 0x494d64;
    writeLock = 0;
    useVirtIO = false;

    resetAnsiState();
}

void Console::initialize(iFramebuffer* framebufferVal, bool virtio){
    framebuffer = framebufferVal;
    useVirtIO = virtio;
    
    Cereal::get().initialize();
}

void Console::resetAnsiState() {
    ansiState = AnsiState::NORMAL;
    ansiParamCount = 0;
    ansiParamIndex = 0;
    for (int i = 0; i < 16; i++) {
        ansiParams[i][0] = '\0';
    }
}

int Console::parseAnsiParam(int index, int defaultValue) {
    if (index >= ansiParamCount || ansiParams[index][0] == '\0') {
        return defaultValue;
    }
    
    int result = 0;
    for (int i = 0; ansiParams[index][i] != '\0'; i++) {
        result = result * 10 + (ansiParams[index][i] - '0');
    }
    return result;
}

void Console::handleSGR(int param) {
    switch (param) {
        case 0:
            drawColor = 0xFFFFFF;
            backgroundColor = 0x000000;
            break;
        case 1:
        case 2:
        case 3:
        case 4:
            break;
        case 22:
        case 23:
        case 24:
            break;

        case 30: case 31: case 32: case 33:
        case 34: case 35: case 36: case 37:
            drawColor = ANSI_COLORS[param - 30];
            break;

        case 90: case 91: case 92: case 93:
        case 94: case 95: case 96: case 97:
            drawColor = ANSI_COLORS[param - 90 + 8];
            break;

        case 40: case 41: case 42: case 43:
        case 44: case 45: case 46: case 47:
            backgroundColor = ANSI_COLORS[param - 40];
            break;

        case 100: case 101: case 102: case 103:
        case 104: case 105: case 106: case 107:
            backgroundColor = ANSI_COLORS[param - 100 + 8];
            break;
        case 39:
            drawColor = 0xFFFFFF;
            break;
        case 49:
            backgroundColor = 0x000000;
            break;
    }
}

void Console::handleCursorMovement(char command) {
    int n, m;
    
    switch (command) {
        case 'A':
            n = parseAnsiParam(0, 1);
            if (posY >= (uint64_t)(n * 16)) {
                posY -= n * 16;
            } else {
                posY = 0;
            }
            break;
        case 'B':
            n = parseAnsiParam(0, 1);
            posY += n * 16;
            if (posY + 16 > framebuffer->getHeight()) {
                posY = framebuffer->getHeight() - 16;
            }
            break;
        case 'C':
            n = parseAnsiParam(0, 1);
            posX += n * 8;
            if (posX >= framebuffer->getWidth()) {
                posX = framebuffer->getWidth() - 8;
            }
            break;
        case 'D':
            n = parseAnsiParam(0, 1);
            if (posX >= (uint64_t)(n * 8)) {
                posX -= n * 8;
            } else {
                posX = 0;
            }
            break;
        case 'H':
        case 'f':
            n = parseAnsiParam(0, 1);
            m = parseAnsiParam(1, 1);
            posY = (n - 1) * 16;
            posX = (m - 1) * 8;
            if (posY + 16 > framebuffer->getHeight()) {
                posY = framebuffer->getHeight() - 16;
            }
            if (posX >= framebuffer->getWidth()) {
                posX = framebuffer->getWidth() - 8;
            }
            break;
        case 'G':
            n = parseAnsiParam(0, 1);
            posX = (n - 1) * 8;
            if (posX >= framebuffer->getWidth()) {
                posX = framebuffer->getWidth() - 8;
            }
            break;
    }
}

void Console::handleEraseSequence(char command) {
    int n = parseAnsiParam(0, 0);
    uint64_t startX, startY, endX, endY;
    
    switch (command) {
        case 'J':
            if (n == 0) {
                startX = posX;
                startY = posY;
                endX = framebuffer->getWidth();
                endY = framebuffer->getHeight();
            } else if (n == 1) {
                startX = 0;
                startY = 0;
                endX = posX + 1;
                endY = posY + 1;
            } else if (n == 2 || n == 3) {
                startX = 0;
                startY = 0;
                endX = framebuffer->getWidth();
                endY = framebuffer->getHeight();
                posX = 0;
                posY = 0;
            } else {
                return;
            }
            
            for (uint64_t y = startY; y < endY; y++) {
                for (uint64_t x = (y == startY ? startX : 0); 
                     x < (y == endY - 1 ? endX : framebuffer->getWidth()); x++) {
                    framebuffer->putPixel(x, y, backgroundColor);
                }
            }
            break;
            
        case 'K':
            if (n == 0) {
                for (uint64_t x = posX; x < framebuffer->getWidth(); x++) {
                    for (uint64_t y = posY; y < posY + 16; y++) {
                        framebuffer->putPixel(x, y, backgroundColor);
                    }
                }
            } else if (n == 1) {
                for (uint64_t x = 0; x <= posX; x++) {
                    for (uint64_t y = posY; y < posY + 16; y++) {
                        framebuffer->putPixel(x, y, backgroundColor);
                    }
                }
            } else if (n == 2) {
                for (uint64_t x = 0; x < framebuffer->getWidth(); x++) {
                    for (uint64_t y = posY; y < posY + 16; y++) {
                        framebuffer->putPixel(x, y, backgroundColor);
                    }
                }
            }
            break;
    }
}

void Console::executeAnsiSequence(char finalByte) {
    switch (finalByte) {
        case 'm':
            if (ansiParamCount == 0) {
                handleSGR(0);
            } else {
                for (int i = 0; i < ansiParamCount; i++) {
                    handleSGR(parseAnsiParam(i, 0));
                }
            }
            break;
        case 'A': case 'B': case 'C': case 'D':
        case 'H': case 'f': case 'G':
            handleCursorMovement(finalByte);
            break;
        case 'J': case 'K':
            handleEraseSequence(finalByte);
            break;
        case 's':
            savedCursorPointer = { posX, posY };
            break;
        case 'u':
            posX = savedCursorPointer.first;
            posY = savedCursorPointer.second;
            break;
    }
    
    resetAnsiState();
}

void Console::handleAnsiChar(char c) {
    switch (ansiState) {
        case AnsiState::NORMAL:
            if (c == '\x1b') {
                ansiState = AnsiState::ESCAPE;
            } else {
                if (c == '\n') {
                    newLine();
                } else if (c == '\t') {
                    drawTextUnlocked("    ");
                } else if (c == '\r') {
                    posX = 0;
                } else if (c == '\b') {
                    if (posX >= 8) {
                        posX -= 8;
                        drawChar(' ');
                    }
                } else {
                    drawChar(c);
                    advance();
                }
            }
            break;
            
        case AnsiState::ESCAPE:
            if (c == '[') {
                ansiState = AnsiState::CSI;
                ansiParamCount = 1;
                ansiParamIndex = 0;
            } else {
                resetAnsiState();
            }
            break;
            
        case AnsiState::CSI:
            if (c >= '0' && c <= '9') {
                int len = 0;
                while (ansiParams[ansiParamCount - 1][len] != '\0') len++;
                if (len < 31) {
                    ansiParams[ansiParamCount - 1][len] = c;
                    ansiParams[ansiParamCount - 1][len + 1] = '\0';
                }
            } else if (c == ';') {
                if (ansiParamCount < 16) {
                    ansiParamCount++;
                    ansiParams[ansiParamCount - 1][0] = '\0';
                }
            } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                executeAnsiSequence(c);
            } else {
                resetAnsiState();
            }
            break;
            
        default:
            resetAnsiState();
            break;
    }
}

void Console::toString(char* ptr, int64_t num, int radix){
    char* ptr1 = ptr;
    char tmp_char;
    int tmp_value;
    bool negative = false;

    if (num == 0) {
        *ptr++ = '0';
        *ptr = '\0';
        return;
    }

    if (num < 0 && radix == 10) {
        negative = true;
        num = -num;
    }

    while (num != 0) {
        tmp_value = num % radix;
        num /= radix;
        
        if (tmp_value < 10)
            *ptr++ = "0123456789"[tmp_value];
        else
            *ptr++ = "abcdefghijklmnopqrstuvwxyz"[tmp_value - 10];
    }

    if (negative)
        *ptr++ = '-';

    *ptr = '\0';

    ptr--;
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
}

void Console::toString(char* ptr, uint64_t num, int radix){
    char* ptr1 = ptr;
    char tmp_char;
    int tmp_value;

    if (num == 0) {
        *ptr++ = '0';
        *ptr = '\0';
        return;
    }

    while (num != 0) {
        tmp_value = num % radix;
        num /= radix;
        
        if (tmp_value < 10)
            *ptr++ = "0123456789"[tmp_value];
        else
            *ptr++ = "abcdefghijklmnopqrstuvwxyz"[tmp_value - 10];
    }

    *ptr = '\0';

    ptr--;
    while (ptr1 < ptr) {
        tmp_char = *ptr;
        *ptr-- = *ptr1;
        *ptr1++ = tmp_char;
    }
}

void Console::drawChar(const char c){
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7F) return;

    const uint8_t* glyph = font_8x16[c - 0x20];

    uint32_t baseX = posX;
    uint32_t baseY = posY;

    for (uint64_t y = 0; y < 16; y++) {
        uint8_t data = glyph[y];
        for (uint64_t x = 0; x < 8; x++) {
            if (data & (0x80 >> x)) {
                uint64_t locX = baseX + x;
                uint64_t locY = baseY + y;
            
                if (locX < framebuffer->getWidth() && locY < framebuffer->getHeight()) {
                    framebuffer->putPixel(locX, locY, drawColor);
                }
            } else {
                uint64_t locX = baseX + x;
                uint64_t locY = baseY + y;
                
                if (locX < framebuffer->getWidth() && locY < framebuffer->getHeight()) {
                    framebuffer->putPixel(locX, locY, backgroundColor);
                }
            }
        }
    }
}

void Console::drawNumber(int64_t str){
    char hi[512];
    toString(hi, str, 10);
    drawText(hi);
}

void Console::drawHex(uint64_t str){
    char hi[512];
    toString(hi, str, 16);
    drawText(hi);
}

void Console::advance() {
    posX += 8;
    if (posX >= framebuffer->getWidth()) {
        newLine();
    }
}

void Console::newLine() {
    posX = 0;
    posY += 16;

    // Check if current line position is off-screen
    if (posY >= framebuffer->getHeight()) {
        scroll();
    }
}

void Console::scroll(){
    uint64_t scrollHeight = 16; // Always scroll by one character line (16 pixels)
    uint64_t pitchPixels = framebuffer->getPitch();
    uint64_t pitchBytes = pitchPixels * sizeof(uint32_t);
    uint64_t height = framebuffer->getHeight();
    uint8_t* fb = (uint8_t*)framebuffer->getRaw();

    // Only scroll if we have enough height to scroll
    if (height > scrollHeight) {
        // Move all content up by one line
        memmove(
            fb,
            fb + scrollHeight * pitchBytes,
            (height - scrollHeight) * pitchBytes
        );
    }

    // Clear the bottom line
    for (uint64_t y = height - scrollHeight; y < height; y++) {
        uint32_t* row = (uint32_t*)(fb + y * pitchBytes);
        for (uint64_t x = 0; x < pitchPixels; x++) {
            row[x] = backgroundColor;
        }
    }

    // After scrolling, cursor should be at the bottom line
    posY = height - scrollHeight;
}

void Console::flushIfNeeded() {
    if (!useVirtIO || !framebuffer) {
        return;
    }

    VirtIOGPUDriver& gpu = VirtIOGPUDriver::get();
    if (gpu.isInitialized()) {
        gpu.flush(0, 0, framebuffer->getWidth(), framebuffer->getHeight());
    }
}

void Console::drawText(const char* str){
    if (!str) return;

    lock();
    drawTextUnlocked(str);
    flushIfNeeded();
    unlock();
}

void Console::lock() {
    while (__sync_lock_test_and_set(&writeLock, 1)) {
        __asm__ volatile("pause" ::: "memory");
    }
}

void Console::unlock() {
    __sync_lock_release(&writeLock);
}

void Console::writeCharUnlocked(char c) {
    Cereal::get().write(c);
    handleAnsiChar(c);
}

void Console::drawTextUnlocked(const char* str) {
    if (!str) return;

    while (*str) {
        writeCharUnlocked(*str);
        str++;
    }
}

void Console::drawNumberUnlocked(int64_t val) {
    char buf[512];
    toString(buf, val, 10);
    drawTextUnlocked(buf);
}

void Console::drawUnsignedNumberUnlocked(uint64_t val) {
    char buf[512];
    toString(buf, val, 10);
    drawTextUnlocked(buf);
}

void Console::drawHexUnlocked(uint64_t val) {
    char buf[512];
    toString(buf, val, 16);
    drawTextUnlocked(buf);
}

void Console::setTextColor(Color color){
    drawColor = color;
}

void Console::setBackgroundColor(Color color){
    backgroundColor = color;
}
