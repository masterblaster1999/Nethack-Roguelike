#include "render.hpp"
#include "ui_font.hpp"
#include "hallucination.hpp"
#include "rng.hpp"
#include "version.hpp"
#include "action_info.hpp"
#include "spritegen3d.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <filesystem>
#include <ctime>
#include <iomanip>

namespace {

// Small RAII helper to scope SDL clip rectangles safely (restores previous clip).
struct ClipRectGuard {
    SDL_Renderer* r = nullptr;
    SDL_Rect prev{};
    bool hadPrev = false;

    ClipRectGuard(SDL_Renderer* renderer, const SDL_Rect* rect) : r(renderer) {
        if (!r) return;
        hadPrev = SDL_RenderIsClipEnabled(r) == SDL_TRUE;
        if (hadPrev) SDL_RenderGetClipRect(r, &prev);
        SDL_RenderSetClipRect(r, rect);
    }

    ~ClipRectGuard() {
        if (!r) return;
        if (hadPrev) SDL_RenderSetClipRect(r, &prev);
        else SDL_RenderSetClipRect(r, nullptr);
    }

    ClipRectGuard(const ClipRectGuard&) = delete;
    ClipRectGuard& operator=(const ClipRectGuard&) = delete;
};

// Clamp any integer to SDL's Uint8 channel range.
inline Uint8 clampToU8(int v) {
    if (v < 0) return Uint8{0};
    if (v > 255) return Uint8{255};
    return static_cast<Uint8>(v);
}

// Fit a string to a fixed character width by truncating the end with "...".
inline std::string fitToChars(const std::string& s, int maxChars) {
    if (maxChars <= 0) return std::string();
    if (static_cast<int>(s.size()) <= maxChars) return s;
    if (maxChars <= 3) return s.substr(0, static_cast<size_t>(maxChars));
    return s.substr(0, static_cast<size_t>(maxChars - 3)) + "...";
}

// Fit a string to a fixed character width using a *middle* ellipsis, preserving
// both the beginning and end (useful for HUD lines that end with controls).
inline std::string fitToCharsMiddle(const std::string& s, int maxChars) {
    if (maxChars <= 0) return std::string();
    if (static_cast<int>(s.size()) <= maxChars) return s;
    if (maxChars <= 3) return s.substr(0, static_cast<size_t>(maxChars));

    const int avail = maxChars - 3;
    const int head = avail / 2;
    const int tail = avail - head;

    if (head <= 0 || tail <= 0) {
        return s.substr(0, static_cast<size_t>(maxChars - 3)) + "...";
    }

    return s.substr(0, static_cast<size_t>(head)) + "..." + s.substr(s.size() - static_cast<size_t>(tail));
}

// Basic ASCII-ish word wrap for the fixed-width 5x7 UI font.
// Returns at least one line and caps output to maxLines.
// The last line is ellipsized if text overflows maxLines.
inline std::vector<std::string> wrapToChars(const std::string& s, int maxChars, int maxLines) {
    std::vector<std::string> out;
    if (maxChars <= 0 || maxLines <= 0) {
        out.push_back(std::string());
        return out;
    }

    size_t pos = 0;
    while (pos < s.size() && static_cast<int>(out.size()) < maxLines) {
        // Skip leading spaces for the next line.
        while (pos < s.size() && s[pos] == ' ') ++pos;
        if (pos >= s.size()) break;

        size_t end = std::min(s.size(), pos + static_cast<size_t>(maxChars));
        if (end >= s.size()) {
            out.push_back(s.substr(pos));
            pos = end;
            break;
        }

        // Prefer breaking on the last space inside the window.
        size_t space = s.rfind(' ', end);
        if (space != std::string::npos && space > pos) {
            end = space;
        }

        std::string line = s.substr(pos, end - pos);
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out.push_back(line);

        pos = end;
    }

    if (out.empty()) out.push_back(std::string());

    // If we ran out of lines but still have remaining text, fold it into the last
    // line and ellipsize.
    if (pos < s.size() && !out.empty()) {
        std::string merged = out.back();
        if (!merged.empty()) merged += " ";
        merged += s.substr(pos);
        out.back() = fitToChars(merged, maxChars);
    }

    // Ensure no line exceeds maxChars (defensive).
    for (std::string& line : out) {
        if (static_cast<int>(line.size()) > maxChars) {
            line = fitToChars(line, maxChars);
        }
    }

    return out;
}

// ------------------------------------------------------------
// Keybind UI formatting helpers
// ------------------------------------------------------------
// Keybinds are stored/edited as parseable tokens (e.g. "cmd+shift+slash"),
// but for the HUD we want a friendlier presentation ("CMD+?", "<", "ENTER").
// This layer is *display-only*; it does not change the underlying config.

inline std::string trimCopy(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

inline std::string toLowerCopy(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

inline std::string toUpperCopy(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return s;
}

inline std::vector<std::string> splitByChar(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    out.push_back(cur);
    return out;
}

// Convert the "key" part of a chord into a compact UI label.
// Returns the label and sets consumedShift=true when the label itself conveys Shift.
inline std::string keyTokenToDisplay(const std::string& keyTokIn, bool shift, bool& consumedShift) {
    consumedShift = false;
    const std::string raw = trimCopy(keyTokIn);
    if (raw.empty()) return "?";

    // Common named tokens produced by KeyBinds::keycodeToToken().
    const std::string k = toLowerCopy(raw);

    // Special keys.
    if (k == "enter" || k == "return") return "ENTER";
    if (k == "escape" || k == "esc") return "ESC";
    if (k == "tab") return "TAB";
    if (k == "space") return "SPACE";
    if (k == "backspace") return "BACK";
    if (k == "delete" || k == "del") return "DEL";
    if (k == "insert" || k == "ins") return "INS";
    if (k == "pageup" || k == "pgup") return "PGUP";
    if (k == "pagedown" || k == "pgdn") return "PGDN";
    if (k == "home") return "HOME";
    if (k == "end") return "END";
    if (k == "up") return "UP";
    if (k == "down") return "DOWN";
    if (k == "left") return "LEFT";
    if (k == "right") return "RIGHT";

    // Function keys ("f1".."f24").
    if (k.size() >= 2 && k[0] == 'f' && std::isdigit(static_cast<unsigned char>(k[1]))) {
        return toUpperCopy(k);
    }

    // Keypad tokens.
    if (k.rfind("kp_", 0) == 0) {
        if (k == "kp_enter") return "KP ENTER";
        if (k.size() == 4 && std::isdigit(static_cast<unsigned char>(k[3]))) {
            return std::string("KP") + static_cast<char>(k[3]);
        }
        return toUpperCopy(k);
    }

    // Punctuation names we emit.
    if (k == "comma") {
        if (shift) { consumedShift = true; return "<"; }
        return ",";
    }
    if (k == "period" || k == "dot") {
        if (shift) { consumedShift = true; return ">"; }
        return ".";
    }
    if (k == "slash") {
        if (shift) { consumedShift = true; return "?"; }
        return "/";
    }
    if (k == "backslash") {
        if (shift) { consumedShift = true; return "|"; }
        return "\\";
    }
    if (k == "minus" || k == "dash") {
        if (shift) { consumedShift = true; return "_"; }
        return "-";
    }
    if (k == "equals" || k == "equal") {
        if (shift) { consumedShift = true; return "+"; }
        return "=";
    }
    if (k == "semicolon") {
        if (shift) { consumedShift = true; return ":"; }
        return ";";
    }
    if (k == "apostrophe" || k == "quote") {
        if (shift) { consumedShift = true; return "\""; }
        return "'";
    }
    if (k == "grave" || k == "backquote") {
        if (shift) { consumedShift = true; return "~"; }
        return "`";
    }
    if (k == "less") {
        // Dedicated '<' key on some layouts.
        if (shift) { consumedShift = true; return ">"; }
        return "<";
    }
    if (k == "greater") {
        // Symmetric handling (rare, but keeps the display consistent).
        if (shift) { consumedShift = true; return "<"; }
        return ">";
    }

    // Single character fallback (letters, digits, brackets, etc.).
    if (raw.size() == 1) {
        const char c = raw[0];
        if (shift && c >= 'a' && c <= 'z') {
            consumedShift = true;
            return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }

        // Common US digit shift pairs (nice for quick readability).
        if (shift && c >= '0' && c <= '9') {
            consumedShift = true;
            switch (c) {
                case '1': return "!";
                case '2': return "@";
                case '3': return "#";
                case '4': return "$";
                case '5': return "%";
                case '6': return "^";
                case '7': return "&";
                case '8': return "*";
                case '9': return "(";
                case '0': return ")";
                default: break;
            }
        }

        return std::string(1, c);
    }

    // Generic fallback: uppercase the token (keeps it readable for SDL key names).
    return toUpperCopy(raw);
}

inline std::string chordTokenToDisplay(const std::string& chordTokIn) {
    std::string chordTok = trimCopy(chordTokIn);
    if (chordTok.empty()) return "";

    const std::string low = toLowerCopy(chordTok);
    if (low == "none" || low == "unbound" || low == "disabled") return "NONE";

    bool cmd = false;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    std::string keyTok;

    const std::vector<std::string> parts = splitByChar(chordTok, '+');
    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string pRaw = trimCopy(parts[i]);
        if (pRaw.empty()) continue;
        const std::string p = toLowerCopy(pRaw);

        // All but the last part are usually modifiers, but be defensive.
        if (p == "cmd" || p == "gui" || p == "meta" || p == "super") cmd = true;
        else if (p == "ctrl" || p == "control") ctrl = true;
        else if (p == "alt" || p == "option") alt = true;
        else if (p == "shift") shift = true;
        else keyTok = pRaw; // treat as key
    }

    bool consumedShift = false;
    const std::string keyDisp = keyTokenToDisplay(keyTok, shift, consumedShift);

    std::string out;
    if (cmd) out += "CMD+";
    if (ctrl) out += "CTRL+";
    if (alt) out += "ALT+";
    if (shift && !consumedShift) out += "SHIFT+";
    out += keyDisp;
    return out;
}

inline std::string chordListToDisplay(const std::string& chordListIn) {
    const std::string chordList = trimCopy(chordListIn);
    if (chordList.empty()) return "NONE";

    const std::string low = toLowerCopy(chordList);
    if (low == "none" || low == "unbound" || low == "disabled") return "NONE";

    std::vector<std::string> parts = splitByChar(chordList, ',');
    std::vector<std::string> unique;
    unique.reserve(parts.size());

    for (std::string& part : parts) {
        part = trimCopy(part);
        if (part.empty()) continue;
        const std::string disp = chordTokenToDisplay(part);
        if (disp.empty() || disp == "NONE") continue;

        // De-dupe at the UI level to avoid noisy repeats (e.g. "<, <" when
        // multiple physical keys converge on the same printed symbol).
        if (std::find(unique.begin(), unique.end(), disp) == unique.end()) {
            unique.push_back(disp);
        }
    }

    std::string out;
    for (size_t i = 0; i < unique.size(); ++i) {
        if (i) out += ", ";
        out += unique[i];
    }
    if (out.empty()) return "NONE";
    return out;
}

inline std::string depthLabel(int depth) {
    if (depth <= 0) return "CAMP";
    return std::to_string(depth);
}

inline std::string depthTag(int depth) {
    if (depth <= 0) return "CAMP";
    return "D" + std::to_string(depth);
}

inline std::string depthLabel(DungeonBranch branch, int depth) {
    if (branch == DungeonBranch::Camp) return "CAMP";
    return std::to_string(depth);
}

inline std::string depthTag(DungeonBranch branch, int depth) {
    if (branch == DungeonBranch::Camp) return "CAMP";
    return "D" + std::to_string(depth);
}

inline DungeonBranch scoreEntryBranch(const ScoreEntry& e) {
    // Score entries store branch as an ID to avoid depending on Game types.
    // 0 = Camp, 1 = Main dungeon (default).
    return (e.branch == 0) ? DungeonBranch::Camp : DungeonBranch::Main;
}

// --- Isometric helpers ---
// mapTileDst() returns the bounding box of the diamond tile in iso mode.
inline void isoDiamondCorners(const SDL_Rect& base, SDL_Point& top, SDL_Point& right, SDL_Point& bottom, SDL_Point& left) {
    const int cx = base.x + base.w / 2;
    const int cy = base.y + base.h / 2;

    top    = SDL_Point{cx, base.y};
    right  = SDL_Point{base.x + base.w, cy};
    bottom = SDL_Point{cx, base.y + base.h};
    left   = SDL_Point{base.x, cy};
}

inline void drawIsoDiamondOutline(SDL_Renderer* r, const SDL_Rect& base) {
    if (!r) return;
    SDL_Point top{}, right{}, bottom{}, left{};
    isoDiamondCorners(base, top, right, bottom, left);
    SDL_RenderDrawLine(r, top.x, top.y, right.x, right.y);
    SDL_RenderDrawLine(r, right.x, right.y, bottom.x, bottom.y);
    SDL_RenderDrawLine(r, bottom.x, bottom.y, left.x, left.y);
    SDL_RenderDrawLine(r, left.x, left.y, top.x, top.y);
}

inline void drawIsoDiamondCross(SDL_Renderer* r, const SDL_Rect& base) {
    if (!r) return;
    SDL_Point top{}, right{}, bottom{}, left{};
    isoDiamondCorners(base, top, right, bottom, left);
    SDL_RenderDrawLine(r, left.x, left.y, right.x, right.y);
    SDL_RenderDrawLine(r, top.x, top.y, bottom.x, bottom.y);
}

inline bool pointInIsoDiamond(int px, int py, const SDL_Rect& base) {
    // Diamond equation in normalized coordinates:
    //   |dx|/(w/2) + |dy|/(h/2) <= 1
    const int hw = std::max(1, base.w / 2);
    const int hh = std::max(1, base.h / 2);
    const int cx = base.x + hw;
    const int cy = base.y + hh;

    const float nx = std::abs(static_cast<float>(px - cx)) / static_cast<float>(hw);
    const float ny = std::abs(static_cast<float>(py - cy)) / static_cast<float>(hh);
    return (nx + ny) <= 1.0f;
}

inline void fillIsoDiamond(SDL_Renderer* r, int cx, int cy, int halfW, int halfH) {
    if (!r) return;
    halfW = std::max(1, halfW);
    halfH = std::max(1, halfH);

    // Rasterize a small diamond using horizontal scanlines.
    // The width scales linearly with vertical distance from the center.
    for (int dy = -halfH; dy <= halfH; ++dy) {
        const float t = 1.0f - (static_cast<float>(std::abs(dy)) / static_cast<float>(halfH));
        const int w = std::max(0, static_cast<int>(std::round(static_cast<float>(halfW) * t)));
        SDL_RenderDrawLine(r, cx - w, cy + dy, cx + w, cy + dy);
    }
}




// Procedural global isometric light direction.
//  0 = NW, 1 = NE, 2 = SE, 3 = SW.
// Chosen per-run from the cosmetic style seed so each run can have a slightly
// different lighting mood (purely visual, deterministic).
inline uint8_t isoLightDirFromStyleSeed(uint32_t styleSeed) {
    if (styleSeed == 0u) return 0u;
    return static_cast<uint8_t>(hash32(styleSeed ^ 0x51A0F00Du) & 0x03u);
}
// --- Coherent procedural variation helpers ---------------------------------
//
// Tile variants were previously selected purely via (hash % N), which can read as
// high-frequency "TV static" across large floor/wall fields.  These helpers use
// a cheap deterministic value-noise field to pick variants more coherently in
// space, producing larger patches of consistent texture while keeping per-tile
// uniqueness and replay determinism.
//
// Note: This is intentionally lightweight (a few hash mixes + lerps) and does
// not require any external noise library.

inline float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

inline float hash01_16(uint32_t h) {
    // Map a stable 16-bit slice of the hash to [0,1].  Using 16 bits keeps this
    // deterministic and fast while still looking "continuous" once interpolated.
    return static_cast<float>(hash32(h) & 0xFFFFu) / 65535.0f;
}

inline float valueNoise2D01(int x, int y, uint32_t seed, int period) {
    period = std::max(1, period);

    const int x0 = x / period;
    const int y0 = y / period;

    const float fx = static_cast<float>(x - x0 * period) / static_cast<float>(period);
    const float fy = static_cast<float>(y - y0 * period) / static_cast<float>(period);

    const uint32_t v00 = hashCombine(seed, hashCombine(static_cast<uint32_t>(x0),     static_cast<uint32_t>(y0)));
    const uint32_t v10 = hashCombine(seed, hashCombine(static_cast<uint32_t>(x0 + 1), static_cast<uint32_t>(y0)));
    const uint32_t v01 = hashCombine(seed, hashCombine(static_cast<uint32_t>(x0),     static_cast<uint32_t>(y0 + 1)));
    const uint32_t v11 = hashCombine(seed, hashCombine(static_cast<uint32_t>(x0 + 1), static_cast<uint32_t>(y0 + 1)));

    const float n00 = hash01_16(v00);
    const float n10 = hash01_16(v10);
    const float n01 = hash01_16(v01);
    const float n11 = hash01_16(v11);

    const float u = smoothstep01(fx);
    const float v = smoothstep01(fy);

    const float a = lerpf(n00, n10, u);
    const float b = lerpf(n01, n11, u);
    return lerpf(a, b, v);
}

inline float fractalNoise2D01(int x, int y, uint32_t seed) {
    // A tiny 3-octave fractal sum (fixed weights) for more natural variation.
    // Larger periods dominate, smaller ones add gentle detail.
    const float n0 = valueNoise2D01(x, y, seed ^ 0xA531F00Du, 12);
    const float n1 = valueNoise2D01(x, y, seed ^ 0xC0FFEE11u, 6);
    const float n2 = valueNoise2D01(x, y, seed ^ 0x1234BEEFu, 3);
    return (n0 * 0.55f) + (n1 * 0.30f) + (n2 * 0.15f);
}

inline size_t pickCoherentVariantIndex(int x, int y, uint32_t seed, size_t count) {
    if (count == 0) return 0;

    const float n = fractalNoise2D01(x, y, seed);
    size_t idx = static_cast<size_t>(std::floor(n * static_cast<float>(count)));
    if (idx >= count) idx = count - 1;

    // Micro-jitter (very small) keeps the texture from looking "too smooth" while
    // preserving the large-scale coherence.
    const uint32_t jh = hash32(hashCombine(seed ^ 0x91E10DAAu,
                                          hashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y))));
    const int j = static_cast<int>(jh % 3u) - 1; // -1..+1
    int ii = static_cast<int>(idx) + j;
    ii %= static_cast<int>(count);
    if (ii < 0) ii += static_cast<int>(count);
    return static_cast<size_t>(ii);
}

// Select at most one "decal anchor" per small grid cell (jittered position).
// This spreads decals out more evenly than per-tile independent RNG.
inline bool jitteredCellAnchor(int x, int y, uint32_t seed, int cellSize, uint32_t& outCellRand) {
    cellSize = std::max(2, cellSize);

    // Offset the grid per-seed so it doesn't align to the origin every run.
    const int ox = static_cast<int>((seed      ) & 0xFFu) % cellSize;
    const int oy = static_cast<int>((seed >> 8 ) & 0xFFu) % cellSize;

    const int gx = x + ox;
    const int gy = y + oy;

    const int cx = gx / cellSize;
    const int cy = gy / cellSize;

    outCellRand = hash32(hashCombine(seed, hashCombine(static_cast<uint32_t>(cx), static_cast<uint32_t>(cy))));

    const int px = static_cast<int>(outCellRand % static_cast<uint32_t>(cellSize));
    const int py = static_cast<int>((outCellRand >> 8) % static_cast<uint32_t>(cellSize));

    if ((gx % cellSize) != px) return false;
    if ((gy % cellSize) != py) return false;
    return true;
}

inline bool shouldPlaceDecalJittered(int x, int y, uint32_t seed, int cellSize, uint8_t chance, uint32_t& outCellRand) {
    if (!jitteredCellAnchor(x, y, seed, cellSize, outCellRand)) return false;
    const uint8_t roll = static_cast<uint8_t>((outCellRand >> 16) & 0xFFu);
    return roll < chance;
}


// -------------------------------------------------------------------------
// Color helpers (HSL, lerp, multiply) for procedural palette tints.
// These are used to derive near-white SDL texture color mods from a seed.
// -------------------------------------------------------------------------
inline float frac01(float x) {
    return x - std::floor(x);
}

inline float hue2rgb(float p, float q, float t) {
    t = frac01(t);
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 1.0f / 2.0f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

inline Color hslToRgb(float h, float s, float l) {
    h = frac01(h);
    s = std::clamp(s, 0.0f, 1.0f);
    l = std::clamp(l, 0.0f, 1.0f);

    float r = l, g = l, b = l;
    if (s > 1e-5f) {
        const float q = (l < 0.5f) ? (l * (1.0f + s)) : (l + s - l * s);
        const float p = 2.0f * l - q;
        r = hue2rgb(p, q, h + 1.0f / 3.0f);
        g = hue2rgb(p, q, h);
        b = hue2rgb(p, q, h - 1.0f / 3.0f);
    }

    return Color{clampToU8(static_cast<int>(r * 255.0f + 0.5f)),
                 clampToU8(static_cast<int>(g * 255.0f + 0.5f)),
                 clampToU8(static_cast<int>(b * 255.0f + 0.5f)),
                 Uint8{255}};
}

inline Color lerpColor(const Color& a, const Color& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const auto lerpChan = [&](Uint8 x, Uint8 y) {
        return clampToU8(static_cast<int>(static_cast<float>(x) + (static_cast<float>(y) - static_cast<float>(x)) * t + 0.5f));
    };
    return Color{lerpChan(a.r, b.r), lerpChan(a.g, b.g), lerpChan(a.b, b.b), Uint8{255}};
}

inline Color mulColor(const Color& a, const Color& b) {
    const auto mulChan = [&](Uint8 x, Uint8 y) {
        return static_cast<Uint8>((static_cast<int>(x) * static_cast<int>(y) + 127) / 255);
    };
    return Color{mulChan(a.r, b.r), mulChan(a.g, b.g), mulChan(a.b, b.b), Uint8{255}};
}

inline Color tintFromHsl(float h, float s, float l, float mix) {
    const Color c = hslToRgb(h, s, l);
    return lerpColor(Color{255, 255, 255, 255}, c, mix);
}

} // namespace


// -----------------------------------------------------------------------------
// Procedural particle VFX (renderer-owned, visual-only)
// -----------------------------------------------------------------------------
struct ParticleView {
    ViewMode mode = ViewMode::TopDown;
    int winW = 0;
    int winH = 0;
    int hudH = 0;
    int tile = 32;

    int camX = 0;
    int camY = 0;
    int isoCamX = 0;
    int isoCamY = 0;

    int mapOffX = 0;
    int mapOffY = 0;
};

struct ParticleEngine {
    enum class Kind : uint8_t { Spark = 0, Smoke, Ember, Mote };

    // Render ordering: some particles (trails) should appear behind projectile sprites,
    // while others (hits/explosions/fire) should sit on top.
    static constexpr uint8_t LAYER_BEHIND = 0;
    static constexpr uint8_t LAYER_FRONT  = 1;

    // Particle textures are tiny procedural sprites. Give them the same 4-frame
    // flipbook contract as the rest of the renderer so they can animate smoothly.
    static constexpr int ANIM_FRAMES = Renderer::FRAMES;

    struct Particle {
        // World position in map tiles (fractional ok). z is "height" in tiles.
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        // Velocity/accel in tiles/sec and tiles/sec^2.
        float vx = 0.0f;
        float vy = 0.0f;
        float vz = 0.0f;
        float ax = 0.0f;
        float ay = 0.0f;
        float az = 0.0f;

        // Linear drag (higher = more damped).
        float drag = 0.0f;

        float age = 0.0f;
        float life = 0.25f;

        // Size in tile-units (scaled by current tile size at render time).
        float size0 = 0.10f;
        float size1 = 0.05f;

        Color c0{255,255,255,255};
        Color c1{255,255,255,0};

        Kind kind = Kind::Spark;
        uint8_t var = 0;
        uint8_t layer = LAYER_FRONT;

        uint32_t seed = 0;
    };

    static constexpr int SPARK_VARS = 6;
    static constexpr int SMOKE_VARS = 6;
    static constexpr int EMBER_VARS = 4;
    static constexpr int MOTE_VARS = 6;

    // Animated flipbooks per particle type. This makes smoke/motes feel "alive"
    // without requiring runtime texture warping or authored sprite sheets.
    SDL_Texture* sparkTex[SPARK_VARS][ANIM_FRAMES]{};
    SDL_Texture* smokeTex[SMOKE_VARS][ANIM_FRAMES]{};
    SDL_Texture* emberTex[EMBER_VARS][ANIM_FRAMES]{};
    SDL_Texture* moteTex[MOTE_VARS][ANIM_FRAMES]{};

    std::vector<Particle> particles;
    float time = 0.0f;

    size_t maxParticles = 4096;

    ~ParticleEngine() { shutdown(); }

    void clear() { particles.clear(); }

    bool init(SDL_Renderer* r) {
        shutdown();

        // Spark: small "star" burst (additive) — animated twinkle.
        for (int i = 0; i < SPARK_VARS; ++i) {
            const uint32_t baseSeed = hashCombine(0x51A7u, static_cast<uint32_t>(i));
            for (int f = 0; f < ANIM_FRAMES; ++f) {
                sparkTex[i][f] = createTex(r, 16, 16, baseSeed, Kind::Spark, f);
                if (!sparkTex[i][f]) return false;
                SDL_SetTextureBlendMode(sparkTex[i][f], SDL_BLENDMODE_ADD);
            }
        }

        // Smoke: noisy blob (alpha blend) — animated domain-warped noise.
        for (int i = 0; i < SMOKE_VARS; ++i) {
            const uint32_t baseSeed = hashCombine(0x5A0C3u, static_cast<uint32_t>(i));
            for (int f = 0; f < ANIM_FRAMES; ++f) {
                smokeTex[i][f] = createTex(r, 32, 32, baseSeed, Kind::Smoke, f);
                if (!smokeTex[i][f]) return false;
                SDL_SetTextureBlendMode(smokeTex[i][f], SDL_BLENDMODE_BLEND);
            }
        }

        // Ember: tiny soft disc (additive) — animated flicker.
        for (int i = 0; i < EMBER_VARS; ++i) {
            const uint32_t baseSeed = hashCombine(0x3E8B3u, static_cast<uint32_t>(i));
            for (int f = 0; f < ANIM_FRAMES; ++f) {
                emberTex[i][f] = createTex(r, 16, 16, baseSeed, Kind::Ember, f);
                if (!emberTex[i][f]) return false;
                SDL_SetTextureBlendMode(emberTex[i][f], SDL_BLENDMODE_ADD);
            }
        }

        // Mote: soft diamond dust (additive) — animated ring + twinkle.
        for (int i = 0; i < MOTE_VARS; ++i) {
            const uint32_t baseSeed = hashCombine(0x4D4F5445u, static_cast<uint32_t>(i));
            for (int f = 0; f < ANIM_FRAMES; ++f) {
                moteTex[i][f] = createTex(r, 16, 16, baseSeed, Kind::Mote, f);
                if (!moteTex[i][f]) return false;
                SDL_SetTextureBlendMode(moteTex[i][f], SDL_BLENDMODE_ADD);
            }
        }

        return true;
    }

    void shutdown() {
        for (int i = 0; i < SPARK_VARS; ++i) {
            for (int f = 0; f < ANIM_FRAMES; ++f) {
                if (sparkTex[i][f]) SDL_DestroyTexture(sparkTex[i][f]);
                sparkTex[i][f] = nullptr;
            }
        }
        for (int i = 0; i < SMOKE_VARS; ++i) {
            for (int f = 0; f < ANIM_FRAMES; ++f) {
                if (smokeTex[i][f]) SDL_DestroyTexture(smokeTex[i][f]);
                smokeTex[i][f] = nullptr;
            }
        }
        for (int i = 0; i < EMBER_VARS; ++i) {
            for (int f = 0; f < ANIM_FRAMES; ++f) {
                if (emberTex[i][f]) SDL_DestroyTexture(emberTex[i][f]);
                emberTex[i][f] = nullptr;
            }
        }
        for (int i = 0; i < MOTE_VARS; ++i) {
            for (int f = 0; f < ANIM_FRAMES; ++f) {
                if (moteTex[i][f]) SDL_DestroyTexture(moteTex[i][f]);
                moteTex[i][f] = nullptr;
            }
        }
        particles.clear();
        time = 0.0f;
    }

    void add(const Particle& p) {
        if (particles.size() >= maxParticles) return;
        particles.push_back(p);
    }

    // Update the simulation by dt seconds.
    //
    // `windAccel` is a small, *visual-only* global acceleration (tiles/sec^2)
    // used to bias smoke/embers/motes so they drift consistently with the
    // game's deterministic per-level wind.
    void update(float dt, Vec2f windAccel = Vec2f{0.0f, 0.0f}) {
        if (dt <= 0.0f) return;
        dt = std::min(dt, 0.10f);

        time += dt;

        // Fixed-ish step to reduce frame-rate dependence.
        float acc = dt;
        const float step = 1.0f / 60.0f;
        int steps = 0;

        while (acc > 0.0f && steps < 6) {
            const float h = (acc >= step) ? step : acc;
            acc -= h;
            ++steps;

            for (size_t i = 0; i < particles.size();) {
                Particle& p = particles[i];
                p.age += h;
                if (p.age >= p.life) {
                    particles[i] = particles.back();
                    particles.pop_back();
                    continue;
                }

                // -----------------------------------------------------------------
                // Procedural drift: curl-noise flow field
                //
                // Instead of adding ad-hoc sin/cos wobble (which can read like a
                // jittery texture slide), we advect smoke/motes/embers through a
                // lightweight divergence-free (curl) noise field.
                //
                // This produces much more "fluid" motion while staying fully
                // procedural and deterministic.
                // -----------------------------------------------------------------
                if (p.kind == Kind::Smoke || p.kind == Kind::Mote || p.kind == Kind::Ember) {
                    const float t01 = std::clamp(p.age / std::max(0.0001f, p.life), 0.0f, 1.0f);
                    // Stronger at spawn, taper later so particles don't accelerate
                    // wildly at the end of their lifetime.
                    float fade = 1.0f - t01;
                    fade = fade * fade;

                    // Per-kind tuning: smoke flows slower/larger-scale, motes
                    // smaller-scale, embers are subtle.
                    float amp = 0.0f;
                    float scale = 1.0f;
                    int octaves = 3;
                    if (p.kind == Kind::Smoke) {
                        amp = 0.65f;
                        scale = 0.80f;
                        octaves = 4;
                    } else if (p.kind == Kind::Mote) {
                        amp = 0.35f;
                        scale = 1.15f;
                        octaves = 3;
                    } else { // Ember
                        amp = 0.22f;
                        scale = 1.35f;
                        octaves = 3;
                    }

                    // Per-particle variation (stable).
                    const float v0 = 0.85f + 0.30f * rand01(p.seed ^ 0xC0A51EEDu);
                    amp *= v0;
                    scale *= (0.90f + 0.25f * rand01(p.seed ^ 0xA11CE5u));

                    const Vec2f flow = curlNoise2D(p.x * scale, p.y * scale, time,
                                                   p.seed ^ 0xBADC0DEu,
                                                   /*eps=*/0.18f,
                                                   /*octaves=*/octaves);

                    p.vx += flow.x * amp * fade * h;
                    p.vy += flow.y * amp * fade * h;
                }

                // Global wind bias (visual-only; comes from the deterministic
                // per-level wind in Game).
                if ((p.kind == Kind::Smoke) || (p.kind == Kind::Mote) || (p.kind == Kind::Ember)) {
                    float k = 0.0f;
                    if (p.kind == Kind::Smoke) k = 1.00f;
                    else if (p.kind == Kind::Mote) k = 0.55f;
                    else k = 0.25f; // Ember
                    p.vx += windAccel.x * k * h;
                    p.vy += windAccel.y * k * h;
                }

                // Integrate.
                p.vx += p.ax * h;
                p.vy += p.ay * h;
                p.vz += p.az * h;

                if (p.drag > 0.0f) {
                    const float k = 1.0f / (1.0f + p.drag * h);
                    p.vx *= k;
                    p.vy *= k;
                    p.vz *= k;
                }

                // Safety clamp: keep rare pathological cases from exploding.
                const float vmax = (p.kind == Kind::Smoke) ? 1.60f
                                  : (p.kind == Kind::Mote)  ? 1.20f
                                  : (p.kind == Kind::Ember) ? 2.80f
                                  : 6.00f;
                const float sp2 = p.vx * p.vx + p.vy * p.vy;
                if (sp2 > vmax * vmax) {
                    const float inv = vmax / std::sqrt(std::max(0.000001f, sp2));
                    p.vx *= inv;
                    p.vy *= inv;
                }

                p.x += p.vx * h;
                p.y += p.vy * h;
                p.z += p.vz * h;

                // Simple ground bounce/damp.
                if (p.z < 0.0f) {
                    p.z = 0.0f;
                    p.vz = -p.vz * 0.25f;
                    p.vx *= 0.65f;
                    p.vy *= 0.65f;
                }

                ++i;
            }
        }
    }

    void render(SDL_Renderer* r, const ParticleView& view, uint8_t layer) {
        if (!r) return;
        if (particles.empty()) return;

        const float tileSize = static_cast<float>(std::max(1, view.tile));
        const float mapH = static_cast<float>(std::max(0, view.winH - view.hudH));

        // NOTE: we rely on the caller to have set a map-space clip rect already.
        for (const Particle& p : particles) {
            if (p.layer != layer) continue;

            const float t01 = std::clamp(p.age / std::max(0.0001f, p.life), 0.0f, 1.0f);
            const float sizeTiles = lerp(p.size0, p.size1, t01);
            const float sizePxF = std::max(1.0f, sizeTiles * tileSize);
            const int sizePx = static_cast<int>(sizePxF + 0.5f);

            const Color c = lerpColor(p.c0, p.c1, t01);

            const int af = animFrameFor(p);
            SDL_Texture* tex = texFor(p, af);
            if (!tex) continue;

            float sx = 0.0f;
            float sy = 0.0f;

            if (view.mode != ViewMode::Isometric) {
                const float dx = p.x - static_cast<float>(view.camX);
                const float dy = p.y - static_cast<float>(view.camY);
                sx = dx * tileSize + static_cast<float>(view.mapOffX);
                sy = dy * tileSize + static_cast<float>(view.mapOffY);
                sy -= p.z * tileSize;
            } else {
                const float tileW = tileSize;
                const float tileH = tileSize * 0.5f;
                const float halfW = tileW * 0.5f;
                const float halfH = tileH * 0.5f;

                const float cx = static_cast<float>(view.winW) * 0.5f + static_cast<float>(view.mapOffX);
                const float cy = mapH * 0.5f + static_cast<float>(view.mapOffY);

                const float dx = p.x - static_cast<float>(view.isoCamX);
                const float dy = p.y - static_cast<float>(view.isoCamY);

                sx = cx + (dx - dy) * halfW;
                sy = cy + (dx + dy) * halfH;
                sy -= p.z * tileSize;
            }

            SDL_Rect dst{
                static_cast<int>(std::round(sx - static_cast<float>(sizePx) * 0.5f)),
                static_cast<int>(std::round(sy - static_cast<float>(sizePx) * 0.5f)),
                sizePx, sizePx
            };

            // Quick cull against map viewport (with a small pad).
            const int pad = sizePx + 8;
            if (dst.x > view.winW + pad || dst.y > static_cast<int>(mapH) + pad) continue;
            if (dst.x + dst.w < -pad || dst.y + dst.h < -pad) continue;

            SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
            SDL_SetTextureAlphaMod(tex, c.a);
            SDL_RenderCopy(r, tex, nullptr, &dst);
        }
    }

private:
    static float lerp(float a, float b, float t) { return a + (b - a) * t; }

    static uint8_t lerpU8(uint8_t a, uint8_t b, float t) {
        const float v = lerp(static_cast<float>(a), static_cast<float>(b), t);
        const int iv = static_cast<int>(std::round(v));
        return static_cast<uint8_t>(std::clamp(iv, 0, 255));
    }

    static Color lerpColor(const Color& a, const Color& b, float t) {
        return Color{
            lerpU8(a.r, b.r, t),
            lerpU8(a.g, b.g, t),
            lerpU8(a.b, b.b, t),
            lerpU8(a.a, b.a, t),
        };
    }

    int animFrameFor(const Particle& p) const {
        // Use particle-relative time so each particle animates across its lifetime,
        // then apply a stable per-particle phase offset to avoid lockstep motion.
        const float base = std::clamp(p.age / std::max(0.0001f, p.life), 0.0f, 1.0f);
        const float phase = static_cast<float>(hash32(p.seed ^ 0xA11CEu) & 0xFFFFu) * (1.0f / 65535.0f);

        float speed = 1.0f;
        switch (p.kind) {
            case Kind::Spark: speed = 2.0f; break;
            case Kind::Ember: speed = 1.6f; break;
            case Kind::Mote:  speed = 1.3f; break;
            case Kind::Smoke:
            default:          speed = 1.0f; break;
        }

        const float t = base * speed + phase;
        int fi = static_cast<int>(std::floor(t * static_cast<float>(ANIM_FRAMES))) % ANIM_FRAMES;
        if (fi < 0) fi += ANIM_FRAMES;
        return fi;
    }

    SDL_Texture* texFor(const Particle& p, int frame) const {
        frame = (ANIM_FRAMES > 0) ? (frame % ANIM_FRAMES) : 0;
        if (frame < 0) frame += ANIM_FRAMES;

        if (p.kind == Kind::Spark) {
            return sparkTex[static_cast<int>(p.var) % SPARK_VARS][frame];
        } else if (p.kind == Kind::Smoke) {
            return smokeTex[static_cast<int>(p.var) % SMOKE_VARS][frame];
        } else if (p.kind == Kind::Mote) {
            return moteTex[static_cast<int>(p.var) % MOTE_VARS][frame];
        }
        return emberTex[static_cast<int>(p.var) % EMBER_VARS][frame];
    }

    static float rand01(uint32_t s) {
        return static_cast<float>(hash32(s) & 0xFFFFu) * (1.0f / 65535.0f);
    }

    static float fade(float t) {
        // Quintic smoothstep.
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    static float valueNoise2D01(float x, float y, uint32_t seed) {
        const float fx = std::floor(x);
        const float fy = std::floor(y);
        const int xi = static_cast<int>(fx);
        const int yi = static_cast<int>(fy);

        const float tx = x - fx;
        const float ty = y - fy;

        const float u = fade(std::clamp(tx, 0.0f, 1.0f));
        const float v = fade(std::clamp(ty, 0.0f, 1.0f));

        auto h = [&](int x0, int y0) -> float {
            const uint32_t hx = static_cast<uint32_t>(x0);
            const uint32_t hy = static_cast<uint32_t>(y0);
            return rand01(hashCombine(seed, hashCombine(hx, hy)));
        };

        const float v00 = h(xi,     yi);
        const float v10 = h(xi + 1, yi);
        const float v01 = h(xi,     yi + 1);
        const float v11 = h(xi + 1, yi + 1);

        const float a = lerp(v00, v10, u);
        const float b = lerp(v01, v11, u);
        return lerp(a, b, v);
    }

    static float fbm2D01(float x, float y, uint32_t seed, int octaves = 4) {
        octaves = std::clamp(octaves, 1, 8);
        float sum = 0.0f;
        float amp = 0.5f;
        float freq = 1.0f;
        float norm = 0.0f;
        uint32_t s = seed;

        for (int i = 0; i < octaves; ++i) {
            sum += valueNoise2D01(x * freq, y * freq, s) * amp;
            norm += amp;
            amp *= 0.5f;
            freq *= 2.0f;
            s = hashCombine(s, static_cast<uint32_t>(0x9E37u + i * 131u));
        }

        return (norm > 0.0f) ? (sum / norm) : 0.0f;
    }

    // Divergence-free 2D flow field derived from a scalar noise potential.
    //
    // We build a (time-varying) scalar field n(x,y,t), estimate its gradient,
    // then rotate that gradient by 90 degrees to obtain a curl field:
    //   v = (dn/dy, -dn/dx)
    //
    // This is a common trick for getting fluid-like advection in particle
    // systems without simulating Navier–Stokes.
    static Vec2f curlNoise2D(float x, float y, float time, uint32_t seed, float eps, int octaves) {
        constexpr float TAU = 6.28318530718f;

        eps = std::clamp(eps, 0.02f, 0.75f);
        octaves = std::clamp(octaves, 1, 6);

        // Animate by drifting through the noise domain along a circle so the
        // field changes smoothly over time.
        const float phase = rand01(seed ^ 0xC0A51EEDu) * TAU;
        const float ang = time * 0.70f + phase;
        const float driftX = std::cos(ang) * 0.85f;
        const float driftY = std::sin(ang) * 0.85f;

        auto pot = [&](float xx, float yy) -> float {
            // A small fixed offset keeps x,y near the origin from looking too
            // "grid aligned" (value-noise artifacts).
            return fbm2D01(xx + driftX + 19.7f, yy + driftY - 8.3f, seed ^ 0xBADC0DEu, octaves);
        };

        const float nL = pot(x - eps, y);
        const float nR = pot(x + eps, y);
        const float nD = pot(x, y - eps);
        const float nU = pot(x, y + eps);

        const float dX = (nR - nL) / (2.0f * eps);
        const float dY = (nU - nD) / (2.0f * eps);

        Vec2f v{ dY, -dX };

        // Clamp magnitude to keep the flow stable regardless of scale.
        const float m2 = v.x * v.x + v.y * v.y;
        if (m2 > 1.0f) {
            const float inv = 1.0f / std::sqrt(m2);
            v.x *= inv;
            v.y *= inv;
        }
        return v;
    }

    SDL_Texture* createTex(SDL_Renderer* r, int w, int h, uint32_t seed, Kind kind, int frame) {
        if (!r) return nullptr;

        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA8888);
        if (!surf) return nullptr;

        uint32_t* px = static_cast<uint32_t*>(surf->pixels);
        SDL_PixelFormat* fmt = surf->format;

        constexpr float kTwoPi = 6.28318530718f;
        const float animT = static_cast<float>(frame % std::max(1, ANIM_FRAMES)) / static_cast<float>(std::max(1, ANIM_FRAMES));
        const float seedPhase = static_cast<float>(hash32(seed ^ 0xBADC0DEu) & 0xFFFFu) * (1.0f / 65535.0f);
        const float ang = (animT + seedPhase) * kTwoPi;

        // Circular drift in noise domain => seamless looping animation.
        const float driftX = std::cos(ang) * 0.35f;
        const float driftY = std::sin(ang) * 0.35f;

        const float pulse1 = 0.85f + 0.15f * std::sin(ang);
        const float pulse2 = 0.80f + 0.20f * std::sin(ang * 2.0f + seedPhase * kTwoPi * 0.37f);

        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const float nx = ((static_cast<float>(x) + 0.5f) / static_cast<float>(w)) * 2.0f - 1.0f;
                const float ny = ((static_cast<float>(y) + 0.5f) / static_cast<float>(h)) * 2.0f - 1.0f;
                const float r0 = std::sqrt(nx * nx + ny * ny);

                float a = 0.0f;

                if (kind == Kind::Spark) {
                    // Subtle rotation wobble so sparks twinkle rather than just scale.
                    const float wob = 0.28f * std::sin(ang * 2.0f + seedPhase * kTwoPi * 0.73f);
                    const float cs = std::cos(wob);
                    const float sn = std::sin(wob);
                    const float rx = nx * cs - ny * sn;
                    const float ry = nx * sn + ny * cs;
                    const float rr = std::sqrt(rx * rx + ry * ry);

                    float core = std::max(0.0f, 1.0f - rr * 1.55f);
                    core = core * core * core;

                    // Star spikes.
                    const float spikeX = std::max(0.0f, 1.0f - std::abs(rx) * 7.0f);
                    const float spikeY = std::max(0.0f, 1.0f - std::abs(ry) * 7.0f);
                    const float spikeD1 = std::max(0.0f, 1.0f - std::abs(rx + ry) * 4.5f);
                    const float spikeD2 = std::max(0.0f, 1.0f - std::abs(rx - ry) * 4.5f);

                    float spikes = (spikeX + spikeY) * 0.35f + (spikeD1 + spikeD2) * 0.20f;

                    // Twinkle modulation from looped noise.
                    const float tw = fbm2D01(rx * 6.0f + driftX * 2.0f, ry * 6.0f + driftY * 2.0f, seed ^ 0x51A7u, 3);
                    const float n = (tw - 0.5f) * 0.20f;

                    a = std::clamp((core + spikes) * pulse2 + n, 0.0f, 1.0f);
                } else if (kind == Kind::Smoke) {
                    // Domain-warped fBm "puff" that loops over 4 frames.
                    float edge = std::max(0.0f, 1.0f - r0);
                    edge = edge * edge;

                    const float w1 = fbm2D01(nx * 2.25f + driftX * 1.8f, ny * 2.25f + driftY * 1.8f, seed ^ 0xBEEF1234u, 4);
                    const float w2 = fbm2D01(nx * 2.25f - driftY * 1.6f + 12.3f, ny * 2.25f + driftX * 1.6f - 9.1f, seed ^ 0x1234u, 4);
                    const float wx = (w1 - 0.5f) * 0.70f;
                    const float wy = (w2 - 0.5f) * 0.70f;

                    const float d0 = fbm2D01((nx + wx) * 3.15f + driftX * 0.8f,
                                            (ny + wy) * 3.15f + driftY * 0.8f,
                                            seed ^ 0x9E3779B9u,
                                            5);

                    const float grain = fbm2D01(nx * 8.0f + driftX * 4.0f,
                                               ny * 8.0f + driftY * 4.0f,
                                               seed ^ 0xC0FFEEu,
                                               3);

                    const float density = d0 * 0.85f + grain * 0.15f;
                    a = edge * (0.25f + density * 0.95f);
                    a = std::clamp(a * pulse1, 0.0f, 1.0f);
                } else if (kind == Kind::Mote) {
                    // Soft diamond "mote": a magical dust speck with a faint animated ring.
                    const float d = std::abs(nx) + std::abs(ny); // diamond distance
                    float core = std::max(0.0f, 1.0f - d * 1.35f);
                    core = core * core * core;

                    const float ringPos = 0.55f + 0.04f * std::sin(ang + seedPhase * kTwoPi * 0.91f);
                    float ring = std::max(0.0f, 1.0f - std::abs(d - ringPos) * 4.2f);
                    ring = ring * ring;

                    const float crossX = std::max(0.0f, 1.0f - std::abs(nx) * 6.0f);
                    const float crossY = std::max(0.0f, 1.0f - std::abs(ny) * 6.0f);

                    const float tw = fbm2D01(nx * 6.0f + driftX * 2.0f,
                                            ny * 6.0f + driftY * 2.0f,
                                            seed ^ 0x4D4F5445u,
                                            3);
                    const float n = (tw - 0.5f) * 0.22f;

                    a = std::clamp((core + ring * 0.28f + (crossX + crossY) * 0.06f + n) * pulse2, 0.0f, 1.0f);
                } else { // Ember
                    // Ember: flickering hot dot with a little internal noise.
                    float core = std::max(0.0f, 1.0f - r0 * 1.85f);
                    core = core * core;

                    const float tw = fbm2D01(nx * 7.0f + driftX * 3.5f,
                                            ny * 7.0f + driftY * 3.5f,
                                            seed ^ 0x3E8B3u,
                                            3);
                    const float n = (tw - 0.5f) * 0.28f;

                    a = std::clamp((core + n) * pulse2, 0.0f, 1.0f);
                }

                const uint8_t alpha = static_cast<uint8_t>(std::clamp<int>(static_cast<int>(std::round(a * 255.0f)), 0, 255));
                px[static_cast<size_t>(y * w + x)] = SDL_MapRGBA(fmt, 255, 255, 255, alpha);
            }
        }

        SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
        SDL_FreeSurface(surf);
        return tex;
    }
};
Renderer::Renderer(int windowW, int windowH, int tileSize, int hudHeight, bool vsync, int textureCacheMB_)
    : winW(windowW), winH(windowH), tile(tileSize), hudH(hudHeight), vsyncEnabled(vsync), textureCacheMB(textureCacheMB_) {
    // Derive viewport size in tiles from the logical window size.
    // The bottom HUD area is not part of the map viewport.
    const int t = std::max(1, tile);
    viewTilesW = std::max(1, winW / t);
    viewTilesH = std::max(1, std::max(0, winH - hudH) / t);

    camX = 0;
    camY = 0;
}

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::init() {
    if (initialized) return true;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0"); // nearest-neighbor

    const std::string title = std::string(PROCROGUE_APPNAME) + " v" + PROCROGUE_VERSION;
    window = SDL_CreateWindow(title.c_str(),
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              winW, winH,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        return false;
    }

    Uint32 rFlags = SDL_RENDERER_ACCELERATED;
    if (vsyncEnabled) rFlags |= SDL_RENDERER_PRESENTVSYNC;
    renderer = SDL_CreateRenderer(window, -1, rFlags);
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window); window = nullptr;
        return false;
    }

    // Keep a fixed "virtual" resolution and let SDL scale the final output.
    // This makes the window resizable while preserving crisp pixel art.
    SDL_RenderSetLogicalSize(renderer, winW, winH);
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);

    pixfmt = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    if (!pixfmt) {
        std::cerr << "SDL_AllocFormat failed\n";
        shutdown();
        return false;
    }

    // Init procedural particle textures (visual-only; failure is non-fatal).
    particles_ = std::make_unique<ParticleEngine>();
    if (!particles_->init(renderer)) {
        std::cerr << "ParticleEngine init failed; continuing without particles.\n";
        particles_.reset();
    }

    // Pre-generate tile variants (with animation frames)
    // Procedural sprite generation now supports higher-res output (up to 256x256).
    // We generate map sprites at (tile) resolution to avoid renderer scaling artifacts.
    const int spritePx = std::clamp(tile, 16, 256);

    // Sprite cache sizing:
    // - Each cached entry stores FRAMES textures of size spritePx*spritePx RGBA.
    // - This is an approximation, but it's stable and lets us cap VRAM usage.
    spriteEntryBytes = static_cast<size_t>(spritePx) * static_cast<size_t>(spritePx) * sizeof(uint32_t) * static_cast<size_t>(FRAMES);

    // Scale some overlay variant counts down for huge tile sizes (keeps VRAM in check).
    decalsPerStyleUsed = (spritePx <= 48) ? 6 : (spritePx <= 96 ? 5 : (spritePx <= 160 ? 4 : 3));
    decalsPerStyleUsed = std::clamp(decalsPerStyleUsed, 1, DECALS_PER_STYLE);

    autoVarsUsed = (spritePx <= 96) ? 4 : (spritePx <= 160 ? 3 : 2);
    autoVarsUsed = std::clamp(autoVarsUsed, 1, AUTO_VARS);

    // Configure the sprite texture cache budget.
    // 0 => unlimited (no eviction).
    size_t budgetBytes = 0;
    if (textureCacheMB > 0) {
        budgetBytes = static_cast<size_t>(textureCacheMB) * 1024ull * 1024ull;
        // Ensure the budget can hold at least a small working set (prevents thrash).
        const size_t minBudget = spriteEntryBytes * 12ull; // ~12 sprites worth
        if (budgetBytes < minBudget) budgetBytes = minBudget;
    }
    spriteTex.setBudgetBytes(budgetBytes);
    spriteTex.resetStats();

    // Configure the UI preview cache budget.
    // These are larger (e.g. 128x128+) and rotate through multiple yaws.
    size_t previewBudgetBytes = 0;
    if (budgetBytes > 0) {
        // Allocate a slice of the main cache budget to UI previews.
        previewBudgetBytes = std::max<size_t>(1024ull * 1024ull, budgetBytes / 8ull);
        previewBudgetBytes = std::min(previewBudgetBytes, 16ull * 1024ull * 1024ull);
    }
    uiPreviewTex.setBudgetBytes(previewBudgetBytes);
    uiPreviewTex.resetStats();

    // More variants reduce visible repetition, but large tile sizes can become
    // expensive in VRAM. Scale the variant count down as tile size increases.
    const int tileVars = (spritePx <= 48) ? 18 : (spritePx <= 96 ? 14 : (spritePx <= 160 ? 10 : 8));

    for (auto& v : floorThemeVar) v.clear();
    wallVar.clear();
    chasmVar.clear();
    pillarOverlayVar.clear();
    boulderOverlayVar.clear();
    fountainOverlayVar.clear();
    altarOverlayVar.clear();

    for (auto& v : floorThemeVar) v.resize(static_cast<size_t>(tileVars));
    wallVar.resize(static_cast<size_t>(tileVars));
    chasmVar.resize(static_cast<size_t>(tileVars));
    pillarOverlayVar.resize(static_cast<size_t>(tileVars));
    boulderOverlayVar.resize(static_cast<size_t>(tileVars));
    fountainOverlayVar.resize(static_cast<size_t>(tileVars));
    altarOverlayVar.resize(static_cast<size_t>(tileVars));

    for (int i = 0; i < tileVars; ++i) {
        // Floor: build a full themed tileset so special rooms pop.
        for (int st = 0; st < ROOM_STYLES; ++st) {
            const uint32_t fSeed = hashCombine(hashCombine(0xF1000u, static_cast<uint32_t>(st)), static_cast<uint32_t>(i));
            for (int f = 0; f < FRAMES; ++f) {
                floorThemeVar[static_cast<size_t>(st)][static_cast<size_t>(i)][static_cast<size_t>(f)] =
                    textureFromSprite(generateThemedFloorTile(fSeed, static_cast<uint8_t>(st), f, spritePx));
            }
        }

        // Other base terrain (not room-themed yet).
        const uint32_t wSeed = hashCombine(0xAA110u, static_cast<uint32_t>(i));
        const uint32_t cSeed = hashCombine(0xC1A500u, static_cast<uint32_t>(i));
        const uint32_t pSeed = hashCombine(0x9111A0u, static_cast<uint32_t>(i));
        const uint32_t bSeed = hashCombine(0xB011D3u, static_cast<uint32_t>(i));
        const uint32_t foSeed = hashCombine(0xF017A1u, static_cast<uint32_t>(i));
        const uint32_t alSeed = hashCombine(0xA17A12u, static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            wallVar[static_cast<size_t>(i)][static_cast<size_t>(f)]  = textureFromSprite(generateWallTile(wSeed, f, spritePx));
            chasmVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateChasmTile(cSeed, f, spritePx));
            // Pillar is generated as a transparent overlay; it will be layered over the
            // underlying themed floor at render-time.
            pillarOverlayVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generatePillarTile(pSeed, f, spritePx));
            boulderOverlayVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateBoulderTile(bSeed, f, spritePx));
            fountainOverlayVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateFountainTile(foSeed, f, spritePx));
            altarOverlayVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateAltarTile(alSeed, f, spritePx));
        }
    }

    for (int f = 0; f < FRAMES; ++f) {
        // Doors and stairs are rendered as overlays layered over the underlying themed floor.
        stairsUpOverlayTex[static_cast<size_t>(f)]   = textureFromSprite(generateStairsTile(0x515A1u, true, f, spritePx));
        stairsDownOverlayTex[static_cast<size_t>(f)] = textureFromSprite(generateStairsTile(0x515A2u, false, f, spritePx));
        doorClosedOverlayTex[static_cast<size_t>(f)] = textureFromSprite(generateDoorTile(0xD00Du, false, f, spritePx));
        doorLockedOverlayTex[static_cast<size_t>(f)] = textureFromSprite(generateLockedDoorTile(0xD00Du, f, spritePx));
        doorOpenOverlayTex[static_cast<size_t>(f)]   = textureFromSprite(generateDoorTile(0xD00Du, true, f, spritePx));
    }

// Default UI skin assets (will refresh if theme changes at runtime).
uiThemeCached = UITheme::DarkStone;
uiAssetsValid = true;
for (int f = 0; f < FRAMES; ++f) {
    uiPanelTileTex[static_cast<size_t>(f)] = textureFromSprite(generateUIPanelTile(uiThemeCached, 0x51A11u, f, 16));
    uiOrnamentTex[static_cast<size_t>(f)]  = textureFromSprite(generateUIOrnamentTile(uiThemeCached, 0x0ABCDu, f, 16));
}

// Pre-generate decal overlays (small transparent patterns blended onto tiles).
floorDecalVar.clear();
wallDecalVar.clear();
floorDecalVar.resize(static_cast<size_t>(DECAL_STYLES * static_cast<size_t>(decalsPerStyleUsed)));
wallDecalVar.resize(static_cast<size_t>(DECAL_STYLES * static_cast<size_t>(decalsPerStyleUsed)));
for (int st = 0; st < DECAL_STYLES; ++st) {
    for (int i = 0; i < decalsPerStyleUsed; ++i) {
        const uint32_t fSeed = hashCombine(0xD3CA10u + static_cast<uint32_t>(st) * 131u, static_cast<uint32_t>(i));
        const uint32_t wSeed = hashCombine(0xBADC0DEu + static_cast<uint32_t>(st) * 191u, static_cast<uint32_t>(i));
        const size_t idx = static_cast<size_t>(st * decalsPerStyleUsed + i);
        for (int f = 0; f < FRAMES; ++f) {
            floorDecalVar[idx][static_cast<size_t>(f)] = textureFromSprite(generateFloorDecalTile(fSeed, static_cast<uint8_t>(st), f, spritePx));
            wallDecalVar[idx][static_cast<size_t>(f)]  = textureFromSprite(generateWallDecalTile(wSeed, static_cast<uint8_t>(st), f, spritePx));
        }
    }
}


// Pre-generate autotile overlays (edge/corner shaping for walls and chasm rims).
for (int mask = 0; mask < AUTO_MASKS; ++mask) {
    for (int v = 0; v < autoVarsUsed; ++v) {
        const uint32_t wSeed = hashCombine(0xE0D6E00u + static_cast<uint32_t>(mask) * 131u, static_cast<uint32_t>(v));
        const uint32_t cSeed = hashCombine(0xC0A5E00u + static_cast<uint32_t>(mask) * 191u, static_cast<uint32_t>(v));
        const uint32_t sSeed = hashCombine(0x5EAD0DEu + static_cast<uint32_t>(mask) * 227u, static_cast<uint32_t>(v));
        for (int f = 0; f < FRAMES; ++f) {
            wallEdgeVar[static_cast<size_t>(mask)][static_cast<size_t>(v)][static_cast<size_t>(f)] =
                (mask == 0) ? nullptr : textureFromSprite(generateWallEdgeOverlay(wSeed, static_cast<uint8_t>(mask), v, f, spritePx));
            chasmRimVar[static_cast<size_t>(mask)][static_cast<size_t>(v)][static_cast<size_t>(f)] =
                (mask == 0) ? nullptr : textureFromSprite(generateChasmRimOverlay(cSeed, static_cast<uint8_t>(mask), v, f, spritePx));
            topDownWallShadeVar[static_cast<size_t>(mask)][static_cast<size_t>(v)][static_cast<size_t>(f)] =
                (mask == 0) ? nullptr : textureFromSprite(generateTopDownWallShadeOverlay(sSeed, static_cast<uint8_t>(mask), v, f, spritePx));
        }
    }
}

// Pre-generate confusion gas overlay tiles.
for (int i = 0; i < GAS_VARS; ++i) {
    const uint32_t gSeed = hashCombine(0x6A5u, static_cast<uint32_t>(i));
    for (int f = 0; f < FRAMES; ++f) {
        gasVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateConfusionGasTile(gSeed, f, spritePx));
    }
}

// Pre-generate fire overlay tiles.
for (int i = 0; i < FIRE_VARS; ++i) {
    const uint32_t fSeed = hashCombine(0xF17Eu, static_cast<uint32_t>(i));
    for (int f = 0; f < FRAMES; ++f) {
        fireVar[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(generateFireTile(fSeed, f, spritePx));
    }
}

// Pre-generate HUD effect icons.
for (int k = 0; k < EFFECT_KIND_COUNT; ++k) {
    const EffectKind ek = static_cast<EffectKind>(k);
    for (int f = 0; f < FRAMES; ++f) {
        effectIconTex[static_cast<size_t>(k)][static_cast<size_t>(f)] = textureFromSprite(generateEffectIcon(ek, f, 16));
    }
}


// Pre-generate cursor / targeting reticle overlays (map-space UI).
for (int f = 0; f < FRAMES; ++f) {
    cursorReticleTex[static_cast<size_t>(f)] = textureFromSprite(generateCursorReticleTile(0xC0A51Eu, /*isometric=*/false, f, spritePx));
    cursorReticleIsoTex[static_cast<size_t>(f)] = textureFromSprite(generateCursorReticleTile(0xC0A51Eu, /*isometric=*/true, f, spritePx));
}

// Reset room-type cache (rebuilt lazily in render()).
roomTypeCache.clear();
roomCacheDungeon = nullptr;
roomCacheBranch = DungeonBranch::Main;
roomCacheDepth = -1;
roomCacheW = 0;
roomCacheH = 0;
roomCacheRooms = 0;

    initialized = true;
    return true;
}

void Renderer::shutdown() {
    if (!initialized) {
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        return;
    }

for (auto& styleVec : floorThemeVar) {
    for (auto& arr : styleVec) {
        for (SDL_Texture* t : arr) {
            if (t) SDL_DestroyTexture(t);
        }
    }
    styleVec.clear();
}

// Isometric terrain tiles (generated lazily).
for (auto& styleVec : floorThemeVarIso) {
    for (auto& arr : styleVec) {
        for (SDL_Texture* t : arr) {
            if (t) SDL_DestroyTexture(t);
        }
    }
    styleVec.clear();
}
for (auto& arr : wallVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : chasmVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : chasmVarIso) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : pillarOverlayVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : boulderOverlayVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : fountainOverlayVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : altarOverlayVar) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : wallBlockVarIso) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : doorBlockClosedVarIso) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : doorBlockLockedVarIso) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : doorBlockOpenVarIso) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : pillarBlockVarIso) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
for (auto& arr : boulderBlockVarIso) {
    for (SDL_Texture* t : arr) {
        if (t) SDL_DestroyTexture(t);
    }
}
wallVar.clear();
chasmVar.clear();
pillarOverlayVar.clear();
boulderOverlayVar.clear();
fountainOverlayVar.clear();
altarOverlayVar.clear();

chasmVarIso.clear();
wallBlockVarIso.clear();
doorBlockClosedVarIso.clear();
doorBlockLockedVarIso.clear();
doorBlockOpenVarIso.clear();
pillarBlockVarIso.clear();
boulderBlockVarIso.clear();

for (auto& t : stairsUpOverlayIsoTex) if (t) SDL_DestroyTexture(t);
for (auto& t : stairsDownOverlayIsoTex) if (t) SDL_DestroyTexture(t);
for (auto& t : doorOpenOverlayIsoTex) if (t) SDL_DestroyTexture(t);
stairsUpOverlayIsoTex.fill(nullptr);
stairsDownOverlayIsoTex.fill(nullptr);
doorOpenOverlayIsoTex.fill(nullptr);

for (auto& t : isoEntityShadowTex) if (t) SDL_DestroyTexture(t);
isoEntityShadowTex.fill(nullptr);

isoTerrainAssetsValid = false;

// Decal overlay textures
for (auto& arr : floorDecalVar) {
    for (SDL_Texture*& t : arr) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}
for (auto& arr : wallDecalVar) {
    for (SDL_Texture*& t : arr) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}
floorDecalVar.clear();
wallDecalVar.clear();


// Isometric floor decal overlay textures (generated lazily with isometric terrain assets)
for (auto& arr : floorDecalVarIso) {
    for (SDL_Texture*& t : arr) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}
floorDecalVarIso.clear();



// Isometric edge shading overlays
for (auto& anim : isoEdgeShadeVar) {
    for (SDL_Texture*& t : anim) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

// Isometric chasm gloom overlays
for (auto& anim : isoChasmGloomVar) {
    for (SDL_Texture*& t : anim) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

// Isometric cast shadow overlays
for (auto& anim : isoCastShadowVar) {
    for (SDL_Texture*& t : anim) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}


// Autotile overlays
for (auto& maskArr : wallEdgeVar) {
    for (auto& anim : maskArr) {
        for (SDL_Texture*& t : anim) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
}
for (auto& maskArr : chasmRimVar) {
    for (auto& anim : maskArr) {
        for (SDL_Texture*& t : anim) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
}

for (auto& maskArr : topDownWallShadeVar) {
    for (auto& anim : maskArr) {
        for (SDL_Texture*& t : anim) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
}

// Confusion gas overlays
for (auto& anim : gasVar) {
    for (SDL_Texture*& t : anim) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

// Confusion gas overlays (isometric)
for (auto& anim : gasVarIso) {
    for (SDL_Texture*& t : anim) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

// Fire overlays
for (auto& anim : fireVar) {
    for (SDL_Texture*& t : anim) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

// Fire overlays (isometric)
for (auto& anim : fireVarIso) {
    for (SDL_Texture*& t : anim) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

// Status effect icons
for (auto& arr : effectIconTex) {
    for (SDL_Texture*& t : arr) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
}

roomTypeCache.clear();
roomCacheDungeon = nullptr;
roomCacheBranch = DungeonBranch::Main;
roomCacheDepth = -1;
roomCacheW = 0;
roomCacheH = 0;
roomCacheRooms = 0;

for (auto& t : uiPanelTileTex) if (t) SDL_DestroyTexture(t);
for (auto& t : uiOrnamentTex) if (t) SDL_DestroyTexture(t);
uiPanelTileTex.fill(nullptr);
uiOrnamentTex.fill(nullptr);
uiAssetsValid = false;

    for (auto& t : stairsUpOverlayTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : stairsDownOverlayTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : doorClosedOverlayTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : doorLockedOverlayTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : doorOpenOverlayTex) if (t) SDL_DestroyTexture(t);

    stairsUpOverlayTex.fill(nullptr);
    stairsDownOverlayTex.fill(nullptr);
    doorClosedOverlayTex.fill(nullptr);
    doorLockedOverlayTex.fill(nullptr);
    doorOpenOverlayTex.fill(nullptr);

    // Targeting / look cursor reticle overlays.
    for (auto& t : cursorReticleTex) if (t) SDL_DestroyTexture(t);
    for (auto& t : cursorReticleIsoTex) if (t) SDL_DestroyTexture(t);
    cursorReticleTex.fill(nullptr);
    cursorReticleIsoTex.fill(nullptr);

    // Entity/item/projectile textures are budget-cached in spriteTex.
    spriteTex.clear();

    // UI preview textures (Codex/etc.).
    uiPreviewTex.clear();

    // Renderer-owned procedural particle textures (visual-only).
    if (particles_) {
        particles_->shutdown();
        particles_.reset();
    }
    prevHpById_.clear();
    prevPosById_.clear();

    if (pixfmt) { SDL_FreeFormat(pixfmt); pixfmt = nullptr; }
    if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
    if (window) { SDL_DestroyWindow(window); window = nullptr; }

    initialized = false;
}

void Renderer::toggleFullscreen() {
    if (!window) return;
    const Uint32 flags = SDL_GetWindowFlags(window);
    const bool isFs = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    SDL_SetWindowFullscreen(window, isFs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

SDL_Rect Renderer::mapTileDst(int mapX, int mapY) const {
    // Map-space tiles are drawn relative to the camera and then optionally offset by transient
    // screen shake (mapOffX/Y).
    //
    // TopDown: camX/camY represents the viewport's top-left map tile.
    // Isometric: isoCamX/isoCamY represents the camera center tile.
    if (viewMode_ != ViewMode::Isometric) {
        return SDL_Rect{ (mapX - camX) * tile + mapOffX, (mapY - camY) * tile + mapOffY, tile, tile };
    }

    const int tileW = std::max(1, tile);
    const int tileH = std::max(1, tile / 2);

    const int halfW = std::max(1, tileW / 2);
    const int halfH = std::max(1, tileH / 2);

    const int mapH = std::max(0, winH - hudH);

    // Anchor the camera tile at the center of the map viewport (not including the HUD).
    const int cx = winW / 2 + mapOffX;
    const int cy = mapH / 2 + mapOffY;

    const int dx = mapX - isoCamX;
    const int dy = mapY - isoCamY;

    // Standard isometric projection (diamond grid).
    const int centerX = cx + (dx - dy) * halfW;
    const int centerY = cy + (dx + dy) * halfH;

    return SDL_Rect{ centerX - tileW / 2, centerY - tileH / 2, tileW, tileH };
}

SDL_Rect Renderer::mapSpriteDst(int mapX, int mapY) const {
    if (viewMode_ != ViewMode::Isometric) return mapTileDst(mapX, mapY);

    // Place sprites so their "feet" land on the center of the isometric tile.
    const SDL_Rect base = mapTileDst(mapX, mapY);
    const int cx = base.x + base.w / 2;
    const int cy = base.y + base.h / 2;

    const int spriteW = std::max(1, tile);
    const int spriteH = std::max(1, tile);

    // Nudge the foot point slightly downward so the sprite reads as standing on the tile.
    const int footY = cy + (base.h / 4);

    return SDL_Rect{ cx - spriteW / 2, footY - spriteH, spriteW, spriteH };
}

bool Renderer::mapTileInView(int mapX, int mapY) const {
    if (viewMode_ != ViewMode::Isometric) {
        return mapX >= camX && mapY >= camY &&
               mapX < (camX + viewTilesW) && mapY < (camY + viewTilesH);
    }

    // In isometric mode, the "viewport" is not axis-aligned in map-space, so we cull by screen rect.
    const SDL_Rect r = mapTileDst(mapX, mapY);
    const int mapH = std::max(0, winH - hudH);
    const int pad = std::max(0, tile); // allow for tall sprites that extend beyond the tile rect

    return !(r.x + r.w < -pad || r.y + r.h < -pad || r.x > (winW + pad) || r.y > (mapH + pad));
}

void Renderer::updateCamera(const Game& game) {
    const Dungeon& d = game.dungeon();


    // Re-derive viewport size in case logical sizing changed.
    const int t = std::max(1, tile);
    viewTilesW = std::max(1, winW / t);
    viewTilesH = std::max(1, std::max(0, winH - hudH) / t);

    // If the viewport fully contains the map, keep camera locked at origin.
    const int maxCamX = std::max(0, d.width - viewTilesW);
    const int maxCamY = std::max(0, d.height - viewTilesH);
    if (maxCamX == 0) camX = 0;
    if (maxCamY == 0) camY = 0;

    // Focus point selection:
    // - Normal: follow the player.
    // - Look: follow the look cursor (so you can pan around).
    // - Targeting: try to keep BOTH player and cursor on-screen if they fit,
    //   otherwise follow the cursor.
    Vec2i playerPos = game.player().pos;

    Vec2i cursorPos = playerPos;
    bool usingCursor = false;
    if (game.isLooking()) {
        cursorPos = game.lookCursor();
        usingCursor = true;
    } else if (game.isTargeting()) {
        cursorPos = game.targetingCursor();
        usingCursor = true;
    }

    // Isometric view: first pass is a simple centered camera on the current focus tile.
    // (TopDown mode retains the existing deadzone + targeting camera logic below.)
    if (viewMode_ == ViewMode::Isometric) {
        Vec2i focus = usingCursor ? cursorPos : playerPos;

        focus.x = std::clamp(focus.x, 0, std::max(0, d.width - 1));
        focus.y = std::clamp(focus.y, 0, std::max(0, d.height - 1));

        isoCamX = focus.x;
        isoCamY = focus.y;
        return;
    }

    auto clampCam = [&]() {
        camX = std::clamp(camX, 0, maxCamX);
        camY = std::clamp(camY, 0, maxCamY);
    };

    // Targeting: keep both points in view when possible.
    if (game.isTargeting() && usingCursor && (maxCamX > 0 || maxCamY > 0)) {
        const int minX = std::min(playerPos.x, cursorPos.x);
        const int maxX = std::max(playerPos.x, cursorPos.x);
        const int minY = std::min(playerPos.y, cursorPos.y);
        const int maxY = std::max(playerPos.y, cursorPos.y);

        if ((maxX - minX + 1) <= viewTilesW && (maxY - minY + 1) <= viewTilesH) {
            const int cx = (minX + maxX) / 2;
            const int cy = (minY + maxY) / 2;
            camX = cx - viewTilesW / 2;
            camY = cy - viewTilesH / 2;
            clampCam();
            return;
        }
    }

    // Deadzone follow (prevents jitter when moving near the center).
    Vec2i focus = usingCursor ? cursorPos : playerPos;

    // Clamp focus to map bounds defensively.
    focus.x = std::clamp(focus.x, 0, std::max(0, d.width - 1));
    focus.y = std::clamp(focus.y, 0, std::max(0, d.height - 1));

    // Margins: smaller viewports need smaller deadzones.
    const int marginX = std::clamp(viewTilesW / 4, 0, std::max(0, (viewTilesW - 1) / 2));
    const int marginY = std::clamp(viewTilesH / 4, 0, std::max(0, (viewTilesH - 1) / 2));

    if (maxCamX > 0) {
        const int left = camX + marginX;
        const int right = camX + viewTilesW - 1 - marginX;
        if (focus.x < left) camX = focus.x - marginX;
        else if (focus.x > right) camX = focus.x - (viewTilesW - 1 - marginX);
    }

    if (maxCamY > 0) {
        const int top = camY + marginY;
        const int bottom = camY + viewTilesH - 1 - marginY;
        if (focus.y < top) camY = focus.y - marginY;
        else if (focus.y > bottom) camY = focus.y - (viewTilesH - 1 - marginY);
    }

    clampCam();
}

bool Renderer::windowToMapTile(const Game& game, int winX, int winY, int& tileX, int& tileY) const {
    if (!renderer) return false;

    const Dungeon& d = game.dungeon();
    const int W = d.width;
    const int H = d.height;
    if (W <= 0 || H <= 0) return false;

    float lx = 0.0f, ly = 0.0f;
    SDL_RenderWindowToLogical(renderer, winX, winY, &lx, &ly);

    const int x = static_cast<int>(lx);
    const int y = static_cast<int>(ly);

    if (x < 0 || y < 0) return false;

    // Map rendering can be temporarily offset (screen shake). Convert clicks in window coordinates
    // back into stable viewport coordinates.
    const int mx = x - mapOffX;
    const int my = y - mapOffY;

    if (mx < 0 || my < 0) return false;

    const int mapH = std::max(0, winH - hudH);

    // Reject clicks outside the map viewport (e.g., HUD area).
    if (my >= mapH) return false;

    if (viewMode_ == ViewMode::Isometric) {
        // Invert the isometric projection used by mapTileDst(), then refine by
        // diamond hit-testing so mouse clicks feel crisp near tile edges.
        const int tileW = std::max(1, tile);
        const int tileH = std::max(1, tile / 2);

        const int halfW = std::max(1, tileW / 2);
        const int halfH = std::max(1, tileH / 2);

        const int cx = winW / 2;
        const int cy = mapH / 2;

        const float dx = static_cast<float>(mx - cx);
        const float dy = static_cast<float>(my - cy);

        const float fx = (dx / static_cast<float>(halfW) + dy / static_cast<float>(halfH)) * 0.5f;
        const float fy = (dy / static_cast<float>(halfH) - dx / static_cast<float>(halfW)) * 0.5f;

        auto roundToInt = [](float v) -> int {
            // Symmetric rounding for negatives.
            return (v >= 0.0f) ? static_cast<int>(std::floor(v + 0.5f)) : static_cast<int>(std::ceil(v - 0.5f));
        };

        const int rx = isoCamX + roundToInt(fx);
        const int ry = isoCamY + roundToInt(fy);

        // Candidate search: the point should lie within the diamond of one of the
        // nearby tiles. We check a small neighborhood around the rounded guess.
        int bestX = rx;
        int bestY = ry;
        int bestD2 = std::numeric_limits<int>::max();
        bool found = false;

        auto isoTileRectStable = [&](int mapX, int mapY) -> SDL_Rect {
            const int dxm = mapX - isoCamX;
            const int dym = mapY - isoCamY;

            const int centerX = cx + (dxm - dym) * halfW;
            const int centerY = cy + (dxm + dym) * halfH;

            return SDL_Rect{ centerX - tileW / 2, centerY - tileH / 2, tileW, tileH };
        };

        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                const int candX = rx + ox;
                const int candY = ry + oy;
                if (candX < 0 || candY < 0 || candX >= W || candY >= H) continue;

                const SDL_Rect rect = isoTileRectStable(candX, candY);
                if (!pointInIsoDiamond(mx, my, rect)) continue;

                const int ccx = rect.x + rect.w / 2;
                const int ccy = rect.y + rect.h / 2;
                const int ddx = mx - ccx;
                const int ddy = my - ccy;
                const int d2 = ddx * ddx + ddy * ddy;

                if (d2 < bestD2) {
                    bestD2 = d2;
                    bestX = candX;
                    bestY = candY;
                    found = true;
                }
            }
        }

        tileX = found ? bestX : rx;
        tileY = found ? bestY : ry;

        if (tileX < 0 || tileY < 0 || tileX >= W || tileY >= H) return false;
        return true;
    }

    const int localX = mx / std::max(1, tile);
    const int localY = my / std::max(1, tile);

    // Reject clicks outside the map viewport.
    if (localX < 0 || localY < 0 || localX >= viewTilesW || localY >= viewTilesH) return false;

    tileX = localX + camX;
    tileY = localY + camY;

    if (tileX < 0 || tileY < 0 || tileX >= W || tileY >= H) return false;
    return true;
}

bool Renderer::windowToMinimapTile(const Game& game, int winX, int winY, int& tileX, int& tileY) const {
    if (!renderer) return false;

    float lx = 0.0f, ly = 0.0f;
    SDL_RenderWindowToLogical(renderer, winX, winY, &lx, &ly);

    const int x = static_cast<int>(lx);
    const int y = static_cast<int>(ly);
    if (x < 0 || y < 0) return false;

    const Dungeon& d = game.dungeon();
    const int W = d.width;
    const int H = d.height;
    if (W <= 0 || H <= 0) return false;

    // Mirror drawMinimapOverlay layout so hit-testing matches visuals.
    int px = 4 + game.minimapZoom();
    px = std::clamp(px, 2, 12);

    const int pad = 10;
    const int margin = 10;
    const int titleH = 30; // matches drawMinimapOverlay (title + hints + info)
    const int maxW = winW / 2;
    const int maxH = (winH - hudH) / 2;
    while (px > 2 && (W * px + pad * 2) > maxW) px--;
    while (px > 2 && (H * px + pad * 2 + titleH) > maxH) px--;

    const int panelW = W * px + pad * 2;

    const int x0 = winW - panelW - margin;
    const int y0 = margin;

    const int mapX = x0 + pad;
    const int mapY = y0 + pad + titleH;

    if (x < mapX || y < mapY) return false;
    if (x >= mapX + W * px || y >= mapY + H * px) return false;

    const int tx = (x - mapX) / px;
    const int ty = (y - mapY) / px;

    tileX = std::clamp(tx, 0, W - 1);
    tileY = std::clamp(ty, 0, H - 1);
    return true;
}



void Renderer::updateParticlesFromGame(const Game& game, float frameDt, uint32_t ticks) {
    if (!particles_) return;

    // Clear between runs/floors so old particles don't "leak" across levels.
    const uint32_t runSeed = static_cast<uint32_t>(game.seed());
    if (prevParticleSeed_ != runSeed || prevParticleBranch_ != game.branch() || prevParticleDepth_ != game.depth()) {
        particles_->clear();
        prevHpById_.clear();
        prevPosById_.clear();
        prevParticleSeed_ = runSeed;
        prevParticleBranch_ = game.branch();
        prevParticleDepth_ = game.depth();
    }

    const Dungeon& d = game.dungeon();
    const int playerId = game.player().id;

    // Small hash-based RNG helper (visual-only; does not touch game RNG).
    auto rand01 = [](uint32_t& s) -> float {
        s = hash32(s + 0x9E3779B9u);
        return static_cast<float>(s & 0x00FFFFFFu) * (1.0f / 16777216.0f);
    };
    auto randRange = [&](uint32_t& s, float a, float b) -> float { return a + (b - a) * rand01(s); };

    auto isVisibleTile = [&](const Vec2i& p) -> bool {
        if (!d.inBounds(p.x, p.y)) return false;
        return d.at(p.x, p.y).visible;
    };

    // ---------------------------------------------------------------------
    // 1) Entity hit/death bursts (based on HP deltas between rendered frames)
    // ---------------------------------------------------------------------
    std::unordered_map<int, int> curHp;
    std::unordered_map<int, Vec2i> curPos;
    curHp.reserve(game.entities().size());
    curPos.reserve(game.entities().size());

    for (const Entity& e : game.entities()) {
        curHp[e.id] = e.hp;
        curPos[e.id] = e.pos;

        const bool isPlayer = (e.id == playerId);
        if (!isPlayer && !isVisibleTile(e.pos)) continue;

        auto itPrevHp = prevHpById_.find(e.id);
        if (itPrevHp != prevHpById_.end()) {
            const int prevHp = itPrevHp->second;
            const int dmg = prevHp - e.hp;
            if (dmg > 0) {
                uint32_t s = hashCombine(hashCombine(runSeed, static_cast<uint32_t>(game.turns())),
                                        hashCombine(static_cast<uint32_t>(e.id), static_cast<uint32_t>((prevHp & 0xFFFF) | ((e.hp & 0xFFFF) << 16))));
                const int count = std::clamp(dmg * 5, 6, 34);

                for (int i = 0; i < count; ++i) {
                    const float ang = randRange(s, 0.0f, 6.28318f);
                    const float sp = randRange(s, 1.5f, 4.5f);
                    const float vx = std::cos(ang) * sp;
                    const float vy = std::sin(ang) * sp;

                    ParticleEngine::Particle p;
                    p.x = static_cast<float>(e.pos.x) + 0.5f + randRange(s, -0.28f, 0.28f);
                    p.y = static_cast<float>(e.pos.y) + 0.5f + randRange(s, -0.28f, 0.28f);
                    p.z = randRange(s, 0.05f, 0.20f);

                    p.vx = vx;
                    p.vy = vy;
                    p.vz = randRange(s, 2.0f, 5.0f);

                    p.az = -9.0f;   // gravity
                    p.drag = 1.8f;

                    p.life = randRange(s, 0.18f, 0.42f);
                    p.size0 = randRange(s, 0.06f, 0.13f);
                    p.size1 = p.size0 * 0.55f;

                    // Player gets brighter "spark" feedback; monsters bleed.
                    if (isPlayer) {
                        p.kind = ParticleEngine::Kind::Spark;
                        p.var = static_cast<uint8_t>(static_cast<int>(randRange(s, 0.0f, static_cast<float>(ParticleEngine::SPARK_VARS))));
                        p.c0 = Color{255, 240, 210, 220};
                        p.c1 = Color{255, 120, 40, 0};
                    } else {
                        p.kind = ParticleEngine::Kind::Smoke;
                        p.var = static_cast<uint8_t>(static_cast<int>(randRange(s, 0.0f, static_cast<float>(ParticleEngine::SMOKE_VARS))));
                        p.c0 = Color{180, 40, 35, 165};
                        p.c1 = Color{80, 10, 10, 0};
                        p.vz *= 0.55f; // heavier
                    }

                    p.layer = ParticleEngine::LAYER_FRONT;
                    p.seed = s;

                    particles_->add(p);
                }
            }
        }
    }

    // If a monster disappears between frames, assume it died and emit a larger burst at last known position.
    for (const auto& [id, prevHp] : prevHpById_) {
        if (curHp.find(id) != curHp.end()) continue;
        if (id == playerId) continue;
        if (prevHp <= 0) continue;

        auto itPos = prevPosById_.find(id);
        if (itPos == prevPosById_.end()) continue;
        if (!isVisibleTile(itPos->second)) continue;

        uint32_t s = hashCombine(hashCombine(runSeed, static_cast<uint32_t>(game.turns())),
                                hashCombine(static_cast<uint32_t>(id), static_cast<uint32_t>(ticks)));
        const int count = 28;

        for (int i = 0; i < count; ++i) {
            const float ang = randRange(s, 0.0f, 6.28318f);
            const float sp = randRange(s, 2.0f, 6.0f);

            ParticleEngine::Particle p;
            p.x = static_cast<float>(itPos->second.x) + 0.5f + randRange(s, -0.38f, 0.38f);
            p.y = static_cast<float>(itPos->second.y) + 0.5f + randRange(s, -0.38f, 0.38f);
            p.z = randRange(s, 0.10f, 0.30f);

            p.vx = std::cos(ang) * sp;
            p.vy = std::sin(ang) * sp;
            p.vz = randRange(s, 2.5f, 6.5f);

            p.az = -10.0f;
            p.drag = 2.1f;

            p.life = randRange(s, 0.25f, 0.55f);
            p.size0 = randRange(s, 0.08f, 0.16f);
            p.size1 = p.size0 * 0.55f;

            p.kind = ParticleEngine::Kind::Smoke;
            p.var = static_cast<uint8_t>(static_cast<int>(randRange(s, 0.0f, static_cast<float>(ParticleEngine::SMOKE_VARS))));
            p.c0 = Color{190, 40, 35, 180};
            p.c1 = Color{60, 10, 10, 0};

            p.layer = ParticleEngine::LAYER_FRONT;
            p.seed = s;
            particles_->add(p);
        }
    }

    // Commit current caches.
    prevHpById_ = std::move(curHp);
    prevPosById_ = std::move(curPos);

    // ---------------------------------------------------------------------
    // 2) Projectile trails
    // ---------------------------------------------------------------------
    for (size_t i = 0; i < game.fxProjectiles().size(); ++i) {
        const FXProjectile& fx = game.fxProjectiles()[i];
        if (fx.path.empty()) continue;

        const size_t pi = std::min(fx.pathIndex, fx.path.size() - 1);
        const Vec2i pos = fx.path[pi];
        if (!d.inBounds(pos.x, pos.y)) continue;
        if (!d.at(pos.x, pos.y).visible) continue;

        Vec2i prev = pos;
        if (pi > 0) prev = fx.path[pi - 1];

        const float dirx = static_cast<float>(pos.x - prev.x);
        const float diry = static_cast<float>(pos.y - prev.y);

        uint32_t s = hashCombine(hashCombine(runSeed, static_cast<uint32_t>(ticks)),
                                hashCombine(static_cast<uint32_t>(i), static_cast<uint32_t>(fx.kind)));

        auto emitSmokePuff = [&](Color c0, Color c1, float size0, float size1, float life) {
            ParticleEngine::Particle p;
            p.x = static_cast<float>(pos.x) + 0.5f + randRange(s, -0.20f, 0.20f);
            p.y = static_cast<float>(pos.y) + 0.5f + randRange(s, -0.20f, 0.20f);
            p.z = randRange(s, 0.02f, 0.10f);

            p.vx = -dirx * randRange(s, 0.6f, 1.4f) + randRange(s, -0.35f, 0.35f);
            p.vy = -diry * randRange(s, 0.6f, 1.4f) + randRange(s, -0.35f, 0.35f);
            p.vz = randRange(s, 0.2f, 0.8f);

            p.drag = 2.8f;

            p.life = life;
            p.size0 = size0;
            p.size1 = size1;

            p.kind = ParticleEngine::Kind::Smoke;
            p.var = static_cast<uint8_t>(static_cast<int>(randRange(s, 0.0f, static_cast<float>(ParticleEngine::SMOKE_VARS))));
            p.c0 = c0;
            p.c1 = c1;

            p.layer = ParticleEngine::LAYER_BEHIND;
            p.seed = s;
            particles_->add(p);
        };

        auto emitEmber = [&](Color c0, Color c1, float size0, float size1, float life) {
            ParticleEngine::Particle p;
            p.x = static_cast<float>(pos.x) + 0.5f + randRange(s, -0.18f, 0.18f);
            p.y = static_cast<float>(pos.y) + 0.5f + randRange(s, -0.18f, 0.18f);
            p.z = randRange(s, 0.02f, 0.12f);

            p.vx = -dirx * randRange(s, 0.4f, 1.2f) + randRange(s, -0.45f, 0.45f);
            p.vy = -diry * randRange(s, 0.4f, 1.2f) + randRange(s, -0.45f, 0.45f);
            p.vz = randRange(s, 0.8f, 2.2f);

            p.az = -6.0f;
            p.drag = 1.6f;

            p.life = life;
            p.size0 = size0;
            p.size1 = size1;

            p.kind = ParticleEngine::Kind::Ember;
            p.var = static_cast<uint8_t>(static_cast<int>(randRange(s, 0.0f, static_cast<float>(ParticleEngine::EMBER_VARS))));
            p.c0 = c0;
            p.c1 = c1;

            p.layer = ParticleEngine::LAYER_BEHIND;
            p.seed = s;
            particles_->add(p);
        };

        if (fx.kind == ProjectileKind::Arrow || fx.kind == ProjectileKind::Rock) {
            emitSmokePuff(Color{185, 185, 190, 70}, Color{110, 110, 120, 0}, 0.08f, 0.16f, randRange(s, 0.16f, 0.26f));
        } else if (fx.kind == ProjectileKind::Spark) {
            emitEmber(Color{255, 230, 170, 170}, Color{255, 130, 60, 0}, 0.06f, 0.03f, randRange(s, 0.12f, 0.20f));
        } else if (fx.kind == ProjectileKind::Fireball || fx.kind == ProjectileKind::Torch) {
            emitEmber(Color{255, 210, 120, 180}, Color{255, 80, 30, 0}, 0.06f, 0.03f, randRange(s, 0.14f, 0.24f));
            emitSmokePuff(Color{55, 35, 20, 80}, Color{20, 15, 10, 0}, 0.10f, 0.22f, randRange(s, 0.30f, 0.55f));
        }
    }

    // ---------------------------------------------------------------------
    // 3) Explosions (sparks + lingering smoke)
    // ---------------------------------------------------------------------
    const float dt = std::clamp(frameDt, 0.0f, 0.10f);
    if (dt > 0.0f) {
        for (const FXExplosion& ex : game.fxExplosions()) {
            if (ex.delay > 0.0f) continue;
            if (ex.tiles.empty()) continue;

            bool anyVisible = false;
            for (const Vec2i& t : ex.tiles) {
                if (d.inBounds(t.x, t.y) && d.at(t.x, t.y).visible) { anyVisible = true; break; }
            }
            if (!anyVisible) continue;

            const float t01 = std::clamp(ex.timer / std::max(0.0001f, ex.duration), 0.0f, 1.0f);
            const float intensity = 1.0f - t01;

            // Scale with explosion footprint, but sub-linear.
            const float scale = std::sqrt(std::max(1.0f, static_cast<float>(ex.tiles.size())));

            const float sparksF = dt * (520.0f * intensity) * (0.35f + 0.18f * scale);
            const float smokeF  = dt * (170.0f * intensity) * (0.35f + 0.14f * scale);

            uint32_t s = hashCombine(hashCombine(runSeed, static_cast<uint32_t>(ticks)),
                                    hashCombine(static_cast<uint32_t>(ex.tiles.size()), static_cast<uint32_t>(game.turns())));

            auto takeCount = [&](float f, int maxCount) {
                int n = static_cast<int>(std::floor(f));
                const float frac = f - static_cast<float>(n);
                if (rand01(s) < frac) ++n;
                return std::min(n, maxCount);
            };

            const int sparks = takeCount(sparksF, 42);
            const int smokes = takeCount(smokeF, 18);

            // Compute explosion centroid for outward bias.
            Vec2f center{0.0f, 0.0f};
            for (const Vec2i& t : ex.tiles) {
                center.x += static_cast<float>(t.x) + 0.5f;
                center.y += static_cast<float>(t.y) + 0.5f;
            }
            center.x /= static_cast<float>(ex.tiles.size());
            center.y /= static_cast<float>(ex.tiles.size());

            for (int i = 0; i < sparks; ++i) {
                const Vec2i spawnTile = ex.tiles[static_cast<size_t>(randRange(s, 0.0f, static_cast<float>(ex.tiles.size()))) % ex.tiles.size()];

                const float tx = static_cast<float>(spawnTile.x) + 0.5f;
                const float ty = static_cast<float>(spawnTile.y) + 0.5f;

                float dx = tx - center.x;
                float dy = ty - center.y;
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len > 0.001f) { dx /= len; dy /= len; }

                const float ang = std::atan2(dy, dx) + randRange(s, -1.1f, 1.1f);
                const float sp  = randRange(s, 2.5f, 7.0f);

                ParticleEngine::Particle p;
                p.x = tx + randRange(s, -0.40f, 0.40f);
                p.y = ty + randRange(s, -0.40f, 0.40f);
                p.z = randRange(s, 0.10f, 0.40f);

                p.vx = std::cos(ang) * sp;
                p.vy = std::sin(ang) * sp;
                p.vz = randRange(s, 2.0f, 6.5f);

                p.az = -10.5f;
                p.drag = 1.2f;

                p.life = randRange(s, 0.16f, 0.45f);
                p.size0 = randRange(s, 0.05f, 0.12f);
                p.size1 = p.size0 * 0.55f;

                p.kind = ParticleEngine::Kind::Spark;
                p.var = static_cast<uint8_t>(static_cast<int>(randRange(s, 0.0f, static_cast<float>(ParticleEngine::SPARK_VARS))));
                p.c0 = Color{255, 245, 225, 230};
                p.c1 = Color{255, 120, 40, 0};

                p.layer = ParticleEngine::LAYER_FRONT;
                p.seed = s;
                particles_->add(p);
            }

            for (int i = 0; i < smokes; ++i) {
                const Vec2i spawnTile = ex.tiles[static_cast<size_t>(randRange(s, 0.0f, static_cast<float>(ex.tiles.size()))) % ex.tiles.size()];
                const float tx = static_cast<float>(spawnTile.x) + 0.5f;
                const float ty = static_cast<float>(spawnTile.y) + 0.5f;

                ParticleEngine::Particle p;
                p.x = tx + randRange(s, -0.45f, 0.45f);
                p.y = ty + randRange(s, -0.45f, 0.45f);
                p.z = randRange(s, 0.05f, 0.25f);

                p.vx = randRange(s, -0.35f, 0.35f);
                p.vy = randRange(s, -0.35f, 0.35f);
                p.vz = randRange(s, 0.6f, 1.8f);

                p.drag = 1.9f;

                p.life = randRange(s, 0.55f, 1.25f);
                p.size0 = randRange(s, 0.20f, 0.38f);
                p.size1 = p.size0 * randRange(s, 1.4f, 2.1f);

                p.kind = ParticleEngine::Kind::Smoke;
                p.var = static_cast<uint8_t>(static_cast<int>(randRange(s, 0.0f, static_cast<float>(ParticleEngine::SMOKE_VARS))));
                p.c0 = Color{55, 32, 18, 95};
                p.c1 = Color{25, 15, 10, 0};

                p.layer = ParticleEngine::LAYER_FRONT;
                p.seed = s;
                particles_->add(p);
            }
        }
    }

    // ---------------------------------------------------------------------
    // 4) Fire field embers (lightweight probabilistic emission)
    // ---------------------------------------------------------------------
    if (dt > 0.0f) {
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                if (!mapTileInView(x, y)) continue;
                if (!d.at(x, y).visible) continue;

                const uint8_t f = game.fireAt(x, y);
                if (f == 0) continue;

                // Particles per second per tile (scaled by intensity).
                const float rate = 0.35f + 0.12f * static_cast<float>(f);
                const float pEmit = std::min(0.60f, rate * dt);

                uint32_t s = hashCombine(hashCombine(runSeed, static_cast<uint32_t>(ticks / 7u)),
                                        hashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y)));
                if (rand01(s) > pEmit) continue;

                ParticleEngine::Particle p;
                p.x = static_cast<float>(x) + 0.5f + randRange(s, -0.28f, 0.28f);
                p.y = static_cast<float>(y) + 0.5f + randRange(s, -0.28f, 0.28f);
                p.z = randRange(s, 0.02f, 0.10f);

                p.vx = randRange(s, -0.55f, 0.55f);
                p.vy = randRange(s, -0.55f, 0.55f);
                p.vz = randRange(s, 1.2f, 2.8f);

                p.az = -6.5f;
                p.drag = 1.4f;

                p.life = randRange(s, 0.18f, 0.48f);
                p.size0 = randRange(s, 0.05f, 0.11f);
                p.size1 = p.size0 * 0.55f;

                p.kind = ParticleEngine::Kind::Ember;
                p.var = static_cast<uint8_t>(static_cast<int>(randRange(s, 0.0f, static_cast<float>(ParticleEngine::EMBER_VARS))));
                p.c0 = Color{255, 215, 120, 195};
                p.c1 = Color{255, 90, 35, 0};

                p.layer = ParticleEngine::LAYER_FRONT;
                p.seed = s;
                particles_->add(p);
            }
        }
    }


    // ---------------------------------------------------------------------
    // 5) Game-driven particle events (spell casts, digging, etc.)
    // ---------------------------------------------------------------------
    if (dt > 0.0f) {
        constexpr float TAU = 6.283185307f;

        for (const FXParticleEvent& ev : game.fxParticles()) {
            if (ev.delay > 0.0f) continue;
            if (!d.inBounds(ev.pos.x, ev.pos.y)) continue;

            // Skip off-screen / unseen events (except on the player).
            const bool isOnPlayer = (ev.pos == game.player().pos);
            if (!isOnPlayer) {
                if (!mapTileInView(ev.pos.x, ev.pos.y)) continue;
                if (!d.at(ev.pos.x, ev.pos.y).visible) continue;
            }

            const float t01 =
                (ev.duration > 0.001f) ? std::clamp(ev.timer / ev.duration, 0.0f, 1.0f) : 1.0f;
            const float strength = std::clamp(static_cast<float>(ev.intensity) / 10.0f, 0.25f, 6.0f);

            float baseRate = 90.0f;
            switch (ev.preset) {
                case FXParticlePreset::Heal:         baseRate = 140.0f; break;
                case FXParticlePreset::Buff:         baseRate = 120.0f; break;
                case FXParticlePreset::Invisibility: baseRate = 90.0f; break;
                case FXParticlePreset::Blink:        baseRate = 220.0f; break;
                case FXParticlePreset::Poison:       baseRate = 110.0f; break;
                case FXParticlePreset::Dig:          baseRate = 150.0f; break;
                case FXParticlePreset::Detect:       baseRate = 120.0f; break;
                default: break;
            }

            const float fade = 0.35f + 0.65f * (1.0f - t01);
            float want = baseRate * strength * fade * dt;
            int emitCount = static_cast<int>(want);

            uint32_t s0 = hashCombine(ev.seed,
                                      hashCombine(static_cast<uint32_t>(ticks / 5u),
                                                  static_cast<uint32_t>(ev.timer * 1000.0f)));
            if (rand01(s0) < (want - static_cast<float>(emitCount))) ++emitCount;
            emitCount = std::min(emitCount, 64);

            const float cx = static_cast<float>(ev.pos.x) + 0.5f;
            const float cy = static_cast<float>(ev.pos.y) + 0.5f;

            for (int i = 0; i < emitCount; ++i) {
                uint32_t s = hashCombine(s0, static_cast<uint32_t>(i + 1));

                ParticleEngine::Particle p{};
                p.x = cx + randRange(s, -0.35f, 0.35f);
                p.y = cy + randRange(s, -0.35f, 0.35f);
                p.z = randRange(s, 0.04f, 0.22f);

                const float ang = randRange(s, 0.0f, TAU);
                const float sp = randRange(s, 0.15f, 1.10f);
                p.vx = std::cos(ang) * sp;
                p.vy = std::sin(ang) * sp;
                p.vz = randRange(s, 0.6f, 2.4f);

                p.az = -6.5f;
                p.drag = 1.35f;

                p.life = randRange(s, 0.18f, 0.55f);
                p.size0 = randRange(s, 0.05f, 0.14f);
                p.size1 = p.size0 * randRange(s, 0.45f, 0.85f);

                p.layer = ParticleEngine::LAYER_FRONT;
                p.seed = s;

                switch (ev.preset) {
                    case FXParticlePreset::Heal: {
                        // Mostly sparks, with a few soft embers for "warmth".
                        const bool ember = (rand01(s) < 0.25f);
                        if (ember) {
                            p.kind = ParticleEngine::Kind::Ember;
                            p.var = static_cast<uint8_t>(static_cast<int>(
                                randRange(s, 0.0f, static_cast<float>(ParticleEngine::EMBER_VARS))));
                            p.c0 = Color{140, 255, 190, 190};
                            p.c1 = Color{60, 140, 80, 0};
                            p.life = randRange(s, 0.20f, 0.45f);
                            p.size0 = randRange(s, 0.05f, 0.11f);
                            p.size1 = p.size0 * 0.55f;
                            p.vz = randRange(s, 1.2f, 3.0f);
                            p.az = -8.5f;
                            p.drag = 1.45f;
                        } else {
                            p.kind = ParticleEngine::Kind::Spark;
                            p.var = static_cast<uint8_t>(static_cast<int>(
                                randRange(s, 0.0f, static_cast<float>(ParticleEngine::SPARK_VARS))));
                            p.c0 = Color{110, 255, 170, 210};
                            p.c1 = Color{40, 90, 55, 0};
                            p.life = randRange(s, 0.15f, 0.42f);
                            p.size0 = randRange(s, 0.05f, 0.12f);
                            p.size1 = p.size0 * 0.55f;
                            p.vz = randRange(s, 1.0f, 2.4f);
                            p.az = -7.5f;
                            p.drag = 1.40f;
                        }
                        break;
                    }

                    case FXParticlePreset::Buff: {
                        const bool ember = (rand01(s) < 0.35f);
                        if (ember) {
                            p.kind = ParticleEngine::Kind::Ember;
                            p.var = static_cast<uint8_t>(static_cast<int>(
                                randRange(s, 0.0f, static_cast<float>(ParticleEngine::EMBER_VARS))));
                            p.c0 = Color{255, 230, 140, 190};
                            p.c1 = Color{255, 90, 35, 0};
                            p.life = randRange(s, 0.20f, 0.50f);
                            p.size0 = randRange(s, 0.05f, 0.11f);
                            p.size1 = p.size0 * 0.55f;
                            p.vz = randRange(s, 1.0f, 2.7f);
                            p.az = -8.0f;
                        } else {
                            p.kind = ParticleEngine::Kind::Spark;
                            p.var = static_cast<uint8_t>(static_cast<int>(
                                randRange(s, 0.0f, static_cast<float>(ParticleEngine::SPARK_VARS))));
                            p.c0 = Color{255, 245, 170, 205};
                            p.c1 = Color{160, 110, 30, 0};
                            p.life = randRange(s, 0.16f, 0.45f);
                            p.size0 = randRange(s, 0.05f, 0.12f);
                            p.size1 = p.size0 * 0.55f;
                            p.vz = randRange(s, 0.9f, 2.3f);
                            p.az = -7.0f;
                        }
                        break;
                    }

                    case FXParticlePreset::Invisibility: {
                        p.kind = ParticleEngine::Kind::Smoke;
                        p.var = static_cast<uint8_t>(static_cast<int>(
                            randRange(s, 0.0f, static_cast<float>(ParticleEngine::SMOKE_VARS))));
                        p.c0 = Color{175, 120, 200, 95};
                        p.c1 = Color{40, 20, 60, 0};
                        p.life = randRange(s, 0.45f, 1.15f);
                        p.size0 = randRange(s, 0.14f, 0.34f);
                        p.size1 = p.size0 * randRange(s, 1.25f, 1.85f);
                        p.vx *= 0.45f;
                        p.vy *= 0.45f;
                        p.vz = randRange(s, 0.3f, 1.3f);
                        p.az = -1.4f;
                        p.drag = 1.15f;
                        break;
                    }

                    case FXParticlePreset::Blink: {
                        const bool smoke = (rand01(s) < 0.65f);
                        if (smoke) {
                            p.kind = ParticleEngine::Kind::Smoke;
                            p.var = static_cast<uint8_t>(static_cast<int>(
                                randRange(s, 0.0f, static_cast<float>(ParticleEngine::SMOKE_VARS))));
                            p.c0 = Color{210, 210, 255, 110};
                            p.c1 = Color{60, 60, 120, 0};
                            p.life = randRange(s, 0.22f, 0.65f);
                            p.size0 = randRange(s, 0.16f, 0.42f);
                            p.size1 = p.size0 * randRange(s, 1.15f, 1.75f);
                            p.vx *= 0.75f;
                            p.vy *= 0.75f;
                            p.vz = randRange(s, 0.8f, 2.5f);
                            p.az = -5.5f;
                            p.drag = 1.25f;
                        } else {
                            p.kind = ParticleEngine::Kind::Spark;
                            p.var = static_cast<uint8_t>(static_cast<int>(
                                randRange(s, 0.0f, static_cast<float>(ParticleEngine::SPARK_VARS))));
                            p.c0 = Color{220, 220, 255, 210};
                            p.c1 = Color{130, 80, 255, 0};
                            p.life = randRange(s, 0.10f, 0.32f);
                            p.size0 = randRange(s, 0.05f, 0.12f);
                            p.size1 = p.size0 * 0.55f;
                            p.vz = randRange(s, 0.9f, 2.7f);
                            p.az = -7.5f;
                            p.drag = 1.35f;
                        }
                        break;
                    }

                    case FXParticlePreset::Poison: {
                        p.kind = ParticleEngine::Kind::Smoke;
                        p.var = static_cast<uint8_t>(static_cast<int>(
                            randRange(s, 0.0f, static_cast<float>(ParticleEngine::SMOKE_VARS))));
                        p.c0 = Color{90, 220, 120, 105};
                        p.c1 = Color{20, 60, 30, 0};
                        p.life = randRange(s, 0.55f, 1.35f);
                        p.size0 = randRange(s, 0.18f, 0.46f);
                        p.size1 = p.size0 * randRange(s, 1.25f, 1.95f);
                        p.vx *= 0.30f;
                        p.vy *= 0.30f;
                        p.vz = randRange(s, 0.15f, 0.85f);
                        p.az = -1.2f;
                        p.drag = 1.10f;
                        break;
                    }

                    case FXParticlePreset::Dig: {
                        p.kind = ParticleEngine::Kind::Smoke;
                        p.var = static_cast<uint8_t>(static_cast<int>(
                            randRange(s, 0.0f, static_cast<float>(ParticleEngine::SMOKE_VARS))));
                        p.c0 = Color{155, 135, 110, 120};
                        p.c1 = Color{50, 40, 30, 0};
                        p.life = randRange(s, 0.25f, 0.70f);
                        p.size0 = randRange(s, 0.16f, 0.38f);
                        p.size1 = p.size0 * randRange(s, 1.20f, 1.70f);
                        p.vx *= 0.65f;
                        p.vy *= 0.65f;
                        p.vz = randRange(s, 0.4f, 1.6f);
                        p.az = -4.5f;
                        p.drag = 1.25f;
                        break;
                    }

                    case FXParticlePreset::Detect: {
                        p.kind = ParticleEngine::Kind::Spark;
                        p.var = static_cast<uint8_t>(static_cast<int>(
                            randRange(s, 0.0f, static_cast<float>(ParticleEngine::SPARK_VARS))));
                        p.c0 = Color{120, 220, 255, 205};
                        p.c1 = Color{40, 80, 120, 0};
                        p.life = randRange(s, 0.16f, 0.48f);
                        p.size0 = randRange(s, 0.05f, 0.12f);
                        p.size1 = p.size0 * 0.55f;
                        p.vz = randRange(s, 0.8f, 2.0f);
                        p.az = -6.5f;
                        p.drag = 1.35f;
                        break;
                    }

                    default:
                        break;
                }

                particles_->add(p);
            }
        }
    }


    // ---------------------------------------------------------------------
    // 6) Ambient environmental emitters (visual-only, procedural)
    // ---------------------------------------------------------------------
    // We use a phase-crossing test so emission is stable across frame rates:
    // each tile gets a deterministic phase in [0, stepMs), and we only emit when
    // that phase wraps within this frame's delta window.
    const uint32_t dtMsRaw = static_cast<uint32_t>(std::clamp(frameDt, 0.0f, 0.25f) * 1000.0f + 0.5f);
    const uint32_t dtMs = std::max<uint32_t>(1u, std::min<uint32_t>(dtMsRaw, 200u));

    const bool dark = game.darknessActive();

    // Deterministic per-level salt (visual-only).
    const uint32_t lvlSalt = hashCombine(hashCombine(runSeed ^ 0xA11CE5u, static_cast<uint32_t>(game.branch())),
                                         static_cast<uint32_t>(game.depth()));

    for (int y = 0; y < d.height; ++y) {
        for (int x = 0; x < d.width; ++x) {
            const Tile& t = d.at(x, y);
            if (!t.visible) continue;

            const TileType tt = t.type;
            if (tt != TileType::Fountain && tt != TileType::Altar) continue;

            const uint8_t L = dark ? game.tileLightLevel(x, y) : 255u;
            if (dark && L == 0u) continue;

            const float lum = static_cast<float>(L) * (1.0f / 255.0f);

            // Unique seed per tile (stable across the floor).
            const uint32_t tileSeed = hashCombine(lvlSalt, hashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y)));

            if (tt == TileType::Fountain) {
                // Cool mist puffs (subtle).
                const uint32_t stepMs = 240u;
                const uint32_t dtClamped = std::min<uint32_t>(dtMs, stepMs - 1u);

                const uint32_t phase = hash32(tileSeed ^ 0xF00D1234u) % stepMs;
                const uint32_t now = (ticks + phase) % stepMs;
                const uint32_t prev = (((ticks > dtClamped) ? (ticks - dtClamped) : 0u) + phase) % stepMs;

                if (now < prev) { // phase wrapped this frame
                    const uint32_t cycle = (ticks + phase) / stepMs;
                    uint32_t s = hash32(tileSeed ^ 0xF00D1234u ^ (cycle * 0x9E3779B9u));

                    // Chance per cycle (roughly ~0.5 spawns/sec per visible fountain).
                    if ((s & 0xFFu) < 34u) {
                        ParticleEngine::Particle p{};
                        p.layer = ParticleEngine::LAYER_BEHIND;
                        p.kind = ParticleEngine::Kind::Smoke;
                        p.var = static_cast<uint8_t>(static_cast<int>(
                            randRange(s, 0.0f, static_cast<float>(ParticleEngine::SMOKE_VARS))));
                        p.seed = s;

                        p.x = static_cast<float>(x) + 0.50f + randRange(s, -0.18f, 0.18f);
                        p.y = static_cast<float>(y) + 0.50f + randRange(s, -0.18f, 0.18f);
                        p.z = randRange(s, 0.03f, 0.12f);

                        p.vx = randRange(s, -0.08f, 0.08f);
                        p.vy = randRange(s, -0.08f, 0.08f);
                        p.vz = randRange(s, 0.18f, 0.55f);
                        p.drag = 0.70f;

                        const int a0 = std::clamp(static_cast<int>(std::round(120.0f * lum)), 22, 140);
                        p.c0 = Color{ 140, 205, 255, static_cast<uint8_t>(a0) };
                        p.c1 = Color{ 45, 70, 100, 0 };

                        p.life = randRange(s, 0.90f, 1.55f);
                        p.size0 = randRange(s, 0.14f, 0.26f);
                        p.size1 = p.size0 * randRange(s, 1.35f, 1.85f);

                        particles_->add(p);

                        // Occasional sparkle on the water surface.
                        if ((s & 0x7Fu) == 0u) {
                            ParticleEngine::Particle sp{};
                            sp.layer = ParticleEngine::LAYER_BEHIND;
                            sp.kind = ParticleEngine::Kind::Mote;
                            sp.var = static_cast<uint8_t>(static_cast<int>(
                                randRange(s, 0.0f, static_cast<float>(ParticleEngine::MOTE_VARS))));
                            sp.seed = s ^ 0xA5A5A5A5u;

                            sp.x = static_cast<float>(x) + 0.50f + randRange(s, -0.14f, 0.14f);
                            sp.y = static_cast<float>(y) + 0.50f + randRange(s, -0.14f, 0.14f);
                            sp.z = randRange(s, 0.05f, 0.14f);

                            sp.vx = randRange(s, -0.04f, 0.04f);
                            sp.vy = randRange(s, -0.04f, 0.04f);
                            sp.vz = randRange(s, 0.08f, 0.22f);
                            sp.drag = 1.25f;

                            const int aS = std::clamp(static_cast<int>(std::round(150.0f * lum)), 30, 190);
                            sp.c0 = Color{ 200, 235, 255, static_cast<uint8_t>(aS) };
                            sp.c1 = Color{ 70, 110, 160, 0 };

                            sp.life = randRange(s, 0.35f, 0.70f);
                            sp.size0 = randRange(s, 0.05f, 0.10f);
                            sp.size1 = sp.size0 * 0.55f;

                            particles_->add(sp);
                        }
                    }
                }
            } else if (tt == TileType::Altar) {
                // Arcane motes: slow drift + twinkle.
                const uint32_t stepMs = 280u;
                const uint32_t dtClamped = std::min<uint32_t>(dtMs, stepMs - 1u);

                const uint32_t phase = hash32(tileSeed ^ 0xA17A1234u) % stepMs;
                const uint32_t now = (ticks + phase) % stepMs;
                const uint32_t prev = (((ticks > dtClamped) ? (ticks - dtClamped) : 0u) + phase) % stepMs;

                if (now < prev) {
                    const uint32_t cycle = (ticks + phase) / stepMs;
                    uint32_t s = hash32(tileSeed ^ 0xA17A1234u ^ (cycle * 0x85EBCA6Bu));

                    // Chance per cycle (~0.35-0.4 spawns/sec per visible altar).
                    if ((s & 0xFFu) < 26u) {
                        ParticleEngine::Particle p{};
                        p.layer = ParticleEngine::LAYER_BEHIND;
                        p.kind = ParticleEngine::Kind::Mote;
                        p.var = static_cast<uint8_t>(static_cast<int>(
                            randRange(s, 0.0f, static_cast<float>(ParticleEngine::MOTE_VARS))));
                        p.seed = s;

                        p.x = static_cast<float>(x) + 0.50f + randRange(s, -0.14f, 0.14f);
                        p.y = static_cast<float>(y) + 0.50f + randRange(s, -0.14f, 0.14f);
                        p.z = randRange(s, 0.06f, 0.26f);

                        p.vx = randRange(s, -0.05f, 0.05f);
                        p.vy = randRange(s, -0.05f, 0.05f);
                        p.vz = randRange(s, 0.10f, 0.42f);
                        p.drag = 1.05f;

                        // Theme-tinted altar glow to match the HUD tone a bit.
                        Color c0{ 220, 170, 255, 180 };
                        Color c1{ 80,  40, 120, 0 };
                        switch (game.uiTheme()) {
                            case UITheme::Parchment:
                                c0 = Color{ 255, 230, 170, 180 };
                                c1 = Color{ 120,  80,  40, 0 };
                                break;
                            case UITheme::Arcane:
                                c0 = Color{ 170, 225, 255, 180 };
                                c1 = Color{  40,  90, 140, 0 };
                                break;
                            case UITheme::DarkStone:
                            default:
                                break;
                        }

                        const int a0 = std::clamp(static_cast<int>(std::round(static_cast<float>(c0.a) * lum)), 26, 210);
                        c0.a = static_cast<uint8_t>(a0);
                        p.c0 = c0;
                        p.c1 = c1;

                        p.life = randRange(s, 0.55f, 1.10f);
                        p.size0 = randRange(s, 0.05f, 0.12f);
                        p.size1 = p.size0 * randRange(s, 0.35f, 0.65f);

                        particles_->add(p);

                        // Rare "spark" burst: one extra mote with a quicker life.
                        if ((s & 0x1FFu) == 0u) {
                            ParticleEngine::Particle p2 = p;
                            p2.seed ^= 0x3C3C3C3Cu;
                            p2.z += randRange(s, 0.03f, 0.10f);
                            p2.vz += randRange(s, 0.12f, 0.26f);
                            p2.life = randRange(s, 0.22f, 0.45f);
                            p2.size0 *= 0.80f;
                            p2.size1 *= 0.70f;
                            p2.c0.a = static_cast<uint8_t>(std::min<int>(255, static_cast<int>(p2.c0.a) + 35));
                            particles_->add(p2);
                        }
                    }
                }
            }
        }
    }

}

void Renderer::updateProceduralAnimations(const Game& game, float frameDt, uint32_t ticks) {
    // Reset between runs/floors so animation state doesn't "leak" across levels.
    const uint32_t runSeed = static_cast<uint32_t>(game.seed());
    if (prevAnimSeed_ != runSeed || prevAnimBranch_ != game.branch() || prevAnimDepth_ != game.depth()) {
        procAnimById_.clear();
        prevAnimSeed_ = runSeed;
        prevAnimBranch_ = game.branch();
        prevAnimDepth_ = game.depth();
    }

    const float dt = std::clamp(frameDt, 0.0f, 0.05f);

    const int playerId = game.playerId();
    const Vec2i playerPos = game.player().pos;

    auto signi = [](int v) -> int { return (v > 0) - (v < 0); };

    std::unordered_set<int> alive;
    alive.reserve(game.entities().size() * 2 + 8);

    for (const Entity& e : game.entities()) {
        alive.insert(e.id);

        ProcAnimState& st = procAnimById_[e.id];
        if (!st.initialized) {
            st.initialized = true;
            st.lastPos = e.pos;
            st.lastHp = e.hp;

            st.moveFrom = e.pos;
            st.moveTo = e.pos;
            st.moveDuration = 0.08f;
            st.moveTime = st.moveDuration;

            st.hurtDir = {0, 0};
            st.hurtDuration = 0.18f;
            st.hurtTime = st.hurtDuration;
            continue;
        }

        // Movement tween.
        if (e.pos.x != st.lastPos.x || e.pos.y != st.lastPos.y) {
            const int dx = e.pos.x - st.lastPos.x;
            const int dy = e.pos.y - st.lastPos.y;
            const bool isStep = (std::abs(dx) <= 1 && std::abs(dy) <= 1);

            if (isStep) {
                st.moveFrom = st.lastPos;
                st.moveTo = e.pos;
                st.moveDuration = (e.id == playerId) ? 0.075f : 0.09f;
                st.moveTime = 0.0f;
            } else {
                // Teleports / long moves: don't tween across the map.
                st.moveFrom = e.pos;
                st.moveTo = e.pos;
                st.moveDuration = 0.0f;
                st.moveTime = 0.0f;
            }

            st.lastPos = e.pos;
        } else {
            st.lastPos = e.pos;
        }

        // Hurt recoil.
        if (e.hp < st.lastHp) {
            st.hurtDuration = 0.18f;
            st.hurtTime = 0.0f;

            Vec2i dir{0, 0};
            if (e.id != playerId) {
                dir.x = signi(e.pos.x - playerPos.x);
                dir.y = signi(e.pos.y - playerPos.y);
            } else {
                // Player recoil: bias opposite the last movement direction if we have it.
                dir.x = -signi(st.moveTo.x - st.moveFrom.x);
                dir.y = -signi(st.moveTo.y - st.moveFrom.y);
            }

            // Fallback: stable pseudo-random direction.
            if (dir.x == 0 && dir.y == 0) {
                const uint32_t h = hash32(hashCombine(runSeed, hashCombine(static_cast<uint32_t>(e.id), ticks)));
                const int r = static_cast<int>(h & 3u);
                if (r == 0) dir.x = 1;
                else if (r == 1) dir.x = -1;
                else if (r == 2) dir.y = 1;
                else dir.y = -1;
            }

            st.hurtDir = dir;
        }

        st.lastHp = e.hp;
    }

    // Advance timers.
    for (auto& kv : procAnimById_) {
        ProcAnimState& st = kv.second;
        if (st.moveDuration > 0.0f && st.moveTime < st.moveDuration) {
            st.moveTime = std::min(st.moveDuration, st.moveTime + dt);
        }
        if (st.hurtDuration > 0.0f && st.hurtTime < st.hurtDuration) {
            st.hurtTime = std::min(st.hurtDuration, st.hurtTime + dt);
        }
    }

    // Cleanup states for entities that no longer exist.
    for (auto it = procAnimById_.begin(); it != procAnimById_.end(); ) {
        if (alive.find(it->first) == alive.end()) it = procAnimById_.erase(it);
        else ++it;
    }
}


SDL_Texture* Renderer::textureFromSprite(const SpritePixels& s) {
    if (!renderer || !pixfmt) return nullptr;

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, s.w, s.h);
    if (!tex) return nullptr;

    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    std::vector<uint32_t> mapped;
    mapped.resize(static_cast<size_t>(s.w * s.h));

    for (int i = 0; i < s.w * s.h; ++i) {
        const Color& c = s.px[static_cast<size_t>(i)];
        mapped[static_cast<size_t>(i)] = SDL_MapRGBA(pixfmt, c.r, c.g, c.b, c.a);
    }

    SDL_UpdateTexture(tex, nullptr, mapped.data(), s.w * static_cast<int>(sizeof(uint32_t)));
    return tex;
}

SDL_Texture* Renderer::tileTexture(TileType t, int x, int y, int level, int frame, int roomStyle) {
    const bool iso = (viewMode_ == ViewMode::Isometric);
    const uint32_t lvl = static_cast<uint32_t>(level);

    switch (t) {
        case TileType::Floor: {
            const int s = std::clamp(roomStyle, 0, ROOM_STYLES - 1);
            const auto& vec = (iso && !floorThemeVarIso[static_cast<size_t>(s)].empty())
                                ? floorThemeVarIso[static_cast<size_t>(s)]
                                : floorThemeVar[static_cast<size_t>(s)];
            if (vec.empty()) return nullptr;

            // Coherent spatial noise keeps large floors from looking like high-frequency static.
            const uint32_t seed = hashCombine(lvl ^ (static_cast<uint32_t>(s) * 0x9E3779B9u), 0xF100CAFEu);
            const size_t idx = pickCoherentVariantIndex(x, y, seed, vec.size());
            return vec[idx][static_cast<size_t>(frame % FRAMES)];
        }
        case TileType::Wall: {
            if (wallVar.empty()) return nullptr;

            const uint32_t seed = hashCombine(lvl ^ 0x511A11u, 0x0A11EDu);
            const size_t idx = pickCoherentVariantIndex(x, y, seed, wallVar.size());
            return wallVar[idx][static_cast<size_t>(frame % FRAMES)];
        }
        case TileType::Chasm: {
            const auto& vec = (iso && !chasmVarIso.empty()) ? chasmVarIso : chasmVar;
            if (vec.empty()) return nullptr;

            const uint32_t seed = hashCombine(lvl ^ 0x000C11A5u, 0x00C4A5Au);
            const size_t idx = pickCoherentVariantIndex(x, y, seed, vec.size());
            return vec[idx][static_cast<size_t>(frame % FRAMES)];
        }
        // Pillars/doors/stairs are rendered as overlays layered on top of the underlying floor.
        // Base tile fetch returns nullptr so the caller doesn't accidentally draw a standalone
        // overlay without its floor.
        case TileType::Pillar:
        case TileType::Boulder:
        case TileType::Fountain:
        case TileType::Altar:
            return nullptr;
        case TileType::DoorSecret: {
            // Draw secret doors as walls until discovered (tile is converted to DoorClosed).
            if (wallVar.empty()) return nullptr;

            const uint32_t seed = hashCombine(lvl ^ 0x511A11u, 0x0A11EDu);
            const size_t idx = pickCoherentVariantIndex(x, y, seed, wallVar.size());
            return wallVar[idx][static_cast<size_t>(frame % FRAMES)];
        }
        case TileType::StairsUp:
        case TileType::StairsDown:
        case TileType::DoorClosed:
        case TileType::DoorLocked:
        case TileType::DoorOpen:
            return nullptr;
        default:
            return nullptr;
    }
}

namespace {

    // Sprite cache categories (packed into the high byte of the cache key).
    constexpr uint8_t CAT_ENTITY = 1;
    constexpr uint8_t CAT_ITEM = 2;
    constexpr uint8_t CAT_PROJECTILE = 3;

    // Key layout (uint64): [cat:8][kind:8][seed:32][flags:16]
    inline uint64_t makeSpriteKey(uint8_t cat, uint8_t kind, uint32_t seed, uint16_t flags = 0) {
        return (static_cast<uint64_t>(cat) << 56) |
               (static_cast<uint64_t>(kind) << 48) |
               (static_cast<uint64_t>(seed) << 16) |
               static_cast<uint64_t>(flags);
    }

    // For NetHack-style identification, identifiable items have randomized
    // *appearances* each run (e.g., "ruby potion", "scroll labeled KLAATU").
    // If we rendered their true item-kind sprites, you'd be able to ID them
    // visually, which undermines the system.
    //
    // To fix this (and to add more procedural art variety), we switch the
    // sprite seed for identifiable items to a stable per-run "appearance seed"
    // and set SPRITE_SEED_IDENT_APPEARANCE_FLAG so spritegen can draw
    // appearance-based art.
    inline uint32_t identAppearanceSpriteSeed(const Game& game, ItemKind k) {
        const uint8_t app = game.itemAppearanceFor(k);

        // Category salt keeps potion/scroll/ring/wand appearance id spaces separate.
        // (These are just arbitrary constants; determinism is all that matters.)
        uint32_t salt = 0x1D3A3u;
        if (isPotionKind(k)) salt = 0xA17C0DE1u;
        else if (isScrollKind(k)) salt = 0x5C2011D5u;
        else if (isRingKind(k)) salt = 0xBADC0FFEu;
        else if (isWandKind(k)) salt = 0xC001D00Du;

        const uint32_t mixed = hash32(hashCombine(game.seed() ^ salt, static_cast<uint32_t>(app)));
        return SPRITE_SEED_IDENT_APPEARANCE_FLAG | (mixed & 0x7FFFFF00u) | static_cast<uint32_t>(app);
    }

    inline void applyIdentificationVisuals(const Game& game, Item& it) {
        if (!game.identificationEnabled()) return;
        if (!isIdentifiableKind(it.kind)) return;
        it.spriteSeed = identAppearanceSpriteSeed(game, it.kind);
    }
}

SDL_Texture* Renderer::entityTexture(const Entity& e, int frame) {
    // In 2D sprite mode (voxel sprites disabled), generate at 256x256 by default
    // to maximize detail, then scale down at render-time using nearest-neighbor.
    // In 3D (voxel) mode we stick to tile-resolution to keep VRAM + gen cost reasonable.
    const int spritePx = voxelSpritesCached ? std::clamp(tile, 16, 256) : 256;
    const uint16_t flags = (voxelSpritesCached && viewMode_ == ViewMode::Isometric)
        ? static_cast<uint16_t>(1u | (isoVoxelRaytraceCached ? 2u : 0u))
        : 0u;
    const uint64_t key = makeSpriteKey(CAT_ENTITY, static_cast<uint8_t>(e.kind), e.spriteSeed, flags);

    auto arr = spriteTex.get(key);
    if (!arr) {
        std::array<SDL_Texture*, FRAMES> tex{};
        tex.fill(nullptr);
        for (int f = 0; f < FRAMES; ++f) {
            tex[static_cast<size_t>(f)] = textureFromSprite(generateEntitySprite(e.kind, e.spriteSeed, f, voxelSpritesCached, spritePx, viewMode_ == ViewMode::Isometric, isoVoxelRaytraceCached));
        }
        const size_t bytes = static_cast<size_t>(spritePx) * static_cast<size_t>(spritePx)
            * sizeof(uint32_t) * static_cast<size_t>(FRAMES);

        spriteTex.put(key, tex, bytes);
        arr = spriteTex.get(key);
        if (!arr) return nullptr;
    }
    return (*arr)[static_cast<size_t>(frame % FRAMES)];
}

SDL_Texture* Renderer::itemTexture(const Item& it, int frame) {
    // In 2D sprite mode (voxel sprites disabled), generate at 256x256 by default
    // to maximize detail, then scale down at render-time using nearest-neighbor.
    // In 3D (voxel) mode we stick to tile-resolution to keep VRAM + gen cost reasonable.
    const int spritePx = voxelSpritesCached ? std::clamp(tile, 16, 256) : 256;
    const uint16_t flags = (voxelSpritesCached && viewMode_ == ViewMode::Isometric)
        ? static_cast<uint16_t>(1u | (isoVoxelRaytraceCached ? 2u : 0u))
        : 0u;
    const uint64_t key = makeSpriteKey(CAT_ITEM, static_cast<uint8_t>(it.kind), it.spriteSeed, flags);

    auto arr = spriteTex.get(key);
    if (!arr) {
        std::array<SDL_Texture*, FRAMES> tex{};
        tex.fill(nullptr);
        for (int f = 0; f < FRAMES; ++f) {
            tex[static_cast<size_t>(f)] = textureFromSprite(generateItemSprite(it.kind, it.spriteSeed, f, voxelSpritesCached, spritePx, viewMode_ == ViewMode::Isometric, isoVoxelRaytraceCached));
        }

        const size_t bytes = static_cast<size_t>(spritePx) * static_cast<size_t>(spritePx)
            * sizeof(uint32_t) * static_cast<size_t>(FRAMES);

        spriteTex.put(key, tex, bytes);
        arr = spriteTex.get(key);
        if (!arr) return nullptr;
    }
    return (*arr)[static_cast<size_t>(frame % FRAMES)];
}

void Renderer::drawItemIcon(const Game& game, const Item& it, int x, int y, int px) {
    if (!renderer) return;

    SDL_BlendMode prevBlend = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &prevBlend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Center within a typical UI row (18px) with a slight vertical inset.
    SDL_Rect dst{ x, y + 1, px, px };

    // Subtle dark backdrop so bright sprites remain readable on any panel theme.
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 55);
    SDL_RenderFillRect(renderer, &dst);

    Item visIt = it;
    if (isHallucinating(game)) {
        visIt.kind = hallucinatedItemKind(game, it);
    }

    applyIdentificationVisuals(game, visIt);

    SDL_Texture* tex = itemTexture(visIt, lastFrame + visIt.id);
    if (tex) {
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
    }

    // Stack count label (tiny) for stackable items.
    if (it.count > 1) {
        const Color white{240, 240, 240, 255};
        const int scale = 1;

        // 16px icons can only comfortably fit 2 digits; clamp larger stacks.
        const int shown = (it.count > 99) ? 99 : it.count;
        const std::string s = std::to_string(shown);

        const int charW = (5 + 1) * scale;
        const int textW = static_cast<int>(s.size()) * charW;
        const int textH = 7 * scale;

        const int tx = dst.x + dst.w - textW;
        const int ty = dst.y + dst.h - textH;

        SDL_Rect bg{ tx - 1, ty - 1, textW + 2, textH + 2 };
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
        SDL_RenderFillRect(renderer, &bg);

        drawText5x7(renderer, tx, ty, scale, white, s);
    }

    SDL_SetRenderDrawBlendMode(renderer, prevBlend);
}

SDL_Texture* Renderer::projectileTexture(ProjectileKind k, int frame) {
    // In 2D sprite mode (voxel sprites disabled), generate at 256x256 by default
    // to maximize detail, then scale down at render-time using nearest-neighbor.
    // In 3D (voxel) mode we stick to tile-resolution to keep VRAM + gen cost reasonable.
    const int spritePx = voxelSpritesCached ? std::clamp(tile, 16, 256) : 256;
    const uint16_t flags = (voxelSpritesCached && viewMode_ == ViewMode::Isometric)
        ? static_cast<uint16_t>(1u | (isoVoxelRaytraceCached ? 2u : 0u))
        : 0u;
    const uint64_t key = makeSpriteKey(CAT_PROJECTILE, static_cast<uint8_t>(k), 0u, flags);

    auto arr = spriteTex.get(key);
    if (!arr) {
        std::array<SDL_Texture*, FRAMES> tex{};
        tex.fill(nullptr);
        for (int f = 0; f < FRAMES; ++f) {
            tex[static_cast<size_t>(f)] = textureFromSprite(generateProjectileSprite(k, 0u, f, voxelSpritesCached, spritePx, viewMode_ == ViewMode::Isometric, isoVoxelRaytraceCached));
        }

        const size_t bytes = static_cast<size_t>(spritePx) * static_cast<size_t>(spritePx)
            * sizeof(uint32_t) * static_cast<size_t>(FRAMES);

        spriteTex.put(key, tex, bytes);
        arr = spriteTex.get(key);
        if (!arr) return nullptr;
    }
    return (*arr)[static_cast<size_t>(frame % FRAMES)];
}

void Renderer::ensureUIAssets(const Game& game) {
    if (!initialized) return;

    const UITheme want = game.uiTheme();
    // Procedural GUI: let the UI tile textures pick up a subtle per-run
    // "paint job" derived from the run seed (purely cosmetic, deterministic).
    const uint32_t runSeed = game.seed();
    const uint32_t styleSeed = (runSeed != 0u) ? (hash32(runSeed ^ 0xA11C0DEu) | 1u) : 0u;

    if (uiAssetsValid && want == uiThemeCached && styleSeed == uiStyleSeedCached_) return;

    for (auto& t : uiPanelTileTex) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }
    for (auto& t : uiOrnamentTex) {
        if (t) SDL_DestroyTexture(t);
        t = nullptr;
    }

    uiThemeCached = want;
    uiStyleSeedCached_ = styleSeed;

    const uint32_t tileSeed = (styleSeed != 0u) ? hashCombine(styleSeed, 0x51A11u) : 0x51A11u;
    const uint32_t ornSeed  = (styleSeed != 0u) ? hashCombine(styleSeed, 0x0ABCDu) : 0x0ABCDu;

    for (int f = 0; f < FRAMES; ++f) {
        uiPanelTileTex[static_cast<size_t>(f)] = textureFromSprite(generateUIPanelTile(uiThemeCached, tileSeed, f, 16));
        uiOrnamentTex[static_cast<size_t>(f)]  = textureFromSprite(generateUIOrnamentTile(uiThemeCached, ornSeed, f, 16));
    }

    uiAssetsValid = true;
}

void Renderer::ensureIsoTerrainAssets(uint32_t styleSeed, bool voxelBlocks, bool isoRaytrace) {
    if (!renderer || !pixfmt) return;

    // Tile textures are generated in a clamped "sprite" resolution to keep VRAM reasonable
    // for very large tile sizes. This matches the logic in Renderer::init().
    const int spritePx = std::max(16, std::min(256, tile));
    const int tileVars = (spritePx >= 224) ? 8 : (spritePx >= 160) ? 10 : (spritePx >= 96) ? 14 : 18;
    const bool useRaytraceBlocks = voxelBlocks && isoRaytrace && spritePx <= 64;
    const int blockVars = (useRaytraceBlocks) ? std::min(tileVars, 10) : tileVars;
    if (isoTerrainAssetsValid
        && isoTerrainStyleSeedCached_ == styleSeed
        && isoTerrainSpritePxCached_ == spritePx
        && isoTerrainVoxelBlocksCached_ == voxelBlocks
        && isoTerrainVoxelBlocksRaytraceCached_ == useRaytraceBlocks) return;

    // Defensive cleanup in case we ever re-generate (e.g., future runtime tile-size changes).
    for (auto& styleVec : floorThemeVarIso) {
        for (auto& arr : styleVec) {
            for (SDL_Texture*& t : arr) {
                if (t) SDL_DestroyTexture(t);
                t = nullptr;
            }
        }
        styleVec.clear();
    }
    for (auto& arr : chasmVarIso) {
        for (SDL_Texture*& t : arr) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
    chasmVarIso.clear();
    for (auto& arr : wallBlockVarIso) {
        for (SDL_Texture*& t : arr) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
    wallBlockVarIso.clear();

    for (auto& arr : doorBlockClosedVarIso) {
        for (SDL_Texture*& t : arr) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
    doorBlockClosedVarIso.clear();

    for (auto& arr : doorBlockLockedVarIso) {
        for (SDL_Texture*& t : arr) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
    doorBlockLockedVarIso.clear();

    for (auto& arr : doorBlockOpenVarIso) {
        for (SDL_Texture*& t : arr) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
    doorBlockOpenVarIso.clear();

    for (auto& arr : pillarBlockVarIso) {
        for (SDL_Texture*& t : arr) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
    pillarBlockVarIso.clear();

    for (auto& arr : boulderBlockVarIso) {
        for (SDL_Texture*& t : arr) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }
    boulderBlockVarIso.clear();

    for (auto& anim : isoEdgeShadeVar) {
        for (SDL_Texture*& t : anim) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }

    for (auto& anim : isoChasmGloomVar) {
        for (SDL_Texture*& t : anim) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }

    for (auto& anim : isoCastShadowVar) {
        for (SDL_Texture*& t : anim) {
            if (t) SDL_DestroyTexture(t);
            t = nullptr;
        }
    }

    for (auto& t : stairsUpOverlayIsoTex) { if (t) SDL_DestroyTexture(t); t = nullptr; }
    for (auto& t : stairsDownOverlayIsoTex) { if (t) SDL_DestroyTexture(t); t = nullptr; }
    for (auto& t : doorOpenOverlayIsoTex) { if (t) SDL_DestroyTexture(t); t = nullptr; }

    for (auto& t : isoEntityShadowTex) { if (t) SDL_DestroyTexture(t); t = nullptr; }

    for (auto& anim : gasVarIso) {
        for (SDL_Texture*& t : anim) { if (t) SDL_DestroyTexture(t); t = nullptr; }
    }
    for (auto& anim : fireVarIso) {
        for (SDL_Texture*& t : anim) { if (t) SDL_DestroyTexture(t); t = nullptr; }
    }

    // Isometric floor decals (diamond-projected overlays)
    for (auto& arr : floorDecalVarIso) {
        for (SDL_Texture*& t : arr) { if (t) SDL_DestroyTexture(t); t = nullptr; }
    }
    floorDecalVarIso.clear();

    auto mixSeed = [&](uint32_t base) -> uint32_t {
        return (styleSeed != 0u) ? hashCombine(styleSeed, base) : base;
    };

    // --- Build isometric terrain ---
    // Floors are generated directly as 2:1 diamond tiles in diamond space (no projection).
    for (int st = 0; st < ROOM_STYLES; ++st) {
        auto& vec = floorThemeVarIso[static_cast<size_t>(st)];
        vec.resize(static_cast<size_t>(tileVars));
        for (int i = 0; i < tileVars; ++i) {
            for (int f = 0; f < FRAMES; ++f) {
                const uint32_t seed = hashCombine(mixSeed(0xC011Du ^ (static_cast<uint32_t>(st) * 0x9E3779B9u)), static_cast<uint32_t>(i * 1000 + f * 17));
                const SpritePixels iso = generateIsometricThemedFloorTile(seed, static_cast<uint8_t>(st), f, spritePx);
                vec[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(iso);
            }
        }
    }

    chasmVarIso.resize(static_cast<size_t>(tileVars));
    for (int i = 0; i < tileVars; ++i) {
        const uint32_t seed = hashCombine(mixSeed(0xC1A500u), static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            const SpritePixels iso = generateIsometricChasmTile(seed, f, spritePx);
            chasmVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(iso);
        }
    }

    // 2.5D walls are drawn as sprites (square textures) so they can extend above the ground plane.
    wallBlockVarIso.resize(static_cast<size_t>(blockVars));
    for (int i = 0; i < blockVars; ++i) {
        const uint32_t seed = hashCombine(mixSeed(0xAA110u ^ 0xB10Cu), static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            SpritePixels sp;
            if (voxelBlocks) {
                sp = renderIsoTerrainBlockVoxel(IsoTerrainBlockKind::Wall, seed, f, spritePx, useRaytraceBlocks);
            } else {
                sp = generateIsometricWallBlockTile(seed, f, spritePx);
            }
            wallBlockVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(sp);
        }
    }

    // 2.5D doors are drawn as sprites too, so closed/locked doors read as part of
    // the wall geometry instead of flat top-down overlays.
    doorBlockClosedVarIso.resize(static_cast<size_t>(blockVars));
    doorBlockLockedVarIso.resize(static_cast<size_t>(blockVars));
    doorBlockOpenVarIso.resize(static_cast<size_t>(blockVars));
    for (int i = 0; i < blockVars; ++i) {
        const uint32_t baseSeed = hashCombine(mixSeed(0xD00Du ^ 0xB10Cu), static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            SpritePixels closed;
            SpritePixels locked;
            SpritePixels open;

            if (voxelBlocks) {
                closed = renderIsoTerrainBlockVoxel(IsoTerrainBlockKind::DoorClosed, baseSeed ^ 0xC105EDu, f, spritePx, useRaytraceBlocks);
                locked = renderIsoTerrainBlockVoxel(IsoTerrainBlockKind::DoorLocked, baseSeed ^ 0x10CCEDu, f, spritePx, useRaytraceBlocks);
                open   = renderIsoTerrainBlockVoxel(IsoTerrainBlockKind::DoorOpen,   baseSeed ^ 0x0B0A1u, f, spritePx, useRaytraceBlocks);
            } else {
                closed = generateIsometricDoorBlockTile(baseSeed ^ 0xC105EDu, /*locked=*/false, f, spritePx);
                locked = generateIsometricDoorBlockTile(baseSeed ^ 0x10CCEDu, /*locked=*/true, f, spritePx);
                open   = generateIsometricDoorwayBlockTile(baseSeed ^ 0x0B0A1u, f, spritePx);
            }

            doorBlockClosedVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(closed);
            doorBlockLockedVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(locked);
            doorBlockOpenVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(open);
        }
    }


    // 2.5D pillars/boulders are also drawn as sprites in isometric view so props read as
    // volumetric blockers instead of flat top-down overlays.
    pillarBlockVarIso.resize(static_cast<size_t>(blockVars));
    boulderBlockVarIso.resize(static_cast<size_t>(blockVars));
    for (int i = 0; i < blockVars; ++i) {
        const uint32_t pSeed = hashCombine(mixSeed(0x9111A0u ^ 0xB10Cu), static_cast<uint32_t>(i));
        const uint32_t bSeed = hashCombine(mixSeed(0xB011D3u ^ 0xB10Cu), static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            SpritePixels psp;
            SpritePixels bsp;

            if (voxelBlocks) {
                psp = renderIsoTerrainBlockVoxel(IsoTerrainBlockKind::Pillar, pSeed, f, spritePx, useRaytraceBlocks);
                bsp = renderIsoTerrainBlockVoxel(IsoTerrainBlockKind::Boulder, bSeed, f, spritePx, useRaytraceBlocks);
            } else {
                psp = generateIsometricPillarBlockTile(pSeed, f, spritePx);
                bsp = generateIsometricBoulderBlockTile(bSeed, f, spritePx);
            }

            pillarBlockVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(psp);
            boulderBlockVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(bsp);
        }
    }

    // Isometric edge shading overlays (contact shadows / chasm rims).
    // These are transparent diamond tiles keyed by the same 4-neighbor mask encoding as the top-down autotile overlays.
    for (int m = 0; m < AUTO_MASKS; ++m) {
        for (int f = 0; f < FRAMES; ++f) {
            if (m == 0) {
                isoEdgeShadeVar[static_cast<size_t>(m)][static_cast<size_t>(f)] = nullptr;
                continue;
            }
            const uint32_t seed = hashCombine(mixSeed(0x150A0u), static_cast<uint32_t>(m * 131 + f * 17));
            isoEdgeShadeVar[static_cast<size_t>(m)][static_cast<size_t>(f)] =
                textureFromSprite(generateIsometricEdgeShadeOverlay(seed, static_cast<uint8_t>(m), f, spritePx));
        }
    }

    // Isometric chasm gloom overlays (deeper pit adjacency darkening).
    // These extend farther inward than the rim band so tiles beside chasms read as
    // slightly darker "drop-off" zones in 2.5D view.
    for (int m = 0; m < AUTO_MASKS; ++m) {
        for (int f = 0; f < FRAMES; ++f) {
            if (m == 0) {
                isoChasmGloomVar[static_cast<size_t>(m)][static_cast<size_t>(f)] = nullptr;
                continue;
            }
            const uint32_t seed = hashCombine(mixSeed(0xC11A500u), static_cast<uint32_t>(m * 97 + f * 19));
            isoChasmGloomVar[static_cast<size_t>(m)][static_cast<size_t>(f)] =
                textureFromSprite(generateIsometricChasmGloomOverlay(seed, static_cast<uint8_t>(m), f, spritePx));
        }
    }

    // Isometric cast shadow overlays (soft directional shadows from tall occluders).
    // These are transparent diamond tiles keyed by the same 4-neighbor mask encoding as other overlays.
    for (int m = 0; m < AUTO_MASKS; ++m) {
        for (int f = 0; f < FRAMES; ++f) {
            if (m == 0) {
                isoCastShadowVar[static_cast<size_t>(m)][static_cast<size_t>(f)] = nullptr;
                continue;
            }
            const uint32_t seed = hashCombine(mixSeed(0xCA570u), static_cast<uint32_t>(m * 97 + f * 19));
            isoCastShadowVar[static_cast<size_t>(m)][static_cast<size_t>(f)] =
                textureFromSprite(generateIsometricCastShadowOverlay(seed, static_cast<uint8_t>(m), f, spritePx));
        }
    }

    const uint8_t isoLightDir = isoLightDirFromStyleSeed(styleSeed);
    // Ground-plane overlays that should sit on the diamond.
    // Isometric entity ground shadows (diamond overlays) used under sprites.
    for (int f = 0; f < FRAMES; ++f) {
        const uint32_t seed = mixSeed(0x5AD0F00u);
        isoEntityShadowTex[static_cast<size_t>(f)] =
            textureFromSprite(generateIsometricEntityShadowOverlay(seed, isoLightDir, f, spritePx));
    }

    for (int f = 0; f < FRAMES; ++f) {
        {
            const uint32_t seed = mixSeed(0x515A1u);
            // Purpose-built isometric stairs overlay (diamond space) for better depth/readability.
            const SpritePixels iso = generateIsometricStairsOverlay(seed, /*up=*/true, f, spritePx);
            stairsUpOverlayIsoTex[static_cast<size_t>(f)] = textureFromSprite(iso);
        }
        {
            const uint32_t seed = mixSeed(0x515A2u);
            const SpritePixels iso = generateIsometricStairsOverlay(seed, /*up=*/false, f, spritePx);
            stairsDownOverlayIsoTex[static_cast<size_t>(f)] = textureFromSprite(iso);
        }
        {
            const uint32_t seed = mixSeed(0xD00Du);
            const SpritePixels sq = generateDoorTile(seed, true, f, spritePx);
            const SpritePixels iso = projectToIsometricDiamond(sq, seed, f, /*outline=*/false);
            doorOpenOverlayIsoTex[static_cast<size_t>(f)] = textureFromSprite(iso);
        }
    }

    // Isometric floor decals: generate decals directly in diamond space (instead of projecting
    // top-down squares) so thin lines stay crisp and patterns align better with the 2.5D grid.
    floorDecalVarIso.clear();
    floorDecalVarIso.resize(static_cast<size_t>(DECAL_STYLES * static_cast<size_t>(decalsPerStyleUsed)));
    for (int st = 0; st < DECAL_STYLES; ++st) {
        for (int i = 0; i < decalsPerStyleUsed; ++i) {
            const uint32_t fSeed = hashCombine(mixSeed(0xD3CA10u + static_cast<uint32_t>(st) * 131u), static_cast<uint32_t>(i));
            const size_t idx = static_cast<size_t>(st * decalsPerStyleUsed + i);
            for (int f = 0; f < FRAMES; ++f) {
                const SpritePixels iso = generateIsometricFloorDecalOverlay(fSeed, static_cast<uint8_t>(st), f, spritePx);
                floorDecalVarIso[idx][static_cast<size_t>(f)] = textureFromSprite(iso);
            }
        }
    }

    // Isometric environmental overlays (gas/fire) so effects follow the diamond grid.
    for (int i = 0; i < GAS_VARS; ++i) {
        const uint32_t gSeed = hashCombine(mixSeed(0x6A5u), static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            const SpritePixels iso = generateIsometricGasTile(gSeed, f, spritePx);
            gasVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(iso);
        }
    }
    for (int i = 0; i < FIRE_VARS; ++i) {
        const uint32_t fSeed = hashCombine(mixSeed(0xF17Eu), static_cast<uint32_t>(i));
        for (int f = 0; f < FRAMES; ++f) {
            const SpritePixels iso = generateIsometricFireTile(fSeed, f, spritePx);
            fireVarIso[static_cast<size_t>(i)][static_cast<size_t>(f)] = textureFromSprite(iso);
        }
    }

    isoTerrainStyleSeedCached_ = styleSeed;
    isoTerrainSpritePxCached_ = spritePx;
    isoTerrainVoxelBlocksCached_ = voxelBlocks;
    isoTerrainVoxelBlocksRaytraceCached_ = useRaytraceBlocks;
    isoTerrainAssetsValid = true;
}

static Color uiBorderForTheme(UITheme theme) {
    switch (theme) {
        case UITheme::DarkStone: return {180, 200, 235, 255};
        case UITheme::Parchment: return {235, 215, 160, 255};
        case UITheme::Arcane:    return {230, 170, 255, 255};
    }
    return {200, 200, 200, 255};
}

void Renderer::drawPanel(const Game& game, const SDL_Rect& rect, uint8_t alpha, int frame) {
    if (!renderer) return;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Drop shadow (subtle)
    SDL_Rect shadow{ rect.x + 2, rect.y + 2, rect.w, rect.h };
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, static_cast<Uint8>(std::min<int>(alpha, 200) / 2));
    SDL_RenderFillRect(renderer, &shadow);

    if (game.uiPanelsTextured()) {
        ensureUIAssets(game);

        SDL_Texture* tileTex = uiPanelTileTex[static_cast<size_t>(frame % FRAMES)];
        if (tileTex) {
            Uint8 oldA = 255;
            SDL_GetTextureAlphaMod(tileTex, &oldA);
            SDL_SetTextureAlphaMod(tileTex, alpha);

            SDL_RenderSetClipRect(renderer, &rect);
            const int step = 16;
            for (int y = rect.y; y < rect.y + rect.h; y += step) {
                for (int x = rect.x; x < rect.x + rect.w; x += step) {
                    SDL_Rect dst{ x, y, step, step };
                    SDL_RenderCopy(renderer, tileTex, nullptr, &dst);
                }
            }
            SDL_RenderSetClipRect(renderer, nullptr);

            SDL_SetTextureAlphaMod(tileTex, oldA);
        } else {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
            SDL_RenderFillRect(renderer, &rect);
        }
    } else {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, alpha);
        SDL_RenderFillRect(renderer, &rect);
    }

    const Color border = uiBorderForTheme(game.uiTheme());
    SDL_SetRenderDrawColor(renderer, border.r, border.g, border.b, static_cast<Uint8>(std::min<int>(alpha + 40, 255)));
    SDL_RenderDrawRect(renderer, &rect);

    if (game.uiPanelsTextured()) {
        SDL_Texture* orn = uiOrnamentTex[static_cast<size_t>(frame % FRAMES)];
        if (orn) {
            Uint8 oldA = 255;
            SDL_GetTextureAlphaMod(orn, &oldA);
            SDL_SetTextureAlphaMod(orn, static_cast<Uint8>(std::min<int>(alpha, 220)));

            const int os = 16;
            SDL_Rect dstTL{ rect.x, rect.y, os, os };
            SDL_RenderCopyEx(renderer, orn, nullptr, &dstTL, 0.0, nullptr, SDL_FLIP_NONE);

            SDL_Rect dstTR{ rect.x + rect.w - os, rect.y, os, os };
            SDL_RenderCopyEx(renderer, orn, nullptr, &dstTR, 0.0, nullptr, SDL_FLIP_HORIZONTAL);

            SDL_Rect dstBL{ rect.x, rect.y + rect.h - os, os, os };
            SDL_RenderCopyEx(renderer, orn, nullptr, &dstBL, 0.0, nullptr, SDL_FLIP_VERTICAL);

            SDL_Rect dstBR{ rect.x + rect.w - os, rect.y + rect.h - os, os, os };
            SDL_RenderCopyEx(renderer, orn, nullptr, &dstBR, 0.0, nullptr,
                static_cast<SDL_RendererFlip>(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL));

            SDL_SetTextureAlphaMod(orn, oldA);
        }
    }
}

// Map sprite helper: draws an optional soft shadow + crisp outline, then the sprite.
// This is a cheap way to dramatically improve sprite readability on noisy tiles.
static void drawSpriteWithShadowOutline(SDL_Renderer* r,
                                        SDL_Texture* tex,
                                        const SDL_Rect& dst,
                                        Color mod,
                                        Uint8 alpha,
                                        bool shadow,
                                        bool outline) {
    if (!r || !tex) return;

    // Scale the outline/shadow strength based on how bright the tile lighting is.
    const int lum = (static_cast<int>(mod.r) + static_cast<int>(mod.g) + static_cast<int>(mod.b)) / 3;
    const Uint8 outA = static_cast<Uint8>(std::clamp((lum * 170) / 255, 40, 190));
    const Uint8 shA  = static_cast<Uint8>(std::clamp((lum * 120) / 255, 28, 150));

    auto renderPass = [&](int dx, int dy, Uint8 rMod, Uint8 gMod, Uint8 bMod, Uint8 aMod) {
        SDL_Rect d = dst;
        d.x += dx;
        d.y += dy;
        SDL_SetTextureColorMod(tex, rMod, gMod, bMod);
        SDL_SetTextureAlphaMod(tex, aMod);
        SDL_RenderCopy(r, tex, nullptr, &d);
    };

    // Shadow first (offset down-right).
    if (shadow && shA > 0) {
        renderPass(2, 2, 0, 0, 0, shA);
    }

    // 4-neighbor outline (1px).
    if (outline && outA > 0) {
        renderPass(-1, 0, 0, 0, 0, outA);
        renderPass( 1, 0, 0, 0, 0, outA);
        renderPass( 0,-1, 0, 0, 0, outA);
        renderPass( 0, 1, 0, 0, 0, outA);
    }

    // Main sprite.
    SDL_SetTextureColorMod(tex, mod.r, mod.g, mod.b);
    SDL_SetTextureAlphaMod(tex, alpha);
    SDL_RenderCopy(r, tex, nullptr, &dst);

    SDL_SetTextureColorMod(tex, 255, 255, 255);
    SDL_SetTextureAlphaMod(tex, 255);
}

// Simple post-process: a gentle vignette that improves focus and mood while
// keeping the HUD crisp (it's applied only to the map region).
static void drawVignette(SDL_Renderer* r, const SDL_Rect& area, int thickness, int maxAlpha) {
    if (!r) return;
    thickness = std::clamp(thickness, 6, 64);
    maxAlpha = std::clamp(maxAlpha, 0, 200);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < thickness; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(std::max(1, thickness - 1));
        // Quadratic falloff: lighter near center, heavier at edges.
        const int a = static_cast<int>(std::round(static_cast<float>(maxAlpha) * (t * t)));
        SDL_SetRenderDrawColor(r, 0, 0, 0, static_cast<uint8_t>(std::clamp(a, 0, 255)));

        SDL_Rect top{ area.x, area.y + i, area.w, 1 };
        SDL_Rect bot{ area.x, area.y + area.h - 1 - i, area.w, 1 };
        SDL_Rect left{ area.x + i, area.y, 1, area.h };
        SDL_Rect right{ area.x + area.w - 1 - i, area.y, 1, area.h };
        SDL_RenderFillRect(r, &top);
        SDL_RenderFillRect(r, &bot);
        SDL_RenderFillRect(r, &left);
        SDL_RenderFillRect(r, &right);
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}

void Renderer::render(const Game& game) {
    if (!initialized) return;

    // Frame timing (for the optional perf overlay).
    if (perfFreq_ == 0) perfFreq_ = SDL_GetPerformanceFrequency();
    const Uint64 nowCounter = SDL_GetPerformanceCounter();
    float frameDt = 0.0f;
    if (perfPrevCounter_ != 0 && perfFreq_ != 0) {
        const double dt = static_cast<double>(nowCounter - perfPrevCounter_) / static_cast<double>(perfFreq_);
        // Clamp extreme pauses (debugger break / alt-tab) so EMA doesn't explode.
        frameDt = static_cast<float>(std::clamp(dt, 0.0, 0.5));
    }
    perfPrevCounter_ = nowCounter;

    if (frameDt > 0.0f) {
        const float instFps = 1.0f / frameDt;
        const float instMs = frameDt * 1000.0f;
        const float a = 0.08f; // EMA alpha (small = smoother)
        if (perfFpsEMA_ <= 0.0f) perfFpsEMA_ = instFps;
        else perfFpsEMA_ = perfFpsEMA_ * (1.0f - a) + instFps * a;
        if (perfMsEMA_ <= 0.0f) perfMsEMA_ = instMs;
        else perfMsEMA_ = perfMsEMA_ * (1.0f - a) + instMs * a;

        perfUpdateTimer_ += frameDt;
    }

    if (game.perfOverlayEnabled() && perfUpdateTimer_ >= 0.25f) {
        perfUpdateTimer_ = 0.0f;
        // Cache formatted text at a low rate to reduce per-frame string churn.
        const size_t usedB = spriteTex.usedBytes();
        const size_t usedMB = usedB / (1024u * 1024u);
        const size_t budgetMB = (textureCacheMB <= 0) ? 0u : static_cast<size_t>(textureCacheMB);

        std::ostringstream l1;
        l1.setf(std::ios::fixed); l1.precision(1);
        l1 << "FPS " << perfFpsEMA_ << "  " << perfMsEMA_ << "ms";
        perfLine1_ = l1.str();

        std::ostringstream l2;
        l2 << "SPRITES " << spriteTex.size() << "  VRAM " << usedMB;
        if (budgetMB > 0) l2 << "/" << budgetMB;
        l2 << "MB  H/M " << spriteTex.hits() << "/" << spriteTex.misses() << "  E " << spriteTex.evictions();
        perfLine2_ = l2.str();

        std::ostringstream l3;
        l3 << "TURN " << game.turns() << "  SEED " << game.seed();
        // Determinism hash is potentially expensive; compute it only at this low rate.
        const uint64_t h = game.determinismHash();
        l3 << "  HASH " << std::hex << std::uppercase << (h & 0xFFFFFFFFull);
        perfLine3_ = l3.str();
    }

    // Keep renderer-side view mode synced (main also calls setViewMode each frame).
    viewMode_ = game.viewMode();

    const uint32_t ticks = SDL_GetTicks();
    const int frame = static_cast<int>((ticks / 220u) % FRAMES);
    lastFrame = frame;

    // ---------------------------------------------------------------------
    // Procedural animation sampling
    //
    // The sprite generator (spritegen) produces discrete animation frames for
    // many tiles/VFX. Here we sample those frames with per-instance phase
    // offsets so the whole world doesn't blink in lockstep, and we expose a
    // sub-frame blend factor for smoother "custom" animations (used for gas/fire).
    // ---------------------------------------------------------------------
    struct FrameBlend {
        int f0 = 0;
        int f1 = 0;
        uint8_t w1 = 0; // 0..255 weight toward f1 (w0 = 255 - w1)
    };

    auto sampleFrameBlend = [&](uint32_t stepMs, uint32_t phaseSeed) -> FrameBlend {
        if (stepMs == 0u) stepMs = 1u;
        const uint32_t cycleMs = stepMs * static_cast<uint32_t>(FRAMES);
        const uint32_t phase = (cycleMs > 0u) ? (hash32(phaseSeed) % cycleMs) : 0u;
        const uint32_t t = ticks + phase;

        const uint32_t idx = t / stepMs;
        const uint32_t rem = t - idx * stepMs;

        FrameBlend fb;
        fb.f0 = static_cast<int>(idx % static_cast<uint32_t>(FRAMES));
        fb.f1 = (fb.f0 + 1) % FRAMES;

        const float frac = static_cast<float>(rem) / static_cast<float>(stepMs);
        int w = static_cast<int>(std::lround(frac * 255.0f));
        w = std::clamp(w, 0, 255);
        fb.w1 = static_cast<uint8_t>(w);
        return fb;
    };


    // If the user toggled 3D voxel sprites, invalidate cached textures so they regenerate.
    const bool wantVoxelSprites = game.voxelSpritesEnabled();
    if (wantVoxelSprites != voxelSpritesCached) {
        // Entity/item/projectile textures are budget-cached in spriteTex.
        spriteTex.clear();
        spriteTex.resetStats();
        uiPreviewTex.clear();
        uiPreviewTex.resetStats();
        voxelSpritesCached = wantVoxelSprites;
    }

    // If the user toggled the isometric voxel raytracer mode, invalidate cached textures so they regenerate.
    const bool wantIsoRaytrace = game.isoVoxelRaytraceEnabled();
    if (wantIsoRaytrace != isoVoxelRaytraceCached) {
        spriteTex.clear();
        spriteTex.resetStats();
        uiPreviewTex.clear();
        uiPreviewTex.resetStats();
        isoVoxelRaytraceCached = wantIsoRaytrace;
    }

    // Background clear
    SDL_SetRenderDrawColor(renderer, 8, 8, 12, 255);
    SDL_RenderClear(renderer);

    const Dungeon& d = game.dungeon();

    // Update camera based on player/cursor and current viewport.
    updateCamera(game);

    // Clip all map-space drawing to the map region so that screen shake / FX never
    // bleed into the HUD area.
    const SDL_Rect mapClip{ 0, 0, viewTilesW * tile, viewTilesH * tile };
    SDL_RenderSetClipRect(renderer, &mapClip);

    // Transient screen shake based on active explosions.
    // (Small and deterministic to avoid nausea and keep capture/replay stable.)
    mapOffX = 0;
    mapOffY = 0;
    {
        int shake = 0;
        for (const auto& ex : game.fxExplosions()) {
            if (ex.delay > 0.0f) continue;
            const float dur = std::max(0.001f, ex.duration);
            const float t01 = std::clamp(ex.timer / dur, 0.0f, 1.0f);
            // Strong at the start, quickly decays.
            const int s = static_cast<int>(std::round((1.0f - t01) * 5.0f));
            if (s > shake) shake = s;
        }

        shake = std::clamp(shake, 0, 6);
        if (shake > 0) {
            const uint32_t seed = hashCombine(static_cast<uint32_t>(ticks), static_cast<uint32_t>(game.turns()));
            const uint32_t rx = hash32(seed ^ 0xA53u);
            const uint32_t ry = hash32(seed ^ 0xC11u);
            mapOffX = static_cast<int>(rx % static_cast<uint32_t>(shake * 2 + 1)) - shake;
            mapOffY = static_cast<int>(ry % static_cast<uint32_t>(shake * 2 + 1)) - shake;
        }
    }

    const bool isoView = (viewMode_ == ViewMode::Isometric);

    // Isometric cutaway focus: used to fade foreground occluders near the player/cursor.
    Vec2i isoCutawayFocus = game.player().pos;
    if (isoView) {
        if (game.isTargeting()) isoCutawayFocus = game.targetingCursor();
        else if (game.isLooking()) isoCutawayFocus = game.lookCursor();
    }
    if (d.width > 0 && d.height > 0) {
        isoCutawayFocus.x = std::clamp(isoCutawayFocus.x, 0, d.width - 1);
        isoCutawayFocus.y = std::clamp(isoCutawayFocus.y, 0, d.height - 1);
    }
    const int isoFocusSum = isoCutawayFocus.x + isoCutawayFocus.y;
    const int isoFocusDiff = isoCutawayFocus.x - isoCutawayFocus.y;
    const bool isoCutawayOn = isoView && game.isoCutawayEnabled();

    // Visual style seed: purely cosmetic per-run "paint job" derived from the game seed.
    // This keeps gameplay determinism untouched, while letting each run look distinct.
    const uint32_t runSeed = game.seed();
    const uint32_t styleSeed = (runSeed != 0u) ? (hash32(runSeed ^ 0xA11C0DEu) | 1u) : 0u;
    const uint8_t isoLightDir = isoLightDirFromStyleSeed(styleSeed);

    // Encode branch + depth into a per-level key for procedural terrain variation.
    // Then mix in the run styleSeed so the same depth can have a different look across runs.
    const int levelKeyBase = (game.branch() == DungeonBranch::Main)
        ? game.depth()
        : ((static_cast<int>(game.branch()) + 1) * 1000 + game.depth());

    const uint32_t lvlSeed = (styleSeed != 0u)
        ? hashCombine(static_cast<uint32_t>(levelKeyBase), styleSeed)
        : static_cast<uint32_t>(levelKeyBase);

    const int levelKey = static_cast<int>(lvlSeed);

// Precompute deterministic terrain materials for this dungeon (used for tinting + LOOK adjectives).
d.ensureMaterials(runSeed, game.branch(), game.depth(), game.dungeonMaxDepth());


    // Build isometric-diamond terrain textures lazily so top-down mode doesn't pay
    // the VRAM + CPU cost unless it is actually used.
    if (isoView) {
        ensureIsoTerrainAssets(styleSeed, game.isoTerrainVoxelBlocksEnabled(), game.isoVoxelRaytraceEnabled());
    }

    // Update procedural particles and emit new ones from current game FX.
    if (particles_) {
        // Per-level wind is deterministic (seed + branch + depth). We feed it
        // into the particle engine as a small global drift so smoke/embers feel
        // like they're in the same "air" as the gas/fire simulation.
        Vec2f windAccel{0.0f, 0.0f};
        {
            const Vec2i w = game.windDir();
            const int ws = game.windStrength();
            if (ws > 0 && (w.x != 0 || w.y != 0)) {
                const float a = 0.12f * static_cast<float>(ws); // tiles/sec^2
                windAccel.x = static_cast<float>(w.x) * a;
                windAccel.y = static_cast<float>(w.y) * a;
            }
        }

        particles_->update(static_cast<float>(frameDt), windAccel);
        updateParticlesFromGame(game, static_cast<float>(frameDt), ticks);
    }

    // Visual-only procedural animation state (smooth movement / bobbing / recoil).
    updateProceduralAnimations(game, static_cast<float>(frameDt), ticks);

    ParticleView particleView;
    if (particles_) {
        particleView.mode = viewMode_;
        particleView.winW = winW;
        particleView.winH = winH;
        particleView.hudH = hudH;
        particleView.tile = tile;
        particleView.camX = camX;
        particleView.camY = camY;
        particleView.isoCamX = isoCamX;
        particleView.isoCamY = isoCamY;
        particleView.mapOffX = mapOffX;
        particleView.mapOffY = mapOffY;
    }

    auto tileDst = [&](int x, int y) -> SDL_Rect {
        return mapTileDst(x, y);
    };

    auto spriteDst = [&](int x, int y) -> SDL_Rect {
        return mapSpriteDst(x, y);
    };

    // ---------------------------------------------------------------------
    // Procedural sprite animation helpers (visual-only).
    // ---------------------------------------------------------------------
    auto smooth01 = [](float t) -> float {
        t = std::clamp(t, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    auto lerpF = [](float a, float b, float t) -> float { return a + (b - a) * t; };
    auto lerpI = [&](int a, int b, float t) -> int {
        return static_cast<int>(std::lround(lerpF(static_cast<float>(a), static_cast<float>(b), t)));
    };

    struct AnimSample {
        SDL_Rect dst{};
        int footX = 0;
        int footY = 0;
        float lift01 = 0.0f;
    };

    auto sampleEntityAnim = [&](const Entity& e) -> AnimSample {
        AnimSample out;
        SDL_Rect baseNow = spriteDst(e.pos.x, e.pos.y);
        out.dst = baseNow;
        out.footX = baseNow.x + baseNow.w / 2;
        out.footY = baseNow.y + baseNow.h;
        out.lift01 = 0.0f;

        bool moving = false;

        auto it = procAnimById_.find(e.id);
        if (it != procAnimById_.end()) {
            const ProcAnimState& st = it->second;
            if (st.moveDuration > 0.0f && st.moveTime < st.moveDuration) {
                moving = true;
                const float t01 = smooth01(st.moveTime / st.moveDuration);

                const SDL_Rect a = spriteDst(st.moveFrom.x, st.moveFrom.y);
                const SDL_Rect b = spriteDst(st.moveTo.x, st.moveTo.y);

                SDL_Rect d = b;
                d.x = lerpI(a.x, b.x, t01);
                d.y = lerpI(a.y, b.y, t01);

                // Ground foot position (pre-hop).
                out.footX = d.x + d.w / 2;
                out.footY = d.y + d.h;

                // Hop arc.
                const float hopAmp = std::clamp(static_cast<float>(tile) * 0.12f, 1.0f, 8.0f);
                const float hop = std::sin(t01 * 3.14159265f) * hopAmp;
                out.lift01 = (hopAmp > 0.0f) ? std::clamp(hop / hopAmp, 0.0f, 1.0f) : 0.0f;
                d.y -= static_cast<int>(std::lround(hop));

                // Squash & stretch (anchored at bottom-center).
                const float bounce = std::sin(t01 * 3.14159265f);
                const float sx = 1.0f + 0.07f * bounce;
                const float sy = 1.0f - 0.07f * bounce;

                const int bottom = d.y + d.h;
                const int cx = d.x + d.w / 2;
                const int nw = std::max(1, static_cast<int>(std::lround(static_cast<float>(d.w) * sx)));
                const int nh = std::max(1, static_cast<int>(std::lround(static_cast<float>(d.h) * sy)));

                d.w = nw;
                d.h = nh;
                d.x = cx - nw / 2;
                d.y = bottom - nh;

                out.dst = d;
            }

            // Hurt recoil.
            if (st.hurtDuration > 0.0f && st.hurtTime < st.hurtDuration) {
                float t = st.hurtTime / st.hurtDuration;
                t = std::clamp(t, 0.0f, 1.0f);
                float k = 1.0f - t;
                k = k * k;

                const float kick = std::clamp(static_cast<float>(tile) * 0.10f, 2.0f, 6.0f);
                const int dx = static_cast<int>(std::lround(static_cast<float>(st.hurtDir.x) * kick * k));
                const int dy = static_cast<int>(std::lround(static_cast<float>(st.hurtDir.y) * kick * k));

                out.dst.x += dx;
                out.dst.y += dy;

                // Shadow should follow ground-plane motion.
                out.footX += dx;
                out.footY += dy;

                // Small extra vertical jolt reads well without hiding the tile.
                out.dst.y -= static_cast<int>(std::lround(kick * 0.35f * k));
            }
        }

        // Idle bob for non-moving entities.
        if (!moving) {
            const uint32_t h = hash32(hashCombine(static_cast<uint32_t>(e.id), lvlSeed));
            const float phase = static_cast<float>(h & 0xFFFFu) * (6.2831853f / 65536.0f);
            const float amp = std::clamp(static_cast<float>(tile) * 0.03f, 0.0f, 2.5f);
            const float bob = std::sin(static_cast<float>(ticks) * 0.0022f + phase) * amp;
            out.dst.y -= static_cast<int>(std::lround(bob));
        }

        return out;
    };

    auto itemBob = [&](const GroundItem& gi) -> float {
        const uint32_t h = hash32(hashCombine(static_cast<uint32_t>(gi.item.id), lvlSeed ^ 0xB0Bu));
        const float phase = static_cast<float>(h & 0xFFFFu) * (6.2831853f / 65536.0f);
        const float amp = std::clamp(static_cast<float>(tile) * 0.035f, 0.0f, 3.0f);
        const float freq = 0.0030f + static_cast<float>((h >> 16) & 0xFFu) * 0.000002f;
        return std::sin(static_cast<float>(ticks) * freq + phase) * amp;
    };

    // Room type cache (used for themed decals / minimap)
    auto rebuildRoomTypeCache = [&]() {
        roomCacheDungeon = &d;
        roomCacheBranch = game.branch();
        roomCacheDepth = game.depth();
        roomCacheW = d.width;
        roomCacheH = d.height;
        roomCacheRooms = d.rooms.size();

        roomTypeCache.assign(static_cast<size_t>(d.width * d.height), static_cast<uint8_t>(RoomType::Normal));
        for (const Room& r : d.rooms) {
            for (int yy = r.y; yy < r.y2(); ++yy) {
                for (int xx = r.x; xx < r.x2(); ++xx) {
                    if (!d.inBounds(xx, yy)) continue;
                    roomTypeCache[static_cast<size_t>(yy * d.width + xx)] = static_cast<uint8_t>(r.type);
                }
            }
        }
    };

    if (roomCacheDungeon != &d || roomCacheBranch != game.branch() || roomCacheDepth != game.depth() ||
        roomCacheW != d.width || roomCacheH != d.height || roomCacheRooms != d.rooms.size() ||
        roomTypeCache.size() != static_cast<size_t>(d.width * d.height)) {
        rebuildRoomTypeCache();
    }

    auto lightMod = [&](int x, int y) -> uint8_t {
        if (!game.darknessActive()) return 255;
        const uint8_t L = game.tileLightLevel(x, y); // 0..255
        constexpr int kMin = 40;
        int mod = kMin + (static_cast<int>(L) * (255 - kMin)) / 255;
        if (mod < kMin) mod = kMin;
        if (mod > 255) mod = 255;
        return static_cast<uint8_t>(mod);
    };

    // Subtle per-depth color grading so each floor feels distinct.
    // If styleSeed is non-zero, we derive a gentle per-run palette so each run feels visually unique.
    auto depthTint = [&]() -> Color {
        auto lerpU8 = [](uint8_t a, uint8_t b, float t) -> uint8_t {
            t = std::clamp(t, 0.0f, 1.0f);
            const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
            int iv = static_cast<int>(v + 0.5f);
            if (iv < 0) iv = 0;
            if (iv > 255) iv = 255;
            return static_cast<uint8_t>(iv);
        };

        const int depth = std::max(1, game.depth());
        const int maxDepth = std::max(1, game.dungeonMaxDepth());
        const float t = (maxDepth > 1) ? (static_cast<float>(depth - 1) / static_cast<float>(maxDepth - 1)) : 0.0f;

        // Default (legacy): warm torchlit stone up top -> colder, bluer depths below.
        Color warm{255, 246, 232, 255};
        Color deep{222, 236, 255, 255};

        if (styleSeed != 0u) {
            auto softTint = [](uint32_t seed, int base, int spread, int biasR, int biasG, int biasB) -> Color {
                seed = hash32(seed);
                auto chan = [&](int shift, int bias) -> uint8_t {
                    const int d = static_cast<int>((seed >> shift) & 0xFFu) - 128;
                    int v = base + (d * spread) / 128 + bias;
                    v = std::clamp(v, 200, 255);
                    return static_cast<uint8_t>(v);
                };
                return { chan(0, biasR), chan(8, biasG), chan(16, biasB), 255 };
            };

            // Branch offset keeps side branches from sharing the exact same palette.
            const uint32_t palSeed = hashCombine(styleSeed, static_cast<uint32_t>(game.branch()));

            warm = softTint(palSeed ^ 0x57A8C0DEu, 245, 18, 0, 0, 0);
            deep = softTint(palSeed ^ 0xC0FFEE99u, 232, 30, 0, 0, 0);

            // UI theme gently biases the palette so the dungeon feel matches the HUD tone.
            switch (game.uiTheme()) {
                case UITheme::Parchment:
                    warm = softTint(palSeed ^ 0x11A7000Fu, 247, 14, +6, +3, -2);
                    deep = softTint(palSeed ^ 0xD00D000Fu, 235, 26, +3, +1, -3);
                    break;
                case UITheme::Arcane:
                    warm = softTint(palSeed ^ 0xBADC0FFEu, 244, 18, -1, +1, +6);
                    deep = softTint(palSeed ^ 0xC001D00Du, 230, 30, -2, 0, +8);
                    break;
                case UITheme::DarkStone:
                default:
                    break;
            }

            // Avoid very dark tints; lighting is applied after this.
            deep.r = std::max<uint8_t>(deep.r, 205u);
            deep.g = std::max<uint8_t>(deep.g, 205u);
            deep.b = std::max<uint8_t>(deep.b, 205u);
        }

        return { lerpU8(warm.r, deep.r, t),
                 lerpU8(warm.g, deep.g, t),
                 lerpU8(warm.b, deep.b, t),
                 255 };
    };





    // Draw map tiles
    const Color tint = depthTint();

const float procPalStrength =
    (game.procPaletteEnabled() ? (std::clamp(game.procPaletteStrength(), 0, 100) / 100.0f) : 0.0f);

struct TerrainPaletteTints {
    std::array<Color, ROOM_STYLES> floorStyle{};
    Color wall{255, 255, 255, 255};
    Color chasm{255, 255, 255, 255};
    Color door{255, 255, 255, 255};
};

const TerrainPaletteTints terrainPalette = [&]() -> TerrainPaletteTints {
    TerrainPaletteTints p;
    for (auto& c : p.floorStyle) c = Color{255, 255, 255, 255};
    p.wall = Color{255, 255, 255, 255};
    p.chasm = Color{255, 255, 255, 255};
    p.door = Color{255, 255, 255, 255};

    if (procPalStrength <= 0.001f) return p;

    const int curDepth = std::max(1, game.depth());
    const int maxDepth = std::max(1, game.dungeonMaxDepth());
    const int clampedDepth = std::clamp(curDepth, 1, maxDepth);
    const float depth01 = (maxDepth > 1)
                              ? (static_cast<float>(clampedDepth - 1) / static_cast<float>(maxDepth - 1))
                              : 0.0f;

    // Base hue is per-run (styleSeed) with a gentle per-depth drift so each floor has
    // a distinct mood but remains coherent within a run.
    float baseHue = (hash32(styleSeed ^ 0xC0FFEEu) / 4294967296.0f);
    baseHue = frac01(baseHue + depth01 * 0.14f);

    float themeHueBias = 0.0f;
    float themeSatBias = 0.0f;
    float themeMixBias = 0.0f;
    switch (game.uiTheme()) {
        case UITheme::Parchment:
            themeHueBias = 0.03f;
            themeSatBias = 0.03f;
            themeMixBias = 0.02f;
            break;
        case UITheme::Arcane:
            themeHueBias = 0.74f;
            themeSatBias = 0.05f;
            themeMixBias = 0.04f;
            break;
        case UITheme::DarkStone:
        default:
            break;
    }

    baseHue = frac01(baseHue + themeHueBias);

    const float runJitter =
        ((hash32(styleSeed ^ 0x9E3779B9u) & 0xFFFFu) / 65535.0f - 0.5f) * 0.04f;
    baseHue = frac01(baseHue + runJitter);

    // Room-style palette offsets (relative to baseHue).
    constexpr float kHueOff[ROOM_STYLES] = {0.00f, 0.10f, 0.33f, 0.76f, 0.56f, 0.58f, 0.12f};
    constexpr float kSat[ROOM_STYLES] = {0.12f, 0.30f, 0.24f, 0.26f, 0.10f, 0.22f, 0.18f};
    constexpr float kLum[ROOM_STYLES] = {0.76f, 0.74f, 0.73f, 0.74f, 0.70f, 0.72f, 0.77f};
    constexpr float kMix[ROOM_STYLES] = {0.14f, 0.30f, 0.24f, 0.26f, 0.18f, 0.22f, 0.20f};

    for (int i = 0; i < ROOM_STYLES; ++i) {
        const float h = frac01(baseHue + kHueOff[i]);
        const float s = std::clamp(kSat[i] + themeSatBias, 0.0f, 0.85f);
        const float l = std::clamp(kLum[i], 0.0f, 1.0f);
        const float mix = std::clamp((kMix[i] + themeMixBias) * procPalStrength, 0.0f, 0.55f);
        p.floorStyle[static_cast<size_t>(i)] = tintFromHsl(h, s, l, mix);
    }

    const float wallMix = std::clamp((0.12f + themeMixBias) * procPalStrength, 0.0f, 0.40f);
    p.wall = tintFromHsl(baseHue + 0.02f, std::clamp(0.10f + themeSatBias * 0.5f, 0.0f, 0.40f), 0.66f,
                         wallMix);

    const float chasmMix = std::clamp((0.16f + themeMixBias) * procPalStrength, 0.0f, 0.45f);
    p.chasm = tintFromHsl(baseHue + 0.55f, std::clamp(0.16f + themeSatBias * 0.6f, 0.0f, 0.55f), 0.56f,
                          chasmMix);

    const float doorMix = std::clamp((0.20f + themeMixBias) * procPalStrength, 0.0f, 0.55f);
    p.door = tintFromHsl(baseHue + 0.08f, std::clamp(0.32f + themeSatBias, 0.0f, 0.85f), 0.66f, doorMix);

    return p;
}();

auto terrainPaletteTint = [&](TileType tt, int floorStyle) -> Color {
    if (procPalStrength <= 0.001f) return Color{255, 255, 255, 255};

    switch (tt) {
        case TileType::Wall:
        case TileType::DoorSecret:
            return terrainPalette.wall;
        case TileType::Chasm:
            return terrainPalette.chasm;
        case TileType::DoorClosed:
        case TileType::DoorLocked:
        case TileType::DoorOpen:
            return terrainPalette.door;
        default:
            break;
    }

    const int s = std::clamp(floorStyle, 0, ROOM_STYLES - 1);
    return terrainPalette.floorStyle[static_cast<size_t>(s)];
};

auto applyTerrainPalette = [&](const Color& baseMod, TileType tt, int floorStyle) -> Color {
    return mulColor(baseMod, terrainPaletteTint(tt, floorStyle));
};

auto terrainMaterialTint = [&](TerrainMaterial mat, TileType tt) -> Color {
    if (procPalStrength <= 0.001f) return Color{255, 255, 255, 255};

    // Doors are "objects" more than terrain substrate; keep them readable.
    if (tt == TileType::DoorClosed || tt == TileType::DoorLocked || tt == TileType::DoorOpen) {
        return Color{255, 255, 255, 255};
    }

    // Chasms are effectively void; don't "material tint" them.
    if (tt == TileType::Chasm) return Color{255, 255, 255, 255};

    float h = 0.58f;
    float s = 0.08f;
    float l = 0.62f;
    float mix = 0.22f;

    switch (mat) {
        case TerrainMaterial::Stone:
            h = 0.58f; s = 0.06f; l = 0.62f; mix = 0.16f; break;
        case TerrainMaterial::Brick:
            h = 0.04f; s = 0.34f; l = 0.56f; mix = 0.26f; break;
        case TerrainMaterial::Marble:
            h = 0.10f; s = 0.10f; l = 0.82f; mix = 0.22f; break;
        case TerrainMaterial::Basalt:
            h = 0.60f; s = 0.08f; l = 0.42f; mix = 0.22f; break;
        case TerrainMaterial::Obsidian:
            h = 0.76f; s = 0.20f; l = 0.30f; mix = 0.24f; break;
        case TerrainMaterial::Moss:
            h = 0.33f; s = 0.28f; l = 0.56f; mix = 0.22f; break;
        case TerrainMaterial::Dirt:
            h = 0.08f; s = 0.28f; l = 0.50f; mix = 0.22f; break;
        case TerrainMaterial::Wood:
            h = 0.09f; s = 0.32f; l = 0.55f; mix = 0.24f; break;
        case TerrainMaterial::Metal:
            h = 0.56f; s = 0.10f; l = 0.60f; mix = 0.20f; break;
        case TerrainMaterial::Crystal:
            h = 0.55f; s = 0.38f; l = 0.78f; mix = 0.26f; break;
        case TerrainMaterial::Bone:
            h = 0.12f; s = 0.18f; l = 0.78f; mix = 0.22f; break;
        default:
            break;
    }

    const bool wallish = (tt == TileType::Wall) || (tt == TileType::DoorSecret) || (tt == TileType::Pillar);
    l = wallish ? std::clamp(l - 0.08f, 0.12f, 0.92f) : std::clamp(l + 0.03f, 0.12f, 0.92f);

    // Scale with the existing procedural palette strength so users have one knob.
    const float m = std::clamp(mix * procPalStrength, 0.0f, 0.45f);
    return tintFromHsl(frac01(h), s, l, m);
};

auto applyTerrainStyleMod = [&](const Color& baseMod, TileType tt, int floorStyle, TerrainMaterial mat) -> Color {
    // Palette first (run+room themed), then substrate material tint.
    return mulColor(applyTerrainPalette(baseMod, tt, floorStyle), terrainMaterialTint(mat, tt));
};


    // Gather dynamic torch light sources so we can add subtle flame flicker in the renderer.
    // (The lightmap itself updates on turns; flicker is a purely visual, per-frame modulation.)
    struct TorchSrc {
        Vec2i pos;
        int radius = 7;
        float strength = 1.0f;
    };

    std::vector<TorchSrc> torches;
    if (game.darknessActive()) {
        // Player-held lit torch.
        bool playerTorch = false;
        for (const Item& it : game.inventory()) {
            if (it.kind == ItemKind::TorchLit && it.charges > 0) { playerTorch = true; break; }
        }
        if (playerTorch) {
            torches.push_back(TorchSrc{ game.player().pos, 9, 1.0f });
        }

        // Ground torches.
        for (const auto& gi : game.groundItems()) {
            if (gi.item.kind == ItemKind::TorchLit && gi.item.charges > 0) {
                torches.push_back(TorchSrc{ gi.pos, 7, 0.85f });
            }
        }
    }

    auto torchFlicker = [&](int x, int y) -> float {
        if (torches.empty()) return 1.0f;

        float best = 0.0f;
        TorchSrc bestT{};
        for (const TorchSrc& t : torches) {
            const int dx = x - t.pos.x;
            const int dy = y - t.pos.y;
            const int d2 = dx * dx + dy * dy;
            const int r2 = t.radius * t.radius;
            if (d2 > r2) continue;
            const float dist = std::sqrt(static_cast<float>(d2));
            const float att = std::max(0.0f, 1.0f - dist / static_cast<float>(t.radius)) * t.strength;
            if (att > best) { best = att; bestT = t; }
        }
        if (best <= 0.0f) return 1.0f;

        // Smooth-ish multi-frequency flicker, seeded by the strongest torch position.
        const float time = static_cast<float>(ticks) * 0.014f;
        const float seed = static_cast<float>(bestT.pos.x * 17 + bestT.pos.y * 31);
        const float w = (std::sin(time + seed) * 0.6f + std::sin(time * 2.13f + seed * 0.7f) * 0.4f);
        const float f = 1.0f + best * 0.05f * w; // very subtle (about +/-5% max near the torch)
        return std::clamp(f, 0.90f, 1.10f);
    };

    // Helper: compute per-tile texture color modulation (RGB) from lighting + depth tint.
    auto tileColorMod = [&](int x, int y, bool visible) -> Color {
        if (!visible) {
            const uint8_t base = game.darknessActive() ? 30u : 80u;
            return {
                static_cast<uint8_t>((static_cast<int>(base) * tint.r) / 255),
                static_cast<uint8_t>((static_cast<int>(base) * tint.g) / 255),
                static_cast<uint8_t>((static_cast<int>(base) * tint.b) / 255),
                255
            };
        }

        // Start with the global depth tint (and then layer lighting / flicker / patina on top).
        Color out{ tint.r, tint.g, tint.b, 255 };

        if (game.darknessActive()) {
            const uint8_t m = lightMod(x, y);
            Color lc = game.tileLightColor(x, y);

            // If the light color is (0,0,0) but the tile is still "visible" due to the short dark-vision radius,
            // fall back to a grayscale minimum brightness so the player can still read nearby terrain.
            if (lc.r == 0 && lc.g == 0 && lc.b == 0) {
                lc = { m, m, m, 255 };
            } else {
                const int minChan = std::max<int>(0, static_cast<int>(m) / 4);
                lc.r = static_cast<uint8_t>(std::max<int>(lc.r, minChan));
                lc.g = static_cast<uint8_t>(std::max<int>(lc.g, minChan));
                lc.b = static_cast<uint8_t>(std::max<int>(lc.b, minChan));
                lc.a = 255;
            }

            out = {
                static_cast<uint8_t>((static_cast<int>(lc.r) * tint.r) / 255),
                static_cast<uint8_t>((static_cast<int>(lc.g) * tint.g) / 255),
                static_cast<uint8_t>((static_cast<int>(lc.b) * tint.b) / 255),
                255
            };

            // Flame flicker: only modulate colors near active torch sources.
            const float f = torchFlicker(x, y);
            if (f != 1.0f) {
                out.r = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(static_cast<float>(out.r) * f)), 0, 255));
                out.g = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(static_cast<float>(out.g) * f)), 0, 255));
                out.b = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(static_cast<float>(out.b) * f)), 0, 255));
            }
        }

        // Procedural "patina" micro-variation: adds a tiny bit of per-tile value noise so floors/walls
        // don't read as perfectly flat even at small tile sizes. This is purely cosmetic.
        if (styleSeed != 0u && d.inBounds(x, y)) {
            float strength = 0.04f;
            const TileType tt = d.at(x, y).type;
            if (tt == TileType::Floor) strength = 0.055f;
            else if (tt == TileType::Wall || tt == TileType::DoorClosed || tt == TileType::DoorLocked) strength = 0.030f;
            else if (tt == TileType::Chasm) strength = 0.022f;

            if (isoView) strength *= 0.75f;

            const uint32_t h = hash32(hashCombine(lvlSeed ^ 0x9A71ACAu, hashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y))));
            const float n = (static_cast<float>(static_cast<int>(h & 0xFFu) - 128) / 128.0f); // [-1, 1]
            const float m = std::clamp(1.0f + n * strength, 0.85f, 1.15f);

            out.r = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(static_cast<float>(out.r) * m)), 0, 255));
            out.g = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(static_cast<float>(out.g) * m)), 0, 255));
            out.b = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(static_cast<float>(out.b) * m)), 0, 255));
        }

        return out;
    };

    auto styleForRoomType = [&](uint8_t rt) -> int {
        switch (static_cast<RoomType>(rt)) {
            case RoomType::Treasure: return 1;
            case RoomType::Lair:     return 2;
            case RoomType::Shrine:   return 3;
            case RoomType::Secret:   return 4;
            case RoomType::Vault:    return 5;
            case RoomType::Shop:     return 6;
            case RoomType::Armory:   return 5; // reuse Vault style
            case RoomType::Library:  return 3; // reuse Shrine style
            case RoomType::Laboratory: return 4; // reuse Secret style
            case RoomType::Normal:
            default: return 0;
        }
    };

    const std::array<uint8_t, DECAL_STYLES> decalChance = { 34, 64, 56, 72, 58, 52, 54 };

    // Returns the themed floor style for a tile coordinate, even when that tile is a
    // door/stairs/pillar. We primarily query the cached room type, and fall back to
    // adjacent tiles so door thresholds inherit the room style.
    auto floorStyleAt = [&](int tx, int ty) -> int {
        if (!d.inBounds(tx, ty)) return 0;
        const size_t ii = static_cast<size_t>(ty * d.width + tx);
        if (ii < roomTypeCache.size()) {
            const int s = styleForRoomType(roomTypeCache[ii]);
            if (s != 0) return s;
        }

        // Neighbor bias (useful for doors placed on room boundaries).
        const int dx[4] = { 1, -1, 0, 0 };
        const int dy[4] = { 0, 0, 1, -1 };
        for (int k = 0; k < 4; ++k) {
            const int nx = tx + dx[k];
            const int ny = ty + dy[k];
            if (!d.inBounds(nx, ny)) continue;
            const size_t jj = static_cast<size_t>(ny * d.width + nx);
            if (jj >= roomTypeCache.size()) continue;
            const int s2 = styleForRoomType(roomTypeCache[jj]);
            if (s2 != 0) return s2;
        }
        return 0;
    };

    auto isWallMass = [&](TileType tt) -> bool {
        switch (tt) {
            case TileType::Wall:
            case TileType::DoorClosed:
            case TileType::DoorLocked:
            case TileType::DoorSecret:
            case TileType::Pillar:
                return true;
            default:
                return false;
        }
    };

    auto wallOpenMaskAt = [&](int tx, int ty) -> uint8_t {
        uint8_t m = 0;
        if (!d.inBounds(tx, ty - 1) || !isWallMass(d.at(tx, ty - 1).type)) m |= 0x01u; // N
        if (!d.inBounds(tx + 1, ty) || !isWallMass(d.at(tx + 1, ty).type)) m |= 0x02u; // E
        if (!d.inBounds(tx, ty + 1) || !isWallMass(d.at(tx, ty + 1).type)) m |= 0x04u; // S
        if (!d.inBounds(tx - 1, ty) || !isWallMass(d.at(tx - 1, ty).type)) m |= 0x08u; // W
        return m;
    };



    auto isShadeOccluder = [&](TileType tt) -> bool {
        // Superset of wall-mass: also treat a few tall overlay props as occluders for floor contact shadows.
        return isWallMass(tt) || (tt == TileType::Boulder) || (tt == TileType::Fountain) || (tt == TileType::Altar);
    };

    // Mask bits: 1=N, 2=E, 4=S, 8=W (bit set means "neighbor is a shade occluder")
    auto wallOccMaskAt = [&](int tx, int ty) -> uint8_t {
        uint8_t m = 0;
        if (d.inBounds(tx, ty - 1) && isShadeOccluder(d.at(tx, ty - 1).type)) m |= 0x01u; // N
        if (d.inBounds(tx + 1, ty) && isShadeOccluder(d.at(tx + 1, ty).type)) m |= 0x02u; // E
        if (d.inBounds(tx, ty + 1) && isShadeOccluder(d.at(tx, ty + 1).type)) m |= 0x04u; // S
        if (d.inBounds(tx - 1, ty) && isShadeOccluder(d.at(tx - 1, ty).type)) m |= 0x08u; // W
        return m;
    };

    auto chasmOpenMaskAt = [&](int tx, int ty) -> uint8_t {
        uint8_t m = 0;
        auto isCh = [&](int xx, int yy) -> bool {
            return d.inBounds(xx, yy) && d.at(xx, yy).type == TileType::Chasm;
        };
        if (!isCh(tx, ty - 1)) m |= 0x01u; // N
        if (!isCh(tx + 1, ty)) m |= 0x02u; // E
        if (!isCh(tx, ty + 1)) m |= 0x04u; // S
        if (!isCh(tx - 1, ty)) m |= 0x08u; // W
        return m;
    };



    auto drawMapTile = [&](int x, int y) {
        if (!mapTileInView(x, y)) return;
        const Tile& t = d.at(x, y);
        SDL_Rect dst = tileDst(x, y);

        if (!t.explored) {
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderFillRect(renderer, &dst);
            return;
        }

        // Isometric mode: draw a diamond-projected ground tile, then draw any tall
        // blocking terrain (walls/doors/pillars/boulders) as sprite-sized overlays.
        // This keeps the ground plane clean (no square squashing artifacts) and gives
        // a more convincing 2.5D feel.
        if (isoView) {
            // Ground base: chasms keep their own material; everything else gets a themed
            // floor so wall blocks sit on something consistent.
            TileType base = (t.type == TileType::Chasm) ? TileType::Chasm : TileType::Floor;
            const int style = (base == TileType::Floor) ? floorStyleAt(x, y) : 0;

            SDL_Texture* btex = tileTexture(base, x, y, levelKey, frame, style);
            const Color baseMod = tileColorMod(x, y, t.visible);
            const TerrainMaterial mat = d.materialAtCached(x, y);
            const Color mod = applyTerrainStyleMod(baseMod, base, style, mat);
            const Color modTall = applyTerrainStyleMod(baseMod, t.type, style, mat);
            const Uint8 a = t.visible ? 255 : (game.darknessActive() ? 115 : 175);

            if (btex) {
                SDL_SetTextureColorMod(btex, mod.r, mod.g, mod.b);
                SDL_SetTextureAlphaMod(btex, a);
                SDL_RenderCopy(renderer, btex, nullptr, &dst);
                SDL_SetTextureColorMod(btex, 255, 255, 255);
                SDL_SetTextureAlphaMod(btex, 255);
            }

            // Themed floor decals (isometric): project decal overlays onto the diamond grid so room
            // themes keep their subtle surface detail in 2.5D view.
            //
            // We skip true wall-mass tiles (Wall / secret doors) since those are rendered as 2.5D blocks.
            if (base == TileType::Floor && t.type != TileType::Wall && t.type != TileType::DoorSecret && !floorDecalVarIso.empty()) {
                const int dStyle = style;

                // Jittered-grid placement gives a more even, less clumpy distribution than independent per-tile RNG.
                const uint32_t dSeed = hashCombine(lvlSeed ^ 0xDECA151u,
                                                  static_cast<uint32_t>(dStyle) * 0x9E3779B9u);
                uint32_t cellR = 0;
                const int cell = 3;

                if (dStyle >= 0 && dStyle < DECAL_STYLES &&
                    shouldPlaceDecalJittered(x, y, dSeed, cell, decalChance[static_cast<size_t>(dStyle)], cellR)) {
                    const int var = static_cast<int>((cellR >> 24) % static_cast<uint32_t>(decalsPerStyleUsed));
                    const size_t di = static_cast<size_t>(dStyle * decalsPerStyleUsed + var);

                    if (di < floorDecalVarIso.size()) {
                        // Custom animation: de-sync some animated decal styles so special rooms
                        // don't all pulse in perfect unison.
                        int dFrame = frame % FRAMES;
                        if (dStyle == 2 || dStyle == 3) {
                            const uint32_t ph = hash32(hashCombine(hashCombine(lvlSeed ^ 0xD3CA1u, static_cast<uint32_t>(dStyle)),
                                                                   hashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y))));
                            dFrame = (dFrame + static_cast<int>(ph & static_cast<uint32_t>(FRAMES - 1))) % FRAMES;
                        }

                        SDL_Texture* dtex = floorDecalVarIso[di][static_cast<size_t>(dFrame)];
                        if (dtex) {
                            SDL_SetTextureColorMod(dtex, mod.r, mod.g, mod.b);
                            SDL_SetTextureAlphaMod(dtex, a);
                            SDL_RenderCopy(renderer, dtex, nullptr, &dst);
                            SDL_SetTextureColorMod(dtex, 255, 255, 255);
                            SDL_SetTextureAlphaMod(dtex, 255);
                        }
                    }
                }

            }

            // Ground-plane overlays that should stay on the diamond tile.
            if (t.type == TileType::StairsUp) {
                SDL_Texture* otex = stairsUpOverlayIsoTex[static_cast<size_t>(frame % FRAMES)];
                if (!otex) otex = stairsUpOverlayTex[static_cast<size_t>(frame % FRAMES)];
                if (otex) {
                    SDL_SetTextureColorMod(otex, mod.r, mod.g, mod.b);
                    SDL_SetTextureAlphaMod(otex, a);
                    SDL_RenderCopy(renderer, otex, nullptr, &dst);
                    SDL_SetTextureColorMod(otex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(otex, 255);
                }
            } else if (t.type == TileType::StairsDown) {
                SDL_Texture* otex = stairsDownOverlayIsoTex[static_cast<size_t>(frame % FRAMES)];
                if (!otex) otex = stairsDownOverlayTex[static_cast<size_t>(frame % FRAMES)];
                if (otex) {
                    SDL_SetTextureColorMod(otex, mod.r, mod.g, mod.b);
                    SDL_SetTextureAlphaMod(otex, a);
                    SDL_RenderCopy(renderer, otex, nullptr, &dst);
                    SDL_SetTextureColorMod(otex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(otex, 255);
                }
            } else if (t.type == TileType::DoorOpen) {
                // If we have a 2.5D doorway frame sprite available, prefer that over a flat floor overlay.
                // (Open doors are passable, but still read better as vertical architecture in isometric view.)
                if (doorBlockOpenVarIso.empty()) {
                    SDL_Texture* otex = doorOpenOverlayIsoTex[static_cast<size_t>(frame % FRAMES)];
                    if (!otex) otex = doorOpenOverlayTex[static_cast<size_t>(frame % FRAMES)];
                    if (otex) {
                        const Color doorMod = applyTerrainPalette(baseMod, t.type, style);
                        SDL_SetTextureColorMod(otex, doorMod.r, doorMod.g, doorMod.b);
                        SDL_SetTextureAlphaMod(otex, a);
                        SDL_RenderCopy(renderer, otex, nullptr, &dst);
                        SDL_SetTextureColorMod(otex, 255, 255, 255);
                        SDL_SetTextureAlphaMod(otex, 255);
                    }
                }
            }

            // Isometric cast shadows: soft directional shadows from tall occluders (walls/closed doors/pillars/etc)
            // onto adjacent ground tiles. This adds a strong depth cue in the 2.5D isometric view.
            if (t.type != TileType::Chasm) {
                auto isIsoShadowCaster = [&](TileType tt) -> bool {
                    switch (tt) {
                        case TileType::Wall:
                        case TileType::DoorClosed:
                        case TileType::DoorLocked:
                        case TileType::DoorOpen:
                        case TileType::DoorSecret:
                        case TileType::Pillar:
                        case TileType::Boulder:
                        case TileType::Fountain:
                        case TileType::Altar:
                            return true;
                        default:
                            return false;
                    }
                };

                if (!isIsoShadowCaster(t.type)) {
                    // Procedural per-run isometric light direction. We only shade from occluders that are
                    // known (visible or explored) to avoid leaking information about unexplored space.
                    struct ShadowDir { int dx; int dy; uint8_t bit; };

                    ShadowDir sdA{0, -1, 0x01u};
                    ShadowDir sdB{-1, 0, 0x08u};
                    int dxDiag = -1;
                    int dyDiag = -1;

                    switch (isoLightDir & 0x03u) {
                        default:
                        case 0: // light from NW (shadows to SE)
                            sdA = ShadowDir{0, -1, 0x01u};
                            sdB = ShadowDir{-1, 0, 0x08u};
                            dxDiag = -1; dyDiag = -1;
                            break;
                        case 1: // light from NE (shadows to SW)
                            sdA = ShadowDir{0, -1, 0x01u};
                            sdB = ShadowDir{1, 0, 0x02u};
                            dxDiag = 1; dyDiag = -1;
                            break;
                        case 2: // light from SE (shadows to NW)
                            sdA = ShadowDir{0, 1, 0x04u};
                            sdB = ShadowDir{1, 0, 0x02u};
                            dxDiag = 1; dyDiag = 1;
                            break;
                        case 3: // light from SW (shadows to NE)
                            sdA = ShadowDir{0, 1, 0x04u};
                            sdB = ShadowDir{-1, 0, 0x08u};
                            dxDiag = -1; dyDiag = 1;
                            break;
                    }

                    // Scale shadow strength by caster type: walls/closed doors should feel taller
                    // than props like altars/fountains, so their cast shadow reads more clearly.
                    auto shadowStrengthFor = [&](TileType tt) -> float {
                        switch (tt) {
                            case TileType::Wall:
                            case TileType::DoorSecret:
                                return 1.00f;
                            case TileType::DoorClosed:
                            case TileType::DoorLocked:
                                return 0.95f;
                            case TileType::Pillar:
                                return 0.85f;
                            case TileType::Boulder:
                                return 0.80f;
                            case TileType::Fountain:
                                return 0.72f;
                            case TileType::Altar:
                                return 0.68f;
                            case TileType::DoorOpen:
                                return 0.65f;
                            default:
                                return 0.80f;
                        }
                    };

                    auto distFalloff = [&](int dist) -> float {
                        // Lightweight distance falloff for multi-tile soft shadows.
                        switch (dist) {
                            case 1: return 1.00f;
                            case 2: return 0.62f;
                            case 3: return 0.38f;
                            default: return 0.24f;
                        }
                    };

                    constexpr int kMaxShadowDist = 3;

                    uint8_t shMask = 0u;
                    float strength = 0.0f;

                    auto considerRay = [&](const ShadowDir& sd) {
                        for (int dist = 1; dist <= kMaxShadowDist; ++dist) {
                            const int nx = x + sd.dx * dist;
                            const int ny = y + sd.dy * dist;
                            if (!d.inBounds(nx, ny)) break;

                            const Tile& ntile = d.at(nx, ny);
                            // Stop at unknown tiles to avoid revealing information about unexplored space.
                            if (!ntile.visible && !ntile.explored) break;

                            if (isIsoShadowCaster(ntile.type)) {
                                shMask |= sd.bit;
                                strength = std::max(strength, shadowStrengthFor(ntile.type) * distFalloff(dist));
                                break;
                            }
                        }
                    };

                    considerRay(sdA);
                    considerRay(sdB);

                    // Diagonal occluder boosts the inner corner (tight corridor grounding).
                    for (int dist = 1; dist <= kMaxShadowDist; ++dist) {
                        const int nx = x + dxDiag * dist;
                        const int ny = y + dyDiag * dist;
                        if (!d.inBounds(nx, ny)) break;

                        const Tile& ntile = d.at(nx, ny);
                        if (!ntile.visible && !ntile.explored) break;

                        if (isIsoShadowCaster(ntile.type)) {
                            shMask |= (sdA.bit | sdB.bit);
                            strength = std::max(strength, shadowStrengthFor(ntile.type) * distFalloff(dist) * 0.92f);
                            break;
                        }
                    }

                    strength = std::clamp(strength, 0.0f, 1.0f);

                    if (shMask != 0u) {
                        SDL_Texture* stex = isoCastShadowVar[static_cast<size_t>(shMask)][static_cast<size_t>(frame % FRAMES)];
                        if (stex) {
                            const uint8_t lm = t.visible ? lightMod(x, y) : (game.darknessActive() ? 120u : 170u);
                            int a2 = 44;
                            a2 = (a2 * static_cast<int>(lm)) / 255;
                            if (!t.visible) a2 = std::min(a2, 26);

                            if (strength > 0.0f) {
                                a2 = static_cast<int>(std::lround(static_cast<float>(a2) * strength));
                            }

                            SDL_SetTextureColorMod(stex, 0, 0, 0);
                            SDL_SetTextureAlphaMod(stex, static_cast<Uint8>(std::clamp(a2, 0, 255)));
                            SDL_RenderCopy(renderer, stex, nullptr, &dst);
                            SDL_SetTextureColorMod(stex, 255, 255, 255);
                            SDL_SetTextureAlphaMod(stex, 255);
                        }
                    }
                }

            }

            // Isometric edge shading: contact shadows against tall occluders + a cool rim against chasms.
            // This helps the 2.5D view read depth without requiring new hand-authored art.
            if (t.type != TileType::Chasm) {
                auto isIsoOccluder = [&](TileType tt) -> bool {
                    switch (tt) {
                        case TileType::Wall:
                        case TileType::DoorClosed:
                        case TileType::DoorLocked:
                        case TileType::DoorOpen:
                        case TileType::DoorSecret:
                        case TileType::Pillar:
                        case TileType::Boulder:
                        case TileType::Fountain:
                        case TileType::Altar:
                            return true;
                        default:
                            return false;
                    }
                };

                if (!isIsoOccluder(t.type)) {
                    uint8_t occMask = 0u;
                    uint8_t chMask = 0u;

                    auto accumulate = [&](int nx, int ny, uint8_t bit) {
                        if (!d.inBounds(nx, ny)) {
                            occMask |= bit;
                            return;
                        }
                        const TileType nt = d.at(nx, ny).type;
                        if (nt == TileType::Chasm) chMask |= bit;
                        else if (isIsoOccluder(nt)) occMask |= bit;
                    };

                    accumulate(x, y - 1, 0x01u); // N
                    accumulate(x + 1, y, 0x02u); // E
                    accumulate(x, y + 1, 0x04u); // S
                    accumulate(x - 1, y, 0x08u); // W

                    if ((occMask | chMask) != 0u) {
                        const uint8_t lm = t.visible ? lightMod(x, y) : (game.darknessActive() ? 120u : 170u);
                        int baseA2 = 32;
                        baseA2 = (baseA2 * static_cast<int>(lm)) / 255;
                        if (!t.visible) baseA2 = std::min(baseA2, 22);

                        auto drawEdge = [&](uint8_t mask, Color col, int alpha) {
                            if (mask == 0u || alpha <= 0) return;
                            if (alpha > 255) alpha = 255;
                            SDL_Texture* etex = isoEdgeShadeVar[static_cast<size_t>(mask)][static_cast<size_t>(frame % FRAMES)];
                            if (!etex) return;
                            SDL_SetTextureColorMod(etex, col.r, col.g, col.b);
                            SDL_SetTextureAlphaMod(etex, static_cast<Uint8>(alpha));
                            SDL_RenderCopy(renderer, etex, nullptr, &dst);
                            SDL_SetTextureColorMod(etex, 255, 255, 255);
                            SDL_SetTextureAlphaMod(etex, 255);
                        };

                        auto drawGloom = [&](uint8_t mask, Color col, int alpha) {
                            if (mask == 0u || alpha <= 0) return;
                            if (alpha > 255) alpha = 255;
                            SDL_Texture* gtex = isoChasmGloomVar[static_cast<size_t>(mask)][static_cast<size_t>(frame % FRAMES)];
                            if (!gtex) return;
                            SDL_SetTextureColorMod(gtex, col.r, col.g, col.b);
                            SDL_SetTextureAlphaMod(gtex, static_cast<Uint8>(alpha));
                            SDL_RenderCopy(renderer, gtex, nullptr, &dst);
                            SDL_SetTextureColorMod(gtex, 255, 255, 255);
                            SDL_SetTextureAlphaMod(gtex, 255);
                        };

                        // Chasm edges: faint blue rim + darkness.
                        if (chMask != 0u) {
                            // Extra interior darkening to suggest the "drop" into the chasm.
                            int gloomA = baseA2 + 18;
                            if (!t.visible) gloomA = std::min(gloomA, baseA2 + 10);
                            drawGloom(chMask, {0, 0, 0, 255}, std::max(12, gloomA));

                            drawEdge(chMask, {40, 80, 160, 255}, std::max(8, baseA2 / 2));
                            drawEdge(chMask, {0, 0, 0, 255}, baseA2);
                        }
                        // Tall occluders: contact shadow only.
                        if (occMask != 0u) {
                            drawEdge(occMask, {0, 0, 0, 255}, baseA2);
                        }
                    }
                }
            }

            // Tall blockers & objects.
            auto drawTall = [&](SDL_Texture* tex, bool outline) {
                if (!tex) return;
                SDL_Rect sdst = spriteDst(x, y);

                // In explored-but-not-visible memory view we draw a bit darker so the
                // player can still navigate without everything looking "lit".
                Uint8 aa = t.visible ? 255 : (game.darknessActive() ? 150 : 190);

                // Isometric cutaway: fade foreground occluders near the player/cursor so
                // interiors remain readable in the 2.5D camera.
                if (isoCutawayOn) {
                    const int focusSum = isoFocusSum;
                    const int focusDiff = isoFocusDiff;

                    const int ahead = (x + y) - focusSum;                         // >0 = in front (towards camera)
                    const int side  = std::abs((x - y) - focusDiff);              // horizontal band in screen space
                    const int man   = std::abs(x - isoCutawayFocus.x) + std::abs(y - isoCutawayFocus.y);

                    // Only cut away a shallow wedge in front of the focus tile.
                    if (ahead >= 1 && ahead <= 5 && side <= 2 && man <= 6) {
                        const int cut = ahead + side; // 1..7 typically
                        const int maxCut = 7;

                        const int minA = t.visible ? 70 : 55; // keep some presence even when heavily faded
                        const int target = minA + (cut * (static_cast<int>(aa) - minA)) / maxCut;

                        aa = static_cast<Uint8>(std::min<int>(static_cast<int>(aa), std::max(minA, target)));
                    }
                }

                drawSpriteWithShadowOutline(renderer, tex, sdst, modTall, aa, /*shadow=*/false, outline);
            };

            if (t.type == TileType::Wall || t.type == TileType::DoorSecret) {
                if (!wallBlockVarIso.empty()) {
                    const uint32_t seed = hashCombine(lvlSeed ^ 0x00AA110u, 0x000B10Cu);
                    const size_t v = pickCoherentVariantIndex(x, y, seed, wallBlockVarIso.size());
                    SDL_Texture* wtex = wallBlockVarIso[v][static_cast<size_t>(frame % FRAMES)];
                    // Wall blocks are already outlined in their procedural art.
                    drawTall(wtex, /*outline=*/false);
                }
            } else if (t.type == TileType::DoorClosed) {
                if (!doorBlockClosedVarIso.empty()) {
                    const uint32_t hh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0xD00Du ^ 0xC105EDu;
                    const size_t idx = static_cast<size_t>(hash32(hh) % static_cast<uint32_t>(doorBlockClosedVarIso.size()));
                    drawTall(doorBlockClosedVarIso[idx][static_cast<size_t>(frame % FRAMES)], /*outline=*/false);
                } else {
                    // Fallback (should be rare): top-down overlay sprite.
                    drawTall(doorClosedOverlayTex[static_cast<size_t>(frame % FRAMES)], /*outline=*/true);
                }
            } else if (t.type == TileType::DoorLocked) {
                if (!doorBlockLockedVarIso.empty()) {
                    const uint32_t hh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0xD00Du ^ 0x10CCEDu;
                    const size_t idx = static_cast<size_t>(hash32(hh) % static_cast<uint32_t>(doorBlockLockedVarIso.size()));
                    drawTall(doorBlockLockedVarIso[idx][static_cast<size_t>(frame % FRAMES)], /*outline=*/false);
                } else {
                    // Fallback (should be rare): top-down overlay sprite.
                    drawTall(doorLockedOverlayTex[static_cast<size_t>(frame % FRAMES)], /*outline=*/true);
                }
            } else if (t.type == TileType::DoorOpen) {
                if (!doorBlockOpenVarIso.empty()) {
                    const uint32_t hh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0xD00Du ^ 0x0B0A1u;
                    const size_t idx = static_cast<size_t>(hash32(hh) % static_cast<uint32_t>(doorBlockOpenVarIso.size()));
                    drawTall(doorBlockOpenVarIso[idx][static_cast<size_t>(frame % FRAMES)], /*outline=*/false);
                }
            } else if (t.type == TileType::Pillar) {
                const uint32_t hh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                               static_cast<uint32_t>(y)) ^ 0x9111A0u;
                if (!pillarBlockVarIso.empty()) {
                    const size_t idx = static_cast<size_t>(hash32(hh) % static_cast<uint32_t>(pillarBlockVarIso.size()));
                    drawTall(pillarBlockVarIso[idx][static_cast<size_t>(frame % FRAMES)], /*outline=*/false);
                } else if (!pillarOverlayVar.empty()) {
                    // Fallback: top-down overlay sprite (less ideal in isometric view).
                    const size_t idx = static_cast<size_t>(hash32(hh) % static_cast<uint32_t>(pillarOverlayVar.size()));
                    drawTall(pillarOverlayVar[idx][static_cast<size_t>(frame % FRAMES)], /*outline=*/true);
                }
            } else if (t.type == TileType::Boulder) {
                const uint32_t hh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                               static_cast<uint32_t>(y)) ^ 0xB011D3u;
                if (!boulderBlockVarIso.empty()) {
                    const size_t idx = static_cast<size_t>(hash32(hh) % static_cast<uint32_t>(boulderBlockVarIso.size()));
                    drawTall(boulderBlockVarIso[idx][static_cast<size_t>(frame % FRAMES)], /*outline=*/false);
                } else if (!boulderOverlayVar.empty()) {
                    // Fallback: top-down overlay sprite (less ideal in isometric view).
                    const size_t idx = static_cast<size_t>(hash32(hh) % static_cast<uint32_t>(boulderOverlayVar.size()));
                    drawTall(boulderOverlayVar[idx][static_cast<size_t>(frame % FRAMES)], /*outline=*/true);
                }
            } else if (t.type == TileType::Fountain) {
                if (!fountainOverlayVar.empty()) {
                    const uint32_t hh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0xF017A1u;
                    const size_t idx = static_cast<size_t>(hash32(hh) % static_cast<uint32_t>(fountainOverlayVar.size()));
                    drawTall(fountainOverlayVar[idx][static_cast<size_t>(frame % FRAMES)], /*outline=*/true);
                }
            } else if (t.type == TileType::Altar) {
                if (!altarOverlayVar.empty()) {
                    const uint32_t hh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0xA17A12u;
                    const size_t idx = static_cast<size_t>(hash32(hh) % static_cast<uint32_t>(altarOverlayVar.size()));
                    drawTall(altarOverlayVar[idx][static_cast<size_t>(frame % FRAMES)], /*outline=*/true);
                }
            }

            return;
        }

        // Doors/stairs/pillars are rendered as transparent overlays layered on top of the
        // underlying floor so they inherit themed room flooring.
        const bool isOverlay =
            (t.type == TileType::Pillar) ||
            (t.type == TileType::Boulder) ||
            (t.type == TileType::Fountain) ||
            (t.type == TileType::Altar) ||
            (t.type == TileType::StairsUp) || (t.type == TileType::StairsDown) ||
            (t.type == TileType::DoorClosed) || (t.type == TileType::DoorLocked) || (t.type == TileType::DoorOpen);

        TileType baseType = t.type;
        if (isOverlay) baseType = TileType::Floor;

        int floorStyle = (baseType == TileType::Floor) ? floorStyleAt(x, y) : 0;

        SDL_Texture* tex = tileTexture(baseType, x, y, levelKey, frame, floorStyle);
        if (!tex) return;

        const Color baseMod = tileColorMod(x, y, t.visible);
        const TerrainMaterial mat = d.materialAtCached(x, y);
        const Color mod = applyTerrainStyleMod(baseMod, baseType, floorStyle, mat);
        const Color modObj = isOverlay ? applyTerrainStyleMod(baseMod, t.type, floorStyle, mat) : mod;
        SDL_SetTextureColorMod(tex, mod.r, mod.g, mod.b);
        SDL_SetTextureAlphaMod(tex, 255);

        SDL_RenderCopy(renderer, tex, nullptr, &dst);

        SDL_SetTextureColorMod(tex, 255, 255, 255);
        SDL_SetTextureAlphaMod(tex, 255);

        // Themed floor decals add subtle detail and make special rooms stand out.
        // Applied to any tile whose *base* is floor (including overlay tiles).
        if (baseType == TileType::Floor && !floorDecalVar.empty()) {
            const int style = floorStyle;

            // Jittered-grid placement gives a more even, less clumpy distribution than independent per-tile RNG.
            const uint32_t dSeed = hashCombine(lvlSeed ^ 0xDECA151u,
                                              static_cast<uint32_t>(style) * 0x9E3779B9u);
            uint32_t cellR = 0;
            const int cell = 3;

            if (style >= 0 && style < DECAL_STYLES &&
                shouldPlaceDecalJittered(x, y, dSeed, cell, decalChance[static_cast<size_t>(style)], cellR)) {
                const int var = static_cast<int>((cellR >> 24) % static_cast<uint32_t>(decalsPerStyleUsed));
                const size_t di = static_cast<size_t>(style * decalsPerStyleUsed + var);

                if (di < floorDecalVar.size()) {
                    // Custom animation: de-sync some animated decal styles so special rooms
                    // (lairs/shrines) don't all pulse in perfect unison.
                    int dFrame = frame % FRAMES;
                    if (style == 2 || style == 3) {
                        const uint32_t ph = hash32(hashCombine(hashCombine(lvlSeed ^ 0xD3CA1u, static_cast<uint32_t>(style)),
                                                               hashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y))));
                        dFrame = (dFrame + static_cast<int>(ph & static_cast<uint32_t>(FRAMES - 1))) % FRAMES;
                    }

                    SDL_Texture* dtex = floorDecalVar[di][static_cast<size_t>(dFrame)];
                    if (dtex) {
                        const Uint8 a = t.visible ? 255 : (game.darknessActive() ? 120 : 160);
                        SDL_SetTextureColorMod(dtex, mod.r, mod.g, mod.b);
                        SDL_SetTextureAlphaMod(dtex, a);
                        SDL_RenderCopy(renderer, dtex, nullptr, &dst);
                        SDL_SetTextureColorMod(dtex, 255, 255, 255);
                        SDL_SetTextureAlphaMod(dtex, 255);
                    }
                }
            }
        }


        // Top-down wall contact shadows (ambient occlusion): subtle depth shading on floors next to walls.
        if (baseType == TileType::Floor) {
            const uint8_t occMask = wallOccMaskAt(x, y);
            if (occMask != 0u) {
                const uint32_t h = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                               static_cast<uint32_t>(y)) ^ 0x5EAD0DEu ^ static_cast<uint32_t>(occMask);
                const uint32_t r = hash32(h);
                const size_t v = static_cast<size_t>(r % static_cast<uint32_t>(autoVarsUsed));

                SDL_Texture* stex = topDownWallShadeVar[static_cast<size_t>(occMask)][v][static_cast<size_t>(frame % FRAMES)];
                if (stex) {
                    // Keep subtle: stronger when visible, weaker when only "explored".
                    const Uint8 a = t.visible ? 140 : (game.darknessActive() ? 70 : 95);
                    SDL_SetTextureColorMod(stex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(stex, a);
                    SDL_RenderCopy(renderer, stex, nullptr, &dst);
                    SDL_SetTextureColorMod(stex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(stex, 255);
                }
            }
        }

        // Occasional wall stains/cracks (very low frequency; helps break large flat walls).
        if ((t.type == TileType::Wall || t.type == TileType::DoorSecret) && !wallDecalVar.empty()) {
            const uint32_t h = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                           static_cast<uint32_t>(y)) ^ 0xBADC0DEu;
            const uint32_t r = hash32(h);
            const uint8_t roll = static_cast<uint8_t>(r & 0xFFu);
            if (roll < 18u) {
                // Avoid clumps: only render if this tile is the lowest-roll candidate among immediate wall neighbors.
                bool keep = true;
                const int ndx[4] = {1, -1, 0, 0};
                const int ndy[4] = {0, 0, 1, -1};
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + ndx[k];
                    const int ny = y + ndy[k];
                    if (!d.inBounds(nx, ny)) continue;
                    const TileType nt = d.at(nx, ny).type;
                    if (nt != TileType::Wall && nt != TileType::DoorSecret) continue;

                    const uint32_t nh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(nx)),
                                                    static_cast<uint32_t>(ny)) ^ 0xBADC0DEu;
                    const uint32_t nr = hash32(nh);
                    const uint8_t nroll = static_cast<uint8_t>(nr & 0xFFu);
                    if (nroll < 18u && nroll < roll) { keep = false; break; }
                }

                if (!keep) {
                    // A neighboring wall tile "won" this cluster; skip to keep stains sparse and nicely distributed.
                } else {
                    int style = 0;
                    // If a neighboring floor belongs to a special room, bias the wall decal style.
                    const int dx[4] = {1,-1,0,0};
                    const int dy[4] = {0,0,1,-1};
                    for (int k = 0; k < 4; ++k) {
                        const int nx = x + dx[k];
                        const int ny = y + dy[k];
                        if (!d.inBounds(nx, ny)) continue;
                        if (d.at(nx, ny).type != TileType::Floor) continue;
                        const size_t jj = static_cast<size_t>(ny * d.width + nx);
                        if (jj >= roomTypeCache.size()) continue;
                        const int s2 = styleForRoomType(roomTypeCache[jj]);
                        if (s2 != 0) { style = s2; break; }
                    }
    
                    const int var = static_cast<int>((r >> 8) % static_cast<uint32_t>(decalsPerStyleUsed));
                    const size_t di = static_cast<size_t>(style * decalsPerStyleUsed + var);
                    if (di < wallDecalVar.size()) {
                        SDL_Texture* dtex = wallDecalVar[di][static_cast<size_t>(frame % FRAMES)];
                        if (dtex) {
                            const Uint8 a = t.visible ? 220 : 120;
                            SDL_SetTextureColorMod(dtex, mod.r, mod.g, mod.b);
                            SDL_SetTextureAlphaMod(dtex, a);
                            SDL_RenderCopy(renderer, dtex, nullptr, &dst);
                            SDL_SetTextureColorMod(dtex, 255, 255, 255);
                            SDL_SetTextureAlphaMod(dtex, 255);
                        }
                    }
                }
            }
        }

        // Autotile edge/rim overlays add crisp silhouette and depth for large wall/chasm fields.
        if ((t.type == TileType::Wall || t.type == TileType::DoorSecret)) {
            const uint8_t mask = wallOpenMaskAt(x, y);
            if (mask != 0u) {
                const uint32_t h = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                               static_cast<uint32_t>(y)) ^ 0xED6E7u ^ static_cast<uint32_t>(mask);
                const uint32_t r = hash32(h);
                const size_t v = static_cast<size_t>(r % static_cast<uint32_t>(autoVarsUsed));

                SDL_Texture* etex = wallEdgeVar[static_cast<size_t>(mask)][v][static_cast<size_t>(frame % FRAMES)];
                if (etex) {
                    const Uint8 a = t.visible ? 255 : (game.darknessActive() ? 150 : 190);
                    SDL_SetTextureColorMod(etex, mod.r, mod.g, mod.b);
                    SDL_SetTextureAlphaMod(etex, a);
                    SDL_RenderCopy(renderer, etex, nullptr, &dst);
                    SDL_SetTextureColorMod(etex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(etex, 255);
                }
            }
        } else if (t.type == TileType::Chasm) {
            const uint8_t mask = chasmOpenMaskAt(x, y);
            if (mask != 0u) {
                const uint32_t h = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                               static_cast<uint32_t>(y)) ^ 0xC11A5u ^ static_cast<uint32_t>(mask);
                const uint32_t r = hash32(h);
                const size_t v = static_cast<size_t>(r % static_cast<uint32_t>(autoVarsUsed));

                SDL_Texture* rtex = chasmRimVar[static_cast<size_t>(mask)][v][static_cast<size_t>(frame % FRAMES)];
                if (rtex) {
                    const Uint8 a = t.visible ? 255 : (game.darknessActive() ? 135 : 175);
                    SDL_SetTextureColorMod(rtex, mod.r, mod.g, mod.b);
                    SDL_SetTextureAlphaMod(rtex, a);
                    SDL_RenderCopy(renderer, rtex, nullptr, &dst);
                    SDL_SetTextureColorMod(rtex, 255, 255, 255);
                    SDL_SetTextureAlphaMod(rtex, 255);
                }
            }
        }

        // Render overlays on top of floor base.
        if (isOverlay) {
            SDL_Texture* otex = nullptr;
            switch (t.type) {
                case TileType::Pillar: {
                    if (!pillarOverlayVar.empty()) {
                        const uint32_t hh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                       static_cast<uint32_t>(y)) ^ 0x9111A0u;
                        const uint32_t rr = hash32(hh);
                        const size_t idx = static_cast<size_t>(rr % static_cast<uint32_t>(pillarOverlayVar.size()));
                        otex = pillarOverlayVar[idx][static_cast<size_t>(frame % FRAMES)];
                    }
                    break;
                }
                case TileType::Boulder: {
                    if (!boulderOverlayVar.empty()) {
                        const uint32_t hh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                       static_cast<uint32_t>(y)) ^ 0xB011D3u;
                        const uint32_t rr = hash32(hh);
                        const size_t idx = static_cast<size_t>(rr % static_cast<uint32_t>(boulderOverlayVar.size()));
                        otex = boulderOverlayVar[idx][static_cast<size_t>(frame % FRAMES)];
                    }
                    break;
                }
                case TileType::Fountain: {
                    if (!fountainOverlayVar.empty()) {
                        const uint32_t hh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                       static_cast<uint32_t>(y)) ^ 0xF017A1u;
                        const uint32_t rr = hash32(hh);
                        const size_t idx = static_cast<size_t>(rr % static_cast<uint32_t>(fountainOverlayVar.size()));
                        otex = fountainOverlayVar[idx][static_cast<size_t>(frame % FRAMES)];
                    }
                    break;
                }
                case TileType::Altar: {
                    if (!altarOverlayVar.empty()) {
                        const uint32_t hh = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                       static_cast<uint32_t>(y)) ^ 0xA17A12u;
                        const uint32_t rr = hash32(hh);
                        const size_t idx = static_cast<size_t>(rr % static_cast<uint32_t>(altarOverlayVar.size()));
                        otex = altarOverlayVar[idx][static_cast<size_t>(frame % FRAMES)];
                    }
                    break;
                }
                case TileType::StairsUp:
                    otex = stairsUpOverlayTex[static_cast<size_t>(frame % FRAMES)];
                    break;
                case TileType::StairsDown:
                    otex = stairsDownOverlayTex[static_cast<size_t>(frame % FRAMES)];
                    break;
                case TileType::DoorClosed:
                    otex = doorClosedOverlayTex[static_cast<size_t>(frame % FRAMES)];
                    break;
                case TileType::DoorLocked:
                    otex = doorLockedOverlayTex[static_cast<size_t>(frame % FRAMES)];
                    break;
                case TileType::DoorOpen:
                    otex = doorOpenOverlayTex[static_cast<size_t>(frame % FRAMES)];
                    break;
                default:
                    break;
            }

            if (otex) {
                Color om = modObj;

                // Subtle deterministic "glint" on special overlays (helps stairs/altars/fountains read quickly).
                // Scales with proc_palette so users have one knob for "extra rendering spice".
                if (t.visible && procPalStrength > 0.001f) {
                    const bool glintTile = (t.type == TileType::Altar) || (t.type == TileType::Fountain) ||
                                           (t.type == TileType::StairsUp) || (t.type == TileType::StairsDown);
                    if (glintTile) {
                        const uint32_t hh = hash32(hashCombine(hashCombine(lvlSeed ^ 0x61D11C7u, static_cast<uint32_t>(t.type)),
                                                              hashCombine(static_cast<uint32_t>(x), static_cast<uint32_t>(y))));
                        const float phase = static_cast<float>(hh & 0xFFFFu) * (6.2831853f / 65536.0f);
                        const float speed = 0.0045f + static_cast<float>((hh >> 16) & 0xFFu) * 0.00001f;
                        const float w = std::sin(static_cast<float>(ticks) * speed + phase);

                        // k is a tiny mix-to-white factor.
                        const float k = std::clamp(std::max(0.0f, w) * 0.10f * procPalStrength, 0.0f, 0.18f);

                        const int dr = static_cast<int>(std::lround((255.0f - static_cast<float>(om.r)) * k));
                        const int dg = static_cast<int>(std::lround((255.0f - static_cast<float>(om.g)) * k));
                        const int db = static_cast<int>(std::lround((255.0f - static_cast<float>(om.b)) * k));
                        om.r = static_cast<uint8_t>(std::clamp(static_cast<int>(om.r) + dr, 0, 255));
                        om.g = static_cast<uint8_t>(std::clamp(static_cast<int>(om.g) + dg, 0, 255));
                        om.b = static_cast<uint8_t>(std::clamp(static_cast<int>(om.b) + db, 0, 255));
                    }
                }

                SDL_SetTextureColorMod(otex, om.r, om.g, om.b);
                SDL_SetTextureAlphaMod(otex, 255);
                SDL_RenderCopy(renderer, otex, nullptr, &dst);
                SDL_SetTextureColorMod(otex, 255, 255, 255);
                SDL_SetTextureAlphaMod(otex, 255);
            }
        }


    };

    if (isoView) {
        // Painter's order for isometric tiles: back-to-front by diagonal (x+y).
        const int maxSum = (d.width - 1) + (d.height - 1);
        for (int s = 0; s <= maxSum; ++s) {
            for (int y = 0; y < d.height; ++y) {
                const int x = s - y;
                if (x < 0 || x >= d.width) continue;
                drawMapTile(x, y);
            }
        }
    } else {
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                drawMapTile(x, y);
            }
        }
    }

    // Ambient-occlusion + directional shadows are tuned for the top-down tileset.
    // For isometric mode we rely on the diamond-projected ground tiles + taller
    // wall blocks for depth/readability.
    if (!isoView) {
// Ambient-occlusion style edge shading (walls/pillars/chasm) makes rooms and corridors pop.
        {
        auto isOccluder = [&](TileType tt) -> bool {
            switch (tt) {
                case TileType::Wall:
                case TileType::DoorClosed:
                case TileType::DoorLocked:
                case TileType::DoorSecret:
                case TileType::Pillar:
                case TileType::Boulder:
                case TileType::Chasm:
                    return true;
                default:
                    return false;
            }
        };

        const int thick = std::max(1, tile / 8);

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.explored) continue;
                if (isOccluder(t.type)) continue;

                // Fade AO with visibility/light.
                const uint8_t lm = t.visible ? lightMod(x, y) : (game.darknessActive() ? 120u : 170u);
                int baseA = 38;
                baseA = (baseA * static_cast<int>(lm)) / 255;
                if (!t.visible) baseA = std::min(baseA, 26);

                const auto nType = (y > 0) ? d.at(x, y - 1).type : TileType::Wall;
                const auto sType = (y + 1 < d.height) ? d.at(x, y + 1).type : TileType::Wall;
                const auto wType = (x > 0) ? d.at(x - 1, y).type : TileType::Wall;
                const auto eType = (x + 1 < d.width) ? d.at(x + 1, y).type : TileType::Wall;

                const bool nOcc = isOccluder(nType);
                const bool sOcc = isOccluder(sType);
                const bool wOcc = isOccluder(wType);
                const bool eOcc = isOccluder(eType);

                if (!nOcc && !sOcc && !wOcc && !eOcc) continue;

                SDL_Rect dst = tileDst(x, y);

                auto drawEdge = [&](const SDL_Rect& r, int a, bool chasmEdge) {
                    if (a <= 0) return;
                    if (a > 255) a = 255;

                    // A subtle blue rim for chasms reads as "danger" without being loud.
                    if (chasmEdge) {
                        const int ga = std::max(8, a / 2);
                        SDL_SetRenderDrawColor(renderer, 40, 80, 160, static_cast<Uint8>(ga));
                        SDL_RenderFillRect(renderer, &r);
                    }

                    SDL_SetRenderDrawColor(renderer, 0, 0, 0, static_cast<Uint8>(a));
                    SDL_RenderFillRect(renderer, &r);
                };

                const int aTop = static_cast<int>(baseA * 0.82f);
                const int aLeft = static_cast<int>(baseA * 0.82f);
                const int aBot = std::min(255, baseA + 10);
                const int aRight = std::min(255, baseA + 10);

                if (nOcc) drawEdge(SDL_Rect{ dst.x, dst.y, dst.w, thick }, aTop, nType == TileType::Chasm);
                if (wOcc) drawEdge(SDL_Rect{ dst.x, dst.y, thick, dst.h }, aLeft, wType == TileType::Chasm);
                if (sOcc) drawEdge(SDL_Rect{ dst.x, dst.y + dst.h - thick, dst.w, thick }, aBot, sType == TileType::Chasm);
                if (eOcc) drawEdge(SDL_Rect{ dst.x + dst.w - thick, dst.y, thick, dst.h }, aRight, eType == TileType::Chasm);

                // Darken corners a touch so diagonal contacts don't feel "open".
                if (nOcc && wOcc) drawEdge(SDL_Rect{ dst.x, dst.y, thick, thick }, baseA, (nType == TileType::Chasm) || (wType == TileType::Chasm));
                if (nOcc && eOcc) drawEdge(SDL_Rect{ dst.x + dst.w - thick, dst.y, thick, thick }, baseA, (nType == TileType::Chasm) || (eType == TileType::Chasm));
                if (sOcc && wOcc) drawEdge(SDL_Rect{ dst.x, dst.y + dst.h - thick, thick, thick }, baseA + 6, (sType == TileType::Chasm) || (wType == TileType::Chasm));
                if (sOcc && eOcc) drawEdge(SDL_Rect{ dst.x + dst.w - thick, dst.y + dst.h - thick, thick, thick }, baseA + 6, (sType == TileType::Chasm) || (eType == TileType::Chasm));
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }


    // Directional occluder shadows: adds a subtle sense of "height" for walls/pillars/closed doors
    // without requiring any new tile art. This pass is intentionally very light.
    {
        auto isTall = [&](TileType tt) -> bool {
            switch (tt) {
                case TileType::Wall:
                case TileType::Pillar:
                case TileType::Boulder:
                case TileType::DoorClosed:
                case TileType::DoorLocked:
                case TileType::DoorSecret:
                    return true;
                default:
                    return false;
            }
        };
        auto receives = [&](TileType tt) -> bool {
            switch (tt) {
                case TileType::Floor:
                case TileType::DoorOpen:
                case TileType::StairsUp:
                case TileType::StairsDown:
                case TileType::Chasm:
                    return true;
                default:
                    return false;
            }
        };

        const int grad = std::max(2, tile / 4);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        auto castShadow = [&](int tx, int ty, int baseA) {
            if (!d.inBounds(tx, ty)) return;
            const Tile& rt = d.at(tx, ty);
            if (!rt.explored) return;
            if (!receives(rt.type)) return;

            // Fade the shadow in darkness / memory.
            const uint8_t lm = rt.visible ? lightMod(tx, ty) : (game.darknessActive() ? 110u : 160u);
            int a = (baseA * static_cast<int>(lm)) / 255;
            a = std::clamp(a, 0, 110);
            if (a <= 0) return;

            SDL_Rect base = tileDst(tx, ty);
            // Draw a top-to-bottom gradient strip at the top of the receiving tile.
            for (int i = 0; i < grad; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(std::max(1, grad - 1));
                const int ai = static_cast<int>(std::round(static_cast<float>(a) * (1.0f - t)));
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, static_cast<uint8_t>(std::clamp(ai, 0, 255)));
                SDL_Rect r{ base.x, base.y + i, base.w, 1 };
                SDL_RenderFillRect(renderer, &r);
            }
        };

        // Assume a gentle ambient light direction from top-left => shadows fall down/right.
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.explored) continue;
                if (!isTall(t.type)) continue;

                // Don't over-darken in the explored-but-not-visible memory view.
                const int baseA = t.visible ? 54 : 34;

                castShadow(x, y + 1, baseA);
                // A slightly weaker diagonal shadow helps break the grid feel.
                castShadow(x + 1, y + 1, baseA / 2);
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }

    }



    // Auto-move path overlay
    if (game.isAutoActive()) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        const bool exploring = game.isAutoExploring();
        const Uint8 cr = 80;
        const Uint8 cg = exploring ? Uint8{220} : Uint8{170};
        const Uint8 cb = exploring ? Uint8{140} : Uint8{255};

        // In isometric view, render a more legible path: a faint polyline plus
        // diamond pips that match the tile projection.
        if (isoView) {
            std::vector<Vec2i> tiles;
            tiles.reserve(game.autoPath().size());

            std::vector<SDL_Point> pts;
            pts.reserve(game.autoPath().size());

            for (const Vec2i& p : game.autoPath()) {
                if (!d.inBounds(p.x, p.y)) continue;
                const Tile& t = d.at(p.x, p.y);
                if (!t.explored) continue;

                tiles.push_back(p);
                const SDL_Rect base = mapTileDst(p.x, p.y);
                pts.push_back(SDL_Point{ base.x + base.w / 2, base.y + base.h / 2 });
            }

            // Faint connecting line to make long paths readable at a glance.
            SDL_SetRenderDrawColor(renderer, cr, cg, cb, Uint8{55});
            for (size_t i = 1; i < pts.size(); ++i) {
                SDL_RenderDrawLine(renderer, pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y);
            }

            // Per-tile pips (small diamonds).
            SDL_SetRenderDrawColor(renderer, cr, cg, cb, Uint8{90});
            for (const Vec2i& p : tiles) {
                const SDL_Rect base = mapTileDst(p.x, p.y);
                const int cx = base.x + base.w / 2;
                const int cy = base.y + base.h / 2;

                const int hw = std::max(1, base.w / 10);
                const int hh = std::max(1, base.h / 5);
                fillIsoDiamond(renderer, cx, cy, hw, hh);
            }

            // Destination accent.
            if (!tiles.empty()) {
                const Vec2i& end = tiles.back();
                SDL_Rect endRect = mapTileDst(end.x, end.y);
                SDL_SetRenderDrawColor(renderer, cr, cg, cb, Uint8{180});
                drawIsoDiamondOutline(renderer, endRect);
            }
        } else {
            SDL_SetRenderDrawColor(renderer, cr, cg, cb, Uint8{90});

            for (const Vec2i& p : game.autoPath()) {
                if (!d.inBounds(p.x, p.y)) continue;
                const Tile& t = d.at(p.x, p.y);
                if (!t.explored) continue;

                SDL_Rect base = tileDst(p.x, p.y);
                SDL_Rect r{ base.x + base.w / 3, base.y + base.h / 3, base.w / 3, base.h / 3 };
                SDL_RenderFillRect(renderer, &r);
            }

            if (!game.autoPath().empty()) {
                const Vec2i& end = game.autoPath().back();
                if (d.inBounds(end.x, end.y) && d.at(end.x, end.y).explored) {
                    SDL_Rect endRect = tileDst(end.x, end.y);
                    SDL_SetRenderDrawColor(renderer, cr, cg, cb, Uint8{180});
                    SDL_RenderDrawRect(renderer, &endRect);
                }
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    // Draw items (visible only)
    if (isoView) {
        // Sort by isometric draw order so items layer nicely.
        std::vector<const GroundItem*> draw;
        draw.reserve(game.groundItems().size());
        for (const auto& gi : game.groundItems()) {
            if (!d.inBounds(gi.pos.x, gi.pos.y)) continue;
            const Tile& t = d.at(gi.pos.x, gi.pos.y);
            if (!t.visible) continue;
            draw.push_back(&gi);
        }

        std::sort(draw.begin(), draw.end(), [](const GroundItem* a, const GroundItem* b) {
            const int sa = a->pos.x + a->pos.y;
            const int sb = b->pos.x + b->pos.y;
            if (sa != sb) return sa < sb;
            if (a->pos.y != b->pos.y) return a->pos.y < b->pos.y;
            return a->pos.x < b->pos.x;
        });

        for (const GroundItem* gip : draw) {
            const GroundItem& gi = *gip;

            Item visIt = gi.item;
            if (isHallucinating(game)) {
                visIt.kind = hallucinatedItemKind(game, gi.item);
            }

            applyIdentificationVisuals(game, visIt);

            SDL_Texture* tex = itemTexture(visIt, frame + gi.item.id);
            if (!tex) continue;

            SDL_Rect base = spriteDst(gi.pos.x, gi.pos.y);
            SDL_Rect dst = base;
            const float bob = itemBob(gi);
            dst.y -= static_cast<int>(std::lround(bob));

            const Color mod = tileColorMod(gi.pos.x, gi.pos.y, /*visible*/true);

            // Small ground shadow to anchor floating items in isometric view.
            {
                SDL_Texture* sh = isoEntityShadowTex[static_cast<size_t>(frame % FRAMES)];
                if (sh) {
                    const SDL_Rect tileBase = mapTileDst(gi.pos.x, gi.pos.y);
                    const int cx = base.x + base.w / 2;
                    const int cy = base.y + base.h;

                    SDL_Rect sd = tileBase;
                    sd.w = (tileBase.w * 2) / 3;
                    sd.h = (tileBase.h * 2) / 3;

                    // Shrink a little as the item rises.
                    const float amp = std::clamp(static_cast<float>(tile) * 0.035f, 0.0f, 3.0f);
                    const float lift01 = (amp > 0.0f && bob > 0.0f) ? std::clamp(bob / amp, 0.0f, 1.0f) : 0.0f;
                    const float sc = 1.0f - 0.10f * lift01;
                    sd.w = std::max(1, static_cast<int>(std::lround(static_cast<float>(sd.w) * sc)));
                    sd.h = std::max(1, static_cast<int>(std::lround(static_cast<float>(sd.h) * sc)));

                    sd.x = cx - sd.w / 2;
                    sd.y = cy - sd.h / 2;

                    const int lum = (static_cast<int>(mod.r) + static_cast<int>(mod.g) + static_cast<int>(mod.b)) / 3;
                    int a = (lum * 90) / 255;
                    a = static_cast<int>(std::lround(static_cast<float>(a) * (1.0f - 0.30f * lift01)));
                    a = std::clamp(a, 12, 90);

                    SDL_SetTextureColorMod(sh, 0, 0, 0);
                    SDL_SetTextureAlphaMod(sh, static_cast<Uint8>(a));
                    SDL_RenderCopy(renderer, sh, nullptr, &sd);
                    SDL_SetTextureColorMod(sh, 255, 255, 255);
                    SDL_SetTextureAlphaMod(sh, 255);
                }
            }

            drawSpriteWithShadowOutline(renderer, tex, dst, mod, 255, /*shadow*/false, /*outline*/true);
        }
    } else {
        for (const auto& gi : game.groundItems()) {
            if (!d.inBounds(gi.pos.x, gi.pos.y)) continue;
            const Tile& t = d.at(gi.pos.x, gi.pos.y);
            if (!t.visible) continue;

            Item visIt = gi.item;
            if (isHallucinating(game)) {
                visIt.kind = hallucinatedItemKind(game, gi.item);
            }

            applyIdentificationVisuals(game, visIt);

            SDL_Texture* tex = itemTexture(visIt, frame + gi.item.id);
            if (!tex) continue;

            SDL_Rect dst = spriteDst(gi.pos.x, gi.pos.y);
            const float bob = itemBob(gi);
            dst.y -= static_cast<int>(std::lround(bob));
            const Color mod = tileColorMod(gi.pos.x, gi.pos.y, /*visible*/true);
            drawSpriteWithShadowOutline(renderer, tex, dst, mod, 255, /*shadow*/false, /*outline*/true);
        }
    }




    // Draw confusion gas (visible tiles only). This is a persistent, tile-based field
    // spawned by Confusion Gas traps.
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        const bool haveGasTex = isoView ? (gasVarIso[0][0] != nullptr) : (gasVar[0][0] != nullptr);

        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.visible) continue;

                const uint8_t g = game.confusionGasAt(x, y);
                if (g == 0u) continue;

                const uint8_t m = lightMod(x, y);

                // Scale intensity by light; keep a minimum so it reads even in deep shadow.
                int a = 70 + static_cast<int>(g) * 12;
                a = (a * static_cast<int>(m)) / 255;
                a = std::max(24, std::min(230, a));

                // Slight shimmer so the cloud feels alive (deterministic per tile/frame).
                a = std::max(24, std::min(240, a + (static_cast<int>((frame + x * 3 + y * 7) % 9) - 4)));

                SDL_Rect r = tileDst(x, y);

                if (haveGasTex) {
                    const uint32_t h = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0x6A5u;
                    const size_t vi = static_cast<size_t>(hash32(h) % static_cast<uint32_t>(GAS_VARS));

                    // Smooth, desynced per-tile sampling so large gas fields don't "blink" in lockstep.
                    const FrameBlend fb = sampleFrameBlend(180u, h ^ 0x51A11u);
                    const uint8_t w1 = fb.w1;
                    const uint8_t w0 = static_cast<uint8_t>(255u - w1);

                    const bool useIso = isoView && (gasVarIso[0][0] != nullptr);
                    const auto* gset = useIso ? &gasVarIso : &gasVar;

                    SDL_Texture* g0 = (*gset)[vi][static_cast<size_t>(fb.f0)];
                    SDL_Texture* g1 = (*gset)[vi][static_cast<size_t>(fb.f1)];

                    if (g0 || g1) {
                        // Multiply a "signature" purple by the tile lighting/tint so it feels embedded in the world.
                        const Color lmod = tileColorMod(x, y, /*visible=*/true);
                        const Color base{200, 120, 255, 255};

                        const uint8_t mr = static_cast<uint8_t>((static_cast<int>(base.r) * lmod.r) / 255);
                        const uint8_t mg = static_cast<uint8_t>((static_cast<int>(base.g) * lmod.g) / 255);
                        const uint8_t mb = static_cast<uint8_t>((static_cast<int>(base.b) * lmod.b) / 255);

                        auto drawOne = [&](SDL_Texture* tex, uint8_t alpha) {
                            if (!tex || alpha == 0u) return;
                            SDL_SetTextureColorMod(tex, mr, mg, mb);
                            SDL_SetTextureAlphaMod(tex, alpha);
                            SDL_RenderCopy(renderer, tex, nullptr, &r);
                            SDL_SetTextureColorMod(tex, 255, 255, 255);
                            SDL_SetTextureAlphaMod(tex, 255);
                        };

                        const int a0i = (a * static_cast<int>(w0)) / 255;
                        const int a1i = (a * static_cast<int>(w1)) / 255;
                        const uint8_t a0 = static_cast<uint8_t>(std::clamp(a0i, 0, 255));
                        const uint8_t a1 = static_cast<uint8_t>(std::clamp(a1i, 0, 255));

                        drawOne(g0, a0);
                        drawOne(g1, a1);
                        continue;
                    }
                }

                // Fallback: simple tinted quad (should rarely be used).
                SDL_SetRenderDrawColor(renderer, 190, 90, 255, static_cast<uint8_t>(a));
                SDL_RenderFillRect(renderer, &r);
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }


    // Draw poison gas (visible tiles only). This is a persistent, tile-based hazard
    // spawned by Poison Gas traps.
    {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        const bool haveGasTex = isoView ? (gasVarIso[0][0] != nullptr) : (gasVar[0][0] != nullptr);

        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.visible) continue;

                const uint8_t g = game.poisonGasAt(x, y);
                if (g == 0u) continue;

                const uint8_t m = lightMod(x, y);

                // Scale intensity by light; keep a minimum so it reads even in deep shadow.
                int a = 80 + static_cast<int>(g) * 14;
                a = (a * static_cast<int>(m)) / 255;
                a = std::max(30, std::min(235, a));

                // Slight shimmer so the cloud feels alive (deterministic per tile/frame).
                a = std::max(30, std::min(245, a + (static_cast<int>((frame + x * 5 + y * 11) % 9) - 4)));

                SDL_Rect r = tileDst(x, y);

                if (haveGasTex) {
                    const uint32_t h = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0xC41u;
                    const size_t vi = static_cast<size_t>(hash32(h) % static_cast<uint32_t>(GAS_VARS));

                    // Smooth, desynced per-tile sampling so large gas fields don't "blink" in lockstep.
                    const FrameBlend fb = sampleFrameBlend(200u, h ^ 0xBADC0DEu);
                    const uint8_t w1 = fb.w1;
                    const uint8_t w0 = static_cast<uint8_t>(255u - w1);

                    const bool useIso = isoView && (gasVarIso[0][0] != nullptr);
                    const auto* gset = useIso ? &gasVarIso : &gasVar;

                    SDL_Texture* g0 = (*gset)[vi][static_cast<size_t>(fb.f0)];
                    SDL_Texture* g1 = (*gset)[vi][static_cast<size_t>(fb.f1)];

                    if (g0 || g1) {
                        // Multiply a "signature" green by the tile lighting/tint so it feels embedded in the world.
                        const Color lmod = tileColorMod(x, y, /*visible=*/true);
                        const Color base{120, 255, 120, 255};

                        const uint8_t mr = static_cast<uint8_t>((static_cast<int>(base.r) * lmod.r) / 255);
                        const uint8_t mg = static_cast<uint8_t>((static_cast<int>(base.g) * lmod.g) / 255);
                        const uint8_t mb = static_cast<uint8_t>((static_cast<int>(base.b) * lmod.b) / 255);

                        auto drawOne = [&](SDL_Texture* tex, uint8_t alpha) {
                            if (!tex || alpha == 0u) return;
                            SDL_SetTextureColorMod(tex, mr, mg, mb);
                            SDL_SetTextureAlphaMod(tex, alpha);
                            SDL_RenderCopy(renderer, tex, nullptr, &r);
                            SDL_SetTextureColorMod(tex, 255, 255, 255);
                            SDL_SetTextureAlphaMod(tex, 255);
                        };

                        const int a0i = (a * static_cast<int>(w0)) / 255;
                        const int a1i = (a * static_cast<int>(w1)) / 255;
                        const uint8_t a0 = static_cast<uint8_t>(std::clamp(a0i, 0, 255));
                        const uint8_t a1 = static_cast<uint8_t>(std::clamp(a1i, 0, 255));

                        drawOne(g0, a0);
                        drawOne(g1, a1);
                        continue;
                    }
                }

                // Fallback: simple tinted quad (should rarely be used).
                SDL_SetRenderDrawColor(renderer, 90, 220, 90, static_cast<uint8_t>(a));
                SDL_RenderFillRect(renderer, &r);
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }


    // Draw fire field (visible tiles only). This is a persistent, tile-based hazard
    // spawned primarily by Fireball explosions.
    {
        // Additive blend gives a nice glow without completely obscuring tiles.
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);

        const bool haveFireTex = isoView ? (fireVarIso[0][0] != nullptr) : (fireVar[0][0] != nullptr);

        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.visible) continue;

                const uint8_t f = game.fireAt(x, y);
                if (f == 0u) continue;

                const uint8_t m = lightMod(x, y);

                // Scale intensity by light; keep a minimum so it reads even in deep shadow.
                int a = 40 + static_cast<int>(f) * 22;
                a = (a * static_cast<int>(m)) / 255;
                a = std::max(28, std::min(235, a));

                // Flicker
                a = std::max(24, std::min(245, a + (static_cast<int>((frame + x * 5 + y * 11) % 7) - 3)));

                SDL_Rect r = tileDst(x, y);

                if (haveFireTex) {
                    const uint32_t h = hashCombine(hashCombine(lvlSeed, static_cast<uint32_t>(x)),
                                                   static_cast<uint32_t>(y)) ^ 0xF17Eu;
                    const size_t vi = static_cast<size_t>(hash32(h) % static_cast<uint32_t>(FIRE_VARS));

                    // Fire wants to feel "alive": faster sampling + per-tile phase offset.
                    const FrameBlend fb = sampleFrameBlend(130u, h ^ 0xF17ECAFEu);
                    const uint8_t w1 = fb.w1;
                    const uint8_t w0 = static_cast<uint8_t>(255u - w1);

                    const bool useIso = isoView && (fireVarIso[0][0] != nullptr);
                    const auto* fset = useIso ? &fireVarIso : &fireVar;

                    SDL_Texture* f0 = (*fset)[vi][static_cast<size_t>(fb.f0)];
                    SDL_Texture* f1 = (*fset)[vi][static_cast<size_t>(fb.f1)];

                    if (f0 || f1) {
                        // Warm fire tint, modulated by world lighting.
                        const Color lmod = tileColorMod(x, y, /*visible=*/true);
                        const Color base{255, 160, 80, 255};

                        const uint8_t mr = static_cast<uint8_t>((static_cast<int>(base.r) * lmod.r) / 255);
                        const uint8_t mg = static_cast<uint8_t>((static_cast<int>(base.g) * lmod.g) / 255);
                        const uint8_t mb = static_cast<uint8_t>((static_cast<int>(base.b) * lmod.b) / 255);

                        auto drawOne = [&](SDL_Texture* tex, uint8_t alpha) {
                            if (!tex || alpha == 0u) return;
                            SDL_SetTextureColorMod(tex, mr, mg, mb);
                            SDL_SetTextureAlphaMod(tex, alpha);
                            SDL_RenderCopy(renderer, tex, nullptr, &r);
                            SDL_SetTextureColorMod(tex, 255, 255, 255);
                            SDL_SetTextureAlphaMod(tex, 255);
                        };

                        const int a0i = (a * static_cast<int>(w0)) / 255;
                        const int a1i = (a * static_cast<int>(w1)) / 255;
                        const uint8_t a0 = static_cast<uint8_t>(std::clamp(a0i, 0, 255));
                        const uint8_t a1 = static_cast<uint8_t>(std::clamp(a1i, 0, 255));

                        drawOne(f0, a0);
                        drawOne(f1, a1);
                        continue;
                    }
                }

                // Fallback: simple tinted quad (should rarely be used).
                SDL_SetRenderDrawColor(renderer, 255, 140, 70, static_cast<uint8_t>(a));
                SDL_RenderFillRect(renderer, &r);
            }
        }

        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }


    // Draw discovered traps (shown on explored tiles; bright when visible, dim when remembered)
    for (const auto& tr : game.traps()) {
        if (!tr.discovered) continue;
        if (!d.inBounds(tr.pos.x, tr.pos.y)) continue;
        const Tile& t = d.at(tr.pos.x, tr.pos.y);
        if (!t.explored) continue;

        uint8_t r = 220, g = 80, b = 80;
        switch (tr.kind) {
            case TrapKind::Spike:     r = 220; g = 80;  b = 80;  break;
            case TrapKind::PoisonDart:r = 80;  g = 220; b = 80;  break;
            case TrapKind::Teleport: r = 170; g = 110; b = 230; break;
            case TrapKind::Alarm:    r = 220; g = 220; b = 80;  break;
            case TrapKind::Web:       r = 140; g = 180; b = 255; break;
            case TrapKind::ConfusionGas: r = 200; g = 120; b = 255; break;
            case TrapKind::PoisonGas: r = 90; g = 220; b = 90; break;
            case TrapKind::RollingBoulder: r = 200; g = 170; b = 90; break;
            case TrapKind::TrapDoor: r = 180; g = 130; b = 90; break;
            case TrapKind::LetheMist: r = 160; g = 160; b = 210; break;
        }

        const uint8_t a = t.visible ? 220 : 120;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, r, g, b, a);

        SDL_Rect base = mapTileDst(tr.pos.x, tr.pos.y);
        if (isoView) {
            // In isometric mode, match the projection: a diamond outline + cross.
            drawIsoDiamondOutline(renderer, base);
            SDL_SetRenderDrawColor(renderer, r, g, b, static_cast<uint8_t>(std::max(40, a / 2)));
            drawIsoDiamondCross(renderer, base);
            SDL_SetRenderDrawColor(renderer, r, g, b, a);
            SDL_RenderDrawPoint(renderer, base.x + base.w / 2, base.y + base.h / 2);
        } else {
            const int x0 = base.x;
            const int y0 = base.y;
            const int x1 = x0 + base.w - 5;
            const int y1 = y0 + base.h - 5;
            SDL_RenderDrawLine(renderer, x0 + 4, y0 + 4, x1, y1);
            SDL_RenderDrawLine(renderer, x1, y0 + 4, x0 + 4, y1);
            SDL_RenderDrawPoint(renderer, x0 + base.w / 2, y0 + base.h / 2);
        }
    }

    // Draw player map markers / notes (shown on explored tiles; subtle indicator).
    for (const auto& m : game.mapMarkers()) {
        if (!d.inBounds(m.pos.x, m.pos.y)) continue;
        const Tile& t = d.at(m.pos.x, m.pos.y);
        if (!t.explored) continue;
        if (!mapTileInView(m.pos.x, m.pos.y)) continue;

        uint8_t r = 220, g = 220, b = 220;
        switch (m.kind) {
            case MarkerKind::Danger: r = 230; g = 80;  b = 80;  break;
            case MarkerKind::Loot:   r = 235; g = 200; b = 80;  break;
            case MarkerKind::Note:
            default:                 r = 220; g = 220; b = 220; break;
        }

        const uint8_t a = t.visible ? 220 : 120;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, r, g, b, a);

        SDL_Rect base = mapTileDst(m.pos.x, m.pos.y);
        const int s = (m.kind == MarkerKind::Danger) ? 6 : 4;

        if (isoView) {
            // Pin the marker to the isometric diamond corner so it reads as part of the tile.
            SDL_Point top{}, right{}, bottom{}, left{};
            isoDiamondCorners(base, top, right, bottom, left);

            const int hw = std::max(1, s / 2);
            const int hh = std::max(1, hw / 2);

            // Slight inset so the pip doesn't clip at the map edge.
            const int cx = right.x - hw - 1;
            const int cy = right.y;
            fillIsoDiamond(renderer, cx, cy, hw, hh);
        } else {
            SDL_Rect pip{ base.x + base.w - s - 2, base.y + 2, s, s };
            SDL_RenderFillRect(renderer, &pip);
        }
    }

    // Draw entities (only if their tile is visible; player always visible)
    if (isoView) {
        // Sort entities for isometric painter's algorithm (back-to-front).
        std::vector<const Entity*> draw;
        draw.reserve(game.entities().size());
        for (const auto& e : game.entities()) {
            if (!d.inBounds(e.pos.x, e.pos.y)) continue;
            const bool show = (e.id == game.playerId()) || d.at(e.pos.x, e.pos.y).visible;
            if (!show) continue;
            draw.push_back(&e);
        }

        const int playerId = game.playerId();
        std::sort(draw.begin(), draw.end(), [&](const Entity* a, const Entity* b) {
            const bool aIsPlayer = (a->id == playerId);
            const bool bIsPlayer = (b->id == playerId);

            // Player last so they don't get hidden behind other sprites.
            if (aIsPlayer != bIsPlayer) return !aIsPlayer && bIsPlayer;

            const int sa = a->pos.x + a->pos.y;
            const int sb = b->pos.x + b->pos.y;
            if (sa != sb) return sa < sb;
            if (a->pos.y != b->pos.y) return a->pos.y < b->pos.y;
            if (a->pos.x != b->pos.x) return a->pos.x < b->pos.x;
            return a->id < b->id;
        });

        for (const Entity* ep : draw) {
            const Entity& e = *ep;
            const bool isPlayer = (e.id == playerId);

            Entity visE = e;
            if (isHallucinating(game)) {
                visE.kind = hallucinatedEntityKind(game, e);
            }

            SDL_Texture* tex = entityTexture(visE, (frame + e.id) % FRAMES);
            if (!tex) continue;

            AnimSample anim = sampleEntityAnim(e);
            SDL_Rect dst = anim.dst;
            const bool tileVis = isPlayer ? true : d.at(e.pos.x, e.pos.y).visible;
            const Color mod = tileColorMod(e.pos.x, e.pos.y, tileVis);
            // In isometric view, draw a dedicated ground-plane shadow diamond under the sprite.
            // This anchors the entity to the tile more convincingly than a simple screen-space offset shadow.
            {
                SDL_Texture* sh = isoEntityShadowTex[static_cast<size_t>(frame % FRAMES)];
                if (sh) {
                    const SDL_Rect base = mapTileDst(e.pos.x, e.pos.y);

                    // Use the procedural animation's ground foot point so the shadow tracks smooth movement.
                    const int cx = anim.footX;
                    const int cy = anim.footY;

                    SDL_Rect sd = base;
                    sd.w = (base.w * 3) / 4;
                    sd.h = (base.h * 3) / 4;

                    // Shrink + fade a bit as the entity hops upward.
                    const float sc = 1.0f - 0.12f * anim.lift01;
                    sd.w = std::max(1, static_cast<int>(std::lround(static_cast<float>(sd.w) * sc)));
                    sd.h = std::max(1, static_cast<int>(std::lround(static_cast<float>(sd.h) * sc)));

                    sd.x = cx - sd.w / 2;
                    sd.y = cy - sd.h / 2;

                    const int lum = (static_cast<int>(mod.r) + static_cast<int>(mod.g) + static_cast<int>(mod.b)) / 3;
                    int a = (lum * 140) / 255;
                    a = std::clamp(a, 18, 140);
                    a = static_cast<int>(std::lround(static_cast<float>(a) * (1.0f - 0.35f * anim.lift01)));
                    a = std::clamp(a, 10, 140);

                    SDL_SetTextureColorMod(sh, 0, 0, 0);
                    SDL_SetTextureAlphaMod(sh, static_cast<Uint8>(a));
                    SDL_RenderCopy(renderer, sh, nullptr, &sd);
                    SDL_SetTextureColorMod(sh, 255, 255, 255);
                    SDL_SetTextureAlphaMod(sh, 255);
                }
            }

            drawSpriteWithShadowOutline(renderer, tex, dst, mod, 255, /*shadow*/false, /*outline*/true);

            // Small HP pip for monsters
            if (!isPlayer && e.hp > 0) {
                SDL_Rect bar{ dst.x + 2, dst.y + 2, std::max(1, (tile - 4) * e.hp / std::max(1, e.hpMax)), 4 };
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

                Uint8 br = 200, bg = 40, bb = 40, ba = 160;
                if (!isHallucinating(game) && e.procRank != ProcMonsterRank::Normal) {
                    switch (e.procRank) {
                        case ProcMonsterRank::Elite:    br = 210; bg = 170; bb = 70; ba = 200; break;
                        case ProcMonsterRank::Champion: br = 90;  bg = 160; bb = 230; ba = 200; break;
                        case ProcMonsterRank::Mythic:   br = 200; bg = 90;  bb = 230; ba = 210; break;
                        default: break;
                    }
                }

                SDL_SetRenderDrawColor(renderer, br, bg, bb, ba);
                SDL_RenderFillRect(renderer, &bar);
            }
        }
    } else {
        for (const auto& e : game.entities()) {
            if (!d.inBounds(e.pos.x, e.pos.y)) continue;

            bool show = (e.id == game.playerId()) || d.at(e.pos.x, e.pos.y).visible;
            if (!show) continue;

            Entity visE = e;
            if (isHallucinating(game)) {
                visE.kind = hallucinatedEntityKind(game, e);
            }

            SDL_Texture* tex = entityTexture(visE, (frame + e.id) % FRAMES);
            if (!tex) continue;

            AnimSample anim = sampleEntityAnim(e);
            SDL_Rect dst = anim.dst;
            const bool tileVis = (e.id == game.playerId()) ? true : d.at(e.pos.x, e.pos.y).visible;
            const Color mod = tileColorMod(e.pos.x, e.pos.y, tileVis);
            drawSpriteWithShadowOutline(renderer, tex, dst, mod, 255, /*shadow*/true, /*outline*/true);

            // Small HP pip for monsters
            if (e.id != game.playerId() && e.hp > 0) {
                SDL_Rect bar{ dst.x + 2, dst.y + 2, std::max(1, (tile - 4) * e.hp / std::max(1, e.hpMax)), 4 };
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

                Uint8 br = 200, bg = 40, bb = 40, ba = 160;
                if (!isHallucinating(game) && e.procRank != ProcMonsterRank::Normal) {
                    switch (e.procRank) {
                        case ProcMonsterRank::Elite:    br = 210; bg = 170; bb = 70; ba = 200; break;
                        case ProcMonsterRank::Champion: br = 90;  bg = 160; bb = 230; ba = 200; break;
                        case ProcMonsterRank::Mythic:   br = 200; bg = 90;  bb = 230; ba = 210; break;
                        default: break;
                    }
                }

                SDL_SetRenderDrawColor(renderer, br, bg, bb, ba);
                SDL_RenderFillRect(renderer, &bar);
            }
        }
    }

    // Hallucination "phantoms": purely visual, fake monsters that appear on
    // empty visible tiles while the player is hallucinating.
    //
    // These are intentionally derived from stable hashes of (seed, phase, tile)
    // so that they do NOT consume RNG state and remain compatible with
    // replay/state-hash verification.
    if (isHallucinating(game)) {
        const int w = d.width;
        const int h = d.height;
        if (w > 0 && h > 0) {
            // Occupancy map so we don't spawn phantoms on top of real entities/items.
            std::vector<uint8_t> occ;
            occ.assign(static_cast<size_t>(w * h), 0u);
            auto idx = [&](int x, int y) -> size_t {
                return static_cast<size_t>(y * w + x);
            };

            for (const auto& e : game.entities()) {
                if (!d.inBounds(e.pos.x, e.pos.y)) continue;
                occ[idx(e.pos.x, e.pos.y)] = 1u;
            }
            for (const auto& gi : game.groundItems()) {
                if (!d.inBounds(gi.pos.x, gi.pos.y)) continue;
                occ[idx(gi.pos.x, gi.pos.y)] |= 2u;
            }

            struct Phantom {
                Vec2i pos;
                EntityKind kind;
                uint32_t seed;
                uint32_t h;
            };

            // Keep the number of phantoms low to avoid overwhelming the player
            // and to keep sprite cache churn under control.
            const int maxPhantoms = 12;
            constexpr uint32_t kCount = static_cast<uint32_t>(ENTITY_KIND_COUNT);
            static_assert(kCount > 1u, "ENTITY_KIND_COUNT must include Player plus at least one monster kind.");

            std::vector<Phantom> ph;
            ph.reserve(static_cast<size_t>(maxPhantoms));

            const uint32_t phase = hallucinationPhase(game);
            const uint32_t base = hashCombine(game.seed() ^ 0xF00DFACEu, phase);

            // Keep phantoms grounded in places that are normally passable and
            // unambiguous to read: avoid spawning them on stairs/doors.
            auto phantomAllowedTile = [](TileType tt) -> bool {
                return tt == TileType::Floor || tt == TileType::DoorOpen;
            };

            // Sample tiles in scanline order but gate via a hash so the
            // distribution feels "random" and deterministic.
            for (int y = 0; y < h && static_cast<int>(ph.size()) < maxPhantoms; ++y) {
                for (int x = 0; x < w && static_cast<int>(ph.size()) < maxPhantoms; ++x) {
                    if (!mapTileInView(x, y)) continue;
                    const Tile& t = d.at(x, y);
                    if (!t.visible) continue;
                    if (!phantomAllowedTile(t.type)) continue;
                    if (occ[idx(x, y)] != 0u) continue;
                    if (x == game.player().pos.x && y == game.player().pos.y) continue;

                    // Roughly ~2% chance per visible tile, then capped by maxPhantoms.
                    const uint32_t h0 = hashCombine(base, static_cast<uint32_t>(x) ^ (static_cast<uint32_t>(y) << 16));
                    const uint32_t r = hash32(h0);
                    if ((r % 1000u) >= 20u) continue;

                    const uint32_t kk = 1u + (hash32(r ^ 0x9E3779B9u) % (kCount - 1u));

                    Phantom p;
                    p.pos = {x, y};
                    p.kind = static_cast<EntityKind>(kk);
                    p.seed = hash32(r ^ 0xA53A9u);
                    p.h = r;
                    ph.push_back(p);
                }
            }

            if (!ph.empty()) {
                // For isometric view, draw in painter order so they sit nicely in the world.
                if (isoView) {
                    std::sort(ph.begin(), ph.end(), [](const Phantom& a, const Phantom& b) {
                        const int sa = a.pos.x + a.pos.y;
                        const int sb = b.pos.x + b.pos.y;
                        if (sa != sb) return sa < sb;
                        if (a.pos.y != b.pos.y) return a.pos.y < b.pos.y;
                        return a.pos.x < b.pos.x;
                    });
                }

                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                for (const Phantom& p : ph) {
                    Entity e{};
                    e.kind = p.kind;
                    e.spriteSeed = p.seed;
                    e.pos = p.pos;

                    SDL_Texture* tex = entityTexture(e, (frame + static_cast<int>(p.seed & 3u)) % FRAMES);
                    if (!tex) continue;

                    SDL_Rect dst = spriteDst(p.pos.x, p.pos.y);

                    // Subtle jitter so the phantoms feel unstable.
                    const int jx = ((static_cast<int>(hash32(p.h ^ static_cast<uint32_t>(frame))) & 1) ? 1 : -1);
                    const int jy = ((static_cast<int>(hash32(p.h ^ static_cast<uint32_t>(frame + 17))) & 1) ? 1 : -1);
                    if ((frame & 1) != 0) {
                        dst.x += jx;
                        dst.y += jy;
                    }

                    const Color mod = tileColorMod(p.pos.x, p.pos.y, /*visible*/true);

                    // Flickering alpha in a readable range.
                    const uint8_t a = static_cast<uint8_t>(std::clamp<int>(110 + static_cast<int>(hash32(p.h ^ static_cast<uint32_t>(frame * 31))) % 120, 60, 210));
                    if (isoView) {
                        // Keep hallucination phantoms grounded in isometric mode with the same
                        // diamond ground shadow used by real entities.
                        SDL_Texture* sh = isoEntityShadowTex[static_cast<size_t>(frame % FRAMES)];
                        if (sh) {
                            const SDL_Rect baseRect = mapTileDst(p.pos.x, p.pos.y);
                            const int cx = baseRect.x + baseRect.w / 2;
                            const int cy = baseRect.y + baseRect.h / 2 + (baseRect.h / 4);

                            SDL_Rect sd = baseRect;
                            sd.w = (baseRect.w * 3) / 4;
                            sd.h = (baseRect.h * 3) / 4;
                            sd.x = cx - sd.w / 2;
                            sd.y = cy - sd.h / 2;

                            const int lum = (static_cast<int>(mod.r) + static_cast<int>(mod.g) + static_cast<int>(mod.b)) / 3;
                            int sa = (lum * 120) / 255;
                            sa = std::min<int>(sa, static_cast<int>(a));
                            sa = std::clamp(sa, 10, 130);

                            SDL_SetTextureColorMod(sh, 0, 0, 0);
                            SDL_SetTextureAlphaMod(sh, static_cast<Uint8>(sa));
                            SDL_RenderCopy(renderer, sh, nullptr, &sd);
                            SDL_SetTextureColorMod(sh, 255, 255, 255);
                            SDL_SetTextureAlphaMod(sh, 255);
                        }

                        drawSpriteWithShadowOutline(renderer, tex, dst, mod, a, /*shadow*/false, /*outline*/true);
                    } else {
                        drawSpriteWithShadowOutline(renderer, tex, dst, mod, a, /*shadow*/true, /*outline*/true);
                    }
                }
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
            }
        }
    }

    // Soft bloom on brightly lit visible tiles.
    // This provides a cheap "glow" effect without shaders by using additive blending.
    if (game.darknessActive()) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
        for (int y = 0; y < d.height; ++y) {
            for (int x = 0; x < d.width; ++x) {
                const Tile& t = d.at(x, y);
                if (!t.visible) continue;
                const uint8_t L = game.tileLightLevel(x, y);
                if (L < 200u) continue;

                Color lc = game.tileLightColor(x, y);
                if (lc.r == 0 && lc.g == 0 && lc.b == 0) continue;

                // Intensity ramps up only in the top ~20% of the light range.
                const int strength = static_cast<int>(L) - 200;
                uint8_t a = static_cast<uint8_t>(std::clamp(strength * 3, 0, 70));
                // Torch flame flicker adds life to the bloom.
                const float f = torchFlicker(x, y);
                if (f != 1.0f) {
                    a = static_cast<uint8_t>(std::clamp(static_cast<int>(std::round(static_cast<float>(a) * f)), 0, 90));
                }
                if (a == 0) continue;

                SDL_Rect base = tileDst(x, y);

                // Two-layer bloom: wide + soft, then a tighter core.
                SDL_SetRenderDrawColor(renderer, lc.r, lc.g, lc.b, a);
                SDL_Rect wide{ base.x - 1, base.y - 1, base.w + 2, base.h + 2 };
                SDL_RenderFillRect(renderer, &wide);

                SDL_SetRenderDrawColor(renderer, lc.r, lc.g, lc.b, static_cast<uint8_t>(std::min<int>(static_cast<int>(a) + 10, 90)));
                SDL_Rect tight{ base.x + 2, base.y + 2, base.w - 4, base.h - 4 };
                SDL_RenderFillRect(renderer, &tight);
            }
        }
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
    }

    // Particles behind (e.g., projectile trails).
    if (particles_) {
        particles_->render(renderer, particleView, ParticleEngine::LAYER_BEHIND);
    }

    // FX projectiles
    for (const auto& fx : game.fxProjectiles()) {
        if (fx.path.empty()) continue;

        size_t idx = std::min(fx.pathIndex, fx.path.size() - 1);
        Vec2i cur = fx.path[idx];
        Vec2i nxt = cur;

        float tSeg = 0.0f;
        if (idx + 1 < fx.path.size() && fx.stepTime > 0.0f) {
            nxt = fx.path[idx + 1];
            tSeg = std::clamp(fx.stepTimer / fx.stepTime, 0.0f, 1.0f);
        }
        tSeg = smooth01(tSeg);

        if (!d.inBounds(cur.x, cur.y)) continue;
        const Tile& t = d.at(cur.x, cur.y);
        if (!t.explored) continue;

        SDL_Texture* tex = projectileTexture(fx.kind, frame);
        if (!tex) continue;

        SDL_Rect a = spriteDst(cur.x, cur.y);
        SDL_Rect b = spriteDst(nxt.x, nxt.y);

        SDL_Rect dst = a;
        dst.x = lerpI(a.x, b.x, tSeg);
        dst.y = lerpI(a.y, b.y, tSeg);

        // Arc it slightly so the projectile reads as moving through space.
        const float arcAmp = std::clamp(static_cast<float>(tile) * 0.10f, 1.0f, 7.0f);
        const float arc = std::sin(tSeg * 3.14159265f) * arcAmp;
        dst.y -= static_cast<int>(std::lround(arc));

        const Color mod = tileColorMod(cur.x, cur.y, t.visible);
        drawSpriteWithShadowOutline(renderer, tex, dst, mod, 255, /*shadow*/false, /*outline*/true);
    }

    // FX explosions (visual-only flashes; gameplay already applied)
    // Upgraded to a layered "white-hot" core + warm bloom + spark specks.
    if (!game.fxExplosions().empty()) {
        for (const auto& ex : game.fxExplosions()) {
            if (ex.delay > 0.0f) continue;
            if (ex.tiles.empty()) continue;

            const float dur = std::max(0.001f, ex.duration);
            const float t01 = std::clamp(ex.timer / dur, 0.0f, 1.0f);
            const float inv = 1.0f - t01;

            const int aBase = static_cast<int>(std::round(240.0f * inv));
            if (aBase <= 0) continue;

            // Approximate center so the effect can be slightly brighter in the middle.
            float cx = 0.0f;
            float cy = 0.0f;
            for (const Vec2i& p : ex.tiles) {
                cx += static_cast<float>(p.x) + 0.5f;
                cy += static_cast<float>(p.y) + 0.5f;
            }
            cx /= static_cast<float>(ex.tiles.size());
            cy /= static_cast<float>(ex.tiles.size());

            auto lerpU8 = [](uint8_t a, uint8_t b, float t) -> uint8_t {
                t = std::clamp(t, 0.0f, 1.0f);
                const float v = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
                int iv = static_cast<int>(v + 0.5f);
                return static_cast<uint8_t>(std::clamp(iv, 0, 255));
            };

            // Color shifts from a bright white-hot flash to a warmer orange as it fades.
            const Color hot{ 255, 250, 235, 255 };
            const Color warm{ 255, 150,  70, 255 };
            const Color core{ lerpU8(hot.r, warm.r, t01),
                              lerpU8(hot.g, warm.g, t01),
                              lerpU8(hot.b, warm.b, t01),
                              255 };

            // Bright core uses additive blending to "pop" without obscuring tile detail.
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);

            for (const Vec2i& p : ex.tiles) {
                if (!d.inBounds(p.x, p.y)) continue;
                const Tile& t = d.at(p.x, p.y);
                if (!t.explored) continue;

                const float dx = (static_cast<float>(p.x) + 0.5f) - cx;
                const float dy = (static_cast<float>(p.y) + 0.5f) - cy;
                const float dist = std::sqrt(dx * dx + dy * dy);
                const float centerBoost = std::clamp(1.0f - dist * 0.45f, 0.4f, 1.0f);

                const int aCore = static_cast<int>(std::round(static_cast<float>(aBase) * centerBoost));
                if (aCore <= 0) continue;

                SDL_Rect base = tileDst(p.x, p.y);

                // Inner flash + bloom ring.
                if (isoView) {
                    const int cx = base.x + base.w / 2;
                    const int cy = base.y + base.h / 2;

                    // Inner flash.
                    SDL_SetRenderDrawColor(renderer, core.r, core.g, core.b, static_cast<uint8_t>(std::min(255, aCore)));
                    {
                        const int hw = std::max(1, base.w / 4);
                        const int hh = std::max(1, base.h / 4);
                        fillIsoDiamond(renderer, cx, cy, hw, hh);
                    }

                    // Soft bloom ring.
                    SDL_SetRenderDrawColor(renderer, 255, 190, 110, static_cast<uint8_t>(std::min(255, aCore / 2)));
                    {
                        const int hw = std::max(1, base.w / 3);
                        const int hh = std::max(1, base.h / 3);
                        fillIsoDiamond(renderer, cx, cy, hw, hh);
                    }

                    // Tiny spark specks (deterministic) for texture (kept inside the diamond).
                    uint32_t seed = hashCombine(
                        hashCombine(static_cast<uint32_t>(game.turns()), static_cast<uint32_t>(ticks / 40u)),
                        hashCombine(static_cast<uint32_t>(p.x), static_cast<uint32_t>(p.y)));
                    const int sparks = 1 + static_cast<int>(seed & 0x3u);

                    SDL_SetRenderDrawColor(renderer, 255, 240, 200, static_cast<uint8_t>(std::min(255, (aCore * 2) / 3)));
                    for (int s = 0; s < sparks; ++s) {
                        seed = hash32(seed + 0x9e3779b9u + static_cast<uint32_t>(s) * 101u);

                        int sx = cx;
                        int sy = cy;

                        // Few attempts to land inside the isometric diamond.
                        for (int attempt = 0; attempt < 6; ++attempt) {
                            const int bw = std::max(1, base.w - 4);
                            const int bh = std::max(1, base.h - 4);

                            const int rx = static_cast<int>(seed % static_cast<uint32_t>(bw));
                            const int ry = static_cast<int>((seed >> 8) % static_cast<uint32_t>(bh));

                            sx = base.x + 2 + rx;
                            sy = base.y + 2 + ry;

                            if (pointInIsoDiamond(base, sx, sy)) break;
                            seed = hash32(seed + 0xBEEFu + static_cast<uint32_t>(attempt) * 97u);
                        }

                        SDL_RenderDrawPoint(renderer, sx, sy);
                    }
                } else {
                    // Top-down (square) version.
                    SDL_SetRenderDrawColor(renderer, core.r, core.g, core.b, static_cast<uint8_t>(std::min(255, aCore)));
                    SDL_Rect inner{ base.x + 4, base.y + 4, base.w - 8, base.h - 8 };
                    SDL_RenderFillRect(renderer, &inner);

                    SDL_SetRenderDrawColor(renderer, 255, 190, 110, static_cast<uint8_t>(std::min(255, aCore / 2)));
                    SDL_Rect mid{ base.x + 2, base.y + 2, base.w - 4, base.h - 4 };
                    SDL_RenderFillRect(renderer, &mid);

                    uint32_t seed = hashCombine(
                        hashCombine(static_cast<uint32_t>(game.turns()), static_cast<uint32_t>(ticks / 40u)),
                        hashCombine(static_cast<uint32_t>(p.x), static_cast<uint32_t>(p.y)));
                    const int sparks = 1 + static_cast<int>(seed & 0x3u);

                    SDL_SetRenderDrawColor(renderer, 255, 240, 200, static_cast<uint8_t>(std::min(255, (aCore * 2) / 3)));
                    for (int s = 0; s < sparks; ++s) {
                        seed = hash32(seed + 0x9e3779b9u + static_cast<uint32_t>(s) * 101u);
                        const int bw = std::max(1, base.w - 4);
                        const int bh = std::max(1, base.h - 4);
                        const int sx = base.x + 2 + static_cast<int>(seed % static_cast<uint32_t>(bw));
                        const int sy = base.y + 2 + static_cast<int>((seed >> 8) % static_cast<uint32_t>(bh));
                        SDL_RenderDrawPoint(renderer, sx, sy);
                    }
                }
            }

            // A very subtle warm "smoke" pass using normal alpha blending.
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 40, 18, 8, static_cast<uint8_t>(std::min(110, aBase / 3)));
            for (const Vec2i& p : ex.tiles) {
                if (!d.inBounds(p.x, p.y)) continue;
                const Tile& t = d.at(p.x, p.y);
                if (!t.explored) continue;
                SDL_Rect base = tileDst(p.x, p.y);
                if (isoView) {
                    const int cx = base.x + base.w / 2;
                    const int cy = base.y + base.h / 2;
                    const int hw = std::max(1, base.w / 2 - 1);
                    const int hh = std::max(1, base.h / 2 - 1);
                    fillIsoDiamond(renderer, cx, cy, hw, hh);
                } else {
                    SDL_Rect outer{ base.x + 1, base.y + 1, base.w - 2, base.h - 2 };
                    SDL_RenderFillRect(renderer, &outer);
                }
            }

            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }
    }

    // Particles in front (hits/explosions/fire).
    if (particles_) {
        particles_->render(renderer, particleView, ParticleEngine::LAYER_FRONT);
    }

    // Overlays
    if (isoView) {
        drawIsoHoverOverlay(game);
    }

    if (game.isLooking()) {
        drawLookOverlay(game);
    }

    if (game.isTargeting()) {
        drawTargetingOverlay(game);
    }

    // Post FX: subtle vignette over map region only.
    drawVignette(renderer, mapClip, /*thickness*/tile / 2, /*maxAlpha*/70);

    // Map drawing complete; release clip so HUD/UI can render normally.
    SDL_RenderSetClipRect(renderer, nullptr);

    // HUD (messages, stats)
    drawHud(game);

    // Level-up talent allocation overlay (forced while points are pending)
    if (game.isLevelUpOpen()) {
        drawLevelUpOverlay(game);
    }

    if (game.isMinimapOpen()) {
        drawMinimapOverlay(game);
    }

    if (game.isStatsOpen()) {
        drawStatsOverlay(game);
    }

    if (game.isCodexOpen()) {
        drawCodexOverlay(game);
    }

    if (game.isDiscoveriesOpen()) {
        drawDiscoveriesOverlay(game);
    }

    if (game.isScoresOpen()) {
        drawScoresOverlay(game);
    }

    if (game.isMessageHistoryOpen()) {
        drawMessageHistoryOverlay(game);
    }

    if (game.isSpellsOpen()) {
        drawSpellsOverlay(game);
    }

    if (game.isInventoryOpen()) {
        drawInventoryOverlay(game);
    }

    if (game.isChestOpen()) {
        drawChestOverlay(game);
    }

    if (game.isOptionsOpen()) {
        drawOptionsOverlay(game);
    }

    if (game.isKeybindsOpen()) {
        drawKeybindsOverlay(game);
    }

    if (game.isHelpOpen()) {
        drawHelpOverlay(game);
    }

    if (game.isCommandOpen()) {
        drawCommandOverlay(game);
    }

    if (game.perfOverlayEnabled()) {
        drawPerfOverlay(game);
    }

    SDL_RenderPresent(renderer);
}


std::string Renderer::saveScreenshotBMP(const std::string& directory, const std::string& prefix) const {
    namespace fs = std::filesystem;

    std::error_code ec;
    if (!directory.empty()) {
        fs::create_directories(fs::path(directory), ec);
    }

    // Timestamp for filename.
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    tm = *std::localtime(&t);
#endif

    std::ostringstream name;
    name << prefix << "_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".bmp";

    fs::path outPath;
    if (directory.empty()) {
        outPath = fs::path(name.str());
    } else {
        outPath = fs::path(directory) / name.str();
    }

    // Read back the current backbuffer.
    int w = 0, h = 0;
    if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0) {
        w = winW;
        h = winH;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return {};

    if (SDL_RenderReadPixels(renderer, nullptr, surface->format->format, surface->pixels, surface->pitch) != 0) {
        SDL_FreeSurface(surface);
        return {};
    }

    if (SDL_SaveBMP(surface, outPath.string().c_str()) != 0) {
        SDL_FreeSurface(surface);
        return {};
    }

    SDL_FreeSurface(surface);
    return outPath.string();
}

void Renderer::drawHud(const Game& game) {
    // HUD background
    SDL_Rect hudRect = {0, winH - hudH, winW, hudH};
    drawPanel(game, hudRect, 220, lastFrame);

    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};
    const Color red{255,80,80,255};
    const Color green{120,255,120,255};
    const Color important{255,160,255,255};

    const int hudTop = winH - hudH;

    const int scale = 2;
    const int charW = 6 * scale;
    const int lineH = 16;
    const int maxChars = std::max(1, (winW - 16) / std::max(1, charW));

    auto fitToChars = [](const std::string& s, int maxCharsLocal) -> std::string {
        if (maxCharsLocal <= 0) return std::string();
        if (static_cast<int>(s.size()) <= maxCharsLocal) return s;
        if (maxCharsLocal <= 3) return s.substr(0, static_cast<size_t>(maxCharsLocal));
        return s.substr(0, static_cast<size_t>(maxCharsLocal - 3)) + "...";
    };

    // Simple word wrap (ASCII-ish) with hard breaks for long tokens.
    auto wrap = [&](const std::string& s, int maxCharsLocal) -> std::vector<std::string> {
        std::vector<std::string> out;
        if (maxCharsLocal <= 0) return out;

        std::string cur;
        cur.reserve(s.size());

        auto flush = [&]() {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        };

        std::string word;
        word.reserve(32);

        auto emitWord = [&]() {
            if (word.empty()) return;

            // Hard-wrap very long words/tokens.
            if (static_cast<int>(word.size()) > maxCharsLocal) {
                flush();
                size_t pos = 0;
                while (pos < word.size()) {
                    const size_t n = std::min(word.size() - pos, static_cast<size_t>(maxCharsLocal));
                    out.push_back(word.substr(pos, n));
                    pos += n;
                }
                word.clear();
                return;
            }

            const int need = static_cast<int>(word.size()) + (cur.empty() ? 0 : 1);
            if (!cur.empty() && static_cast<int>(cur.size()) + need > maxCharsLocal) {
                flush();
            }

            if (!cur.empty()) cur.push_back(' ');
            cur += word;
            word.clear();
        };

        for (size_t i = 0; i <= s.size(); ++i) {
            const char c = (i < s.size()) ? s[i] : '\n';
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                emitWord();
                if (c == '\n') flush();
            } else {
                word.push_back(c);
            }
        }

        emitWord();
        flush();
        return out;
    };

    // Top row: Title and basic stats
    {
        const std::string hudTitle = std::string("PROCROGUE++ V") + PROCROGUE_VERSION;
        drawText5x7(renderer, 8, hudTop + 8, scale, white, hudTitle);
    }

    const Entity& p = game.player();

    // Status effect icons (right side of the top HUD row).
    {
        std::vector<std::pair<EffectKind, int>> effs;
        effs.reserve(static_cast<size_t>(EFFECT_KIND_COUNT));
        for (int k = 0; k < EFFECT_KIND_COUNT; ++k) {
            const EffectKind ek = static_cast<EffectKind>(k);
            const int turns = p.effects.get(ek);
            if (turns > 0) effs.emplace_back(ek, turns);
        }

        if (!effs.empty()) {
            const int icon = 16;
            const int gap = 3;
            const int totalW = static_cast<int>(effs.size()) * (icon + gap) - gap;
            int x0 = winW - 8 - totalW;
            const int y0 = hudTop + 6;

            for (int i = 0; i < static_cast<int>(effs.size()); ++i) {
                const int k = static_cast<int>(effs[static_cast<size_t>(i)].first);
                SDL_Texture* tex = effectIconTex[static_cast<size_t>(k)][static_cast<size_t>(lastFrame)];
                if (!tex) continue;

                SDL_Rect dst { x0 + i * (icon + gap), y0, icon, icon };
                SDL_SetTextureAlphaMod(tex, 240);
                SDL_RenderCopy(renderer, tex, nullptr, &dst);
                SDL_SetTextureAlphaMod(tex, 255);

                if (game.showEffectTimers()) {
                    int turns = effs[static_cast<size_t>(i)].second;
                    if (turns > 99) turns = 99;
                    const std::string tstr = std::to_string(turns);

                    // Bottom-right corner
                    const int tx = dst.x + icon - static_cast<int>(tstr.size()) * 6;
                    const int ty = dst.y + icon - 7;
                    drawText5x7(renderer, tx, ty, 1, white, tstr);
                }
            }
        }
    }

    std::stringstream ss;
    ss << "HP: " << p.hp << "/" << p.hpMax;
    ss << " | LV: " << game.playerCharLevel();
    ss << " | XP: " << game.playerXp() << "/" << game.playerXpToNext();
    ss << " | MANA: " << game.playerMana() << "/" << game.playerManaMax();
    ss << " | GOLD: " << game.goldCount();
    const int debtAll = game.shopDebtTotal();
    if (debtAll > 0) {
        const int debtThis = game.shopDebtThisDepth();
        ss << " | DEBT: " << debtAll;
        if (debtThis > 0 && debtThis != debtAll) {
            ss << " (THIS: " << debtThis << ")";
        }
    }
    const int piety = game.piety();
    const int prayCd = game.prayerCooldownTurns();
    if (piety > 0 || prayCd > 0) {
        ss << " | PIETY: " << piety;
        if (prayCd > 0) ss << " (CD: " << prayCd << ")";
    }

    ss << " | KEYS: " << game.keyCount() << " | PICKS: " << game.lockpickCount();

    const int arrows = ammoCount(game.inventory(), AmmoKind::Arrow);
    const int rocks  = ammoCount(game.inventory(), AmmoKind::Rock);
    if (arrows > 0) ss << " | ARROWS: " << arrows;
    if (rocks > 0)  ss << " | ROCKS: " << rocks;
    if (game.atCamp()) {
        ss << " | DEPTH: CAMP";
    } else if (game.infiniteWorldEnabled() && game.depth() > game.dungeonMaxDepth()) {
        ss << " | DEPTH: " << game.depth() << " (ENDLESS)";
    } else {
        ss << " | DEPTH: " << game.depth() << "/" << game.dungeonMaxDepth();
    }
    ss << " | DEEPEST: " << game.maxDepthReached();
    ss << " | TURNS: " << game.turns();
    ss << " | KILLS: " << game.kills();

    // Companions
    {
        int allies = 0;
        for (const auto& e : game.entities()) {
            if (e.id == p.id) continue;
            if (e.hp <= 0) continue;
            if (e.friendly) ++allies;
        }
        if (allies > 0) ss << " | ALLIES: " << allies;
    }

    // Status effects
    auto addStatus = [&](const char* label, int turns) {
        if (turns <= 0) return;
        if (game.showEffectTimers()) {
            ss << " | " << label << "(" << turns << ")";
        } else {
            ss << " | " << label;
        }
    };

    addStatus("POISON", p.effects.poisonTurns);
    addStatus("WEB", p.effects.webTurns);
    addStatus("CONF", p.effects.confusionTurns);
    addStatus("FEAR", p.effects.fearTurns);
    addStatus("BURN", p.effects.burnTurns);
    addStatus("REGEN", p.effects.regenTurns);
    addStatus("SHIELD", p.effects.shieldTurns);
    addStatus("HASTE", p.effects.hasteTurns);
    addStatus("VISION", p.effects.visionTurns);
    addStatus("INVIS", p.effects.invisTurns);
    addStatus("LEV", p.effects.levitationTurns);
    addStatus("HALL", p.effects.hallucinationTurns);
    {
        const std::string ht = game.hungerTag();
        if (!ht.empty()) ss << " | " << ht;
    }
    {
        if (game.encumbranceEnabled()) {
            ss << " | WT: " << game.inventoryWeight() << "/" << game.carryCapacity();
            const std::string bt = game.burdenTag();
            if (!bt.empty()) ss << " | " << bt;
        }
    }
    {
        const std::string st = game.sneakTag();
        if (!st.empty()) ss << " | " << st;
    }
    {
        const std::string lt = game.lightTag();
        if (!lt.empty()) ss << " | " << lt;
    }
    if (game.yendorDoomActive()) {
        ss << " | DOOM: " << game.yendorDoomLevel();
    }
    if (game.autosaveEveryTurns() > 0) {
        ss << " | AS: " << game.autosaveEveryTurns();
    }

    // Wrap the long stat line so it doesn't clip off-screen on narrow windows.
    std::vector<std::string> statLines = wrap(ss.str(), maxChars);
    if (statLines.empty()) statLines.push_back(std::string());

    // Keep stats compact: show up to 2 wrapped lines.
    constexpr int kMaxStatLines = 2;
    if (static_cast<int>(statLines.size()) > kMaxStatLines) {
        statLines.resize(kMaxStatLines);
        statLines.back() = fitToChars(statLines.back(), std::max(0, maxChars - 3)) + "...";
    }

    int yStats = hudTop + 24;
    for (const auto& ln : statLines) {
        drawText5x7(renderer, 8, yStats, scale, white, ln);
        yStats += lineH;
    }

    const int msgY0 = yStats + 4;

    // Controls (bottom area): wrap each hint line so it stays readable.
    struct HudLine {
        std::string text;
        Color color;
    };
    std::vector<HudLine> controlLines;

    auto pushWrappedControl = [&](const std::string& s, const Color& c) {
        const auto lines = wrap(s, maxChars);
        for (const auto& ln : lines) {
            controlLines.push_back({ln, c});
        }
    };

    pushWrappedControl(
        "MOVE: WASD/ARROWS/NUMPAD | SPACE/. WAIT | R REST | N SNEAK (STEALTH) | < > STAIRS",
        gray
    );

    if (game.isKicking()) {
        pushWrappedControl("KICK: CHOOSE DIRECTION (ESC CANCEL)", yellow);
    } else if (game.isDigging()) {
        pushWrappedControl("DIG: CHOOSE DIRECTION (ESC CANCEL)", yellow);
    } else {
        pushWrappedControl(
            "D DIG | B KICK | F FIRE | G PICKUP | I INV | Z SPELLS | O EXPLORE | P AUTOPICKUP | C SEARCH (TRAPS/SECRETS)",
            gray
        );
    }

    pushWrappedControl(
        "F2 OPT | F3 MSGS | # CMD | M MAP | SHIFT+TAB STATS | F5 SAVE | F6 SCORES | F9 LOAD | PGUP/PGDN LOG | ? HELP",
        gray
    );

    if (controlLines.empty()) controlLines.push_back({"", gray});

    // Compute dynamic layout: message log fills the space between stats and controls.
    int yControlTop = winH - lineH * static_cast<int>(controlLines.size());
    int msgY1 = yControlTop - 4;
    int maxMsgLines = (msgY1 - msgY0) / lineH;

    if (maxMsgLines < 1) {
        // If wrapping made the control area too tall, keep the tail (most relevant hints)
        // and ensure at least one line for the message log.
        const int maxControlLines = std::max(1, (winH - (msgY0 + lineH + 4)) / lineH);
        if (static_cast<int>(controlLines.size()) > maxControlLines) {
            controlLines.erase(controlLines.begin(), controlLines.end() - maxControlLines);
        }
        yControlTop = winH - lineH * static_cast<int>(controlLines.size());
        msgY1 = yControlTop - 4;
        maxMsgLines = (msgY1 - msgY0) / lineH;
    }
    if (maxMsgLines < 0) maxMsgLines = 0;

    // Message log (wrapped)
    const auto& msgs = game.messages();

    struct MsgLine {
        std::string text;
        Color color;
    };
    std::vector<MsgLine> revLines;
    revLines.reserve(static_cast<size_t>(std::max(0, maxMsgLines)));

    const int scroll = game.messageScroll();
    int last = static_cast<int>(msgs.size()) - 1 - scroll;
    last = std::min(last, static_cast<int>(msgs.size()) - 1);

    for (int i = last; i >= 0 && static_cast<int>(revLines.size()) < maxMsgLines; --i) {
        const auto& msg = msgs[static_cast<size_t>(i)];
        Color c = white;
        switch (msg.kind) {
            case MessageKind::Info:         c = white; break;
            case MessageKind::Combat:       c = red; break;
            case MessageKind::Loot:         c = yellow; break;
            case MessageKind::Warning:      c = yellow; break;
            case MessageKind::ImportantMsg: c = important; break;
            case MessageKind::Success:      c = green; break;
            case MessageKind::System:       c = gray; break;
        }

        std::string line = msg.text;
        if (msg.repeat > 1) {
            line += " (x" + std::to_string(msg.repeat) + ")";
        }

        const auto wrapped = wrap(line, maxChars);
        if (wrapped.empty()) continue;

        const int remaining = maxMsgLines - static_cast<int>(revLines.size());
        const int take = std::min(remaining, static_cast<int>(wrapped.size()));

        // Fill bottom-up: push wrapped lines in reverse order.
        for (int j = take - 1; j >= 0; --j) {
            revLines.push_back({wrapped[static_cast<size_t>(j)], c});
        }
    }

    std::reverse(revLines.begin(), revLines.end());

    int y = msgY0;
    for (const auto& ln : revLines) {
        drawText5x7(renderer, 8, y, scale, ln.color, ln.text);
        y += lineH;
    }

    // Controls at the very bottom
    int cy = yControlTop;
    for (const auto& ln : controlLines) {
        drawText5x7(renderer, 8, cy, scale, ln.color, ln.text);
        cy += lineH;
    }

    // End-game banner
    if (game.isGameOver()) {
        drawText5x7(renderer, winW/2 - 80, hudTop + 70, 3, red, "GAME OVER");
    } else if (game.isGameWon()) {
        drawText5x7(renderer, winW/2 - 90, hudTop + 70, 3, green, "YOU ESCAPED!");
    }
}



void Renderer::drawSpellsOverlay(const Game& game) {
    const int panelW = winW - 40;
    const int panelH = winH - 40;
    SDL_Rect bg{ 20, 20, panelW, panelH };

    drawPanel(game, bg, 210, lastFrame);

    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};
    const Color cyan{140,220,255,255};

    const int scale = 2;
    const int pad = 16;
    const int lineH = 18;

    int x = bg.x + pad;
    int y = bg.y + pad;

    drawText5x7(renderer, x, y, scale, yellow, "SPELLS");
    drawText5x7(renderer, x + 160, y, scale, gray, "(ENTER: cast, ESC: close)");

    {
        std::stringstream ms;
        ms << "MANA: " << game.playerMana() << "/" << game.playerManaMax();
        drawText5x7(renderer, x, y + 14, scale, gray, ms.str());
    }

    y += 44;

    const std::vector<SpellKind> spells = game.knownSpellsList();
    const int sel = game.spellsSelection();

    // Layout: list (left) + description (right)
    const int colGap = 18;
    const int listW = (bg.w * 50) / 100;
    SDL_Rect listRect{ x, y, listW, bg.y + bg.h - pad - y };
    SDL_Rect infoRect{ x + listW + colGap, y, bg.x + bg.w - pad - (x + listW + colGap), listRect.h };

    const int maxLines = std::max(1, listRect.h / lineH);
    int start = 0;
    if (!spells.empty()) {
        start = std::clamp(sel - maxLines / 2, 0, std::max(0, (int)spells.size() - maxLines));
    }
    const int end = std::min((int)spells.size(), start + maxLines);

    // Selection background
    if (!spells.empty() && sel >= start && sel < end) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_Rect hi{ listRect.x - 6, listRect.y + (sel - start) * lineH - 2, listRect.w + 12, lineH };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 20);
        SDL_RenderFillRect(renderer, &hi);
    }

    // Simple word wrap for the info panel.
    auto wrap = [&](const std::string& s, int maxChars) -> std::vector<std::string> {
        std::vector<std::string> out;
        std::string cur;
        cur.reserve(s.size());
        auto flush = [&]() {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        };
        std::string word;
        for (size_t i = 0; i <= s.size(); ++i) {
            const char c = (i < s.size()) ? s[i] : ' ';
            if (c == ' ' || c == '\n' || i == s.size()) {
                if (!word.empty()) {
                    const int need = static_cast<int>(word.size()) + (cur.empty() ? 0 : 1);
                    if (!cur.empty() && (int)cur.size() + need > maxChars) {
                        flush();
                    }
                    if (!cur.empty()) cur.push_back(' ');
                    cur += word;
                    word.clear();
                }
                if (c == '\n') {
                    flush();
                }
            } else {
                word.push_back(c);
            }
        }
        flush();
        return out;
    };

    // List
    for (int i = start; i < end; ++i) {
        const SpellKind sk = spells[static_cast<size_t>(i)];
        const SpellDef& sd = spellDef(sk);

        std::stringstream line;
        line << sd.name;
        line << "  (";
        line << "M" << sd.manaCost;
        if (sd.needsTarget) line << ", R" << sd.range;
        else line << ", SELF";
        line << ")";

        const bool enough = game.playerMana() >= sd.manaCost;
        const Color c = enough ? white : gray;
        if (i == sel) {
            drawText5x7(renderer, listRect.x, listRect.y + (i - start) * lineH, scale, cyan, line.str());
        } else {
            drawText5x7(renderer, listRect.x, listRect.y + (i - start) * lineH, scale, c, line.str());
        }
    }

    // Info panel
    if (spells.empty()) {
        drawText5x7(renderer, infoRect.x, infoRect.y, scale, gray, "YOU DON'T KNOW ANY SPELLS.");
        drawText5x7(renderer, infoRect.x, infoRect.y + 18, scale, gray, "READ SPELLBOOKS TO LEARN.");
        return;
    }

    const int selIdx = clampi(sel, 0, (int)spells.size() - 1);
    const SpellKind sk = spells[static_cast<size_t>(selIdx)];
    const SpellDef& sd = spellDef(sk);

    drawText5x7(renderer, infoRect.x, infoRect.y, scale, yellow, sd.name);

    {
        std::stringstream meta;
        meta << "COST: " << sd.manaCost << "  |  " << (sd.needsTarget ? "TARGET" : "SELF") ;
        if (sd.needsTarget) meta << "  |  RANGE: " << sd.range;
        drawText5x7(renderer, infoRect.x, infoRect.y + 18, scale, gray, meta.str());
    }

    const int maxChars = std::max(10, infoRect.w / (6 * scale));
    const auto lines = wrap(sd.description, maxChars);

    int ty = infoRect.y + 42;
    for (const auto& ln : lines) {
        if (ty + 14 > infoRect.y + infoRect.h) break;
        drawText5x7(renderer, infoRect.x, ty, scale, white, ln);
        ty += 18;
    }
}


void Renderer::drawInventoryOverlay(const Game& game) {
    const int panelW = winW - 40;
    const int panelH = winH - 40;
    SDL_Rect bg{ 20, 20, panelW, panelH };

    drawPanel(game, bg, 210, lastFrame);

    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};
    const Color cyan{140,220,255,255};

    const int scale = 2;
    const int pad = 16;

    int x = bg.x + pad;
    int y = bg.y + pad;

    drawText5x7(renderer, x, y, scale, yellow, "INVENTORY");
    drawText5x7(renderer, x + 160, y, scale, gray, "(ENTER: use/equip, D: drop, ESC: close)");
    if (game.encumbranceEnabled()) {
        std::stringstream ws;
        ws << "WT: " << game.inventoryWeight() << "/" << game.carryCapacity();
        const std::string bt = game.burdenTag();
        if (!bt.empty()) ws << " (" << bt << ")";
        drawText5x7(renderer, x, y + 14, scale, gray, ws.str());
        y += 44;
    } else {
        y += 28;
    }

    const auto& inv = game.inventory();
    const int sel = game.inventorySelection();

    // Precompute current stats + equipped items once (used for quick-compare badges and preview).
    const Entity& p = game.player();
    const int baseAtk = p.baseAtk;
    const int shieldBonus = (p.effects.shieldTurns > 0) ? 2 : 0;
    const int curAtk = game.playerAttack();
    const int curDef = game.playerDefense();

    const Item* eqM = game.equippedMelee();
    const Item* eqR = game.equippedRanged();
    const Item* eqA = game.equippedArmor();

    auto bucScalar = [](const Item& it) -> int {
        return (it.buc < 0 ? -1 : (it.buc > 0 ? 1 : 0));
    };


    // Layout: list (left) + preview/info (right)
    const int colGap = 18;
    const int listW = (bg.w * 58) / 100;
    SDL_Rect listRect{ x, y, listW, bg.y + bg.h - pad - y };
    SDL_Rect infoRect{ x + listW + colGap, y, bg.x + bg.w - pad - (x + listW + colGap), listRect.h };

    // List scroll
    const int lineH = 18;
    const int maxLines = std::max(1, listRect.h / lineH);
    int start = 0;
    if (!inv.empty()) {
        start = std::clamp(sel - maxLines / 2, 0, std::max(0, (int)inv.size() - maxLines));
    }
    const int end = std::min((int)inv.size(), start + maxLines);

    // Selection background
    if (!inv.empty() && sel >= start && sel < end) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_Rect hi{ listRect.x - 6, listRect.y + (sel - start) * lineH - 2, listRect.w + 12, lineH };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 20);
        SDL_RenderFillRect(renderer, &hi);
    }

    // Helpers
    auto fitToChars = [](const std::string& s, int maxChars) -> std::string {
        if ((int)s.size() <= maxChars) return s;
        if (maxChars <= 1) return s.substr(0, 1);
        return s.substr(0, static_cast<size_t>(std::max(0, maxChars - 3))) + "...";
    };

    auto itemEffectDesc = [&](const Item& it, bool identified) -> std::string {
        const ItemDef& def = itemDef(it.kind);
		if (!identified && isIdentifiableKind(it.kind)) return "EFFECT: UNKNOWN";
        switch (it.kind) {
			case ItemKind::PotionHealing:
				return "EFFECT: HEAL +" + std::to_string(std::max(0, def.healAmount)) + " HP";
            case ItemKind::PotionAntidote: return "EFFECT: CURE POISON";
            case ItemKind::PotionStrength: return "EFFECT: +ATK";
            case ItemKind::PotionRegeneration: return "EFFECT: REGEN";
			case ItemKind::PotionShielding: return "EFFECT: STONESKIN";
            case ItemKind::PotionHaste: return "EFFECT: HASTE";
            case ItemKind::PotionVision: return "EFFECT: VISION";
            case ItemKind::PotionInvisibility: return "EFFECT: INVISIBILITY";
            case ItemKind::PotionClarity: return "EFFECT: CLARITY";
            case ItemKind::PotionLevitation: return "EFFECT: LEVITATION";
            case ItemKind::PotionHallucination: return "EFFECT: HALLUCINATION";
            case ItemKind::ScrollTeleport: return "EFFECT: TELEPORT";
            case ItemKind::ScrollMapping: return "EFFECT: MAPPING";
            case ItemKind::ScrollDetectTraps: return "EFFECT: DETECT TRAPS";
			case ItemKind::ScrollDetectSecrets: return "EFFECT: DETECT SECRETS";
            case ItemKind::ScrollKnock: return "EFFECT: KNOCK";
            case ItemKind::ScrollEnchantWeapon: return "EFFECT: ENCHANT WEAPON";
            case ItemKind::ScrollEnchantArmor: return "EFFECT: ENCHANT ARMOR";
            case ItemKind::ScrollEnchantRing: return "EFFECT: ENCHANT RING";
            case ItemKind::ScrollIdentify: return "EFFECT: IDENTIFY";
            case ItemKind::ScrollRemoveCurse: return "EFFECT: REMOVE CURSE";
            case ItemKind::ScrollConfusion: return "EFFECT: CONFUSION";
            case ItemKind::ScrollFear: return "EFFECT: FEAR";
            case ItemKind::ScrollEarth: return "EFFECT: EARTH";
            case ItemKind::ScrollTaming: return "EFFECT: TAMING";
			case ItemKind::FoodRation:
				return def.hungerRestore > 0
					? ("EFFECT: RESTORE HUNGER +" + std::to_string(def.hungerRestore))
					: "EFFECT: FOOD";
            default: break;
        }
        return "EFFECT: —";
    };

    auto fmtSigned = [](int v) -> std::string {
        if (v == 0) return "+0";
        return (v > 0 ? "+" : "") + std::to_string(v);
    };

    // Compact per-item quick-compare badge shown in the inventory list (left panel).
    // Examples: "ATK+2", "DEF-1", "RA+1 RN+2", "D+1 M+1".
    // Returned polarity: +1 = upgrade (positive), -1 = downgrade (negative), 0 = neutral/none.
    auto quickBadge = [&](const Item& it, const std::string& tag) -> std::pair<std::string, int> {
        const ItemDef& def = itemDef(it.kind);
        const int buc = bucScalar(it);

        // Don't spam badges on equipped items; the [M]/[R]/[A]/[1]/[2] tag already conveys state.
        if (!tag.empty()) {
            if ((tag.find('M') != std::string::npos && isMeleeWeapon(it.kind)) ||
                (tag.find('R') != std::string::npos && isRangedWeapon(it.kind)) ||
                (tag.find('A') != std::string::npos && isArmor(it.kind)) ||
                ((tag.find('1') != std::string::npos || tag.find('2') != std::string::npos) && isRingKind(it.kind))) {
                return {"", 0};
            }
        }

        // Melee weapon upgrade/downgrade versus equipped melee weapon.
        if (isMeleeWeapon(it.kind)) {
            int cur = 0;
            if (eqM) {
                const ItemDef& cd = itemDef(eqM->kind);
                cur = cd.meleeAtk + eqM->enchant + bucScalar(*eqM);
            }
            const int cand = def.meleeAtk + it.enchant + buc;
            const int delta = cand - cur;
            if (delta == 0) return {"", 0};
            return {"ATK" + fmtSigned(delta), (delta > 0 ? 1 : -1)};
        }

        // Armor upgrade/downgrade versus equipped armor.
        if (isArmor(it.kind)) {
            int cur = 0;
            if (eqA) {
                const ItemDef& cd = itemDef(eqA->kind);
                cur = cd.defense + eqA->enchant + bucScalar(*eqA);
            }
            const int cand = def.defense + it.enchant + buc;
            const int delta = cand - cur;
            if (delta == 0) return {"", 0};
            return {"DEF" + fmtSigned(delta), (delta > 0 ? 1 : -1)};
        }

        // Ranged weapon: show ranged attack and range deltas versus equipped ranged.
        if (isRangedWeapon(it.kind)) {
            int curAtk = 0;
            int curRng = 0;
            if (eqR) {
                const ItemDef& cd = itemDef(eqR->kind);
                curAtk = cd.rangedAtk + eqR->enchant + bucScalar(*eqR);
                curRng = cd.range;
            }
            const int candAtk = def.rangedAtk + it.enchant + buc;
            const int candRng = def.range;

            const int dAtk = candAtk - curAtk;
            const int dRng = candRng - curRng;

            std::string s;
            int pol = 0;
            if (dAtk != 0) {
                s += "RA" + fmtSigned(dAtk);
                pol = (dAtk > 0 ? 1 : -1);
            }
            if (dRng != 0) {
                if (!s.empty()) s += " ";
                s += "RN" + fmtSigned(dRng);
                if (pol == 0) pol = (dRng > 0 ? 1 : -1);
            }
            return {s, pol};
        }

        // Rings: show up to two strongest modifiers (not relative; just quick scan info).
        if (isRingKind(it.kind)) {
            auto applyMod = [&](int base) -> int {
                if (base == 0) return 0;
                return base + it.enchant + buc;
            };

            std::vector<std::pair<char, int>> mods;
            mods.reserve(5);
            mods.push_back({'D', applyMod(def.defense)});
            mods.push_back({'M', applyMod(def.modMight)});
            mods.push_back({'A', applyMod(def.modAgility)});
            mods.push_back({'V', applyMod(def.modVigor)});
            mods.push_back({'F', applyMod(def.modFocus)});

            std::vector<std::pair<char, int>> nz;
            for (const auto& m : mods) {
                if (m.second != 0) nz.push_back(m);
            }
            if (nz.empty()) return {"", 0};

            std::sort(nz.begin(), nz.end(), [](const auto& a, const auto& b) {
                const int aa = std::abs(a.second);
                const int bb = std::abs(b.second);
                if (aa != bb) return aa > bb;
                return a.first < b.first;
            });

            std::string s;
            int pol = 0;
            const int take = std::min(2, (int)nz.size());
            for (int i = 0; i < take; ++i) {
                if (!s.empty()) s += " ";
                s.push_back(nz[(size_t)i].first);
                s += fmtSigned(nz[(size_t)i].second);
                if (pol == 0) pol = (nz[(size_t)i].second > 0 ? 1 : -1);
            }
            if ((int)nz.size() > take) s += " ...";
            return {s, pol};
        }

        return {"", 0};
    };

    // Draw list (with item icons)
    int yy = listRect.y;

    const int icon = 16;
    const int arrowW = scale * 6 * 2; // "> " column
    const int iconX = listRect.x + arrowW;
    const int textX = iconX + icon + 6;
    const int maxChars = std::max(10, (listRect.w - (textX - listRect.x)) / (scale * 6));

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    for (int i = start; i < end; ++i) {
        const Item& it = inv[static_cast<size_t>(i)];
        const std::string tag = game.equippedTag(it.id); // "" or "M"/"R"/"A"/...

        Color c = (i == sel) ? white : gray;
        if (i != sel && itemIsArtifact(it)) c = yellow;

        // Selection arrow
        drawText5x7(renderer, listRect.x, yy, scale, c, (i == sel) ? ">" : " ");

        // Icon background (subtle), then sprite
        SDL_Rect iconDst{ iconX, yy + (lineH - icon) / 2, icon, icon };
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, (i == sel) ? 70 : 45);
        SDL_RenderFillRect(renderer, &iconDst);

        Item visIt = it;
        if (isHallucinating(game)) {
            visIt.kind = hallucinatedItemKind(game, it);
        }

        applyIdentificationVisuals(game, visIt);

        SDL_Texture* itex = itemTexture(visIt, lastFrame + visIt.id);
        if (itex) {
            SDL_RenderCopy(renderer, itex, nullptr, &iconDst);
        }

        // Text (tag + name)
        std::string row;
        if (!tag.empty()) {
            row += "[";
            row += tag;
            row += "] ";
        }
        row += game.displayItemName(it);

        const auto badgeInfo = quickBadge(it, tag);
        const std::string& badge = badgeInfo.first;
        const int badgePol = badgeInfo.second;

        // Reserve space for the badge on the right (1 char gap).
        int nameChars = maxChars;
        if (!badge.empty()) {
            nameChars = std::max(1, maxChars - (int)badge.size() - 1);
        }

        drawText5x7(renderer, textX, yy, scale, c, fitToChars(row, nameChars));

        if (!badge.empty()) {
            const int charW = scale * 6;
            const int badgeX = listRect.x + listRect.w - charW - (int)badge.size() * charW;
            if (badgeX > textX + charW) {
                Color bc = gray;
                if (badgePol > 0) bc = cyan;
                else if (badgePol < 0) bc = yellow;
                drawText5x7(renderer, badgeX, yy, scale, bc, badge);
            }
        }

        yy += lineH;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);

    if (inv.empty()) {
        drawText5x7(renderer, listRect.x, listRect.y, scale, gray, "(EMPTY)");
    } else if (sel >= 0 && sel < (int)inv.size()) {
        // Draw preview / info panel
        const Item& it = inv[static_cast<size_t>(sel)];
        const ItemDef& def = itemDef(it.kind);

        bool identified = (game.displayItemNameSingle(it.kind) == itemDisplayNameSingle(it.kind));

        int ix = infoRect.x;
        int iy = infoRect.y;

        // Name (top)
        drawText5x7(renderer, ix, iy, scale, cyan, fitToChars(game.displayItemName(it), 30));
        iy += 22;

        // Sprite preview
        const int previewPx = std::min(96, infoRect.w);
        SDL_Rect sprDst{ ix, iy, previewPx, previewPx };
        Item visIt = it;
        if (isHallucinating(game)) {
            visIt.kind = hallucinatedItemKind(game, it);
        }

        applyIdentificationVisuals(game, visIt);

        SDL_Texture* tex = itemTexture(visIt, lastFrame + visIt.id);
        if (tex) {
            SDL_RenderCopy(renderer, tex, nullptr, &sprDst);
        }
        iy += previewPx + 10;

        // Stats lines
        auto statLine = [&](const std::string& s, const Color& c) {
            drawText5x7(renderer, ix, iy, scale, c, fitToChars(s, 32));
            iy += 18;
        };

		// Type / stats
		auto ammoLabel = [](AmmoKind a) -> const char* {
			switch (a) {
				case AmmoKind::Arrow: return "ARROWS";
				case AmmoKind::Rock:  return "ROCKS";
				default: return "NONE";
			}
		};

		auto statCompare = [&](const char* label, int cur, int after) {
			const int delta = after - cur;
			std::ostringstream ss;
			ss << label << ": " << cur << " -> " << after;
			if (delta > 0) ss << " (+" << delta << ")";
			else if (delta < 0) ss << " (" << delta << ")";
			statLine(ss.str(), gray);
		};

		const bool identifiable = isIdentifiableKind(it.kind);
		const bool isWand = isRangedWeapon(it.kind) && def.maxCharges > 0 && def.ammo == AmmoKind::None;
		const bool isFood = (def.hungerRestore > 0) || (it.kind == ItemKind::FoodRation);

		if (isGold(it.kind)) {
			statLine("TYPE: GOLD", white);
			statLine("VALUE: " + std::to_string(it.count), gray);
		} else if (it.kind == ItemKind::Key) {
			statLine("TYPE: KEY", white);
			statLine("USED FOR: LOCKED DOORS / CHESTS", gray);
		} else if (it.kind == ItemKind::Lockpick) {
			statLine("TYPE: LOCKPICK", white);
			statLine("USED FOR: PICK LOCKS (CHANCE)", gray);
		} else if (it.kind == ItemKind::Torch || it.kind == ItemKind::TorchLit) {
			statLine("TYPE: LIGHT SOURCE", white);
			if (it.kind == ItemKind::TorchLit) {
				statLine("STATUS: LIT", gray);
				statLine("FUEL: " + std::to_string(it.charges) + " TURNS", gray);
				statLine("RADIUS: 8", gray);
			} else {
				statLine("STATUS: UNLIT", gray);
				statLine("USE: LIGHT A TORCH", gray);
			}
		} else if (isFood) {
			statLine("TYPE: FOOD", white);
			if (game.hungerEnabled() && def.hungerRestore > 0) {
				statLine("RESTORE: +" + std::to_string(def.hungerRestore) + " HUNGER", gray);
			} else {
				statLine("HUNGER SYSTEM: DISABLED", gray);
			}
		} else if (isMeleeWeapon(it.kind)) {
			statLine("TYPE: MELEE WEAPON", white);
			const int cand = def.meleeAtk + it.enchant + bucScalar(it);

			int newAtk = curAtk;
			if (eqM) {
				const ItemDef& curD = itemDef(eqM->kind);
				newAtk -= (curD.meleeAtk + eqM->enchant + bucScalar(*eqM));
			}
			newAtk += cand;

			statCompare("ATK", curAtk, newAtk);
		} else if (isArmor(it.kind)) {
			statLine("TYPE: ARMOR", white);
			const int cand = def.defense + it.enchant + bucScalar(it);

			int newDef = curDef;
			if (eqA) {
				const ItemDef& curD = itemDef(eqA->kind);
				newDef -= (curD.defense + eqA->enchant + bucScalar(*eqA));
			}
			newDef += cand;

			statCompare("DEF", curDef, newDef);
			if (shieldBonus > 0) {
				statLine("(INCLUDES SHIELD +2)", gray);
			}
		} else if (isWand) {
			statLine(identifiable ? "TYPE: WAND (IDENTIFIABLE)" : "TYPE: WAND", white);

			if (identifiable && !identified) {
				statLine("EFFECT: UNKNOWN", gray);
				statLine("RANGE: UNKNOWN", gray);
				statLine("CHARGES: UNKNOWN", gray);
				statLine("READY: UNKNOWN", gray);
				statLine("IDENTIFIED: NO", gray);
			} else {
				auto wandEffect = [&]() -> std::string {
					if (it.kind == ItemKind::WandDigging) return "DIGGING";
					switch (def.projectile) {
						case ProjectileKind::Spark: return "SPARKS";
						case ProjectileKind::Fireball: return "FIREBALL";
						default: return "MAGIC";
					}
				};

				statLine("EFFECT: " + wandEffect(), gray);
				statLine("RANGE: " + std::to_string(def.range), gray);
				statLine("CHARGES: " + std::to_string(it.charges) + "/" + std::to_string(def.maxCharges), gray);
				const int baseRAtk = std::max(1, baseAtk + def.rangedAtk + it.enchant + 2);
				statLine("RATK (BASE): " + std::to_string(baseRAtk) + "+", gray);
				statLine(std::string("READY: ") + (it.charges > 0 ? "YES" : "NO"), gray);
				if (def.projectile == ProjectileKind::Fireball) {
					statLine("AOE: RADIUS 1 (3x3)", gray);
				}
				if (identifiable) {
					statLine("IDENTIFIED: YES", gray);
				}
			}
		} else if (isRangedWeapon(it.kind)) {
			statLine("TYPE: RANGED WEAPON", white);
			const int thisRAtk = std::max(1, baseAtk + def.rangedAtk + it.enchant + bucScalar(it));
			if (eqR) {
				const ItemDef& curD = itemDef(eqR->kind);
				const int curRAtk = std::max(1, baseAtk + curD.rangedAtk + eqR->enchant + bucScalar(*eqR));
				statCompare("RATK", curRAtk, thisRAtk);
			} else {
				statLine("RATK (BASE): " + std::to_string(thisRAtk), gray);
			}
			statLine("RANGE: " + std::to_string(def.range), gray);
			if (def.ammo != AmmoKind::None) {
				const int have = ammoCount(inv, def.ammo);
				statLine(std::string("AMMO: ") + ammoLabel(def.ammo) + " (" + std::to_string(have) + ")", gray);
			}
			const bool chargesOk = (def.maxCharges <= 0) || (it.charges > 0);
			const bool ammoOk = (def.ammo == AmmoKind::None) || (ammoCount(inv, def.ammo) > 0);
			const bool ready = (def.range > 0) && chargesOk && ammoOk;
			statLine(std::string("READY: ") + (ready ? "YES" : "NO"), gray);
		} else if (isRingKind(it.kind)) {
			statLine(identifiable ? "TYPE: RING (IDENTIFIABLE)" : "TYPE: RING", white);

			if (identifiable && !identified) {
				statLine("EFFECT: UNKNOWN", gray);
				statLine("IDENTIFIED: NO", gray);
			} else {
				const int bucBonus = (it.buc < 0 ? -1 : (it.buc > 0 ? 1 : 0));
				auto fmtMod = [&](const char* label, int base) {
					if (base == 0) return;
					// Only apply ench/buc if the ring actually provides the stat.
					const int v = base + it.enchant + bucBonus;
					const std::string s = (v >= 0 ? "+" : "") + std::to_string(v);
					statLine(std::string(label) + s, gray);
				};
				fmtMod("MIGHT: ", def.modMight);
				fmtMod("AGILITY: ", def.modAgility);
				fmtMod("VIGOR: ", def.modVigor);
				fmtMod("FOCUS: ", def.modFocus);
				if (def.defense != 0) {
					const int v = def.defense + it.enchant + bucBonus;
					const std::string s = (v >= 0 ? "+" : "") + std::to_string(v);
					statLine("DEF BONUS: " + s, gray);
				}
				if (identifiable) {
					statLine("IDENTIFIED: YES", gray);
				}
			}
		} else if (def.consumable) {
			statLine(identifiable ? "TYPE: CONSUMABLE (IDENTIFIABLE)" : "TYPE: CONSUMABLE", white);
			statLine(itemEffectDesc(it, identified), gray);
			if (identifiable) {
				statLine(std::string("IDENTIFIED: ") + (identified ? "YES" : "NO"), gray);
			}
		} else {
			statLine("TYPE: MISC", white);
		}

        if (it.count > 1) {
            statLine("COUNT: " + std::to_string(it.count), gray);
        }

		// Quick equipment summary (useful when comparing gear).
		iy += 6;
		statLine("EQUIPPED", yellow);
		statLine("M: " + game.equippedMeleeName(), gray);
		statLine("R: " + game.equippedRangedName(), gray);
		statLine("A: " + game.equippedArmorName(), gray);
		statLine("1: " + game.equippedRing1Name(), gray);
		statLine("2: " + game.equippedRing2Name(), gray);
    }
}


void Renderer::drawChestOverlay(const Game& game) {
    const int panelW = winW - 40;
    const int panelH = winH - 40;
    SDL_Rect bg{ 20, 20, panelW, panelH };

    drawPanel(game, bg, 210, lastFrame);

    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};

    const int scale = 2;
    const int pad = 16;

    int x = bg.x + pad;
    int y = bg.y + pad;

    auto tierName = [](int tier) -> const char* {
        switch (tier) {
            case 0: return "COMMON";
            case 1: return "STURDY";
            case 2: return "ORNATE";
            case 3: return "LARGE";
            case 4: return "ANCIENT";
            default: return "CHEST";
        }
    };

    const int tier = game.chestOpenTier();
    const int limit = game.chestOpenStackLimit();
    const int chestStacks = static_cast<int>(game.chestOpenItems().size());

    std::stringstream hs;
    hs << "CHEST (" << tierName(tier) << ")";
    drawText5x7(renderer, x, y, scale, yellow, hs.str());

    drawText5x7(renderer, x + 220, y, scale, gray, "(ENTER: move, D: move 1, G: all, S: sort, ESC/I: close)");

    std::stringstream cap;
    cap << "CAP: " << chestStacks << "/" << limit << " STACKS  (LEFT/RIGHT: switch pane)";
    drawText5x7(renderer, x, y + 14, scale, gray, cap.str());

    y += 44;

    const bool paneChest = game.chestPaneIsChest();

    const int colGap = 18;
    const int colW = (bg.w - pad * 2 - colGap) / 2;

    // Column headers
    drawText5x7(renderer, x, y, scale, paneChest ? yellow : gray, "CHEST CONTENTS");
    drawText5x7(renderer, x + colW + colGap, y, scale, paneChest ? gray : yellow, "INVENTORY");

    y += 28;

    SDL_Rect chestRect{ x, y, colW, bg.y + bg.h - pad - y };
    SDL_Rect invRect{ x + colW + colGap, y, colW, chestRect.h };

    const auto& chestItems = game.chestOpenItems();
    const auto& inv = game.inventory();

    const int chestSel = game.chestSelection();
    const int invSel = game.inventorySelection();

    const int lineH = 18;
    const int maxLines = std::max(1, chestRect.h / lineH);

    auto startIndex = [&](int sel, int count) -> int {
        if (count <= 0) return 0;
        return std::clamp(sel - maxLines / 2, 0, std::max(0, count - maxLines));
    };

    const int chestStart = startIndex(chestSel, (int)chestItems.size());
    const int invStart = startIndex(invSel, (int)inv.size());

    const int chestEnd = std::min((int)chestItems.size(), chestStart + maxLines);
    const int invEnd = std::min((int)inv.size(), invStart + maxLines);

    // Selection highlight
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    if (paneChest && !chestItems.empty() && chestSel >= chestStart && chestSel < chestEnd) {
        SDL_Rect hi{ chestRect.x - 6, chestRect.y + (chestSel - chestStart) * lineH - 2, chestRect.w + 12, lineH };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 20);
        SDL_RenderFillRect(renderer, &hi);
    }
    if (!paneChest && !inv.empty() && invSel >= invStart && invSel < invEnd) {
        SDL_Rect hi{ invRect.x - 6, invRect.y + (invSel - invStart) * lineH - 2, invRect.w + 12, lineH };
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 20);
        SDL_RenderFillRect(renderer, &hi);
    }

    // Helpers
    auto fitToChars = [](const std::string& s, int maxChars) -> std::string {
        if ((int)s.size() <= maxChars) return s;
        if (maxChars <= 3) return s.substr(0, std::max(0, maxChars));
        return s.substr(0, maxChars - 3) + "...";
    };

    auto drawList = [&](const std::vector<Item>& items, const SDL_Rect& r, int start, int end, int sel, bool active, bool showEquippedTag) {
        int rowY = r.y;
        const int iconX = r.x;
        const int textX = iconX + 20;
        const int maxChars = std::max(8, (r.w - 26) / ((5 + 1) * scale));

        if (items.empty()) {
            drawText5x7(renderer, r.x, r.y, scale, gray, "(EMPTY)");
            return;
        }

        for (int i = start; i < end; ++i) {
            const Item& it = items[(size_t)i];

            // Selected arrow (active pane only).
            if (active && i == sel) {
                drawText5x7(renderer, r.x - 12, rowY + 3, scale, yellow, ">");
            }

            drawItemIcon(game, it, iconX, rowY, 16);

            std::string line = game.displayItemName(it);
            if (showEquippedTag) {
                const std::string tag = game.equippedTag(it.id);
                if (!tag.empty()) {
                    line += " " + tag;
                }
            }
            line = fitToChars(line, maxChars);

            drawText5x7(renderer, textX, rowY + 3, scale, white, line);

            rowY += lineH;
        }
    };

    drawList(chestItems, chestRect, chestStart, chestEnd, chestSel, paneChest, false);
    drawList(inv, invRect, invStart, invEnd, invSel, !paneChest, true);
}

void Renderer::drawOptionsOverlay(const Game& game) {
    const int panelW = std::min(winW - 80, 820);
    const int panelH = 460;
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - panelH) / 2;

    SDL_Rect bg{x0, y0, panelW, panelH};
    drawPanel(game, bg, 210, lastFrame);

    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};

    const int scale = 2;
    int y = y0 + 16;

    drawText5x7(renderer, x0 + 16, y, scale, yellow, "OPTIONS");
    y += 26;

    auto yesNo = [](bool b) { return b ? "ON" : "OFF"; };

    auto autoPickupLabel = [](AutoPickupMode m) -> const char* {
        switch (m) {
            case AutoPickupMode::Off:   return "OFF";
            case AutoPickupMode::Gold:  return "GOLD";
            case AutoPickupMode::Smart: return "SMART";
            case AutoPickupMode::All:   return "ALL";
        }
        return "?";
    };

    auto uiThemeLabel = [](UITheme t) -> const char* {
        switch (t) {
            case UITheme::DarkStone: return "DARKSTONE";
            case UITheme::Parchment: return "PARCHMENT";
            case UITheme::Arcane:    return "ARCANE";
        }
        return "UNKNOWN";
    };

    const int sel = game.optionsSelection();

    auto drawOpt = [&](int idx, const std::string& label, const std::string& value) {
        const Color c = (idx == sel) ? white : gray;
        std::stringstream ss;
        ss << (idx == sel ? "> " : "  ");
        ss << label;
        if (!value.empty()) ss << ": " << value;
        drawText5x7(renderer, x0 + 16, y, scale, c, ss.str());
        y += 18;
    };

    drawOpt(0, "AUTO-PICKUP", autoPickupLabel(game.autoPickupMode()));
    drawOpt(1, "AUTO-STEP DELAY", std::to_string(game.autoStepDelayMs()) + "ms");
    drawOpt(2, "AUTO-EXPLORE SEARCH", yesNo(game.autoExploreSearchEnabled()));
    drawOpt(3, "AUTOSAVE", (game.autosaveEveryTurns() > 0 ? ("EVERY " + std::to_string(game.autosaveEveryTurns()) + " TURNS") : "OFF"));
    drawOpt(4, "IDENTIFY ITEMS", yesNo(game.identificationEnabled()));
    drawOpt(5, "HUNGER SYSTEM", yesNo(game.hungerEnabled()));
    drawOpt(6, "ENCUMBRANCE", yesNo(game.encumbranceEnabled()));
    drawOpt(7, "LIGHTING", yesNo(game.lightingEnabled()));
    drawOpt(8, "YENDOR DOOM", yesNo(game.yendorDoomEnabled()));
    drawOpt(9, "EFFECT TIMERS", yesNo(game.showEffectTimers()));
    drawOpt(10, "CONFIRM QUIT", yesNo(game.confirmQuitEnabled()));
    drawOpt(11, "AUTO MORTEM", yesNo(game.autoMortemEnabled()));
    drawOpt(12, "BONES FILES", yesNo(game.bonesEnabled()));
    drawOpt(13, "SAVE BACKUPS", (game.saveBackups() > 0 ? std::to_string(game.saveBackups()) : "OFF"));
    drawOpt(14, "UI THEME", uiThemeLabel(game.uiTheme()));
    drawOpt(15, "UI PANELS", (game.uiPanelsTextured() ? "TEXTURED" : "SOLID"));
    drawOpt(16, "3D SPRITES", yesNo(game.voxelSpritesEnabled()));
    drawOpt(17, "ISO CUTAWAY", yesNo(game.isoCutawayEnabled()));
    drawOpt(18, "CONTROL PRESET", game.controlPresetDisplayName());
    drawOpt(19, "KEYBINDS", "");
    drawOpt(20, "CLOSE", "");

    y += 14;
    drawText5x7(renderer, x0 + 16, y, scale, gray,
        "LEFT/RIGHT: change | ENTER: toggle/next/open | ESC: close");
}

void Renderer::drawKeybindsOverlay(const Game& game) {
    const int panelW = std::min(winW - 80, 980);
    const int panelH = std::min(winH - 80, 640);
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - panelH) / 2;

    SDL_Rect bg{x0, y0, panelW, panelH};
    drawPanel(game, bg, 220, lastFrame);

    const Color white{240, 240, 240, 255};
    const Color gray{160, 160, 160, 255};
    const Color yellow{255, 230, 120, 255};
    const Color warn{255, 170, 120, 255};

    const int scale = 2;

    int y = y0 + 16;
    drawText5x7(renderer, x0 + 16, y, scale, yellow, "KEYBINDS");
    y += 24;

    const auto& rows = game.keybindsDescription(); // full list (action -> chords)
    const int total = static_cast<int>(rows.size());

    // Visible subset (filtered).
    std::vector<int> vis;
    game.keybindsBuildVisibleIndices(vis);
    const int n = static_cast<int>(vis.size());

    const int sel = game.keybindsSelection(); // selection within the visible subset
    const int scroll = game.keybindsScroll();

    auto upperSpaces = [&](std::string s) {
        for (char& ch : s) {
            if (ch == '_') ch = ' ';
            else ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        return s;
    };

    auto trim = [&](std::string s) {
        auto notSpace = [](unsigned char c) { return !std::isspace(c); };
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
        s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
        return s;
    };

    auto toLowerLocal = [&](std::string s) {
        for (char& ch : s) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return s;
    };

    auto splitComma = [&](const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : s) {
            if (ch == ',') {
                out.push_back(trim(cur));
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        out.push_back(trim(cur));
        out.erase(std::remove_if(out.begin(), out.end(),
            [](const std::string& t) { return t.empty(); }), out.end());
        return out;
    };

    auto fit = [&](const std::string& s, int maxChars) -> std::string {
        if (maxChars <= 0) return "";
        if (static_cast<int>(s.size()) <= maxChars) return s;
        if (maxChars <= 3) return s.substr(0, maxChars);
        return s.substr(0, maxChars - 3) + "...";
    };

    // Build conflict flags (token appears in more than one action).
    std::vector<bool> hasConflict;
    hasConflict.assign(rows.size(), false);

    std::vector<std::pair<std::string, int>> chordPairs;
    chordPairs.reserve(static_cast<size_t>(total) * 2u);

    for (int i = 0; i < total; ++i) {
        for (const auto& tok : splitComma(rows[i].second)) {
            const std::string t = toLowerLocal(trim(tok));
            if (t.empty()) continue;
            if (t == "none" || t == "unbound" || t == "disabled") continue;
            chordPairs.push_back({t, i});
        }
    }

    std::sort(chordPairs.begin(), chordPairs.end(),
        [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first < b.first;
            return a.second < b.second;
        });

    for (size_t j = 0; j < chordPairs.size();) {
        size_t k = j + 1;
        while (k < chordPairs.size() && chordPairs[k].first == chordPairs[j].first) ++k;
        if (k - j > 1) {
            for (size_t m = j; m < k; ++m) {
                const int idx = chordPairs[m].second;
                if (idx >= 0 && idx < static_cast<int>(hasConflict.size())) hasConflict[idx] = true;
            }
        }
        j = k;
    }

    // Filter line.
    const int maxCharsScale1 = std::max(0, (panelW - 32) / 6); // 5x7 font: ~6px per char at scale1
    if (game.isKeybindsSearchMode() || !game.keybindsSearchQuery().empty()) {
        std::stringstream ss;
        ss << (game.isKeybindsSearchMode() ? "FILTER> " : "FILTER: ");
        ss << game.keybindsSearchQuery();
        ss << " (" << n << "/" << total << ")";
        drawText5x7(renderer, x0 + 16, y, 1, gray, fit(ss.str(), maxCharsScale1));
        y += 18;
    }

    // Layout.
    const int lineH = 18;
    const int footerH = 58;
    const int headerPad = 6;
    const int listTop = y + headerPad;
    const int listH = panelH - (listTop - y0) - footerH;
    const int visibleRows = std::max(1, listH / lineH);

    const int start = clampi(scroll, 0, std::max(0, n - visibleRows));
    int yy = listTop;

    if (total <= 0) {
        drawText5x7(renderer, x0 + 16, yy, scale, warn, "NO KEYBINDS DATA (TRY REOPENING OPTIONS).");
    } else if (n <= 0) {
        drawText5x7(renderer, x0 + 16, yy, scale, warn, "NO MATCHING ACTIONS (CTRL+L TO CLEAR FILTER).");
    } else {
        // Column sizing (monospace-ish 5x7): ~6px per char at scale1.
        const int maxCharsTotal = std::max(0, (panelW - 32) / (6 * scale));
        const int labelChars = 20;
        const int valueChars = std::max(0, maxCharsTotal - 4 - labelChars); // 4 for prefix + spaces

        for (int vi = start; vi < n && vi < start + visibleRows; ++vi) {
            const int idx = vis[vi];
            const bool conflict = (idx >= 0 && idx < static_cast<int>(hasConflict.size())) ? hasConflict[idx] : false;

            const Color c = (vi == sel) ? white : (conflict ? warn : gray);

            std::string label = upperSpaces(rows[idx].first);
            // Show human-friendly key labels in the UI (keep raw tokens in settings).
            std::string val = chordListToDisplay(rows[idx].second);

            // Build a padded label column for alignment.
            label = fit(label, labelChars);
            if (static_cast<int>(label.size()) < labelChars) {
                label += std::string(labelChars - label.size(), ' ');
            }

            const std::string prefix = (vi == sel) ? "> " : "  ";
            const std::string line = prefix + label + " : " + fit(val, valueChars);

            drawText5x7(renderer, x0 + 16, yy, scale, c, line);
            yy += lineH;
        }
    }

    // Footer / instructions
    int fy = y0 + panelH - footerH + 10;
    drawText5x7(renderer, x0 + 16, fy, 1, gray,
        fit("UP/DOWN SELECT  ENTER REBIND  RIGHT ADD  LEFT RESET  DEL UNBIND  / FILTER  ESC BACK", maxCharsScale1));

    fy += 16;

    if (game.isKeybindsCapturing()) {
        const int capIdx = game.keybindsCaptureActionIndex();
        std::string target = "UNKNOWN";
        if (capIdx >= 0 && capIdx < total) target = upperSpaces(rows[capIdx].first);

        const std::string mode = game.keybindsCaptureAddMode() ? "ADD" : "REPLACE";
        drawText5x7(renderer, x0 + 16, fy, 2, warn, "PRESS KEY: " + target + " (" + mode + ")");
    } else if (game.isKeybindsSearchMode()) {
        drawText5x7(renderer, x0 + 16, fy, 1, gray,
            fit("TYPE TO FILTER. ENTER/ESC DONE. CTRL+L CLEAR.", maxCharsScale1));
    } else if (!game.keybindsSearchQuery().empty()) {
        drawText5x7(renderer, x0 + 16, fy, 1, gray,
            fit("FILTER ACTIVE. PRESS / TO EDIT. CTRL+L CLEAR. CONFLICTS HIGHLIGHTED.", maxCharsScale1));
    } else {
        // Context line: show a short description of the currently selected action.
        std::string infoLine;
        if (n > 0) {
            const int ssel = std::clamp(sel, 0, n - 1);
            const int idx = vis[ssel];
            if (idx >= 0 && idx < total) {
                const std::string tok = rows[idx].first;
                if (auto act = actioninfo::parse(tok)) {
                    const char* d = actioninfo::desc(*act);
                    if (d && d[0] != '\0') {
                        infoLine = std::string("INFO: ") + d;
                    }
                }
            }
        }

        if (infoLine.empty()) infoLine = "CONFLICTS HIGHLIGHTED";
        infoLine += ". TIP: EXT CMD #bind / #unbind / #binds.";

        drawText5x7(renderer, x0 + 16, fy, 1, gray,
            fit(infoLine, maxCharsScale1));
    }
}

void Renderer::drawCommandOverlay(const Game& game) {
    // Base height for the prompt + hint row.
    const int baseH = 52;

    // If TAB completion produced a match set, show a small dropdown list.
    const auto& matches = game.commandAutocompleteMatches();
    const auto& hints = game.commandAutocompleteHints();
    const auto& descs = game.commandAutocompleteDescs();
    const int show = std::clamp(static_cast<int>(matches.size()), 0, 8);
    const int sel = game.commandAutocompleteIndex();
    int start = 0;
    if (sel >= 0 && static_cast<int>(matches.size()) > show) {
        start = std::clamp(sel - show / 2, 0, static_cast<int>(matches.size()) - show);
    }
    const bool above = (show > 0 && start > 0);
    const bool below = (show > 0 && start + show < static_cast<int>(matches.size()));
    const int lineH = 10; // 5x7 at scale1 + spacing

    // Selected-item description line (drawn under the dropdown).
    const int infoIdx = (sel >= 0) ? sel : ((show > 0) ? start : -1);
    std::string info;
    if (infoIdx >= 0 && infoIdx < static_cast<int>(descs.size())) {
        info = descs[static_cast<size_t>(infoIdx)];
    }
    const bool showInfo = !info.empty();

    const int extraLines = show + (above ? 1 : 0) + (below ? 1 : 0) + (showInfo ? 1 : 0);
    const int extraH = (extraLines > 0) ? (6 + extraLines * lineH) : 0;

    const int barH = baseH + extraH;
    int y0 = winH - hudH - barH - 10;
    if (y0 < 10) y0 = 10;

    SDL_Rect bg{10, y0, winW - 20, barH};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 220);
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    SDL_RenderDrawRect(renderer, &bg);

    const int pad = 10;
    const int x = bg.x + pad;
    int y = bg.y + 8;

    // Local UI palette.
    const Color white{255, 255, 255, 255};
    const Color gray{180, 180, 180, 255};

    // Fit the command string to the bar width and keep the caret visible.
    const int maxChars2 = std::max(0, (bg.w - 2 * pad) / (6 * 2)); // scale2
    const int maxChars1 = std::max(0, (bg.w - 2 * pad) / 6);       // scale1

    auto fitHead1 = [&](const std::string& s) -> std::string {
        if (static_cast<int>(s.size()) <= maxChars1) return s;
        if (maxChars1 <= 3) return s.substr(0, static_cast<size_t>(maxChars1));
        return s.substr(0, static_cast<size_t>(maxChars1 - 3)) + "...";
    };

    auto fitAroundCaret2 = [&](const std::string& s, size_t caretPos, int maxChars) -> std::string {
        if (maxChars <= 0) return std::string();
        if (static_cast<int>(s.size()) <= maxChars) return s;

        size_t start = 0;
        const size_t half = static_cast<size_t>(maxChars / 2);
        if (caretPos > half) start = caretPos - half;
        if (start + static_cast<size_t>(maxChars) > s.size()) start = s.size() - static_cast<size_t>(maxChars);

        std::string out = s.substr(start, static_cast<size_t>(maxChars));
        if (start > 0 && maxChars >= 3) {
            out.replace(0, 3, "...");
        }
        if (start + static_cast<size_t>(maxChars) < s.size() && maxChars >= 3) {
            out.replace(static_cast<size_t>(maxChars - 3), 3, "...");
        }
        return out;
    };

    const std::string prefix = "EXT CMD: ";
    const std::string& rawBuf = game.commandBuffer();
    const size_t cur = static_cast<size_t>(std::clamp(game.commandCursorByte(), 0, static_cast<int>(rawBuf.size())));

    std::string withCaret = rawBuf;
    withCaret.insert(cur, "|");
    const size_t caretPos = cur;

    const int bodyMax = std::max(0, maxChars2 - static_cast<int>(prefix.size()));
    const std::string body = fitAroundCaret2(withCaret, caretPos, bodyMax);
    drawText5x7(renderer, x, y, 2, white, prefix + body);

    y += 24;
    {
        std::string hint = "ENTER RUN  ESC CANCEL  TAB COMPLETE (CMD/ARGS)";
        if (game.commandAutocompleteFuzzy()) hint += " (FUZZY)";
        hint += "  CTRL+B/F MOVE  CTRL+P/N HISTORY  LEFT/RIGHT EDIT  HOME/END  DEL/CTRL+D FWD  CTRL+W WORD  CTRL+U START  CTRL+K END  CTRL+L CLEAR";
        drawText5x7(renderer, x, y, 1, gray, fitHead1(hint));
    }

    // Dropdown list for TAB completion matches.
    if (show > 0) {
        y += 12;
        auto buildLine = [&](bool isSel, const std::string& cmd, const std::string& hintTok) -> std::string {
            const std::string prefix = isSel ? "> " : "  ";
            std::string hintStr;
            if (!hintTok.empty()) hintStr = "[" + hintTok + "]";

            const int lineMax = maxChars1;
            const int avail = std::max(0, lineMax - static_cast<int>(prefix.size()));

            int cmdMax = avail;
            if (!hintStr.empty()) {
                cmdMax = std::max(0, avail - 1 - static_cast<int>(hintStr.size()));
            }

            std::string cmdFit = fitToChars(cmd, cmdMax);
            std::string out = prefix + cmdFit;

            if (!hintStr.empty()) {
                const int used = static_cast<int>(cmdFit.size()) + static_cast<int>(hintStr.size());
                int spaces = avail - used;
                if (spaces < 1) {
                    out += " " + hintStr;
                    out = fitToChars(out, lineMax);
                } else {
                    out += std::string(static_cast<size_t>(spaces), ' ') + hintStr;
                }
            }

            return out;
        };

        if (above) {
            drawText5x7(renderer, x, y, 1, gray, fitHead1("... (" + std::to_string(start) + " above)"));
            y += lineH;
        }

        for (int i = 0; i < show; ++i) {
            const int idx = start + i;
            const bool isSel = (sel >= 0 && idx == sel);
            const Color col = isSel ? white : gray;
            const std::string& cmd = matches[static_cast<size_t>(idx)];
            const std::string hintTok = (idx >= 0 && idx < static_cast<int>(hints.size())) ? hints[static_cast<size_t>(idx)] : std::string();
            const std::string line = buildLine(isSel, cmd, hintTok);
            drawText5x7(renderer, x, y, 1, col, fitHead1(line));
            y += lineH;
        }

        if (below) {
            const int remain = static_cast<int>(matches.size()) - (start + show);
            drawText5x7(renderer, x, y, 1, gray, fitHead1("... (+" + std::to_string(remain) + ")"));
            y += lineH;
        }

        if (showInfo) {
            drawText5x7(renderer, x, y, 1, gray, fitHead1("INFO: " + info));
        }
    }
}


void Renderer::drawPerfOverlay(const Game& game) {
    (void)game;
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const Color white{255, 255, 255, 255};
    const Color gray{200, 200, 200, 255};

    // Use cached strings (updated at a low rate in render()) to avoid per-frame churn.
    const std::string& l1 = perfLine1_;
    const std::string& l2 = perfLine2_;
    const std::string& l3 = perfLine3_;

    const int scale = 1;
    const int pad = 6;
    const int lineH = 10 * scale; // 5x7 font + spacing
    const int charW = 6 * scale;

    int maxChars = 0;
    maxChars = std::max(maxChars, static_cast<int>(l1.size()));
    maxChars = std::max(maxChars, static_cast<int>(l2.size()));
    maxChars = std::max(maxChars, static_cast<int>(l3.size()));

    // Keep compact and avoid covering too much of the map.
    const int w = std::clamp(pad * 2 + maxChars * charW, 120, winW - 16);
    const int h = pad * 2 + 3 * lineH + 2;
    const int x = 8;
    const int y = 8;

    SDL_Rect bg{x, y, w, h};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 80);
    SDL_RenderDrawRect(renderer, &bg);

    int ty = y + pad;
    if (!l1.empty()) { drawText5x7(renderer, x + pad, ty, scale, white, l1); ty += lineH; }
    if (!l2.empty()) { drawText5x7(renderer, x + pad, ty, scale, gray, l2); ty += lineH; }
    if (!l3.empty()) { drawText5x7(renderer, x + pad, ty, scale, gray, l3); ty += lineH; }
}


void Renderer::drawHelpOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const Color white{255, 255, 255, 255};
    const Color gray{180, 180, 180, 255};

    const int panelW = std::min(winW - 80, 820);
    const int panelH = std::min(520, winH - 40);
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - panelH) / 2;
    const int pad = 14;

    SDL_Rect bg{x0, y0, panelW, panelH};
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 200);
    SDL_RenderFillRect(renderer, &bg);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
    SDL_RenderDrawRect(renderer, &bg);

    int y = y0 + pad;
    drawText5x7(renderer, x0 + pad, y, 2, white, "HELP");
    y += 22;

    const int scale = 2;
    const int charW = 6 * scale;
    const int lineH = 18;

    const int maxChars = std::max(1, (panelW - pad * 2) / std::max(1, charW));

    struct Line {
        std::string text;
        Color color;
    };

    std::vector<Line> raw;
    raw.reserve(128);

    auto add = [&](const std::string& t, const Color& c) {
        raw.push_back(Line{t, c});
    };
    auto blank = [&]() {
        raw.push_back(Line{"", gray});
    };

    add("CONTROLS:", white);
    if (game.controlPreset() == ControlPreset::Nethack) {
        add("MOVE: HJKL + YUBN (ARROWS/NUMPAD OK)", gray);
        add("SPACE/. WAIT  R REST  SHIFT+N SNEAK (STEALTH)  < > STAIRS", gray);
        add("F FIRE  G/, PICKUP  I/TAB INVENTORY", gray);
        add("D DIG  CTRL+D KICK  :/V LOOK  S SEARCH  T DISARM  C CLOSE  SHIFT+C LOCK", gray);
    } else {
        add("MOVE: WASD / ARROWS / NUMPAD + Q/E/Z/C DIAGONALS", gray);
        add("SPACE/. WAIT  R REST  N SNEAK (STEALTH)  < > STAIRS", gray);
        add("F FIRE  G/, PICKUP  I/TAB INVENTORY", gray);
        add("D DIG  B KICK  L/V LOOK  SHIFT+C SEARCH  T DISARM  K CLOSE  SHIFT+K LOCK", gray);
    }
    add("O EXPLORE  P AUTOPICKUP  M MINIMAP  SHIFT+TAB STATS", gray);
    add("MINIMAP: MOVE CURSOR (ARROWS/WASD), [ ] ZOOM, ENTER TRAVEL, L/RMB LOOK, LMB TRAVEL", gray);
    add("F2 OPTIONS  #/CTRL+P EXTENDED COMMANDS  (TAB COMPLETE CMD+ARGS, LEFT/RIGHT EDIT)", gray);
    add("F5 SAVE  F9 LOAD  F10 LOAD AUTO  F6 RESTART", gray);
    add("F11 FULLSCREEN  F12 SCREENSHOT (BINDABLE)", gray);
    add("SHIFT+F10 PERF OVERLAY (BINDABLE)", gray);
    add("F3/SHIFT+M MESSAGE HISTORY  (/ SEARCH, CTRL+L CLEAR)", gray);
    add("F4 MONSTER CODEX  (TAB SORT, LEFT/RIGHT FILTER)", gray);
    add("\\ DISCOVERIES  (TAB/LEFT/RIGHT FILTER, SHIFT+S SORT)", gray);
    add("PGUP/PGDN LOG  ESC CANCEL/QUIT", gray);

    blank();
    add("EXTENDED COMMAND EXAMPLES:", white);
    add("save | load | loadauto | quit | version | seed | name | scores | perf", gray);
    add("autopickup off/gold/all", gray);
    add("mark [note|danger|loot] <label>  marks  travel <index|label>", gray);
    add("name <text>  scores [N]", gray);
    add("autosave <turns>  stepdelay <ms>  identify on/off  timers on/off", gray);
    add("pray [heal|cure|identify|bless|uncurse]", gray);
    add("pay  (IN SHOP / AT CAMP)   debt/ledger  (SHOW SHOP DEBTS)", gray);

    blank();
    add("KEYBINDINGS:", white);

    auto baseName = [](const std::string& p) -> std::string {
        if (p.empty()) return {};
        size_t i = p.find_last_of("/\\");
        if (i == std::string::npos) return p;
        return p.substr(i + 1);
    };
    const std::string settingsFile = baseName(game.settingsPath());
    if (!settingsFile.empty()) add("EDIT " + settingsFile + " (bind_*)", gray);
    else add("EDIT procrogue_settings.ini (bind_*)", gray);

    // Current binds (pulled from the runtime table so this panel stays in sync with user overrides).
    auto bindFor = [&](const char* token) -> std::string {
        for (const auto& row : game.keybindsDescription()) {
            if (row.first == token) return row.second;
        }
        return "unbound";
    };

    add("HELP: " + chordListToDisplay(bindFor("help")), gray);
    add("OPTIONS: " + chordListToDisplay(bindFor("options")) + "   EXT CMD: " + chordListToDisplay(bindFor("command")), gray);
    add("INVENTORY: " + chordListToDisplay(bindFor("inventory")) + "   LOOK: " + chordListToDisplay(bindFor("look")) + "   SEARCH: " + chordListToDisplay(bindFor("search")), gray);
    add("MINIMAP: " + chordListToDisplay(bindFor("toggle_minimap")) + "   STATS: " + chordListToDisplay(bindFor("toggle_stats")) + "   MSGS: " + chordListToDisplay(bindFor("message_history")), gray);
    add("LOOK LENSES: SOUND " + chordListToDisplay(bindFor("sound_preview")) + "   HEARING " + chordListToDisplay(bindFor("hearing_preview")) + "   THREAT " + chordListToDisplay(bindFor("threat_preview")), gray);

    blank();
    add("TIPS:", white);
    add("SEARCH CAN REVEAL TRAPS AND SECRET DOORS. EXT: #SEARCH N [ALL]", gray);
    add("LOCKED DOORS: USE KEYS, LOCKPICKS, A SCROLL OF KNOCK, OR KICK THEM IN (RISKY).", gray);
    add("KICKING CHESTS MAY TRIGGER TRAPS AND CAN SLIDE THEM.", gray);
    add("OPEN CHESTS CAN STORE ITEMS: ENTER OPENS, ENTER MOVES STACK, D MOVES 1, G MOVES ALL.", gray);
    add("SOME VAULT DOORS MAY BE TRAPPED.", gray);
    add("AUTO-EXPLORE STOPS IF YOU SEE AN ENEMY OR GET HURT/DEBUFFED.", gray);
    add("INVENTORY: E EQUIP  U USE  X DROP  SHIFT+X DROP ALL", gray);
    add("SCROLL THE MESSAGE LOG WITH PGUP/PGDN.", gray);

    // Simple word wrap (ASCII-ish) with hard breaks for long tokens.
    auto wrap = [&](const std::string& s, int maxCharsLocal) -> std::vector<std::string> {
        std::vector<std::string> out;
        if (maxCharsLocal <= 0) {
            out.push_back("");
            return out;
        }

        size_t pos = 0;
        while (pos < s.size()) {
            // Skip leading spaces for the next line.
            while (pos < s.size() && s[pos] == ' ') ++pos;
            if (pos >= s.size()) break;

            size_t end = std::min(s.size(), pos + static_cast<size_t>(maxCharsLocal));
            if (end >= s.size()) {
                out.push_back(s.substr(pos));
                break;
            }

            // Prefer breaking on the last space inside the window.
            size_t space = s.rfind(' ', end);
            if (space != std::string::npos && space > pos) {
                end = space;
            }

            std::string line = s.substr(pos, end - pos);
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out.push_back(line);

            pos = end;
        }

        if (out.empty()) out.push_back("");
        return out;
    };

    // Wrap all lines to the panel width.
    std::vector<Line> lines;
    lines.reserve(raw.size() * 2);

    for (const auto& ln : raw) {
        if (ln.text.empty()) {
            lines.push_back(ln);
            continue;
        }
        const auto parts = wrap(ln.text, maxChars);
        for (const auto& p : parts) {
            lines.push_back(Line{p, ln.color});
        }
    }

    // Footer hint (always visible).
    const int footerH = 16;
    const int footerY = y0 + panelH - pad - footerH + 2;
    drawText5x7(renderer, x0 + pad, footerY, 1, gray,
                "UP/DOWN scroll  PGUP/PGDN page  ENTER top  ESC close");

    // Content viewport bounds (above footer).
    const int contentTop = y;
    const int contentBottom = footerY - 6;
    const int availH = std::max(0, contentBottom - contentTop);
    const int maxLines = std::max(1, availH / lineH);

    const int totalLines = static_cast<int>(lines.size());
    const int maxStart = std::max(0, totalLines - maxLines);

    int start = game.helpScrollLines();
    start = std::clamp(start, 0, maxStart);

    // Draw visible lines.
    int yy = contentTop;
    for (int i = 0; i < maxLines; ++i) {
        const int li = start + i;
        if (li < 0 || li >= totalLines) break;
        const auto& ln = lines[static_cast<size_t>(li)];
        drawText5x7(renderer, x0 + pad, yy, scale, ln.color, ln.text);
        yy += lineH;
    }

    // Scroll indicator (right aligned).
    if (totalLines > maxLines) {
        const int page = (start / std::max(1, maxLines)) + 1;
        const int pages = (totalLines + maxLines - 1) / std::max(1, maxLines);

        std::ostringstream ss;
        ss << "PAGE " << page << "/" << pages;

        const std::string txt = ss.str();
        const int txtW = static_cast<int>(txt.size()) * (6 * 1);
        drawText5x7(renderer, x0 + panelW - pad - txtW, footerY, 1, gray, txt);

        // Simple scrollbar on the right edge.
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        const int barX = x0 + panelW - pad / 2;
        const int trackTop = contentTop;
        const int trackH = std::max(1, availH);

        SDL_Rect track{barX, trackTop, 2, trackH};
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 40);
        SDL_RenderFillRect(renderer, &track);

        const float denom = std::max(1.0f, static_cast<float>(totalLines));
        const float t0 = static_cast<float>(start) / denom;
        float t1 = static_cast<float>(start + maxLines) / denom;
        if (t1 > 1.0f) t1 = 1.0f;

        const int thumbY0 = trackTop + static_cast<int>(t0 * trackH);
        const int thumbY1 = trackTop + static_cast<int>(t1 * trackH);

        SDL_Rect thumb{barX - 1, thumbY0, 4, std::max(6, thumbY1 - thumbY0)};
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 120);
        SDL_RenderFillRect(renderer, &thumb);
    }
}





void Renderer::drawMinimapOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const Color white{ 240, 240, 240, 255 };
    const Dungeon& d = game.dungeon();
    // Room type cache (minimap) — rebuilt if the dungeon changed.
    if (roomCacheDungeon != &d || roomCacheBranch != game.branch() || roomCacheDepth != game.depth() ||
        roomCacheW != d.width || roomCacheH != d.height || roomCacheRooms != d.rooms.size() ||
        roomTypeCache.size() != static_cast<size_t>(d.width * d.height)) {

        roomCacheDungeon = &d;
        roomCacheBranch = game.branch();
        roomCacheDepth = game.depth();
        roomCacheW = d.width;
        roomCacheH = d.height;
        roomCacheRooms = d.rooms.size();

        roomTypeCache.assign(static_cast<size_t>(d.width * d.height), static_cast<uint8_t>(RoomType::Normal));
        for (const Room& r : d.rooms) {
            for (int yy = r.y; yy < r.y2(); ++yy) {
                for (int xx = r.x; xx < r.x2(); ++xx) {
                    if (!d.inBounds(xx, yy)) continue;
                    roomTypeCache[static_cast<size_t>(yy * d.width + xx)] = static_cast<uint8_t>(r.type);
                }
            }
        }
    }

    const int W = d.width;
    const int H = d.height;

    // Choose a small per-tile pixel size that fits comfortably on screen.
    int px = 4 + game.minimapZoom();
    px = std::clamp(px, 2, 12);

    const int pad = 10;
    const int margin = 10;
    const int titleH = 30; // title + hint + info line

    // Don't let the minimap eat the whole window.
    const int maxW = winW / 2;
    const int maxH = (winH - hudH) / 2;
    while (px > 2 && (W * px + pad * 2) > maxW) px--;
    while (px > 2 && (H * px + pad * 2 + titleH) > maxH) px--;

    const int panelW = W * px + pad * 2;

    const int x0 = winW - panelW - margin;
    const int y0 = margin;

    const int panelH = H * px + pad * 2 + titleH;

    SDL_Rect panel { x0, y0, panelW, panelH };
    drawPanel(game, panel, 210, lastFrame);

    // Title
    drawText5x7(renderer, x0 + pad, y0 + 4, 2, white, "MINIMAP (M)");

    // Hint line (fit inside the title band).
    const Color gray{ 160, 160, 160, 255 };
    drawText5x7(renderer, x0 + pad, y0 + 4 + 14, 1, gray, "[]:ZOOM  LMB/ENTER:TRAVEL  RMB/L:LOOK");

    // Cursor coordinates + zoom (right aligned).
    if (game.minimapCursorActive()) {
        const Vec2i c = game.minimapCursor();
        const int z = game.minimapZoom();
        std::string ztxt = "Z";
        if (z >= 0) ztxt += "+";
        ztxt += std::to_string(z);

        const std::string coords = ztxt + "  " + std::to_string(c.x) + "," + std::to_string(c.y);
        const int charW = (5 + 1) * 1;
        const int textW = static_cast<int>(coords.size()) * charW;
        drawText5x7(renderer, x0 + panelW - pad - textW, y0 + 4 + 14, 1, gray, coords);
    }

    // Cursor info line (truncated to fit).
    if (game.minimapCursorActive()) {
        const Vec2i c = game.minimapCursor();
        std::string info = game.describeAt(c);

        const int charW = (5 + 1) * 1;
        const int maxChars = std::max(0, (panelW - pad * 2) / charW);
        if (maxChars > 0 && static_cast<int>(info.size()) > maxChars) {
            if (maxChars >= 3) info = info.substr(0, static_cast<size_t>(maxChars - 3)) + "...";
            else info = info.substr(0, static_cast<size_t>(maxChars));
        }

        drawText5x7(renderer, x0 + pad, y0 + 4 + 14 + 8, 1, gray, info);
    }

    const int mapX = x0 + pad;
    const int mapY = y0 + pad + titleH;

    auto drawCell = [&](int tx, int ty, uint8_t r, uint8_t g, uint8_t b, uint8_t a=255) {
        SDL_Rect rc { mapX + tx * px, mapY + ty * px, px, px };
        SDL_SetRenderDrawColor(renderer, r, g, b, a);
        SDL_RenderFillRect(renderer, &rc);
    };

    auto drawDot = [&](int tx, int ty, uint8_t r, uint8_t g, uint8_t b, uint8_t a=255) {
        const int dot = std::max(1, px / 2);
        SDL_Rect rc { mapX + tx * px + (px - dot) / 2, mapY + ty * px + (px - dot) / 2, dot, dot };
        SDL_SetRenderDrawColor(renderer, r, g, b, a);
        SDL_RenderFillRect(renderer, &rc);
    };

    // Tiles
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const Tile& t = d.at(x, y);
            if (!t.explored) {
                // Unexplored: don't draw (keep the background)
                continue;
            }

            const bool vis = t.visible;

            // Basic palette
            if (t.type == TileType::Wall) {
                if (vis) drawCell(x, y, 110, 110, 110);
                else     drawCell(x, y, 60, 60, 60);
            } else if (t.type == TileType::Pillar) {
                // Pillars are interior "walls"; show them slightly brighter so
                // they read as distinct from border stone.
                if (vis) drawCell(x, y, 130, 130, 130);
                else     drawCell(x, y, 75, 75, 75);
            } else if (t.type == TileType::Boulder) {
                // Boulders are pushable obstacles; display them darker than pillars.
                if (vis) drawCell(x, y, 95, 98, 104);
                else     drawCell(x, y, 55, 58, 62);
            } else if (t.type == TileType::Chasm) {
                // Chasms are impassable but not opaque.
                if (vis) drawCell(x, y, 20, 30, 55);
                else     drawCell(x, y, 12, 18, 32);
            } else if (t.type == TileType::DoorClosed) {
                if (vis) drawCell(x, y, 160, 110, 60);
                else     drawCell(x, y, 90, 70, 40);
            } else if (t.type == TileType::DoorLocked) {
                // Slightly more "warning" tint than a normal closed door.
                if (vis) drawCell(x, y, 180, 90, 70);
                else     drawCell(x, y, 100, 60, 50);
            } else if (t.type == TileType::DoorOpen) {
                if (vis) drawCell(x, y, 140, 120, 90);
                else     drawCell(x, y, 80, 70, 55);
            } else if (t.type == TileType::StairsDown || t.type == TileType::StairsUp) {
                if (vis) drawCell(x, y, 220, 220, 120);
                else     drawCell(x, y, 120, 120, 80);
            } else {
                // Floor/other passable (tinted by discovered room type)
                const size_t ii = static_cast<size_t>(y * W + x);
                const uint8_t rt = (ii < roomTypeCache.size()) ? roomTypeCache[ii] : static_cast<uint8_t>(RoomType::Normal);

                uint8_t r = 30, g = 30, b = 30;
                switch (static_cast<RoomType>(rt)) {
                    case RoomType::Treasure: r = 55; g = 45; b = 22; break;
                    case RoomType::Shrine:   r = 25; g = 35; b = 58; break;
                    case RoomType::Lair:     r = 24; g = 42; b = 24; break;
                    case RoomType::Secret:   r = 40; g = 26; b = 45; break;
                    case RoomType::Vault:    r = 30; g = 38; b = 58; break;
                    case RoomType::Shop:     r = 45; g = 35; b = 24; break;
                    case RoomType::Normal:
                    default: break;
                }

                if (vis) {
                    drawCell(x, y, r, g, b);
                } else {
                    drawCell(x, y, static_cast<uint8_t>(std::max<int>(10, r / 2)),
                                   static_cast<uint8_t>(std::max<int>(10, g / 2)),
                                   static_cast<uint8_t>(std::max<int>(10, b / 2)));
                }
            }
        }
    }

    // Room outlines (only if at least one tile has been explored).
    auto outlineColor = [&](RoomType rt) -> Color {
        switch (rt) {
            case RoomType::Treasure: return Color{220, 200, 120, 90};
            case RoomType::Shrine:   return Color{140, 200, 255, 90};
            case RoomType::Lair:     return Color{140, 220, 140, 90};
            case RoomType::Secret:   return Color{220, 140, 255, 90};
            case RoomType::Vault:    return Color{200, 220, 255, 90};
            case RoomType::Shop:     return Color{220, 180, 120, 90};
            case RoomType::Normal:
            default:                 return Color{160, 160, 160, 70};
        }
    };

    for (const Room& r : d.rooms) {
        bool discovered = false;
        for (int yy = r.y; yy < r.y2() && !discovered; ++yy) {
            for (int xx = r.x; xx < r.x2(); ++xx) {
                if (!d.inBounds(xx, yy)) continue;
                if (d.at(xx, yy).explored) { discovered = true; break; }
            }
        }
        if (!discovered) continue;

        const Color c = outlineColor(r.type);
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
        SDL_Rect rr { mapX + r.x * px, mapY + r.y * px, r.w * px, r.h * px };
        SDL_RenderDrawRect(renderer, &rr);
    }

    // Traps (discovered only; explored tiles only).
    for (const auto& tr : game.traps()) {
        if (!tr.discovered) continue;
        if (!d.inBounds(tr.pos.x, tr.pos.y)) continue;
        const Tile& t = d.at(tr.pos.x, tr.pos.y);
        if (!t.explored) continue;

        const bool vis = t.visible;
        uint8_t r = 220, g = 160, b = 220;
        switch (tr.kind) {
            case TrapKind::Spike:          r = 255; g = 140; b = 80;  break;
            case TrapKind::PoisonDart:     r = 120; g = 255; b = 120; break;
            case TrapKind::Teleport:       r = 200; g = 120; b = 255; break;
            case TrapKind::Alarm:          r = 255; g = 255; b = 140; break;
            case TrapKind::Web:            r = 235; g = 235; b = 235; break;
            case TrapKind::ConfusionGas:   r = 120; g = 180; b = 255; break;
            case TrapKind::RollingBoulder: r = 190; g = 150; b = 110; break;
            case TrapKind::TrapDoor:       r = 150; g = 150; b = 150; break;
            case TrapKind::LetheMist:      r = 140; g = 255; b = 255; break;
            case TrapKind::PoisonGas:      r = 90;  g = 220; b = 90;  break;
            default: break;
        }

        // Fade traps in the fog-of-war.
        if (!vis) {
            r = static_cast<uint8_t>(std::max<int>(40, r / 2));
            g = static_cast<uint8_t>(std::max<int>(40, g / 2));
            b = static_cast<uint8_t>(std::max<int>(40, b / 2));
        }

        drawDot(tr.pos.x, tr.pos.y, r, g, b, 220);
    }

    // Player map markers / notes (explored tiles only).
    for (const auto& m : game.mapMarkers()) {
        if (!d.inBounds(m.pos.x, m.pos.y)) continue;
        const Tile& t = d.at(m.pos.x, m.pos.y);
        if (!t.explored) continue;

        const bool vis = t.visible;
        uint8_t r = 230, g = 230, b = 230;
        switch (m.kind) {
            case MarkerKind::Danger: r = 255; g = 80;  b = 80;  break;
            case MarkerKind::Loot:   r = 255; g = 220; b = 120; break;
            case MarkerKind::Note:
            default:                 r = 230; g = 230; b = 230; break;
        }

        // Fade markers in the fog-of-war (still visible, but less prominent).
        if (!vis) {
            r = static_cast<uint8_t>(std::max<int>(40, r / 2));
            g = static_cast<uint8_t>(std::max<int>(40, g / 2));
            b = static_cast<uint8_t>(std::max<int>(40, b / 2));
        }

        drawCell(m.pos.x, m.pos.y, r, g, b, 220);
    }

    // Entities (only show visible monsters; always show player)
    const Entity& p = game.player();
    drawCell(p.pos.x, p.pos.y, 60, 180, 255);

    for (const auto& e : game.entities()) {
        if (e.id == p.id) continue;
        if (e.hp <= 0) continue;
        const Tile& t = d.at(e.pos.x, e.pos.y);
        if (!t.visible) continue;
        drawCell(e.pos.x, e.pos.y, 255, 80, 80);
    }

    // Viewport indicator (camera): draw the currently visible map region on the minimap.
    {
        const int vw = std::min(viewTilesW, W);
        const int vh = std::min(viewTilesH, H);
        if (vw > 0 && vh > 0) {
            const int vx = std::clamp(camX, 0, std::max(0, W - vw));
            const int vy = std::clamp(camY, 0, std::max(0, H - vh));

            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 180);
            SDL_Rect vr { mapX + vx * px, mapY + vy * px, vw * px, vh * px };
            SDL_RenderDrawRect(renderer, &vr);

            // Slightly thicker border for readability (if space allows).
            SDL_Rect vr2 { vr.x - 1, vr.y - 1, vr.w + 2, vr.h + 2 };
            SDL_RenderDrawRect(renderer, &vr2);
        }
    }

    // Minimap cursor highlight (UI-only)
    if (game.minimapCursorActive()) {
        const Vec2i c = game.minimapCursor();
        if (d.inBounds(c.x, c.y)) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
            SDL_Rect rc { mapX + c.x * px, mapY + c.y * px, px, px };
            SDL_RenderDrawRect(renderer, &rc);
            // Slightly thicker border when the minimap is large enough.
            if (px >= 4) {
                SDL_Rect rc2 { rc.x - 1, rc.y - 1, rc.w + 2, rc.h + 2 };
                SDL_RenderDrawRect(renderer, &rc2);
            }
        }
    }
}

void Renderer::drawStatsOverlay(const Game& game) {
    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};

    // Center panel
    const int panelW = winW * 4 / 5;
    const int panelH = (winH - hudH) * 4 / 5;
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - hudH - panelH) / 2;

    SDL_Rect panel { x0, y0, panelW, panelH };
    drawPanel(game, panel, 230, lastFrame);

    const int pad = 14;
    int y = y0 + pad;

    drawText5x7(renderer, x0 + pad, y, 2, white, "STATS / RUN HISTORY (TAB)");
    y += 22;

    const Entity& p = game.player();

    // Run summary
    {
        std::stringstream ss;
        ss << (game.isGameWon() ? "RESULT: WIN" : (game.isGameOver() ? "RESULT: DEAD" : "RESULT: IN PROGRESS"));
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        ss << "SEED: " << game.seed();
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        ss << "CLASS: " << game.playerClassDisplayName();
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        if (game.atCamp()) ss << "DEPTH: CAMP  (DEEPEST: " << game.maxDepthReached() << ")";
        else ss << "DEPTH: " << game.depth() << "/" << game.dungeonMaxDepth() << "  (DEEPEST: " << game.maxDepthReached() << ")";
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        ss << "TURNS: " << game.turns() << "  KILLS: " << game.kills() << "  GOLD: " << game.goldCount() << "  KEYS: " << game.keyCount() << "  PICKS: " << game.lockpickCount();
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        ss << "HP: " << p.hp << "/" << p.hpMax << "  LV: " << game.playerCharLevel()
           << "  XP: " << game.playerXp() << "/" << game.playerXpToNext();
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        ss << "TALENTS: M" << game.playerMight()
           << " A" << game.playerAgility()
           << " V" << game.playerVigor()
           << " F" << game.playerFocus();
        if (game.pendingTalentPoints() > 0) ss << "  (PENDING: " << game.pendingTalentPoints() << ")";
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 18;
    }
    {
        std::stringstream ss;
        std::string c = game.runConductsTag();
        if (c.empty()) c = "NONE";
        ss << "CONDUCTS: " << c;
        // Use a smaller scale so the full tag list fits comfortably.
        drawText5x7(renderer, x0 + pad, y, 1, white, ss.str());
        y += 12;
    }

    {
        std::stringstream ss;
        if (game.autosaveEveryTurns() > 0) {
            ss << "AUTOSAVE: every " << game.autosaveEveryTurns() << " turns (" << game.defaultAutosavePath() << ")";
        } else {
            ss << "AUTOSAVE: OFF";
        }
        drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
        y += 22;
    }

    // Renderer performance/debug info.
    {
        std::stringstream ss;
        ss << "RENDER: TILE " << std::clamp(tile, 16, 256) << "px"
           << "  VOXEL: " << (game.voxelSpritesEnabled() ? "ON" : "OFF")
           << "  VIEW: " << viewTilesW << "x" << viewTilesH
           << "  CAM: " << camX << "," << camY
           << "  DECALS/STYLE: " << decalsPerStyleUsed
           << "  AUTOTILE VARS: " << autoVarsUsed;
        drawText5x7(renderer, x0 + pad, y, 2, gray, ss.str());
        y += 18;
    }
    {
        size_t ent = 0, item = 0, proj = 0;
        spriteTex.countByCategory(ent, item, proj);

        const size_t usedMB = spriteTex.usedBytes() / (1024ull * 1024ull);
        const size_t budgetMB = spriteTex.budgetBytes() / (1024ull * 1024ull);

        std::stringstream ss;
        ss << "SPRITE CACHE: " << usedMB << "MB / ";
        if (spriteTex.budgetBytes() == 0) ss << "UNLIMITED";
        else ss << budgetMB << "MB";
        ss << "  (E:" << ent << " I:" << item << " P:" << proj << ")"
           << "  H:" << spriteTex.hits() << " M:" << spriteTex.misses() << " EV:" << spriteTex.evictions();

        drawText5x7(renderer, x0 + pad, y, 2, gray, ss.str());
        y += 22;
    }

    drawText5x7(renderer, x0 + pad, y, 2, white, "TOP RUNS");
    y += 18;

    const auto& entries = game.scoreBoard().entries();
    const int maxShown = 10;

    if (entries.empty()) {
        drawText5x7(renderer, x0 + pad, y, 2, white, "(NO RUNS RECORDED YET)");
        y += 18;
    } else {
        for (int i = 0; i < (int)entries.size() && i < maxShown; ++i) {
            const auto& e = entries[i];
            auto trunc = [](const std::string& s, size_t n) {
                if (s.size() <= n) return s;
                if (n <= 1) return s.substr(0, n);
                if (n <= 3) return s.substr(0, n);
                return s.substr(0, n - 3) + "...";
            };

            const std::string who = e.name.empty() ? "PLAYER" : e.name;
            const std::string whoCol = trunc(who, 10);
            const std::string cause = e.cause.empty() ? "" : e.cause;
            const std::string causeCol = trunc(cause, 28);

            std::stringstream ss;
            ss << "#" << (i + 1) << " "
               << whoCol;
            if (whoCol.size() < 10) ss << std::string(10 - whoCol.size(), ' ');

            ss << " "
               << (e.won ? "WIN " : "DEAD")
               << " " << e.score
               << " " << depthTag(scoreEntryBranch(e), e.depth)
               << " T" << e.turns
               << " K" << e.kills
               << " S" << e.seed;

            if (!causeCol.empty()) ss << " " << causeCol;

            drawText5x7(renderer, x0 + pad, y, 2, white, ss.str());
            y += 16;
            if (y > y0 + panelH - 36) break;
        }
    }

    // Footer
    drawText5x7(renderer, x0 + pad, y0 + panelH - 20, 2, white, "ESC to close");
}

void Renderer::drawLevelUpOverlay(const Game& game) {
    // A focused, compact overlay that forces the player to spend talent points.
    const int points = game.pendingTalentPoints();
    if (points <= 0) return;

    const int panelW = std::min(winW - 80, 620);
    const int panelH = 260;
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - hudH - panelH) / 2;

    SDL_Rect bg{x0, y0, panelW, panelH};
    drawPanel(game, bg, 220, lastFrame);

    const Color white{240,240,240,255};
    const Color gray{160,160,160,255};
    const Color yellow{255,230,120,255};

    const int scale = 2;
    int y = y0 + 14;

    drawText5x7(renderer, x0 + 16, y, scale, yellow, "LEVEL UP!  CHOOSE A TALENT");
    y += 22;

    {
        std::stringstream ss;
        ss << "TALENT POINTS: " << points
           << "   MIGHT:" << game.playerMight()
           << "  AGI:" << game.playerAgility()
           << "  VIG:" << game.playerVigor()
           << "  FOC:" << game.playerFocus();
        drawText5x7(renderer, x0 + 16, y, scale, white, ss.str());
        y += 20;
    }

    {
        std::stringstream ss;
        ss << "MELEE POWER: " << game.playerMeleePower()
           << "   EVASION: " << game.playerEvasion()
           << "   WAND PWR: " << game.playerWandPower();
        drawText5x7(renderer, x0 + 16, y, scale, gray, ss.str());
        y += 22;
    }

    const int sel = game.levelUpSelection();

    auto drawChoice = [&](int idx, const char* label, const char* desc) {
        const Color c = (idx == sel) ? white : gray;
        std::stringstream ss;
        ss << (idx == sel ? "> " : "  ") << label << ": " << desc;
        drawText5x7(renderer, x0 + 16, y, scale, c, ss.str());
        y += 18;
    };

    drawChoice(0, "MIGHT",   "+1 melee power, +carry, +melee dmg bonus");
    drawChoice(1, "AGILITY", "+1 ranged skill, +evasion, better locks/traps");
    drawChoice(2, "VIGOR",   "+2 max HP now, tougher natural regen");
    drawChoice(3, "FOCUS",   "+1 wand power, better searching");

    y += 14;
    drawText5x7(renderer, x0 + 16, y, scale, gray, "UP/DOWN: select  ENTER: spend  ESC: spend all");
}



void Renderer::drawScoresOverlay(const Game& game) {
    const int pad = 14;
    const int panelW = winW * 9 / 10;
    const int panelH = winH * 9 / 10;
    const int panelX = (winW - panelW) / 2;
    const int panelY = (winH - panelH) / 2;

    SDL_Rect panel{ panelX, panelY, panelW, panelH };
    drawPanel(game, panel, 230, lastFrame);

    const int titleScale = 2;
    const int bodyScale = 1;
    const int lineH = 10 * bodyScale;

    const Color white{255, 255, 255, 255};
    const Color gray{160, 160, 160, 255};
    const Color selCol{240, 240, 120, 255};

    int x = panelX + pad;
    int y = panelY + pad;

    drawText5x7(renderer, x, y, titleScale, white, "SCORES");
    y += 20;

    std::ostringstream header;
    header << "VIEW: " << scoresViewDisplayName(game.scoresView())
           << "  (LEFT/RIGHT TO TOGGLE)   UP/DOWN SELECT   PGUP/PGDN JUMP   ESC CLOSE";
    drawTextWrapped5x7(renderer, x, y, bodyScale, gray, header.str(), panelW - pad * 2);
    y += 30;

    const int topH = (y - panelY) + 10;
    const int innerX = panelX + pad;
    const int innerY = panelY + topH;
    const int innerW = panelW - pad * 2;
    const int innerH = panelH - topH - pad;

    const int listW = innerW * 6 / 10;
    const int detailX = innerX + listW + pad;
    const int detailW = innerW - listW - pad;

    SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
    SDL_RenderDrawLine(renderer, detailX - pad / 2, innerY, detailX - pad / 2, innerY + innerH);

    std::vector<size_t> order;
    game.buildScoresList(order);
    const auto& entries = game.scoreBoard().entries();
    const int total = static_cast<int>(order.size());
    const int sel = clampi(game.scoresSelection(), 0, std::max(0, total - 1));

    auto fitToChars = [&](const std::string& s, int maxChars) -> std::string {
        if (maxChars <= 0) return "";
        if (static_cast<int>(s.size()) <= maxChars) return s;
        if (maxChars <= 3) return s.substr(0, maxChars);
        return s.substr(0, maxChars - 3) + "...";
    };

    // Left: list
    {
        const SDL_Rect clip = {innerX, innerY, listW, innerH};
        ClipRectGuard g(renderer, &clip);

        if (total <= 0) {
            drawText5x7(renderer, innerX, innerY, bodyScale, gray, "NO RUNS RECORDED YET.");
        } else {
            const int rows = std::max(1, innerH / lineH);
            const int maxScroll = std::max(0, total - rows);
            const int scroll = clampi(sel - rows / 2, 0, maxScroll);

            for (int row = 0; row < rows; ++row) {
                const int viewIdx = scroll + row;
                if (viewIdx >= total) break;

                const ScoreEntry& e = entries[order[viewIdx]];
                std::ostringstream ss;

                if (game.scoresView() == ScoresView::Top) {
                    ss << "#" << std::setw(3) << (viewIdx + 1) << "  ";
                    ss << "S" << std::setw(6) << e.score << "  ";
                    if (scoreEntryBranch(e) == DungeonBranch::Camp) ss << "CAMP ";
                    else ss << "D" << std::setw(2) << e.depth << "  ";
                    ss << (e.won ? "W " : "D ");
                    ss << e.name;
                    if (!e.playerClass.empty()) ss << " (" << e.playerClass << ")";
                } else {
                    std::string date = e.timestamp;
                    if (date.size() >= 10) date = date.substr(0, 10);
                    ss << date << "  " << (e.won ? "W " : "D ");
                    ss << "S" << e.score << " " << depthTag(scoreEntryBranch(e), e.depth) << " ";
                    ss << e.name;
                    if (!e.playerClass.empty()) ss << " (" << e.playerClass << ")";
                }

                const int maxChars = std::max(1, (listW - 4) / 6);
                const std::string line = fitToChars(ss.str(), maxChars);
                drawText5x7(renderer, innerX, innerY + row * lineH, bodyScale,
                            (viewIdx == sel) ? selCol : white, line);
            }
        }
    }

    // Right: details
    {
        const SDL_Rect clip = {detailX, innerY, detailW, innerH};
        ClipRectGuard g(renderer, &clip);

        if (total > 0) {
            const ScoreEntry& e = entries[order[sel]];

            int dy = innerY;
            drawText5x7(renderer, detailX, dy, bodyScale + 1, white, "DETAILS");
            dy += 18;

            // Rank by score (always meaningful since entries are stored score-sorted)
            const int rankByScore = static_cast<int>(order[sel]) + 1;

            {
                std::ostringstream ss;
                ss << "RANK: #" << rankByScore;
                if (game.scoresView() == ScoresView::Top) ss << "  (VIEW #" << (sel + 1) << ")";
                drawText5x7(renderer, detailX, dy, bodyScale, gray, ss.str());
                dy += lineH;
            }

            if (!e.timestamp.empty()) {
                std::ostringstream ss;
                ss << "WHEN: " << e.timestamp;
                drawText5x7(renderer, detailX, dy, bodyScale, gray, ss.str());
                dy += lineH;
            }

            {
                std::ostringstream ss;
                ss << "NAME: " << e.name;
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            if (!e.playerClass.empty()) {
                std::ostringstream ss;
                ss << "CLASS: " << e.playerClass;
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            {
                std::ostringstream ss;
                ss << "RESULT: " << (e.won ? "ESCAPED ALIVE" : "DIED");
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            {
                std::ostringstream ss;
                ss << "SCORE: " << e.score;
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            {
                std::ostringstream ss;
                ss << "DEPTH: " << depthLabel(scoreEntryBranch(e), e.depth) << "   TURNS: " << e.turns;
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            {
                std::ostringstream ss;
                ss << "KILLS: " << e.kills << "   LVL: " << e.level << "   GOLD: " << e.gold;
                drawText5x7(renderer, detailX, dy, bodyScale, white, ss.str());
                dy += lineH;
            }

            if (!e.conducts.empty()) {
                std::ostringstream ss;
                ss << "CONDUCTS: " << e.conducts;
                drawText5x7(renderer, detailX, dy, bodyScale, gray, ss.str());
                dy += lineH;
            }

            if (e.seed != 0) {
                std::ostringstream ss;
                ss << "SEED: " << e.seed << "   SLOT: " << e.slot;
                drawText5x7(renderer, detailX, dy, bodyScale, gray, ss.str());
                dy += lineH;
            }

            if (!e.cause.empty()) {
                std::ostringstream ss;
                ss << "CAUSE: " << e.cause;
                drawTextWrapped5x7(renderer, detailX, dy, bodyScale, gray, ss.str(), detailW);
            }
        }

        // Footer: scores file path (handy for backups / sharing)
        {
            const std::string path = game.defaultScoresPath();
            const std::string line = "FILE: " + path;
            drawTextWrapped5x7(renderer, detailX, innerY + innerH - lineH * 2, bodyScale, gray, line, detailW);
        }
    }
}


void Renderer::drawCodexOverlay(const Game& game) {
    const int pad = 14;
    const int panelW = winW * 9 / 10;
    const int panelH = (winH - hudH) * 9 / 10;
    const int panelX = (winW - panelW) / 2;
    const int panelY = (winH - hudH - panelH) / 2;

    SDL_Rect panel{ panelX, panelY, panelW, panelH };
    drawPanel(game, panel, 230, lastFrame);

    const Color white{240, 240, 240, 255};
    const Color gray{170, 170, 170, 255};
    const Color dark{110, 110, 110, 255};
    const Color yellow{255, 230, 120, 255};

    const int titleScale = 2;
    const int bodyScale = 1;
    const int lineH = 10 * bodyScale;

    int x = panelX + pad;
    int y = panelY + pad;

    drawText5x7(renderer, x, y, titleScale, white, "MONSTER CODEX");
    y += 20;

    // Filter / sort summary + quick controls.
    auto filterName = [&]() -> const char* {
        switch (game.codexFilter()) {
            case CodexFilter::All:    return "ALL";
            case CodexFilter::Seen:   return "SEEN";
            case CodexFilter::Killed: return "KILLED";
            default:                  return "ALL";
        }
    };
    auto sortName = [&]() -> const char* {
        switch (game.codexSort()) {
            case CodexSort::Kind:      return "KIND";
            case CodexSort::KillsDesc: return "KILLS";
            default:                   return "KIND";
        }
    };

    {
        std::string meta = "FILTER: ";
        meta += filterName();
        meta += "   SORT: ";
        meta += sortName();
        meta += "   (TAB/I SORT, LEFT/RIGHT FILTER)";
        drawText5x7(renderer, x, y, bodyScale, gray, meta);
        y += 14;
        drawText5x7(renderer, x, y, bodyScale, gray,
                    "UP/DOWN SELECT   ENTER/ESC CLOSE   (3D PREVIEW AUTO-ROTATES)");
        y += 18;
    }

    // Build filtered/sorted list.
    std::vector<EntityKind> list;
    game.buildCodexList(list);

    // Layout: list column + details column.
    const int innerW = panelW - pad * 2;
    const int innerH = panelH - pad * 2 - (y - (panelY + pad));
    const int listW = innerW * 4 / 10;
    const int detailsW = innerW - listW - pad;
    const int listX = x;
    const int listY = y;
    const int detailsX = listX + listW + pad;
    const int detailsY = listY;

    const int maxLines = std::max(1, innerH / lineH);

    int sel = game.codexSelection();
    if (list.empty()) {
        sel = 0;
    } else {
        sel = clampi(sel, 0, static_cast<int>(list.size()) - 1);
    }

    // Keep selection visible by auto-scrolling.
    int first = 0;
    if (sel >= maxLines) first = sel - maxLines + 1;
    const int maxFirst = std::max(0, static_cast<int>(list.size()) - maxLines);
    first = clampi(first, 0, maxFirst);

    // Draw list.
    {
        SDL_Rect clip{listX, listY, listW, innerH};
        SDL_RenderSetClipRect(renderer, &clip);

        for (int row = 0; row < maxLines; ++row) {
            const int idx = first + row;
            if (idx >= static_cast<int>(list.size())) break;

            const EntityKind k = list[idx];
            const bool seen = game.codexHasSeen(k);
            const uint16_t kills = game.codexKills(k);

            const int rowY = listY + row * lineH;

            if (idx == sel) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 36);
                SDL_Rect r{listX, rowY - 1, listW, lineH};
                SDL_RenderFillRect(renderer, &r);
            }

            const Color nameCol = seen ? white : dark;
            const char* nm = seen ? entityKindName(k) : "??????";

            // Left: name. Right: kill count.
            const std::string killsStr = (kills > 0) ? ("K:" + std::to_string(kills)) : "";

            drawText5x7(renderer, listX + 4, rowY, bodyScale, nameCol, nm);

            if (!killsStr.empty()) {
                const int wKills = static_cast<int>(killsStr.size()) * 6 * bodyScale;
                drawText5x7(renderer, listX + listW - 4 - wKills, rowY, bodyScale,
                            seen ? gray : dark, killsStr);
            }
        }

        SDL_RenderSetClipRect(renderer, nullptr);

        // Divider.
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 40);
        SDL_RenderDrawLine(renderer, listX + listW + pad / 2, listY,
                           listX + listW + pad / 2, listY + innerH);
    }

    // Draw details.
    {
        SDL_Rect detailsClip{detailsX, detailsY, detailsW, innerH};
        ClipRectGuard _clip(renderer, &detailsClip);

        auto dline = [&](const std::string& s, const Color& c) {
            drawText5x7(renderer, detailsX, y, bodyScale, c, s);
            y += 14;
        };

        y = detailsY;

        if (list.empty()) {
            dline("NO ENTRIES", gray);
            dline("(TRY EXPLORING TO DISCOVER MONSTERS)", dark);
            return;
        }

        const EntityKind k = list[sel];
        const bool seen = game.codexHasSeen(k);
        const uint16_t kills = game.codexKills(k);

        if (!seen) {
            dline("UNKNOWN CREATURE", gray);
            dline("YOU HAVEN'T ENCOUNTERED THIS MONSTER YET.", dark);
            dline("FILTER: ALL SHOWS PLACEHOLDERS FOR UNSEEN KINDS.", dark);
            return;
        }

        // Header.
        dline(std::string(entityKindName(k)), white);

        // --- 3D turntable preview -------------------------------------------------
        //
        // Large UI preview sprites are rendered through the voxel renderer with an
        // explicit yaw angle (camera rotated around the model).
        // This gives the Codex a "3D inspect" feel without changing gameplay.
        {
            // Clamp to keep the details column readable.
            const int maxPx = std::min(detailsW, std::min(innerH / 2, 220));
            const int prevPx = std::clamp(maxPx, 96, 220);

            // Don't try to draw if the panel is too small.
            if (prevPx >= 96) {
                drawText5x7(renderer, detailsX, y, bodyScale, gray, "3D PREVIEW");
                y += 14;

                const int px = detailsX + (detailsW - prevPx) / 2;
                SDL_Rect r{px, y, prevPx, prevPx};

                // Background plate.
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 90);
                SDL_RenderFillRect(renderer, &r);

                SDL_Texture* tex = nullptr;

                // Cache key: [cat=4][sub=1 entity][kind][yawStep][px][animFrame]
                const uint32_t t = SDL_GetTicks();
                constexpr int yawSteps = 24;
                const int yawStep = static_cast<int>((t / 120u) % static_cast<uint32_t>(yawSteps));
                const int animF = static_cast<int>((t / 180u) % static_cast<uint32_t>(FRAMES));
                constexpr float PI = 3.14159265358979323846f;
                const float yaw = (2.0f * PI) * (static_cast<float>(yawStep) / static_cast<float>(yawSteps));

                const uint8_t kind8 = static_cast<uint8_t>(k);
                const uint8_t px8 = static_cast<uint8_t>(std::clamp(prevPx, 0, 255));
                const uint8_t a8 = static_cast<uint8_t>(std::clamp(animF, 0, 255));
                const uint16_t yaw16 = static_cast<uint16_t>(yawStep);

                const uint64_t key = (static_cast<uint64_t>(4) << 56) |
                                     (static_cast<uint64_t>(1) << 48) |
                                     (static_cast<uint64_t>(kind8) << 40) |
                                     (static_cast<uint64_t>(yaw16) << 24) |
                                     (static_cast<uint64_t>(px8) << 16) |
                                     (static_cast<uint64_t>(a8) << 8);

                if (auto arr = uiPreviewTex.get(key)) {
                    tex = (*arr)[0];
                } else {
                    // Stable, per-kind seed so the Codex feels consistent across runs.
                    const uint32_t seed = hash32(0xC0D3u ^ (static_cast<uint32_t>(kind8) * 0x9E3779B9u));

                    // Build the base 2D sprite, then render a 3D turntable preview at prevPx.
                    SpritePixels base2d = generateEntitySprite(k, seed, animF,
                                                              /*use3d=*/false,
                                                              /*pxSize=*/16,
                                                              /*isometric=*/false,
                                                              /*isoRaytrace=*/false);

                    SpritePixels prev = game.voxelSpritesEnabled()
                        ? renderSprite3DEntityTurntable(k, base2d, seed, animF, yaw, prevPx)
                        : resampleSpriteToSize(base2d, prevPx);

                    SDL_Texture* created = textureFromSprite(prev);
                    if (created) {
                        std::array<SDL_Texture*, 1> a{created};
                        const size_t bytes = static_cast<size_t>(prevPx) * static_cast<size_t>(prevPx) * sizeof(uint32_t);
                        uiPreviewTex.put(key, a, bytes);
                        if (auto arr2 = uiPreviewTex.get(key)) tex = (*arr2)[0];
                    }
                }

                if (tex) {
                    SDL_RenderCopy(renderer, tex, nullptr, &r);
                }

                // Subtle border.
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 50);
                SDL_RenderDrawRect(renderer, &r);

                y += prevPx + 12;
            }
        }

        // Stats.
        const MonsterBaseStats base = baseMonsterStatsFor(k);
        const MonsterBaseStats scaled = monsterStatsForDepth(k, game.depth());

        dline("SEEN: YES   KILLS: " + std::to_string(kills), gray);
        dline("XP (ON KILL): " + std::to_string(game.xpFor(k)), gray);
        dline("SPEED: " + std::to_string(baseSpeedFor(k)), gray);

        dline("BASE STATS (DEPTH 1):", gray);
        dline("  HP " + std::to_string(base.hpMax) + "   ATK " + std::to_string(base.baseAtk) +
              "   DEF " + std::to_string(base.baseDef), white);

        if (game.depth() != 1) {
            dline("SCALED STATS (CURRENT DEPTH " + std::to_string(game.depth()) + "):", gray);
            dline("  HP " + std::to_string(scaled.hpMax) + "   ATK " + std::to_string(scaled.baseAtk) +
                  "   DEF " + std::to_string(scaled.baseDef), white);
        } else {
            dline("(STATS SCALE WITH DEPTH)", dark);
        }

        // Behavior / abilities.
        {
            if (base.canRanged) {
                std::string r = "RANGED: ";
                switch (base.rangedProjectile) {
                    case ProjectileKind::Arrow: r += "ARROWS"; break;
                    case ProjectileKind::Rock:  r += "ROCKS"; break;
                    case ProjectileKind::Spark: r += "SPARK"; break;
                    case ProjectileKind::Fireball: r += "FIREBALL"; break;
                    case ProjectileKind::Torch: r += "TORCH"; break;
                    default:                    r += "PROJECTILE"; break;
                }
                r += "  (R" + std::to_string(base.rangedRange) + " ATK " + std::to_string(base.rangedAtk) + ")";
                dline(r, gray);
            }

            if (base.regenChancePct > 0 && base.regenAmount > 0) {
                dline("REGEN: " + std::to_string(base.regenChancePct) + "% CHANCE / TURN (" +
                      std::to_string(base.regenAmount) + " HP)", gray);
            }

            if (base.packAI) {
                dline("BEHAVIOR: PACK HUNTER", gray);
            }
            if (base.willFlee) {
                dline("BEHAVIOR: MAY FLEE WHEN HURT", gray);
            }
        }

        // Monster-specific notes. These are intentionally short & gameplay-focused.
        {
            auto note = [&](const char* s) { dline(std::string("NOTE: ") + s, dark); };
            switch (k) {
                case EntityKind::Snake:  note("POISONOUS BITE."); break;
                case EntityKind::Spider: note("CAN WEB YOU, LIMITING MOVEMENT."); break;
                case EntityKind::Mimic:  note("DISGUISES ITSELF AS LOOT."); break;
                case EntityKind::Ghost:  note("RARE; CAN REGENERATE."); break;
                case EntityKind::Leprechaun: note("STEALS GOLD AND BLINKS AWAY."); break;
                case EntityKind::Nymph: note("STEALS ITEMS AND BLINKS AWAY."); break;
                case EntityKind::Zombie: note("SLOW UNDEAD; OFTEN RISES FROM CORPSES. IMMUNE TO POISON."); break;
                case EntityKind::Minotaur: note("BOSS-LIKE THREAT; SCALES MORE SLOWLY UNTIL DEEPER LEVELS."); break;
                case EntityKind::Shopkeeper: note("ATTACKING MAY ANGER THE SHOP."); break;
                case EntityKind::Guard: note("MERCHANT GUILD ENFORCER; APPEARS WHEN YOU STEAL."); break;
                default: break;
            }
        }
    }
}

void Renderer::drawDiscoveriesOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const Color white{240, 240, 240, 255};
    const Color gray{180, 180, 180, 255};
    const Color dark{120, 120, 120, 255};

    const int pad = 18;
    const int titleScale = 2;
    const int bodyScale = 2;
    const int lineH = 14;

    const int panelW = std::min(winW - 80, 980);
    const int panelH = std::min(winH - 80, 600);
    const int px = (winW - panelW) / 2;
    const int py = (winH - panelH) / 2;
    SDL_Rect panel{px, py, panelW, panelH};

    drawPanel(game, panel, 220, lastFrame);

    int x = px + pad;
    int y = py + pad;

    drawText5x7(renderer, x, y, titleScale, white, "DISCOVERIES");
    y += 22;

    // Header: filter/sort + known count.
    const DiscoveryFilter filter = game.discoveriesFilter();
    const DiscoverySort sort = game.discoveriesSort();

    auto matches = [&](ItemKind k) -> bool {
        switch (filter) {
            case DiscoveryFilter::All:     return true;
            case DiscoveryFilter::Potions: return isPotionKind(k);
            case DiscoveryFilter::Scrolls: return isScrollKind(k);
            case DiscoveryFilter::Rings:   return isRingKind(k);
            case DiscoveryFilter::Wands:   return isWandKind(k);
            default:                       return true;
        }
    };

    int total = 0;
    int known = 0;
    for (int i = 0; i < ITEM_KIND_COUNT; ++i) {
        const ItemKind k = static_cast<ItemKind>(i);
        if (!isIdentifiableKind(k)) continue;
        if (!matches(k)) continue;
        ++total;
        if (game.discoveriesIsIdentified(k)) ++known;
    }

    {
        std::ostringstream ss;
        ss << "FILTER: " << discoveryFilterDisplayName(filter)
           << "  SORT: " << discoverySortDisplayName(sort)
           << "  KNOWN: " << known << "/" << total;
        drawText5x7(renderer, x, y, bodyScale, gray, ss.str());
    }
    y += 16;
    drawText5x7(renderer, x, y, bodyScale, dark, "LEFT/RIGHT/TAB FILTER  SHIFT+S SORT  ESC CLOSE   (3D PREVIEW AUTO-ROTATES)");
    y += 18;

    // Build current list.
    std::vector<ItemKind> list;
    game.buildDiscoveryList(list);

    int sel = game.discoveriesSelection();
    if (list.empty()) sel = 0;
    else sel = clampi(sel, 0, static_cast<int>(list.size()) - 1);

    // Layout.
    const int innerW = panelW - pad * 2;
    const int innerH = (py + panelH - pad) - y;
    const int listW = std::max(260, innerW * 5 / 11);
    const int detailsW = innerW - listW - pad;
    const int listX = x;
    const int listY = y;
    const int detailsX = listX + listW + pad;
    const int detailsY = listY;
    const int maxLines = std::max(1, innerH / lineH);

    // Keep selection visible by auto-scrolling.
    int first = 0;
    if (sel >= maxLines) first = sel - maxLines + 1;
    const int maxFirst = std::max(0, static_cast<int>(list.size()) - maxLines);
    first = clampi(first, 0, maxFirst);

    // Draw list.
    {
        SDL_Rect clip{listX, listY, listW, innerH};
        SDL_RenderSetClipRect(renderer, &clip);

        for (int row = 0; row < maxLines; ++row) {
            const int idx = first + row;
            if (idx >= static_cast<int>(list.size())) break;

            const ItemKind k = list[idx];
            const bool id = game.discoveriesIsIdentified(k);
            const int rowY = listY + row * lineH;

            if (idx == sel) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 36);
                SDL_Rect r{listX, rowY - 1, listW, lineH};
                SDL_RenderFillRect(renderer, &r);
            }

            const std::string app = game.discoveryAppearanceLabel(k);
            const std::string prefix = id ? "* " : "  ";
            drawText5x7(renderer, listX + 4, rowY, bodyScale, id ? white : dark, prefix + app);
        }

        SDL_RenderSetClipRect(renderer, nullptr);

        // Divider.
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 40);
        SDL_RenderDrawLine(renderer, listX + listW + pad / 2, listY,
                           listX + listW + pad / 2, listY + innerH);
    }

    // Draw details.
    {
        SDL_Rect detailsClip{detailsX, detailsY, detailsW, innerH};
        ClipRectGuard _clip(renderer, &detailsClip);

        int dy = detailsY;
        auto dline = [&](const std::string& s, const Color& c) {
            drawText5x7(renderer, detailsX, dy, bodyScale, c, s);
            dy += 14;
        };

        if (list.empty()) {
            dline("NO IDENTIFIABLE ITEMS", gray);
            dline("(PICK UP POTIONS/SCROLLS/RINGS/WANDS TO START)", dark);
            return;
        }

        const ItemKind k = list[sel];
        const bool id = game.discoveriesIsIdentified(k);
        const std::string app = game.discoveryAppearanceLabel(k);
        const std::string trueName = itemDisplayNameSingle(k);

        auto category = [&]() -> const char* {
            if (isPotionKind(k)) return "POTION";
            if (isScrollKind(k)) return "SCROLL";
            if (isRingKind(k))   return "RING";
            if (isWandKind(k))   return "WAND";
            return "ITEM";
        };

        // A lightweight, UI-only summary of the known effect.
        struct Blurb { const char* a; const char* b; const char* c; };
        auto blurbFor = [&](ItemKind kk) -> Blurb {
            switch (kk) {
                // Potions
                case ItemKind::PotionHealing:       return {"HEALS YOU.", "", ""};
                case ItemKind::PotionStrength:      return {"CHANGES MIGHT TALENT.", "(BLESSED STRONGER, CURSED WEAKER)", ""};
                case ItemKind::PotionAntidote:      return {"CURES POISON.", "", ""};
                case ItemKind::PotionRegeneration:  return {"GRANTS REGENERATION.", "", ""};
                case ItemKind::PotionShielding:     return {"GRANTS A TEMPORARY SHIELD.", "", ""};
                case ItemKind::PotionHaste:         return {"GRANTS HASTE.", "", ""};
                case ItemKind::PotionVision:        return {"GRANTS SHARPENED VISION.", "(INCREASES FOV TEMPORARILY)", ""};
                case ItemKind::PotionInvisibility:  return {"MAKES YOU INVISIBLE.", "", ""};
                case ItemKind::PotionClarity:       return {"CURES CONFUSION.", "(ALSO ENDS HALLUCINATIONS)", ""};
                case ItemKind::PotionLevitation:    return {"GRANTS LEVITATION.", "(FLOAT OVER TRAPS/CHASMS)", ""};
                case ItemKind::PotionHallucination: return {"CAUSES HALLUCINATIONS.", "BLESSED: SHORT + VISION.", "CURSED: LONG + CONFUSION."};

                // Scrolls
                case ItemKind::ScrollTeleport:      return {"TELEPORTS YOU.", "CONFUSED: SHORT-RANGE BLINK.", ""};
                case ItemKind::ScrollMapping:       return {"REVEALS THE MAP.", "CONFUSED: CAUSES AMNESIA.", ""};
                case ItemKind::ScrollEnchantWeapon: return {"ENCHANTS YOUR WEAPON.", "", ""};
                case ItemKind::ScrollEnchantArmor:  return {"ENCHANTS YOUR ARMOR.", "", ""};
                case ItemKind::ScrollEnchantRing:   return {"ENCHANTS A RING.", "PROMPTS IF MULTIPLE RINGS.", ""};
                case ItemKind::ScrollIdentify:      return {"IDENTIFIES AN UNKNOWN ITEM.", "", ""};
                case ItemKind::ScrollDetectTraps:   return {"DETECTS TRAPS NEARBY.", "", ""};
                case ItemKind::ScrollDetectSecrets: return {"REVEALS SECRET DOORS.", "", ""};
                case ItemKind::ScrollKnock:         return {"UNLOCKS DOORS/CONTAINERS.", "", ""};
                case ItemKind::ScrollRemoveCurse:   return {"REMOVES CURSES (AND CAN BLESS).", "", ""};
                case ItemKind::ScrollConfusion:     return {"CAUSES CONFUSION AROUND YOU.", "", ""};
                case ItemKind::ScrollFear:          return {"CAUSES FEAR AROUND YOU.", "", ""};
                case ItemKind::ScrollEarth:         return {"CREATES BOULDERS.", "", ""};
                case ItemKind::ScrollTaming:        return {"TAMES A CREATURE.", "", ""};

                // Rings
                case ItemKind::RingMight:           return {"PASSIVE MIGHT BONUS.", "", ""};
                case ItemKind::RingAgility:         return {"PASSIVE AGILITY BONUS.", "", ""};
                case ItemKind::RingFocus:           return {"PASSIVE FOCUS BONUS.", "", ""};
                case ItemKind::RingProtection:      return {"PASSIVE DEFENSE BONUS.", "", ""};
                case ItemKind::RingSearching:       return {"PASSIVE SEARCHING.", "(AUTO-SEARCHES AROUND YOU)", "(ENCHANT/BUC BOOSTS POTENCY)"};
                case ItemKind::RingSustenance:     return {"PASSIVE SUSTENANCE.", "(SLOWS HUNGER LOSS IF ENABLED)", "(ENCHANT/BUC BOOSTS POTENCY)"};

                // Wands
                case ItemKind::WandSparks:          return {"FIRES SPARKS.", "(RANGED, USES CHARGES)", ""};
                case ItemKind::WandDigging:         return {"DIGS THROUGH WALLS.", "(RANGED, USES CHARGES)", ""};
                case ItemKind::WandFireball:        return {"FIRES AN EXPLOSIVE FIREBALL.", "", ""};

                default:
                    return {"", "", ""};
            }
        };

        // Header.
        dline(id ? trueName : "UNKNOWN ITEM", white);
        dline(std::string("CATEGORY: ") + category(), gray);
        dline(std::string("APPEARANCE: ") + app, gray);
        dline(std::string("IDENTIFIED: ") + (id ? "YES" : "NO"), gray);

        // --- 3D turntable preview -------------------------------------------------
        {
            const int maxPx = std::min(detailsW, std::min(innerH / 2, 220));
            const int prevPx = std::clamp(maxPx, 96, 220);

            if (prevPx >= 96) {
                drawText5x7(renderer, detailsX, dy, bodyScale, gray, "3D PREVIEW");
                dy += 14;

                const int px = detailsX + (detailsW - prevPx) / 2;
                SDL_Rect r{px, dy, prevPx, prevPx};

                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 90);
                SDL_RenderFillRect(renderer, &r);

                SDL_Texture* tex = nullptr;

                const uint32_t t = SDL_GetTicks();
                constexpr int yawSteps = 24;
                const int yawStep = static_cast<int>((t / 120u) % static_cast<uint32_t>(yawSteps));
                const int animF = static_cast<int>((t / 190u) % static_cast<uint32_t>(FRAMES));
                constexpr float PI = 3.14159265358979323846f;
                const float yaw = (2.0f * PI) * (static_cast<float>(yawStep) / static_cast<float>(yawSteps));

                const uint8_t kind8 = static_cast<uint8_t>(k);
                const uint8_t px8 = static_cast<uint8_t>(std::clamp(prevPx, 0, 255));
                const uint8_t a8 = static_cast<uint8_t>(std::clamp(animF, 0, 255));
                const uint16_t yaw16 = static_cast<uint16_t>(yawStep);

                const uint8_t appId = static_cast<uint8_t>(game.itemAppearanceFor(k));
                const uint8_t variant = static_cast<uint8_t>((id ? 0x80u : 0x00u) | (appId & 0x7Fu));

                const uint64_t key = (static_cast<uint64_t>(4) << 56) |
                                     (static_cast<uint64_t>(2) << 48) |
                                     (static_cast<uint64_t>(kind8) << 40) |
                                     (static_cast<uint64_t>(yaw16) << 24) |
                                     (static_cast<uint64_t>(px8) << 16) |
                                     (static_cast<uint64_t>(a8) << 8) |
                                     (static_cast<uint64_t>(variant));

                if (auto arr = uiPreviewTex.get(key)) {
                    tex = (*arr)[0];
                } else {
                    // If unidentified, seed by appearance id only (NetHack-style). Otherwise, use a stable per-kind seed.
                    const uint32_t seed = id
                        ? hash32(0xD15Cu ^ (static_cast<uint32_t>(kind8) * 0x9E3779B9u))
                        : (SPRITE_SEED_IDENT_APPEARANCE_FLAG | static_cast<uint32_t>(appId));

                    SpritePixels base2d = generateItemSprite(k, seed, animF,
                                                            /*use3d=*/false,
                                                            /*pxSize=*/16,
                                                            /*isometric=*/false,
                                                            /*isoRaytrace=*/false);

                    SpritePixels prev;
                    if (game.voxelSpritesEnabled()) {
                        // If unidentified, use extrusion to preserve the appearance-based 2D details.
                        prev = id
                            ? renderSprite3DItemTurntable(k, base2d, seed, animF, yaw, prevPx)
                            : renderSprite3DExtrudedTurntable(base2d, seed, animF, yaw, prevPx);
                    } else {
                        prev = resampleSpriteToSize(base2d, prevPx);
                    }

                    SDL_Texture* created = textureFromSprite(prev);
                    if (created) {
                        std::array<SDL_Texture*, 1> a{created};
                        const size_t bytes = static_cast<size_t>(prevPx) * static_cast<size_t>(prevPx) * sizeof(uint32_t);
                        uiPreviewTex.put(key, a, bytes);
                        if (auto arr2 = uiPreviewTex.get(key)) tex = (*arr2)[0];
                    }
                }

                if (tex) SDL_RenderCopy(renderer, tex, nullptr, &r);

                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 50);
                SDL_RenderDrawRect(renderer, &r);

                dy += prevPx + 12;
            }
        }

        if (!id) {
            dline("", gray);
            dline("USE IT TO IDENTIFY... OR READ A", dark);
            dline("SCROLL OF IDENTIFY FOR SAFETY.", dark);
            return;
        }

        const Blurb b = blurbFor(k);
        if (b.a && b.a[0]) {
            dline("", gray);
            dline(b.a, white);
            if (b.b && b.b[0]) dline(b.b, dark);
            if (b.c && b.c[0]) dline(b.c, dark);
        }

        // If the item has an underlying stat modifier, show it.
        if (isRingKind(k)) {
            const ItemDef& d = itemDef(k);
            std::ostringstream ss;
            ss << "BONUSES: ";
            bool any = false;
            if (d.modMight != 0)   { ss << "MIGHT " << d.modMight << "  "; any = true; }
            if (d.modAgility != 0) { ss << "AGI " << d.modAgility << "  "; any = true; }
            if (d.modVigor != 0)   { ss << "VIG " << d.modVigor << "  "; any = true; }
            if (d.modFocus != 0)   { ss << "FOC " << d.modFocus << "  "; any = true; }
            if (d.defense != 0)    { ss << "DEF " << d.defense; any = true; }
            if (!any) ss << "(NONE)";
            dline(ss.str(), gray);
        }
    }
}


void Renderer::drawMessageHistoryOverlay(const Game& game) {
    ensureUIAssets(game);

    const Color white{255,255,255,255};
    const Color gray{180,180,180,255};

    // Center panel
    const int panelW = winW * 9 / 10;
    const int panelH = (winH - hudH) * 9 / 10;
    const int x0 = (winW - panelW) / 2;
    const int y0 = (winH - hudH - panelH) / 2;

    SDL_Rect panel { x0, y0, panelW, panelH };
    drawPanel(game, panel, 230, lastFrame);

    const int pad = 14;
    int y = y0 + pad;

    drawText5x7(renderer, x0 + pad, y, 2, white, "MESSAGE HISTORY");
    y += 22;

    {
        std::stringstream ss;
        ss << "FILTER: " << messageFilterDisplayName(game.messageHistoryFilter());
        if (!game.messageHistorySearch().empty()) {
            ss << "  SEARCH: \"" << game.messageHistorySearch() << "\"";
        }
        if (game.isMessageHistorySearchMode()) {
            ss << "  (TYPE)";
        }
        drawText5x7(renderer, x0 + pad, y, 2, gray, ss.str());
        y += 20;
    }

    drawText5x7(renderer, x0 + pad, y, 1, gray, "UP/DOWN scroll  LEFT/RIGHT filter  PGUP/PGDN scroll  / search  CTRL+L clear  CTRL+C copy  ESC close");
    y += 18;

    // Build filtered view.
    const auto& msgs = game.messages();
    std::vector<int> idx;
    idx.reserve(msgs.size());

    auto lowerAscii = [](unsigned char c) -> unsigned char {
        if (c >= 'A' && c <= 'Z') return static_cast<unsigned char>(c - 'A' + 'a');
        return c;
    };

    auto ifindAscii = [&](const std::string& haystack, const std::string& needle) -> size_t {
        if (needle.empty()) return 0;
        const size_t n = needle.size();
        const size_t m = haystack.size();
        if (n > m) return std::string::npos;

        for (size_t i = 0; i + n <= m; ++i) {
            bool ok = true;
            for (size_t j = 0; j < n; ++j) {
                if (lowerAscii(static_cast<unsigned char>(haystack[i + j])) != lowerAscii(static_cast<unsigned char>(needle[j]))) {
                    ok = false;
                    break;
                }
            }
            if (ok) return i;
        }
        return std::string::npos;
    };

    auto icontainsAscii = [&](const std::string& haystack, const std::string& needle) -> bool {
        return ifindAscii(haystack, needle) != std::string::npos;
    };

    const MessageFilter filter = game.messageHistoryFilter();
    const std::string& needle = game.messageHistorySearch();
    for (size_t i = 0; i < msgs.size(); ++i) {
        const auto& m = msgs[i];
        if (!messageFilterMatches(filter, m.kind)) continue;
        if (!needle.empty() && !icontainsAscii(m.text, needle)) continue;
        idx.push_back(static_cast<int>(i));
    }

    int scroll = game.messageHistoryScroll();
    const int maxScroll = std::max(0, static_cast<int>(idx.size()) - 1);
    scroll = std::max(0, std::min(scroll, maxScroll));

    // Text area
    const int scale = 2;
    const int charW = 6 * scale;
    const int lineH = 16;
    const int textTop = y;
    const int footerH = 18;
    const int textBottom = y0 + panelH - pad - footerH;

    const int availH = std::max(0, textBottom - textTop);
    const int maxLines = std::max(1, availH / lineH);

    auto kindColor = [&](MessageKind k) -> Color {
        switch (k) {
            case MessageKind::Combat:       return Color{255,230,120,255};
            case MessageKind::Loot:         return Color{120,255,120,255};
            case MessageKind::System:       return Color{160,200,255,255};
            case MessageKind::Warning:      return Color{255,120,120,255};
            case MessageKind::ImportantMsg: return Color{255,170,80,255};
            case MessageKind::Success:      return Color{120,255,255,255};
            case MessageKind::Info:
            default:                        return Color{255,255,255,255};
        }
    };

    auto fitToChars = [](const std::string& s, int maxChars) -> std::string {
        if (maxChars <= 0) return "";
        if (static_cast<int>(s.size()) <= maxChars) return s;
        if (maxChars <= 3) return s.substr(0, static_cast<size_t>(maxChars));
        return s.substr(0, static_cast<size_t>(maxChars - 3)) + "...";
    };

    const int maxChars = std::max(1, (panelW - 2 * pad) / std::max(1, charW));

    // Compute a consistent prefix field width so wrapped lines align.
    int prefixW = 0;
    for (int mi : idx) {
        const auto& m = msgs[static_cast<size_t>(mi)];
        const std::string prefix = depthTag(m.branch, m.depth) + " T" + std::to_string(m.turn) + " ";
        prefixW = std::max(prefixW, static_cast<int>(prefix.size()));
    }
    prefixW = std::min(prefixW, maxChars);

    const int bodyMaxChars = std::max(0, maxChars - prefixW);

    // Simple word wrap (ASCII-ish) with hard breaks for long tokens.
    auto wrap = [&](const std::string& s, int maxCharsLocal) -> std::vector<std::string> {
        std::vector<std::string> out;
        if (maxCharsLocal <= 0) {
            out.push_back("");
            return out;
        }

        size_t pos = 0;
        while (pos < s.size()) {
            // Skip leading spaces for the next line.
            while (pos < s.size() && s[pos] == ' ') ++pos;
            if (pos >= s.size()) break;

            size_t end = std::min(s.size(), pos + static_cast<size_t>(maxCharsLocal));
            if (end >= s.size()) {
                out.push_back(s.substr(pos));
                break;
            }

            // Prefer breaking on the last space inside the window.
            size_t space = s.rfind(' ', end);
            if (space != std::string::npos && space > pos) {
                end = space;
            }

            std::string line = s.substr(pos, end - pos);
            while (!line.empty() && line.back() == ' ') line.pop_back();
            out.push_back(line);

            pos = end;
        }

        if (out.empty()) out.push_back("");
        return out;
    };

    struct LineEntry {
        int msgIdx = 0;     // index into msgs
        int lineIdx = 0;    // 0 = first wrapped line for this message
        std::string text;   // wrapped body line
    };

    // Build a set of wrapped lines that fills the viewport bottom-up, keeping
    // the newest (respecting scroll) pinned to the bottom.
    std::vector<LineEntry> linesRev;

    if (!idx.empty()) {
        linesRev.reserve(static_cast<size_t>(maxLines));

        int bottomMsg = static_cast<int>(idx.size()) - 1 - scroll;
        bottomMsg = std::max(0, std::min(bottomMsg, static_cast<int>(idx.size()) - 1));

        for (int ii = bottomMsg; ii >= 0; --ii) {
            const int mi = idx[static_cast<size_t>(ii)];
            const auto& m = msgs[static_cast<size_t>(mi)];

            std::string body = m.text;
            if (m.repeat > 1) {
                body += " (x" + std::to_string(m.repeat) + ")";
            }

            std::vector<std::string> bodyLines = wrap(body, bodyMaxChars);
            const int need = std::max(1, static_cast<int>(bodyLines.size()));

            if (static_cast<int>(linesRev.size()) + need > maxLines) {
                // If the very first message is too tall, show as many lines from
                // the beginning as we can (at least the prefix line).
                if (linesRev.empty()) {
                    const int take = std::min(need, maxLines);
                    for (int li = take - 1; li >= 0; --li) {
                        linesRev.push_back(LineEntry{mi, li, bodyLines[static_cast<size_t>(li)]});
                    }
                }
                break;
            }

            for (int li = need - 1; li >= 0; --li) {
                linesRev.push_back(LineEntry{mi, li, bodyLines[static_cast<size_t>(li)]});
            }

            if (static_cast<int>(linesRev.size()) >= maxLines) break;
        }
    }

    std::reverse(linesRev.begin(), linesRev.end());

    const int bx = x0 + pad + prefixW * charW;

    if (idx.empty()) {
        drawText5x7(renderer, x0 + pad, y + 10, 2, gray, "NO MESSAGES MATCH.");
    } else if (linesRev.empty()) {
        drawText5x7(renderer, x0 + pad, y + 10, 2, gray, "NO MESSAGES TO SHOW.");
    } else {
        int yy = y;

        for (const auto& e : linesRev) {
            const auto& m = msgs[static_cast<size_t>(e.msgIdx)];
            const Color c = kindColor(m.kind);

            if (e.lineIdx == 0) {
                const std::string prefix = depthTag(m.branch, m.depth) + " T" + std::to_string(m.turn) + " ";
                drawText5x7(renderer, x0 + pad, yy, scale, gray, fitToChars(prefix, prefixW));
            }

            const std::string disp = fitToChars(e.text, bodyMaxChars);

            if (!needle.empty()) {
                const size_t pos = ifindAscii(disp, needle);
                if (pos != std::string::npos && pos < disp.size()) {
                    const size_t matchLen = std::min(needle.size(), disp.size() - pos);

                    const std::string pre = disp.substr(0, pos);
                    const std::string mid = disp.substr(pos, matchLen);
                    const std::string post = disp.substr(pos + matchLen);

                    // Pre
                    if (!pre.empty()) {
                        drawText5x7(renderer, bx, yy, scale, c, pre);
                    }

                    // Highlight background behind the match to keep it visible regardless of message kind color.
                    SDL_BlendMode oldBm = SDL_BLENDMODE_NONE;
                    SDL_GetRenderDrawBlendMode(renderer, &oldBm);
                    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                    SDL_Rect hi{
                        bx + static_cast<int>(pos) * charW - 2,
                        yy - 1,
                        static_cast<int>(matchLen) * charW + 4,
                        lineH - 2
                    };
                    SDL_SetRenderDrawColor(renderer, Uint8{255}, Uint8{255}, Uint8{255}, Uint8{55});
                    SDL_RenderFillRect(renderer, &hi);
                    SDL_SetRenderDrawBlendMode(renderer, oldBm);

                    // Match + Post
                    drawText5x7(renderer, bx + static_cast<int>(pos) * charW, yy, scale, c, mid);
                    if (!post.empty()) {
                        drawText5x7(renderer, bx + static_cast<int>(pos + matchLen) * charW, yy, scale, c, post);
                    }
                } else {
                    drawText5x7(renderer, bx, yy, scale, c, disp);
                }
            } else {
                drawText5x7(renderer, bx, yy, scale, c, disp);
            }

            yy += lineH;
        }
    }

    // Footer status
    {
        std::stringstream ss;
        ss << "SHOWING " << idx.size() << "/" << msgs.size();
        if (maxScroll > 0) ss << "  SCROLL " << scroll << "/" << maxScroll;
        drawText5x7(renderer, x0 + pad, y0 + panelH - pad - 12, 1, gray, ss.str());
    }
}

void Renderer::drawIsoHoverOverlay(const Game& game) {
    if (!renderer) return;
    if (viewMode_ != ViewMode::Isometric) return;

    // Don't fight with explicit inspect/target modes; those already own the cursor.
    if (game.isLooking() || game.isTargeting()) return;

    // Suppress hover-inspect when modal UIs are open.
    if (game.isCommandOpen() || game.isInventoryOpen() || game.isSpellsOpen() || game.isChestOpen() ||
        game.isOptionsOpen() || game.isKeybindsOpen() || game.isHelpOpen() || game.isMessageHistoryOpen() ||
        game.isScoresOpen() || game.isCodexOpen() || game.isDiscoveriesOpen() || game.isMinimapOpen() ||
        game.isStatsOpen() || game.isLevelUpOpen()) {
        return;
    }

    // Only update while the window is focused (prevents stale hover text when alt-tabbed).
    if (!window) return;
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) == 0u) return;

    int mx = 0;
    int my = 0;
    SDL_GetMouseState(&mx, &my);

    int tx = 0;
    int ty = 0;
    if (!windowToMapTile(game, mx, my, tx, ty)) {
        isoHoverValid_ = false;
        return;
    }

    const Dungeon& d = game.dungeon();
    if (!d.inBounds(tx, ty)) {
        isoHoverValid_ = false;
        return;
    }

    const Tile& t = d.at(tx, ty);
    if (!t.explored) {
        // No hover UI on unknown tiles (avoids accidentally "revealing" coordinate structure).
        isoHoverValid_ = false;
        return;
    }

    const Vec2i p{tx, ty};
    const uint32_t now = SDL_GetTicks();

    // Refresh when the mouse moves to a different tile, or occasionally while hovering
    // (so visibility/exploration state changes can update without requiring mouse motion).
    if (!isoHoverValid_ || isoHoverTile_.x != p.x || isoHoverTile_.y != p.y || (now - isoHoverTextTick_) > 350u) {
        isoHoverTile_ = p;
        isoHoverValid_ = true;
        isoHoverText_ = game.describeAt(p);
        isoHoverTextTick_ = now;
    }

    if (!isoHoverValid_) return;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Tile highlight (diamond) - subtle outline + inner glow.
    SDL_Rect base = mapTileDst(isoHoverTile_.x, isoHoverTile_.y);
    const int cx = base.x + base.w / 2;
    const int cy = base.y + base.h / 2;

    // Inner glow keeps the cursor readable over bright floors.
    SDL_SetRenderDrawColor(renderer, Uint8{255}, Uint8{255}, Uint8{255}, Uint8{28});
    {
        const int hw = std::max(1, base.w / 4);
        const int hh = std::max(1, base.h / 4);
        fillIsoDiamond(renderer, cx, cy, hw, hh);
    }

    // Outline + crosshair.
    SDL_SetRenderDrawColor(renderer, Uint8{160}, Uint8{235}, Uint8{255}, Uint8{185});
    drawIsoDiamondOutline(renderer, base);
    SDL_SetRenderDrawColor(renderer, Uint8{160}, Uint8{235}, Uint8{255}, Uint8{75});
    drawIsoDiamondCross(renderer, base);

    // One-line (up to two wrapped lines) hover tooltip near the HUD.
    {
        const int scale = 2;
        const Color cyan{ 140, 220, 255, 255 };
        const int hudTop = winH - hudH;

        std::string s = isoHoverText_.empty() ? "HOVER" : ("HOVER: " + isoHoverText_);

        const int charW = 6 * scale;
        const int maxChars = (winW - 20) / std::max(1, charW);
        const std::vector<std::string> lines = wrapToChars(s, maxChars, 2);
        const int lineH = 16;

        if (lines.size() >= 2) {
            drawText5x7(renderer, 10, hudTop - 18 - lineH, scale, cyan, fitToCharsMiddle(lines[0], maxChars));
            drawText5x7(renderer, 10, hudTop - 18,          scale, cyan, fitToCharsMiddle(lines[1], maxChars));
        } else if (!lines.empty()) {
            drawText5x7(renderer, 10, hudTop - 18, scale, cyan, fitToCharsMiddle(lines[0], maxChars));
        }
    }
}

void Renderer::drawTargetingOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const bool iso = (viewMode_ == ViewMode::Isometric);

    const auto& linePts = game.targetingLine();
    Vec2i cursor = game.targetingCursor();
    const bool ok = game.targetingIsValid();

    const std::string warning = game.targetingWarningText();
    const bool warn = ok && !warning.empty();

    Uint8 lr = 0, lg = Uint8{255}, lb = 0;
    if (!ok) {
        lr = Uint8{255}; lg = 0; lb = 0;
    } else if (warn) {
        lr = Uint8{255}; lg = Uint8{200}; lb = 0;
    }

    // Draw LOS line tiles (excluding player tile)
    SDL_SetRenderDrawColor(renderer, lr, lg, lb, Uint8{80});
    for (size_t i = 1; i < linePts.size(); ++i) {
        Vec2i p = linePts[i];
        SDL_Rect base = mapTileDst(p.x, p.y);
        if (iso) {
            const int cx = base.x + base.w / 2;
            const int cy = base.y + base.h / 2;
            const int hw = std::max(1, base.w / 8);
            const int hh = std::max(1, base.h / 4);
            fillIsoDiamond(renderer, cx, cy, hw, hh);
        } else {
            SDL_Rect r{ base.x + tile / 4, base.y + tile / 4, tile / 2, tile / 2 };
            SDL_RenderFillRect(renderer, &r);
        }
    }

    // Crosshair / reticle on cursor (procedural animated texture).
    SDL_Rect c = mapTileDst(cursor.x, cursor.y);
    SDL_Texture* ret = iso ? cursorReticleIsoTex[static_cast<size_t>(lastFrame)]
                           : cursorReticleTex[static_cast<size_t>(lastFrame)];
    if (ret) {
        SDL_SetTextureBlendMode(ret, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(ret, lr, lg, lb);
        SDL_SetTextureAlphaMod(ret, 255);
        SDL_RenderCopy(renderer, ret, nullptr, &c);

        // Reset mods (textures are shared between overlays).
        SDL_SetTextureColorMod(ret, 255, 255, 255);
        SDL_SetTextureAlphaMod(ret, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, lr, lg, lb, Uint8{200});
        if (iso) {
            drawIsoDiamondOutline(renderer, c);
            SDL_SetRenderDrawColor(renderer, lr, lg, lb, Uint8{110});
            drawIsoDiamondCross(renderer, c);
        } else {
            SDL_RenderDrawRect(renderer, &c);
        }
    }

    // Small label near bottom HUD: two-line hint bar.
    // Line 1: targeting context (enemy/tile + preview/warnings).
    // Line 2: controls (or the current failure reason).
    const int scale = 2;
    const Color yellow{ 255, 230, 120, 255 };
    const int hudTop = winH - hudH;

    const std::string info = game.targetingInfoText();
    const std::string preview = game.targetingCombatPreviewText();
    const std::string status = game.targetingStatusText();

    std::string line1 = info.empty() ? "TARGET:" : ("TARGET: " + info);
    if (!preview.empty()) {
        line1 += " | " + preview;
    }
    if (!warning.empty()) {
        line1 += " | " + warning;
    }

    std::string line2;
    if (ok) {
        line2 = game.targetingNeedsConfirm()
                    ? "ENTER CONFIRM  ESC CANCEL  TAB NEXT  SHIFT+TAB PREV"
                    : "ENTER FIRE  ESC CANCEL  TAB NEXT  SHIFT+TAB PREV";
    } else {
        line2 = status.empty() ? std::string("NO CLEAR SHOT") : status;
        line2 += "  (ESC CANCEL)";
    }

    const int charW = 6 * scale;
    const int maxChars = (winW - 20) / std::max(1, charW);
    const int lineH = 16;

    // Controls/status closest to the HUD.
    drawText5x7(renderer, 10, hudTop - 18, scale, yellow, fitToCharsMiddle(line2, maxChars));
    // Context line above it.
    drawText5x7(renderer, 10, hudTop - 18 - lineH, scale, yellow, fitToCharsMiddle(line1, maxChars));
}
void Renderer::drawLookOverlay(const Game& game) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    const bool iso = (viewMode_ == ViewMode::Isometric);

    const Dungeon& d = game.dungeon();
    Vec2i cursor = game.lookCursor();
    if (!d.inBounds(cursor.x, cursor.y)) return;

    // Acoustic preview heatmap (UI-only): visualize sound propagation from the LOOK cursor
    // without revealing unexplored tiles.
    if (game.isSoundPreviewOpen()) {
        const auto& dist = game.soundPreviewMap();
        const int vol = game.soundPreviewVolume();
        const Vec2i src = game.soundPreviewSource();

        if (!dist.empty() && (int)dist.size() == d.width * d.height && vol > 0) {
            // Color choice: a cool tint reads as sound/echo without clashing with
            // targeting lines (usually warm). Alpha encodes remaining loudness.
            for (int y = 0; y < d.height; ++y) {
                for (int x = 0; x < d.width; ++x) {
                    const Tile& t = d.at(x, y);
                    if (!t.explored) continue;
                    const int idx = y * d.width + x;
                    const int dd = dist[idx];
                    if (dd < 0 || dd > vol) continue;

                    const int strength = vol - dd;
                    const int alpha = std::clamp(20 + strength * 10, 20, 190);
                    SDL_SetRenderDrawColor(renderer, Uint8{90}, Uint8{200}, Uint8{255}, clampToU8(alpha));
                    SDL_Rect r = mapTileDst(x, y);
                    if (iso) {
                        fillIsoDiamond(renderer, r.x + r.w / 2, r.y + r.h / 2, r.w / 2, r.h / 2);
                    } else {
                        SDL_RenderFillRect(renderer, &r);
                    }
                }
            }

            // Slightly accent the source tile to make the heatmap origin obvious.
            if (d.inBounds(src.x, src.y)) {
                SDL_SetRenderDrawColor(renderer, Uint8{255}, Uint8{255}, Uint8{255}, Uint8{80});
                SDL_Rect r = mapTileDst(src.x, src.y);
                if (iso) {
                    drawIsoDiamondOutline(renderer, r);
                } else {
                    SDL_RenderDrawRect(renderer, &r);
                }
            }

            // Highlight visible hostiles that would hear this sound (uses the
            // same per-kind hearing stats as actual noise emission).
            SDL_SetRenderDrawColor(renderer, Uint8{255}, Uint8{220}, Uint8{120}, Uint8{200});
            for (const auto& m : game.entities()) {
                if (m.id == game.playerId()) continue;
                if (m.hp <= 0) continue;
                if (m.friendly) continue;
                if (m.kind == EntityKind::Shopkeeper && !m.alerted) continue;
                if (!d.inBounds(m.pos.x, m.pos.y)) continue;

                const Tile& mt = d.at(m.pos.x, m.pos.y);
                if (!mt.visible) continue;

                const int eff = vol + entityHearingDelta(m.kind);
                if (eff <= 0) continue;

                const int dd = dist[m.pos.y * d.width + m.pos.x];
                if (dd < 0 || dd > eff) continue;

                SDL_Rect r = mapTileDst(m.pos.x, m.pos.y);
                if (iso) {
                    drawIsoDiamondOutline(renderer, r);
                } else {
                    SDL_RenderDrawRect(renderer, &r);
                }
            }
        }
    }

    

    // Hearing preview heatmap (UI-only): visualize where your footsteps would be audible
    // to the *currently visible* hostile listeners (visibility-gated; never leaks unseen enemies).
    if (game.isHearingPreviewOpen()) {
        const auto& req = game.hearingPreviewMinRequiredVolume();
        const auto& step = game.hearingPreviewFootstepVolume();
        const int bias = game.hearingPreviewVolumeBias();
        const auto& listeners = game.hearingPreviewListeners();

        if (!req.empty() && !step.empty() && (int)req.size() == d.width * d.height && (int)step.size() == d.width * d.height) {
            // Color choice: a violet tint reads as "stealth / audibility" and stays distinct
            // from the blue sound preview and the red threat preview.
            for (int y = 0; y < d.height; ++y) {
                for (int x = 0; x < d.width; ++x) {
                    const Tile& t = d.at(x, y);
                    if (!t.explored) continue;
                    const int idx = y * d.width + x;
                    const int r = req[idx];
                    if (r < 0) continue;

                    const int v = std::clamp(step[idx] + bias, 0, 30);
                    if (v <= 0) continue;
                    if (v < r) continue;

                    const int margin = v - r;
                    int alpha = 35 + margin * 18;
                    if (r <= 2) alpha += 30; // very close listeners: punchier overlay
                    alpha = std::clamp(alpha, 35, 215);

                    SDL_SetRenderDrawColor(renderer, Uint8{200}, Uint8{120}, Uint8{255}, clampToU8(alpha));
                    SDL_Rect rct = mapTileDst(x, y);
                    if (iso) {
                        fillIsoDiamond(renderer, rct.x + rct.w / 2, rct.y + rct.h / 2, rct.w / 2, rct.h / 2);
                    } else {
                        SDL_RenderFillRect(renderer, &rct);
                    }
                }
            }

            // Accent the listener tiles so the player can see who the field is computed from.
            if (!listeners.empty()) {
                SDL_SetRenderDrawColor(renderer, Uint8{255}, Uint8{255}, Uint8{255}, Uint8{75});
                for (const Vec2i& s : listeners) {
                    if (!d.inBounds(s.x, s.y)) continue;
                    if (!d.at(s.x, s.y).visible) continue;
                    SDL_Rect rct = mapTileDst(s.x, s.y);
                    if (iso) {
                        drawIsoDiamondOutline(renderer, rct);
                    } else {
                        SDL_RenderDrawRect(renderer, &rct);
                    }
                }

	                // Emphasize the "dominant" listener for the current cursor tile (the one that
	                // minimizes required volume), making it easy to tell *who* is most likely to
	                // hear you without leaking unseen info.
	                const int dom = game.hearingPreviewDominantListenerIndexAt(cursor);
	                if (dom >= 0 && dom < (int)listeners.size()) {
	                    const Vec2i s = listeners[static_cast<size_t>(dom)];
	                    if (d.inBounds(s.x, s.y) && d.at(s.x, s.y).visible) {
	                        SDL_SetRenderDrawColor(renderer, Uint8{255}, Uint8{255}, Uint8{255}, Uint8{165});
	                        SDL_Rect rr = mapTileDst(s.x, s.y);
	                        if (iso) {
	                            drawIsoDiamondOutline(renderer, rr);
	                            drawIsoDiamondCross(renderer, rr);
	                        } else {
	                            SDL_RenderDrawRect(renderer, &rr);
	                            SDL_RenderDrawLine(renderer, rr.x, rr.y, rr.x + rr.w, rr.y + rr.h);
	                            SDL_RenderDrawLine(renderer, rr.x + rr.w, rr.y, rr.x, rr.y + rr.h);
	                        }
	                    }
	                }
            }
        }
    }
// Threat preview heatmap (UI-only): visualize approximate "time-to-contact" from
    // the nearest currently VISIBLE hostile. This is intentionally visibility-gated
    // so it never leaks information about unseen enemies.
    if (game.isThreatPreviewOpen()) {
        const auto& dist = game.threatPreviewMap();
        const int horizon = game.threatPreviewHorizon();

        if (!dist.empty() && (int)dist.size() == d.width * d.height && horizon > 0) {
            // Color choice: a warm tint reads as danger without clashing too hard with
            // targeting lines. Alpha encodes urgency (closer threats are more opaque).
            for (int y = 0; y < d.height; ++y) {
                for (int x = 0; x < d.width; ++x) {
                    const Tile& t = d.at(x, y);
                    if (!t.explored) continue;

                    const int idx = y * d.width + x;
                    const int dd = dist[idx];
                    if (dd < 0 || dd > horizon) continue;

                    const int strength = horizon - dd;
                    const int alpha = std::clamp(24 + strength * 12, 24, 205);
                    SDL_SetRenderDrawColor(renderer, Uint8{255}, Uint8{90}, Uint8{90}, clampToU8(alpha));
                    SDL_Rect r = mapTileDst(x, y);
                    if (iso) {
                        fillIsoDiamond(renderer, r.x + r.w / 2, r.y + r.h / 2, r.w / 2, r.h / 2);
                    } else {
                        SDL_RenderFillRect(renderer, &r);
                    }
                }
            }

            // Accent visible hostile source tiles so the player can "read" the field at a glance.
            const auto& srcs = game.threatPreviewSources();
            if (!srcs.empty()) {
                SDL_SetRenderDrawColor(renderer, Uint8{255}, Uint8{255}, Uint8{255}, Uint8{70});
                for (const Vec2i& s : srcs) {
                    if (!d.inBounds(s.x, s.y)) continue;
                    if (!d.at(s.x, s.y).visible) continue;
                    SDL_Rect r = mapTileDst(s.x, s.y);
                    if (iso) {
                        drawIsoDiamondOutline(renderer, r);
                    } else {
                        SDL_RenderDrawRect(renderer, &r);
                    }
                }
            }
        }
    }

    // Cursor reticle (procedural animated texture).
    SDL_Rect c = mapTileDst(cursor.x, cursor.y);
    SDL_Texture* ret = iso ? cursorReticleIsoTex[static_cast<size_t>(lastFrame)]
                           : cursorReticleTex[static_cast<size_t>(lastFrame)];
    if (ret) {
        SDL_SetTextureBlendMode(ret, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(ret, 255, 255, 255);
        SDL_SetTextureAlphaMod(ret, 240);
        SDL_RenderCopy(renderer, ret, nullptr, &c);

        // Reset alpha mod (textures are shared).
        SDL_SetTextureAlphaMod(ret, 255);
    } else {
        // Fallback (primitives) if textures are missing.
        SDL_SetRenderDrawColor(renderer, Uint8{255}, Uint8{255}, Uint8{255}, Uint8{200});
        if (iso) {
            drawIsoDiamondOutline(renderer, c);
        } else {
            SDL_RenderDrawRect(renderer, &c);
        }

        SDL_SetRenderDrawColor(renderer, Uint8{255}, Uint8{255}, Uint8{255}, Uint8{90});
        if (iso) {
            drawIsoDiamondCross(renderer, c);
        } else {
            SDL_RenderDrawLine(renderer, c.x, c.y + c.h / 2, c.x + c.w, c.y + c.h / 2);
            SDL_RenderDrawLine(renderer, c.x + c.w / 2, c.y, c.x + c.w / 2, c.y + c.h);
        }
    }

    // Label near bottom of map
    const int scale = 2;
    const Color yellow{ 255, 230, 120, 255 };
    const int hudTop = winH - hudH;

    if (!game.isCommandOpen()) {
        std::string s = game.lookInfoText();
        if (s.empty()) s = "LOOK";

        const int charW = 6 * scale;
        const int maxChars = (winW - 20) / std::max(1, charW);

        // Use up to two wrapped lines (prevents valuable info from being lost on
        // narrow windows), and preserve both ends if we still have to ellipsize.
        const std::vector<std::string> lines = wrapToChars(s, maxChars, 2);
        const int lineH = 16;

        if (lines.size() >= 2) {
            drawText5x7(renderer, 10, hudTop - 18 - lineH, scale, yellow, fitToCharsMiddle(lines[0], maxChars));
            drawText5x7(renderer, 10, hudTop - 18,          scale, yellow, fitToCharsMiddle(lines[1], maxChars));
        } else {
            drawText5x7(renderer, 10, hudTop - 18, scale, yellow, fitToCharsMiddle(lines[0], maxChars));
        }
    }
}

