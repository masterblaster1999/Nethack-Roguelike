#pragma once
#include "sdl.hpp"

#include "common.hpp"
#include <string>
#include <algorithm>

// Tiny built-in 5x7 bitmap font to avoid SDL_ttf dependency.
// Includes common ASCII used by the HUD/messages.
// Unknown characters are rendered as '?'.

struct Glyph5x7 {
    // 7 rows, 5 bits used (bit 4 is leftmost).
    uint8_t rows[7];
};

inline Glyph5x7 glyph5x7(char c) {
    // Normalize to uppercase for simplicity.
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');

    switch (c) {
        case ' ': return {{0,0,0,0,0,0,0}};

        // Digits
        case '0': return {{0b01110,0b10001,0b10011,0b10101,0b11001,0b10001,0b01110}};
        case '1': return {{0b00100,0b01100,0b00100,0b00100,0b00100,0b00100,0b01110}};
        case '2': return {{0b01110,0b10001,0b00001,0b00010,0b00100,0b01000,0b11111}};
        case '3': return {{0b11110,0b00001,0b00001,0b01110,0b00001,0b00001,0b11110}};
        case '4': return {{0b00010,0b00110,0b01010,0b10010,0b11111,0b00010,0b00010}};
        case '5': return {{0b11111,0b10000,0b10000,0b11110,0b00001,0b00001,0b11110}};
        case '6': return {{0b00110,0b01000,0b10000,0b11110,0b10001,0b10001,0b01110}};
        case '7': return {{0b11111,0b00001,0b00010,0b00100,0b01000,0b01000,0b01000}};
        case '8': return {{0b01110,0b10001,0b10001,0b01110,0b10001,0b10001,0b01110}};
        case '9': return {{0b01110,0b10001,0b10001,0b01111,0b00001,0b00010,0b01100}};

        // Letters A-Z
        case 'A': return {{0b01110,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}};
        case 'B': return {{0b11110,0b10001,0b10001,0b11110,0b10001,0b10001,0b11110}};
        case 'C': return {{0b01110,0b10001,0b10000,0b10000,0b10000,0b10001,0b01110}};
        case 'D': return {{0b11100,0b10010,0b10001,0b10001,0b10001,0b10010,0b11100}};
        case 'E': return {{0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b11111}};
        case 'F': return {{0b11111,0b10000,0b10000,0b11110,0b10000,0b10000,0b10000}};
        case 'G': return {{0b01110,0b10001,0b10000,0b10000,0b10011,0b10001,0b01110}};
        case 'H': return {{0b10001,0b10001,0b10001,0b11111,0b10001,0b10001,0b10001}};
        case 'I': return {{0b01110,0b00100,0b00100,0b00100,0b00100,0b00100,0b01110}};
        case 'J': return {{0b00001,0b00001,0b00001,0b00001,0b10001,0b10001,0b01110}};
        case 'K': return {{0b10001,0b10010,0b10100,0b11000,0b10100,0b10010,0b10001}};
        case 'L': return {{0b10000,0b10000,0b10000,0b10000,0b10000,0b10000,0b11111}};
        case 'M': return {{0b10001,0b11011,0b10101,0b10101,0b10001,0b10001,0b10001}};
        case 'N': return {{0b10001,0b10001,0b11001,0b10101,0b10011,0b10001,0b10001}};
        case 'O': return {{0b01110,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}};
        case 'P': return {{0b11110,0b10001,0b10001,0b11110,0b10000,0b10000,0b10000}};
        case 'Q': return {{0b01110,0b10001,0b10001,0b10001,0b10101,0b10010,0b01101}};
        case 'R': return {{0b11110,0b10001,0b10001,0b11110,0b10100,0b10010,0b10001}};
        case 'S': return {{0b01111,0b10000,0b10000,0b01110,0b00001,0b00001,0b11110}};
        case 'T': return {{0b11111,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}};
        case 'U': return {{0b10001,0b10001,0b10001,0b10001,0b10001,0b10001,0b01110}};
        case 'V': return {{0b10001,0b10001,0b10001,0b10001,0b10001,0b01010,0b00100}};
        case 'W': return {{0b10001,0b10001,0b10001,0b10101,0b10101,0b10101,0b01010}};
        case 'X': return {{0b10001,0b10001,0b01010,0b00100,0b01010,0b10001,0b10001}};
        case 'Y': return {{0b10001,0b10001,0b01010,0b00100,0b00100,0b00100,0b00100}};
        case 'Z': return {{0b11111,0b00001,0b00010,0b00100,0b01000,0b10000,0b11111}};

        // Punctuation (subset)
        case '.': return {{0,0,0,0,0,0b01100,0b01100}};
        case ',': return {{0,0,0,0,0,0b01100,0b00100}};
        case '!': return {{0b00100,0b00100,0b00100,0b00100,0b00100,0,0b00100}};
        case '?': return {{0b01110,0b10001,0b00001,0b00010,0b00100,0,0b00100}};
        case ':': return {{0,0b01100,0b01100,0,0b01100,0b01100,0}};
        case ';': return {{0,0b01100,0b01100,0,0b01100,0b00100,0}};
        case '-': return {{0,0,0,0b11111,0,0,0}};
        case '_': return {{0,0,0,0,0,0,0b11111}};
        case '/': return {{0b00001,0b00010,0b00100,0b01000,0b10000,0,0}};
        case '\\': return {{0b10000,0b01000,0b00100,0b00010,0b00001,0,0}};
        case '>': return {{0b10000,0b01000,0b00100,0b00010,0b00100,0b01000,0b10000}};
        case '<': return {{0b00001,0b00010,0b00100,0b01000,0b00100,0b00010,0b00001}};
        case '|': return {{0b00100,0b00100,0b00100,0b00100,0b00100,0b00100,0b00100}};
        case '+': return {{0,0b00100,0b00100,0b11111,0b00100,0b00100,0}};
        case '=': return {{0,0,0b11111,0,0b11111,0,0}};
        case '(': return {{0b00100,0b01000,0b10000,0b10000,0b10000,0b01000,0b00100}};
        case ')': return {{0b00100,0b00010,0b00001,0b00001,0b00001,0b00010,0b00100}};
        case '[': return {{0b11100,0b10000,0b10000,0b10000,0b10000,0b10000,0b11100}};
        case ']': return {{0b00111,0b00001,0b00001,0b00001,0b00001,0b00001,0b00111}};
        case '\'': return {{0b00100,0b00100,0,0,0,0,0}};
        case '"': return {{0b01010,0b01010,0,0,0,0,0}};

        default:
            return {{0b01110,0b10001,0b00010,0b00100,0b00100,0,0b00100}}; // '?'
    }
}

inline void drawText5x7(SDL_Renderer* r, int x, int y, int scale, Color c, const std::string& text) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);

    int penX = x;
    for (char ch : text) {
        Glyph5x7 g = glyph5x7(ch);

        // 1 column spacing
        for (int row = 0; row < 7; ++row) {
            uint8_t bits = g.rows[row];
            for (int col = 0; col < 5; ++col) {
                int bit = 1 << (4 - col);
                if (bits & bit) {
                    SDL_Rect px{penX + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(r, &px);
                }
            }
        }

        penX += (5 + 1) * scale;
    }
}


// Word-wrapped text helper for the built-in 5x7 font.
// Returns the number of lines drawn.
inline int drawTextWrapped5x7(SDL_Renderer* r, int x, int y, int scale, Color c,
                             const std::string& text, int maxWidthPx) {
    if (!r) return 0;
    if (scale <= 0) scale = 1;

    const int charW = (5 + 1) * scale;
    int maxChars = (charW > 0) ? (maxWidthPx / charW) : 0;
    if (maxChars <= 0) maxChars = 1;

    const int lineH = (7 + 1) * scale;

    std::string line;
    line.reserve(static_cast<size_t>(maxChars));

    std::string word;
    word.reserve(32);

    int lines = 0;
    int cy = y;

    auto flushLine = [&]() {
        drawText5x7(r, x, cy, scale, c, line);
        line.clear();
        cy += lineH;
        ++lines;
    };

    auto placeWord = [&](const std::string& w) {
        if (w.empty()) return;

        if (line.empty()) {
            line = w;
            return;
        }

        if (static_cast<int>(line.size() + 1 + w.size()) <= maxChars) {
            line.push_back(' ');
            line += w;
            return;
        }

        flushLine();
        line = w;
    };

    auto emitWord = [&](const std::string& w) {
        if (w.empty()) return;

        // Fits cleanly.
        if (static_cast<int>(w.size()) <= maxChars) {
            placeWord(w);
            return;
        }

        // Word is longer than a line: break it.
        size_t start = 0;
        while (start < w.size()) {
            const size_t remaining = w.size() - start;
            const size_t n = std::min(static_cast<size_t>(maxChars), remaining);
            const std::string chunk = w.substr(start, n);

            if (!line.empty()) flushLine();
            line = chunk;

            // If there's more coming, flush immediately. Otherwise, leave it in `line`
            // so more words can be appended (rare, but keeps spacing consistent).
            start += n;
            if (start < w.size()) flushLine();
        }
    };

    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];

        if (ch == '\n') {
            emitWord(word);
            word.clear();

            if (!line.empty()) {
                flushLine();
            } else {
                cy += lineH;
                ++lines;
            }
            continue;
        }

        if (ch == ' ' || ch == '\t' || ch == '\r') {
            emitWord(word);
            word.clear();
            continue;
        }

        word.push_back(ch);
    }

    emitWord(word);

    if (!line.empty()) {
        drawText5x7(r, x, cy, scale, c, line);
        ++lines;
    }

    return lines;
}
