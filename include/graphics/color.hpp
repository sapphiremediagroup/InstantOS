#pragma once

#include <stdint.h>

class Color {
public:
    uint8_t r;
    uint8_t g;
    uint8_t b;

    constexpr Color() : r(0), g(0), b(0) {}
    constexpr Color(uint32_t rgb) : r((rgb >> 16) & 0xFF), g((rgb >> 8) & 0xFF), b(rgb & 0xFF) {}
    constexpr Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}

    operator uint32_t() const {
        return (static_cast<uint32_t>(r) << 16) |
               (static_cast<uint32_t>(g) << 8) |
               (static_cast<uint32_t>(b));
    }

    static const Color Rosewater;
    static const Color Flamingo;
    static const Color Pink;
    static const Color Mauve;
    static const Color Red;
    static const Color Maroon;
    static const Color Peach;
    static const Color Yellow;
    static const Color Green;
    static const Color Teal;
    static const Color Sky;
    static const Color Sapphire;
    static const Color Blue;
    static const Color Lavender;
    static const Color Text;
    static const Color Subtext1;
    static const Color Subtext0;
    static const Color Overlay2;
    static const Color Overlay1;
    static const Color Overlay0;
    static const Color Surface2;
    static const Color Surface1;
    static const Color Surface0;
    static const Color Base;
    static const Color Mantle;
    static const Color Crust;

    static const Color Black;
    static const Color White;
};