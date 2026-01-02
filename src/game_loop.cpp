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
        if (invOpen || chestOpen || targeting || kicking || helpOpen || looking || minimapOpen || statsOpen || msgHistoryOpen || levelUpOpen || optionsOpen || keybindsOpen || commandOpen || isFinished()) {
            stopAutoMove(true);
            return;
        }

        if (!inputLock) {
            autoStepTimer += dt;
            int guard = 0;
            while (autoStepTimer >= autoStepDelay && !inputLock) {
                autoStepTimer -= autoStepDelay;
                // stepAutoMove() returns false when auto-move stops (blocked, finished, etc.)
                if (!stepAutoMove()) break;
                // If a step spawned FX, stop stepping this frame so animations can play.
                inputLock = (!fx.empty() || !fxExpl.empty());
                if (autoMode == AutoMoveMode::None) break;
                if (++guard >= 32) {
                    // Safety: avoid spending too long in a single frame if something goes wrong.
                    autoStepTimer = 0.0f;
                    break;
                }
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

    // Keybinds overlay (interactive editor): consumes all actions (including LogUp/LogDown).
    if (keybindsOpen) {
        const int n = static_cast<int>(keybindsDesc_.size());
        if (n <= 0) {
            // Still allow Cancel to exit even if the cache isn't ready yet.
            if (a == Action::Cancel) {
                keybindsOpen = false;
                keybindsCapture = false;
                keybindsCaptureIndex = -1;
                keybindsCaptureAdd = false;
            }
            return;
        }

        keybindsSel = clampi(keybindsSel, 0, n - 1);

        auto clampScroll = [&]() {
            constexpr int kVisibleRows = 18;
            const int maxScroll = std::max(0, n - kVisibleRows);
            if (keybindsSel < keybindsScroll_) keybindsScroll_ = keybindsSel;
            if (keybindsSel >= keybindsScroll_ + kVisibleRows) keybindsScroll_ = keybindsSel - kVisibleRows + 1;
            keybindsScroll_ = clampi(keybindsScroll_, 0, maxScroll);
        };

        clampScroll();

        // While capturing, ignore most actions (keyboard input is routed raw via main.cpp).
        if (keybindsCapture) {
            if (a == Action::Cancel) {
                keybindsCancelCapture();
            }
            return;
        }

        switch (a) {
            case Action::Cancel:
                keybindsOpen = false;
                return;
            case Action::Up:
                keybindsSel = clampi(keybindsSel - 1, 0, n - 1);
                clampScroll();
                return;
            case Action::Down:
                keybindsSel = clampi(keybindsSel + 1, 0, n - 1);
                clampScroll();
                return;
            case Action::LogUp:
                keybindsSel = clampi(keybindsSel - 10, 0, n - 1);
                clampScroll();
                return;
            case Action::LogDown:
                keybindsSel = clampi(keybindsSel + 10, 0, n - 1);
                clampScroll();
                return;
            case Action::Confirm:
                keybindsCapture = true;
                keybindsCaptureAdd = false;
                keybindsCaptureIndex = keybindsSel;
                pushSystemMessage("PRESS A KEY TO REBIND (ESC CANCEL).");
                return;
            case Action::Right:
                keybindsCapture = true;
                keybindsCaptureAdd = true;
                keybindsCaptureIndex = keybindsSel;
                pushSystemMessage("PRESS A KEY TO ADD BINDING (ESC CANCEL).");
                return;
            case Action::Left: {
                const std::string actionName = keybindsDesc_[keybindsSel].first;
                const std::string bindKey = std::string("bind_") + actionName;
                if (settingsPath_.empty()) {
                    pushSystemMessage("NO SETTINGS PATH; CANNOT RESET BIND.");
                } else {
                    if (removeIniKey(settingsPath_, bindKey)) {
                        requestKeyBindsReload();
                        pushSystemMessage("RESET " + bindKey + " TO DEFAULT.");
                    } else {
                        pushSystemMessage("FAILED TO RESET " + bindKey + ".");
                    }
                }
                return;
            }
            default:
                return;
        }
    }

    // Message log scroll works in any mode.
    // If the message history overlay is open, scrolling applies to that overlay.
    if (a == Action::LogUp || a == Action::LogDown) {
        auto historyFilteredCount = [&]() -> int {
            const std::string needle = toLower(msgHistorySearch);
            int c = 0;
            for (const auto& m : msgs) {
                if (!messageFilterMatches(msgHistoryFilter, m.kind)) continue;
                if (!needle.empty()) {
                    const std::string hay = toLower(m.text);
                    if (hay.find(needle) == std::string::npos) continue;
                }
                ++c;
            }
            return c;
        };

        if (msgHistoryOpen) {
            const int maxScroll = std::max(0, historyFilteredCount() - 1);
            if (a == Action::LogUp) {
                msgHistoryScroll = clampi(msgHistoryScroll + 1, 0, maxScroll);
            } else {
                msgHistoryScroll = clampi(msgHistoryScroll - 1, 0, maxScroll);
            }
        } else {
            const int maxScroll = std::max(0, static_cast<int>(msgs.size()) - 1);
            if (a == Action::LogUp) {
                msgScroll = clampi(msgScroll + 1, 0, maxScroll);
            } else {
                msgScroll = clampi(msgScroll - 1, 0, maxScroll);
            }
        }
        return;
    }

    auto closeOverlays = [&]() {
        invOpen = false;
        invIdentifyMode = false;
        closeChestOverlay();
        targeting = false;
        kicking = false;
        helpOpen = false;
        looking = false;
        minimapOpen = false;
        statsOpen = false;
        optionsOpen = false;
        keybindsOpen = false;
        keybindsCapture = false;
        keybindsCaptureIndex = -1;
        keybindsCaptureAdd = false;
        keybindsScroll_ = 0;

        msgHistoryOpen = false;
        msgHistorySearchMode = false;
        msgHistoryFilter = MessageFilter::All;
        msgHistorySearch.clear();
        msgHistoryScroll = 0;

        codexOpen = false;

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
        case Action::MessageHistory:
            if (msgHistoryOpen) {
                msgHistoryOpen = false;
                msgHistorySearchMode = false;
                msgHistoryFilter = MessageFilter::All;
                msgHistorySearch.clear();
                msgHistoryScroll = 0;
            } else {
                closeOverlays();
                msgHistoryOpen = true;
                msgHistorySearchMode = false;
                msgHistoryFilter = MessageFilter::All;
                msgHistorySearch.clear();
                msgHistoryScroll = 0;
            }
            return;
        case Action::Codex:
            if (codexOpen) {
                codexOpen = false;
            } else {
                closeOverlays();
                codexOpen = true;
                std::vector<EntityKind> list;
                buildCodexList(list);
                if (list.empty()) {
                    codexSel = 0;
                } else {
                    codexSel = clampi(codexSel, 0, static_cast<int>(list.size()) - 1);
                }
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
        keybindsOpen = false;
        keybindsCapture = false;
        keybindsCaptureIndex = -1;
        keybindsCaptureAdd = false;
        keybindsScroll_ = 0;
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
        constexpr int kOptionCount = 20;

        if (a == Action::Cancel || a == Action::Options) {
            optionsOpen = false;
        keybindsOpen = false;
        keybindsCapture = false;
        keybindsCaptureIndex = -1;
        keybindsCaptureAdd = false;
        keybindsScroll_ = 0;
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

        // 2) Auto-explore secret search
        if (optionsSel == 2) {
            if (left || right || confirm) {
                setAutoExploreSearchEnabled(!autoExploreSearchEnabled());
                settingsDirtyFlag = true;
            }
            return;
        }

        // 3) Autosave interval
        if (optionsSel == 3) {
            if (left || right) {
                int t = autosaveInterval;
                t += left ? -50 : +50;
                t = clampi(t, 0, 5000);
                setAutosaveEveryTurns(t);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 4) Identification helper
        if (optionsSel == 4) {
            if (left || right || confirm) {
                setIdentificationEnabled(!identifyItemsEnabled);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 5) Hunger system
        if (optionsSel == 5) {
            if (left || right || confirm) {
                setHungerEnabled(!hungerEnabled_);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 6) Encumbrance system
        if (optionsSel == 6) {
            if (left || right || confirm) {
                setEncumbranceEnabled(!encumbranceEnabled_);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 7) Lighting / darkness
        if (optionsSel == 7) {
            if (left || right || confirm) {
                setLightingEnabled(!lightingEnabled_);
                settingsDirtyFlag = true;
            }
            return;
        }

        // 8) Yendor Doom (endgame escalation)
        if (optionsSel == 8) {
            if (left || right || confirm) {
                setYendorDoomEnabled(!yendorDoomEnabled());
                settingsDirtyFlag = true;
            }
            return;
        }

        // 9) Effect timers (HUD)
        if (optionsSel == 9) {
            if (left || right || confirm) {
                showEffectTimers_ = !showEffectTimers_;
                settingsDirtyFlag = true;
            }
            return;
        }

        // 10) Confirm quit (double-ESC)
        if (optionsSel == 10) {
            if (left || right || confirm) {
                confirmQuitEnabled_ = !confirmQuitEnabled_;
                settingsDirtyFlag = true;
            }
            return;
        }

        // 11) Auto mortem (write a dump file on win/death)
        if (optionsSel == 11) {
            if (left || right || confirm) {
                autoMortemEnabled_ = !autoMortemEnabled_;
                settingsDirtyFlag = true;
            }
            return;
        }

        // 12) Bones files (persistent death remnants)
        if (optionsSel == 12) {
            if (left || right || confirm) {
                bonesEnabled_ = !bonesEnabled_;
                settingsDirtyFlag = true;
            }
            return;
        }

        // 13) Save backups (0..10)
        if (optionsSel == 13) {
            if (left || right) {
                int n = saveBackups_;
                n += left ? -1 : +1;
                setSaveBackups(n);
                settingsDirtyFlag = true;
            }
            return;
        }

// 14) UI Theme (cycle)
if (optionsSel == 14) {
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

// 15) UI Panels (textured / solid)
if (optionsSel == 15) {
    if (left || right || confirm) {
        uiPanelsTextured_ = !uiPanelsTextured_;
        settingsDirtyFlag = true;
    }
    return;
}

// 16) 3D voxel sprites (entities/items/projectiles)
if (optionsSel == 16) {
    if (left || right || confirm) {
        voxelSpritesEnabled_ = !voxelSpritesEnabled_;
        settingsDirtyFlag = true;
    }
    return;
}

// 17) Control preset (Modern / NetHack)
if (optionsSel == 17) {
    if (left || right || confirm) {
        ControlPreset next = (controlPreset_ == ControlPreset::Modern) ? ControlPreset::Nethack : ControlPreset::Modern;
        setControlPreset(next);
        applyControlPreset(*this, next);
    }
    return;
}

// 18) Keybinds editor
if (optionsSel == 18) {
    if (left || right || confirm) {
        optionsOpen = false;
        keybindsOpen = true;
        keybindsSel = 0;
        keybindsScroll_ = 0;
        keybindsCapture = false;
        keybindsCaptureIndex = -1;
        keybindsCaptureAdd = false;
    }
    return;
}

// 19) Close
if (optionsSel == 19) {
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

    // Overlay: monster codex (bestiary / encounter log)
    if (codexOpen) {
        std::vector<EntityKind> list;
        buildCodexList(list);
        const int n = static_cast<int>(list.size());
        if (n <= 0) {
            codexSel = 0;
        } else {
            codexSel = clampi(codexSel, 0, n - 1);
        }

        switch (a) {
            case Action::Cancel:
            case Action::Codex:
            case Action::Confirm:
                codexOpen = false;
                return;
            case Action::Up:
                codexSel -= 1;
                break;
            case Action::Down:
                codexSel += 1;
                break;
            case Action::LogUp:
                codexSel -= 10;
                break;
            case Action::LogDown:
                codexSel += 10;
                break;
            case Action::Left:
            case Action::Right: {
                const int dir = (a == Action::Left) ? -1 : 1;
                const int maxF = static_cast<int>(CodexFilter::Killed);
                int v = static_cast<int>(codexFilter_);
                v += dir;
                if (v < 0) v = maxF;
                if (v > maxF) v = 0;
                codexFilter_ = static_cast<CodexFilter>(v);
                codexSel = 0;
                return;
            }
            case Action::Inventory:
            case Action::SortInventory:
                codexSort_ = (codexSort_ == CodexSort::Kind) ? CodexSort::KillsDesc : CodexSort::Kind;
                codexSel = 0;
                return;
            default:
                return;
        }

        if (n <= 0) {
            codexSel = 0;
            return;
        }
        codexSel = clampi(codexSel, 0, n - 1);
        return;
    }

    // Overlay: message history (full log viewer)
    if (msgHistoryOpen) {
        auto historyFilteredCount = [&]() -> int {
            const std::string needle = toLower(msgHistorySearch);
            int c = 0;
            for (const auto& m : msgs) {
                if (!messageFilterMatches(msgHistoryFilter, m.kind)) continue;
                if (!needle.empty()) {
                    const std::string hay = toLower(m.text);
                    if (hay.find(needle) == std::string::npos) continue;
                }
                ++c;
            }
            return c;
        };

        const int maxScroll = std::max(0, historyFilteredCount() - 1);
        switch (a) {
            case Action::Cancel:
                if (msgHistorySearchMode) {
                    msgHistorySearchMode = false;
                } else {
                    closeOverlays();
                }
                return;
            case Action::Confirm:
                // Enter: exit search mode if active, otherwise jump to newest.
                if (msgHistorySearchMode) {
                    msgHistorySearchMode = false;
                } else {
                    msgHistoryScroll = 0;
                }
                return;
            case Action::Inventory:
                // Convenient: Tab cycles filters.
                if (!msgHistorySearchMode) messageHistoryCycleFilter(+1);
                return;
            case Action::Left:
                if (!msgHistorySearchMode) messageHistoryCycleFilter(-1);
                return;
            case Action::Right:
                if (!msgHistorySearchMode) messageHistoryCycleFilter(+1);
                return;
            case Action::Up:
                if (!msgHistorySearchMode) msgHistoryScroll = clampi(msgHistoryScroll + 1, 0, maxScroll);
                return;
            case Action::Down:
                if (!msgHistorySearchMode) msgHistoryScroll = clampi(msgHistoryScroll - 1, 0, maxScroll);
                return;
            case Action::Wait:
                // Space: clear search (only when not actively typing).
                if (!msgHistorySearchMode && !msgHistorySearch.empty()) {
                    msgHistorySearch.clear();
                    msgHistoryScroll = 0;
                }
                return;
            default:
                break;
        }

        // Ignore all other actions while the overlay is open.
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

    // Kick prompt mode (directional).
    // This is a lightweight two-step command: press KICK, then a direction key.
    if (kicking) {
        switch (a) {
            case Action::Up:        acted = kickInDirection(0, -1); break;
            case Action::Down:      acted = kickInDirection(0, 1); break;
            case Action::Left:      acted = kickInDirection(-1, 0); break;
            case Action::Right:     acted = kickInDirection(1, 0); break;
            case Action::UpLeft:    acted = kickInDirection(-1, -1); break;
            case Action::UpRight:   acted = kickInDirection(1, -1); break;
            case Action::DownLeft:  acted = kickInDirection(-1, 1); break;
            case Action::DownRight: acted = kickInDirection(1, 1); break;

            case Action::Inventory:
                kicking = false;
                openInventory();
                return;

            case Action::Cancel:
            case Action::Kick:
                kicking = false;
                pushMsg("NEVER MIND.", MessageKind::System, true);
                return;

            default:
                // Ignore non-directional input while waiting for a direction.
                return;
        }

        // Only exit the prompt if an action actually consumed time.
        if (acted) {
            kicking = false;
            advanceAfterPlayerAction();
        }
        return;
    }


    // Chest container (loot/stash) overlay mode
    if (chestOpen) {
        bool acted = false;

        switch (a) {
            case Action::Cancel:
            case Action::Inventory:
                closeChestOverlay();
                return;

            case Action::Left:
                chestPaneChest = true;
                return;

            case Action::Right:
                chestPaneChest = false;
                return;

            case Action::Up:
                moveChestSelection(-1);
                return;

            case Action::Down:
                moveChestSelection(1);
                return;

            case Action::Confirm:
                acted = chestMoveSelected(true);
                break;

            case Action::Drop:
                acted = chestMoveSelected(false);
                break;

            case Action::DropAll:
                acted = chestMoveSelected(true);
                break;

            case Action::Pickup:
                acted = chestMoveAll();
                break;

            case Action::SortInventory:
                if (chestPaneChest) {
                    sortChestContents(chestOpenId, &chestSel);
                } else {
                    sortInventory();
                }
                return;

            default:
                return;
        }

        if (acted) {
            advanceAfterPlayerAction();
        }
        return;
    }

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
        case Action::Kick:
            beginKick();
            acted = false;
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
                bool hasClosedChest = false;
                bool hasOpenChest = false;
                bool hasPickableItem = false;

                for (const auto& gi : ground) {
                    if (gi.pos != p.pos) continue;

                    if (gi.item.kind == ItemKind::Chest) hasClosedChest = true;
                    else if (gi.item.kind == ItemKind::ChestOpen) hasOpenChest = true;
                    else hasPickableItem = true;
                }

                bool handled = false;

                if (hasClosedChest) {
                    acted = openChestAtPlayer();
                    handled = acted; // only handled if the attempt consumes the action (open / pick / reveal)
                }

                if (!handled && hasOpenChest) {
                    handled = openChestOverlayAtPlayer();
                    acted = false; // opening the UI is instant; time passes when you move items
                }

                // If we didn't interact with a chest, allow picking up other items on the tile.
                if (!handled && hasPickableItem) {
                    acted = pickupAtPlayer();
                } else if (!hasClosedChest && !hasOpenChest && !hasPickableItem) {
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

void Game::whistle() {
    if (isFinished()) return;

    Entity& p = playerMut();
    Entity* d = nullptr;
    for (auto& e : ents) {
        if (e.kind == EntityKind::Dog && e.hp > 0) { d = &e; break; }
    }

    pushMsg("YOU WHISTLE SHARPLY.", MessageKind::Info, true);
    emitNoise(p.pos, 14);

    if (!d) {
        pushMsg("...BUT NOTHING ANSWERS.", MessageKind::Info, true);
        advanceAfterPlayerAction();
        return;
    }

    const int dist = chebyshev(d->pos, p.pos);
    if (dist <= 2) {
        pushMsg("YOUR DOG LOOKS UP AT YOU.", MessageKind::Info, true);
        advanceAfterPlayerAction();
        return;
    }

    // Try to call the dog to a nearby free tile.
    Vec2i best = d->pos;
    bool found = false;
    for (int r = 1; r <= 3 && !found; ++r) {
        for (int dy = -r; dy <= r && !found; ++dy) {
            for (int dx = -r; dx <= r && !found; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int nx = p.pos.x + dx;
                const int ny = p.pos.y + dy;
                if (!dung.inBounds(nx, ny)) continue;
                if (!dung.isWalkable(nx, ny)) continue;
                if (entityAt(nx, ny)) continue;
                // Avoid placing the dog directly on stairs when possible.
                if ((nx == dung.stairsUp.x && ny == dung.stairsUp.y) || (nx == dung.stairsDown.x && ny == dung.stairsDown.y)) {
                    continue;
                }
                best = {nx, ny};
                found = true;
            }
        }
    }

    if (found) {
        d->pos = best;
        pushMsg("YOUR DOG COMES RUNNING.", MessageKind::Success, true);
    }

    advanceAfterPlayerAction();
}

void Game::setAlliesOrder(AllyOrder order, bool verbose) {
    int n = 0;
    for (auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp <= 0) continue;
        if (!e.friendly) continue;
        e.allyOrder = order;
        ++n;
    }

    if (!verbose) return;

    if (n <= 0) {
        pushMsg("YOU HAVE NO COMPANIONS.", MessageKind::Info, true);
        return;
    }

    const char* oname = "FOLLOW";
    switch (order) {
        case AllyOrder::Stay:  oname = "STAY"; break;
        case AllyOrder::Fetch: oname = "FETCH"; break;
        case AllyOrder::Guard: oname = "GUARD"; break;
        case AllyOrder::Follow:
        default: break;
    }

    std::ostringstream ss;
    ss << "COMPANIONS ORDERED: " << oname << ".";
    pushMsg(ss.str(), MessageKind::System, true);
}

void Game::tame() {
    if (isFinished()) return;

    // Require a food ration to offer.
    int foodIdx = -1;
    for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
        if (inv[static_cast<size_t>(i)].kind == ItemKind::FoodRation && inv[static_cast<size_t>(i)].count > 0) {
            foodIdx = i;
            break;
        }
    }
    if (foodIdx < 0) {
        pushMsg("YOU HAVE NO FOOD TO OFFER.", MessageKind::Info, true);
        return;
    }

    // Find an adjacent tameable creature.
    auto tameable = [&](EntityKind k) -> bool {
        switch (k) {
            case EntityKind::Wolf:
            case EntityKind::Snake:
            case EntityKind::Spider:
            case EntityKind::Bat:
                return true;
            default:
                return false;
        }
    };

    Entity* target = nullptr;
    const int dirs8[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    for (int i = 0; i < 8; ++i) {
        const int nx = player().pos.x + dirs8[i][0];
        const int ny = player().pos.y + dirs8[i][1];
        Entity* e = entityAtMut(nx, ny);
        if (!e) continue;
        if (e->id == playerId_) continue;
        if (e->hp <= 0) continue;
        if (e->friendly) continue;
        if (!tameable(e->kind)) continue;
        if (e->kind == EntityKind::Shopkeeper) continue;
        target = e;
        break;
    }

    // Offering food costs a turn even if it fails.
    pushMsg("YOU OFFER SOME FOOD.", MessageKind::Info, true);

    // Consume one ration.
    {
        Item& it = inv[static_cast<size_t>(foodIdx)];
        it.count = std::max(0, it.count - 1);
        if (isStackable(it.kind) && it.count <= 0) {
            inv.erase(inv.begin() + foodIdx);
        }
    }

    if (!target) {
        pushMsg("...BUT NOTHING SEEMS INTERESTED.", MessageKind::Info, true);
        advanceAfterPlayerAction();
        return;
    }

    // Taming chance: scales with Focus/Agility, and gets harder deeper down.
    int chance = 40;
    chance += playerFocus() * 4;
    chance += playerAgility() * 2;
    chance -= depth_ * 3;

    // Wolves are more receptive (classic starter pet vibe).
    if (target->kind == EntityKind::Wolf) chance += 8;

    chance = clampi(chance, 10, 85);
    const int roll = rng.range(1, 100);

    if (roll <= chance) {
        target->friendly = true;
        target->allyOrder = AllyOrder::Follow;
        target->alerted = false;
        target->lastKnownPlayerPos = {-1, -1};
        target->lastKnownPlayerAge = 9999;

        std::ostringstream ss;
        ss << "THE " << kindName(target->kind) << " SEEMS FRIENDLY!";
        pushMsg(ss.str(), MessageKind::Success, true);
    } else {
        // Failure can make it more aggressive.
        target->alerted = true;
        target->lastKnownPlayerPos = player().pos;
        target->lastKnownPlayerAge = 0;

        std::ostringstream ss;
        ss << "THE " << kindName(target->kind) << " REFUSES YOUR OFFERING.";
        pushMsg(ss.str(), MessageKind::Warning, true);
    }

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
        if (turnHookFn_) turnHookFn_(turnHookUser_, turnCount, determinismHash());
        maybeRecordRun();
        return;
    }


    // Update per-level scent trail (used by smell-capable monsters).
    updateScentMap();

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

    // Endgame escalation tick (optional): after the Amulet is acquired, the dungeon
    // starts fighting back with noise pulses and hunter packs.
    tickYendorDoom();

    if (turnHookFn_) turnHookFn_(turnHookUser_, turnCount, determinismHash());

    maybeAutosave();
}

bool Game::anyVisibleHostiles() const {
    for (const auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.friendly) continue;
        if (e.kind == EntityKind::Shopkeeper && !e.alerted) continue;
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
    e.playerClass = playerClassIdString();
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

    if (bonesEnabled_ && gameOver && !gameWon && !bonesWritten_ && player().hp <= 0) {
        if (writeBonesFile()) {
            pushMsg("YOUR BONES MAY HAUNT THIS DUNGEON...", MessageKind::System);
        } else {
            pushMsg("FAILED TO WRITE BONES FILE.", MessageKind::Warning);
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


// ------------------------------------------------------------
// Keybinds overlay helpers
// ------------------------------------------------------------

void Game::keybindsCancelCapture() {
    if (!keybindsCapture) return;
    keybindsCapture = false;
    keybindsCaptureAdd = false;
    keybindsCaptureIndex = -1;
    pushSystemMessage("BIND CAPTURE CANCELLED.");
}

static std::vector<std::string> splitCommaList(const std::string& s) {
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
    // Remove empties.
    out.erase(std::remove_if(out.begin(), out.end(), [](const std::string& t) { return t.empty(); }), out.end());
    return out;
}

void Game::keybindsCaptureToken(const std::string& chordToken) {
    if (!keybindsCapture) return;

    const int idx = keybindsCaptureIndex;
    const bool addMode = keybindsCaptureAdd;

    keybindsCapture = false;
    keybindsCaptureAdd = false;
    keybindsCaptureIndex = -1;

    if (idx < 0 || idx >= static_cast<int>(keybindsDesc_.size())) {
        pushSystemMessage("INVALID KEYBIND TARGET.");
        return;
    }

    const std::string actionName = keybindsDesc_[idx].first;
    const std::string bindKey = std::string("bind_") + actionName;

    std::string cur = keybindsDesc_[idx].second;
    std::string next;

    if (addMode) {
        const std::vector<std::string> curChords = splitCommaList(cur);
        bool already = false;
        for (const auto& c : curChords) {
            if (toLower(c) == toLower(chordToken)) {
                already = true;
                break;
            }
        }

        if (already) {
            pushSystemMessage("BINDING ALREADY PRESENT.");
            return;
        }

        if (cur.empty() || toLower(trim(cur)) == "none") {
            next = chordToken;
        } else {
            next = cur + ", " + chordToken;
        }
    } else {
        next = chordToken;
    }

    // Conflict warning (best effort, based on the cached table).
    std::vector<std::string> conflicts;
    for (int i = 0; i < static_cast<int>(keybindsDesc_.size()); ++i) {
        if (i == idx) continue;
        for (const auto& c : splitCommaList(keybindsDesc_[i].second)) {
            if (toLower(c) == toLower(chordToken)) {
                conflicts.push_back(keybindsDesc_[i].first);
                break;
            }
        }
    }

    if (!conflicts.empty()) {
        std::string msg = "WARNING: " + chordToken + " ALSO BINDS ";
        for (size_t i = 0; i < conflicts.size(); ++i) {
            if (i) msg += ", ";
            msg += conflicts[i];
        }
        msg += ".";
        pushSystemMessage(msg);
    }

    if (settingsPath_.empty()) {
        pushSystemMessage("NO SETTINGS PATH; CANNOT WRITE BIND.");
        return;
    }

    if (!updateIniKey(settingsPath_, bindKey, next)) {
        pushSystemMessage("FAILED TO WRITE " + bindKey + ".");
        return;
    }

    // Optimistic UI update; main.cpp will reload and refresh the cache shortly.
    keybindsDesc_[idx].second = next;

    requestKeyBindsReload();
    pushSystemMessage("SET " + bindKey + " = " + next);
}

