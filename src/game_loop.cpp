#include "game_internal.hpp"

void Game::update(float dt) {
    // Animate FX projectiles.
    if (!fx.empty()) {
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

    // Animate explosion flashes.
    if (!fxExpl.empty()) {
        for (auto& ex : fxExpl) {
            if (ex.delay > 0.0f) {
                ex.delay = std::max(0.0f, ex.delay - dt);
            } else {
                ex.timer += dt;
            }
        }
        fxExpl.erase(std::remove_if(fxExpl.begin(), fxExpl.end(), [](const FXExplosion& ex) {
            return ex.delay <= 0.0f && ex.timer >= ex.duration;
        }), fxExpl.end());
    }

    // Lock input while any FX are active.
    inputLock = (!fx.empty() || !fxExpl.empty());

    // Auto-move (travel / explore) steps are processed here to keep the game turn-based
    // while still providing smooth-ish movement.
    if (autoMode != AutoMoveMode::None) {
        // If the player opened an overlay, stop (don't keep walking while in menus).
        if (invOpen || targeting || helpOpen || looking || minimapOpen || statsOpen || levelUpOpen || optionsOpen || commandOpen || isFinished()) {
            stopAutoMove(true);
            return;
        }

        if (!inputLock) {
            autoStepTimer += dt;
            if (autoStepTimer >= autoStepDelay) {
                autoStepTimer = 0.0f;
                (void)stepAutoMove();
            }
        }
    }
}

void Game::handleAction(Action a) {
    if (a == Action::None) return;

    // Any manual action stops auto-move (except log scrolling).
    if (autoMode != AutoMoveMode::None && a != Action::LogUp && a != Action::LogDown) {
        stopAutoMove(true);
    }

    // Message log scroll works in any mode.
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

    auto closeOverlays = [&]() {
        invOpen = false;
        invIdentifyMode = false;
        targeting = false;
        helpOpen = false;
        looking = false;
        minimapOpen = false;
        statsOpen = false;
        optionsOpen = false;

        if (commandOpen) {
            commandOpen = false;
            commandBuf.clear();
            commandDraft.clear();
            commandHistoryPos = -1;
        }

        msgScroll = 0;
    };

    // ------------------------------------------------------------
    // Modal inventory prompt: selecting an item for Scroll of Identify
    // ------------------------------------------------------------
    // This runs *before* global hotkeys so the prompt can't be dismissed by opening other overlays.
    if (invOpen && invIdentifyMode) {
        auto candidates = [&]() {
            std::vector<ItemKind> out;
            out.reserve(16);
            auto seen = [&](ItemKind k) {
                for (ItemKind x : out) if (x == k) return true;
                return false;
            };

            for (const auto& invIt : inv) {
                if (!isIdentifiableKind(invIt.kind)) continue;
                if (invIt.kind == ItemKind::ScrollIdentify) continue;
                if (isIdentified(invIt.kind)) continue;
                if (!seen(invIt.kind)) out.push_back(invIt.kind);
            }
            return out;
        };

        auto identifyRandom = [&]() {
            std::vector<ItemKind> c = candidates();
            if (c.empty()) {
                pushMsg("YOU LEARN NOTHING NEW.", MessageKind::Info, true);
                return;
            }
            const int idx = rng.range(0, static_cast<int>(c.size()) - 1);
            (void)markIdentified(c[static_cast<size_t>(idx)], false);
        };

        switch (a) {
            case Action::Up:
                moveInventorySelection(-1);
                break;
            case Action::Down:
                moveInventorySelection(1);
                break;
            case Action::SortInventory:
                sortInventory();
                break;
            case Action::Confirm: {
                if (inv.empty()) {
                    invIdentifyMode = false;
                    break;
                }
                invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
                const Item& selIt = inv[static_cast<size_t>(invSel)];

                if (!isIdentifiableKind(selIt.kind) || selIt.kind == ItemKind::ScrollIdentify || isIdentified(selIt.kind)) {
                    pushMsg("THAT DOESN'T TEACH YOU ANYTHING.", MessageKind::Info, true);
                    break;
                }

                (void)markIdentified(selIt.kind, false);
                invIdentifyMode = false;
                break;
            }
            case Action::Cancel:
            case Action::Inventory:
                // Treat cancel as "pick randomly" to preserve the old (random) behavior.
                identifyRandom();
                closeInventory();
                break;
            default:
                // Ignore other actions while the prompt is active.
                break;
        }
        return;
    }

    // ------------------------------------------------------------
    // Level-up talent allocation overlay (forced while points are pending)
    // ------------------------------------------------------------
    if (levelUpOpen && talentPointsPending_ > 0) {
        // Allow save/load even while allocation is pending.
        if (a == Action::Save) { (void)saveToFile(defaultSavePath()); return; }
        if (a == Action::Load) { (void)loadFromFile(defaultSavePath()); return; }
        if (a == Action::LoadAuto) { (void)loadFromFile(defaultAutosavePath()); return; }

        auto spendOne = [&]() {
            if (talentPointsPending_ <= 0) return;

            Entity& p = playerMut();

            switch (levelUpSel) {
                case 0: // Might
                    talentMight_ += 1;
                    pushMsg("MIGHT INCREASES.", MessageKind::Success, true);
                    break;
                case 1: // Agility
                    talentAgility_ += 1;
                    pushMsg("AGILITY INCREASES.", MessageKind::Success, true);
                    break;
                case 2: // Vigor
                    talentVigor_ += 1;
                    p.hpMax += 2;
                    p.hp = std::min(p.hpMax, p.hp + 2);
                    pushMsg("VIGOR INCREASES. +2 MAX HP.", MessageKind::Success, true);
                    break;
                case 3: // Focus
                default:
                    talentFocus_ += 1;
                    pushMsg("FOCUS INCREASES.", MessageKind::Success, true);
                    break;
            }

            talentPointsPending_ -= 1;
            if (talentPointsPending_ <= 0) {
                talentPointsPending_ = 0;
                levelUpOpen = false;
                pushMsg("TALENT CHOSEN.", MessageKind::System, true);
            }
        };

        switch (a) {
            case Action::Up:
                levelUpSel = (levelUpSel + 3) % 4;
                break;
            case Action::Down:
                levelUpSel = (levelUpSel + 1) % 4;
                break;
            case Action::Confirm:
                spendOne();
                break;
            case Action::Cancel:
                // Convenience: ESC spends all remaining points on the highlighted talent.
                while (talentPointsPending_ > 0) spendOne();
                break;
            default:
                // Ignore other inputs while allocation is pending.
                break;
        }
        return;
    } else if (levelUpOpen && talentPointsPending_ <= 0) {
        levelUpOpen = false;
    }

    // Global hotkeys (available even while dead/won).
    switch (a) {
        case Action::Save:
            (void)saveToFile(defaultSavePath());
            return;
        case Action::Load:
            (void)loadFromFile(defaultSavePath());
            return;
        case Action::LoadAuto:
            (void)loadFromFile(defaultAutosavePath());
            return;
        case Action::Help:
            // Toggle help overlay.
            helpOpen = !helpOpen;
            if (helpOpen) {
                closeOverlays();
                helpOpen = true;
            }
            return;
        case Action::ToggleMinimap:
            if (minimapOpen) {
                minimapOpen = false;
            } else {
                closeOverlays();
                minimapOpen = true;
            }
            return;
        case Action::ToggleStats:
            if (statsOpen) {
                statsOpen = false;
            } else {
                closeOverlays();
                statsOpen = true;
            }
            return;
        case Action::Options:
            if (optionsOpen) {
                optionsOpen = false;
            } else {
                closeOverlays();
                optionsOpen = true;
                optionsSel = 0;
            }
            return;
        case Action::Command:
            if (commandOpen) {
                commandOpen = false;
                commandBuf.clear();
                commandDraft.clear();
                commandHistoryPos = -1;
            } else {
                closeOverlays();
                commandOpen = true;
                commandBuf.clear();
                commandDraft.clear();
                commandHistoryPos = -1;
            }
            return;
        default:
            break;
    }

    // Toggle auto-pickup (safe to do in any non-finished state).
    if (a == Action::ToggleAutoPickup) {
        switch (autoPickup) {
            case AutoPickupMode::Off:   autoPickup = AutoPickupMode::Gold;  break;
            case AutoPickupMode::Gold:  autoPickup = AutoPickupMode::Smart; break;
            case AutoPickupMode::Smart: autoPickup = AutoPickupMode::All;   break;
            case AutoPickupMode::All:   autoPickup = AutoPickupMode::Off;   break;
            default:                    autoPickup = AutoPickupMode::Gold;  break;
        }

        settingsDirtyFlag = true;

        const char* mode =
            (autoPickup == AutoPickupMode::Off)   ? "OFF" :
            (autoPickup == AutoPickupMode::Gold)  ? "GOLD" :
            (autoPickup == AutoPickupMode::Smart) ? "SMART" : "ALL";

        std::string msg = std::string("AUTO-PICKUP: ") + mode + ".";
        pushMsg(msg, MessageKind::System);
        return;
    }

    // Auto-explore request.
    if (a == Action::AutoExplore) {
        requestAutoExplore();
        return;
    }

    // Overlay: extended command prompt (does not consume turns)
    if (commandOpen) {
        if (a == Action::Cancel || a == Action::Command) {
            commandOpen = false;
            commandBuf.clear();
            commandDraft.clear();
            commandHistoryPos = -1;
            return;
        }

        if (a == Action::Confirm) {
            std::string line = trim(commandBuf);
            commandOpen = false;
            commandBuf.clear();
            commandDraft.clear();
            commandHistoryPos = -1;

            if (!line.empty()) {
                // Store history (keep it small).
                if (commandHistory.empty() || commandHistory.back() != line) {
                    commandHistory.push_back(line);
                    if (commandHistory.size() > 50) {
                        commandHistory.erase(commandHistory.begin());
                    }
                }
                runExtendedCommand(*this, line);
            }
            return;
        }

        if (a == Action::Up) {
            if (!commandHistory.empty()) {
                if (commandHistoryPos < 0) {
                    commandDraft = commandBuf;
                    commandHistoryPos = static_cast<int>(commandHistory.size()) - 1;
                } else {
                    commandHistoryPos = std::max(0, commandHistoryPos - 1);
                }
                commandBuf = commandHistory[commandHistoryPos];
            }
            return;
        }

        if (a == Action::Down) {
            if (commandHistoryPos >= 0) {
                if (commandHistoryPos + 1 < static_cast<int>(commandHistory.size())) {
                    ++commandHistoryPos;
                    commandBuf = commandHistory[commandHistoryPos];
                } else {
                    commandHistoryPos = -1;
                    commandBuf = commandDraft;
                    commandDraft.clear();
                }
            }
            return;
        }

        // Ignore any other actions while the prompt is open.
        return;
    }

    // Overlay: options menu (does not consume turns)
    if (optionsOpen) {
        constexpr int kOptionCount = 14;

        if (a == Action::Cancel || a == Action::Options) {
            optionsOpen = false;
            return;
        }

        if (a == Action::Up) {
            optionsSel = clampi(optionsSel - 1, 0, kOptionCount - 1);
            return;
        }
        if (a == Action::Down) {
            optionsSel = clampi(optionsSel + 1, 0, kOptionCount - 1);
            return;
        }

        const bool left = (a == Action::Left);
        const bool right = (a == Action::Right);
        const bool confirm = (a == Action::Confirm);

        auto cycleAutoPickup = [&](int dir) {
            const AutoPickupMode order[4] = { AutoPickupMode::Off, AutoPickupMode::Gold, AutoPickupMode::Smart, AutoPickupMode::All };
            int idx = 0;
            for (int i = 0; i < 4; ++i) {
                if (order[i] == autoPickup) { idx = i; break; }
            }
            idx = (idx + dir) % 4;
            if (idx < 0) idx += 4;
            autoPickup = order[idx];
            settingsDirtyFlag = true;
        };

        // 0) Auto-pickup
        if (optionsSel == 0) {
            if (left) cycleAutoPickup(-1);
            else if (right || confirm) cycleAutoPickup(+1);
            return;
        }

        // 1) Auto-step delay
        if (optionsSel == 1) {
            if (left || right) {
                int ms = autoStepDelayMs();
                ms += left ? -5 : +5;
                ms = clampi(ms, 10, 500);
                setAutoStepDelayMs(ms);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 2) Autosave interval
        if (optionsSel == 2) {
            if (left || right) {
                int t = autosaveInterval;
                t += left ? -50 : +50;
                t = clampi(t, 0, 5000);
                setAutosaveEveryTurns(t);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 3) Identification helper
        if (optionsSel == 3) {
            if (left || right || confirm) {
                setIdentificationEnabled(!identifyItemsEnabled);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 4) Hunger system
        if (optionsSel == 4) {
            if (left || right || confirm) {
                setHungerEnabled(!hungerEnabled_);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 5) Encumbrance system
        if (optionsSel == 5) {
            if (left || right || confirm) {
                setEncumbranceEnabled(!encumbranceEnabled_);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 6) Lighting / darkness
        if (optionsSel == 6) {
            if (left || right || confirm) {
                setLightingEnabled(!lightingEnabled_);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 7) Effect timers (HUD)
        if (optionsSel == 7) {
            if (left || right || confirm) {
                showEffectTimers_ = !showEffectTimers_;
                settingsDirtyFlag = true;
            }
            return;
        }

        // 8) Confirm quit (double-ESC)
        if (optionsSel == 8) {
            if (left || right || confirm) {
                confirmQuitEnabled_ = !confirmQuitEnabled_;
                settingsDirtyFlag = true;
            }
            return;
        }

        // 8) Auto mortem (write a dump file on win/death)
        if (optionsSel == 9) {
            if (left || right || confirm) {
                autoMortemEnabled_ = !autoMortemEnabled_;
                settingsDirtyFlag = true;
            }
            return;
        }

        // 10) Save backups (0..10)
        if (optionsSel == 10) {
            if (left || right) {
                int n = saveBackups_;
                n += left ? -1 : +1;
                setSaveBackups(n);
                settingsDirtyFlag = true;
            }
            return;
        }

// 11) UI Theme (cycle)
if (optionsSel == 11) {
    if (left || right || confirm) {
        int dir = right ? 1 : -1;
        if (confirm && !left && !right) dir = 1;

        int ti = static_cast<int>(uiTheme_);
        ti = (ti + dir + 3) % 3;
        uiTheme_ = static_cast<UITheme>(ti);
        settingsDirtyFlag = true;
    }
    return;
}

// 11) UI Panels (textured / solid)
if (optionsSel == 12) {
    if (left || right || confirm) {
        uiPanelsTextured_ = !uiPanelsTextured_;
        settingsDirtyFlag = true;
    }
    return;
}

// 13) Close
if (optionsSel == 13) {
    if (left || right || confirm) optionsOpen = false;
    return;
}

        return;
    }

    // Finished runs: allow restart (and global UI hotkeys above).
    if (isFinished()) {
        if (a == Action::Restart) {
            newGame(hash32(rng.nextU32()));
        }
        return;
    }

    // If animating FX, only allow Cancel to close overlays.
    if (inputLock) {
        if (a == Action::Cancel) {
            closeOverlays();
        }
        return;
    }

    // Overlay: minimap
    if (minimapOpen) {
        if (a == Action::Cancel) minimapOpen = false;
        return;
    }

    // Overlay: stats
    if (statsOpen) {
        if (a == Action::Cancel) statsOpen = false;
        return;
    }

    // Help overlay mode.
    if (helpOpen) {
        if (a == Action::Cancel || a == Action::Inventory || a == Action::Help) {
            helpOpen = false;
        }
        return;
    }

    // Look / examine mode.
    if (looking) {
        switch (a) {
            case Action::Up:        moveLookCursor(0, -1); break;
            case Action::Down:      moveLookCursor(0, 1); break;
            case Action::Left:      moveLookCursor(-1, 0); break;
            case Action::Right:     moveLookCursor(1, 0); break;
            case Action::UpLeft:    moveLookCursor(-1, -1); break;
            case Action::UpRight:   moveLookCursor(1, -1); break;
            case Action::DownLeft:  moveLookCursor(-1, 1); break;
            case Action::DownRight: moveLookCursor(1, 1); break;
            case Action::Inventory:
                endLook();
                openInventory();
                break;
            case Action::Fire:
                // Convenient: jump straight from look -> targeting (cursor stays where you were looking).
                {
                    Vec2i desired = lookPos;
                    endLook();
                    beginTargeting();
                    if (targeting) {
                        targetPos = desired;
                        recomputeTargetLine();
                    }
                }
                break;
            case Action::Confirm:
                // Auto-travel to the looked-at tile (doesn't consume a turn by itself).
                if (requestAutoTravel(lookPos)) {
                    endLook();
                }
                break;
            case Action::Cancel:
            case Action::Look:
                endLook();
                break;
            default:
                break;
        }
        return;
    }

    bool acted = false;

    // Inventory mode.
    if (invOpen) {
        switch (a) {
            case Action::Up: moveInventorySelection(-1); break;
            case Action::Down: moveInventorySelection(1); break;
            case Action::Inventory:
            case Action::Cancel:
                closeInventory();
                break;

            case Action::Confirm: {
                // Context action: equip if equipable, otherwise use if consumable.
                if (!inv.empty()) {
                    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
                    const Item& it = inv[static_cast<size_t>(invSel)];
                    const ItemDef& d = itemDef(it.kind);
                    if (d.slot != EquipSlot::None) acted = equipSelected();
                    else if (d.consumable) acted = useSelected();
                }
                break;
            }

            case Action::Equip:
                acted = equipSelected();
                break;
            case Action::Use:
                acted = useSelected();
                break;
            case Action::Drop:
                acted = dropSelected();
                break;
            case Action::DropAll:
                acted = dropSelectedAll();
                break;
            case Action::SortInventory:
                sortInventory();
                break;
            default:
                break;
        }

        if (acted) {
            advanceAfterPlayerAction();
        }
        return;
    }

    // Targeting mode.
    if (targeting) {
        switch (a) {
            case Action::Up:        moveTargetCursor(0, -1); break;
            case Action::Down:      moveTargetCursor(0, 1); break;
            case Action::Left:      moveTargetCursor(-1, 0); break;
            case Action::Right:     moveTargetCursor(1, 0); break;
            case Action::UpLeft:    moveTargetCursor(-1, -1); break;
            case Action::UpRight:   moveTargetCursor(1, -1); break;
            case Action::DownLeft:  moveTargetCursor(-1, 1); break;
            case Action::DownRight: moveTargetCursor(1, 1); break;
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
            advanceAfterPlayerAction();
        }
        return;
    }

    // Normal play mode.
    Entity& p = playerMut();    switch (a) {
        case Action::Up:        acted = tryMove(p, 0, -1); break;
        case Action::Down:      acted = tryMove(p, 0, 1); break;
        case Action::Left:      acted = tryMove(p, -1, 0); break;
        case Action::Right:     acted = tryMove(p, 1, 0); break;
        case Action::UpLeft:    acted = tryMove(p, -1, -1); break;
        case Action::UpRight:   acted = tryMove(p, 1, -1); break;
        case Action::DownLeft:  acted = tryMove(p, -1, 1); break;
        case Action::DownRight: acted = tryMove(p, 1, 1); break;
        case Action::Wait:
            pushMsg("YOU WAIT.", MessageKind::Info);
            acted = true;
            break;
        case Action::Search:
            acted = searchForTraps();
            break;
        case Action::Disarm:
            acted = disarmTrap();
            break;
        case Action::CloseDoor:
            acted = closeDoor();
            break;
        case Action::LockDoor:
            acted = lockDoor();
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
        case Action::Look:
            beginLook();
            acted = false;
            break;
        case Action::Rest:
            restUntilSafe();
            acted = false;
            break;
        case Action::ToggleSneak:
            toggleSneakMode();
            acted = false;
            break;
        case Action::Confirm: {
            if (p.pos == dung.stairsDown) {
                if (encumbranceEnabled_ && burdenState() == BurdenState::Overloaded) {
                    pushMsg("YOU ARE OVERLOADED!", MessageKind::Warning, true);
                } else {
                    changeLevel(depth_ + 1, true);
                }
                acted = false;
            } else if (p.pos == dung.stairsUp) {
                // At depth 1, stairs up is the exit.
                if (depth_ <= 1) {
                    if (encumbranceEnabled_ && burdenState() == BurdenState::Overloaded) {
                        pushMsg("YOU ARE OVERLOADED!", MessageKind::Warning, true);
                    } else if (playerHasAmulet()) {
                        gameWon = true;
                        if (endCause_.empty()) endCause_ = "ESCAPED WITH THE AMULET";
                        pushMsg("YOU ESCAPE WITH THE AMULET OF YENDOR!", MessageKind::Success);
                        pushMsg("VICTORY!", MessageKind::Success);
                        maybeRecordRun();
                    } else {
                        pushMsg("THE EXIT IS HERE... BUT YOU STILL NEED THE AMULET.");
                    }
                } else {
                    if (encumbranceEnabled_ && burdenState() == BurdenState::Overloaded) {
                        pushMsg("YOU ARE OVERLOADED!", MessageKind::Warning, true);
                    } else {
                        changeLevel(depth_ - 1, false);
                    }
                }
                acted = false;
            } else {
                // QoL: context action on the current tile.
                // 1) Chests (world-interactable) have priority.
                bool hasChest = false;
                bool hasPickableItem = false;
                for (const auto& gi : ground) {
                    if (gi.pos != p.pos) continue;
                    if (gi.item.kind == ItemKind::Chest) hasChest = true;
                    if (!isChestKind(gi.item.kind)) hasPickableItem = true;
                }

                if (hasChest) {
                    acted = openChestAtPlayer();
                    // If we didn't open the chest (e.g., locked and no keys/picks), still allow picking
                    // up any other items on the tile.
                    if (!acted && hasPickableItem) {
                        acted = pickupAtPlayer();
                    }
                } else if (hasPickableItem) {
                    acted = pickupAtPlayer();
                } else {
                    pushMsg("THERE IS NOTHING HERE.");
                }
            }
        } break;
        case Action::StairsDown:
            if (p.pos == dung.stairsDown) {
                if (encumbranceEnabled_ && burdenState() == BurdenState::Overloaded) {
                    pushMsg("YOU ARE OVERLOADED!", MessageKind::Warning, true);
                } else {
                    changeLevel(depth_ + 1, true);
                }
                acted = false;
            } else {
                pushMsg("THERE ARE NO STAIRS HERE.");
            }
            break;
        case Action::StairsUp:
            if (p.pos == dung.stairsUp) {
                if (encumbranceEnabled_ && burdenState() == BurdenState::Overloaded) {
                    pushMsg("YOU ARE OVERLOADED!", MessageKind::Warning, true);
                    acted = false;
                    break;
                }
                if (depth_ <= 1) {
                    if (playerHasAmulet()) {
                        gameWon = true;
                        if (endCause_.empty()) endCause_ = "ESCAPED WITH THE AMULET";
                        pushMsg("YOU ESCAPE WITH THE AMULET OF YENDOR!", MessageKind::Success);
                        pushMsg("VICTORY!", MessageKind::Success);
                        maybeRecordRun();
                    } else {
                        pushMsg("THE EXIT IS HERE... BUT YOU STILL NEED THE AMULET.");
                    }
                } else {
                    changeLevel(depth_ - 1, false);
                }
                acted = false;
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
        advanceAfterPlayerAction();
    }
}

void Game::shout() {
    if (gameOver || gameWon) return;

    // Shouting always costs a turn.
    pushMsg("YOU SHOUT!", MessageKind::Info, true);
    emitNoise(player().pos, 18);
    advanceAfterPlayerAction();
}

void Game::advanceAfterPlayerAction() {
    // One "turn" = one player action that consumes time.
    // Haste gives the player an extra action every other turn by skipping the monster turn.
    ++turnCount;

    if (isFinished()) {
        // Don't let monsters act after a decisive player action.
        cleanupDead();
        recomputeFov();
        maybeRecordRun();
        return;
    }

    Entity& p = playerMut();
    bool runMonsters = true;
    if (p.effects.hasteTurns > 0) {
        if (!hastePhase) {
            // Free haste action: skip monsters this time.
            runMonsters = false;
            hastePhase = true;
        } else {
            // Monster turn occurs, and one haste "cycle" is consumed.
            runMonsters = true;
            hastePhase = false;
            p.effects.hasteTurns = std::max(0, p.effects.hasteTurns - 1);
            if (p.effects.hasteTurns == 0) {
                pushMsg("YOUR SPEED RETURNS TO NORMAL.", MessageKind::System, true);
            }
        }
    } else {
        hastePhase = false;
    }

    if (runMonsters) {
        monsterTurn();

        // Encumbrance: heavier burdens make the player effectively slower.
        // We model this as occasional extra monster turns (without additional hunger/regen ticks)
        // to keep the pacing simple and predictable.
        if (encumbranceEnabled_) {
            int extra = 0;
            switch (burdenState()) {
                case BurdenState::Unburdened: extra = 0; break;
                case BurdenState::Burdened:   extra = (turnCount % 7u == 0u) ? 1 : 0; break; // ~14%
                case BurdenState::Stressed:   extra = (turnCount % 4u == 0u) ? 1 : 0; break; // 25%
                case BurdenState::Strained:   extra = 1; break;
                case BurdenState::Overloaded: extra = 1; break;
            }

            for (int i = 0; i < extra && !isFinished(); ++i) {
                monsterTurn();
            }
        }
    }

    applyEndOfTurnEffects();
    cleanupDead();
    if (isFinished()) {
        maybeRecordRun();
    }

    // Burden status messages (throttled to threshold changes).
    if (!encumbranceEnabled_) {
        burdenPrev_ = BurdenState::Unburdened;
    } else {
        const BurdenState cur = burdenState();
        if (cur != burdenPrev_) {
            switch (cur) {
                case BurdenState::Unburdened: pushMsg("YOU FEEL LESS BURDENED.", MessageKind::System, true); break;
                case BurdenState::Burdened:   pushMsg("YOU FEEL BURDENED.", MessageKind::Warning, true); break;
                case BurdenState::Stressed:   pushMsg("YOU FEEL STRESSED BY YOUR LOAD.", MessageKind::Warning, true); break;
                case BurdenState::Strained:   pushMsg("YOU ARE STRAINED BY YOUR LOAD!", MessageKind::Warning, true); break;
                case BurdenState::Overloaded: pushMsg("YOU ARE OVERLOADED!", MessageKind::Warning, true); break;
            }
            burdenPrev_ = cur;
        }
    }
    recomputeFov();
    maybeAutosave();
}

bool Game::anyVisibleHostiles() const {
    for (const auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp <= 0) continue;
        if (!dung.inBounds(e.pos.x, e.pos.y)) continue;
        if (dung.at(e.pos.x, e.pos.y).visible) return true;
    }
    return false;
}


void Game::maybeAutosave() {
    if (autosaveInterval <= 0) return;
    if (isFinished()) return;
    if (turnCount == 0) return;

    const uint32_t interval = static_cast<uint32_t>(autosaveInterval);
    if (interval == 0) return;

    if ((turnCount % interval) != 0) return;
    if (lastAutosaveTurn == turnCount) return;

    const std::string path = defaultAutosavePath();
    if (path.empty()) return;

    if (saveToFile(path, true)) {
        lastAutosaveTurn = turnCount;
    }
}

static std::string nowTimestampLocal() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    tm = *std::localtime(&t);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

void Game::maybeRecordRun() {
    if (runRecorded) return;
    if (!isFinished()) return;

    ScoreEntry e;
    e.timestamp = nowTimestampLocal();
    e.won = gameWon;
    e.depth = maxDepth;
    e.turns = turnCount;
    e.kills = killCount;
    e.level = charLevel;
    e.gold = goldCount();
    e.seed = seed_;

    e.name = playerName_;
    e.slot = activeSlot_.empty() ? std::string("default") : activeSlot_;
    e.cause = endCause_;
    e.gameVersion = PROCROGUE_VERSION;

    e.score = computeScore(e);

    const std::string scorePath = defaultScoresPath();
    if (!scorePath.empty()) {
        const bool ok = scores.append(scorePath, e);
        if (ok) {
            pushMsg("RUN RECORDED.", MessageKind::System);
        }
    }

    if (autoMortemEnabled_ && !mortemWritten_) {
        namespace fs = std::filesystem;
        const fs::path dir = exportBaseDir(*this);
        const std::string ts = timestampForFilename();
        const fs::path outPath = dir / ("procrogue_mortem_" + ts + ".txt");

        const auto res = exportRunDumpToFile(*this, outPath);
        if (res.first) {
            mortemWritten_ = true;
            pushMsg("MORTEM DUMP WRITTEN.", MessageKind::System);
        } else {
            pushMsg("FAILED TO WRITE MORTEM DUMP.", MessageKind::Warning);
        }
    }

    runRecorded = true;
}

// ------------------------------------------------------------
// Auto-move / auto-explore
// ------------------------------------------------------------

