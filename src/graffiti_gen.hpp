#pragma once

#include "dungeon.hpp"
#include "rng.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

// Procedural graffiti / rumor generation.
//
// Goals:
//  - Keep messages short and readable (LOOK mode + log).
//  - Bias toward local, floor-specific hints (secret doors, vaults, chasms).
//  - Remain deterministic across platforms.
//
// NOTE: Graffiti is intentionally not "interactive" (unlike wards/sigils). It is
// flavor + soft guidance, inspired by NetHack's random engravings/rumors.

namespace graffitigen {

enum class HintKind : uint8_t {
    SecretDoor = 0,
    LockedDoor,
    Vault,
    Shrine,
    Shop,
    Chasm,
    BoulderBridge,
};

struct Hint {
    HintKind kind{};
    Vec2i pos{};
};

inline const char* dirWord8(Vec2i from, Vec2i to) {
    const int dx = to.x - from.x;
    const int dy = to.y - from.y;
    if (dx == 0 && dy == 0) return "HERE";

    const int ax = std::abs(dx);
    const int ay = std::abs(dy);

    // Strong axis bias -> cardinal.
    if (ay * 2 < ax) return (dx > 0) ? "EAST" : "WEST";
    if (ax * 2 < ay) return (dy > 0) ? "SOUTH" : "NORTH";

    // Otherwise -> diagonal.
    if (dx > 0 && dy > 0) return "SOUTHEAST";
    if (dx > 0 && dy < 0) return "NORTHEAST";
    if (dx < 0 && dy > 0) return "SOUTHWEST";
    return "NORTHWEST";
}

inline const char* distWord(int manhattan) {
    if (manhattan <= 6) return "NEAR";
    if (manhattan <= 14) return "CLOSE";
    if (manhattan <= 26) return "FAR";
    return "VERY FAR";
}

inline bool hasNeighborChasm(const Dungeon& dung, int x, int y) {
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            const int nx = x + dx;
            const int ny = y + dy;
            if (!dung.inBounds(nx, ny)) continue;
            if (dung.at(nx, ny).type == TileType::Chasm) return true;
        }
    }
    return false;
}

inline void addWeighted(std::vector<Hint>& out, HintKind k, Vec2i pos, int w) {
    w = std::max(1, w);
    for (int i = 0; i < w; ++i) out.push_back(Hint{k, pos});
}

inline std::vector<Hint> collectHints(const Dungeon& dung) {
    std::vector<Hint> out;
    out.reserve(64);

    // Room-level hints (weighted; these are rarer but more meaningful).
    for (const Room& r : dung.rooms) {
        Vec2i c{r.cx(), r.cy()};
        switch (r.type) {
            case RoomType::Vault:   addWeighted(out, HintKind::Vault,  c, 4); break;
            case RoomType::Shrine:  addWeighted(out, HintKind::Shrine, c, 3); break;
            case RoomType::Shop:    addWeighted(out, HintKind::Shop,   c, 2); break;
            default: break;
        }
    }

    // Tile-level hints.
    int chasmCount = 0;
    int chasmSumX = 0;
    int chasmSumY = 0;

    for (int y = 0; y < dung.height; ++y) {
        for (int x = 0; x < dung.width; ++x) {
            const TileType tt = dung.at(x, y).type;
            if (tt == TileType::DoorSecret) {
                // Secret doors can be common; keep weight low.
                addWeighted(out, HintKind::SecretDoor, Vec2i{x, y}, 1);
            } else if (tt == TileType::DoorLocked) {
                addWeighted(out, HintKind::LockedDoor, Vec2i{x, y}, 1);
            } else if (tt == TileType::Chasm) {
                chasmCount += 1;
                chasmSumX += x;
                chasmSumY += y;
            } else if (tt == TileType::Boulder) {
                if (hasNeighborChasm(dung, x, y)) {
                    // Boulder bridge opportunities are valuable.
                    addWeighted(out, HintKind::BoulderBridge, Vec2i{x, y}, 2);
                }
            }
        }
    }

    // Add a single centroid hint for chasm presence (avoids flooding the hint list).
    if (chasmCount > 0) {
        Vec2i c{chasmSumX / chasmCount, chasmSumY / chasmCount};
        addWeighted(out, HintKind::Chasm, c, 2);
    }

    return out;
}

inline std::string clipLine(std::string s, size_t maxLen = 72) {
    if (s.size() <= maxLen) return s;
    s.resize(maxLen);
    // Avoid leaving a dangling half-token at the end.
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (!s.empty() && s.back() != '.') s.push_back('.');
    return s;
}

inline std::string makeHintLine(uint32_t seed, const Hint& h, Vec2i from) {
    const char* dir = dirWord8(from, h.pos);
    const int md = std::abs(h.pos.x - from.x) + std::abs(h.pos.y - from.y);
    const char* dist = distWord(md);

    const uint32_t m = hash32(seed ^ 0xBADC0FFEu);
    const int pat = static_cast<int>(m % 6u);

    std::ostringstream ss;

    switch (h.kind) {
        case HintKind::SecretDoor: {
            switch (pat) {
                case 0: ss << "HOLLOW WALL TO THE " << dir << "."; break;
                case 1: ss << "THE " << dir << " WALL SOUNDS WRONG."; break;
                case 2: ss << "SECRETS HIDE " << dist << " " << dir << "."; break;
                case 3: ss << "SCRATCH THE " << dir << " WALL."; break;
                case 4: ss << "STONE LIES " << dir << "."; break;
                default:ss << "LOOK " << dir << "."; break;
            }
        } break;
        case HintKind::LockedDoor: {
            switch (pat) {
                case 0: ss << "LOCK TO THE " << dir << "."; break;
                case 1: ss << "A KEY OPENS THE WAY " << dir << "."; break;
                case 2: ss << "IRON BITES " << dist << " " << dir << "."; break;
                case 3: ss << "HEAR THE TUMBLERS " << dir << "."; break;
                case 4: ss << "DON'T FORGET THE KEY."; break;
                default:ss << "LOCKED DOOR " << dir << "."; break;
            }
        } break;
        case HintKind::Vault: {
            switch (pat) {
                case 0: ss << "TREASURE " << dist << " " << dir << "."; break;
                case 1: ss << "GOLD SLEEPS " << dir << "."; break;
                case 2: ss << "THE VAULT HUNGERS."; break;
                case 3: ss << "THE LOCK LIES. THE TRAP DOESN'T."; break;
                case 4: ss << "COUNT YOUR COINS BEFORE YOU BLEED."; break;
                default:ss << "NOT WORTH IT."; break;
            }
        } break;
        case HintKind::Shrine: {
            switch (pat) {
                case 0: ss << "PRAYER " << dir << "."; break;
                case 1: ss << "LEAVE AN OFFERING."; break;
                case 2: ss << "THE GODS LISTEN."; break;
                case 3: ss << "CLEAN HANDS. QUIET STEPS."; break;
                case 4: ss << "ALTAR " << dist << " " << dir << "."; break;
                default:ss << "KNEEL OR RUN."; break;
            }
        } break;
        case HintKind::Shop: {
            switch (pat) {
                case 0: ss << "PAY YOUR DEBTS."; break;
                case 1: ss << "SHOP " << dir << "."; break;
                case 2: ss << "THE SHOPKEEPER REMEMBERS."; break;
                case 3: ss << "STEALING IS LOUD."; break;
                case 4: ss << "COUNT THE PRICE TWICE."; break;
                default:ss << "NO FREE LUNCH."; break;
            }
        } break;
        case HintKind::Chasm: {
            switch (pat) {
                case 0: ss << "RIFT " << dir << "."; break;
                case 1: ss << "DON'T LOOK DOWN."; break;
                case 2: ss << "THE CHASM EATS SOUND."; break;
                case 3: ss << "THE EDGE IS THIRSTY."; break;
                case 4: ss << "BRIDGE THE GAP."; break;
                default:ss << "WATCH YOUR STEP."; break;
            }
        } break;
        case HintKind::BoulderBridge: {
            switch (pat) {
                case 0: ss << "BOULDERS CAN CROSS."; break;
                case 1: ss << "PUSH THE STONE."; break;
                case 2: ss << "MAKE YOUR OWN BRIDGE."; break;
                case 3: ss << "THE RIFT FEARS WEIGHT."; break;
                case 4: ss << "STONE TO " << dir << "."; break;
                default:ss << "MOVE THE BOULDER."; break;
            }
        } break;
        default:
            ss << "...";
            break;
    }

    return clipLine(ss.str());
}

inline std::string makeAmbientLine(uint32_t seed, RoomType roomType, int depth) {
    // Short, classic one-liners (kept for clarity and NetHack-ish flavor).
    static constexpr std::array<const char*, 16> kOneLiners = {{
        "DON'T PANIC.",
        "KICKING DOORS HURTS.",
        "THE WALLS HAVE EARS.",
        "THE DEAD CAN SMELL YOU.",
        "TRUST YOUR NOSE.",
        "WORDS CAN BE WEAPONS.",
        "SALT KEEPS THE DEAD BACK.",
        "COLD IRON STOPS TRICKSTERS.",
        "FIRE MAKES SLIME WARY.",
        "YOU ARE NOT THE FIRST.",
        "BONES DON'T LIE.",
        "GREED GETS YOU KILLED.",
        "THE FLOOR REMEMBERS.",
        "SOME WORDS SCARE BEASTS.",
        "WRITE IN THE DUST.",
        "LOOK BEFORE YOU LEAP.",
    }};

    // A small grammar with lots of combinatorial output.
    static constexpr std::array<const char*, 16> kImp = {{
        "RUN", "HIDE", "LISTEN", "BREATHE", "WAIT", "SEARCH", "PRAY", "WATCH",
        "REMEMBER", "FORGET", "DODGE", "BACK OFF", "COUNT", "EAT", "SLEEP", "WAKE",
    }};

    static constexpr std::array<const char*, 20> kNoun = {{
        "THE DARK", "THE WALLS", "THE FLOOR", "THE DEAD", "THE RIFT", "THE SMELL",
        "YOUR FEAR", "GREED", "THE KEY", "THE LOCK", "THE ECHO", "THE TORCH",
        "THE STAIRS", "THE GODS", "THE MIRROR", "THE SLIME", "THE WEB", "THE BONES",
        "THE MAP", "THE DOOR",
    }};

    static constexpr std::array<const char*, 16> kVerb = {{
        "REMEMBERS", "LISTENS", "HUNTS", "WHISPERS", "BITES", "WATCHES", "WAITS", "LIES",
        "DREAMS", "BREATHES", "TURNS", "SLEEPS", "SCREAMS", "SMILES", "STARVES", "CALLS",
    }};

    static constexpr std::array<const char*, 16> kAdj = {{
        "COLD", "HUNGRY", "SILENT", "LOUD", "SHARP", "DULL", "HOLLOW", "ANCIENT",
        "RESTLESS", "BRIGHT", "BROKEN", "HIDDEN", "TWISTED", "WET", "DRY", "WARY",
    }};

    // A few room-flavored nudges.
    static constexpr std::array<const char*, 6> kRoomShrine = {{
        "LEAVE AN OFFERING.",
        "PRAY WITH CLEAN HANDS.",
        "THE GODS DO NOT FORGET.",
        "DON'T LIE TO THE ALTAR.",
        "BLESSINGS HAVE A PRICE.",
        "KNEEL OR RUN.",
    }};

    static constexpr std::array<const char*, 5> kRoomLibrary = {{
        "SILENCE, PLEASE.",
        "READ CAREFULLY.",
        "WORDS CUT DEEPER.",
        "DON'T TRUST TITLES.",
        "THE INDEX IS A LIE.",
    }};

    static constexpr std::array<const char*, 5> kRoomLab = {{
        "DO NOT MIX POTIONS.",
        "EYE PROTECTION ADVISED.",
        "IF IT BUBBLES, RUN.",
        "THE SMOKE THINKS.",
        "GLASS BREAKS.",
    }};

    static constexpr std::array<const char*, 5> kRoomArmory = {{
        "POINTY END OUT.",
        "COUNT YOUR ARROWS.",
        "BLADES RUST. SKILLS DON'T.",
        "DROPPED WEAPONS ARE BAIT.",
        "OIL YOUR EDGE.",
    }};

    static constexpr std::array<const char*, 6> kRoomVault = {{
        "LOCKS LIE.",
        "TREASURE BITES.",
        "NOT WORTH IT.",
        "THE VAULT HUNGERS.",
        "COUNT YOUR COINS BEFORE YOU BLEED.",
        "GREED GETS YOU KILLED.",
    }};

    static constexpr std::array<const char*, 5> kRoomSecret = {{
        "SHHH.",
        "YOU FOUND IT.",
        "LOOK BEHIND THE LOOK.",
        "DON'T TRUST WALLS.",
        "SECRETS WANT COMPANY.",
    }};

    static constexpr std::array<const char*, 6> kRoomShop = {{
        "PAY FIRST.",
        "NO FREE LUNCH.",
        "STEALING IS LOUD.",
        "THE SHOPKEEPER REMEMBERS.",
        "COUNT THE PRICE TWICE.",
        "DEBTS FOLLOW YOU.",
    }};

    // Choose a path using a single hashed seed (avoid consuming global RNG beyond one draw).
    const uint32_t h = hash32(seed ^ 0xFEEDBEEFu);
    const int mode = static_cast<int>(h % 8u);

    // Some percentage of ambient lines are direct one-liners.
    const uint32_t oneRoll = hash32(h ^ 0x0BADCABEu) % 100u;
    if (oneRoll < 32u) {
        return std::string(kOneLiners[hash32(h ^ 0x00u) % kOneLiners.size()]);
    }

    // Strong room-type override sometimes.
    const uint32_t roomRoll = hash32(h ^ 0xA11CEu) % 100u;
    if (roomType == RoomType::Shrine && roomRoll < 70u) {
        return std::string(kRoomShrine[hash32(h ^ 0x51u) % kRoomShrine.size()]);
    }
    if (roomType == RoomType::Library && roomRoll < 70u) {
        return std::string(kRoomLibrary[hash32(h ^ 0x52u) % kRoomLibrary.size()]);
    }
    if (roomType == RoomType::Laboratory && roomRoll < 70u) {
        return std::string(kRoomLab[hash32(h ^ 0x53u) % kRoomLab.size()]);
    }
    if (roomType == RoomType::Armory && roomRoll < 70u) {
        return std::string(kRoomArmory[hash32(h ^ 0x54u) % kRoomArmory.size()]);
    }
    if (roomType == RoomType::Vault && roomRoll < 70u) {
        return std::string(kRoomVault[hash32(h ^ 0x55u) % kRoomVault.size()]);
    }
    if (roomType == RoomType::Secret && roomRoll < 70u) {
        return std::string(kRoomSecret[hash32(h ^ 0x56u) % kRoomSecret.size()]);
    }
    if (roomType == RoomType::Shop && roomRoll < 70u) {
        return std::string(kRoomShop[hash32(h ^ 0x57u) % kRoomShop.size()]);
    }

    const char* imp = kImp[hash32(h ^ 0x01u) % kImp.size()];
    const char* noun = kNoun[hash32(h ^ 0x02u) % kNoun.size()];
    const char* verb = kVerb[hash32(h ^ 0x03u) % kVerb.size()];
    const char* adj = kAdj[hash32(h ^ 0x04u) % kAdj.size()];

    std::ostringstream ss;
    switch (mode) {
        case 0: ss << imp << "."; break;
        case 1: ss << "BEWARE OF " << noun << "."; break;
        case 2: ss << noun << " " << verb << "."; break;
        case 3: ss << noun << " IS " << adj << "."; break;
        case 4: ss << "DEPTH " << std::max(1, depth) << " " << verb << "."; break;
        case 5: ss << imp << ": " << noun << "."; break;
        case 6: ss << "NEVER TRUST " << noun << "."; break;
        default: ss << "REMEMBER " << noun << "."; break;
    }

    return clipLine(ss.str());
}

inline int hintChancePct(RoomType roomType, int depth) {
    // Hints are more common in rooms that already "read" as authored.
    int base = 22;
    switch (roomType) {
        case RoomType::Secret: base = 55; break;
        case RoomType::Vault:  base = 45; break;
        case RoomType::Shrine: base = 32; break;
        case RoomType::Shop:   base = 28; break;
        default: break;
    }
    // Slightly increase with depth.
    base += std::min(10, std::max(0, depth - 3));
    return std::clamp(base, 10, 70);
}

inline std::string maybeAddSignature(uint32_t seed, std::string line) {
    static constexpr std::array<const char*, 10> kSig = {{
        "A FRIEND",
        "A FOOL",
        "THE SCRIBE",
        "THE LAST ONE",
        "NO ONE",
        "A GHOST",
        "THE WATCHER",
        "SOMEONE",
        "THE CARTOGRAPHER",
        "THE QUIET ONE",
    }};

    const uint32_t r = hash32(seed ^ 0x51515151u) % 100u;
    if (r >= 12u) return line;

    const char* sig = kSig[hash32(seed ^ 0xC0FFEEu) % kSig.size()];

    const std::string suffix = std::string(" - ") + sig;
    if (line.size() + suffix.size() > 72) return line;
    line += suffix;
    return line;
}

inline std::string generateLine(uint32_t seed,
                                const Dungeon& dung,
                                int depth,
                                RoomType roomType,
                                Vec2i at,
                                const std::vector<Hint>& hints) {
    (void)dung; // reserved for future branch/theme hooks
    // Decide between hint vs ambient message.
    const int pct = hintChancePct(roomType, depth);
    const uint32_t roll = hash32(seed ^ 0x1234ABCDu) % 100u;

    if (!hints.empty() && static_cast<int>(roll) < pct) {
        // Prefer hints near this graffiti position (more "authored" feel).
        std::vector<const Hint*> local;
        local.reserve(16);
        for (const Hint& h : hints) {
            const int md = std::abs(h.pos.x - at.x) + std::abs(h.pos.y - at.y);
            if (md <= 24) local.push_back(&h);
        }

        if (!local.empty()) {
            const Hint& h = *local[hash32(seed ^ 0x99u) % local.size()];
            return maybeAddSignature(seed, makeHintLine(seed, h, at));
        }

        const Hint& h = hints[hash32(seed ^ 0x77u) % hints.size()];
        return maybeAddSignature(seed, makeHintLine(seed, h, at));
    }

    return maybeAddSignature(seed, makeAmbientLine(seed, roomType, depth));
}

} // namespace graffitigen
