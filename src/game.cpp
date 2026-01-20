#include "game_internal.hpp"
#include "overworld.hpp"
#include <cmath>

uint32_t dailySeedUtc(std::string* outDateIso) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif

    const int year = tm.tm_year + 1900;
    const int mon = tm.tm_mon + 1;
    const int day = tm.tm_mday;

    if (outDateIso) {
        std::ostringstream ss;
        ss << std::setfill('0') << std::setw(4) << year << "-" << std::setw(2) << mon << "-" << std::setw(2) << day;
        *outDateIso = ss.str();
    }

    // YYYYMMDD -> stable hash (not crypto; just deterministic across platforms).
    const uint32_t ymd = static_cast<uint32_t>(year * 10000 + mon * 100 + day);
    return hash32(ymd ^ 0xDABA0B1Du);
}



Game::Game() : dung(MAP_W, MAP_H) {}

namespace {
// During early boot, main.cpp applies user settings before a run is created/loaded.
// Some getters/setters (lighting/encumbrance) may consult the player entity. In that
// phase, `ents` is still empty. Returning a stable dummy prevents UB (and Windows
// access violations) while keeping the rest of the code simple.
Entity& dummyPlayerEntity() {
    static Entity dummy;
    static bool init = false;
    if (!init) {
        dummy.id = 0;
        dummy.kind = EntityKind::Player;
        dummy.hpMax = 1;
        dummy.hp = 1;
        dummy.pos = {0, 0};
        init = true;
    }
    return dummy;
}
}

const Entity& Game::player() const {
    for (const auto& e : ents) if (e.id == playerId_) return e;
    if (!ents.empty()) return ents.front();
    return dummyPlayerEntity();
}

Entity& Game::playerMut() {
    for (auto& e : ents) if (e.id == playerId_) return e;
    if (!ents.empty()) return ents.front();
    return dummyPlayerEntity();
}

namespace {

static bool icontainsAscii(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    // ASCII-only case-insensitive substring search (good enough for our message text).
    auto lower = [](unsigned char c) -> unsigned char {
        if (c >= 'A' && c <= 'Z') return static_cast<unsigned char>(c - 'A' + 'a');
        return c;
    };

    const size_t n = needle.size();
    const size_t m = haystack.size();
    if (n > m) return false;

    for (size_t i = 0; i + n <= m; ++i) {
        bool ok = true;
        for (size_t j = 0; j < n; ++j) {
            if (lower(static_cast<unsigned char>(haystack[i + j])) != lower(static_cast<unsigned char>(needle[j]))) {
                ok = false;
                break;
            }
        }
        if (ok) return true;
    }
    return false;
}

static void utf8PopBack(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    // Walk back over UTF-8 continuation bytes (10xxxxxx).
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) {
        --i;
    }
    s.erase(i);
}

} // namespace

namespace {

struct MapSizeWH {
    int w = 0;
    int h = 0;
};

static int lerpInt(int a, int b, int num, int den) {
    if (den <= 0) return a;
    return a + (b - a) * num / den;
}

static float lerpFloat(float a, float b, float t) {
    return a + (b - a) * t;
}

static float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float u32ToSignedUnit(uint32_t v) {
    // Map uint32 -> [-1, +1] (inclusive-ish; exact endpoints aren't important here).
    constexpr float denom = 4294967295.0f; // 2^32 - 1
    return (static_cast<float>(v) / denom) * 2.0f - 1.0f;
}

// Mirror the endless "strata" banding used by Dungeon generation so map size drift aligns with
// macro-themed stretches (ruins -> mines -> catacombs...) instead of being pure per-floor jitter.
struct EndlessBandInfo {
    int index = -1;
    int startDepth = -1;
    int len = 0;
    int local = 0;
    EndlessStratumTheme theme = EndlessStratumTheme::Ruins;
    uint32_t seed = 0u;
};

static EndlessStratumTheme endlessThemeForIndex(uint32_t worldSeed, int idx) {
    // Deterministically pick a macro theme for an endless band.
    // Adjacent bands are prevented from repeating the same theme.
    const uint32_t base = hashCombine(worldSeed, 0xA11B1A0Du);
    const uint32_t v = hash32(base ^ static_cast<uint32_t>(idx));
    constexpr int kCount = 6;
    int t = static_cast<int>(v % static_cast<uint32_t>(kCount));

    if (idx > 0) {
        const uint32_t pv = hash32(base ^ static_cast<uint32_t>(idx - 1));
        const int prev = static_cast<int>(pv % static_cast<uint32_t>(kCount));
        if (t == prev) {
            // Deterministically pick a different theme using higher bits.
            const int bump = 1 + static_cast<int>((v / static_cast<uint32_t>(kCount)) % static_cast<uint32_t>(kCount - 1));
            t = (t + bump) % kCount;
        }
    }

    return static_cast<EndlessStratumTheme>(t);
}

static int endlessBandLengthForIndex(uint32_t worldSeed, int idx) {
    // 5..9 floors per band, run-seed dependent but stable.
    const uint32_t base = hashCombine(worldSeed, 0x57A7A11Eu);
    return 5 + static_cast<int>(hash32(base ^ static_cast<uint32_t>(idx)) % 5u);
}

static EndlessBandInfo computeEndlessBand(uint32_t worldSeed, DungeonBranch branch, int depth, int maxDepth) {
    EndlessBandInfo out;
    if (worldSeed == 0u) return out;
    if (branch != DungeonBranch::Main) return out;
    if (depth <= maxDepth) return out;

    const int endlessStart = maxDepth + 1;
    const int endlessD = depth - endlessStart;
    if (endlessD < 0) return out;

    int idx = 0;
    int cursor = 0;
    int d = endlessD;

    for (;;) {
        const int len = endlessBandLengthForIndex(worldSeed, idx);
        if (d < len) {
            out.index = idx;
            out.len = len;
            out.local = d;
            out.startDepth = endlessStart + cursor;
            out.theme = endlessThemeForIndex(worldSeed, idx);

            out.seed = hashCombine(hashCombine(worldSeed, 0x517A7A5Eu), static_cast<uint32_t>(idx));
            if (out.seed == 0u) out.seed = 1u;
            return out;
        }

        d -= len;
        cursor += len;
        idx += 1;

        // Safety guard for absurd depths (should never trigger in normal play).
        if (idx > 200000) {
            out.index = idx;
            out.len = len;
            out.local = 0;
            out.startDepth = endlessStart + cursor;
            out.theme = endlessThemeForIndex(worldSeed, idx);
            out.seed = hashCombine(hashCombine(worldSeed, 0x517A7A5Eu), static_cast<uint32_t>(idx));
            if (out.seed == 0u) out.seed = 1u;
            return out;
        }
    }
}

// Picks a map size for a newly generated level.
// Philosophy: early floors slightly smaller/tighter, late floors slightly larger.
// In Infinite World mode, post-quest depths get a smooth, band-aligned size/aspect drift so
// infinite descent has large-scale geometric texture instead of one fixed map forever.
static MapSizeWH pickProceduralMapSize(RNG& rng, DungeonBranch branch, int depth, int maxDepth, bool infiniteWorldEnabled, uint32_t worldSeed) {
    MapSizeWH out{Dungeon::DEFAULT_W, Dungeon::DEFAULT_H};

    // ------------------------------------------------------------
    // Surface camp (branch=Camp, depth=0): procedurally sized hub.
    // The main dungeon already varies its size as depth increases, but the
    // surface camp used to be locked to the canonical dimensions. This
    // makes the hub feel more distinct per run and allows the camp
    // generator to scale its palisade + wilderness dressing naturally.
    //
    // Constraints:
    // - Keep within the same hard bounds used by the dungeon generators.
    // - Avoid extreme sizes that make hub traversal tedious.
    // ------------------------------------------------------------
    if (branch == DungeonBranch::Camp && depth == 0) {
        // 3 size tiers: compact / standard / sprawling.
        int wMin = 96, wMax = 124;
        int hMin = 60, hMax = 78;

        const int tierRoll = rng.range(0, 99);
        if (tierRoll < 22) {
            // Compact camp: still roomy enough for a palisade + a few POIs.
            wMin = 84; wMax = 112;
            hMin = 52; hMax = 70;
        } else if (tierRoll >= 86) {
            // Sprawling camp: more wilderness canvas for trails + outer POIs.
            wMin = 110; wMax = 132;
            hMin = 70;  hMax = 86;
        }

        int w = rng.range(wMin, wMax);
        int h = rng.range(hMin, hMax);

        // Mild aspect drift (keeps camps from all feeling like the same rectangle).
        if (rng.chance(0.45f)) {
            const int a = rng.range(3, 10);
            if (rng.chance(0.5f)) {
                w += a;
                h -= a / 2;
            } else {
                w -= a;
                h += a / 2;
            }
        }

        // Tiny jitter so the tier bounds don't create obvious buckets.
        w += rng.range(-2, 2);
        h += rng.range(-1, 1);

        // Clamp to safe bounds for generators (same bounds as the main branch sizing).
        w = std::clamp(w, 80, 132);
        h = std::clamp(h, 50, 86);

        out.w = w;
        out.h = h;
        return out;
    }

    // Keep bespoke/special layouts at the canonical size (they were authored/tuned for it).
    if (depth <= 0) return out;
    if (depth == Dungeon::SOKOBAN_DEPTH) return out;
    if (depth == Dungeon::ROGUE_LEVEL_DEPTH) return out;
    if (maxDepth >= 2 && depth == maxDepth - 1) return out; // labyrinth / pre-sanctum
    if (depth == maxDepth) return out; // sanctum
    if (branch != DungeonBranch::Main) return out;

    // If infinite world is disabled, depth>maxDepth should never happen; keep stable if it does.
    if (!infiniteWorldEnabled && depth > maxDepth) return out;

    // Endless depths: coherent drift tied to the same banding used by the generator.
    if (infiniteWorldEnabled && depth > maxDepth) {
        const EndlessBandInfo st = computeEndlessBand(worldSeed, branch, depth, maxDepth);

        // Theme-shaped base dimensions (kept conservative; generators have hard bounds).
        int baseW = 118;
        int baseH = 74;
        switch (st.theme) {
            case EndlessStratumTheme::Ruins:      baseW = 118; baseH = 74; break;
            case EndlessStratumTheme::Caverns:    baseW = 126; baseH = 82; break;
            case EndlessStratumTheme::Labyrinth:  baseW = 116; baseH = 76; break;
            case EndlessStratumTheme::Warrens:    baseW = 112; baseH = 74; break;
            case EndlessStratumTheme::Mines:      baseW = 110; baseH = 78; break;
            case EndlessStratumTheme::Catacombs:  baseW = 120; baseH = 80; break;
            default:                              baseW = 118; baseH = 74; break;
        }

        // Continuous "knot" values at band boundaries -> smoothstep interpolate within the band.
        // Using knots(index) and knots(index+1) ensures continuity across band boundaries.
        auto knot = [&](uint32_t salt, int idx) -> float {
            const uint32_t h = hash32(hashCombine(hashCombine(worldSeed, salt), static_cast<uint32_t>(idx)));
            return u32ToSignedUnit(h);
        };

        const float t = (st.len > 1) ? (static_cast<float>(st.local) / static_cast<float>(st.len - 1)) : 0.0f;
        const float ft = smoothstep(t);

        const float sizeN = lerpFloat(knot(0x51F17C1Du, st.index), knot(0x51F17C1Du, st.index + 1), ft);
        const float aspN  = lerpFloat(knot(0xA57EC71Du, st.index), knot(0xA57EC71Du, st.index + 1), ft);

        // Deeper into endless => slightly larger amplitude (slow ramp).
        const int extra = std::max(0, depth - (maxDepth + 1));
        const float ramp = std::clamp(extra / 60.0f, 0.0f, 1.0f);

        float sizeAmp = 0.08f + 0.12f * ramp; // 8%..20%
        float aspAmp  = 0.06f + 0.08f * ramp; // 6%..14%

        // Theme tweaks (subtle): caverns feel bigger, labyrinths less so.
        switch (st.theme) {
            case EndlessStratumTheme::Caverns:
                sizeAmp *= 1.10f;
                aspAmp  *= 0.90f;
                break;
            case EndlessStratumTheme::Labyrinth:
                sizeAmp *= 0.95f;
                aspAmp  *= 0.80f;
                break;
            case EndlessStratumTheme::Mines:
                sizeAmp *= 0.95f;
                aspAmp  *= 1.05f;
                break;
            default:
                break;
        }

        const float scale = 1.0f + sizeAmp * sizeN;
        const float aspect = aspAmp * aspN;

        float wf = static_cast<float>(baseW) * scale * (1.0f + aspect);
        float hf = static_cast<float>(baseH) * scale * (1.0f - aspect);

        int w = static_cast<int>(std::round(wf));
        int h = static_cast<int>(std::round(hf));

        // Tiny deterministic per-floor jitter to avoid "perfectly smooth" patterns.
        w += rng.range(-2, 2);
        h += rng.range(-1, 1);

        // Clamp to safe bounds for generators (same bounds as finite-depth sizing).
        w = std::clamp(w, 80, 132);
        h = std::clamp(h, 50, 86);

        w = std::max(w, 32);
        h = std::max(h, 24);

        out.w = w;
        out.h = h;
        return out;
    }

    // Regular finite-depth sizing (unchanged).
    if (depth >= maxDepth) return out;

    // Depth progress 0..(maxDepth-1) (with depth 1 at 0).
    const int den = std::max(1, maxDepth - 1);
    const int num = std::clamp(depth - 1, 0, den);

    // Base size bands.
    // These are intentionally conservative to avoid making floors feel empty or overly sparse.
    const int wMin = 90;
    const int wMax = 124;
    const int hMin = 56;
    const int hMax = 78;

    int w = lerpInt(wMin, wMax, num, den);
    int h = lerpInt(hMin, hMax, num, den);

    // Mild jitter so consecutive floors don't feel "grid-locked".
    w += rng.range(-4, 4);
    h += rng.range(-3, 3);

    // Depth-specific aspect nudges (keeps certain themes feeling distinct).
    if (depth == Dungeon::MINES_DEPTH || depth == Dungeon::DEEP_MINES_DEPTH) {
        w -= 6;
        h += 2;
    }
    if (depth == Dungeon::CATACOMBS_DEPTH) {
        w += 4;
        h += 2;
    }

    // Occasional wider-or-taller bias.
    if (rng.chance(0.35f)) {
        w += rng.range(-6, 6);
    }

    // Clamp to safe bounds for generators.
    w = std::clamp(w, 80, 132);
    h = std::clamp(h, 50, 86);

    // Keep a 1-tile border and reasonable interior.
    w = std::max(w, 32);
    h = std::max(h, 24);

    out.w = w;
    out.h = h;
    return out;
}

} // namespace

Vec2i Game::proceduralMapSizeFor(RNG& rngRef, DungeonBranch branch, int depth) const {
    const MapSizeWH msz = pickProceduralMapSize(rngRef, branch, depth, DUNGEON_MAX_DEPTH, infiniteWorldEnabled_, seed_);
    return Vec2i{msz.w, msz.h};
}

void Game::pushMsg(const std::string& s, MessageKind kind, bool fromPlayer) {
    // Coalesce consecutive identical messages to reduce spam in combat / auto-move.
    // This preserves the original text and adds a repeat counter for the renderer.
    if (!msgs.empty()) {
        Message& last = msgs.back();
        if (last.text == s && last.kind == kind && last.fromPlayer == fromPlayer && last.branch == branch_ && last.depth == depth_) {
            if (last.repeat < 9999) {
                ++last.repeat;
            }
            // Treat the compacted line as "latest occurrence".
            last.turn = turnCount;
            last.branch = branch_;
            last.depth = depth_;
            return;
        }
    }

    auto historyMatches = [&](const Message& m) -> bool {
        if (!messageFilterMatches(msgHistoryFilter, m.kind)) return false;
        if (!msgHistorySearch.empty() && !icontainsAscii(m.text, msgHistorySearch)) return false;
        return true;
    };

    auto historyFilteredCount = [&]() -> int {
        int c = 0;
        for (const auto& m : msgs) if (historyMatches(m)) ++c;
        return c;
    };

    // Keep some scrollback
    if (msgs.size() > 400) {
        msgs.erase(msgs.begin(), msgs.begin() + 100);
        msgScroll = std::min(msgScroll, static_cast<int>(msgs.size()));

        // Also clamp the history overlay scroll.
        if (msgHistoryScroll > 0) {
            msgHistoryScroll = std::min(msgHistoryScroll, std::max(0, historyFilteredCount() - 1));
        }
    }

    Message m;
    m.text = s;
    m.kind = kind;
    m.fromPlayer = fromPlayer;
    m.turn = turnCount;
    m.branch = branch_;
    m.depth = depth_;
    msgs.push_back(m);

    // If not scrolled up, stay pinned to newest.
    if (msgScroll == 0) {
        // pinned
    } else {
        // keep viewing older lines; new messages increase effective scroll
        msgScroll = std::min(msgScroll + 1, static_cast<int>(msgs.size()));
    }

    // Keep message-history viewport stable while scrolled up.
    if (msgHistoryOpen && msgHistoryScroll > 0) {
        if (historyMatches(m)) {
            ++msgHistoryScroll;
        }
        msgHistoryScroll = std::min(msgHistoryScroll, std::max(0, historyFilteredCount() - 1));
    }
}

void Game::pushSystemMessage(const std::string& msg) {
    pushMsg(msg, MessageKind::System, false);
}

void Game::messageHistoryTextInput(const char* utf8) {
    if (!msgHistoryOpen || !msgHistorySearchMode) return;
    if (!utf8) return;
    // Basic cap so the overlay stays sane.
    if (msgHistorySearch.size() > 120) return;
    msgHistorySearch += utf8;
    // New search terms: jump back to newest matches.
    msgHistoryScroll = 0;
}

void Game::messageHistoryBackspace() {
    if (!msgHistoryOpen) return;
    if (msgHistorySearch.empty()) return;
    utf8PopBack(msgHistorySearch);
    msgHistoryScroll = 0;
}

void Game::messageHistoryToggleSearchMode() {
    if (!msgHistoryOpen) return;
    msgHistorySearchMode = !msgHistorySearchMode;
}

void Game::messageHistoryClearSearch() {
    if (!msgHistoryOpen) return;
    msgHistorySearch.clear();
    msgHistoryScroll = 0;
}

void Game::messageHistoryCycleFilter(int dir) {
    if (!msgHistoryOpen) return;
    const int maxFilter = static_cast<int>(MessageFilter::Info);
    int v = static_cast<int>(msgHistoryFilter);
    v += dir;
    if (v < 0) v = maxFilter;
    if (v > maxFilter) v = 0;
    msgHistoryFilter = static_cast<MessageFilter>(v);
    msgHistoryScroll = 0;
}


std::string Game::messageHistoryClipboardText() const {
    std::ostringstream f;
    f << PROCROGUE_APPNAME << " message history\n";
    f << "Filter: " << messageFilterDisplayName(msgHistoryFilter);
    if (!msgHistorySearch.empty()) {
        f << "  Search: \"" << msgHistorySearch << "\"";
    }
    f << "\n\n";

    int shown = 0;

    for (const auto& m : msgs) {
        if (!messageFilterMatches(msgHistoryFilter, m.kind)) continue;
        if (!msgHistorySearch.empty() && !icontainsAscii(m.text, msgHistorySearch)) continue;

        ++shown;

        const char* k = (m.kind == MessageKind::Info)         ? "INFO"
                      : (m.kind == MessageKind::Combat)       ? "COMBAT"
                      : (m.kind == MessageKind::Loot)         ? "LOOT"
                      : (m.kind == MessageKind::System)       ? "SYSTEM"
                      : (m.kind == MessageKind::ImportantMsg) ? "IMPORTANT"
                      : (m.kind == MessageKind::Warning)      ? "WARNING"
                      : (m.kind == MessageKind::Success)      ? "SUCCESS"
                                                             : "INFO";

        const std::string depthTag = (m.branch == DungeonBranch::Camp)
            ? std::string("CAMP")
            : std::string("D") + std::to_string(m.depth);

        f << "[" << k << "] [" << depthTag << " T" << m.turn << "] " << m.text;
        if (m.repeat > 1) f << " (x" << m.repeat << ")";
        f << "\n";
    }

    if (shown == 0) {
        f << "(no messages matched)\n";
    } else {
        f << "\n" << shown << "/" << msgs.size() << " messages shown\n";
    }

    return f.str();
}

void Game::buildCodexList(std::vector<EntityKind>& out) const {
    out.clear();
    out.reserve(ENTITY_KIND_COUNT);

    for (int i = 0; i < ENTITY_KIND_COUNT; ++i) {
        EntityKind k = static_cast<EntityKind>(i);
        if (k == EntityKind::Player) continue;

        if (codexFilter_ == CodexFilter::Seen && !codexHasSeen(k)) continue;
        if (codexFilter_ == CodexFilter::Killed && codexKills(k) == 0) continue;

        out.push_back(k);
    }

    if (codexSort_ == CodexSort::KillsDesc) {
        std::stable_sort(out.begin(), out.end(), [&](EntityKind a, EntityKind b) {
            const uint16_t ka = codexKills(a);
            const uint16_t kb = codexKills(b);
            if (ka != kb) return ka > kb;
            return std::string(entityKindName(a)) < std::string(entityKindName(b));
        });
    }
}

std::string Game::discoveryAppearanceLabel(ItemKind k) const {
    // Build an "unidentified" label without any BUC/enchant prefix noise.
    // (Discoveries is about appearance -> true name mapping.)
    Item tmp;
    tmp.kind = k;
    tmp.count = 1;
    tmp.buc = 0;
    tmp.enchant = 0;
    tmp.charges = 0;
    tmp.shopPrice = 0;
    tmp.shopDepth = 0;
    return unknownDisplayName(tmp);
}

void Game::buildDiscoveryList(std::vector<ItemKind>& out) const {
    buildDiscoveryList(out, discoveriesFilter_, discoveriesSort_);
}

void Game::buildDiscoveryList(std::vector<ItemKind>& out, DiscoveryFilter filter, DiscoverySort sort) const {
    out.clear();
    out.reserve(64);

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

    for (int i = 0; i < ITEM_KIND_COUNT; ++i) {
        const ItemKind k = static_cast<ItemKind>(i);
        if (!isIdentifiableKind(k)) continue;
        if (!matches(k)) continue;
        out.push_back(k);
    }

    auto catOrder = [&](ItemKind k) -> int {
        if (isPotionKind(k)) return 0;
        if (isScrollKind(k)) return 1;
        if (isRingKind(k))   return 2;
        if (isWandKind(k))   return 3;
        return 4;
    };

    std::stable_sort(out.begin(), out.end(), [&](ItemKind a, ItemKind b) {
        // When showing "ALL", keep categories grouped in a predictable order.
        if (filter == DiscoveryFilter::All) {
            const int ca = catOrder(a);
            const int cb = catOrder(b);
            if (ca != cb) return ca < cb;
        }

        if (sort == DiscoverySort::IdentifiedFirst) {
            const bool ia = isIdentified(a);
            const bool ib = isIdentified(b);
            if (ia != ib) return ia > ib;
        }

        const std::string sa = discoveryAppearanceLabel(a);
        const std::string sb = discoveryAppearanceLabel(b);
        if (sa != sb) return sa < sb;

        // Tie-breaker: stable by true name.
        return itemDisplayNameSingle(a) < itemDisplayNameSingle(b);
    });
}

void Game::buildScoresList(std::vector<size_t>& out) const {
    out.clear();
    const auto& es = scores.entries();
    out.reserve(es.size());
    for (size_t i = 0; i < es.size(); ++i) out.push_back(i);

    if (scoresView_ == ScoresView::Recent) {
        // Newest first; keep tie-breakers stable so the list doesn't jitter.
        std::stable_sort(out.begin(), out.end(), [&](size_t ia, size_t ib) {
            const ScoreEntry& a = es[ia];
            const ScoreEntry& b = es[ib];
            if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
            if (a.score != b.score) return a.score > b.score;
            if (a.won != b.won) return a.won > b.won;
            if (a.turns != b.turns) return a.turns < b.turns;
            if (a.name != b.name) return a.name < b.name;
            return a.cause < b.cause;
        });
    }
}

Entity* Game::entityById(int id) {
    for (auto& e : ents) if (e.id == id) return &e;
    return nullptr;
}

const Entity* Game::entityById(int id) const {
    for (const auto& e : ents) if (e.id == id) return &e;
    return nullptr;
}

Entity* Game::entityAtMut(int x, int y) {
    for (auto& e : ents) {
        if (e.hp > 0 && e.pos.x == x && e.pos.y == y) return &e;
    }
    return nullptr;
}

const Entity* Game::entityAt(int x, int y) const {
    for (const auto& e : ents) {
        if (e.hp > 0 && e.pos.x == x && e.pos.y == y) return &e;
    }
    return nullptr;
}

int Game::equippedMeleeIndex() const {
    return findItemIndexById(inv, equipMeleeId);
}

int Game::equippedRangedIndex() const {
    return findItemIndexById(inv, equipRangedId);
}

int Game::equippedArmorIndex() const {
    return findItemIndexById(inv, equipArmorId);
}

int Game::equippedRing1Index() const {
    return findItemIndexById(inv, equipRing1Id);
}

int Game::equippedRing2Index() const {
    return findItemIndexById(inv, equipRing2Id);
}

const Item* Game::equippedMelee() const {
    int idx = equippedMeleeIndex();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

const Item* Game::equippedRanged() const {
    int idx = equippedRangedIndex();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

const Item* Game::equippedArmor() const {
    int idx = equippedArmorIndex();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

const Item* Game::equippedRing1() const {
    int idx = equippedRing1Index();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

const Item* Game::equippedRing2() const {
    int idx = equippedRing2Index();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

bool Game::isEquipped(int itemId) const {
    return itemId != 0 && (itemId == equipMeleeId || itemId == equipRangedId || itemId == equipArmorId || itemId == equipRing1Id || itemId == equipRing2Id);
}

std::string Game::equippedTag(int itemId) const {
    std::string t;
    if (itemId != 0 && itemId == equipMeleeId) t += "M";
    if (itemId != 0 && itemId == equipRangedId) t += "R";
    if (itemId != 0 && itemId == equipArmorId) t += "A";
    if (itemId != 0 && itemId == equipRing1Id) t += "1";
    if (itemId != 0 && itemId == equipRing2Id) t += "2";
    return t;
}

std::string Game::equippedMeleeName() const {
    const Item* w = equippedMelee();
    return w ? displayItemName(*w) : std::string("(NONE)");
}

std::string Game::equippedRangedName() const {
    const Item* w = equippedRanged();
    return w ? displayItemName(*w) : std::string("(NONE)");
}

std::string Game::equippedArmorName() const {
    const Item* a = equippedArmor();
    return a ? displayItemName(*a) : std::string("(NONE)");
}

std::string Game::equippedRing1Name() const {
    const Item* r = equippedRing1();
    return r ? displayItemName(*r) : std::string("(NONE)");
}

std::string Game::equippedRing2Name() const {
    const Item* r = equippedRing2();
    return r ? displayItemName(*r) : std::string("(NONE)");
}

namespace {
inline int bucScalar(const Item& it) {
    return (it.buc < 0 ? -1 : (it.buc > 0 ? 1 : 0));
}

inline int applyGearModIfNonZero(int base, const Item& it) {
    if (base == 0) return 0;
    return base + it.enchant + bucScalar(it);
}
}

int Game::ringTalentBonusMight() const {
    int b = 0;
    if (const Item* r = equippedRing1()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modMight, *r);
    }
    if (const Item* r = equippedRing2()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modMight, *r);
    }
    return b;
}

int Game::ringTalentBonusAgility() const {
    int b = 0;
    if (const Item* r = equippedRing1()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modAgility, *r);
    }
    if (const Item* r = equippedRing2()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modAgility, *r);
    }
    return b;
}

int Game::ringTalentBonusVigor() const {
    int b = 0;
    if (const Item* r = equippedRing1()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modVigor, *r);
    }
    if (const Item* r = equippedRing2()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modVigor, *r);
    }
    return b;
}

int Game::ringTalentBonusFocus() const {
    int b = 0;
    if (const Item* r = equippedRing1()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modFocus, *r);
    }
    if (const Item* r = equippedRing2()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.modFocus, *r);
    }
    return b;
}

int Game::ringDefenseBonus() const {
    int b = 0;
    if (const Item* r = equippedRing1()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.defense, *r);
    }
    if (const Item* r = equippedRing2()) {
        const ItemDef& d = itemDef(r->kind);
        b += applyGearModIfNonZero(d.defense, *r);
    }
    return b;
}

int Game::playerAttack() const {
    int atk = playerMeleePower();
    if (const Item* w = equippedMelee()) {
        atk += itemDef(w->kind).meleeAtk;
        atk += w->enchant;
        atk += (w->buc < 0 ? -1 : (w->buc > 0 ? 1 : 0));
    }
    return atk;
}

int Game::playerDefense() const {
    int def = playerEvasion();
    if (const Item* a = equippedArmor()) {
        def += itemDef(a->kind).defense;
        def += a->enchant;
        def += (a->buc < 0 ? -1 : (a->buc > 0 ? 1 : 0));
    }

    // Rings can provide small passive defense (e.g., Ring of Protection).
    def += ringDefenseBonus();
    // Temporary shielding buff
    if (player().effects.shieldTurns > 0) def += 2;

    // Corrosion reduces effective protection (represents pitted armor / burned skin).
    if (player().effects.corrosionTurns > 0) {
        const int p = clampi(1 + player().effects.corrosionTurns / 4, 1, 3);
        def -= p;
    }
    return def;
}

int Game::playerRangedRange() const {
    // Preferred: an equipped ranged weapon that is actually ready (ammo/charges).
    if (const Item* w = equippedRanged()) {
        const ItemDef& d = itemDef(w->kind);
        const bool hasRange = (d.range > 0);
        const bool chargesOk = (d.maxCharges <= 0) || (w->charges > 0);
        const bool ammoOk = (d.ammo == AmmoKind::None) || (ammoCount(inv, d.ammo) > 0);

        if (hasRange && chargesOk && ammoOk) {
            return d.range;
        }
    }

    // Fallback: "throw by hand" when you have ammo (rocks/arrows) but no usable ranged weapon.
    ThrowAmmoSpec spec;
    if (choosePlayerThrowAmmo(inv, spec)) {
        return throwRangeFor(player(), spec.ammo);
    }

    return 0;
}


bool Game::playerHasRangedReady(std::string* reasonOut) const {
    // Prefer an equipped ranged weapon when it is ready (ammo/charges).
    if (const Item* w = equippedRanged()) {
        const ItemDef& d = itemDef(w->kind);

        const bool hasRange = (d.range > 0);
        const bool chargesOk = (d.maxCharges <= 0) || (w->charges > 0);
        const bool ammoOk = (d.ammo == AmmoKind::None) || (ammoCount(inv, d.ammo) > 0);

        if (hasRange && chargesOk && ammoOk) {
            return true;
        }

        // Fallback: if the equipped weapon can't be used (no ammo/charges), allow throwing
        // (rocks/arrows) so the player still has a ranged option without menu friction.
        ThrowAmmoSpec spec;
        if (choosePlayerThrowAmmo(inv, spec)) {
            return true;
        }

        // No fallback available: explain why the equipped weapon can't be used.
        if (!hasRange) {
            if (reasonOut) *reasonOut = "THAT WEAPON CAN'T FIRE.";
            return false;
        }
        if (!chargesOk) {
            if (reasonOut) *reasonOut = "THE WAND IS OUT OF CHARGES.";
            return false;
        }
        if (!ammoOk) {
            if (reasonOut) {
                *reasonOut = (d.ammo == AmmoKind::Arrow) ? "NO ARROWS." : "NO ROCKS.";
            }
            return false;
        }
    }

    // No equipped ranged weapon: allow throwing ammo by hand if available.
    ThrowAmmoSpec spec;
    if (choosePlayerThrowAmmo(inv, spec)) return true;

    if (reasonOut) *reasonOut = "NO RANGED WEAPON OR THROWABLE AMMO.";
    return false;
}


int Game::playerManaMax() const {
    // Simple initial formula (subject to rebalancing): base + focus scaling + a tiny level scaling.
    const int base = 6;
    const int focus = std::max(0, playerFocus());
    const int lvl = std::max(1, playerCharLevel());

    int maxMana = base + focus * 2 + (lvl - 1);
    maxMana = clampi(maxMana, 0, 99);
    return maxMana;
}

bool Game::knowsSpell(SpellKind k) const {
    const uint32_t idx = static_cast<uint32_t>(k);
    if (idx >= 32u) return false;
    return (knownSpellsMask_ & (1u << idx)) != 0u;
}

std::vector<SpellKind> Game::knownSpellsList() const {
    std::vector<SpellKind> out;
    out.reserve(8);
    for (int i = 0; i < SPELL_KIND_COUNT; ++i) {
        SpellKind k = static_cast<SpellKind>(i);
        if (knowsSpell(k)) out.push_back(k);
    }
    return out;
}

SpellKind Game::selectedSpell() const {
    const std::vector<SpellKind> ks = knownSpellsList();
    if (ks.empty()) return SpellKind::MagicMissile;
    const int idx = clampi(spellsSel, 0, static_cast<int>(ks.size()) - 1);
    return ks[static_cast<size_t>(idx)];
}


int Game::xpFor(EntityKind k) const {
    switch (k) {
        case EntityKind::Goblin: return 8;
        case EntityKind::Bat: return 6;
        case EntityKind::Slime: return 10;
        case EntityKind::Snake: return 12;
        case EntityKind::Spider: return 14;
        case EntityKind::KoboldSlinger: return 12;
        case EntityKind::SkeletonArcher: return 16;
        case EntityKind::Wolf: return 10;
        case EntityKind::Orc: return 14;
        case EntityKind::Troll: return 28;
        case EntityKind::Ogre: return 30;
        case EntityKind::Wizard: return 32;
        case EntityKind::Ghost: return 34;
        case EntityKind::Leprechaun: return 18;
        case EntityKind::Nymph: return 24;
        case EntityKind::Zombie: return 18;
        case EntityKind::Mimic: return 22;
        case EntityKind::Minotaur: return 45;
        case EntityKind::Shopkeeper: return 0;
        case EntityKind::Dog: return 0;
        default: return 10;
    }
}


int Game::xpFor(const Entity& e) const {
    const int base = xpFor(e.kind);
    if (base <= 0) return base;

    const int affCount = procMonsterAffixCount(e.procAffixMask);
    const int abilCount = procMonsterAbilityCount(e.procAbility1, e.procAbility2);

    int tierBonusPct = 0;
    switch (e.procRank) {
        case ProcMonsterRank::Elite:    tierBonusPct = 40; break;
        case ProcMonsterRank::Champion: tierBonusPct = 120; break;
        case ProcMonsterRank::Mythic:   tierBonusPct = 220; break;
        default:                        tierBonusPct = 0; break;
    }

    const int affBonusPct = affCount * 25;  // +25% per affix
    const int abilBonusPct = abilCount * 20; // +20% per proc ability
    const int pct = 100 + tierBonusPct + affBonusPct + abilBonusPct;

    if (pct == 100) return base;

    const int64_t scaled = static_cast<int64_t>(base) * static_cast<int64_t>(pct);
    int xpVal = static_cast<int>((scaled + 50) / 100); // rounded
    xpVal = std::max(base, xpVal);

    // Hard cap against extreme future combinations.
    xpVal = std::min(xpVal, base * 10);

    return xpVal;
}

void Game::grantXp(int amount) {
    if (amount <= 0) return;
    xp += amount;

    std::ostringstream ss;
    ss << "YOU GAIN " << amount << " XP.";
    pushMsg(ss.str(), MessageKind::Success);

    while (xp >= xpNext) {
        xp -= xpNext;
        charLevel += 1;
        // Scale XP requirement for the next level.
        xpNext = static_cast<int>(xpNext * 1.35f + 10);
        onPlayerLevelUp();
    }
}

void Game::onPlayerLevelUp() {
    Entity& p = playerMut();

    const int hpGain = 2 + rng.range(0, 2);
    p.hpMax += hpGain;

    bool atkUp = false;
    bool defUp = false;
    if (charLevel % 2 == 0) {
        p.baseAtk += 1;
        atkUp = true;
    }
    if (charLevel % 3 == 0) {
        p.baseDef += 1;
        defUp = true;
    }

    // Full heal on level up.
    p.hp = p.hpMax;

    std::ostringstream ss;
    ss << "LEVEL UP! YOU ARE NOW LEVEL " << charLevel << ".";
    pushMsg(ss.str(), MessageKind::Success);

    std::ostringstream ss2;
    ss2 << "+" << hpGain << " MAX HP";
    if (atkUp) ss2 << ", +1 ATK";
    if (defUp) ss2 << ", +1 DEF";
    ss2 << ".";
    pushMsg(ss2.str(), MessageKind::Success);

    // Award a talent point and force the allocation overlay to open.
    talentPointsPending_ += 1;
    levelUpOpen = true;
    levelUpSel = std::clamp(levelUpSel, 0, 3);

    // Cancel other UI modes / automation so the player can make a choice.
    invOpen = false;
    invIdentifyMode = false;
    invEnchantRingMode = false;
    invPrompt_ = InvPromptKind::None;
    closeChestOverlay();
    targeting = false;
    targetLine.clear();
    targetValid = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    optionsOpen = false;
    commandOpen = false;
    looking = false;
    stopAutoMove(true);

    pushMsg("CHOOSE A TALENT: MIGHT / AGILITY / VIGOR / FOCUS (ARROWS + ENTER).", MessageKind::System, true);
}

bool Game::playerHasAmulet() const {
    for (const auto& it : inv) {
        if (it.kind == ItemKind::AmuletYendor) return true;
    }
    return false;
}

// ------------------------------------------------------------
// Yendor Doom (endgame escalation)
//
// Once the player acquires the Amulet of Yendor, the dungeon begins applying
// pressure through periodic "noise pulses" (waking/alerting monsters) and
// hunter packs that spawn out of sight.
// ------------------------------------------------------------

int Game::computeYendorDoomLevel() const {
    if (!yendorDoomActive_) return 0;

    const uint32_t now = turnCount;
    const uint32_t start = yendorDoomStartTurn_;
    const int turnsSince = (now >= start) ? static_cast<int>(now - start) : 0;

    // 0 at quest depth, grows as the player ascends toward the surface.
    const int ascension = std::max(0, QUEST_DEPTH - depth_);

    // Time pressure: +1 every ~40 player turns after acquiring the Amulet.
    const int timeTerm = turnsSince / 40;

    int lvl = 1 + (ascension * 2) + timeTerm;
    return std::clamp(lvl, 1, 20);
}

void Game::onAmuletAcquired() {
    if (!yendorDoomEnabled_) return;
    if (gameOver || gameWon) return;
    if (yendorDoomActive_) return;

    yendorDoomActive_ = true;
    if (yendorDoomStartTurn_ == 0u) yendorDoomStartTurn_ = turnCount;
    yendorDoomLastPulseTurn_ = turnCount;
    yendorDoomLastSpawnTurn_ = turnCount;
    yendorDoomMsgStage_ = 0;
    yendorDoomLevel_ = computeYendorDoomLevel();

    pushMsg("THE DUNGEON STIRS. SOMETHING HUNTS YOU...", MessageKind::ImportantMsg, true);
}

void Game::spawnYendorHunterPack(int doomLevel) {
    if (dung.rooms.empty()) return;
    if (ents.size() >= 120u) return; // keep things sane on huge levels

    const Vec2i ppos = player().pos;

    // How many hunters? Scale slowly with doom.
    int count = 2 + (doomLevel / 5);
    if (rng.range(0, 3) == 0) count += 1;
    count = std::clamp(count, 2, 6);

    auto pickKind = [&](int dlvl) -> EntityKind {
        // Weighted selection by doom intensity.
        const int r = rng.range(0, 99);
        if (dlvl >= 18) {
            if (r < 10) return EntityKind::Minotaur;
            if (r < 30) return EntityKind::Wizard;
            if (r < 55) return EntityKind::Troll;
            if (r < 80) return EntityKind::Ogre;
            return EntityKind::SkeletonArcher;
        }
        if (dlvl >= 12) {
            if (r < 10) return EntityKind::Wizard;
            if (r < 35) return EntityKind::Troll;
            if (r < 60) return EntityKind::Ogre;
            if (r < 80) return EntityKind::SkeletonArcher;
            return EntityKind::Wolf;
        }
        if (dlvl >= 6) {
            if (r < 25) return EntityKind::Ogre;
            if (r < 50) return EntityKind::SkeletonArcher;
            if (r < 70) return EntityKind::Orc;
            if (r < 85) return EntityKind::Wolf;
            return EntityKind::KoboldSlinger;
        }
        // Early doom: mostly fast/annoying hunters.
        if (r < 35) return EntityKind::Wolf;
        if (r < 60) return EntityKind::Orc;
        if (r < 80) return EntityKind::SkeletonArcher;
        return EntityKind::KoboldSlinger;
    };

    // Find a spawn point out of sight and not too close to the player.
    Vec2i anchor{-1, -1};
    for (int attempt = 0; attempt < 50; ++attempt) {
        const Room& r = dung.rooms[static_cast<size_t>(rng.range(0, static_cast<int>(dung.rooms.size() - 1)))];
        if (r.type == RoomType::Shop) continue;

        // Avoid spawning in the player's current room.
        if (ppos.x >= r.x && ppos.x < r.x + r.w && ppos.y >= r.y && ppos.y < r.y + r.h) continue;

        Vec2i p = randomFreeTileInRoom(r);
        if (!dung.inBounds(p.x, p.y)) continue;
        if (dung.at(p.x, p.y).visible) continue;
        const int dist = std::abs(p.x - ppos.x) + std::abs(p.y - ppos.y);
        if (dist < 10) continue;

        anchor = p;
        break;
    }

    // Fallback: random hidden floor tile.
    if (anchor.x < 0) {
        for (int attempt = 0; attempt < 400; ++attempt) {
            const int x = rng.range(1, dung.width - 2);
            const int y = rng.range(1, dung.height - 2);
            if (!dung.inBounds(x, y)) continue;
            const TileType t = dung.at(x, y).type;
            if (!(t == TileType::Floor || t == TileType::DoorOpen || t == TileType::StairsUp || t == TileType::StairsDown)) continue;
            if (entityAt(x, y)) continue;
            if (dung.at(x, y).visible) continue;
            const int dist = std::abs(x - ppos.x) + std::abs(y - ppos.y);
            if (dist < 10) continue;
            anchor = {x, y};
            break;
        }
    }

    if (anchor.x < 0) return;

    const int gid = nextEntityId; // good enough unique group id
    for (int i = 0; i < count; ++i) {
        Vec2i pos = anchor;

        // Spread slightly around the anchor.
        for (int j = 0; j < 12; ++j) {
            const int dx = rng.range(-2, 2);
            const int dy = rng.range(-2, 2);
            if (dx == 0 && dy == 0) continue;
            Vec2i q{anchor.x + dx, anchor.y + dy};
            if (!dung.inBounds(q.x, q.y)) continue;
            const TileType t = dung.at(q.x, q.y).type;
            if (!(t == TileType::Floor || t == TileType::DoorOpen || t == TileType::StairsUp || t == TileType::StairsDown)) continue;
            if (entityAt(q.x, q.y)) continue;
            if (dung.at(q.x, q.y).visible) continue;
            pos = q;
            break;
        }

        const EntityKind kind = pickKind(doomLevel);
        spawnMonster(kind, pos, gid, true);

        // Hunters begin alerted and immediately know (roughly) where you are.
        Entity& m = ents.back();
        m.alerted = true;
        m.lastKnownPlayerPos = ppos;
        m.lastKnownPlayerAge = 0;
    }

    // Lightly telegraph without being too spammy.
    if (rng.range(0, 2) == 0) {
        pushMsg("YOU FEEL A MALEVOLENT PRESENCE DRAWING NEAR.", MessageKind::ImportantMsg);
    }
}

void Game::tickYendorDoom() {
    if (!yendorDoomEnabled_) return;
    if (gameOver || gameWon) return;

    // Safety: if the player no longer has the Amulet, the system should not run.
    if (!playerHasAmulet()) {
        yendorDoomActive_ = false;
        yendorDoomLevel_ = 0;
        return;
    }

    // If we loaded a legacy save with an Amulet already in hand, start silently.
    if (!yendorDoomActive_) {
        yendorDoomActive_ = true;
        if (yendorDoomStartTurn_ == 0u) yendorDoomStartTurn_ = turnCount;
        if (yendorDoomLastPulseTurn_ == 0u) yendorDoomLastPulseTurn_ = turnCount;
        if (yendorDoomLastSpawnTurn_ == 0u) yendorDoomLastSpawnTurn_ = turnCount;
    }

    const int lvl = computeYendorDoomLevel();
    yendorDoomLevel_ = lvl;

    // One-time escalating warnings.
    if (lvl >= 6 && yendorDoomMsgStage_ < 1) {
        yendorDoomMsgStage_ = 1;
        pushMsg("THE AIR GROWS HEAVY WITH DREAD.", MessageKind::ImportantMsg);
    } else if (lvl >= 12 && yendorDoomMsgStage_ < 2) {
        yendorDoomMsgStage_ = 2;
        pushMsg("THE DUNGEON'S RAGE BUILDS BEHIND YOU.", MessageKind::ImportantMsg);
    } else if (lvl >= 18 && yendorDoomMsgStage_ < 3) {
        yendorDoomMsgStage_ = 3;
        pushMsg("THE VERY STONES SCREAM FOR YOUR BLOOD!", MessageKind::ImportantMsg);
    }

    const uint32_t now = turnCount;
    const int pulseEvery = std::clamp(60 - (lvl * 2), 15, 60);
    const int spawnEvery = std::clamp(90 - (lvl * 3), 25, 90);

    if (now - yendorDoomLastPulseTurn_ >= static_cast<uint32_t>(pulseEvery)) {
        yendorDoomLastPulseTurn_ = now;
        const int volume = 10 + lvl; // louder as doom rises
        emitNoise(player().pos, volume);

        if (lvl >= 10 && rng.range(0, 4) == 0) {
            pushMsg("THE AMULET PULSES WITH DARK POWER.", MessageKind::System);
        }
    }

    if (now - yendorDoomLastSpawnTurn_ >= static_cast<uint32_t>(spawnEvery)) {
        yendorDoomLastSpawnTurn_ = now;
        spawnYendorHunterPack(lvl);
    }
}

// ------------------------------------------------------------
// Identification (items start unknown; appearances randomized per run)
// ------------------------------------------------------------

void Game::initIdentificationTables() {
    identKnown.fill(1);
    identAppearance.fill(0);
    identCall.fill(std::string());

    if (!identifyItemsEnabled) {
        // All items show true names.
        return;
    }

    // Mark identifiable kinds as unknown by default.
    for (ItemKind k : POTION_KINDS) {
        identKnown[static_cast<size_t>(k)] = 0;
    }
    for (ItemKind k : SCROLL_KINDS) {
        identKnown[static_cast<size_t>(k)] = 0;
    }
    for (ItemKind k : RING_KINDS) {
        identKnown[static_cast<size_t>(k)] = 0;
    }
    for (ItemKind k : WAND_KINDS) {
        identKnown[static_cast<size_t>(k)] = 0;
    }

    // Build a random 1:1 mapping of appearance tokens to each kind.
    auto shuffledIndices = [&](size_t n) {
        std::vector<uint8_t> idx;
        idx.reserve(n);
        for (size_t i = 0; i < n; ++i) idx.push_back(static_cast<uint8_t>(i));
        for (size_t i = n; i-- > 1;) {
            const int j = rng.range(0, static_cast<int>(i));
            std::swap(idx[i], idx[static_cast<size_t>(j)]);
        }
        return idx;
    };

    const size_t potionN = sizeof(POTION_KINDS) / sizeof(POTION_KINDS[0]);
    const size_t scrollN = sizeof(SCROLL_KINDS) / sizeof(SCROLL_KINDS[0]);
    const size_t ringN = sizeof(RING_KINDS) / sizeof(RING_KINDS[0]);
    const size_t wandN = sizeof(WAND_KINDS) / sizeof(WAND_KINDS[0]);

    const size_t potionAppearN = sizeof(POTION_APPEARANCES) / sizeof(POTION_APPEARANCES[0]);
    const size_t scrollAppearN = sizeof(SCROLL_APPEARANCES) / sizeof(SCROLL_APPEARANCES[0]);
    const size_t ringAppearN = sizeof(RING_APPEARANCES) / sizeof(RING_APPEARANCES[0]);
    const size_t wandAppearN = sizeof(WAND_APPEARANCES) / sizeof(WAND_APPEARANCES[0]);

    std::vector<uint8_t> p = shuffledIndices(potionAppearN);
    std::vector<uint8_t> s = shuffledIndices(scrollAppearN);
    std::vector<uint8_t> r = shuffledIndices(ringAppearN);
    std::vector<uint8_t> w = shuffledIndices(wandAppearN);

    // If someone later adds more kinds than appearances, we still function
    // (we'll reuse appearances), but keep the common case unique.
    for (size_t i = 0; i < potionN; ++i) {
        const uint8_t app = p[i % p.size()];
        identAppearance[static_cast<size_t>(POTION_KINDS[i])] = app;
    }
    for (size_t i = 0; i < scrollN; ++i) {
        const uint8_t app = s[i % s.size()];
        identAppearance[static_cast<size_t>(SCROLL_KINDS[i])] = app;
    }
    for (size_t i = 0; i < ringN; ++i) {
        const uint8_t app = r[i % r.size()];
        identAppearance[static_cast<size_t>(RING_KINDS[i])] = app;
    }
    for (size_t i = 0; i < wandN; ++i) {
        const uint8_t app = w[i % w.size()];
        identAppearance[static_cast<size_t>(WAND_KINDS[i])] = app;
    }
}

bool Game::isIdentified(ItemKind k) const {
    if (!identifyItemsEnabled) return true;
    const size_t idx = static_cast<size_t>(k);
    if (idx >= static_cast<size_t>(ITEM_KIND_COUNT)) return true;
    return identKnown[idx] != 0;
}

uint8_t Game::appearanceFor(ItemKind k) const {
    const size_t idx = static_cast<size_t>(k);
    if (idx >= static_cast<size_t>(ITEM_KIND_COUNT)) return 0;
    return identAppearance[idx];
}

std::string Game::appearanceName(ItemKind k) const {
    if (isPotionKind(k)) {
        constexpr size_t n = sizeof(POTION_APPEARANCES) / sizeof(POTION_APPEARANCES[0]);
        static_assert(n > 0, "POTION_APPEARANCES must not be empty");
        uint8_t a = appearanceFor(k);
        if (a >= n) a = static_cast<uint8_t>(a % n);
        return POTION_APPEARANCES[a];
    }
    if (isScrollKind(k)) {
        constexpr size_t n = sizeof(SCROLL_APPEARANCES) / sizeof(SCROLL_APPEARANCES[0]);
        static_assert(n > 0, "SCROLL_APPEARANCES must not be empty");
        uint8_t a = appearanceFor(k);
        if (a >= n) a = static_cast<uint8_t>(a % n);
        return SCROLL_APPEARANCES[a];
    }
    if (isRingKind(k)) {
        constexpr size_t n = sizeof(RING_APPEARANCES) / sizeof(RING_APPEARANCES[0]);
        static_assert(n > 0, "RING_APPEARANCES must not be empty");
        uint8_t a = appearanceFor(k);
        if (a >= n) a = static_cast<uint8_t>(a % n);
        return RING_APPEARANCES[a];
    }
    if (isWandKind(k)) {
        constexpr size_t n = sizeof(WAND_APPEARANCES) / sizeof(WAND_APPEARANCES[0]);
        static_assert(n > 0, "WAND_APPEARANCES must not be empty");
        uint8_t a = appearanceFor(k);
        if (a >= n) a = static_cast<uint8_t>(a % n);
        return WAND_APPEARANCES[a];
    }
    return "";
}

std::string Game::unknownDisplayName(const Item& it) const {
    std::ostringstream ss;

    auto appendGearPrefix = [&]() {
        if (!isWearableGear(it.kind)) return;

        if (itemIsArtifact(it)) ss << "ARTIFACT ";

        if (it.buc < 0) ss << "CURSED ";
        else if (it.buc > 0) ss << "BLESSED ";

        if (it.enchant != 0) {
            if (it.enchant > 0) ss << "+";
            ss << it.enchant << " ";
        }
    };

    if (isPotionKind(it.kind)) {
        const std::string app = appearanceName(it.kind);
        if (it.count > 1) ss << it.count << " " << app << " POTIONS";
        else ss << app << " POTION";
    } else if (isScrollKind(it.kind)) {
        const std::string app = appearanceName(it.kind);
        if (it.count > 1) ss << it.count << " SCROLLS '" << app << "'";
        else ss << "SCROLL '" << app << "'";
    } else if (isRingKind(it.kind)) {
        appendGearPrefix();
        const std::string app = appearanceName(it.kind);
        ss << app << " RING";
    } else if (isWandKind(it.kind)) {
        appendGearPrefix();
        const std::string app = appearanceName(it.kind);
        ss << app << " WAND";
    } else {
        // Non-identifiable kinds keep their normal names.
        return itemDisplayName(it);
    }

    // NetHack-style "call" label: user notes about an unidentified appearance.
    const size_t callIdx = static_cast<size_t>(it.kind);
    if (callIdx < static_cast<size_t>(ITEM_KIND_COUNT)) {
        const std::string& note = identCall[callIdx];
        if (!note.empty()) {
            std::string shown = note;
            if (shown.size() > 28) shown.resize(28);
            ss << " {" << toUpper(shown) << "}";
        }
    }

    // Preserve shop price tags even while unidentified.
    if (it.shopPrice > 0 && it.shopDepth > 0) {
        const ItemDef& d = itemDef(it.kind);
        const int n = d.stackable ? std::max(1, it.count) : 1;
        const int total = it.shopPrice * n;
        ss << " [PRICE " << total << "G]";
    }

    return ss.str();
}

bool Game::markIdentified(ItemKind k, bool quiet) {
    if (!identifyItemsEnabled) return false;
    if (!isIdentifiableKind(k)) return false;
    const size_t idx = static_cast<size_t>(k);
    if (idx >= static_cast<size_t>(ITEM_KIND_COUNT)) return false;
    if (identKnown[idx] != 0) return false;
    identKnown[idx] = 1;

    if (!quiet) {
        Item tmp;
        tmp.kind = k;
        tmp.count = 1;
        const std::string oldName = unknownDisplayName(tmp);
        const std::string newName = itemDisplayNameSingle(k);
        pushMsg("IDENTIFIED: " + oldName + " = " + newName + ".", MessageKind::System, true);
    }

    return true;
}

std::string Game::displayItemName(const Item& it) const {
    if (!identifyItemsEnabled) return itemDisplayName(it);
    if (!isIdentifiableKind(it.kind)) return itemDisplayName(it);
    return isIdentified(it.kind) ? itemDisplayName(it) : unknownDisplayName(it);
}

std::string Game::displayItemNameSingle(ItemKind k) const {
    Item tmp;
    tmp.kind = k;
    tmp.count = 1;
    return displayItemName(tmp);
}

bool Game::hasItemCallLabel(ItemKind k) const {
    if (!isIdentifiableKind(k)) return false;
    const size_t idx = static_cast<size_t>(k);
    if (idx >= static_cast<size_t>(ITEM_KIND_COUNT)) return false;
    return !identCall[idx].empty();
}

std::string Game::itemCallLabel(ItemKind k) const {
    if (!isIdentifiableKind(k)) return std::string();
    const size_t idx = static_cast<size_t>(k);
    if (idx >= static_cast<size_t>(ITEM_KIND_COUNT)) return std::string();
    return identCall[idx];
}

static std::string sanitizeCallLabel(std::string s) {
    s = trim(std::move(s));
    if (s.empty()) return std::string();

    // Strip control chars so the HUD/CSV/log remain clean.
    std::string filtered;
    filtered.reserve(s.size());
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc == 127) continue;
        filtered.push_back(c);
    }

    filtered = trim(std::move(filtered));
    if (filtered.empty()) return std::string();

    // Keep it short so inventory rows stay readable.
    if (filtered.size() > 28) filtered.resize(28);
    return filtered;
}

bool Game::setItemCallLabel(ItemKind k, const std::string& label) {
    if (!isIdentifiableKind(k)) return false;
    const size_t idx = static_cast<size_t>(k);
    if (idx >= static_cast<size_t>(ITEM_KIND_COUNT)) return false;

    const std::string v = sanitizeCallLabel(label);
    if (v.empty()) {
        return clearItemCallLabel(k);
    }

    if (identCall[idx] == v) return false;
    identCall[idx] = v;
    return true;
}

bool Game::clearItemCallLabel(ItemKind k) {
    if (!isIdentifiableKind(k)) return false;
    const size_t idx = static_cast<size_t>(k);
    if (idx >= static_cast<size_t>(ITEM_KIND_COUNT)) return false;
    if (identCall[idx].empty()) return false;
    identCall[idx].clear();
    return true;
}


void Game::newGame(uint32_t seed) {
    if (seed == 0) {
        // Fall back to a simple randomized seed if user passes 0.
        seed = hash32(static_cast<uint32_t>(std::rand()) ^ 0xA5A5F00Du);
    }

    rng = RNG(seed);
    seed_ = seed;
    // Start the run at the surface camp hub.
    branch_ = DungeonBranch::Camp;
    depth_ = 0;
    levels.clear();
    trapdoorFallers_.clear();
    overworldX_ = 0;
    overworldY_ = 0;
    overworldChunks_.clear();

    ents.clear();
    ground.clear();
    chestContainers_.clear();
    trapsCur.clear();
    mapMarkers_.clear();
    engravings_.clear();
    confusionGas_.clear();
    poisonGas_.clear();
    corrosiveGas_.clear();
    fireField_.clear();
    scentField_.clear();
    inv.clear();
    shopDebtLedger_.fill(0);
    merchantGuildAlerted_ = false;
    piety_ = 0;
    prayerCooldownUntilTurn_ = 0u;

    // Spell system (WIP)
    mana_ = 0;
    knownSpellsMask_ = 0u;
    fx.clear();
    fxExpl.clear();
    fxParticles_.clear();

    // Reset endgame escalation state.
    yendorDoomActive_ = false;
    yendorDoomLevel_ = 0;
    yendorDoomStartTurn_ = 0u;
    yendorDoomLastPulseTurn_ = 0u;
    yendorDoomLastSpawnTurn_ = 0u;
    yendorDoomMsgStage_ = 0;

    nextEntityId = 1;
    nextItemId = 1;
    equipMeleeId = 0;
    equipRangedId = 0;
    equipArmorId = 0;
    equipRing1Id = 0;
    equipRing2Id = 0;

    invOpen = false;
    invIdentifyMode = false;
    invEnchantRingMode = false;
    invPrompt_ = InvPromptKind::None;
    invSel = 0;
    targeting = false;

    spellsOpen = false;
    spellsSel = 0;
    targetLine.clear();
    targetValid = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    levelUpOpen = false;
    levelUpSel = 0;

    msgs.clear();
    msgScroll = 0;

    msgHistoryOpen = false;
    msgHistorySearchMode = false;
    msgHistoryFilter = MessageFilter::All;
    msgHistorySearch.clear();
    msgHistoryScroll = 0;

    scoresOpen = false;
    scoresView_ = ScoresView::Top;
    scoresSel = 0;

    codexOpen = false;
    codexFilter_ = CodexFilter::All;
    codexSort_ = CodexSort::Kind;
    codexSel = 0;

    // autoPickup is a user setting; do not reset it between runs.

    sneakMode_ = false;

    // Randomize identifiable item appearances and reset identification knowledge.
    initIdentificationTables();

    autoMode = AutoMoveMode::None;
    autoPathTiles.clear();
    autoPathIndex = 0;
    autoStepTimer = 0.0f;
    autoExploreGoalIsLoot = false;
    autoExploreGoalPos = Vec2i{-1, -1};
    autoExploreGoalIsSearch = false;
    autoExploreSearchGoalPos = Vec2i{-1, -1};
    autoExploreSearchTurnsLeft = 0;
    autoExploreSearchAnnounced = false;
    autoExploreSearchTriedTurns.clear();

    turnCount = 0;
    naturalRegenCounter = 0;
    lastAutosaveTurn = 0;

    killCount = 0;
    directKillCount_ = 0;
    conductFoodEaten_ = 0;
    conductCorpseEaten_ = 0;
    conductScrollsRead_ = 0;
    conductSpellbooksRead_ = 0;
    conductPrayers_ = 0;
    maxDepth = 0;
    codexSeen_.fill(0);
    codexKills_.fill(0);
    runRecorded = false;
    mortemWritten_ = false;
    bonesWritten_ = false;
    hastePhase = false;
    looking = false;
    lookPos = {0,0};

    inputLock = false;
    gameOver = false;
    gameWon = false;

    endCause_.clear();

    charLevel = 1;
    xp = 0;
    xpNext = 20;

    talentMight_ = 0;
    talentAgility_ = 0;
    talentVigor_ = 0;
    talentFocus_ = 0;
    talentPointsPending_ = 0;

    // Class starting bias (small passive nudge; most growth comes from level-ups).
    // This is applied before the first level is generated so it is part of the run identity.
    switch (playerClass_) {
        case PlayerClass::Knight: {
            talentMight_ = 1;
            talentVigor_ = 1;
        } break;
        case PlayerClass::Rogue: {
            talentAgility_ = 1;
            talentFocus_ = 1;
        } break;
        case PlayerClass::Archer: {
            talentAgility_ = 2;
        } break;
        case PlayerClass::Wizard: {
            talentFocus_ = 2;
        } break;
        case PlayerClass::Adventurer:
        default:
            break;
    }

    // Starting spell knowledge (WIP). Wizards begin with a few basics.
    knownSpellsMask_ = 0u;
    if (playerClass_ == PlayerClass::Wizard) {
        auto learn = [&](SpellKind k) {
            const uint32_t idx = static_cast<uint32_t>(k);
            if (idx < 32u) knownSpellsMask_ |= (1u << idx);
        };
        learn(SpellKind::MagicMissile);
        learn(SpellKind::Blink);
        learn(SpellKind::MinorHeal);
    }
    // Start full.
    mana_ = playerManaMax();

    // Hunger pacing (optional setting; stored per-run in save files).
    // The run is longer now, so slightly increase the default hunger budget
    // to keep the system from becoming an unintended hard timer.
    hungerMax = 800 + std::max(0, DUNGEON_MAX_DEPTH - 10) * 40;
    hunger = hungerMax;
    hungerStatePrev = hungerStateFor(hunger, hungerMax);

    // Level generation is deterministic per level identity and does not perturb the gameplay RNG stream.
    // This keeps the entire 1..25 dungeon stable given the run seed (important for previews).
    const uint32_t gameplayRngState = rng.state;
    rng.state = levelGenSeed(LevelId{branch_, depth_});

    const Vec2i msz = proceduralMapSizeFor(rng, branch_, depth_);
    dung = Dungeon(msz.x, msz.y);
    dung.generate(rng, branch_, depth_, DUNGEON_MAX_DEPTH, seed_);
    if (atHomeCamp()) {
        overworld::ensureBorderGates(dung);
    }
    // Environmental fields reset per floor (no lingering gas on a fresh level).
    confusionGas_.assign(static_cast<size_t>(dung.width * dung.height), 0u);
    poisonGas_.assign(static_cast<size_t>(dung.width * dung.height), 0u);
    fireField_.assign(static_cast<size_t>(dung.width * dung.height), 0u);
    scentField_.assign(static_cast<size_t>(dung.width * dung.height), 0u);

    // Auto-explore bookkeeping is transient per-floor; size it to this dungeon.
    autoExploreSearchTriedTurns.assign(static_cast<size_t>(dung.width * dung.height), 0u);

    // Shrines get a visible altar overlay tile (placed before graffiti/spawns so it stays clear).
    spawnAltars();

    // Flavor graffiti/engravings are generated once per freshly created floor.
    spawnGraffiti();

    // Create player
    Entity p;
    p.id = nextEntityId++;
    p.kind = EntityKind::Player;
    // Spawn in the surface camp hub (near the stash), otherwise default to the upstairs.
    Vec2i startPos = dung.stairsUp;
    if (atCamp()) {
        startPos = dung.campStashSpot;
        if (!dung.inBounds(startPos.x, startPos.y) || !dung.isWalkable(startPos.x, startPos.y)) {
            startPos = dung.stairsDown;
        }
        if (!dung.inBounds(startPos.x, startPos.y) || !dung.isWalkable(startPos.x, startPos.y)) {
            startPos = dung.stairsUp;
        }
    }
    p.pos = startPos;

    // Class baseline stats.
    // These are intentionally modest; items and talents still matter a lot.
    p.hpMax = 18;
    p.baseAtk = 3;
    p.baseDef = 0;

    switch (playerClass_) {
        case PlayerClass::Knight:
            p.hpMax = 22;
            p.baseAtk = 4;
            p.baseDef = 1;
            break;
        case PlayerClass::Rogue:
            p.hpMax = 16;
            p.baseAtk = 3;
            p.baseDef = 0;
            break;
        case PlayerClass::Archer:
            p.hpMax = 17;
            p.baseAtk = 3;
            p.baseDef = 0;
            break;
        case PlayerClass::Wizard:
            p.hpMax = 14;
            p.baseAtk = 2;
            p.baseDef = 0;
            break;
        case PlayerClass::Adventurer:
        default:
            break;
    }

    p.hp = p.hpMax;
    p.spriteSeed = rng.nextU32();
    playerId_ = p.id;

    ents.push_back(p);

    // Starting companion: a friendly dog.
    {
        Entity d;
        d.id = nextEntityId++;
        d.kind = EntityKind::Dog;
        d.hpMax = 10;
        d.hp = d.hpMax;
        d.baseAtk = 2;
        d.baseDef = 0;
        d.spriteSeed = rng.nextU32();
        d.speed = baseSpeedFor(d.kind);
        d.willFlee = false;
        d.alerted = false;
        d.friendly = true;
        d.allyOrder = AllyOrder::Follow;

        // Spawn near the player (prefer adjacent, but fall back to a wider search).
        Vec2i spawn{-1, -1};
        const int dirs8[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
        for (int i = 0; i < 8; ++i) {
            Vec2i c{p.pos.x + dirs8[i][0], p.pos.y + dirs8[i][1]};
            if (!dung.inBounds(c.x, c.y)) continue;
            if (!dung.isWalkable(c.x, c.y)) continue;
            if (entityAt(c.x, c.y)) continue;
            spawn = c;
            break;
        }
        if (spawn.x < 0) {
            // Extremely rare: player started boxed-in. Find the first free walkable tile.
            for (int y = 0; y < dung.height && spawn.x < 0; ++y) {
                for (int x = 0; x < dung.width && spawn.x < 0; ++x) {
                    if (!dung.isWalkable(x, y)) continue;
                    if (entityAt(x, y)) continue;
                    spawn = {x, y};
                }
            }
        }

        if (spawn.x >= 0) {
            d.pos = spawn;
            ents.push_back(d);
        }
    }

    // Starting gear
    auto give = [&](ItemKind k, int count = 1) {
        Item it;
        it.id = nextItemId++;
        it.kind = k;
        it.count = std::max(1, count);
        it.spriteSeed = rng.nextU32();
        const ItemDef& d = itemDef(k);
        if (d.maxCharges > 0) it.charges = d.maxCharges;

        inv.push_back(it);
        return it.id;
    };

    int startGold = 10;
    int meleeId = 0;
    int rangedId = 0;
    int armorId = 0;

    // Class-specific kit
    switch (playerClass_) {
        case PlayerClass::Knight: {
            meleeId = give(ItemKind::Sword, 1);
            rangedId = give(ItemKind::Sling, 1);
            give(ItemKind::Rock, 10);
            armorId = give(ItemKind::ChainArmor, 1);
            give(ItemKind::PotionHealing, 1);
            give(ItemKind::PotionStrength, 1);
            startGold = 8;
        } break;
        case PlayerClass::Rogue: {
            meleeId = give(ItemKind::Dagger, 1);
            rangedId = give(ItemKind::Sling, 1);
            give(ItemKind::Rock, 8);
            armorId = give(ItemKind::LeatherArmor, 1);
            give(ItemKind::Lockpick, 2);
            give(ItemKind::PotionHealing, 1);
            give(ItemKind::PotionInvisibility, 1);
            give(ItemKind::ScrollDetectTraps, 1);
            startGold = 15;
        } break;
        case PlayerClass::Archer: {
            rangedId = give(ItemKind::Bow, 1);
            give(ItemKind::Arrow, 24);
            meleeId = give(ItemKind::Dagger, 1);
            armorId = give(ItemKind::LeatherArmor, 1);
            give(ItemKind::PotionHealing, 1);
            startGold = 12;
        } break;
        case PlayerClass::Wizard: {
            rangedId = give(ItemKind::WandSparks, 1);
            meleeId = give(ItemKind::Dagger, 1);
            armorId = give(ItemKind::LeatherArmor, 1);
            give(ItemKind::PotionHealing, 1);
            give(ItemKind::PotionClarity, 1);
            give(ItemKind::ScrollIdentify, 2);
            startGold = 6;
        } break;
        case PlayerClass::Adventurer:
        default: {
            rangedId = give(ItemKind::Bow, 1);
            give(ItemKind::Arrow, 14);
            meleeId = give(ItemKind::Dagger, 1);
            armorId = give(ItemKind::LeatherArmor, 1);
            give(ItemKind::PotionHealing, 2);
            startGold = 10;
        } break;
    }

    // Shared baseline resources (survivability + escape + scouting)
    // Slightly more food if hunger is enabled to account for the longer run.
    give(ItemKind::FoodRation, hungerEnabled_ ? 3 : 1);

    // If lighting/darkness is enabled, start with a couple torches so early dark floors are survivable.
    if (lightingEnabled_) {
        give(ItemKind::Torch, 2);
    }

    give(ItemKind::ScrollTeleport, 1);
    give(ItemKind::ScrollMapping, 1);
    give(ItemKind::Gold, startGold);

    // Equip both melee + ranged so bump-attacks and FIRE both work immediately.
    equipMeleeId = meleeId;
    equipRangedId = rangedId;
    equipArmorId = armorId;

    spawnMonsters();
    spawnItems();
    spawnTraps();

    if (atCamp()) {
        setupSurfaceCampInstallations();
    }

    tryApplyBones();

    spawnFountains();

    // Restore gameplay RNG after deterministic generation/spawns.
    rng.state = gameplayRngState;

    storeCurrentLevel();

    recomputeFov();


    // Encumbrance message throttling: establish initial burden state for this run.
    burdenPrev_ = burdenState();

    pushMsg("WELCOME TO PROCROGUE++.", MessageKind::System);
    pushMsg(std::string("CLASS: ") + playerClassDisplayName(), MessageKind::System);
    {
        std::ostringstream ms;
        ms << "LEVEL SIZE: " << dung.width << "x" << dung.height << ".";
        pushMsg(ms.str(), MessageKind::System);
    }
    pushMsg("A DOG TROTS AT YOUR HEELS.", MessageKind::System);
    {
        std::ostringstream ss;
        ss << "GOAL: FIND THE AMULET OF YENDOR (DEPTH " << QUEST_DEPTH
           << "), THEN RETURN TO THE EXIT (<) TO WIN.";
        pushMsg(ss.str(), MessageKind::System);
    }
    pushMsg("PRESS ? FOR HELP. I INVENTORY. F TARGET/FIRE. M MINIMAP. TAB STATS. F3 LOG. F4 CODEX. F12 SCREENSHOT.", MessageKind::System);
    if (controlPreset_ == ControlPreset::Nethack) {
        pushMsg("MOVE: HJKL + YUBN DIAGONALS (ALSO ARROWS/NUMPAD). TIP: S SEARCH. : LOOK. CTRL+D KICK. C CLOSE DOOR. SHIFT+C LOCK DOOR.", MessageKind::System);
    } else {
        pushMsg("MOVE: WASD/ARROWS/NUMPAD + Q/E/Z/C DIAGONALS. TIP: SHIFT+C SEARCH. T DISARM TRAPS. O AUTO-EXPLORE. P AUTO-PICKUP.", MessageKind::System);
    }
    pushMsg("SAVE: F5   LOAD: F9   LOAD AUTO: F10", MessageKind::System);
}

void Game::storeCurrentLevel() {
    LevelState st;
    st.branch = branch_;
    st.depth = depth_;
    st.dung = dung;
    st.ground = ground;
    st.traps = trapsCur;
    st.markers = mapMarkers_;
    st.engravings = engravings_;
    st.chestContainers = chestContainers_;
    st.confusionGas = confusionGas_;
    st.poisonGas = poisonGas_;
    st.corrosiveGas = corrosiveGas_;
    st.fireField = fireField_;
    st.scentField = scentField_;
    st.monsters.clear();
    for (const auto& e : ents) {
        if (e.id == playerId_) continue;
        st.monsters.push_back(e);
    }
    if (atCamp() && !atHomeCamp()) {
        overworldChunks_[OverworldKey{overworldX_, overworldY_}] = std::move(st);
    } else {
        levels[{branch_, depth_}] = std::move(st);
    }
}

bool Game::restoreLevel(LevelId id) {
    auto it = levels.find(id);
    if (it == levels.end()) return false;

    dung = it->second.dung;
    ground = it->second.ground;
    trapsCur = it->second.traps;
    mapMarkers_ = it->second.markers;
    engravings_ = it->second.engravings;
    chestContainers_ = it->second.chestContainers;

    // Drop any orphaned containers (e.g., chests that were destroyed).
    if (!chestContainers_.empty()) {
        chestContainers_.erase(std::remove_if(chestContainers_.begin(), chestContainers_.end(), [&](const ChestContainer& c) {
            if (c.chestId <= 0) return true;
            for (const auto& gi : ground) {
                if (isChestKind(gi.item.kind) && gi.item.id == c.chestId) return false;
            }
            return true;
        }), chestContainers_.end());
    }

    confusionGas_ = it->second.confusionGas;
    const size_t expect = static_cast<size_t>(dung.width * dung.height);
    if (confusionGas_.size() != expect) confusionGas_.assign(expect, 0u);

    poisonGas_ = it->second.poisonGas;
    if (poisonGas_.size() != expect) poisonGas_.assign(expect, 0u);

    corrosiveGas_ = it->second.corrosiveGas;
    if (corrosiveGas_.size() != expect) corrosiveGas_.assign(expect, 0u);

    fireField_ = it->second.fireField;
    if (fireField_.size() != expect) fireField_.assign(expect, 0u);

    scentField_ = it->second.scentField;
    if (scentField_.size() != expect) scentField_.assign(expect, 0u);

    // Keep player, restore monsters.
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return e.id != playerId_;
    }), ents.end());

    for (const auto& m : it->second.monsters) {
        ents.push_back(m);
    }

    if (id.branch == DungeonBranch::Camp && id.depth == 0) {
        overworld::ensureBorderGates(dung);
    }

    return true;
}



bool Game::restoreOverworldChunk(int x, int y) {
    OverworldKey key{x, y};
    auto it = overworldChunks_.find(key);
    if (it == overworldChunks_.end()) return false;

    dung = it->second.dung;
    ground = it->second.ground;
    trapsCur = it->second.traps;
    mapMarkers_ = it->second.markers;
    engravings_ = it->second.engravings;
    chestContainers_ = it->second.chestContainers;

    // Drop any orphaned containers (e.g., chests that were destroyed).
    if (!chestContainers_.empty()) {
        chestContainers_.erase(std::remove_if(chestContainers_.begin(), chestContainers_.end(), [&](const ChestContainer& c) {
            if (c.chestId <= 0) return true;
            for (const auto& gi : ground) {
                if (isChestKind(gi.item.kind) && gi.item.id == c.chestId) return false;
            }
            return true;
        }), chestContainers_.end());
    }

    confusionGas_ = it->second.confusionGas;
    const size_t expect = static_cast<size_t>(dung.width * dung.height);
    if (confusionGas_.size() != expect) confusionGas_.assign(expect, 0u);

    poisonGas_ = it->second.poisonGas;
    if (poisonGas_.size() != expect) poisonGas_.assign(expect, 0u);

    corrosiveGas_ = it->second.corrosiveGas;
    if (corrosiveGas_.size() != expect) corrosiveGas_.assign(expect, 0u);

    fireField_ = it->second.fireField;
    if (fireField_.size() != expect) fireField_.assign(expect, 0u);

    scentField_ = it->second.scentField;
    if (scentField_.size() != expect) scentField_.assign(expect, 0u);

    // Keep player, restore monsters.
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return e.id != playerId_;
    }), ents.end());

    for (const auto& m : it->second.monsters) {
        ents.push_back(m);
    }

    // Overworld chunks always have edge gates.
    overworld::ensureBorderGates(dung);

    return true;
}

void Game::pruneOverworldChunks() {
    // Keep a bounded window around the current chunk to avoid unbounded memory growth.
    // (This is intentionally larger than the main-dungeon infinite keep window: overworld
    // travel is mostly lateral and players tend to backtrack.)
    constexpr int kKeepRadius = 6;

    for (auto it = overworldChunks_.begin(); it != overworldChunks_.end(); ) {
        const int dx = it->first.x - overworldX_;
        const int dy = it->first.y - overworldY_;
        if (std::abs(dx) > kKeepRadius || std::abs(dy) > kKeepRadius) {
            it = overworldChunks_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Game::tryOverworldStep(int dx, int dy) {
    if (!atCamp()) return false;
    if ((dx == 0 && dy == 0) || (dx != 0 && dy != 0)) return false;
    if (dx < -1 || dx > 1 || dy < -1 || dy > 1) return false;

    Entity& p = playerMut();

    // Only allow travel from edge tiles.
    if (dx == -1 && p.pos.x != 0) return false;
    if (dx ==  1 && p.pos.x != dung.width - 1) return false;
    if (dy == -1 && p.pos.y != 0) return false;
    if (dy ==  1 && p.pos.y != dung.height - 1) return false;

    // The edge tile must be walkable (i.e., an opened gate).
    if (!dung.isWalkable(p.pos.x, p.pos.y)) return false;

    cancelAutoMove(true);

    // Carry friendly companions that are set to follow/fetch.
    std::vector<Entity> companions;
    companions.reserve(4);
    for (auto it = ents.begin(); it != ents.end(); ) {
        if (it->id == playerId_) { ++it; continue; }
        if (it->hp <= 0) { ++it; continue; }
        if (!it->friendly) { ++it; continue; }
        if (!(it->allyOrder == AllyOrder::Follow || it->allyOrder == AllyOrder::Fetch)) { ++it; continue; }
        companions.push_back(*it);
        it = ents.erase(it);
    }

    // Store the current chunk (without traveling companions).
    storeCurrentLevel();

    const int oldX = overworldX_;
    const int oldY = overworldY_;

    overworldX_ += dx;
    overworldY_ += dy;

    // Close transient effects; treat travel like a level transition.
    fx.clear();
    fxExpl.clear();
    fxParticles_.clear();
    inputLock = false;
    msgScroll = 0;

    // Restore destination chunk, or generate if first visit.
    bool restored = false;
    if (atHomeCamp()) {
        restored = restoreLevel(LevelId{DungeonBranch::Camp, 0});
    } else {
        restored = restoreOverworldChunk(overworldX_, overworldY_);
    }

    auto computeArrival = [&]() -> Vec2i {
        // You exit one side, enter the opposite side in the destination chunk.
        const int w = dung.width;
        const int h = dung.height;
        if (dx ==  1) return Vec2i{0,      h / 2};
        if (dx == -1) return Vec2i{w - 1,  h / 2};
        if (dy ==  1) return Vec2i{w / 2,  0};
        return Vec2i{w / 2,  h - 1};
    };

    // Helper: check for a free tile (optionally ignoring the player).
    auto isFreeTile = [&](int x, int y, bool ignorePlayer) -> bool {
        if (!dung.inBounds(x, y)) return false;
        if (!dung.isWalkable(x, y)) return false;

        const Entity* e = entityAt(x, y);
        if (!e) return true;
        if (ignorePlayer && e->id == playerId_) return true;
        return false;
    };

    auto findNearbyFreeTile = [&](Vec2i center, int maxRadius, bool ignorePlayer) -> Vec2i {
        if (isFreeTile(center.x, center.y, ignorePlayer)) return center;

        for (int r = 1; r <= maxRadius; ++r) {
            for (int ddy = -r; ddy <= r; ++ddy) {
                for (int ddx = -r; ddx <= r; ++ddx) {
                    if (std::abs(ddx) != r && std::abs(ddy) != r) continue; // ring
                    Vec2i cand{center.x + ddx, center.y + ddy};
                    if (isFreeTile(cand.x, cand.y, ignorePlayer)) return cand;
                }
            }
        }

        return center;
    };

    if (!restored) {
        // New chunk: generate + populate deterministically without perturbing gameplay RNG.
        ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
            return e.id != playerId_;
        }), ents.end());
        ground.clear();
        trapsCur.clear();
        mapMarkers_.clear();
        engravings_.clear();
        chestContainers_.clear();
        confusionGas_.clear();
        poisonGas_.clear();
        corrosiveGas_.clear();
        fireField_.clear();
        scentField_.clear();

        const uint32_t gameplayRngState = rng.state;
        rng.state = overworld::chunkSeed(seed_, overworldX_, overworldY_);

        // Keep chunk size stable (use whatever size the current surface uses).
        const int w = std::max(16, dung.width);
        const int h = std::max(16, dung.height);
        dung = Dungeon(w, h);
        overworld::generateWildernessChunk(dung, seed_, overworldX_, overworldY_);
        dung.ensureMaterials(seed_, branch_, depth_, dungeonMaxDepth());

        confusionGas_.assign(static_cast<size_t>(dung.width * dung.height), 0u);
        poisonGas_.assign(static_cast<size_t>(dung.width * dung.height), 0u);
        corrosiveGas_.assign(static_cast<size_t>(dung.width * dung.height), 0u);
        fireField_.assign(static_cast<size_t>(dung.width * dung.height), 0u);
        scentField_.assign(static_cast<size_t>(dung.width * dung.height), 0u);

        // Place player before spawning so we never spawn on top of them.
        const Vec2i arrival = computeArrival();
        p.pos = arrival;
        p.alerted = false;

        spawnGraffiti();
        spawnMonsters();
        spawnItems();
        spawnTraps();
        spawnFountains();

        // Restore gameplay RNG after deterministic generation/spawns.
        rng.state = gameplayRngState;
    }

    // Ensure gates in destination chunk (home camp or wilderness).
    if (atCamp()) {
        overworld::ensureBorderGates(dung);
    }

    // Place player at the arrival gate; if blocked, step aside.
    {
        const Vec2i arrival = computeArrival();
        const Entity* blocker = entityAt(arrival.x, arrival.y);
        if (blocker && blocker->id != playerId_) {
            p.pos = findNearbyFreeTile(arrival, 6, true);
            pushMsg("THE PASSAGE IS BLOCKED! YOU STUMBLE ASIDE.", MessageKind::Warning, true);
        } else {
            p.pos = arrival;
        }
        p.alerted = false;
    }

    // Place companions near the player on the destination chunk.
    size_t companionCount = 0;
    for (auto& c : companions) {
        c.alerted = false;
        c.lastKnownPlayerPos = {-1, -1};
        c.lastKnownPlayerAge = 9999;
        c.energy = 0;

        Vec2i spawn = findNearbyFreeTile(p.pos, 4, false);
        if (!dung.inBounds(spawn.x, spawn.y)) continue;
        const Entity* b = entityAt(spawn.x, spawn.y);
        if (b && b->id != playerId_) continue;
        if (!dung.isWalkable(spawn.x, spawn.y)) continue;

        c.pos = spawn;
        ents.push_back(c);
        ++companionCount;
    }
    if (companionCount > 0) {
        if (companionCount == 1 && companions.size() == 1 && companions[0].kind == EntityKind::Dog) {
            pushMsg("YOUR DOG FOLLOWS YOU.", MessageKind::System, true);
        } else {
            pushMsg("YOUR COMPANIONS FOLLOW YOU.", MessageKind::System, true);
        }
    }

    // Auto-explore bookkeeping is transient per-floor; size it to the current dungeon.
    autoExploreSearchTriedTurns.assign(static_cast<size_t>(dung.width * dung.height), 0u);

    recomputeFov();

    // Prune the overworld cache around the new position.
    pruneOverworldChunks();

    // Flavor messages.
    if (overworldX_ == 0 && overworldY_ == 0) {
        pushMsg("YOU RETURN TO CAMP.", MessageKind::System, true);
    } else if (oldX == 0 && oldY == 0) {
        std::ostringstream ss;
        ss << "YOU STEP INTO THE WILDERNESS (" << overworldX_ << "," << overworldY_ << ").";
        pushMsg(ss.str(), MessageKind::System, true);
    } else {
        std::ostringstream ss;
        ss << "YOU TRAVEL TO (" << overworldX_ << "," << overworldY_ << ").";
        pushMsg(ss.str(), MessageKind::System, true);
    }

    return true;
}
uint32_t Game::levelGenSeed(LevelId id) const {
    // Stable per-level seed derived from the run seed + level identity.
    // Domain separation constant ensures future seed uses don't accidentally correlate.
    uint32_t s = seed_;
    s = hashCombine(s, static_cast<uint32_t>(static_cast<uint8_t>(id.branch)));
    s = hashCombine(s, static_cast<uint32_t>(id.depth));
    s = hashCombine(s, 0xE11D5EEDu);
    if (s == 0u) s = 1u;
    return s;
}

void Game::ensureEndlessSanctumDownstairs(LevelId id, Dungeon& d, RNG& rngForPlacement) const {
    if (!infiniteWorldEnabled_) return;
    if (id.branch != DungeonBranch::Main) return;
    if (id.depth != DUNGEON_MAX_DEPTH) return;

    // If a downstairs already exists, don't disturb it.
    if (d.inBounds(d.stairsDown.x, d.stairsDown.y)) {
        if (d.at(d.stairsDown.x, d.stairsDown.y).type == TileType::StairsDown) return;
    }

    // Pick a floor tile reasonably far from the upstairs so the sanctum doesn't become a choke.
    const Vec2i up = d.stairsUp;
    Vec2i best{-1, -1};
    int bestDist = -1;

    for (int tries = 0; tries < 400; ++tries) {
        const Vec2i cand = d.randomFloor(rngForPlacement, true);
        if (!d.inBounds(cand.x, cand.y)) continue;
        if (cand == up) continue;
        if (!d.isWalkable(cand.x, cand.y)) continue;

        const int md = std::abs(cand.x - up.x) + std::abs(cand.y - up.y);
        if (md > bestDist) {
            bestDist = md;
            best = cand;
        }
    }

    if (!d.inBounds(best.x, best.y)) {
        // Deterministic-ish fallback: first walkable tile that's not the upstairs.
        for (int y = 1; y < d.height - 1; ++y) {
            for (int x = 1; x < d.width - 1; ++x) {
                if (x == up.x && y == up.y) continue;
                if (!d.isWalkable(x, y)) continue;
                best = {x, y};
                break;
            }
            if (d.inBounds(best.x, best.y)) break;
        }
    }

    if (d.inBounds(best.x, best.y)) {
        d.stairsDown = best;
        d.at(best.x, best.y).type = TileType::StairsDown;
    }
}

void Game::pruneEndlessLevels() {
    if (!infiniteWorldEnabled_) return;
    if (infiniteKeepWindow_ <= 0) return;

    // Only prune post-quest depths in the Main branch.
    const int lo = std::max(DUNGEON_MAX_DEPTH + 1, depth_ - infiniteKeepWindow_);
    const int hi = depth_ + infiniteKeepWindow_;

    for (auto it = levels.begin(); it != levels.end(); ) {
        const LevelId id = it->first;
        if (id.branch != DungeonBranch::Main) {
            ++it;
            continue;
        }
        if (id.depth <= DUNGEON_MAX_DEPTH) {
            ++it;
            continue;
        }

        if (id.depth < lo || id.depth > hi) {
            it = levels.erase(it);
        } else {
            ++it;
        }
    }
}

const Engraving* Game::engravingAt(Vec2i p) const {
    for (const auto& e : engravings_) {
        if (e.pos.x == p.x && e.pos.y == p.y) return &e;
    }
    return nullptr;
}

void Game::spawnGraffiti() {
    // Clear any leftover data in case callers forgot.
    engravings_.clear();

    // Keep graffiti sparse: it's a flavor accent, not a UI spam source.
    constexpr size_t kMaxGraffitiPerFloor = 8;

    auto addGraffiti = [&](Vec2i pos, const std::string& text) {
        if (!dung.inBounds(pos.x, pos.y)) return;
        if (!dung.isWalkable(pos.x, pos.y)) return;
        // Avoid placing graffiti on special interactable overlays (keeps them readable).
        const TileType tt = dung.at(pos.x, pos.y).type;
        if (tt == TileType::Altar || tt == TileType::Fountain) return;
        if (pos == dung.stairsUp || pos == dung.stairsDown) return; // don't clutter stairs
        if (text.empty()) return;

        // Replace existing engraving on this tile (rare).
        for (auto& e : engravings_) {
            if (e.pos == pos) {
                e.text = text;
                e.strength = 255;
                e.isWard = false;
                e.isGraffiti = true;
                return;
            }
        }

        if (engravings_.size() >= kMaxGraffitiPerFloor) return;

        Engraving e;
        e.pos = pos;
        e.text = text;
        e.strength = 255;   // permanent flavor
        e.isWard = false;   // generated graffiti is never a ward (even if it looks like one)
        e.isGraffiti = true;
        engravings_.push_back(std::move(e));
    };

    auto addSigil = [&](Vec2i pos, const std::string& keyword, uint8_t uses) {
        // Sigils are limited-use magical graffiti that trigger when stepped on.
        // Keep them rare and avoid stair tiles for readability.
        if (!dung.inBounds(pos.x, pos.y)) return;
        if (!dung.isWalkable(pos.x, pos.y)) return;
        // Avoid placing sigils on special interactable overlays (keeps them readable).
        const TileType tt = dung.at(pos.x, pos.y).type;
        if (tt == TileType::Altar || tt == TileType::Fountain) return;
        if (pos == dung.stairsUp || pos == dung.stairsDown) return;
        if (keyword.empty()) return;
        if (uses == 0u) uses = 1u;
        if (uses == 255u) uses = 1u;

        std::string text = std::string("SIGIL: ") + keyword;

        // Replace existing engraving on this tile (rare).
        for (auto& e : engravings_) {
            if (e.pos == pos) {
                e.text = text;
                e.strength = uses;
                e.isWard = false;
                e.isGraffiti = true;
                return;
            }
        }

        if (engravings_.size() >= kMaxGraffitiPerFloor) return;

        Engraving e;
        e.pos = pos;
        e.text = std::move(text);
        e.strength = uses;
        e.isWard = false;
        e.isGraffiti = true;
        engravings_.push_back(std::move(e));
    };

    auto pickMsg = [&](const std::vector<std::string>& msgs) -> std::string {
        if (msgs.empty()) return std::string();
        const int idx = rng.range(0, static_cast<int>(msgs.size()) - 1);
        return msgs[static_cast<size_t>(idx)];
    };

    auto pickFloorInRoom = [&](const Room& r) -> Vec2i {
        // Prefer interior tiles (avoid walls).
        const int x0 = r.x + 1;
        const int y0 = r.y + 1;
        const int x1 = r.x + std::max(1, r.w - 2);
        const int y1 = r.y + std::max(1, r.h - 2);

        for (int tries = 0; tries < 40; ++tries) {
            Vec2i p{rng.range(x0, x1), rng.range(y0, y1)};
            if (!dung.inBounds(p.x, p.y)) continue;
            if (!dung.isWalkable(p.x, p.y)) continue;
            // Keep shrine overlays readable (altars/fountains should not be graffiti targets).
            const TileType tt = dung.at(p.x, p.y).type;
            if (tt == TileType::Altar || tt == TileType::Fountain) continue;
            if (p == dung.stairsUp || p == dung.stairsDown) continue;
            return p;
        }
        // Fall back to the room center.
        return Vec2i{r.cx(), r.cy()};
    };

    // --- Message pools ---
    static const std::vector<std::string> kGeneric = {
        "DON'T PANIC.",
        "KICKING DOORS HURTS.",
        "THE WALLS HAVE EARS.",
        "THE DEAD CAN SMELL YOU.",
        "TRUST YOUR NOSE.",
        "WORDS CAN BE WEAPONS.",
        "YOU ARE NOT THE FIRST.",
        "BONES DON'T LIE.",
        "GREED GETS YOU KILLED.",
        "THE FLOOR REMEMBERS.",
        "SOME WORDS SCARE BEASTS.",
        "WRITE IN THE DUST.",
    };

    static const std::vector<std::string> kShrine = {
        "LEAVE AN OFFERING.",
        "PRAY WITH CLEAN HANDS.",
        "THE GODS DO NOT FORGET.",
    };

    static const std::vector<std::string> kLibrary = {
        "SILENCE, PLEASE.",
        "WORDS CUT DEEPER.",
        "READ CAREFULLY.",
    };

    static const std::vector<std::string> kLaboratory = {
        "DO NOT MIX POTIONS.",
        "EYE PROTECTION ADVISED.",
        "IF IT BUBBLES, RUN.",
    };

    static const std::vector<std::string> kArmory = {
        "POINTY END OUT.",
        "COUNT YOUR ARROWS.",
        "BLADES RUST, SKILLS DON'T.",
    };

    static const std::vector<std::string> kVault = {
        "LOCKS LIE.",
        "TREASURE BITES.",
        "NOT WORTH IT.",
    };

    static const std::vector<std::string> kSecret = {
        "SHHH.",
        "YOU FOUND IT.",
        "LOOK BEHIND THE LOOK.",
    };

    // Rare runic sigils (glyph-like floor inscriptions) that have small, local effects.
    // These are deliberately terse so they read well in LOOK mode.
    static const std::vector<std::string> kSigilAny = {
        "SEER",
        "NEXUS",
        "MIASMA",
        "EMBER",
    };

    // Camp-specific: the surface is calmer, so the scribbles are more practical.
    if (atCamp()) {
        if (dung.inBounds(dung.campStashSpot.x, dung.campStashSpot.y)) {
            addGraffiti(dung.campStashSpot, "STASH");
        }
        addGraffiti(dung.stairsDown, "DUNGEON");
        // Keep exit uncluttered: the upstairs tile is the real win condition.
        return;
    }

    // Special rooms get a higher chance of graffiti.
    for (const auto& r : dung.rooms) {
        if (engravings_.size() >= kMaxGraffitiPerFloor) break;

        switch (r.type) {
            case RoomType::Shrine: {
                const Vec2i p = pickFloorInRoom(r);
                if (rng.chance(0.18f)) {
                    addSigil(p, "SEER", 1);
                } else if (rng.chance(0.80f)) {
                    addGraffiti(p, pickMsg(kShrine));
                }
            } break;
            case RoomType::Library: {
                const Vec2i p = pickFloorInRoom(r);
                if (rng.chance(0.12f)) {
                    addSigil(p, "SEER", 1);
                } else if (rng.chance(0.70f)) {
                    addGraffiti(p, pickMsg(kLibrary));
                }
            } break;
            case RoomType::Laboratory: {
                const Vec2i p = pickFloorInRoom(r);
                if (rng.chance(0.22f)) {
                    // Labs are where alchemical accidents happen.
                    addSigil(p, rng.chance(0.50f) ? "MIASMA" : "EMBER", 2);
                } else if (rng.chance(0.70f)) {
                    addGraffiti(p, pickMsg(kLaboratory));
                }
            } break;
            case RoomType::Armory: {
                const Vec2i p = pickFloorInRoom(r);
                if (rng.chance(0.15f)) {
                    addSigil(p, "EMBER", 1);
                } else if (rng.chance(0.60f)) {
                    addGraffiti(p, pickMsg(kArmory));
                }
            } break;
            case RoomType::Vault: {
                const Vec2i p = pickFloorInRoom(r);
                if (rng.chance(0.18f)) {
                    addSigil(p, "NEXUS", 1);
                } else if (rng.chance(0.85f)) {
                    addGraffiti(p, pickMsg(kVault));
                }
            } break;
            case RoomType::Secret: {
                const Vec2i p = pickFloorInRoom(r);
                if (rng.chance(0.30f)) {
                    addSigil(p, "NEXUS", 1);
                } else if (rng.chance(0.90f)) {
                    addGraffiti(p, pickMsg(kSecret));
                }
            } break;
            default:
                break;
        }
    }

    // A few generic scribbles across the floor.
    const int extra = rng.chance(0.35f) ? rng.range(1, 3) : rng.range(0, 1);
    for (int i = 0; i < extra && engravings_.size() < kMaxGraffitiPerFloor; ++i) {
        if (dung.rooms.empty()) break;
        const auto& r = dung.rooms[static_cast<size_t>(rng.range(0, static_cast<int>(dung.rooms.size()) - 1))];
        const Vec2i p = pickFloorInRoom(r);
        if (rng.chance(0.08f) && depth_ >= 2) {
            std::string sk = pickMsg(kSigilAny);
            uint8_t uses = (sk == "MIASMA" || sk == "EMBER") ? 2 : 1;
            addSigil(p, sk, uses);
        } else {
            addGraffiti(p, pickMsg(kGeneric));
        }
    }
}

void Game::setupSurfaceCampInstallations() {
    // Surface camp installations are only valid in the Camp hub (depth 0).
    // Keep this branch-aware so future branches can safely use depth 0.
    if (!atCamp()) return;

    // Stash anchor comes from dungeon generation; fall back to the downstairs if needed.
    Vec2i stash = dung.campStashSpot;
    if (!dung.inBounds(stash.x, stash.y) || !dung.isWalkable(stash.x, stash.y)) {
        stash = dung.stairsDown;
    }

    // If a chest already exists at the stash location, don't duplicate.
    for (const auto& gi : ground) {
        if (gi.pos == stash && isChestKind(gi.item.kind)) {
            // Still add helpful markers if missing.
            bool haveStashMarker = false;
            for (const auto& m : mapMarkers_) {
                if (m.pos == stash && m.label == "STASH") {
                    haveStashMarker = true;
                    break;
                }
            }
            if (!haveStashMarker) mapMarkers_.push_back({stash, MarkerKind::Loot, "STASH"});
            return;
        }
    }

    // Place a persistent, already-open chest (so it behaves like storage rather than a loot piñata).
    const int chestId = nextItemId;
    Item chest;
    chest.kind = ItemKind::ChestOpen;
    chest.charges = CHEST_FLAG_OPENED;
    chest.enchant = 2; // bigger stack limit than a basic chest
    chest.spriteSeed = rng.nextU32();
    dropGroundItemItem(stash, chest);

    // Ensure a container exists for it (and pre-stock a couple of basic supplies).
    bool haveContainer = false;
    for (const auto& c : chestContainers_) {
        if (c.chestId == chestId) {
            haveContainer = true;
            break;
        }
    }

    if (!haveContainer) {
        ChestContainer cc;
        cc.chestId = chestId;

        // A tiny emergency kit (one-time, since the chest persists).
        Item torch;
        torch.id = nextItemId++;
        torch.kind = ItemKind::Torch;
        torch.count = 2;
        torch.spriteSeed = rng.nextU32();
        torch.charges = itemDef(torch.kind).maxCharges;

        Item ration;
        ration.id = nextItemId++;
        ration.kind = ItemKind::FoodRation;
        ration.count = 1;
        ration.spriteSeed = rng.nextU32();

        cc.items.push_back(torch);
        cc.items.push_back(ration);

        chestContainers_.push_back(cc);
    }

    // Add a few default map markers for convenience.
    auto addMarker = [&](Vec2i pos, MarkerKind kind, const std::string& label) {
        if (!dung.inBounds(pos.x, pos.y)) return;
        for (const auto& m : mapMarkers_) {
            if (m.pos == pos && m.label == label) return;
        }
        mapMarkers_.push_back({pos, kind, label});
    };

    addMarker(stash, MarkerKind::Loot, "STASH");
    addMarker(dung.stairsDown, MarkerKind::Note, "DUNGEON");
    addMarker(dung.stairsUp, MarkerKind::Note, "EXIT");

    // Optional: mark camp landmarks placed by the surface-camp generator.
    if (dung.inBounds(dung.campSideGateIn.x, dung.campSideGateIn.y)) {
        addMarker(dung.campSideGateIn, MarkerKind::Note, "SIDEGATE");
    }
    if (dung.inBounds(dung.campFireSpot.x, dung.campFireSpot.y)) {
        addMarker(dung.campFireSpot, MarkerKind::Note, "FIRE");
    }
    if (dung.inBounds(dung.campAltarSpot.x, dung.campAltarSpot.y)) {
        addMarker(dung.campAltarSpot, MarkerKind::Note, "ALTAR");
    }

    // Optional: mark any camp well placed by the generator (TileType::Fountain).
    // This is useful because camp fountains are *not* spawned by the standard dungeon fountain pass.
    Vec2i well = dung.campWellSpot;
    if (!dung.inBounds(well.x, well.y)) {
        // Fallback: scan for the closest fountain (older saves / legacy generators).
        well = {-1, -1};
        int best = 1 << 30;
        for (int y = 0; y < dung.height; ++y) {
            for (int x = 0; x < dung.width; ++x) {
                if (dung.at(x, y).type != TileType::Fountain) continue;
                const int sc = manhattan({x, y}, stash);
                if (sc < best) {
                    best = sc;
                    well = {x, y};
                }
            }
        }
    }
    if (dung.inBounds(well.x, well.y)) {
        addMarker(well, MarkerKind::Note, "WELL");
    }

}


void Game::changeLevel(int newDepth, bool goingDown) {
    // Convenience overload: change depth *within the current branch*.
    //
    // Branch transitions (Camp <-> Main, etc.) should use the LevelId overload so
    // each branch can have its own independent depth numbering in the future.
    changeLevel(LevelId{branch_, newDepth}, goingDown);
}

void Game::changeLevel(LevelId newLevel, bool goingDown) {
    const int newDepth = newLevel.depth;
    if (newDepth < 0) return;

    const bool endlessOk = infiniteWorldEnabled_ && newLevel.branch == DungeonBranch::Main;
    if (newDepth > DUNGEON_MAX_DEPTH && !endlessOk) {
        pushMsg("THE STAIRS END HERE. YOU SENSE YOU ARE AT THE BOTTOM.", MessageKind::System, true);
        return;
    }

    // If we're leaving a shop while still owing money, the shopkeeper becomes hostile.
    if (playerInShop()) {
        const int debt = shopDebtThisDepth();
        if (debt > 0 && anyPeacefulShopkeeper(ents, playerId_)) {
            triggerShopTheftAlarm(player().pos, player().pos);
        }
    }

    // --- Stair travel now has teeth ---
    // Using stairs is noisy, and monsters adjacent to the stairs may follow you between floors.
    // This makes "stair-dancing" less of a guaranteed reset without making stairs unusable.

    const Vec2i leavePos = player().pos;

    // Alert nearby monsters before selecting followers (so "unaware" creatures standing next to the
    // stairs can still react to you clomping up/down).
    emitNoise(leavePos, 12);

    auto canMonsterUseStairs = [&](const Entity& m) -> bool {
        if (m.hp <= 0) return false;
        if (m.id == playerId_) return false;

        // Shopkeepers don't follow you between levels (keeps shops stable and prevents leaving the
        // shop floor from turning into a cross-dungeon chase).
        if (m.kind == EntityKind::Shopkeeper) return false;

        // Webbed monsters are physically stuck.
        if (m.effects.webTurns > 0) return false;

        // Everything else can traverse stairs for now.
        return true;
    };

    // Carry companions across floors (NetHack-style): they always follow the player.
    int leftBehindCompanions = 0;
    std::vector<Entity> companions;
    companions.reserve(8);
    for (size_t i = 0; i < ents.size(); ) {
        if (ents[i].friendly && ents[i].hp > 0 && (ents[i].allyOrder == AllyOrder::Follow || ents[i].allyOrder == AllyOrder::Fetch)) {
            companions.push_back(ents[i]);
            ents.erase(ents.begin() + static_cast<std::vector<Entity>::difference_type>(i));
            continue;
        }
        if (ents[i].friendly && ents[i].hp > 0) {
            // STAY/GUARD companions remain on this floor.
            leftBehindCompanions += 1;
        }
        ++i;
    }

    if (leftBehindCompanions > 0) {
        pushMsg("YOU LEAVE A COMPANION BEHIND.", MessageKind::System, true);
    }

        // Collect eligible followers (adjacent to the stairs tile the player is using).
    std::vector<size_t> followerIdx;
    followerIdx.reserve(8);

    for (size_t i = 0; i < ents.size(); ++i) {
        const Entity& m = ents[i];
        if (!canMonsterUseStairs(m)) continue;
        if (chebyshev(m.pos, leavePos) > 1) continue;
        if (!m.alerted) continue;
        // Keep the surface camp as a safe-ish hub: hostile monsters don't follow you up.
        if (newLevel.branch == DungeonBranch::Camp && !m.friendly) continue;
        followerIdx.push_back(i);
    }

    // Shuffle follower order for variety, then cap (prevents pathological surround-deaths).
    for (int i = static_cast<int>(followerIdx.size()) - 1; i > 0; --i) {
        const int j = rng.range(0, i);
        std::swap(followerIdx[static_cast<size_t>(i)], followerIdx[static_cast<size_t>(j)]);
    }

    const size_t maxFollowers = 4;
    if (followerIdx.size() > maxFollowers) followerIdx.resize(maxFollowers);

    // Remove followers from the current level so the old level saves without them.
    // Erase from high -> low indices so offsets stay valid.
    std::sort(followerIdx.begin(), followerIdx.end(), [](size_t a, size_t b) { return a > b; });

    std::vector<Entity> followers;
    followers.reserve(followerIdx.size());

    for (size_t idx : followerIdx) {
        if (idx >= ents.size()) continue;
        followers.push_back(ents[idx]);
        ents.erase(ents.begin() + static_cast<std::vector<Entity>::difference_type>(idx));
    }

    storeCurrentLevel();

    // Clear transient states.
    fx.clear();
    fxExpl.clear();
    fxParticles_.clear();
    inputLock = false;

    autoMode = AutoMoveMode::None;
    autoPathTiles.clear();
    autoPathIndex = 0;
    autoStepTimer = 0.0f;
    autoExploreGoalIsLoot = false;
    autoExploreGoalPos = Vec2i{-1, -1};
    autoExploreGoalIsSearch = false;
    autoExploreSearchGoalPos = Vec2i{-1, -1};
    autoExploreSearchTurnsLeft = 0;
    autoExploreSearchAnnounced = false;
    autoExploreSearchTriedTurns.clear();
    invOpen = false;
    targeting = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;

    branch_ = newLevel.branch;
    depth_ = newDepth;

    if (branch_ == DungeonBranch::Camp && depth_ == 0) {
        // Surface hub is always chunk (0,0).
        overworldX_ = 0;
        overworldY_ = 0;
    }
    maxDepth = std::max(maxDepth, depth_);

    bool restored = restoreLevel(newLevel);

    Entity& p = playerMut();

    // Helper: find a nearby free, walkable tile (used for safe stair arrival + follower placement).
    auto isFreeTile = [&](int x, int y, bool ignorePlayer) -> bool {
        if (!dung.inBounds(x, y)) return false;
        if (!dung.isWalkable(x, y)) return false;

        const Entity* e = entityAt(x, y);
        if (!e) return true;
        if (ignorePlayer && e->id == playerId_) return true;
        return false;
    };

    auto findNearbyFreeTile = [&](Vec2i center, int maxRadius, bool avoidStairs, bool ignorePlayer) -> Vec2i {
        // Search expanding rings (chebyshev distance).
        std::vector<Vec2i> candidates;

        auto isAvoided = [&](const Vec2i& v) -> bool {
            if (!avoidStairs) return false;
            if (v == dung.stairsUp) return true;
            if (v == dung.stairsDown) return true;
            return false;
        };

        for (int r = 1; r <= maxRadius; ++r) {
            candidates.clear();
            candidates.reserve(static_cast<size_t>(8 * r));

            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    const int d = std::max(std::abs(dx), std::abs(dy));
                    if (d != r) continue;
                    Vec2i v{center.x + dx, center.y + dy};
                    if (!dung.inBounds(v.x, v.y)) continue;
                    if (isAvoided(v)) continue;
                    candidates.push_back(v);
                }
            }

            // Randomize the ring for less predictable spawns.
            for (int i = static_cast<int>(candidates.size()) - 1; i > 0; --i) {
                const int j = rng.range(0, i);
                std::swap(candidates[static_cast<size_t>(i)], candidates[static_cast<size_t>(j)]);
            }

            for (const Vec2i& v : candidates) {
                if (isFreeTile(v.x, v.y, ignorePlayer)) return v;
            }
        }

        // Fallback: brute scan.
        for (int y = 0; y < dung.height; ++y) {
            for (int x = 0; x < dung.width; ++x) {
                Vec2i v{x, y};
                if (isAvoided(v)) continue;
                if (isFreeTile(x, y, ignorePlayer)) return v;
            }
        }

        return center; // Worst-case (shouldn't happen).
    };

    auto placeFollowersNear = [&](Vec2i anchor) -> size_t {
        size_t placed = 0;

        for (auto& m : followers) {
            // Followers arrive "ready": they know where you are and will act on their next turn.
            m.alerted = true;
            m.lastKnownPlayerPos = p.pos;
            m.lastKnownPlayerAge = 0;
            // Treat stair-travel as consuming their action budget.
            m.energy = 0;

            // Avoid spawning on stairs tiles to reduce accidental soft-locks.
            Vec2i spawn = findNearbyFreeTile(anchor, 6, true, false);
            if (!dung.inBounds(spawn.x, spawn.y) || entityAt(spawn.x, spawn.y) != nullptr || !dung.isWalkable(spawn.x, spawn.y)) {
                continue;
            }

            m.pos = spawn;
            ents.push_back(m);
            ++placed;
        }

        return placed;
    };

    auto placeTrapdoorFallersHere = [&]() -> size_t {
        // Spawn any monsters/companions that previously fell through trap doors into this level.
        const LevelId here{branch_, depth_};
        auto it = trapdoorFallers_.find(here);
        if (it == trapdoorFallers_.end()) return 0;
        auto& pending = it->second;
        if (pending.empty()) return 0;

        auto isSafeLanding = [&](Vec2i v) -> bool {
            if (!dung.inBounds(v.x, v.y)) return false;
            if (!dung.isWalkable(v.x, v.y)) return false;
            if (v == dung.stairsUp || v == dung.stairsDown) return false;
            if (entityAt(v.x, v.y) != nullptr) return false;
            if (fireAt(v.x, v.y) > 0u) return false;
            for (const auto& tr : trapsCur) {
                if (tr.pos == v) return false;
            }
            return true;
        };

        size_t placed = 0;
        std::vector<Entity> stillPending;
        stillPending.reserve(pending.size());

        for (auto& m : pending) {
            Vec2i dst{-1, -1};
            bool ok = false;
            for (int tries = 0; tries < 250; ++tries) {
                const Vec2i cand = dung.randomFloor(rng, true);
                if (!isSafeLanding(cand)) continue;
                dst = cand;
                ok = true;
                break;
            }

            if (!ok) {
                stillPending.push_back(m);
                continue;
            }

            m.pos = dst;
            // Landing costs the creature its action budget for the turn.
            m.energy = 0;

            // The fall is startling and loud: hostiles become alert, allies reset tracking.
            if (m.friendly) {
                m.alerted = false;
                m.lastKnownPlayerPos = {-1, -1};
                m.lastKnownPlayerAge = 9999;
            } else {
                m.alerted = true;
                m.lastKnownPlayerPos = p.pos;
                m.lastKnownPlayerAge = 0;
            }

            ents.push_back(m);
            ++placed;

            // Impact noise can wake nearby monsters.
            emitNoise(dst, 14);
        }

        pending = std::move(stillPending);
        if (pending.empty()) {
            trapdoorFallers_.erase(it);
        }
        return placed;
    };

    size_t followedCount = 0;

    if (!restored) {
        // New level: generate and populate.
        ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
            return e.id != playerId_;
        }), ents.end());
        ground.clear();
        trapsCur.clear();
        mapMarkers_.clear();
        engravings_.clear();
        chestContainers_.clear();
        confusionGas_.clear();
        poisonGas_.clear();
        corrosiveGas_.clear();
        fireField_.clear();
        scentField_.clear();

        // Level generation is deterministic per level identity and does not perturb the gameplay
        // RNG stream. This makes the 1..25 dungeon stable for previews (Augury) and supports
        // Infinite World regeneration when pruning old levels.
        const bool deterministicLevel = true;
        const uint32_t gameplayRngState = rng.state;
        if (deterministicLevel) {
            rng.state = levelGenSeed(newLevel);
        }

        const Vec2i msz = proceduralMapSizeFor(rng, branch_, depth_);
        dung = Dungeon(msz.x, msz.y);
        dung.generate(rng, branch_, depth_, DUNGEON_MAX_DEPTH, seed_);

        if (branch_ == DungeonBranch::Camp && depth_ == 0) {
            overworld::ensureBorderGates(dung);
        }

        // In infinite mode, the sanctum (depth == max) gains a downstairs so depth 26+ is reachable.
        ensureEndlessSanctumDownstairs(newLevel, dung, rng);
        dung.computeEndlessStratumInfo(seed_, branch_, depth_, DUNGEON_MAX_DEPTH);

        confusionGas_.assign(static_cast<size_t>(dung.width * dung.height), 0u);
        poisonGas_.assign(static_cast<size_t>(dung.width * dung.height), 0u);
        corrosiveGas_.assign(static_cast<size_t>(dung.width * dung.height), 0u);
        fireField_.assign(static_cast<size_t>(dung.width * dung.height), 0u);
        scentField_.assign(static_cast<size_t>(dung.width * dung.height), 0u);

        // Shrines get a visible altar overlay tile (placed before graffiti/spawns so it stays clear).
        spawnAltars();

        // Flavor graffiti/engravings are generated once per freshly created floor.
        spawnGraffiti();

        const Vec2i desiredArrival = goingDown ? dung.stairsUp : dung.stairsDown;

        // Place player before spawning so we never spawn on top of them.
        p.pos = desiredArrival;
        p.alerted = false;

        // Generate deterministic level content (monsters/items/traps/bones) before placing
        // dynamic arrivals (followers/companions/trapdoor fallers).
        spawnMonsters();
        spawnItems();
        spawnTraps();

        if (branch_ == DungeonBranch::Camp) {
            setupSurfaceCampInstallations();
        }

        tryApplyBones();

        spawnFountains();

        // Restore gameplay RNG after deterministic generation/spawns.
        if (deterministicLevel) {
            rng.state = gameplayRngState;
        }

        // Place stair followers after spawning so generation is deterministic in infinite mode.
        followedCount = placeFollowersNear(p.pos);

        // Place companions (allies) near the player on the new level.
        size_t companionCount = 0;
        for (auto& c : companions) {
            // Reset hostile-tracking state for allies.
            c.alerted = false;
            c.lastKnownPlayerPos = {-1, -1};
            c.lastKnownPlayerAge = 9999;
            // Treat stair-travel as consuming their action budget.
            c.energy = 0;

            Vec2i spawn = findNearbyFreeTile(p.pos, 4, true, false);
            if (!dung.inBounds(spawn.x, spawn.y) || entityAt(spawn.x, spawn.y) != nullptr || !dung.isWalkable(spawn.x, spawn.y)) {
                continue;
            }

            c.pos = spawn;
            ents.push_back(c);
            ++companionCount;
        }
        if (companionCount > 0) {
            if (companionCount == 1 && companions.size() == 1 && companions[0].kind == EntityKind::Dog) {
                pushMsg("YOUR DOG FOLLOWS YOU.", MessageKind::System, true);
            } else {
                pushMsg("YOUR COMPANIONS FOLLOW YOU.", MessageKind::System, true);
            }
        }

        placeTrapdoorFallersHere();

        // Stair arrival is noisy: nearby monsters may wake and investigate.
        emitNoise(p.pos, 12);

        // Save this freshly created level.
        storeCurrentLevel();
    } else {
        // Returning to a visited level.
        ensureEndlessSanctumDownstairs(newLevel, dung, rng);
        dung.computeEndlessStratumInfo(seed_, branch_, depth_, DUNGEON_MAX_DEPTH);

        const Vec2i desiredArrival = goingDown ? dung.stairsUp : dung.stairsDown;

        // If a monster is camping the stairs, don't overlap: step off to the side.
        const Entity* blocker = entityAt(desiredArrival.x, desiredArrival.y);
        if (blocker && blocker->id != playerId_) {
            p.pos = findNearbyFreeTile(desiredArrival, 6, true, true);
            pushMsg("THE STAIRS ARE BLOCKED! YOU STUMBLE ASIDE.", MessageKind::Warning, true);
        } else {
            p.pos = desiredArrival;
        }

        p.alerted = false;

        followedCount = placeFollowersNear(p.pos);

        // Place companions (allies) near the player on the new level.
        size_t companionCount = 0;
        for (auto& c : companions) {
            // Reset hostile-tracking state for allies.
            c.alerted = false;
            c.lastKnownPlayerPos = {-1, -1};
            c.lastKnownPlayerAge = 9999;
            // Treat stair-travel as consuming their action budget.
            c.energy = 0;

            Vec2i spawn = findNearbyFreeTile(p.pos, 4, true, false);
            if (!dung.inBounds(spawn.x, spawn.y) || entityAt(spawn.x, spawn.y) != nullptr || !dung.isWalkable(spawn.x, spawn.y)) {
                continue;
            }

            c.pos = spawn;
            ents.push_back(c);
            ++companionCount;
        }
        if (companionCount > 0) {
            if (companionCount == 1 && companions.size() == 1 && companions[0].kind == EntityKind::Dog) {
                pushMsg("YOUR DOG FOLLOWS YOU.", MessageKind::System, true);
            } else {
                pushMsg("YOUR COMPANIONS FOLLOW YOU.", MessageKind::System, true);
            }
        }

        placeTrapdoorFallersHere();

        // Stair arrival is noisy on visited floors too.
        emitNoise(p.pos, 12);
    }

    // Auto-explore bookkeeping is transient per-floor; size it to the current dungeon.
    autoExploreSearchTriedTurns.assign(static_cast<size_t>(dung.width * dung.height), 0u);

    // Small heal on travel.
    p.hp = std::min(p.hpMax, p.hp + 2);

    std::ostringstream ss;
    if (!goingDown && atCamp()) {
        ss << "YOU RETURN TO YOUR CAMP.";
    } else if (goingDown && branch_ == DungeonBranch::Main && depth_ == 1) {
        ss << "YOU DESCEND INTO THE DUNGEON (DEPTH 1).";
    } else if (goingDown) {
        ss << "YOU DESCEND TO DEPTH " << depth_ << ".";
    } else {
        ss << "YOU ASCEND TO DEPTH " << depth_ << ".";
    }
    pushMsg(ss.str());

    // Show the procedurally-chosen map dimensions once, when a floor is first generated.
    if (!restored) {
        std::ostringstream ms;
        ms << "LEVEL SIZE: " << dung.width << "x" << dung.height << ".";
        pushSystemMessage(ms.str());
    }

    // Environmental flavor: a per-level draft direction that biases drifting hazards (gas, fire).
    // Only announce it once, when a floor is first generated, to avoid spamming on revisits.
    if (!restored && !atCamp()) {
        const Vec2i w = windDir();
        const int ws = windStrength();
        if (ws > 0 && (w.x != 0 || w.y != 0)) {
            const char* dir =
                (w.x > 0) ? "EAST" :
                (w.x < 0) ? "WEST" :
                (w.y > 0) ? "SOUTH" : "NORTH";

            const char* mag =
                (ws == 1) ? "A LIGHT" :
                (ws == 2) ? "A STEADY" : "A STRONG";

            std::string msg = std::string(mag) + " DRAFT BLOWS TO THE " + dir + ".";
            pushSystemMessage(msg);
        }
    }


    if (followedCount > 0) {
        std::ostringstream ms;
        if (goingDown) {
            if (followedCount == 1) ms << "SOMETHING FOLLOWS YOU DOWN THE STAIRS...";
            else ms << followedCount << " CREATURES FOLLOW YOU DOWN THE STAIRS...";
        } else {
            if (followedCount == 1) ms << "SOMETHING FOLLOWS YOU UP THE STAIRS...";
            else ms << followedCount << " CREATURES FOLLOW YOU UP THE STAIRS...";
        }
        pushMsg(ms.str(), MessageKind::Warning, true);
    }

    // MERCHANT GUILD PURSUIT: if you fled a shop with unpaid goods, guards may pursue across floors.
    if (merchantGuildAlerted_ && shopDebtTotal() > 0 && !atCamp()) {
        if (rng.chance(0.65f)) {
            Vec2i gpos = findNearbyFreeTile(p.pos, 6, true, true);
            if (dung.inBounds(gpos.x, gpos.y) && dung.isWalkable(gpos.x, gpos.y) && !entityAt(gpos.x, gpos.y)) {
                Entity& g = spawnMonster(EntityKind::Guard, gpos, 0, /*allowGear=*/true);
                g.alerted = true;
                g.lastKnownPlayerPos = p.pos;
                g.lastKnownPlayerAge = 0;
                g.energy = 0;
                pushMsg("YOU HEAR BOOTS ECHOING IN THE DARK...", MessageKind::Warning, true);
            }
        }
    }

    recomputeFov();

    if (branch_ == DungeonBranch::Main) {

        if (goingDown && depth_ == MIDPOINT_DEPTH) {
            pushMsg("YOU HAVE REACHED THE MIDPOINT OF THE DUNGEON.", MessageKind::System, true);
        }

    
        // Grotto callout: the cavern-like floor features a subterranean lake carved out of chasms.
        if (goingDown && depth_ == Dungeon::GROTTO_DEPTH) {
            pushMsg("A DAMP CHILL RISES FROM BELOW...", MessageKind::System, true);
            if (dung.hasCavernLake) {
                pushMsg("THE GROTTO OPENS INTO A SUBTERRANEAN LAKE.", MessageKind::System, true);
            } else {
                pushMsg("THE ROCK GIVES WAY TO A NATURAL CAVERN.", MessageKind::System, true);
            }
        }

        // Cavern hint: metaballs caverns have a smoother, organic silhouette (implicit surface).
        if (goingDown && dung.cavernMetaballsUsed) {
            pushMsg("THE CAVERN WALLS CURVE AS IF MOLDED FROM LIQUID STONE.", MessageKind::System, true);
        }

        // Maze hint: Wilson's algorithm mazes (uniform spanning trees) tend to feel
        // uncannily 'even' compared to backtracker mazes.
        if (goingDown && dung.mazeAlgorithm == MazeAlgorithm::Wilson) {
            pushMsg("THE PASSAGES TWIST WITH AN UNCANNY, EVEN CHANCE.", MessageKind::System, true);
        }

        // Special floor callout: the procedural mines floors are about winding tunnels,
        // loops, and small chambers (less "architected" than BSP rooms).
        if (goingDown && (depth_ == Dungeon::MINES_DEPTH || depth_ == Dungeon::DEEP_MINES_DEPTH)) {
            if (depth_ == Dungeon::MINES_DEPTH) {
                pushMsg("THE STONE GIVES WAY TO ROUGH-HEWN TUNNELS...", MessageKind::System, true);
                pushMsg("YOU HAVE ENTERED THE MINES.", MessageKind::System, true);
            } else {
                pushMsg("THE AIR GROWS THICK WITH DUST AND ECHOES...", MessageKind::System, true);
                pushMsg("DEEP MINES: FISSURES CRACK THE EARTH BETWEEN WANDERING TUNNELS.", MessageKind::System, true);
            }
        }

        // Special floor callout: the organic Warrens layout is a network of dug-out burrows,
        // with chambers as landmarks and lots of dead ends (great for secrets/closets).
        if (goingDown && dung.hasWarrens) {
            pushMsg("THE PASSAGES NARROW INTO DAMP BURROWS...", MessageKind::System, true);
            pushMsg("YOU HAVE ENTERED THE WARRENS.", MessageKind::System, true);
        }

        // Fixed-depth generator callout: the Catacombs floor is a dense maze of small tomb rooms.
        if (goingDown && depth_ == Dungeon::CATACOMBS_DEPTH) {
            pushMsg("YOU ENTER A MAZE OF NARROW VAULTS AND CRUMBLING TOMBS.", MessageKind::System, true);
            pushMsg("THE CATACOMBS STRETCH OUT IN EVERY DIRECTION.", MessageKind::System, true);
        }

        // Infinite World: endless depths are grouped into run-seeded strata (macro-themed bands).
        // Announce when we enter a new stratum so the player feels the large-scale progression.
        if (!restored && goingDown && infiniteWorldEnabled_ && depth_ > DUNGEON_MAX_DEPTH && dung.endlessStratumIndex >= 0) {
            if (dung.endlessStratumLocal == 0) {
                pushMsg("THE STONE SHIFTS UNDERFOOT...", MessageKind::System, true);

                std::ostringstream ms;
                ms << "YOU ENTER THE " << endlessStratumThemeName(dung.endlessStratumTheme) << " STRATUM.";
                pushMsg(ms.str(), MessageKind::System, true);
            }
        }



        // Subtle hint: some floors hide secret shortcut doors between adjacent passages.
        if (goingDown && dung.secretShortcutCount > 0) {
            pushMsg("YOU FEEL A FAINT DRAFT IN THE WALLS.", MessageKind::System, true);
        }

        // Hint: some floors also contain visible locked shortcut gates (key/lockpick shortcuts).
        if (goingDown && dung.lockedShortcutCount > 0) {
            pushMsg("YOU HEAR THE RATTLE OF CHAINS IN THE DARK.", MessageKind::System, true);
        }

        // Hint: annex micro-dungeons are optional side areas behind (usually) hidden doors.
        if (goingDown && dung.annexCount > 0) {
            pushMsg("YOU HEAR A HOLLOW ECHO BEHIND THE STONE.", MessageKind::System, true);
        }

        // Hint: fractal annexes feel like branching roots/coral tunnels.
        if (goingDown && dung.annexFractalCount > 0) {
            pushMsg("YOU HEAR A THOUSAND BRANCHING ECHOES UNDERFOOT.", MessageKind::System, true);
        }

        // Hint: WFC annexes have a distinct "manufactured" / shifting-lattice feel.
        if (goingDown && dung.annexWfcCount > 0) {
            pushMsg("THE STONE SEEMS TO REARRANGE ITSELF IN STRANGE PATTERNS.", MessageKind::System, true);
        }

        if (goingDown && dung.annexKeyGateCount > 0) {
            pushMsg("YOU HEAR THE CLICK OF A LOCK DEEP WITHIN.", MessageKind::System, true);
        }

        // Special floor callout: the Sokoban puzzle floor teaches/spotlights the
        // boulder-into-chasm bridging mechanic.
        if (goingDown && depth_ == Dungeon::SOKOBAN_DEPTH) {
            pushMsg("THE AIR VIBRATES WITH A LOW RUMBLE...", MessageKind::System, true);
            pushMsg("BOULDERS AND CHASMS AHEAD. BRIDGE THE GAPS BY PUSHING BOULDERS INTO THEM.", MessageKind::System, true);
        }

        // Special floor callout: the Rogue homage floor is deliberately doorless and grid-based,
        // echoing classic Rogue/NetHack pacing.
        if (goingDown && depth_ == Dungeon::ROGUE_LEVEL_DEPTH) {
            pushMsg("YOU ENTER WHAT SEEMS TO BE AN OLDER, MORE PRIMITIVE WORLD.", MessageKind::System, true);
        }

        // Deep descent callout: the default run is longer now, so the "final approach"
        // begins a few floors earlier than the labyrinth setpiece.
        if (goingDown && depth_ == QUEST_DEPTH - 6) {
            pushMsg("THE DUNGEON PLUNGES EVEN DEEPER...", MessageKind::System, true);
            pushMsg("THE AIR GROWS THICK WITH OLD MAGIC.", MessageKind::System, true);
        }

        if (goingDown && depth_ == QUEST_DEPTH - 1) {
            pushMsg("THE PASSAGES TWIST AND TURN... YOU ENTER A LABYRINTH.", MessageKind::System, true);
            pushMsg("IN THE DISTANCE, YOU HEAR THE DRUMMING OF HEAVY HOOVES...", MessageKind::Warning, true);
        }
        if (goingDown && depth_ == QUEST_DEPTH && !playerHasAmulet()) {
            pushMsg("A SINISTER PRESENCE LURKS AHEAD. THE AMULET MUST BE NEAR...", MessageKind::System, true);
        }

    }

    // Infinite world: prune distant deep levels to keep the cache bounded.
    pruneEndlessLevels();

    // Safety: when autosave is enabled, also autosave on floor transitions.
    // This avoids losing progress between levels even if the turn-based autosave interval hasn't triggered yet.
    if (autosaveInterval > 0 && !isFinished()) {
        const std::string ap = defaultAutosavePath();
        if (!ap.empty()) {
            if (saveToFile(ap, true)) {
                lastAutosaveTurn = turnCount;
            }
        }
    }
}


void Game::triggerShopTheftAlarm(Vec2i shopInsidePos, Vec2i playerPos) {
    // If no peaceful shopkeeper remains, there is nobody to raise the alarm.
    if (!anyPeacefulShopkeeper(ents, playerId_)) return;

    setShopkeepersAlerted(ents, playerId_, playerPos, true);
    pushMsg("THE SHOPKEEPER SHOUTS: \"THIEF!\"", MessageKind::Warning, true);

    // Merchant guild now considers you "wanted" until the debt is cleared.
    merchantGuildAlerted_ = true;

    // Try to find the shop room so guards can spawn just outside.
    const Room* shopRoom = nullptr;
    for (const Room& r : dung.rooms) {
        if (r.type != RoomType::Shop) continue;
        if (r.contains(shopInsidePos.x, shopInsidePos.y)) {
            shopRoom = &r;
            break;
        }
    }

    // Sample potential spawn tiles near the shop boundary.
    std::vector<Vec2i> candidates;
    if (shopRoom) {
        for (int y = shopRoom->y - 1; y <= shopRoom->y2(); ++y) {
            for (int x = shopRoom->x - 1; x <= shopRoom->x2(); ++x) {
                if (!dung.inBounds(x, y)) continue;

                const Vec2i p{x, y};
                if (!dung.isWalkable(x, y)) continue;
                if (entityAt(x, y)) continue;

                // Avoid spawning inside the shop room.
                if (shopRoom->contains(p.x, p.y)) continue;

                candidates.push_back(p);
            }
        }
    }

    auto pickSpawn = [&]() -> Vec2i {
        if (!candidates.empty()) {
            return candidates[rng.range(0, static_cast<int>(candidates.size()) - 1)];
        }
        // Fallback: near the player.
        Vec2i best = playerPos;
        for (int tries = 0; tries < 120; ++tries) {
            Vec2i p{playerPos.x + rng.range(-6, 6), playerPos.y + rng.range(-6, 6)};
            if (!dung.inBounds(p.x, p.y)) continue;
            if (!dung.isWalkable(p.x, p.y)) continue;
            if (entityAt(p.x, p.y)) continue;
            best = p;
            break;
        }
        return best;
    };

    // Spawn a small response team.
    const int n = 2 + (rng.chance(0.35f) ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        Vec2i sp = pickSpawn();
        if (!dung.inBounds(sp.x, sp.y) || !dung.isWalkable(sp.x, sp.y) || entityAt(sp.x, sp.y)) continue;

        Entity& g = spawnMonster(EntityKind::Guard, sp, 0, /*allowGear=*/true);
        g.alerted = true;
        g.lastKnownPlayerPos = player().pos;
        g.lastKnownPlayerAge = 0;
        g.energy = 0;
    }
}




// -----------------------------------------------------------------------------
// Determinism hash
// -----------------------------------------------------------------------------
//
// This is used by replay recording/verification to detect divergences in gameplay
// simulation. It intentionally excludes:
//   - UI-only state (menus, cursors, message log)
//   - transient visual FX animation (projectiles/explosions)
//   - real-time timers (dt-dependent pacing)
// so that it remains stable across frame rates.
//
// The goal is not cryptographic security; it's a fast, deterministic fingerprint.
namespace {
struct Hash64 {
    // FNV-1a 64-bit
    uint64_t h = 14695981039346656037ULL;
    static constexpr uint64_t kPrime = 1099511628211ull;

    void addByte(uint8_t b) {
        h ^= static_cast<uint64_t>(b);
        h *= kPrime;
    }

    void addBool(bool v) { addByte(v ? 1u : 0u); }
    void addU8(uint8_t v) { addByte(v); }

    void addU16(uint16_t v) {
        addByte(static_cast<uint8_t>(v & 0xFFu));
        addByte(static_cast<uint8_t>((v >> 8) & 0xFFu));
    }

    void addU32(uint32_t v) {
        addByte(static_cast<uint8_t>(v & 0xFFu));
        addByte(static_cast<uint8_t>((v >> 8) & 0xFFu));
        addByte(static_cast<uint8_t>((v >> 16) & 0xFFu));
        addByte(static_cast<uint8_t>((v >> 24) & 0xFFu));
    }

    void addU64(uint64_t v) {
        addByte(static_cast<uint8_t>(v & 0xFFull));
        addByte(static_cast<uint8_t>((v >> 8) & 0xFFull));
        addByte(static_cast<uint8_t>((v >> 16) & 0xFFull));
        addByte(static_cast<uint8_t>((v >> 24) & 0xFFull));
        addByte(static_cast<uint8_t>((v >> 32) & 0xFFull));
        addByte(static_cast<uint8_t>((v >> 40) & 0xFFull));
        addByte(static_cast<uint8_t>((v >> 48) & 0xFFull));
        addByte(static_cast<uint8_t>((v >> 56) & 0xFFull));
    }

    void addI32(int v) { addU32(static_cast<uint32_t>(v)); }
    void addSize(size_t v) { addU64(static_cast<uint64_t>(v)); }

    void addVec2(Vec2i p) {
        addI32(p.x);
        addI32(p.y);
    }

    void addString(const std::string& s) {
        addU32(static_cast<uint32_t>(s.size()));
        for (unsigned char c : s) addByte(static_cast<uint8_t>(c));
    }

    template <typename EnumT>
    void addEnum(EnumT e) {
        addU32(static_cast<uint32_t>(e));
    }
};

static void hashItem(Hash64& hh, const Item& it) {
    hh.addI32(it.id);
    hh.addEnum(it.kind);
    hh.addI32(it.count);
    hh.addI32(it.charges);
    hh.addI32(it.enchant);
    hh.addI32(it.buc);
    hh.addU32(it.spriteSeed);
    hh.addI32(it.shopPrice);
    hh.addI32(it.shopDepth);
    hh.addEnum(it.ego);
    hh.addU32(static_cast<uint32_t>(it.flags));
}

static void hashEffects(Hash64& hh, const Effects& ef) {
    hh.addI32(ef.poisonTurns);
    hh.addI32(ef.regenTurns);
    hh.addI32(ef.shieldTurns);
    hh.addI32(ef.hasteTurns);
    hh.addI32(ef.visionTurns);
    hh.addI32(ef.invisTurns);
    hh.addI32(ef.webTurns);
    hh.addI32(ef.confusionTurns);
    hh.addI32(ef.burnTurns);
    hh.addI32(ef.levitationTurns);
    hh.addI32(ef.fearTurns);
    hh.addI32(ef.hallucinationTurns);
    hh.addI32(ef.corrosionTurns);
}

static void hashEntity(Hash64& hh, const Entity& e) {
    hh.addI32(e.id);
    hh.addEnum(e.kind);
    hh.addVec2(e.pos);
    hh.addI32(e.hp);
    hh.addI32(e.hpMax);
    hh.addI32(e.baseAtk);
    hh.addI32(e.baseDef);

    hashItem(hh, e.gearMelee);
    hashItem(hh, e.gearArmor);

    hh.addBool(e.canRanged);
    hh.addI32(e.rangedRange);
    hh.addI32(e.rangedAtk);
    hh.addEnum(e.rangedProjectile);
    hh.addEnum(e.rangedAmmo);
    hh.addI32(e.rangedAmmoCount);

    hh.addBool(e.willFlee);
    hh.addBool(e.packAI);
    hh.addI32(e.groupId);

    hh.addI32(e.regenChancePct);
    hh.addI32(e.regenAmount);

    hh.addBool(e.alerted);
    hh.addVec2(e.lastKnownPlayerPos);
    hh.addI32(e.lastKnownPlayerAge);

    hashEffects(hh, e.effects);

    hh.addU32(e.spriteSeed);

    hh.addI32(e.speed);
    hh.addI32(e.energy);

    hh.addBool(e.friendly);
    hh.addEnum(e.allyOrder);

    hh.addI32(e.stolenGold);

    // v38+: pocket consumable (used by some monsters)
    hashItem(hh, e.pocketConsumable);
}

static void hashDungeon(Hash64& hh, const Dungeon& d) {
    hh.addI32(d.width);
    hh.addI32(d.height);
    hh.addVec2(d.stairsUp);
    hh.addVec2(d.stairsDown);

    // Tiles: type + fog-of-war.
    hh.addU32(static_cast<uint32_t>(d.tiles.size()));
    for (const auto& t : d.tiles) {
        hh.addEnum(t.type);
        hh.addBool(t.visible);
        hh.addBool(t.explored);
    }

    // Rooms: used for special behaviors (shops, shrines, etc.).
    hh.addU32(static_cast<uint32_t>(d.rooms.size()));
    for (const auto& r : d.rooms) {
        hh.addI32(r.x);
        hh.addI32(r.y);
        hh.addI32(r.w);
        hh.addI32(r.h);
        hh.addEnum(r.type);
    }

    // bonusLootSpots intentionally excluded: generation-only, not serialized, not gameplay.
}

static void hashTrap(Hash64& hh, const Trap& t) {
    hh.addEnum(t.kind);
    hh.addVec2(t.pos);
    hh.addBool(t.discovered);
}

static void hashGroundItem(Hash64& hh, const GroundItem& g) {
    hashItem(hh, g.item);
    hh.addVec2(g.pos);
}

static void hashMapMarker(Hash64& hh, const MapMarker& m) {
    hh.addVec2(m.pos);
    hh.addEnum(m.kind);
    hh.addString(m.label);
}

static void hashEngraving(Hash64& hh, const Engraving& e) {
    hh.addVec2(e.pos);
    hh.addString(e.text);
    hh.addU8(e.strength);
    hh.addBool(e.isWard);
    hh.addBool(e.isGraffiti);
}

static void hashChestContainer(Hash64& hh, const ChestContainer& c) {
    hh.addI32(c.chestId);
    hh.addU32(static_cast<uint32_t>(c.items.size()));
    for (const auto& it : c.items) hashItem(hh, it);
}

static void hashLevelState(Hash64& hh, const LevelState& ls) {
    hh.addEnum(ls.branch);
    hh.addI32(ls.depth);
    hashDungeon(hh, ls.dung);

    hh.addU32(static_cast<uint32_t>(ls.monsters.size()));
    for (const auto& e : ls.monsters) hashEntity(hh, e);

    hh.addU32(static_cast<uint32_t>(ls.ground.size()));
    for (const auto& g : ls.ground) hashGroundItem(hh, g);

    hh.addU32(static_cast<uint32_t>(ls.traps.size()));
    for (const auto& t : ls.traps) hashTrap(hh, t);

    hh.addU32(static_cast<uint32_t>(ls.markers.size()));
    for (const auto& m : ls.markers) hashMapMarker(hh, m);

    hh.addU32(static_cast<uint32_t>(ls.engravings.size()));
    for (const auto& e : ls.engravings) hashEngraving(hh, e);

    hh.addU32(static_cast<uint32_t>(ls.chestContainers.size()));
    for (const auto& c : ls.chestContainers) hashChestContainer(hh, c);

    hh.addU32(static_cast<uint32_t>(ls.confusionGas.size()));
    for (uint8_t v : ls.confusionGas) hh.addU8(v);

    hh.addU32(static_cast<uint32_t>(ls.poisonGas.size()));
    for (uint8_t v : ls.poisonGas) hh.addU8(v);

    hh.addU32(static_cast<uint32_t>(ls.corrosiveGas.size()));
    for (uint8_t v : ls.corrosiveGas) hh.addU8(v);

    hh.addU32(static_cast<uint32_t>(ls.fireField.size()));
    for (uint8_t v : ls.fireField) hh.addU8(v);

    hh.addU32(static_cast<uint32_t>(ls.scentField.size()));
    for (uint8_t v : ls.scentField) hh.addU8(v);
}
} // namespace

uint64_t Game::determinismHash() const {
    Hash64 hh;

    // Core run identity.
    hh.addU32(seed_);
    hh.addU32(rng.state);
    hh.addEnum(branch_);
    hh.addI32(depth_);
    hh.addI32(maxDepth);
    hh.addU32(turnCount);

    // Core counters that affect future simulation.
    hh.addI32(playerId_);
    hh.addU32(killCount);
    hh.addI32(naturalRegenCounter);
    hh.addBool(hastePhase);
    hh.addBool(gameOver);
    hh.addBool(gameWon);

    // ID allocation (affects future spawns/loot).
    hh.addI32(nextEntityId);
    hh.addI32(nextItemId);

    // Global toggles that affect gameplay.
    hh.addEnum(autoPickup);
    hh.addBool(autoExploreSearchEnabled_);
    hh.addBool(identifyItemsEnabled);
    hh.addBool(hungerEnabled_);
    hh.addI32(hunger);
    hh.addI32(hungerMax);
    hh.addBool(encumbranceEnabled_);
    hh.addBool(lightingEnabled_);
    hh.addBool(sneakMode_);

    // Shrine state.
    hh.addI32(piety_);
    hh.addU32(prayerCooldownUntilTurn_);

    // Endgame escalation.
    hh.addBool(yendorDoomEnabled_);
    hh.addBool(yendorDoomActive_);
    hh.addI32(yendorDoomLevel_);
    hh.addU32(yendorDoomStartTurn_);
    hh.addU32(yendorDoomLastPulseTurn_);
    hh.addU32(yendorDoomLastSpawnTurn_);
    hh.addI32(yendorDoomMsgStage_);

    // Automation (affects future turns without further player input).
    hh.addEnum(autoMode);
    hh.addU32(static_cast<uint32_t>(autoPathTiles.size()));
    for (const auto& p : autoPathTiles) hh.addVec2(p);
    hh.addSize(autoPathIndex);
    hh.addU32(static_cast<uint32_t>(autoStepDelayMs()));

    hh.addBool(autoExploreGoalIsLoot);
    hh.addVec2(autoExploreGoalPos);
    hh.addBool(autoExploreGoalIsSearch);
    hh.addVec2(autoExploreSearchGoalPos);
    hh.addI32(autoExploreSearchTurnsLeft);
    hh.addBool(autoExploreSearchAnnounced);
    hh.addU32(static_cast<uint32_t>(autoExploreSearchTriedTurns.size()));
    for (uint8_t v : autoExploreSearchTriedTurns) hh.addU8(v);

    // Player progression.
    hh.addEnum(playerClass_);
    hh.addI32(charLevel);
    hh.addI32(xp);
    hh.addI32(xpNext);
    hh.addI32(talentMight_);
    hh.addI32(talentAgility_);
    hh.addI32(talentVigor_);
    hh.addI32(talentFocus_);
    hh.addI32(talentPointsPending_);

    // Inventory + equipment.
    hh.addU32(static_cast<uint32_t>(inv.size()));
    for (const auto& it : inv) hashItem(hh, it);
    hh.addI32(equipMeleeId);
    hh.addI32(equipRangedId);
    hh.addI32(equipRing1Id);
    hh.addI32(equipRing2Id);
    hh.addI32(equipArmorId);

    // Identification tables.
    for (uint8_t v : identKnown) hh.addU8(v);
    for (uint8_t v : identAppearance) hh.addU8(v);

    // Current level: dungeon, entities, items, traps, markers, fields.
    hashDungeon(hh, dung);

    hh.addU32(static_cast<uint32_t>(ents.size()));
    for (const auto& e : ents) hashEntity(hh, e);

    hh.addU32(static_cast<uint32_t>(ground.size()));
    for (const auto& g : ground) hashGroundItem(hh, g);

    hh.addU32(static_cast<uint32_t>(trapsCur.size()));
    for (const auto& t : trapsCur) hashTrap(hh, t);

    hh.addU32(static_cast<uint32_t>(mapMarkers_.size()));
    for (const auto& m : mapMarkers_) hashMapMarker(hh, m);

    hh.addU32(static_cast<uint32_t>(engravings_.size()));
    for (const auto& e : engravings_) hashEngraving(hh, e);

    hh.addU32(static_cast<uint32_t>(chestContainers_.size()));
    for (const auto& c : chestContainers_) hashChestContainer(hh, c);

    hh.addU32(static_cast<uint32_t>(confusionGas_.size()));
    for (uint8_t v : confusionGas_) hh.addU8(v);

    hh.addU32(static_cast<uint32_t>(poisonGas_.size()));
    for (uint8_t v : poisonGas_) hh.addU8(v);

    hh.addU32(static_cast<uint32_t>(corrosiveGas_.size()));
    for (uint8_t v : corrosiveGas_) hh.addU8(v);

    hh.addU32(static_cast<uint32_t>(fireField_.size()));
    for (uint8_t v : fireField_) hh.addU8(v);

    hh.addU32(static_cast<uint32_t>(scentField_.size()));
    for (uint8_t v : scentField_) hh.addU8(v);

    // Persisted off-screen levels.
    //
    // NOTE: `levels` is primarily intended to cache *off-screen* floors, but the
    // currently active depth may also be mirrored into `levels` as a convenience
    // for saving. That cached copy is not authoritative while we're on the
    // level (it may be stale until storeCurrentLevel() runs), so exclude it
    // from the determinism hash to make the hash stable across save/load
    // round-trips.
    const LevelId curId{branch_, depth_};

    uint32_t levelCount = static_cast<uint32_t>(levels.size());
    if (levels.find(curId) != levels.end()) {
        levelCount -= 1u;
    }
    hh.addU32(levelCount);
    for (const auto& kv : levels) {
        if (kv.first == curId) continue;
        hh.addEnum(kv.first.branch);
        hh.addI32(kv.first.depth);
        hashLevelState(hh, kv.second);
    }

    // Pending trapdoor fallers (creatures that fell to deeper levels but aren't placed yet).
    // Keyed by (branch, depth) so multiple branches can safely coexist.
    uint32_t fallEntryCount = 0;
    for (const auto& kv : trapdoorFallers_) {
        if (!kv.second.empty()) ++fallEntryCount;
    }
    hh.addU32(fallEntryCount);
    for (const auto& kv : trapdoorFallers_) {
        const auto& id = kv.first;
        const auto& vec = kv.second;
        if (vec.empty()) continue;
        hh.addEnum(id.branch);
        hh.addI32(id.depth);
        hh.addU32(static_cast<uint32_t>(vec.size()));
        for (const auto& e : vec) hashEntity(hh, e);
    }

    return hh.h;
}


Vec2i Game::windDir() const {
    // Camp is sheltered; keep the surface calm.
    if (branch_ == DungeonBranch::Camp || depth_ <= 0) return {0, 0};

    // Derive from run seed + level id without consuming RNG.
    // This keeps replays deterministic and avoids subtly changing dungeon generation.
    const uint32_t b = static_cast<uint32_t>(static_cast<int>(branch_));
    const uint32_t d = static_cast<uint32_t>(std::max(0, depth_));
    const uint32_t h = hashCombine(seed_, hashCombine(b * 0x9E3779B9u, d * 0x85EBCA6Bu));

    // 1/5 chance of calm air. Otherwise one of four cardinal drafts.
    switch (h % 5u) {
        case 0: return {0, 0};   // calm
        case 1: return {1, 0};   // east
        case 2: return {-1, 0};  // west
        case 3: return {0, 1};   // south
        default: return {0, -1}; // north
    }
}

int Game::windStrength() const {
    const Vec2i w = windDir();
    if (w.x == 0 && w.y == 0) return 0;

    // Independent-ish hash so direction and strength aren't perfectly correlated.
    const uint32_t b = static_cast<uint32_t>(static_cast<int>(branch_));
    const uint32_t d = static_cast<uint32_t>(std::max(0, depth_));
    const uint32_t h = hashCombine(seed_ ^ 0xA341316Cu, hashCombine(b * 0xC8013EA4u, d * 0x7E95761Eu));

    int s = 1 + static_cast<int>((h >> 3) % 3u); // 1..3

    // Gentle depth bias: deeper floors trend slightly draftier, but never exceed 3.
    if (branch_ == DungeonBranch::Main && depth_ >= 13) s += 1;

    return clampi(s, 1, 3);
}



void Game::pushFxParticle(FXParticlePreset preset, Vec2i pos, int intensity, float duration, float delay, uint32_t seed) {
    // Visual-only helper; safe to ignore out-of-bounds events.
    if (!dung.inBounds(pos.x, pos.y)) return;

    FXParticleEvent ev;
    ev.preset = preset;
    ev.pos = pos;
    ev.intensity = clampi(intensity, 1, 200);
    ev.delay = std::max(0.0f, delay);
    ev.timer = 0.0f;
    ev.duration = std::max(0.02f, duration);

    if (seed == 0u) {
        const uint32_t p = (static_cast<uint32_t>(pos.x) & 0xFFFFu) | ((static_cast<uint32_t>(pos.y) & 0xFFFFu) << 16u);
        seed = hashCombine(seed_, hashCombine(turnCount, hashCombine(p, static_cast<uint32_t>(preset))));
    }
    ev.seed = seed;

    constexpr size_t kMaxFxParticles = 256;
    if (fxParticles_.size() >= kMaxFxParticles) {
        // Drop oldest (avoid unbounded growth in pathological cases).
        fxParticles_.erase(fxParticles_.begin());
    }
    fxParticles_.push_back(ev);
}


std::string Game::runConductsTag() const {
    std::vector<const char*> tags;
    tags.reserve(6);

    // A run starts with many conducts intact; they are broken by specific actions.
    // This is inspired by NetHack-style voluntary challenges, but tailored to this game.
    if (conductFoodEaten_ == 0) tags.push_back("FOODLESS");
    if (conductCorpseEaten_ == 0) tags.push_back("VEGETARIAN");
    if (conductPrayers_ == 0) tags.push_back("ATHEIST");
    if (directKillCount_ == 0) tags.push_back("PACIFIST");
    if (conductScrollsRead_ == 0 && conductSpellbooksRead_ == 0) tags.push_back("ILLITERATE");

    std::string out;
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i != 0) out += " | ";
        out += tags[i];
    }
    return out;
}
