#pragma once

#include <stdint.h>

class Cereal {
public:
    static Cereal& get();
    
    void initialize();
    bool hasInput();
    char read();
    void write(char c);
    void write(const char* str);
    
private:
    Cereal() : initialized(false) {}
    
    static constexpr uint16_t COM1 = 0x3F8;
    bool initialized;
    
    bool isTransmitEmpty();
};
