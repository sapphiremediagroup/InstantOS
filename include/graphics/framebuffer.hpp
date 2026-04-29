#pragma once

#include <stdint.h>
#include <graphics/color.hpp>
#include <graphics/buffer.hpp>
#include <iboot/memory.hpp>

class iFramebuffer {
    public:
        iFramebuffer(Framebuffer* fb);
        ~iFramebuffer();

        void putPixel(uint64_t x, uint64_t y, Color color);
        void clear(Color color);
        void clearGradient(Color c1, Color c2);
        uint64_t getWidth();
        uint64_t getHeight();
        Color getPixel(uint64_t x, uint64_t y);
        void* getRaw();
        uint64_t getPitch();
        
        // Switch to VirtIO GPU framebuffer
        void switchToVirtIO(void* addr, uint32_t width, uint32_t height, uint32_t pitch);

        uint8_t getRedMaskSize() { return buffer.getRedMaskSize(); }
        uint8_t getGreenMaskSize() { return buffer.getGreenMaskSize(); }
        uint8_t getBlueMaskSize() { return buffer.getBlueMaskSize(); }
        PixelFormat getPixelFormat() { return buffer.getPixelFormat(); }
        uint64_t getFBSize() { return buffer.getFBSize(); }

    private:
        Buffer buffer;
};