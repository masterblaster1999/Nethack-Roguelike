#include "game.hpp"
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <sstream>

namespace {

const char* kindName(EntityKind k) {
    switch (k) {
        case EntityKind::Player: return "YOU";
        case EntityKind::Goblin: return "GOBLIN";
        case EntityKind::Orc: return "ORC";
        case EntityKind::Bat: return "BAT";
        case EntityKind::Slime: return "SLIME";
        case EntityKind::SkeletonArcher: return "SKELETON";
        case EntityKind::KoboldSlinger: return "KOBOLD";
        case EntityKind::Wolf: return "WOLF";
        default: return "THING";
    }
}

bool isAdjacent4(const Vec2i& a, const Vec2i& b) {
    return (std::abs(a.x - b.x) + std::abs(a.y - b.y)) == 1;
}

} // namespace

Game::Game() : dung(MAP_W, MAP_H) {}

const Entity& Game::player() const {
    for (const auto& e : ents) if (e.id == playerId_) return e;
    return ents.front();
}

Entity& Game::playerMut() {
    for (auto& e : ents) if (e.id == playerId_) return e;
    return ents.front();
}

void Game::pushMsg(const std::string& s) {
    // Keep some scrollback
    if (msgs.size() > 400) {
        msgs.erase(msgs.begin(), msgs.begin() + 100);
        msgScroll = std::min(msgScroll, static_cast<int>(msgs.size()));
    }
    msgs.push_back(s);
    // If not scrolled up, stay pinned to newest.
    if (msgScroll == 0) {
        // pinned
    } else {
        // keep viewing older lines; new messages increase effective scroll
        msgScroll = std::min(msgScroll + 1, static_cast<int>(msgs.size()));
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

int Game::equippedWeaponIndex() const {
    return findItemIndexById(inv, equipWeaponId);
}

int Game::equippedArmorIndex() const {
    return findItemIndexById(inv, equipArmorId);
}

const Item* Game::equippedWeapon() const {
    int idx = equippedWeaponIndex();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

const Item* Game::equippedArmor() const {
    int idx = equippedArmorIndex();
    if (idx < 0) return nullptr;
    return &inv[static_cast<size_t>(idx)];
}

bool Game::isEquipped(int itemId) const {
    return itemId != 0 && (itemId == equipWeaponId || itemId == equipArmorId);
}

std::string Game::equippedWeaponName() const {
    const Item* w = equippedWeapon();
    return w ? itemDisplayName(*w) : std::string("(NONE)");
}

std::string Game::equippedArmorName() const {
    const Item* a = equippedArmor();
    return a ? itemDisplayName(*a) : std::string("(NONE)");
}

int Game::playerAttack() const {
    int atk = player().baseAtk;
    if (const Item* w = equippedWeapon()) {
        atk += itemDef(w->kind).meleeAtk;
    }
    return atk;
}

int Game::playerDefense() const {
    int def = player().baseDef;
    if (const Item* a = equippedArmor()) {
        def += itemDef(a->kind).defense;
    }
    return def;
}

int Game::playerRangedRange() const {
    const Item* w = equippedWeapon();
    if (!w) return 0;
    return itemDef(w->kind).range;
}

bool Game::playerHasRangedReady(std::string* reasonOut) const {
    const Item* w = equippedWeapon();
    if (!w) {
        if (reasonOut) *reasonOut = "NO WEAPON EQUIPPED.";
        return false;
    }
    const ItemDef& d = itemDef(w->kind);
    if (d.range <= 0) {
        if (reasonOut) *reasonOut = "THAT WEAPON CAN'T FIRE.";
        return false;
    }
    if (w->kind == ItemKind::Bow) {
        if (ammoCount(inv, AmmoKind::Arrow) <= 0) {
            if (reasonOut) *reasonOut = "NO ARROWS.";
            return false;
        }
    }
    if (w->kind == ItemKind::WandSparks) {
        if (w->charges <= 0) {
            if (reasonOut) *reasonOut = "THE WAND IS EMPTY.";
            return false;
        }
    }
    return true;
}

void Game::newGame(uint32_t seed) {
    rng = RNG(seed);
    level_ = 1;

    ents.clear();
    ground.clear();
    inv.clear();
    fx.clear();

    nextEntityId = 1;
    nextItemId = 1;
    equipWeaponId = 0;
    equipArmorId = 0;

    invOpen = false;
    invSel = 0;
    targeting = false;
    targetLine.clear();
    targetValid = false;

    msgs.clear();
    msgScroll = 0;

    inputLock = false;
    gameOver = false;

    dung.generate(rng);

    // Create player
    Entity p;
    p.id = nextEntityId++;
    p.kind = EntityKind::Player;
    p.pos = dung.stairsUp;
    p.hpMax = 18;
    p.hp = p.hpMax;
    p.baseAtk = 3;
    p.baseDef = 0;
    p.spriteSeed = rng.nextU32();
    playerId_ = p.id;

    ents.push_back(p);

    // Starting gear
    auto give = [&](ItemKind k, int count = 1) {
        Item it;
        it.id = nextItemId++;
        it.kind = k;
        it.count = std::max(1, count);
        it.spriteSeed = rng.nextU32();
        if (k == ItemKind::WandSparks) it.charges = itemDef(k).maxCharges;
        inv.push_back(it);
        return it.id;
    };

    int bowId = give(ItemKind::Bow, 1);
    give(ItemKind::Arrow, 14);
    int dagId = give(ItemKind::Dagger, 1);
    int armId = give(ItemKind::LeatherArmor, 1);
    give(ItemKind::PotionHealing, 2);
    give(ItemKind::ScrollTeleport, 1);
    give(ItemKind::Gold, 10);

    // Equip bow by default so FIRE works immediately.
    equipWeaponId = bowId;
    equipArmorId = armId;

    spawnMonsters();
    spawnItems();

    recomputeFov();

    pushMsg("WELCOME TO PROCROGUE++.");
    pushMsg("PRESS I FOR INVENTORY. PRESS F TO TARGET/FIRE.");
    pushMsg("FIND THE STAIRS (>) TO DESCEND.");
    (void)dagId;
}

void Game::nextLevel() {
    level_ += 1;

    // Keep player and inventory, regenerate dungeon + spawns.
    Entity& p = playerMut();

    // Remove monsters
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return e.id != playerId_;
    }), ents.end());

    ground.clear();
    fx.clear();
    inputLock = false;

    invOpen = false;
    targeting = false;
    msgScroll = 0;

    dung.generate(rng);
    p.pos = dung.stairsUp;
    p.alerted = false;

    // Small heal on level transition
    p.hp = std::min(p.hpMax, p.hp + 2);

    std::ostringstream ss;
    ss << "YOU DESCEND TO LEVEL " << level_ << ".";
    pushMsg(ss.str());

    spawnMonsters();
    spawnItems();
    recomputeFov();
}

void Game::update(float dt) {
    // Animate FX projectiles.
    if (!fx.empty()) {
        inputLock = true;
        for (auto& p : fx) {
            p.stepTimer += dt;
            while (p.stepTimer >= p.stepTime) {
                p.stepTimer -= p.stepTime;
                if (p.pathIndex + 1 < p.path.size()) {
                    p.pathIndex++;
                } else {
                    p.pathIndex = p.path.size();
                    break;
                }
            }
        }
        fx.erase(std::remove_if(fx.begin(), fx.end(), [](const FXProjectile& p) {
            return p.path.empty() || p.pathIndex >= p.path.size();
        }), fx.end());
    }

    if (fx.empty()) {
        inputLock = false;
    }
}

void Game::handleAction(Action a) {
    if (a == Action::None) return;

    // Message log scroll works in any mode, even during targeting/inv.
    if (a == Action::LogUp) {
        int maxScroll = std::max(0, static_cast<int>(msgs.size()) - 1);
        msgScroll = clampi(msgScroll + 1, 0, maxScroll);
        return;
    }
    if (a == Action::LogDown) {
        int maxScroll = std::max(0, static_cast<int>(msgs.size()) - 1);
        msgScroll = clampi(msgScroll - 1, 0, maxScroll);
        return;
    }

    if (gameOver) {
        if (a == Action::Restart) {
            newGame(hash32(rng.nextU32()));
        }
        return;
    }

    if (inputLock) {
        // Ignore actions while animating.
        if (a == Action::Cancel && (invOpen || targeting)) {
            invOpen = false;
            targeting = false;
            return;
        }
        return;
    }

    bool acted = false;

    // Inventory mode
    if (invOpen) {
        switch (a) {
            case Action::Up: moveInventorySelection(-1); break;
            case Action::Down: moveInventorySelection(1); break;
            case Action::Inventory:
            case Action::Cancel:
                closeInventory();
                break;
            case Action::Equip:
                acted = equipSelected();
                break;
            case Action::Use:
                acted = useSelected();
                break;
            case Action::Drop:
                acted = dropSelected();
                break;
            default:
                break;
        }

        if (acted) {
            monsterTurn();
            cleanupDead();
            recomputeFov();
        }
        return;
    }

    // Targeting mode
    if (targeting) {
        switch (a) {
            case Action::Up: moveTargetCursor(0, -1); break;
            case Action::Down: moveTargetCursor(0, 1); break;
            case Action::Left: moveTargetCursor(-1, 0); break;
            case Action::Right: moveTargetCursor(1, 0); break;
            case Action::Confirm:
            case Action::Fire:
                endTargeting(true);
                acted = true;
                break;
            case Action::Cancel:
                endTargeting(false);
                break;
            default:
                break;
        }

        if (acted) {
            monsterTurn();
            cleanupDead();
            recomputeFov();
        }
        return;
    }

    // Normal play mode
    Entity& p = playerMut();
    switch (a) {
        case Action::Up:    acted = tryMove(p, 0, -1); break;
        case Action::Down:  acted = tryMove(p, 0, 1); break;
        case Action::Left:  acted = tryMove(p, -1, 0); break;
        case Action::Right: acted = tryMove(p, 1, 0); break;
        case Action::Wait:
            pushMsg("YOU WAIT.");
            acted = true;
            break;
        case Action::Pickup:
            acted = pickupAtPlayer();
            break;
        case Action::Inventory:
            openInventory();
            break;
        case Action::Fire:
            beginTargeting();
            break;
        case Action::Confirm:
        case Action::StairsDown:
            if (p.pos == dung.stairsDown) {
                nextLevel();
                acted = false; // nextLevel already advances state
            } else {
                pushMsg("THERE ARE NO STAIRS HERE.");
            }
            break;
        case Action::Restart:
            newGame(hash32(rng.nextU32()));
            acted = false;
            break;
        default:
            break;
    }

    if (acted) {
        monsterTurn();
        cleanupDead();
        recomputeFov();
    }
}

bool Game::tryMove(Entity& e, int dx, int dy) {
    int nx = e.pos.x + dx;
    int ny = e.pos.y + dy;

    if (!dung.inBounds(nx, ny)) return false;

    // Closed door: opening consumes a turn.
    if (dung.isDoorClosed(nx, ny)) {
        dung.openDoor(nx, ny);
        if (e.kind == EntityKind::Player) pushMsg("YOU OPEN THE DOOR.");
        return true;
    }

    if (!dung.isWalkable(nx, ny)) {
        if (e.kind == EntityKind::Player) pushMsg("YOU BUMP INTO A WALL.");
        return false;
    }

    if (Entity* other = entityAtMut(nx, ny)) {
        if (other->id == e.id) return false;
        attackMelee(e, *other);
        return true;
    }

    e.pos.x = nx;
    e.pos.y = ny;
    return true;
}

void Game::attackMelee(Entity& attacker, Entity& defender) {
    int atk = attacker.baseAtk;
    int def = defender.baseDef;

    if (attacker.kind == EntityKind::Player) atk = playerAttack();
    if (defender.kind == EntityKind::Player) def = playerDefense();

    int dmg = std::max(1, atk - def + rng.range(0, 1));
    defender.hp -= dmg;

    std::ostringstream ss;
    if (attacker.kind == EntityKind::Player) {
        ss << "YOU HIT " << kindName(defender.kind) << " FOR " << dmg << ".";
    } else if (defender.kind == EntityKind::Player) {
        ss << kindName(attacker.kind) << " HITS YOU FOR " << dmg << ".";
    } else {
        ss << kindName(attacker.kind) << " HITS " << kindName(defender.kind) << ".";
    }
    pushMsg(ss.str());

    if (defender.hp <= 0) {
        if (defender.kind == EntityKind::Player) {
            pushMsg("YOU DIE.");
            gameOver = true;
        } else {
            std::ostringstream ds;
            ds << kindName(defender.kind) << " DIES.";
            pushMsg(ds.str());
        }
    }
}

std::vector<Vec2i> Game::bresenhamLine(Vec2i a, Vec2i b) {
    std::vector<Vec2i> pts;
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;

    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (true) {
        pts.push_back({x0, y0});
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
        if (pts.size() > 512) break;
    }
    return pts;
}

void Game::attackRanged(Entity& attacker, Vec2i target, int range, int atk, ProjectileKind projKind, bool fromPlayer) {
    std::vector<Vec2i> line = bresenhamLine(attacker.pos, target);
    if (line.size() <= 1) return;

    // Clamp to range (+ start tile)
    if (range > 0 && static_cast<int>(line.size()) > range + 1) {
        line.resize(static_cast<size_t>(range + 1));
    }

    bool hitEntity = false;
    bool hitWall = false;
    Entity* hit = nullptr;

    size_t stopIdx = line.size() - 1;

    for (size_t i = 1; i < line.size(); ++i) {
        Vec2i p = line[i];
        if (!dung.inBounds(p.x, p.y)) { stopIdx = i - 1; break; }

        // Walls/closed doors block projectiles
        if (dung.isOpaque(p.x, p.y)) {
            hitWall = true;
            stopIdx = i;
            break;
        }

        if (Entity* e = entityAtMut(p.x, p.y)) {
            if (e->id != attacker.id && e->hp > 0) {
                hitEntity = true;
                hit = e;
                stopIdx = i;
                break;
            }
        }
    }

    // Apply damage immediately (visual projectile is FX only).
    if (hitEntity && hit) {
        int def = hit->baseDef;
        if (hit->kind == EntityKind::Player) def = playerDefense();

        int dmg = std::max(1, atk - def + rng.range(0, 1));
        hit->hp -= dmg;

        std::ostringstream ss;
        if (fromPlayer) {
            ss << "YOU HIT " << kindName(hit->kind) << " FOR " << dmg << ".";
        } else if (hit->kind == EntityKind::Player) {
            ss << kindName(attacker.kind) << " HITS YOU FOR " << dmg << ".";
        } else {
            ss << kindName(attacker.kind) << " HITS " << kindName(hit->kind) << ".";
        }
        pushMsg(ss.str());

        if (hit->hp <= 0) {
            if (hit->kind == EntityKind::Player) {
                pushMsg("YOU DIE.");
                gameOver = true;
            } else {
                std::ostringstream ds;
                ds << kindName(hit->kind) << " DIES.";
                pushMsg(ds.str());
            }
        }
    } else if (hitWall) {
        if (fromPlayer) pushMsg("THE SHOT HITS A WALL.");
    } else {
        if (fromPlayer) pushMsg("YOU FIRE.");
    }

    // FX projectile path (truncate)
    std::vector<Vec2i> fxPath;
    fxPath.reserve(stopIdx + 1);
    for (size_t i = 0; i <= stopIdx && i < line.size(); ++i) fxPath.push_back(line[i]);

    FXProjectile fxp;
    fxp.kind = projKind;
    fxp.path = std::move(fxPath);
    fxp.pathIndex = (fxp.path.size() > 1) ? 1 : 0;
    fxp.stepTimer = 0.0f;
    fxp.stepTime = (projKind == ProjectileKind::Spark) ? 0.02f : 0.03f;
    fx.push_back(std::move(fxp));

    inputLock = true;
}

void Game::recomputeFov() {
    Entity& p = playerMut();
    dung.computeFov(p.pos.x, p.pos.y, 9);
}

void Game::openInventory() {
    invOpen = true;
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
}

void Game::closeInventory() {
    invOpen = false;
}

void Game::moveInventorySelection(int dy) {
    if (inv.empty()) { invSel = 0; return; }
    invSel = clampi(invSel + dy, 0, static_cast<int>(inv.size()) - 1);
}

bool Game::pickupAtPlayer() {
    Vec2i ppos = player().pos;

    std::vector<size_t> idxs;
    for (size_t i = 0; i < ground.size(); ++i) {
        if (ground[i].pos == ppos) idxs.push_back(i);
    }
    if (idxs.empty()) {
        pushMsg("NOTHING HERE.");
        return false;
    }

    const int maxInv = 26;
    bool pickedAny = false;

    // Pick up in reverse order so erase indices stay valid.
    for (size_t k = idxs.size(); k-- > 0;) {
        size_t gi = idxs[k];
        if (gi >= ground.size()) continue;

        Item it = ground[gi].item;

        if (tryStackItem(inv, it)) {
            // stacked
            pickedAny = true;
            pushMsg("YOU PICK UP " + itemDisplayName(it) + ".");
            ground.erase(ground.begin() + static_cast<long>(gi));
            continue;
        }

        if (static_cast<int>(inv.size()) >= maxInv) {
            pushMsg("YOUR PACK IS FULL.");
            break;
        }

        inv.push_back(it);
        pickedAny = true;
        pushMsg("YOU PICK UP " + itemDisplayName(it) + ".");
        ground.erase(ground.begin() + static_cast<long>(gi));
    }

    return pickedAny;
}

bool Game::dropSelected() {
    if (inv.empty()) {
        pushMsg("NOTHING TO DROP.");
        return false;
    }

    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
    Item& it = inv[static_cast<size_t>(invSel)];

    // Unequip if needed
    if (it.id == equipWeaponId) equipWeaponId = 0;
    if (it.id == equipArmorId) equipArmorId = 0;

    Item drop = it;
    if (isStackable(it.kind) && it.count > 1) {
        drop.count = 1;
        it.count -= 1;
    } else {
        // remove whole item
        inv.erase(inv.begin() + invSel);
        invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
    }

    GroundItem gi;
    gi.item = drop;
    gi.pos = player().pos;
    ground.push_back(gi);

    pushMsg("YOU DROP " + itemDisplayName(drop) + ".");
    return true;
}

bool Game::equipSelected() {
    if (inv.empty()) {
        pushMsg("NOTHING TO EQUIP.");
        return false;
    }
    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
    const Item& it = inv[static_cast<size_t>(invSel)];
    const ItemDef& d = itemDef(it.kind);

    if (d.slot == EquipSlot::Weapon) {
        if (equipWeaponId == it.id) {
            equipWeaponId = 0;
            pushMsg("YOU UNWIELD " + itemDisplayName(it) + ".");
        } else {
            equipWeaponId = it.id;
            pushMsg("YOU WIELD " + itemDisplayName(it) + ".");
        }
        return true;
    }
    if (d.slot == EquipSlot::Armor) {
        if (equipArmorId == it.id) {
            equipArmorId = 0;
            pushMsg("YOU REMOVE " + itemDisplayName(it) + ".");
        } else {
            equipArmorId = it.id;
            pushMsg("YOU WEAR " + itemDisplayName(it) + ".");
        }
        return true;
    }

    pushMsg("YOU CAN'T EQUIP THAT.");
    return false;
}

bool Game::useSelected() {
    if (inv.empty()) {
        pushMsg("NOTHING TO USE.");
        return false;
    }
    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
    Item& it = inv[static_cast<size_t>(invSel)];

    if (it.kind == ItemKind::PotionHealing) {
        Entity& p = playerMut();
        int heal = itemDef(it.kind).healAmount;
        int before = p.hp;
        p.hp = std::min(p.hpMax, p.hp + heal);

        std::ostringstream ss;
        ss << "YOU DRINK A POTION. HP " << before << "->" << p.hp << ".";
        pushMsg(ss.str());

        it.count -= 1;
        if (it.count <= 0) {
            inv.erase(inv.begin() + invSel);
            invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
        }
        return true;
    }

    if (it.kind == ItemKind::ScrollTeleport) {
        // Teleport to a random free floor
        for (int tries = 0; tries < 2000; ++tries) {
            Vec2i p = dung.randomFloor(rng, true);
            if (entityAt(p.x, p.y)) continue;
            playerMut().pos = p;
            break;
        }
        pushMsg("YOU READ A SCROLL. YOU VANISH!");
        it.count -= 1;
        if (it.count <= 0) {
            inv.erase(inv.begin() + invSel);
            invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
        }
        recomputeFov();
        return true;
    }

    pushMsg("NOTHING HAPPENS.");
    return false;
}

void Game::beginTargeting() {
    std::string reason;
    if (!playerHasRangedReady(&reason)) {
        pushMsg(reason);
        return;
    }
    targeting = true;
    invOpen = false;
    targetPos = player().pos;
    recomputeTargetLine();
    pushMsg("TARGETING...");
}

void Game::endTargeting(bool fire) {
    if (!targeting) return;

    if (fire) {
        if (!targetValid) {
            pushMsg("NO CLEAR SHOT.");
        } else {
            // Compute ranged attack from equipped weapon.
            int wIdx = equippedWeaponIndex();
            if (wIdx >= 0) {
                Item& w = inv[static_cast<size_t>(wIdx)];
                const ItemDef& d = itemDef(w.kind);

                if (w.kind == ItemKind::Bow) {
                    if (ammoCount(inv, AmmoKind::Arrow) <= 0) {
                        pushMsg("NO ARROWS.");
                    } else {
                        consumeAmmo(inv, AmmoKind::Arrow, 1);
                        int atk = std::max(1, (playerAttack() + d.rangedAtk) / 2 + rng.range(0, 1));
                        attackRanged(playerMut(), targetPos, d.range, atk, d.projectile, true);
                    }
                } else if (w.kind == ItemKind::WandSparks) {
                    if (w.charges <= 0) {
                        pushMsg("THE WAND IS EMPTY.");
                    } else {
                        w.charges -= 1;
                        int atk = std::max(1, d.rangedAtk + 2 + rng.range(0, 2));
                        attackRanged(playerMut(), targetPos, d.range, atk, d.projectile, true);
                    }
                } else {
                    pushMsg("THAT WEAPON CAN'T FIRE.");
                }
            }
        }
    }

    targeting = false;
    targetLine.clear();
    targetValid = false;
}

void Game::moveTargetCursor(int dx, int dy) {
    targetPos.x = clampi(targetPos.x + dx, 0, dung.width - 1);
    targetPos.y = clampi(targetPos.y + dy, 0, dung.height - 1);
    recomputeTargetLine();
}

void Game::recomputeTargetLine() {
    targetLine = bresenhamLine(player().pos, targetPos);

    // Clamp to range
    int range = playerRangedRange();
    if (range > 0 && static_cast<int>(targetLine.size()) > range + 1) {
        targetLine.resize(static_cast<size_t>(range + 1));
    }

    // Determine validity: must have LOS and be within visible tiles (you can't target what you can't see).
    targetValid = false;

    if (!dung.inBounds(targetPos.x, targetPos.y)) return;
    if (!dung.at(targetPos.x, targetPos.y).visible) return;

    // Verify LOS along clamped line (stop at opaque).
    for (size_t i = 1; i < targetLine.size(); ++i) {
        Vec2i p = targetLine[i];
        if (dung.isOpaque(p.x, p.y)) {
            // If the target is behind an opaque tile, invalid.
            if (p != targetPos) return;
        }
    }

    // Must be within range (by path length)
    if (range > 0) {
        int dist = static_cast<int>(targetLine.size()) - 1;
        if (dist > range) return;
    }

    // Weapon ready?
    std::string reason;
    if (!playerHasRangedReady(&reason)) return;

    targetValid = true;
}

Vec2i Game::randomFreeTileInRoom(const Room& r, int tries) {
    for (int i = 0; i < tries; ++i) {
        int x0 = rng.range(r.x + 1, std::max(r.x + 1, r.x + r.w - 2));
        int y0 = rng.range(r.y + 1, std::max(r.y + 1, r.y + r.h - 2));
        if (!dung.inBounds(x0, y0)) continue;
        TileType t = dung.at(x0, y0).type;
        if (!(t == TileType::Floor || t == TileType::StairsUp || t == TileType::StairsDown || t == TileType::DoorOpen)) continue;
        if (entityAt(x0, y0)) continue;
        return {x0, y0};
    }
    return {r.cx(), r.cy()};
}

void Game::spawnMonsters() {
    const auto& rooms = dung.rooms;
    if (rooms.empty()) return;

    int nextGroup = 1;

    auto addMonster = [&](EntityKind k, Vec2i pos, int groupId) {
        Entity e;
        e.id = nextEntityId++;
        e.kind = k;
        e.pos = pos;
        e.spriteSeed = rng.nextU32();
        e.groupId = groupId;

        switch (k) {
            case EntityKind::Goblin:
                e.hpMax = 6; e.baseAtk = 2; e.baseDef = 0;
                break;
            case EntityKind::Orc:
                e.hpMax = 10; e.baseAtk = 3; e.baseDef = 1;
                break;
            case EntityKind::Bat:
                e.hpMax = 4; e.baseAtk = 1; e.baseDef = 0;
                break;
            case EntityKind::Slime:
                e.hpMax = 8; e.baseAtk = 2; e.baseDef = 0;
                break;
            case EntityKind::SkeletonArcher:
                e.hpMax = 7; e.baseAtk = 2; e.baseDef = 0;
                e.canRanged = true; e.rangedRange = 7; e.rangedAtk = 2;
                e.rangedProjectile = ProjectileKind::Arrow;
                break;
            case EntityKind::KoboldSlinger:
                e.hpMax = 5; e.baseAtk = 1; e.baseDef = 0;
                e.canRanged = true; e.rangedRange = 6; e.rangedAtk = 1;
                e.rangedProjectile = ProjectileKind::Rock;
                e.willFlee = true;
                break;
            case EntityKind::Wolf:
                e.hpMax = 6; e.baseAtk = 2; e.baseDef = 0;
                e.packAI = true; e.groupId = groupId;
                break;
            default:
                e.hpMax = 5; e.baseAtk = 2; e.baseDef = 0;
                break;
        }
        e.hp = e.hpMax;

        ents.push_back(e);
    };

    // Spawn per room, scaling with level.
    for (size_t i = 0; i < rooms.size(); ++i) {
        const Room& r = rooms[i];

        // Don't spawn in the starting room too aggressively.
        bool isStart = (r.contains(dung.stairsUp.x, dung.stairsUp.y));

        int base = isStart ? 0 : 1;
        int n = rng.range(0, base + (level_ >= 3 ? 2 : 1));

        if (r.type == RoomType::Lair && !isStart) {
            // Pack spawns
            int pack = rng.range(3, 5);
            int gid = nextGroup++;
            for (int k = 0; k < pack; ++k) {
                Vec2i p = randomFreeTileInRoom(r);
                addMonster(EntityKind::Wolf, p, gid);
            }
            continue;
        }

        for (int m = 0; m < n; ++m) {
            Vec2i p = randomFreeTileInRoom(r);
            // Choose kind based on level.
            int roll = rng.range(0, 99);
            EntityKind k = EntityKind::Goblin;

            if (level_ <= 1) {
                if (roll < 45) k = EntityKind::Goblin;
                else if (roll < 70) k = EntityKind::Bat;
                else if (roll < 90) k = EntityKind::Slime;
                else k = EntityKind::KoboldSlinger;
            } else if (level_ == 2) {
                if (roll < 30) k = EntityKind::Goblin;
                else if (roll < 55) k = EntityKind::KoboldSlinger;
                else if (roll < 75) k = EntityKind::SkeletonArcher;
                else if (roll < 90) k = EntityKind::Slime;
                else k = EntityKind::Orc;
            } else {
                if (roll < 25) k = EntityKind::Orc;
                else if (roll < 45) k = EntityKind::SkeletonArcher;
                else if (roll < 65) k = EntityKind::Goblin;
                else if (roll < 80) k = EntityKind::KoboldSlinger;
                else if (roll < 92) k = EntityKind::Slime;
                else k = EntityKind::Bat;
            }

            addMonster(k, p, 0);
        }

        // Treasure rooms get a guardian sometimes.
        if (r.type == RoomType::Treasure && !isStart && rng.chance(0.6f)) {
            Vec2i p = randomFreeTileInRoom(r);
            addMonster(level_ >= 3 ? EntityKind::Orc : EntityKind::Goblin, p, 0);
        }
    }
}

void Game::spawnItems() {
    const auto& rooms = dung.rooms;
    if (rooms.empty()) return;

    auto dropItemAt = [&](ItemKind k, Vec2i pos, int count = 1) {
        Item it;
        it.id = nextItemId++;
        it.kind = k;
        it.count = std::max(1, count);
        it.spriteSeed = rng.nextU32();
        if (k == ItemKind::WandSparks) it.charges = itemDef(k).maxCharges;

        GroundItem gi;
        gi.item = it;
        gi.pos = pos;
        ground.push_back(gi);
    };

    for (const Room& r : rooms) {
        Vec2i p = randomFreeTileInRoom(r);

        if (r.type == RoomType::Treasure) {
            dropItemAt(ItemKind::Gold, p, rng.range(15, 40));
            // One good item
            int roll = rng.range(0, 99);
            if (roll < 35) dropItemAt(ItemKind::Sword, randomFreeTileInRoom(r));
            else if (roll < 55) dropItemAt(ItemKind::ChainArmor, randomFreeTileInRoom(r));
            else if (roll < 75) dropItemAt(ItemKind::WandSparks, randomFreeTileInRoom(r));
            else dropItemAt(ItemKind::PotionHealing, randomFreeTileInRoom(r), 2);
            continue;
        }

        if (r.type == RoomType::Shrine) {
            dropItemAt(ItemKind::PotionHealing, p, rng.range(1, 2));
            if (rng.chance(0.5f)) dropItemAt(ItemKind::ScrollTeleport, randomFreeTileInRoom(r), 1);
            if (rng.chance(0.4f)) dropItemAt(ItemKind::Gold, randomFreeTileInRoom(r), rng.range(6, 18));
            continue;
        }

        if (r.type == RoomType::Lair) {
            if (rng.chance(0.4f)) dropItemAt(ItemKind::Rock, p, rng.range(3, 7));
            continue;
        }

        // Normal rooms: small chance for loot
        if (rng.chance(0.30f)) {
            int roll = rng.range(0, 99);
            if (roll < 30) dropItemAt(ItemKind::Gold, p, rng.range(3, 10));
            else if (roll < 50) dropItemAt(ItemKind::PotionHealing, p, 1);
            else if (roll < 70) dropItemAt(ItemKind::Arrow, p, rng.range(4, 10));
            else if (roll < 80) dropItemAt(ItemKind::Dagger, p, 1);
            else if (roll < 90) dropItemAt(ItemKind::LeatherArmor, p, 1);
            else dropItemAt(ItemKind::ScrollTeleport, p, 1);
        }
    }

    // A little extra ammo somewhere on the map.
    if (rng.chance(0.7f)) {
        Vec2i pos = dung.randomFloor(rng, true);
        if (!entityAt(pos.x, pos.y)) {
            dropItemAt(ItemKind::Arrow, pos, rng.range(6, 12));
        }
    }
}

void Game::monsterTurn() {
    if (gameOver) return;

    const Entity& p = player();
    const int W = dung.width;
    const int H = dung.height;

    // Build distance map from player (passable tiles).
    std::vector<int> dist(static_cast<size_t>(W * H), -1);
    auto idx = [&](int x, int y) { return y * W + x; };

    std::deque<Vec2i> q;
    dist[static_cast<size_t>(idx(p.pos.x, p.pos.y))] = 0;
    q.push_back(p.pos);

    const int dirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};

    while (!q.empty()) {
        Vec2i cur = q.front();
        q.pop_front();
        int cd = dist[static_cast<size_t>(idx(cur.x, cur.y))];
        for (auto& dv : dirs) {
            int nx = cur.x + dv[0];
            int ny = cur.y + dv[1];
            if (!dung.inBounds(nx, ny)) continue;
            if (!dung.isPassable(nx, ny)) continue;
            if (dist[static_cast<size_t>(idx(nx, ny))] != -1) continue;
            dist[static_cast<size_t>(idx(nx, ny))] = cd + 1;
            q.push_back({nx, ny});
        }
    }

    // Helper to choose move based on dist map.
    auto stepToward = [&](const Entity& m) -> Vec2i {
        Vec2i best = m.pos;
        int bestD = 1e9;
        for (auto& dv : dirs) {
            int nx = m.pos.x + dv[0];
            int ny = m.pos.y + dv[1];
            if (!dung.inBounds(nx, ny)) continue;
            if (!dung.isPassable(nx, ny)) continue;
            if (entityAt(nx, ny) && !(nx == p.pos.x && ny == p.pos.y)) continue;
            int d0 = dist[static_cast<size_t>(idx(nx, ny))];
            if (d0 >= 0 && d0 < bestD) {
                bestD = d0;
                best = {nx, ny};
            }
        }
        return best;
    };

    auto stepAway = [&](const Entity& m) -> Vec2i {
        Vec2i best = m.pos;
        int bestD = -1;
        for (auto& dv : dirs) {
            int nx = m.pos.x + dv[0];
            int ny = m.pos.y + dv[1];
            if (!dung.inBounds(nx, ny)) continue;
            if (!dung.isPassable(nx, ny)) continue;
            if (entityAt(nx, ny)) continue;
            int d0 = dist[static_cast<size_t>(idx(nx, ny))];
            if (d0 >= 0 && d0 > bestD) {
                bestD = d0;
                best = {nx, ny};
            }
        }
        return best;
    };

    for (auto& m : ents) {
        if (m.id == playerId_) continue;
        if (m.hp <= 0) continue;

        int d0 = dist[static_cast<size_t>(idx(m.pos.x, m.pos.y))];
        int man = manhattan(m.pos, p.pos);

        bool seesPlayer = false;
        if (man <= 12) {
            seesPlayer = dung.hasLineOfSight(m.pos.x, m.pos.y, p.pos.x, p.pos.y);
        }

        if (seesPlayer) m.alerted = true;

        if (!m.alerted) {
            // Idle wander
            float wanderChance = (m.kind == EntityKind::Bat) ? 0.65f : 0.25f;
            if (rng.chance(wanderChance)) {
                int di = rng.range(0, 3);
                tryMove(m, dirs[di][0], dirs[di][1]);
            }
            continue;
        }

        // If adjacent, melee attack.
        if (isAdjacent4(m.pos, p.pos)) {
            Entity& pm = playerMut();
            attackMelee(m, pm);
            continue;
        }

        // Fleeing behavior
        if (m.willFlee && m.hp <= std::max(1, m.hpMax / 3) && d0 >= 0) {
            Vec2i to = stepAway(m);
            if (to != m.pos) {
                tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
            }
            continue;
        }

        // Ranged behavior
        if (m.canRanged && seesPlayer && man <= m.rangedRange) {
            // If too close, step back a bit
            if (man <= 2 && d0 >= 0) {
                Vec2i to = stepAway(m);
                if (to != m.pos) {
                    tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
                    continue;
                }
            }

            attackRanged(m, p.pos, m.rangedRange, m.rangedAtk, m.rangedProjectile, false);
            continue;
        }

        // Pack behavior: try to occupy adjacent tiles around player
        if (m.packAI) {
            // If any adjacent tile is free, take it.
            Vec2i bestAdj = m.pos;
            bool found = false;
            for (auto& dv : dirs) {
                int ax = p.pos.x + dv[0];
                int ay = p.pos.y + dv[1];
                if (!dung.inBounds(ax, ay)) continue;
                if (!dung.isPassable(ax, ay)) continue;
                if (entityAt(ax, ay)) continue;
                // Prefer closer-to-monster candidate
                if (!found || manhattan({ax, ay}, m.pos) < manhattan(bestAdj, m.pos)) {
                    bestAdj = {ax, ay};
                    found = true;
                }
            }
            if (found) {
                Vec2i lineStep = stepToward(m); // fallback
                // Move toward chosen adjacent tile using a greedy step
                std::vector<Vec2i> path = bresenhamLine(m.pos, bestAdj);
                if (path.size() > 1) {
                    Vec2i step = path[1];
                    tryMove(m, step.x - m.pos.x, step.y - m.pos.y);
                    continue;
                }
                if (lineStep != m.pos) {
                    tryMove(m, lineStep.x - m.pos.x, lineStep.y - m.pos.y);
                    continue;
                }
            }
        }

        // Default: step toward using dist map
        if (d0 >= 0) {
            Vec2i to = stepToward(m);
            if (to != m.pos) {
                tryMove(m, to.x - m.pos.x, to.y - m.pos.y);
            }
        }
    }
}

void Game::cleanupDead() {
    // Drop loot from dead monsters (before removal)
    for (const auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp > 0) continue;

        // Simple drops
        if (rng.chance(0.55f)) {
            GroundItem gi;
            gi.pos = e.pos;
            gi.item.id = nextItemId++;
            gi.item.spriteSeed = rng.nextU32();

            int roll = rng.range(0, 99);
            if (roll < 45) { gi.item.kind = ItemKind::Gold; gi.item.count = rng.range(2, 8); }
            else if (roll < 60) { gi.item.kind = ItemKind::Arrow; gi.item.count = rng.range(3, 7); }
            else if (roll < 70) { gi.item.kind = ItemKind::Rock; gi.item.count = rng.range(2, 6); }
            else if (roll < 80) { gi.item.kind = ItemKind::PotionHealing; gi.item.count = 1; }
            else if (roll < 88) { gi.item.kind = ItemKind::Dagger; gi.item.count = 1; }
            else { gi.item.kind = ItemKind::ScrollTeleport; gi.item.count = 1; }

            ground.push_back(gi);
        }
    }

    // Remove dead monsters
    ents.erase(std::remove_if(ents.begin(), ents.end(), [&](const Entity& e) {
        return (e.id != playerId_) && (e.hp <= 0);
    }), ents.end());

    // Player death handled in attack functions
}

