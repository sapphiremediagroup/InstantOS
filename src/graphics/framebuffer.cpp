#include <graphics/framebuffer.hpp>
#include <iboot/memory.hpp>

iFramebuffer::iFramebuffer(Framebuffer* fb) : buffer(fb) {}

iFramebuffer::~iFramebuffer() {}

void iFramebuffer::putPixel(uint64_t x, uint64_t y, Color color) {
    buffer.putPixel(x, y, color);
}

void iFramebuffer::clear(Color color) {
    buffer.clear(color);
}

void iFramebuffer::clearGradient(Color c1, Color c2) {
    uint64_t w = getWidth();
    uint64_t h = getHeight();
    if (h <= 1) return;

    for (uint64_t y = 0; y < h; ++y) {
        uint8_t r = c1.r + (static_cast<int32_t>(c2.r) - c1.r) * static_cast<int64_t>(y) / (h - 1);
        uint8_t g = c1.g + (static_cast<int32_t>(c2.g) - c1.g) * static_cast<int64_t>(y) / (h - 1);
        uint8_t b = c1.b + (static_cast<int32_t>(c2.b) - c1.b) * static_cast<int64_t>(y) / (h - 1);
        Color rowCol(r, g, b);
        for (uint64_t x = 0; x < w; ++x) {
            putPixel(x, y, rowCol);
        }
    }
}

uint64_t iFramebuffer::getWidth() {
    return buffer.getWidth();
}

uint64_t iFramebuffer::getHeight() {
    return buffer.getHeight();
}

Color iFramebuffer::getPixel(uint64_t x, uint64_t y) {
    return buffer.getPixel(x, y);
}

void* iFramebuffer::getRaw(){
    return buffer.getRaw();
}

uint64_t iFramebuffer::getPitch(){
    return buffer.getPitch();
}

void iFramebuffer::switchToVirtIO(void* addr, uint32_t width, uint32_t height, uint32_t pitch) {
    buffer = Buffer(addr, width, height, pitch);
}