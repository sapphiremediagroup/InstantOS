#pragma once

#include <stdint.h>
#include <graphics/color.hpp>
#include <iboot/memory.hpp>

class Buffer {
    public:
        Buffer(Framebuffer* fb);
        Buffer(void* addr, uint32_t w, uint32_t h, uint32_t p);
        ~Buffer();

        void putPixel(uint64_t x, uint64_t y, Color color);
        void clear(Color color);
        uint64_t getWidth();
        uint64_t getHeight();
        Color getPixel(uint64_t x, uint64_t y);
        void* getRaw();
        uint64_t getPitch();

        uint8_t getRedMaskSize() { return red_mask_size; }
        uint8_t getGreenMaskSize() { return green_mask_size; }
        uint8_t getBlueMaskSize() { return blue_mask_size; }
        PixelFormat getPixelFormat() { return format; }
        uint64_t getFBSize() { return size; }
    private:
        uint32_t* address;
        uint64_t width;
        uint64_t height;
        uint64_t pitch;
        PixelFormat format;
        uint64_t size;
        uint8_t red_mask_size, green_mask_size, blue_mask_size;
};