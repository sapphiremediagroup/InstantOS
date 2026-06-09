#include <graphics/buffer.hpp>
#include <iboot/memory.hpp>
#include <common/string.hpp>

Buffer::Buffer(Framebuffer* fb) {
    address = reinterpret_cast<uint32_t*>(fb->base);
    width = fb->width;
    height = fb->height;
    pitch = fb->pixelsPerScanLine;
    red_mask_size = fb->redMask;
    green_mask_size = fb->greenMask;
    blue_mask_size = fb->blueMask;
    format = fb->format;
    size = fb->size;
}

Buffer::Buffer(void* addr, uint32_t w, uint32_t h, uint32_t p) {
    address = reinterpret_cast<uint32_t*>(addr);
    width = w;
    height = h;
    pitch = p / 4;  // Convert bytes to pixels
    format = PixelFormat::BGRReserved;
    size = static_cast<uint64_t>(p) * h;
    red_mask_size = 8;
    green_mask_size = 8;
    blue_mask_size = 8;
}

Buffer::~Buffer() {
    
}

void Buffer::putPixel(uint64_t x, uint64_t y, Color color) {
    if (x >= width || y >= height) return;
    address[y * pitch + x] = color;
}

void Buffer::clear(Color color) {
    fillRect(0, 0, width, height, color);
}

void Buffer::fillRect(uint64_t x, uint64_t y, uint64_t w, uint64_t h, Color color) {
    if (x >= width || y >= height || w == 0 || h == 0) {
        return;
    }

    if (x + w > width) {
        w = width - x;
    }
    if (y + h > height) {
        h = height - y;
    }

    uint32_t* row = address + y * pitch + x;
    for (uint64_t rowIndex = 0; rowIndex < h; ++rowIndex) {
        memset32(row, color, w);
        row += pitch;
    }
}

uint64_t Buffer::getWidth() {
    return width;
}

uint64_t Buffer::getHeight() {
    return height;
}

void* Buffer::getRaw() {
    return (void*)address;
}

uint64_t Buffer::getPitch(){
    return pitch;
}

Color Buffer::getPixel(uint64_t x, uint64_t y) {
    if (x >= width || y >= height) return Color{0, 0, 0};
    uint64_t pixelColor = address[y * pitch + x];
    Color color;
    color.r = (pixelColor >> 16) & 0xFF;
    color.g = (pixelColor >> 8) & 0xFF;
    color.b = pixelColor & 0xFF;
    return color;
}
