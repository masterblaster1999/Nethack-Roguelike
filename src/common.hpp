#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Vec2i {
    int x = 0;
    int y = 0;
};

struct Color {
    uint8_t r = 255;
    uint8_t g = 255;
    uint8_t b = 255;
    uint8_t a = 255;
};

inline int sign(int v) {
    return (v > 0) - (v < 0);
}

inline int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline std::string toUpper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}
