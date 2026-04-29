#pragma once

#include <stdint.h>
#include <common/ports.hpp>

class PIC {
public:
    static void disable() {
        outb(0xA1, 0xFF);
        outb(0x21, 0xFF);
    }
};
