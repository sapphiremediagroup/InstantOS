#pragma once

#include <stdint.h>
#include <stddef.h>

class Cereal {
public:
    static Cereal& get();
    
    void initialize();
    bool hasInput();
    char read();
    void write(char c);
    void write(const char* str);

    // Everything written is also kept in a ring (the last kLogSize bytes),
    // whether or not a serial port exists: real hardware rarely has one, and
    // KernelLog (dmesg) reads it back. Copies the newest bytes, oldest first,
    // up to `capacity`; returns how many.
    size_t copyLog(char* out, size_t capacity);

private:
    Cereal() : initialized(false), lockFlag(0) {}

    static constexpr size_t kLogSize = 128 * 1024;
    char logRing[kLogSize] {};
    uint64_t logWritten = 0;   // bytes ever written; the ring holds the tail
    void record(char c) { logRing[logWritten++ % kLogSize] = c; }

    static constexpr uint16_t COM1 = 0x3F8;
    bool initialized;
    // Serializes writers so a whole write() call reaches the port unbroken
    // once more than one CPU is logging. Taken with interrupts disabled.
    volatile int lockFlag;

    bool isTransmitEmpty();
    void putRaw(char c);
    uint64_t lock();
    void unlock(uint64_t flags);
};
