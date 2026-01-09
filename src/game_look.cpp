#include "game_internal.hpp"

#include "hallucination.hpp"

void Game::beginLook() {
    // Close other overlays
    invOpen = false;
    closeChestOverlay();
    targeting = false;
    helpOpen = false;
    minimapOpen = false;
    statsOpen = false;
    codexOpen = false;
    msgScroll = 0;

    looking = true;
    lookPos = player().pos;
}

void Game::endLook() {
    looking = false;
}

void Game::beginLookAt(Vec2i p) {
    beginLook();
    setLookCursor(p);
}

void Game::setLookCursor(Vec2i p) {
    if (!looking) return;
    p.x = clampi(p.x, 0, MAP_W - 1);
    p.y = clampi(p.y, 0, MAP_H - 1);
    lookPos = p;
}

void Game::setTargetCursor(Vec2i p) {
    if (!targeting) return;
    p.x = clampi(p.x, 0, MAP_W - 1);
    p.y = clampi(p.y, 0, MAP_H - 1);
    targetPos = p;
    recomputeTargetLine();
}

void Game::moveLookCursor(int dx, int dy) {
    if (!looking) return;
    Vec2i p = lookPos;
    p.x = clampi(p.x + dx, 0, MAP_W - 1);
    p.y = clampi(p.y + dy, 0, MAP_H - 1);
    lookPos = p;
}

std::string Game::describeAt(Vec2i p) const {
    if (!dung.inBounds(p.x, p.y)) return "OUT OF BOUNDS";

    const Tile& t = dung.at(p.x, p.y);
    if (!t.explored) {
        return "UNKNOWN";
    }

    std::ostringstream ss;

    const bool hallu = isHallucinating(*this);

    // Base tile description
    switch (t.type) {
        case TileType::Wall: ss << "WALL"; break;
        case TileType::DoorSecret: ss << "WALL"; break; // don't spoil undiscovered secrets
        case TileType::Pillar: ss << "PILLAR"; break;
        case TileType::Boulder: ss << "BOULDER"; break;
        case TileType::Chasm: ss << "CHASM"; break;
        case TileType::Floor: ss << "FLOOR"; break;
        case TileType::StairsUp: ss << "STAIRS UP"; break;
        case TileType::StairsDown: ss << "STAIRS DOWN"; break;
        case TileType::DoorClosed: ss << "DOOR (CLOSED)"; break;
        case TileType::DoorLocked: ss << "DOOR (LOCKED)"; break;
        case TileType::DoorOpen: ss << "DOOR (OPEN)"; break;
        default: ss << "TILE"; break;
    }

    // Trap (can be remembered once discovered)
    for (const auto& tr : trapsCur) {
        if (!tr.discovered) continue;
        if (tr.pos.x != p.x || tr.pos.y != p.y) continue;
        ss << " | TRAP: ";
        switch (tr.kind) {
            case TrapKind::Spike: ss << "SPIKE"; break;
            case TrapKind::PoisonDart: ss << "POISON DART"; break;
            case TrapKind::Teleport: ss << "TELEPORT"; break;
            case TrapKind::Alarm: ss << "ALARM"; break;
            case TrapKind::Web: ss << "WEB"; break;
            case TrapKind::ConfusionGas: ss << "CONFUSION GAS"; break;
            case TrapKind::RollingBoulder: ss << "ROLLING BOULDER"; break;
            case TrapKind::TrapDoor: ss << "TRAP DOOR"; break;
            case TrapKind::LetheMist: ss << "LETHE MIST"; break;
            case TrapKind::PoisonGas: ss << "POISON GAS"; break;
        }
        break;
    }

    // Environmental fields (only if currently visible).
    if (t.visible) {
        if (confusionGasAt(p.x, p.y) > 0u) ss << " | CONFUSION GAS";
        if (poisonGasAt(p.x, p.y) > 0u) ss << " | POISON GAS";
        if (fireAt(p.x, p.y) > 0u) ss << " | FIRE";
    }

    // Player map marker / note (persistent on this floor).
    if (const MapMarker* mm = markerAt(p)) {
        ss << " | MARK: " << markerKindName(mm->kind) << " \"" << mm->label << "\"";
    }

    // Floor engraving / graffiti (persistent on this floor).
    if (const Engraving* eg = engravingAt(p)) {
        std::string sk;
        if (engravingIsSigil(*eg, &sk)) {
            ss << " | SIGIL: \"" << eg->text << "\"";
            if (eg->strength != 255) {
                const int uses = static_cast<int>(eg->strength);
                ss << " (" << uses << " USE" << (uses == 1 ? "" : "S") << " LEFT)";
            }
        } else {
            ss << " | ENGRAVING: \"" << eg->text << "\"";
            if (eg->isWard) ss << " (WARD)";
        }
    }

    // Entities/items: only if currently visible.
    if (t.visible) {
        if (const Entity* e = entityAt(p.x, p.y)) {
            if (e->id == playerId_) {
                ss << " | YOU";
            } else {
                const EntityKind showKind = hallucinatedEntityKind(*this, *e);
                std::string label = kindName(showKind);
                if (e->friendly) {
                    label += " (ALLY";
                    switch (e->allyOrder) {
                        case AllyOrder::Stay:  label += ", STAY"; break;
                        case AllyOrder::Fetch: label += ", FETCH"; break;
                        case AllyOrder::Guard: label += ", GUARD"; break;
                        case AllyOrder::Follow:
                        default: break;
                    }
                    label += ")";
                }
                ss << " | " << label << " " << e->hp << "/" << e->hpMax;

                // Codex (per-run) stats: kills by kind + XP value.
                const uint16_t kindKills = codexKills(showKind);
                if (kindKills > 0) {
                    ss << " | KILLS: " << kindKills;
                }
                ss << " | XP: " << xpFor(showKind);

                if (showKind == EntityKind::Ghost) {
                    ss << " | ETHEREAL";
                }

                if (e->stolenGold > 0) {
                    if (showKind == EntityKind::Leprechaun) {
                        ss << " | STOLEN: " << e->stolenGold << "G";
                    } else if (e->friendly) {
                        ss << " | CARRY: " << e->stolenGold << "G";
                    } else {
                        ss << " | LOOT: " << e->stolenGold << "G";
                    }
                }

                
                if (!hallu && e->friendly && e->pocketConsumable.id != 0 && e->pocketConsumable.count > 0) {
                    ss << " | PACK: " << displayItemName(e->pocketConsumable);
                }

if (e->effects.fearTurns > 0) {
                    ss << " | FEARED";
                }

                // Don't leak extra information while hallucinating: the player is already
                // being shown a distorted creature type.
                if (!hallu) {
                    if (e->gearMelee.id != 0 && isMeleeWeapon(e->gearMelee.kind)) {
                        ss << " | WPN: " << itemDisplayNameSingle(e->gearMelee.kind);
                    }
                    if (e->gearArmor.id != 0 && isArmor(e->gearArmor.kind)) {
                        ss << " | ARM: " << itemDisplayNameSingle(e->gearArmor.kind);
                    }
                }

            }
        }

        // Items (show first one + count)
        int itemCount = 0;
        const GroundItem* first = nullptr;
        for (const auto& gi : ground) {
            if (gi.pos.x == p.x && gi.pos.y == p.y) {
                ++itemCount;
                if (!first) first = &gi;
            }
        }
        if (itemCount > 0 && first) {
            Item showItem = first->item;
            if (hallu) {
                showItem.kind = hallucinatedItemKind(*this, first->item);
            }

            std::string itemLabel = displayItemName(showItem);

            // Chest metadata is deliberately suppressed while hallucinating: it would otherwise
            // reveal the true underlying object even if the player "sees" something else.
            if (!hallu) {
                if (first->item.kind == ItemKind::Chest) {
                    if (chestLocked(first->item)) itemLabel += " (LOCKED)";
                    if (chestTrapped(first->item) && chestTrapKnown(first->item)) itemLabel += " (TRAPPED)";
                } else if (first->item.kind == ItemKind::ChestOpen) {
                    int stacks = 0;
                    for (const auto& c : chestContainers_) {
                        if (c.chestId == first->item.id) { stacks = static_cast<int>(c.items.size()); break; }
                    }
                    const int tier = chestTier(first->item);
                    const int limit = chestStackLimitForTier(tier);
                    std::stringstream cs;
                    cs << " (" << chestTierName(tier) << " " << stacks << "/" << limit << ")";
                    itemLabel += cs.str();
                }
            }
            ss << " | ITEM: " << itemLabel;
            if (itemCount > 1) ss << " (+" << (itemCount - 1) << ")";
        }
    }

    // Distance (Manhattan for clarity)
    Vec2i pp = player().pos;
    int dist = std::abs(p.x - pp.x) + std::abs(p.y - pp.y);
    ss << " | DIST " << dist;

    return ss.str();
}

std::string Game::lookInfoText() const {
    if (!looking) return std::string();
    return describeAt(lookPos);
}

void Game::restUntilSafe() {
    if (isFinished()) return;
    if (inputLock) return;

    // If nothing to do, don't burn time.
    if (player().hp >= player().hpMax) {
        pushMsg("YOU ARE ALREADY AT FULL HEALTH.", MessageKind::System, true);
        return;
    }

    // Resting while standing in fire (or actively burning) is a great way to die.
    if (player().effects.burnTurns > 0 || fireAt(player().pos.x, player().pos.y) > 0u) {
        pushMsg("YOU CAN'T REST WHILE ON FIRE!", MessageKind::Warning, true);
        return;
    }

    pushMsg("YOU REST...", MessageKind::Info, true);

    // Safety valve to prevent accidental infinite loops.
    const int maxSteps = 2000;
    int steps = 0;
    while (!isFinished() && steps < maxSteps) {
        if (anyVisibleHostiles()) {
            pushMsg("REST INTERRUPTED!", MessageKind::Warning, true);
            break;
        }
        if (player().hp >= player().hpMax) {
            pushMsg("YOU FEEL RESTED.", MessageKind::Success, true);
            break;
        }

        // Consume a "wait" turn without spamming the log.
        advanceAfterPlayerAction();
        ++steps;
    }
}


int Game::repeatSearch(int maxTurns, bool stopOnFind) {
    if (isFinished()) return 0;
    if (inputLock) return 0;

    if (maxTurns <= 0) return 0;
    maxTurns = clampi(maxTurns, 1, 2000);

    // Cancel auto-move to avoid fighting the stepper.
    if (autoMode != AutoMoveMode::None) {
        stopAutoMove(true);
    }

    // Single-turn: behave exactly like the normal Search action.
    if (maxTurns == 1) {
        (void)searchForTraps(true);
        advanceAfterPlayerAction();
        return 1;
    }

    // Repeated searching is usually only safe when no hostiles are visible.
    if (anyVisibleHostiles()) {
        pushMsg("TOO DANGEROUS TO SEARCH REPEATEDLY!", MessageKind::Warning, true);
        return 0;
    }

    pushMsg("YOU SEARCH...", MessageKind::Info, true);

    int steps = 0;
    int totalFoundTraps = 0;
    int totalFoundSecrets = 0;
    bool foundAny = false;
    bool interrupted = false;

    while (!isFinished() && steps < maxTurns) {
        // Abort if something hostile comes into view.
        if (anyVisibleHostiles()) {
            pushMsg("SEARCH INTERRUPTED!", MessageKind::Warning, true);
            interrupted = true;
            break;
        }

        int ft = 0;
        int fs = 0;
        (void)searchForTraps(false, &ft, &fs);

        totalFoundTraps += ft;
        totalFoundSecrets += fs;

        if (ft > 0 || fs > 0) {
            foundAny = true;
            if (stopOnFind) {
                // Report the discovery immediately (before monsters act), like normal search.
                pushMsg(formatSearchDiscoveryMessage(ft, fs), MessageKind::Info, true);
            }
        }

        advanceAfterPlayerAction();
        ++steps;

        if (foundAny && stopOnFind) break;
    }

    if (!isFinished()) {
        if (foundAny && !stopOnFind) {
            pushMsg(formatSearchDiscoveryMessage(totalFoundTraps, totalFoundSecrets), MessageKind::Info, true);
        } else if (!foundAny && !interrupted) {
            pushMsg("YOU FIND NOTHING.", MessageKind::Info, true);
        }
    }

    return steps;
}

