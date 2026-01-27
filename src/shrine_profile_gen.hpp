#pragma once

// Procedural shrine patrons.
//
// Each Shrine room gets a deterministic "patron" profile derived from:
//   (runSeed, depth, shrine room rect)
//
// The profile is NOT saved; it is recomputed on demand.
//
// Hooks:
//   - Shrine services can apply small cost biases based on the patron's domain.
//   - UI can show the patron name/domain in HUD + LOOK descriptions.

#include "rng.hpp"
#include "dungeon.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace shrinegen {

// The patron domain roughly corresponds to which shrine service is favored.
// This keeps the mechanic legible: players can learn that "MERCY" shrines
// make HEAL cheaper, "ARTIFICE" shrines make RECHARGE cheaper, etc.
enum class ShrineDomain : uint8_t {
    Mercy = 0,      // Heal
    Cleansing,      // Cure
    Insight,        // Identify (+ augury)
    Benediction,    // Bless
    Purging,        // Uncurse
    Artifice,       // Recharge
    Count
};

// Shrine services that can be cost-biased.
enum class ShrineService : uint8_t {
    Heal = 0,
    Cure,
    Identify,
    Bless,
    Uncurse,
    Recharge,
    Donate,
    Sacrifice,
    Augury,
};

struct ShrineProfile {
    uint32_t seed = 0;
    ShrineDomain domain = ShrineDomain::Mercy;
};

inline const char* domainName(ShrineDomain d) {
    switch (d) {
        case ShrineDomain::Mercy:      return "MERCY";
        case ShrineDomain::Cleansing:  return "CLEANSING";
        case ShrineDomain::Insight:    return "INSIGHT";
        case ShrineDomain::Benediction:return "BENEDICTION";
        case ShrineDomain::Purging:    return "PURGING";
        case ShrineDomain::Artifice:   return "ARTIFICE";
        default:                       return "MYSTERY";
    }
}

inline ShrineService favoredService(ShrineDomain d) {
    switch (d) {
        case ShrineDomain::Mercy:       return ShrineService::Heal;
        case ShrineDomain::Cleansing:   return ShrineService::Cure;
        case ShrineDomain::Insight:     return ShrineService::Identify;
        case ShrineDomain::Benediction: return ShrineService::Bless;
        case ShrineDomain::Purging:     return ShrineService::Uncurse;
        case ShrineDomain::Artifice:    return ShrineService::Recharge;
        default:                        return ShrineService::Heal;
    }
}

inline uint32_t roomGeomKey(const Room& r) {
    // Room coords are small (fits in 8-bit for current maps), but keep it robust.
    const uint32_t x = static_cast<uint32_t>(std::max(0, r.x) & 0x3FF);
    const uint32_t y = static_cast<uint32_t>(std::max(0, r.y) & 0x3FF);
    const uint32_t w = static_cast<uint32_t>(std::max(0, r.w) & 0x3FF);
    const uint32_t h = static_cast<uint32_t>(std::max(0, r.h) & 0x3FF);

    // Mix into one 32-bit word (not strictly a pack; just a stable mix).
    uint32_t g = 0u;
    g = hashCombine(g, x);
    g = hashCombine(g, y);
    g = hashCombine(g, w);
    g = hashCombine(g, h);
    return g;
}

inline ShrineProfile profileFor(uint32_t runSeed, int depth, const Room& shrineRoom) {
    ShrineProfile p;

    uint32_t s = hashCombine(runSeed, "SHRINE_PROF"_tag);
    s = hashCombine(s, static_cast<uint32_t>(std::max(0, depth)));
    s = hashCombine(s, roomGeomKey(shrineRoom));

    RNG rng(hashCombine(s, "DOM"_tag));

    const int domCount = static_cast<int>(ShrineDomain::Count);
    const int domIdx = (domCount > 0) ? rng.range(0, domCount - 1) : 0;
    p.domain = static_cast<ShrineDomain>(domIdx);
    p.seed = s;

    return p;
}

inline int serviceCostPct(ShrineDomain d, ShrineService s) {
    // Returns an integer percentage to multiply a base cost.
    // Target range: 75..120 (small bias; never extreme).

    // Non-prayer actions: keep neutral unless explicitly favored.
    if (s == ShrineService::Donate || s == ShrineService::Sacrifice) {
        return 100;
    }

    // Augury is slightly favored by Insight patrons.
    if (s == ShrineService::Augury) {
        return (d == ShrineDomain::Insight) ? 80 : 100;
    }

    const ShrineService fav = favoredService(d);
    if (s == fav) return 75;

    // Secondary affinities: a small nudge for "neighbor" services.
    switch (d) {
        case ShrineDomain::Mercy:
            if (s == ShrineService::Cure) return 90;
            break;
        case ShrineDomain::Cleansing:
            if (s == ShrineService::Uncurse) return 90;
            break;
        case ShrineDomain::Insight:
            if (s == ShrineService::Recharge) return 90;
            break;
        case ShrineDomain::Benediction:
            if (s == ShrineService::Heal) return 95;
            break;
        case ShrineDomain::Purging:
            if (s == ShrineService::Cure) return 90;
            break;
        case ShrineDomain::Artifice:
            if (s == ShrineService::Identify) return 90;
            break;
        default:
            break;
    }

    // Everything else is slightly pricier.
    return 110;
}

inline std::string deityNameFor(const ShrineProfile& p) {
    // Compact, all-caps names for HUD friendliness.
    RNG rng(hashCombine(p.seed, "DEITY_NAME"_tag));

    static constexpr const char* A[] = {
        "AR", "AZ", "EL", "KA", "LA", "MA", "NO", "OR", "SA", "TA", "UR", "VA", "VO", "XI", "ZA", "OM", "RA", "TH"
    };
    static constexpr const char* B[] = {
        "ON", "US", "IS", "OR", "EN", "UM", "ATH", "IR", "OS", "AEL", "ION", "EKA", "ARA", "ETH"
    };

    const int aN = static_cast<int>(sizeof(A) / sizeof(A[0]));
    const int bN = static_cast<int>(sizeof(B) / sizeof(B[0]));

    std::string n;
    n.reserve(12);
    n += A[rng.range(0, aN - 1)];
    n += B[rng.range(0, bN - 1)];

    // Sometimes extend with a third syllable.
    if (rng.chance(0.35f)) {
        n += B[rng.range(0, bN - 1)];
    }

    // Clamp to something HUD-friendly.
    if (n.size() > 12) n.resize(12);

    // Ensure A-Z only (the tables are already caps; this is defensive).
    for (char& c : n) {
        if (c < 'A' || c > 'Z') c = 'A';
    }

    return n;
}

inline std::string deityEpithetFor(const ShrineProfile& p) {
    RNG rng(hashCombine(p.seed, "DEITY_EPITHET"_tag));

    auto pick = [&](const char* const* arr, int n) -> const char* {
        if (!arr || n <= 0) return "";
        return arr[rng.range(0, n - 1)];
    };

    switch (p.domain) {
        case ShrineDomain::Mercy: {
            static constexpr const char* T[] = {"THE MERCIFUL", "THE GENTLE", "THE KIND"};
            return pick(T, static_cast<int>(sizeof(T) / sizeof(T[0])));
        }
        case ShrineDomain::Cleansing: {
            static constexpr const char* T[] = {"THE PURIFIER", "OF CLEAN HANDS", "THE BRIGHT FLOOD"};
            return pick(T, static_cast<int>(sizeof(T) / sizeof(T[0])));
        }
        case ShrineDomain::Insight: {
            static constexpr const char* T[] = {"THE VEILED EYE", "THE LISTENER", "OF SECRET NAMES"};
            return pick(T, static_cast<int>(sizeof(T) / sizeof(T[0])));
        }
        case ShrineDomain::Benediction: {
            static constexpr const char* T[] = {"THE ANOINTING LIGHT", "THE BLESSED JUDGE", "THE RADIANT"};
            return pick(T, static_cast<int>(sizeof(T) / sizeof(T[0])));
        }
        case ShrineDomain::Purging: {
            static constexpr const char* T[] = {"THE BANISHER", "THE UNBINDING WIND", "OF BROKEN CHAINS"};
            return pick(T, static_cast<int>(sizeof(T) / sizeof(T[0])));
        }
        case ShrineDomain::Artifice: {
            static constexpr const char* T[] = {"THE FORGE MIND", "THE CLOCKMAKER", "OF HIDDEN GEARS"};
            return pick(T, static_cast<int>(sizeof(T) / sizeof(T[0])));
        }
        default:
            break;
    }

    return "";
}

inline std::string deityFullTitleFor(const ShrineProfile& p) {
    const std::string base = deityNameFor(p);
    const std::string epi = deityEpithetFor(p);
    if (epi.empty()) return base;
    return base + " " + epi;
}

inline std::string hudLabelFor(const ShrineProfile& p) {
    // Keep HUD label compact: "NAME (DOMAIN)".
    return deityNameFor(p) + " (" + domainName(p.domain) + ")";
}

inline const Room* shrineRoomAt(const Dungeon& d, Vec2i pos) {
    for (const auto& r : d.rooms) {
        if (r.type != RoomType::Shrine) continue;
        if (r.contains(pos.x, pos.y)) return &r;
    }
    return nullptr;
}

} // namespace shrinegen
