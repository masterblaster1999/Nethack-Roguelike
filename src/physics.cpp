#include "physics.hpp"

#include "grid_utils.hpp"

#include <algorithm>

namespace {

Entity* entityById(std::vector<Entity>& ents, int id) {
    for (auto& e : ents) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

Entity* entityAt(std::vector<Entity>& ents, Vec2i pos, int ignoreId = -1) {
    for (auto& e : ents) {
        if (e.hp <= 0) continue;
        if (e.id == ignoreId) continue;
        if (e.pos == pos) return &e;
    }
    return nullptr;
}


inline bool immuneToChasm(const Entity& e) {
    // A tiny bit of flavor/balance: bats (and wizards, who can "float"/blink) don't
    // instantly die to forced chasm shoves.
    return (e.kind == EntityKind::Bat || e.kind == EntityKind::Wizard);
}

int collisionDamage(RNG& rng, const KnockbackConfig& cfg) {
    const int lo = std::min(cfg.collisionMin, cfg.collisionMax);
    const int hi = std::max(cfg.collisionMin, cfg.collisionMax);

    // Power slightly scales the upper bound so heavy knockbacks feel heavier.
    const int scaledHi = std::max(hi, hi + std::max(0, cfg.power - 1));
    return rng.range(lo, scaledHi);
}

float doorSmashChance(TileType doorType, const KnockbackConfig& cfg) {
    if (!cfg.allowDoorSmash) return 0.0f;

    // Fairly conservative: doors shouldn't become irrelevant.
    // Higher power makes it more likely.
    float base = (doorType == TileType::DoorLocked) ? 0.10f : 0.22f;
    base += 0.10f * static_cast<float>(std::max(0, cfg.power - 1));

    // Cap hard.
    if (base > 0.85f) base = 0.85f;
    return base;
}

} // namespace

KnockbackResult applyKnockback(Dungeon& dung,
                               std::vector<Entity>& ents,
                               RNG& rng,
                               int attackerId,
                               int defenderId,
                               int dx,
                               int dy,
                               const KnockbackConfig& cfg) {
    KnockbackResult out;

    dx = clampi(dx, -1, 1);
    dy = clampi(dy, -1, 1);

    Entity* attacker = entityById(ents, attackerId);
    Entity* defender = entityById(ents, defenderId);
    if (!attacker || !defender) {
        out.stop = KnockbackStop::Blocked;
        return out;
    }

    (void)attacker;

    out.start = defender->pos;
    out.end = defender->pos;

    if (dx == 0 && dy == 0) {
        out.stop = KnockbackStop::Blocked;
        return out;
    }

    const int distance = std::max(0, cfg.distance);
    if (distance == 0) {
        out.stop = KnockbackStop::Blocked;
        return out;
    }

    for (int i = 0; i < distance; ++i) {
        const Vec2i from = defender->pos;
        const Vec2i to{from.x + dx, from.y + dy};

        // Bounds -> treat as solid wall collision.
        if (!dung.inBounds(to.x, to.y)) {
            out.stop = KnockbackStop::SlammedWall;
            out.blockedTile = TileType::Wall;
            out.collisionDamageDefender = collisionDamage(rng, cfg);
            defender->hp -= out.collisionDamageDefender;
            return out;
        }

        // Prevent diagonal corner-cutting (same rule as normal movement).
        if (dx != 0 && dy != 0 && !diagonalPassable(dung, from, dx, dy)) {
            out.stop = KnockbackStop::SlammedWall;
            out.blockedTile = TileType::Wall;
            out.collisionDamageDefender = collisionDamage(rng, cfg);
            defender->hp -= out.collisionDamageDefender;
            return out;
        }

        const TileType t = dung.at(to.x, to.y).type;

        // Chasm: special-case. Our chasm is "bottomless" and normally impassable.
        if (t == TileType::Chasm) {
            if (immuneToChasm(*defender)) {
                out.stop = KnockbackStop::ImmuneToChasm;
                return out;
            }

            if (defender->kind == EntityKind::Player && cfg.playerCatchChance > 0.0f && rng.chance(cfg.playerCatchChance)) {
                out.stop = KnockbackStop::CaughtEdge;
                out.collisionDamageDefender = rng.range(1, 3);
                defender->hp -= out.collisionDamageDefender;
                return out;
            }

            // Fall = death.
            out.stop = KnockbackStop::FellIntoChasm;
            defender->hp = 0;
            return out;
        }

        // Entity collision.
        if (Entity* other = entityAt(ents, to, defender->id)) {
            out.stop = KnockbackStop::HitEntity;
            out.otherEntityId = other->id;

            // Damage both a little.
            const int dmg = collisionDamage(rng, cfg);
            out.collisionDamageDefender = dmg;
            out.collisionDamageOther = std::max(1, dmg / 2);
            defender->hp -= out.collisionDamageDefender;
            other->hp -= out.collisionDamageOther;
            return out;
        }

        // Closed/locked doors can sometimes smash open under sufficient force.
        if (t == TileType::DoorClosed || t == TileType::DoorLocked) {
            if (cfg.allowDoorSmash) {
            const float p = doorSmashChance(t, cfg);
            if (rng.chance(p)) {
                // Smash open into a normal open door.
                out.doorChanged = true;
                out.doorPos = to;
                out.doorFrom = t;
                out.doorTo = TileType::DoorOpen;
                dung.at(to.x, to.y).type = TileType::DoorOpen;

                // Continue moving into the doorway.
                defender->pos = to;
                out.stepsMoved += 1;
                out.end = defender->pos;

                // Mark stop reason only if this ends the knockback (caller can still
                // check doorChanged).
                if (i == distance - 1) {
                    out.stop = KnockbackStop::DoorSmashed;
                    return out;
                }

                // Keep pushing beyond the door.
                continue;
            }

            }

            // Door held (or door-smash disabled).
            out.stop = KnockbackStop::SlammedDoor;
            out.blockedTile = t;
            out.collisionDamageDefender = collisionDamage(rng, cfg);
            defender->hp -= out.collisionDamageDefender;
            return out;
        }

        // Solid tiles.
        if (!dung.isWalkable(to.x, to.y)) {
            out.stop = KnockbackStop::SlammedWall;
            out.blockedTile = t;
            out.collisionDamageDefender = collisionDamage(rng, cfg);
            defender->hp -= out.collisionDamageDefender;
            return out;
        }

        // Regular movement step.
        defender->pos = to;
        out.stepsMoved += 1;
        out.end = defender->pos;
    }

    out.stop = KnockbackStop::None;
    return out;
}