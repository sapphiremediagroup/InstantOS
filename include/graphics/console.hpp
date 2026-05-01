#pragma once
#include <stdint.h>
#include <common/tuple.hpp>
#include <cpu/cereal/cereal.hpp>
#include <graphics/framebuffer.hpp>

class Console {
private:
    iFramebuffer* framebuffer;
    uint64_t posX;
    uint64_t posY;
    Color drawColor;
    Color backgroundColor;
    int writeLock;
    Pair<uint64_t, uint64_t> savedCursorPointer { 0, 0 };
    bool useVirtIO;
    bool copyScrollEnabled;
    
    enum class AnsiState {
        NORMAL,
        ESCAPE,
        CSI,
        CSI_PARAM
    };
    
    AnsiState ansiState;
    char ansiParams[16][32];
    int ansiParamCount;
    int ansiParamIndex;
    
    const Color ANSI_COLORS[16] = {
        0x000000,
        0xd20f39,
        0x40a02b,
        0xfe640b,
        0x1e66f5,
        0x8839ef,
        0x209fb5,
        0xeff1f5,
        0x4c4f69,
        0xe64553,
        0xa6da95,
        0xdf8e1d,
        0x5555ff,
        0xea76cb,
        0x04a5e5,
        0xdce0e8 
    };
    
    void toString(char* ptr, int64_t num, int radix);
    void toString(char* ptr, uint64_t num, int radix);
    void drawChar(const char c);
    void advance();
    void newLine();
    void scroll();
    void flushIfNeeded();
    void lock();
    void unlock();
    void writeCharUnlocked(char c);
    void drawTextUnlocked(const char* str);
    void drawNumberUnlocked(int64_t val);
    void drawUnsignedNumberUnlocked(uint64_t val);
    void drawHexUnlocked(uint64_t val);
    
    void resetAnsiState();
    void handleAnsiChar(char c);
    void executeAnsiSequence(char finalByte);
    int parseAnsiParam(int index, int defaultValue);
    void handleSGR(int param);
    void handleCursorMovement(char command);
    void handleEraseSequence(char command);

    void printValue(const char* val) { drawTextUnlocked(val ? val : "(null)"); }
    void printValue(char* val) { drawTextUnlocked(val ? val : "(null)"); }
    void printValue(char val) { writeCharUnlocked(val); }
    void printValue(bool val) { drawTextUnlocked(val ? "true" : "false"); }
    void printValue(int val) { drawNumberUnlocked(val); }
    void printValue(long val) { drawNumberUnlocked(val); }
    void printValue(long long val) { drawNumberUnlocked(val); }
    void printValue(unsigned int val) { drawUnsignedNumberUnlocked(val); }
    void printValue(unsigned long val) { drawUnsignedNumberUnlocked(val); }
    void printValue(unsigned long long val) { drawUnsignedNumberUnlocked(val); }
    void printValue(void* val) { drawTextUnlocked("0x"); drawHexUnlocked((uint64_t)val); } 

    void printFormat(const char* s) {
        while (*s) {
            writeCharUnlocked(*s);
            s++;
        }
    }

    template<typename T, typename... Args>
    void printFormat(const char* s, T value, Args... args) {
        while (*s) {
            if (*s == '{' && *(s + 1) == '}') {
                printValue(value);
                printFormat(s + 2, args...);
                return;
            }
            writeCharUnlocked(*s);
            s++;
        }
    }

public:
    static Console& get() {
        static Console instance;
        return instance;
    }
    
    void initialize(iFramebuffer* framebufferVal, bool virtio = false);

private:
    Console();

public:
    void drawText(const char* str);
    void drawNumber(int64_t str);
    void drawHex(uint64_t str);
    void setTextColor(Color color);
    void setBackgroundColor(Color color);
    void setVirtIO(bool enabled) { useVirtIO = enabled; }
    void setCopyScrollEnabled(bool enabled) { copyScrollEnabled = enabled; }
    iFramebuffer* getFramebuffer() { return framebuffer; }

    template<typename... Args>
    void log(const char* format, Args... args) {
        if (!format) return;

        lock();
        printFormat(format, args...);
        writeCharUnlocked('\n');
        flushIfNeeded();
        unlock();
    }
};
