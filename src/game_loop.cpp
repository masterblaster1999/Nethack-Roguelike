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


    // Animate particle FX events (visual-only; does not lock input).
    if (!fxParticles_.empty()) {
        for (auto& e : fxParticles_) {
            if (e.delay > 0.0f) {
                e.delay = std::max(0.0f, e.delay - dt);
            } else {
                e.timer += dt;
            }
        }
        fxParticles_.erase(std::remove_if(fxParticles_.begin(), fxParticles_.end(), [](const FXParticleEvent& e) {
            return e.delay <= 0.0f && e.timer >= e.duration;
        }), fxParticles_.end());
    }

    // Lock input while any FX are active.
    inputLock = (!fx.empty() || !fxExpl.empty());

    // Auto-move (travel / explore) steps are processed here to keep the game turn-based
    // while still providing smooth-ish movement.
    if (autoMode != AutoMoveMode::None) {
        // If the player opened an overlay, stop (don't keep walking while in menus).
        if (invOpen || chestOpen || spellsOpen || targeting || kicking || digging || helpOpen || looking || minimapOpen || statsOpen || msgHistoryOpen || scoresOpen || codexOpen || discoveriesOpen || levelUpOpen || optionsOpen || keybindsOpen || commandOpen || isFinished()) {
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
        // Apply search/filter (if any) to build the visible row list.
        std::vector<int> vis;
        keybindsBuildVisibleIndices(vis);
        const int n = static_cast<int>(vis.size());

        if (n <= 0) {
            // Still allow Cancel to exit even if the cache isn't ready yet / filter has no matches.
            if (a == Action::Cancel) {
                if (keybindsSearchMode_) {
                    keybindsSearchMode_ = false;
                } else {
                    keybindsOpen = false;
                    keybindsCapture = false;
                    keybindsCaptureIndex = -1;
                    keybindsCaptureAdd = false;
                }
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
                // Mirror message-history UX: ESC exits typing mode first, then closes the overlay.
                if (keybindsSearchMode_) {
                    keybindsSearchMode_ = false;
                } else {
                    keybindsOpen = false;
                }
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
                // Enter exits typing mode first; otherwise starts capture (replace).
                if (keybindsSearchMode_) {
                    keybindsSearchMode_ = false;
                    return;
                }
                keybindsCapture = true;
                keybindsCaptureAdd = false;
                keybindsCaptureIndex = vis[keybindsSel];
                pushSystemMessage("PRESS A KEY TO REBIND (ESC CANCEL).");
                return;

            case Action::Right:
                // Right starts capture (add) unless we're in typing mode.
                if (keybindsSearchMode_) return;
                keybindsCapture = true;
                keybindsCaptureAdd = true;
                keybindsCaptureIndex = vis[keybindsSel];
                pushSystemMessage("PRESS A KEY TO ADD BINDING (ESC CANCEL).");
                return;

            case Action::Left: {
                // Left resets the selected binding to default (removes bind_* override),
                // unless we're in typing mode.
                if (keybindsSearchMode_) return;

                const int rowIdx = vis[keybindsSel];
                const std::string actionName = keybindsDesc_[rowIdx].first;
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

    // Message log scroll (PageUp/PageDown) works in most modes.
    // Overlays that want to use LOG UP/DOWN for their own navigation must be listed here.
    if (a == Action::LogUp || a == Action::LogDown) {
        // Allow certain overlays (codex / discoveries / scores / minimap / inventory / chests / spells / options / help / message history)
        // to use LOG UP/DOWN (PageUp/PageDown) for their own navigation.
        if (!(codexOpen || discoveriesOpen || scoresOpen || minimapOpen || invOpen || chestOpen || spellsOpen || optionsOpen || msgHistoryOpen || helpOpen)) {
            const int maxScroll = std::max(0, static_cast<int>(msgs.size()) - 1);
            if (a == Action::LogUp) {
                msgScroll = clampi(msgScroll + 1, 0, maxScroll);
            } else {
                msgScroll = clampi(msgScroll - 1, 0, maxScroll);
            }
            return;
        }
    }

    auto closeOverlays = [&]() {
        invOpen = false;
        invIdentifyMode = false;
        invEnchantRingMode = false;
        closeChestOverlay();
        targeting = false;
        targetingMode_ = TargetingMode::Ranged;
        // Cancel any in-progress fishing fight prompt (UI-only).
        fishingFightActive_ = false;
        fishingFightRodItemId_ = 0;
        fishingFightFishSeed_ = 0u;
        fishingFightLabel_.clear();
        targetLine.clear();
        targetValid = false;
        targetStatusText_.clear();
        spellsOpen = false;
        kicking = false;
        helpOpen = false;
        helpScroll_ = 0;
        looking = false;
        minimapOpen = false;
        statsOpen = false;
        optionsOpen = false;
        keybindsOpen = false;
        keybindsCapture = false;
        keybindsCaptureIndex = -1;
        keybindsCaptureAdd = false;
        keybindsSearchMode_ = false;
        keybindsSearch_.clear();
        keybindsScroll_ = 0;

        msgHistoryOpen = false;
        msgHistorySearchMode = false;
        msgHistoryFilter = MessageFilter::All;
        msgHistorySearch.clear();
        msgHistoryScroll = 0;

        codexOpen = false;
        scoresOpen = false;
        discoveriesOpen = false;

        if (commandOpen) {
            commandOpen = false;
            commandBuf.clear();
            commandCursor_ = 0;
            commandDraft.clear();
            commandHistoryPos = -1;
            commandAutoBase.clear();
            commandAutoPrefix.clear();
            commandAutoMatches.clear();
            commandAutoHints.clear();
            commandAutoDescs.clear();
            commandAutoIndex = -1;
            commandAutoFuzzy = false;
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
            case Action::LogUp:
                moveInventorySelection(-10);
                break;
            case Action::LogDown:
                moveInventorySelection(10);
                break;
            case Action::SortInventory:
                sortInventory();
                break;
            case Action::Confirm: {
                if (inv.empty()) {
                    invIdentifyMode = false;
                    invEnchantRingMode = false;
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
                invEnchantRingMode = false;
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
    // Modal inventory prompt: selecting a ring for Scroll of Enchant Ring
    // ------------------------------------------------------------
    // This runs *before* global hotkeys so the prompt can't be dismissed by opening other overlays.
    if (invOpen && invEnchantRingMode) {
        auto candidates = [&]() {
            std::vector<int> out;
            out.reserve(8);
            for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
                const Item& it = inv[static_cast<size_t>(i)];
                if (!isRingKind(it.kind)) continue;
                out.push_back(i);
            }
            return out;
        };

        auto enchantAt = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(inv.size())) return;
            Item& r = inv[static_cast<size_t>(idx)];
            if (!isRingKind(r.kind)) return;
            r.enchant += 1;
            pushMsg("YOUR RING GLOWS BRIEFLY.", MessageKind::Success, true);
        };

        auto enchantRandom = [&]() {
            const std::vector<int> c = candidates();
            if (c.empty()) {
                pushMsg("NOTHING HAPPENS.", MessageKind::Info, true);
                invEnchantRingMode = false;
                return;
            }
            const int pick = rng.range(0, static_cast<int>(c.size()) - 1);
            enchantAt(c[static_cast<size_t>(pick)]);
        };

        switch (a) {
            case Action::Up:
                moveInventorySelection(-1);
                break;
            case Action::Down:
                moveInventorySelection(1);
                break;
            case Action::LogUp:
                moveInventorySelection(-10);
                break;
            case Action::LogDown:
                moveInventorySelection(10);
                break;
            case Action::SortInventory:
                sortInventory();
                break;
            case Action::Confirm: {
                if (inv.empty()) {
                    invEnchantRingMode = false;
                    break;
                }
                invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
                const Item& selIt = inv[static_cast<size_t>(invSel)];

                if (!isRingKind(selIt.kind)) {
                    pushMsg("THAT IS NOT A RING.", MessageKind::Info, true);
                    break;
                }

                enchantAt(invSel);
                invEnchantRingMode = false;
                break;
            }
            case Action::Cancel:
            case Action::Inventory:
                // Treat cancel as "pick randomly" so the scroll isn't wasted.
                enchantRandom();
                closeInventory();
                break;
            default:
                // Ignore other actions while the prompt is active.
                break;
        }
        return;
    }


    // ------------------------------------------------------------
    // Modal inventory prompt: shrine services that need a target item
    // ------------------------------------------------------------
    // Like Scroll of Identify, the shrine prayer action already consumed the turn;
    // this prompt is UI-only and does not advance the simulation.
    if (invOpen && invPrompt_ != InvPromptKind::None) {
        auto unidentifiedKinds = [&]() {
            std::vector<ItemKind> out;
            out.reserve(16);
            auto seen = [&](ItemKind k) {
                for (ItemKind x : out) if (x == k) return true;
                return false;
            };
            for (const auto& it : inv) {
                if (!isIdentifiableKind(it.kind)) continue;
                if (isIdentified(it.kind)) continue;
                if (!seen(it.kind)) out.push_back(it.kind);
            }
            return out;
        };

        auto blessable = [&](const Item& it) {
            if (it.kind == ItemKind::Gold) return false;
            if (it.kind == ItemKind::AmuletYendor) return false;
            const ItemDef& d = itemDef(it.kind);
            return (d.slot != EquipSlot::None) || d.consumable;
        };

        auto isRechargeableWand = [&](const Item& it) {
            if (!isWandKind(it.kind)) return false;
            const ItemDef& d = itemDef(it.kind);
            if (d.maxCharges <= 0) return false;
            return it.charges < d.maxCharges;
        };

        auto blessOne = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(inv.size())) return;
            Item& it = inv[static_cast<size_t>(idx)];
            if (!blessable(it)) {
                pushMsg("THAT CANNOT BE BLESSED.", MessageKind::Info, true);
                return;
            }

            Item named = it;
            named.buc = 0;
            const std::string nm = displayItemName(named);

            if (it.buc < 0) {
                it.buc = 0;
                pushMsg("A WARMTH LIFTS THE CURSE FROM YOUR " + nm + ".", MessageKind::Success, true);
            } else if (it.buc == 0) {
                it.buc = 1;
                pushMsg("YOUR " + nm + " GLOWS WITH HOLY LIGHT.", MessageKind::Success, true);
            } else {
                pushMsg("YOUR " + nm + " SHINES BRIEFLY.", MessageKind::Info, true);
            }
        };

        auto rechargeOne = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(inv.size())) return;
            Item& it = inv[static_cast<size_t>(idx)];
            if (!isWandKind(it.kind)) {
                pushMsg("THAT IS NOT A WAND.", MessageKind::Info, true);
                return;
            }
            const ItemDef& d = itemDef(it.kind);
            if (d.maxCharges <= 0) {
                pushMsg("THAT WAND CANNOT HOLD A CHARGE.", MessageKind::Info, true);
                return;
            }

            const int before = it.charges;
            if (it.buc < 0) it.buc = 0;
            it.charges = d.maxCharges;

            Item named = it;
            named.buc = 0;
            const std::string nm = displayItemName(named);

            if (before < d.maxCharges) {
                pushMsg("DIVINE ENERGY FLOWS INTO YOUR " + nm + ".", MessageKind::Success, true);
            } else {
                pushMsg("YOUR " + nm + " IS ALREADY FULLY CHARGED.", MessageKind::Info, true);
            }
        };

        auto sacrificeOne = [&](int idx) {
            if (idx < 0 || idx >= static_cast<int>(inv.size())) return;
            Item& it = inv[static_cast<size_t>(idx)];
            if (!isCorpseKind(it.kind)) {
                pushMsg("THAT IS NOT A PROPER SACRIFICE.", MessageKind::Info, true);
                return;
            }

            // Corpse freshness is tracked in charges (see useSelected corpse-eating logic).
            const bool rotten = (it.charges <= 0);

            // Map corpse kind to a representative monster for piety value.
            EntityKind ek = EntityKind::Goblin;
            switch (it.kind) {
                case ItemKind::CorpseGoblin:   ek = EntityKind::Goblin; break;
                case ItemKind::CorpseOrc:      ek = EntityKind::Orc; break;
                case ItemKind::CorpseBat:      ek = EntityKind::Bat; break;
                case ItemKind::CorpseSlime:    ek = EntityKind::Slime; break;
                case ItemKind::CorpseKobold:   ek = EntityKind::KoboldSlinger; break;
                case ItemKind::CorpseWolf:     ek = EntityKind::Wolf; break;
                case ItemKind::CorpseTroll:    ek = EntityKind::Troll; break;
                case ItemKind::CorpseWizard:   ek = EntityKind::Wizard; break;
                case ItemKind::CorpseSnake:    ek = EntityKind::Snake; break;
                case ItemKind::CorpseSpider:   ek = EntityKind::Spider; break;
                case ItemKind::CorpseOgre:     ek = EntityKind::Ogre; break;
                case ItemKind::CorpseMimic:    ek = EntityKind::Mimic; break;
                case ItemKind::CorpseMinotaur: ek = EntityKind::Minotaur; break;
                default: break;
            }

            int gain = std::max(1, xpFor(ek) / 8);
            if (rotten) gain = std::max(1, gain / 2);

            // Consume the corpse.
            if (it.count > 1) {
                it.count -= 1;
            } else {
                inv.erase(inv.begin() + idx);
                invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
            }

            piety_ = std::min(999, piety_ + gain);

            pushMsg("YOU OFFER A SACRIFICE. (+"
                    + std::to_string(gain)
                    + " PIETY)", MessageKind::Success, true);
        };

        auto identifyRandom = [&]() {
            std::vector<ItemKind> c = unidentifiedKinds();
            if (c.empty()) {
                pushMsg("YOU LEARN NOTHING NEW.", MessageKind::Info, true);
                return;
            }
            const int idx = rng.range(0, static_cast<int>(c.size()) - 1);
            (void)markIdentified(c[static_cast<size_t>(idx)], false);
            pushMsg("DIVINE INSIGHT REVEALS THE TRUTH.", MessageKind::Success, true);
        };

        auto blessEquippedFallback = [&]() {
            // Prefer equipped gear, otherwise first blessable item.
            const int equippedIds[5] = { equipMeleeId, equipArmorId, equipRangedId, equipRing1Id, equipRing2Id };
            for (int id : equippedIds) {
                if (id == 0) continue;
                const int idx = findItemIndexById(inv, id);
                if (idx >= 0 && blessable(inv[static_cast<size_t>(idx)])) {
                    blessOne(idx);
                    return;
                }
            }
            for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
                if (blessable(inv[static_cast<size_t>(i)])) {
                    blessOne(i);
                    return;
                }
            }
            pushMsg("NOTHING HERE RESPONDS.", MessageKind::Info, true);
        };

        auto rechargeBestFallback = [&]() {
            int bestIdx = -1;
            int bestMissing = -1;
            int bestMax = 0;
            for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
                const Item& it = inv[static_cast<size_t>(i)];
                if (!isRechargeableWand(it)) continue;
                const ItemDef& d = itemDef(it.kind);
                const int missing = d.maxCharges - it.charges;
                if (missing > bestMissing || (missing == bestMissing && d.maxCharges > bestMax)) {
                    bestMissing = missing;
                    bestMax = d.maxCharges;
                    bestIdx = i;
                }
            }
            if (bestIdx >= 0) rechargeOne(bestIdx);
            else pushMsg("YOU HAVE NOTHING TO RECHARGE.", MessageKind::Info, true);
        };

        auto sacrificeBestFallback = [&]() {
            // Prefer the freshest/highest-value corpse.
            int bestIdx = -1;
            int bestScore = -999999;
            for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
                const Item& it = inv[static_cast<size_t>(i)];
                if (!isCorpseKind(it.kind)) continue;
                int score = 0;
                // Freshness in charges (higher = fresher).
                score += it.charges;
                // Roughly weight by corpse kind (minotaur > troll > ogre > ...)
                switch (it.kind) {
                    case ItemKind::CorpseMinotaur: score += 500; break;
                    case ItemKind::CorpseWizard:   score += 300; break;
                    case ItemKind::CorpseTroll:    score += 250; break;
                    case ItemKind::CorpseOgre:     score += 200; break;
                    case ItemKind::CorpseMimic:    score += 180; break;
                    case ItemKind::CorpseWolf:     score += 120; break;
                    case ItemKind::CorpseSpider:   score += 110; break;
                    case ItemKind::CorpseSnake:    score += 100; break;
                    case ItemKind::CorpseOrc:      score += 90; break;
                    case ItemKind::CorpseKobold:   score += 70; break;
                    case ItemKind::CorpseGoblin:   score += 60; break;
                    case ItemKind::CorpseBat:      score += 50; break;
                    case ItemKind::CorpseSlime:    score += 40; break;
                    default: break;
                }
                if (score > bestScore) { bestScore = score; bestIdx = i; }
            }
            if (bestIdx >= 0) sacrificeOne(bestIdx);
            else pushMsg("YOU HAVE NOTHING TO SACRIFICE.", MessageKind::Info, true);
        };

        switch (a) {
            case Action::Up:
                moveInventorySelection(-1);
                break;
            case Action::Down:
                moveInventorySelection(1);
                break;
            case Action::LogUp:
                moveInventorySelection(-10);
                break;
            case Action::LogDown:
                moveInventorySelection(10);
                break;
            case Action::SortInventory:
                sortInventory();
                break;

            case Action::Confirm: {
                if (inv.empty()) {
                    closeInventory();
                    break;
                }
                invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
                const Item& selIt = inv[static_cast<size_t>(invSel)];

                switch (invPrompt_) {
                    case InvPromptKind::ShrineIdentify:
                        if (!isIdentifiableKind(selIt.kind) || isIdentified(selIt.kind)) {
                            pushMsg("THAT DOESN'T REVEAL ANYTHING.", MessageKind::Info, true);
                            break;
                        }
                        (void)markIdentified(selIt.kind, false);
                        pushMsg("DIVINE INSIGHT REVEALS THE TRUTH.", MessageKind::Success, true);
                        closeInventory();
                        break;

                    case InvPromptKind::ShrineBless:
                        if (!blessable(selIt)) {
                            pushMsg("THAT CANNOT BE BLESSED.", MessageKind::Info, true);
                            break;
                        }
                        blessOne(invSel);
                        closeInventory();
                        break;

                    case InvPromptKind::ShrineRecharge:
                        if (!isRechargeableWand(selIt)) {
                            pushMsg("THAT CANNOT BE RECHARGED HERE.", MessageKind::Info, true);
                            break;
                        }
                        rechargeOne(invSel);
                        closeInventory();
                        break;

                    case InvPromptKind::ShrineSacrifice:
                        if (!isCorpseKind(selIt.kind)) {
                            pushMsg("THAT IS NOT A PROPER SACRIFICE.", MessageKind::Info, true);
                            break;
                        }
                        sacrificeOne(invSel);
                        closeInventory();
                        break;

                    default:
                        closeInventory();
                        break;
                }
                break;
            }

            case Action::Cancel:
            case Action::Inventory:
                // Cancel chooses the prompt's suggested default.
                switch (invPrompt_) {
                    case InvPromptKind::ShrineIdentify:
                        identifyRandom();
                        closeInventory();
                        break;
                    case InvPromptKind::ShrineBless:
                        blessEquippedFallback();
                        closeInventory();
                        break;
                    case InvPromptKind::ShrineRecharge:
                        rechargeBestFallback();
                        closeInventory();
                        break;
                    case InvPromptKind::ShrineSacrifice:
                        sacrificeBestFallback();
                        closeInventory();
                        break;
                    default:
                        closeInventory();
                        break;
                }
                break;

            default:
                // Ignore other actions while the prompt is active.
                break;
        }
        return;
    }


    // ------------------------------------------------------------
    // Modal inventory prompt: crafting (Crafting Kit)
    // ------------------------------------------------------------
    // This prompt is UI-only until the player confirms a craft, at which point it consumes a turn.
    if (invOpen && invCraftMode) {
        // If the kit is missing (dropped/destroyed), abort the prompt safely.
        bool haveKit = false;
        for (const auto& it : inv) {
            if (it.kind == ItemKind::CraftingKit) { haveKit = true; break; }
        }
        if (!haveKit) {
            invCraftMode = false;
            invCraftFirstId_ = 0;
            rebuildCraftingPreview();
            pushMsg("YOU NO LONGER HAVE A CRAFTING KIT.", MessageKind::Warning, true);
            return;
        }

        auto selectedItemName = [&](const Item& it) -> std::string {
            Item one = it;
            one.count = 1;
            return displayItemName(one);
        };

        switch (a) {
            case Action::Up:
                moveInventorySelection(-1);
                break;
            case Action::Down:
                moveInventorySelection(1);
                break;
            case Action::LogUp:
                moveInventorySelection(-10);
                break;
            case Action::LogDown:
                moveInventorySelection(10);
                break;
            case Action::SortInventory:
                sortInventory();
                break;

            case Action::Confirm: {
                if (inv.empty()) {
                    invCraftMode = false;
                    invCraftFirstId_ = 0;
                    rebuildCraftingPreview();
                    pushMsg("NOTHING TO CRAFT WITH.", MessageKind::Info, true);
                    break;
                }

                invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
                const Item& selIt = inv[static_cast<size_t>(invSel)];

                if (!isCraftIngredientKind(selIt.kind)) {
                    pushMsg("THAT IS NOT A CRAFTING INGREDIENT.", MessageKind::Info, true);
                    break;
                }

                if (invCraftFirstId_ == 0) {
                    invCraftFirstId_ = selIt.id;
                    pushMsg("INGREDIENT 1: " + selectedItemName(selIt) + ". SELECT INGREDIENT 2.", MessageKind::System, true);

                    // Small UX: nudge selection off the first ingredient when possible.
                    moveInventorySelection(1);
                } else {
                    const int firstId = invCraftFirstId_;
                    const bool acted = craftCombineById(firstId, selIt.id);
                    if (acted) {
                        // Chain-crafting convenience: stay in craft mode, clear selection.
                        invCraftFirstId_ = 0;
                        pushMsg("CRAFT AGAIN OR ESC TO EXIT.", MessageKind::System, true);
                        rebuildCraftingPreview();
                        advanceAfterPlayerAction();
                    }
                }
                break;
            }

            case Action::Cancel:
            case Action::Inventory:
                if (invCraftFirstId_ != 0) {
                    invCraftFirstId_ = 0;
                    pushMsg("INGREDIENT 1 CLEARED.", MessageKind::System, true);
                } else {
                    invCraftMode = false;
                    pushMsg("CRAFTING MODE EXITED.", MessageKind::System, true);
                }
                rebuildCraftingPreview();
                break;

            default:
                // Ignore other actions while crafting mode is active.
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
        if (a == Action::Load) { (void)loadFromFileWithBackups(defaultSavePath()); return; }
        if (a == Action::LoadAuto) { (void)loadFromFileWithBackups(defaultAutosavePath()); return; }

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
            (void)loadFromFileWithBackups(defaultSavePath());
            return;
        case Action::LoadAuto:
            (void)loadFromFileWithBackups(defaultAutosavePath());
            return;

        case Action::Help:
            if (helpOpen) {
                helpOpen = false;
                helpScroll_ = 0;
            } else {
                closeOverlays();
                helpOpen = true;
                helpScroll_ = 0;
            }
            return;

        case Action::Spells:
            if (spellsOpen) {
                spellsOpen = false;
            } else {
                closeOverlays();
                openSpells();
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

        case Action::Scores:
            if (scoresOpen) {
                scoresOpen = false;
            } else {
                closeOverlays();
                scoresOpen = true;
                scoresView_ = ScoresView::Top;
                scoresSel = 0;
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

        case Action::Discoveries:
            if (discoveriesOpen) {
                discoveriesOpen = false;
            } else {
                closeOverlays();
                discoveriesOpen = true;
                std::vector<ItemKind> list;
                buildDiscoveryList(list);
                if (list.empty()) {
                    discoveriesSel = 0;
                } else {
                    discoveriesSel = clampi(discoveriesSel, 0, static_cast<int>(list.size()) - 1);
                }
            }
            return;

        case Action::ToggleMinimap:
            if (minimapOpen) {
                minimapOpen = false;
                minimapCursorActive_ = false;
            } else {
                closeOverlays();
                minimapOpen = true;
                minimapCursorPos_ = player().pos;
                minimapCursorActive_ = true;
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

        case Action::TogglePerfOverlay:
            // UI-only: do not close overlays; this is a tiny debug HUD that can be shown anywhere.
            setPerfOverlayEnabled(!perfOverlayEnabled());
            settingsDirtyFlag = true;
            pushMsg(std::string("PERF OVERLAY: ") + (perfOverlayEnabled() ? "ON" : "OFF") + ".", MessageKind::System);
            return;

        case Action::ToggleViewMode:
            viewMode_ = (viewMode_ == ViewMode::TopDown) ? ViewMode::Isometric : ViewMode::TopDown;
            settingsDirtyFlag = true;
            pushMsg(std::string("VIEW: ") + viewModeDisplayName() + ".", MessageKind::System);
            return;

        case Action::ToggleVoxelSprites:
            setVoxelSpritesEnabled(!voxelSpritesEnabled());
            settingsDirtyFlag = true;
            pushMsg(std::string("3D SPRITES: ") + (voxelSpritesEnabled() ? "ON" : "OFF") + ".", MessageKind::System);
            return;

        case Action::Options:
            if (optionsOpen) {
                optionsOpen = false;
                // Defensive: ensure any capture UI is closed too.
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
                commandCursor_ = 0;
                commandDraft.clear();
                commandHistoryPos = -1;
                commandAutoBase.clear();
                commandAutoPrefix.clear();
                commandAutoMatches.clear();
                commandAutoHints.clear();
                commandAutoDescs.clear();
                commandAutoIndex = -1;
                commandAutoFuzzy = false;
            } else {
                // Allow opening the command prompt while in LOOK mode without losing the cursor,
                // so commands like #mark / #travel / #throwvoice can apply to the looked-at tile.
                const bool preserveLook = looking;
                const Vec2i savedLook = lookPos;
                closeOverlays();
                commandOpen = true;
                commandBuf.clear();
                commandCursor_ = 0;
                commandDraft.clear();
                commandHistoryPos = -1;
                commandAutoBase.clear();
                commandAutoPrefix.clear();
                commandAutoMatches.clear();
                commandAutoHints.clear();
                commandAutoDescs.clear();
                commandAutoIndex = -1;
                commandAutoFuzzy = false;
                if (preserveLook) {
                    looking = true;
                    lookPos = savedLook;
                }
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
            commandCursor_ = 0;
            commandDraft.clear();
            commandHistoryPos = -1;
            commandAutoBase.clear();
            commandAutoPrefix.clear();
            commandAutoMatches.clear();
            commandAutoHints.clear();
            commandAutoDescs.clear();
            commandAutoIndex = -1;
            commandAutoFuzzy = false;
            return;
        }

        if (a == Action::Confirm) {
            std::string line = trim(commandBuf);
            commandOpen = false;
            commandBuf.clear();
            commandCursor_ = 0;
            commandDraft.clear();
            commandHistoryPos = -1;
            commandAutoBase.clear();
            commandAutoPrefix.clear();
            commandAutoMatches.clear();
            commandAutoHints.clear();
            commandAutoDescs.clear();
            commandAutoIndex = -1;
            commandAutoFuzzy = false;
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
            // Navigating history cancels any active TAB-completion cycling state.
            commandAutoBase.clear();
            commandAutoPrefix.clear();
            commandAutoMatches.clear();
            commandAutoHints.clear();
            commandAutoDescs.clear();
            commandAutoIndex = -1;
            commandAutoFuzzy = false;

            if (!commandHistory.empty()) {
                if (commandHistoryPos < 0) {
                    commandDraft = commandBuf;
                    commandHistoryPos = static_cast<int>(commandHistory.size()) - 1;
                } else {
                    commandHistoryPos = std::max(0, commandHistoryPos - 1);
                }
                commandBuf = commandHistory[commandHistoryPos];
                commandCursor_ = static_cast<int>(commandBuf.size());
            }
            return;
        }

        if (a == Action::Down) {
            // Navigating history cancels any active TAB-completion cycling state.
            commandAutoBase.clear();
            commandAutoPrefix.clear();
            commandAutoMatches.clear();
            commandAutoHints.clear();
            commandAutoDescs.clear();
            commandAutoIndex = -1;
            commandAutoFuzzy = false;

            if (commandHistoryPos >= 0) {
                if (commandHistoryPos + 1 < static_cast<int>(commandHistory.size())) {
                    ++commandHistoryPos;
                    commandBuf = commandHistory[commandHistoryPos];
                    commandCursor_ = static_cast<int>(commandBuf.size());
                } else {
                    commandHistoryPos = -1;
                    commandAutoBase.clear();
                    commandAutoPrefix.clear();
                    commandAutoMatches.clear();
                    commandAutoHints.clear();
                    commandAutoDescs.clear();
                    commandAutoIndex = -1;
                    commandAutoFuzzy = false;
                    commandBuf = commandDraft;
                    commandCursor_ = static_cast<int>(commandBuf.size());
                    commandDraft.clear();
                }
            }
            return;
        }

        // Line editing: cursor navigation (UI-only; does not consume turns).
        if (a == Action::Left) {
            commandCursorLeft();
            return;
        }
        if (a == Action::Right) {
            commandCursorRight();
            return;
        }
        if (a == Action::LogUp) {
            commandCursorHome();
            return;
        }
        if (a == Action::LogDown) {
            commandCursorEnd();
            return;
        }

        // Ignore any other actions while the prompt is open.
        return;
    }

    // Overlay: options menu (does not consume turns)
    if (optionsOpen) {
        constexpr int kOptionCount = 21;

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

        // Page navigation (LOG keys / PageUp/PageDown)
        if (a == Action::LogUp) {
            optionsSel = clampi(optionsSel - 5, 0, kOptionCount - 1);
            return;
        }
        if (a == Action::LogDown) {
            optionsSel = clampi(optionsSel + 5, 0, kOptionCount - 1);
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

// 17) ISO cutaway (foreground wall fade for readability; visual-only)
if (optionsSel == 17) {
    if (left || right || confirm) {
        setIsoCutawayEnabled(!isoCutawayEnabled());
        settingsDirtyFlag = true;
    }
    return;
}

// 18) Control preset (Modern / NetHack)
if (optionsSel == 20) {
    if (left || right || confirm) {
        ControlPreset next = (controlPreset_ == ControlPreset::Modern) ? ControlPreset::Nethack : ControlPreset::Modern;
        setControlPreset(next);
        applyControlPreset(*this, next);
    }
    return;
}

// 19) Keybinds editor
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

// 20) Close
if (optionsSel == 19) {
    if (left || right || confirm) optionsOpen = false;
    return;
}

        return;
    }

    // Finished runs: allow restart, but keep UI overlays navigable.
    if (isFinished()) {
        if (a == Action::Restart) {
            newGame(hash32(rng.nextU32()));
            return;
        }
        // Note: other actions may be consumed by UI overlays below; gameplay actions
        // are blocked later in the function.
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
        if (!minimapCursorActive_) {
            minimapCursorPos_ = player().pos;
            minimapCursorActive_ = true;
        }

        // Close
        if (a == Action::Cancel || a == Action::ToggleMinimap) {
            minimapOpen = false;
            minimapCursorActive_ = false;
            return;
        }


        // Zoom (UI-only; does not take a turn)
        if (a == Action::MinimapZoomIn || a == Action::MinimapZoomOut) {
            const int cur = minimapZoom();
            int next = cur + (a == Action::MinimapZoomIn ? 1 : -1);
            next = std::clamp(next, -3, 3);
            if (next != cur) {
                setMinimapZoom(next);
                markSettingsDirty();
            }
            return;
        }

        // Navigation (supports diagonals)
        int dx = 0;
        int dy = 0;
        switch (a) {
            case Action::Up:       dy = -1; break;
            case Action::Down:     dy =  1; break;
            case Action::Left:     dx = -1; break;
            case Action::Right:    dx =  1; break;
            case Action::UpLeft:   dx = -1; dy = -1; break;
            case Action::UpRight:  dx =  1; dy = -1; break;
            case Action::DownLeft: dx = -1; dy =  1; break;
            case Action::DownRight:dx =  1; dy =  1; break;

            // Quick scroll (LOG keys / mouse wheel) move by 10 tiles.
            case Action::LogUp:    dy = -10; break;
            case Action::LogDown:  dy =  10; break;
            default: break;
        }

        if (dx != 0 || dy != 0) {
            const int maxX = std::max(0, dung.width - 1);
            const int maxY = std::max(0, dung.height - 1);
            minimapCursorPos_.x = clampi(minimapCursorPos_.x + dx, 0, maxX);
            minimapCursorPos_.y = clampi(minimapCursorPos_.y + dy, 0, maxY);
            return;
        }

        // Confirm: auto-travel to the selected tile (closes the minimap automatically).
        if (a == Action::Confirm) {
            requestAutoTravel(minimapCursorPos_);
            return;
        }

        // Look: switch to LOOK mode at the selected tile.
        if (a == Action::Look) {
            beginLookAt(minimapCursorPos_);
            return;
        }

        // Consume all other input while the minimap is open.
        return;
    }

    // Overlay: stats
    if (statsOpen) {
        if (a == Action::Cancel) statsOpen = false;
        return;
    }

    // Overlay: scores / run history (Hall of Fame)
    if (scoresOpen) {
        std::vector<size_t> list;
        buildScoresList(list);
        const int n = static_cast<int>(list.size());
        const int maxSel = std::max(0, n - 1);
        if (n <= 0) {
            scoresSel = 0;
        } else {
            scoresSel = clampi(scoresSel, 0, maxSel);
        }

        auto toggleView = [&]() {
            scoresView_ = (scoresView_ == ScoresView::Top) ? ScoresView::Recent : ScoresView::Top;
            scoresSel = 0;
        };

        switch (a) {
            case Action::Cancel:
            case Action::Scores:
            case Action::Confirm:
                scoresOpen = false;
                return;
            case Action::Up:      scoresSel = clampi(scoresSel - 1, 0, maxSel); return;
            case Action::Down:    scoresSel = clampi(scoresSel + 1, 0, maxSel); return;
            case Action::LogUp:   scoresSel = clampi(scoresSel - 10, 0, maxSel); return;
            case Action::LogDown: scoresSel = clampi(scoresSel + 10, 0, maxSel); return;
            case Action::Left:
            case Action::Right:
                toggleView();
                return;
            default:
                return;
        }
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

    // Overlay: discoveries (identification reference)
    if (discoveriesOpen) {
        std::vector<ItemKind> list;
        buildDiscoveryList(list);
        const int n = static_cast<int>(list.size());
        if (n <= 0) {
            discoveriesSel = 0;
        } else {
            discoveriesSel = clampi(discoveriesSel, 0, n - 1);
        }

        auto cycleFilter = [&](int dir) {
            const int maxF = static_cast<int>(DiscoveryFilter::Wands);
            int v = static_cast<int>(discoveriesFilter_);
            v += dir;
            if (v < 0) v = maxF;
            if (v > maxF) v = 0;
            discoveriesFilter_ = static_cast<DiscoveryFilter>(v);
            discoveriesSel = 0;
        };

        switch (a) {
            case Action::Cancel:
            case Action::Discoveries:
            case Action::Confirm:
                discoveriesOpen = false;
                return;
            case Action::Up:
                discoveriesSel -= 1;
                break;
            case Action::Down:
                discoveriesSel += 1;
                break;
            case Action::LogUp:
                discoveriesSel -= 10;
                break;
            case Action::LogDown:
                discoveriesSel += 10;
                break;
            case Action::Left:
                cycleFilter(-1);
                return;
            case Action::Right:
            case Action::Inventory:
                // Convenient: Tab cycles forward.
                cycleFilter(+1);
                return;
            case Action::SortInventory:
                discoveriesSort_ = (discoveriesSort_ == DiscoverySort::Appearance)
                    ? DiscoverySort::IdentifiedFirst
                    : DiscoverySort::Appearance;
                discoveriesSel = 0;
                return;
            default:
                return;
        }

        if (n <= 0) {
            discoveriesSel = 0;
            return;
        }
        discoveriesSel = clampi(discoveriesSel, 0, n - 1);
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
            case Action::LogUp:
                // Page scroll (uses the same keys as the in-game log scrollback).
                if (!msgHistorySearchMode) msgHistoryScroll = clampi(msgHistoryScroll + 10, 0, maxScroll);
                return;
            case Action::LogDown:
                if (!msgHistorySearchMode) msgHistoryScroll = clampi(msgHistoryScroll - 10, 0, maxScroll);
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

    // Help overlay mode (UI-only; does not consume turns).
    if (helpOpen) {
        switch (a) {
            case Action::Cancel:
            case Action::Inventory:
            case Action::Help:
                helpOpen = false;
                helpScroll_ = 0;
                return;

            case Action::Up:
                helpScroll_ = clampi(helpScroll_ - 1, 0, 100000);
                return;
            case Action::Down:
                helpScroll_ = clampi(helpScroll_ + 1, 0, 100000);
                return;

            case Action::LogUp:
                helpScroll_ = clampi(helpScroll_ - 10, 0, 100000);
                return;
            case Action::LogDown:
                helpScroll_ = clampi(helpScroll_ + 10, 0, 100000);
                return;

            case Action::Confirm:
                // Convenience: jump back to the top.
                helpScroll_ = 0;
                return;

            default:
                return;
        }
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

            case Action::ToggleSoundPreview:
                toggleSoundPreview();
                break;
            case Action::ToggleThreatPreview:
                toggleThreatPreview();
                break;
            case Action::ToggleHearingPreview:
                toggleHearingPreview();
                break;
            case Action::ToggleScentPreview:
                toggleScentPreview();
                break;
            case Action::MinimapZoomIn:
                if (threatPreviewOpen) adjustThreatPreviewHorizon(+1);
                else if (soundPreviewOpen) adjustSoundPreviewVolume(+1);
                else if (hearingPreviewOpen) adjustHearingPreviewVolume(+1);
                else if (scentPreviewOpen) adjustScentPreviewCutoff(+8);
                break;
            case Action::MinimapZoomOut:
                if (threatPreviewOpen) adjustThreatPreviewHorizon(-1);
                else if (soundPreviewOpen) adjustSoundPreviewVolume(-1);
                else if (hearingPreviewOpen) adjustHearingPreviewVolume(-1);
                else if (scentPreviewOpen) adjustScentPreviewCutoff(-8);
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

    // Dig prompt mode (directional).
    // This is a lightweight two-step command: press DIG, then a direction key.
    if (digging) {
        bool spent = false;
        switch (a) {
            case Action::Up:        spent = digInDirection(0, -1); break;
            case Action::Down:      spent = digInDirection(0, 1); break;
            case Action::Left:      spent = digInDirection(-1, 0); break;
            case Action::Right:     spent = digInDirection(1, 0); break;
            case Action::UpLeft:    spent = digInDirection(-1, -1); break;
            case Action::UpRight:   spent = digInDirection(1, -1); break;
            case Action::DownLeft:  spent = digInDirection(-1, 1); break;
            case Action::DownRight: spent = digInDirection(1, 1); break;

            case Action::Inventory:
                digging = false;
                openInventory();
                return;

            case Action::Cancel:
            case Action::Dig:
                digging = false;
                pushMsg("NEVER MIND.", MessageKind::System, true);
                return;

            default:
                // Ignore non-directional input while waiting for a direction.
                return;
        }

        // digInDirection() advances the turn internally (used by extended command too),
        // so we don't call advanceAfterPlayerAction() here.
        if (spent) {
            digging = false;
        }
        return;
    }

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
        bool chestActed = false;

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

            case Action::LogUp:
                moveChestSelection(-10);
                return;

            case Action::LogDown:
                moveChestSelection(10);
                return;

            case Action::Confirm:
                chestActed = chestMoveSelected(true);
                break;

            case Action::Drop:
                chestActed = chestMoveSelected(false);
                break;

            case Action::DropAll:
                chestActed = chestMoveSelected(true);
                break;

            case Action::Pickup:
                chestActed = chestMoveAll();
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

        if (chestActed) {
            advanceAfterPlayerAction();
        }
        return;
    }

    // Spells overlay (does not consume turns except when casting).
    if (spellsOpen) {
        switch (a) {
            case Action::Up:
                moveSpellsSelection(-1);
                return;
            case Action::Down:
                moveSpellsSelection(1);
                return;
            case Action::LogUp:
                moveSpellsSelection(-5);
                return;
            case Action::LogDown:
                moveSpellsSelection(5);
                return;
            case Action::Cancel:
            case Action::Spells:
                closeSpells();
                return;
            case Action::Inventory:
                closeSpells();
                openInventory();
                return;
            case Action::Confirm:
            case Action::Fire: {
                // Close the overlay first so feedback messages are visible.
                spellsOpen = false;

                const auto known = knownSpellsList();
                if (known.empty()) {
                    pushSystemMessage("YOU DON'T KNOW ANY SPELLS.");
                    return;
                }

                const SpellKind sk = selectedSpell();
                const SpellDef& sd = spellDef(sk);
                if (sd.needsTarget) {
                    beginSpellTargeting(sk);
                    return;
                }

                if (castSpell(sk)) {
                    advanceAfterPlayerAction();
                }
                return;
            }
            default:
                return;
        }
    }

    // Inventory mode.
    if (invOpen) {
        switch (a) {
            case Action::Up: moveInventorySelection(-1); break;
            case Action::Down: moveInventorySelection(1); break;
            case Action::LogUp: moveInventorySelection(-10); break;
            case Action::LogDown: moveInventorySelection(10); break;
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

    // Finished runs: ignore turn-consuming actions (movement/combat/etc).
    if (isFinished()) {
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
            // QoL: while targeting, TAB / SHIFT+TAB cycle between visible hostiles.
            // These map to Inventory / ToggleStats by default.
            case Action::Inventory:
                cycleTargetCursor(+1);
                break;
            case Action::ToggleStats:
                cycleTargetCursor(-1);
                break;
            case Action::Confirm:
            case Action::Fire:
                acted = endTargeting(true);
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



    // Fishing fight mini-mode (after hooking a big fish).
    if (fishingFightActive_) {
        auto clearFight = [&]() {
            fishingFightActive_ = false;
            fishingFightRodItemId_ = 0;
            fishingFightFishSeed_ = 0u;
            fishingFightLabel_.clear();
            fishingFightProgress_ = 0;
            fishingFightTension_ = 0;
            fishingFightSafeMin_ = 0;
            fishingFightSafeMax_ = 0;
            fishingFightTurnsLeft_ = 0;
            fishingFightPull_ = 0;
            fishingFightStep_ = 0;
        };

        auto findRodIndex = [&]() -> int {
            for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
                if (inv[static_cast<size_t>(i)].id == fishingFightRodItemId_) return i;
            }
            return -1;
        };

        auto loseFish = [&](const std::string& msg, MessageKind kind) {
            pushMsg(msg, kind, true);
            clearFight();
        };

        auto landFish = [&]() {
            Item fish;
            fish.id = nextItemId++;
            fish.kind = ItemKind::Fish;
            fish.count = 1;
            fish.spriteSeed = fishingFightFishSeed_;
            fish.charges = static_cast<int>(fishingFightFishSeed_);
            fish.enchant = fishingFightFishEnchant_;

            const std::string fishName = itemDisplayName(fish);

            if (inv.size() >= 26) {
                dropGroundItemItem(player().pos, fish);
                pushMsg("YOU LAND " + fishName + "! (PACK FULL - DROPPED)", MessageKind::Loot, true);
            } else {
                inv.push_back(fish);
                pushMsg("YOU LAND " + fishName + "!", MessageKind::Loot, true);
            }

            if (fishIsShinyFromEnchant(fishingFightFishEnchant_) && player().effects.hallucinationTurns == 0) {
                pushMsg("IT GLITTERS.", MessageKind::Success, true);
            }

            clearFight();
        };

        auto stepFight = [&](bool reel) -> bool {
            // If the rod is missing, the fish gets away.
            const int rodIdx = findRodIndex();
            if (rodIdx < 0 || !isFishingRodKind(inv[static_cast<size_t>(rodIdx)].kind)) {
                loseFish("YOUR LINE GOES SLACK.", MessageKind::Warning);
                return false;
            }

            const int prevTension = fishingFightTension_;

            // Fish pull + thrash (deterministic from seed/step).
            const uint32_t h = hash32(hashCombine(fishingFightFishSeed_ ^ 0xA11CE0u, static_cast<uint32_t>(fishingFightStep_)));
            fishingFightStep_++;
            const int delta = static_cast<int>(h % 7u) - 3; // -3..+3
            fishingFightTension_ += fishingFightPull_ + delta;

            // Player control.
            if (reel) {
                const int reelTension = std::max(3, 8 - (playerAgility() / 2));
                fishingFightTension_ += reelTension;

                int gain = 14 + playerFocus() * 2;
                gain -= fishingFightPull_ / 2;
                if (fishingFightTension_ >= fishingFightSafeMin_ && fishingFightTension_ <= fishingFightSafeMax_) gain += 10;
                else gain -= 6;
                gain = clampi(gain, 2, 40);
                fishingFightProgress_ += gain;
            } else {
                const int slack = 12 + (playerAgility() / 2);
                fishingFightTension_ -= slack;
                // If we go too slack, the fish regains ground.
                if (fishingFightTension_ < fishingFightSafeMin_) {
                    fishingFightProgress_ = std::max(0, fishingFightProgress_ - 4);
                }
            }

            fishingFightTurnsLeft_ = std::max(0, fishingFightTurnsLeft_ - 1);

            // Break / escape conditions.
            if (fishingFightTension_ >= 100) {
                // Line snaps: punish the rod a bit.
                Item& rod = inv[static_cast<size_t>(rodIdx)];
                rod.charges = std::max(0, rod.charges - 2);
                if (rod.charges <= 0) {
                    inv.erase(inv.begin() + rodIdx);
                    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
                    pushMsg("YOUR FISHING ROD SNAPS!", MessageKind::Warning, true);
                } else {
                    pushMsg("THE LINE SNAPS!", MessageKind::Warning, true);
                }

                loseFish("THE FISH GETS AWAY.", MessageKind::Info);
                return true;
            }

            if (fishingFightTension_ <= 0) {
                loseFish("THE FISH SLIPS FREE.", MessageKind::Info);
                return true;
            }

            if (fishingFightTurnsLeft_ <= 0) {
                loseFish("THE FISH TIRES OF YOU AND ESCAPES.", MessageKind::Info);
                return true;
            }

            if (fishingFightProgress_ >= 100) {
                landFish();
                return true;
            }

            // Subtle feedback when crossing the safe band boundaries.
            if (prevTension <= fishingFightSafeMax_ && fishingFightTension_ > fishingFightSafeMax_) {
                pushMsg("THE LINE STRAINS.", MessageKind::Info);
            } else if (prevTension >= fishingFightSafeMin_ && fishingFightTension_ < fishingFightSafeMin_) {
                pushMsg("THE LINE GOES SLACK.", MessageKind::Info);
            }

            return true;
        };

        switch (a) {
            case Action::Confirm:
            case Action::Fire:
                acted = stepFight(true);
                break;
            case Action::Wait:
                acted = stepFight(false);
                break;
            case Action::Cancel:
                pushMsg("YOU LET THE LINE GO.", MessageKind::Info, true);
                clearFight();
                acted = false;
                break;
            default:
                // Ignore other inputs while fishing.
                acted = false;
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
        case Action::Dig:
            beginDig();
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
        case Action::ToggleSoundPreview:
            toggleSoundPreview();
            acted = false;
            break;
        case Action::ToggleThreatPreview:
            toggleThreatPreview();
            acted = false;
            break;
        case Action::ToggleHearingPreview:
            toggleHearingPreview();
            acted = false;
            break;
        case Action::ToggleScentPreview:
            toggleScentPreview();
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
        case Action::Evade:
            acted = evadeStep();
            break;
        case Action::Confirm: {
            if (p.pos == dung.stairsDown) {
                if (encumbranceEnabled_ && burdenState() == BurdenState::Overloaded) {
                    pushMsg("YOU ARE OVERLOADED!", MessageKind::Warning, true);
                } else {
                    // Branch-aware: the Camp stairs down always leads into the Main dungeon.
                    if (atCamp()) {
                        changeLevel(LevelId{DungeonBranch::Main, 1}, true);
                    } else {
                        changeLevel(depth_ + 1, true);
                    }
                }
                acted = false;
            } else if (p.pos == dung.stairsUp) {
                // In the surface camp, stairs up is the exit.
                if (atCamp()) {
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
                        // Branch-aware: ascending from Main depth 1 returns to the Camp hub.
                        if (branch_ == DungeonBranch::Main && depth_ <= 1) {
                            changeLevel(LevelId{DungeonBranch::Camp, 0}, false);
                        } else {
                            changeLevel(depth_ - 1, false);
                        }
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
                    // Branch-aware: the Camp stairs down always leads into the Main dungeon.
                    if (atCamp()) {
                        changeLevel(LevelId{DungeonBranch::Main, 1}, true);
                    } else {
                        changeLevel(depth_ + 1, true);
                    }
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
                if (atCamp()) {
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
                    // Branch-aware: ascending from Main depth 1 returns to the Camp hub.
                    if (branch_ == DungeonBranch::Main && depth_ <= 1) {
                        changeLevel(LevelId{DungeonBranch::Camp, 0}, false);
                    } else {
                        changeLevel(depth_ - 1, false);
                    }
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

    pushMsg("YOU WHISTLE SHARPLY...", MessageKind::Info, true);
    emitNoise(p.pos, 14);

    int totalFriendly = 0;
    std::vector<Entity*> callable;
    callable.reserve(8);

    for (auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp <= 0) continue;
        if (!e.friendly) continue;
        if (e.kind == EntityKind::Shopkeeper) continue;
        ++totalFriendly;
        if (e.allyOrder == AllyOrder::Follow || e.allyOrder == AllyOrder::Fetch) {
            callable.push_back(&e);
        }
    }

    if (callable.empty()) {
        if (totalFriendly == 0) {
            pushMsg("...BUT NOTHING ANSWERS.", MessageKind::Info, true);
        } else {
            pushMsg("...BUT YOUR COMPANIONS HOLD THEIR POSITION.", MessageKind::Info, true);
        }
        advanceAfterPlayerAction();
        return;
    }

    // Pull stragglers first.
    std::sort(callable.begin(), callable.end(), [&](Entity* a, Entity* b) {
        return chebyshev(a->pos, p.pos) > chebyshev(b->pos, p.pos);
    });

    int moved = 0;
    int alreadyNear = 0;

    for (Entity* ally : callable) {
        const int dist = chebyshev(ally->pos, p.pos);
        if (dist <= 2) {
            ++alreadyNear;
            continue;
        }

        // Try to call the companion to a nearby free tile.
        Vec2i best = ally->pos;
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
                    // Avoid placing allies directly on stairs when possible.
                    if ((nx == dung.stairsUp.x && ny == dung.stairsUp.y) || (nx == dung.stairsDown.x && ny == dung.stairsDown.y)) {
                        continue;
                    }
                    best = {nx, ny};
                    found = true;
                }
            }
        }

        if (found) {
            ally->pos = best;
            ++moved;
        }
    }

    if (moved > 0) {
        if (moved == 1) pushMsg("A COMPANION COMES RUNNING.", MessageKind::Success, true);
        else pushMsg("YOUR COMPANIONS COME RUNNING.", MessageKind::Success, true);
    } else if (alreadyNear > 0) {
        pushMsg("YOUR COMPANIONS LOOK UP AT YOU.", MessageKind::Info, true);
    } else {
        // No spots nearby (very crowded), or allies were blocked by stairs constraints.
        pushMsg("YOU HEAR FOOTSTEPS, BUT THEY CAN'T REACH YOU.", MessageKind::Info, true);
    }

    advanceAfterPlayerAction();
}

void Game::listen() {
    if (isFinished()) return;

    Entity& p = playerMut();

    // Listening is an intentional, turn-spending action (like searching) but it does NOT make noise.
    // It reports *unseen* creatures that are close enough to be heard through doors/corridors.
    int range = 10 + (playerFocus() / 2);
    if (isSneaking()) range += 2;
    range = clampi(range, 6, 20);

    const int W = dung.width;
    auto idx = [&](int x, int y) { return y * W + x; };

    // Ensure deterministic substrate cache so sound propagation can incorporate
    // material acoustics (moss/dirt dampen; metal/crystal carry).
    dung.ensureMaterials(seed_, branch_, depth_, dungeonMaxDepth());

    const std::vector<int> sound = dung.computeSoundMap(p.pos.x, p.pos.y, range);

    struct DirInfo {
        int count = 0;
        int best = 9999;
    };

    std::array<DirInfo, 8> dirs{};

    auto dirIndex = [&](int dx, int dy) -> int {
        const int sx = (dx > 0) - (dx < 0);
        const int sy = (dy > 0) - (dy < 0);
        // 0:N, 1:NE, 2:E, 3:SE, 4:S, 5:SW, 6:W, 7:NW
        if (sx == 0 && sy < 0) return 0;
        if (sx > 0 && sy < 0) return 1;
        if (sx > 0 && sy == 0) return 2;
        if (sx > 0 && sy > 0) return 3;
        if (sx == 0 && sy > 0) return 4;
        if (sx < 0 && sy > 0) return 5;
        if (sx < 0 && sy == 0) return 6;
        return 7;
    };

    bool heard = false;

    for (const auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp <= 0) continue;
        if (e.friendly) continue;
        if (!dung.inBounds(e.pos.x, e.pos.y)) continue;

        // Shopkeepers are special; unalerted ones effectively "ignore" player noise.
        if (e.kind == EntityKind::Shopkeeper && !e.alerted) continue;

        // Listening only reports things you cannot currently see.
        if (dung.at(e.pos.x, e.pos.y).visible) continue;

        const int d = sound[static_cast<size_t>(idx(e.pos.x, e.pos.y))];
        if (d < 0 || d > range) continue;

        heard = true;
        const int di = dirIndex(e.pos.x - p.pos.x, e.pos.y - p.pos.y);
        DirInfo& info = dirs[static_cast<size_t>(di)];
        info.count += 1;
        info.best = std::min(info.best, d);
    }

    if (!heard) {
        pushMsg("YOU HEAR NOTHING UNUSUAL.", MessageKind::Info, true);
        advanceAfterPlayerAction();
        return;
    }

    struct Out {
        int di = 0;
        int best = 9999;
        int count = 0;
    };

    std::vector<Out> outs;
    outs.reserve(8);
    for (int i = 0; i < 8; ++i) {
        const DirInfo& info = dirs[static_cast<size_t>(i)];
        if (info.count <= 0) continue;
        outs.push_back({i, info.best, info.count});
    }

    std::sort(outs.begin(), outs.end(), [&](const Out& a, const Out& b) {
        // Prefer closest sounds; tie-break with more activity.
        if (a.best != b.best) return a.best < b.best;
        return a.count > b.count;
    });

    static const char* DIRNAMES[8] = {
        "NORTH", "NORTHEAST", "EAST", "SOUTHEAST", "SOUTH", "SOUTHWEST", "WEST", "NORTHWEST"
    };

    const int maxLines = 3;
    const int n = static_cast<int>(outs.size());
    for (int i = 0; i < n && i < maxLines; ++i) {
        const Out& o = outs[static_cast<size_t>(i)];
        const char* strength = (o.best <= 3) ? "CLOSE" : (o.best <= 7) ? "NEARBY" : "FAINT";
        std::string msg = std::string("YOU HEAR ") + strength + " SOUNDS TO THE " + DIRNAMES[o.di] + ".";
        pushMsg(msg, MessageKind::Info, true);
    }

    if (n > maxLines) {
        pushMsg("YOU HEAR OTHER DISTANT SOUNDS.", MessageKind::Info, true);
    }

    advanceAfterPlayerAction();
}

bool Game::throwVoiceAt(Vec2i target) {
    if (isFinished()) return false;

    Entity& p = playerMut();

    // Range scales with Focus (same stat used for searching/wands). Sneaking helps a bit too.
    int range = 8 + playerFocus();
    if (isSneaking()) range += 2;
    range = clampi(range, 6, 22);

    if (!dung.inBounds(target.x, target.y)) {
        pushMsg("THAT'S OUT OF BOUNDS.", MessageKind::Info, true);
        return false;
    }

    const int W = dung.width;
    auto idx = [&](int x, int y) { return y * W + x; };

    // Use the dungeon-aware sound propagation map as the validity check:
    // you can only throw a voice where sound could plausibly travel.
    // Ensure deterministic substrate cache so sound propagation can incorporate
    // material acoustics (moss/dirt dampen; metal/crystal carry).
    dung.ensureMaterials(seed_, branch_, depth_, dungeonMaxDepth());

    const std::vector<int> sound = dung.computeSoundMap(p.pos.x, p.pos.y, range);
    const int dist = sound[static_cast<size_t>(idx(target.x, target.y))];

    if (dist < 0 || dist > range) {
        pushMsg("YOUR VOICE CAN'T REACH THERE.", MessageKind::Info, true);
        return false;
    }

    Vec2i actual = target;

    // Confusion makes the illusion drift.
    if (p.effects.confusionTurns > 0) {
        std::vector<Vec2i> candidates;
        candidates.reserve(25);

        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                Vec2i q{target.x + dx, target.y + dy};
                if (!dung.inBounds(q.x, q.y)) continue;
                const int dd = sound[static_cast<size_t>(idx(q.x, q.y))];
                if (dd < 0 || dd > range) continue;
                candidates.push_back(q);
            }
        }

        if (!candidates.empty()) {
            actual = candidates[static_cast<size_t>(rng.range(0, static_cast<int>(candidates.size()) - 1))];
        }
    }

    pushMsg("YOU THROW YOUR VOICE.", MessageKind::Info, true);

    // A tiny whisper still originates from you; the main apparent sound is at the target.
    emitNoise(p.pos, isSneaking() ? 1 : 2);
    emitNoise(actual, 14);

    if (actual.x != target.x || actual.y != target.y) {
        pushMsg("...IT WOBBLES OFF TARGET.", MessageKind::Info, true);
    }

    advanceAfterPlayerAction();
    return true;
}

void Game::setAlliesOrder(AllyOrder order, bool verbose) {
    int n = 0;
    for (auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp <= 0) continue;
        if (!e.friendly) continue;
        e.allyOrder = order;

        // For STAY/GUARD orders, remember the current tile as an anchor so
        // companions can return after chasing enemies or being displaced.
        if (order == AllyOrder::Stay || order == AllyOrder::Guard) {
            e.allyHomePos = e.pos;
        } else {
            e.allyHomePos = {-1, -1};
        }

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
        target->willFlee = false;
        target->lastKnownPlayerPos = {-1, -1};
        target->lastKnownPlayerAge = 9999;

        ensurePetTraits(*target);

        std::ostringstream ss;
        ss << petDisplayName(*target) << " SEEMS FRIENDLY!";
        const std::string traits = petgen::petTraitList(target->procAffixMask);
        if (!traits.empty()) ss << " (" << traits << ")";
        ss << ".";
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

        // Sneak mode: acting carefully is slower. Model this as occasional extra monster turns
        // (without additional hunger/regen ticks) so stealth has a real tradeoff.
        if (isSneaking() && !isFinished()) {
            // Base: extra turn every 3..6 turns, depending on agility.
            int period = 3 + std::max(0, playerAgility()) / 6;
            if (period < 3) period = 3;
            if (period > 6) period = 6;

            // Heavy armor: clanking makes careful movement slower.
            if (const Item* a = equippedArmor()) {
                if (a->kind == ItemKind::ChainArmor) period -= 1;
                if (a->kind == ItemKind::PlateArmor) period -= 2;
            }

            // Encumbrance stacks with sneaking slowdown when enabled.
            if (encumbranceEnabled_) {
                switch (burdenState()) {
                    case BurdenState::Unburdened: break;
                    case BurdenState::Burdened:   period -= 1; break;
                    case BurdenState::Stressed:   period -= 2; break;
                    case BurdenState::Strained:   period -= 3; break;
                    case BurdenState::Overloaded: period -= 3; break;
                }
            }

            if (period < 2) period = 2;
            if (period > 6) period = 6;

            const int extraSneak = (turnCount % static_cast<uint32_t>(period) == 0u) ? 1 : 0;
            for (int i = 0; i < extraSneak && !isFinished(); ++i) {
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
    e.branch = (e.depth <= 0) ? 0 : 1;
    e.turns = turnCount;
    e.kills = killCount;
    e.level = charLevel;
    e.gold = goldCount();
    e.seed = seed_;

    e.name = playerName_;
    e.playerClass = playerClassIdString();
    e.slot = activeSlot_.empty() ? std::string("default") : activeSlot_;
    e.cause = endCause_;

    // Record NetHack-style conducts kept for the run (if any).
    e.conducts = runConductsTag();
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

// --- Keybinds overlay filtering/search (UI-only) ---

static void utf8PopBackKeybinds(std::string& s) {
    if (s.empty()) return;
    // Remove the last UTF-8 codepoint (best-effort; matches other UI text inputs).
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0u) == 0x80u) {
        --i;
    }
    s.erase(i);
}

void Game::keybindsBuildVisibleIndices(std::vector<int>& out) const {
    out.clear();

    const std::string needle = toLower(trim(keybindsSearch_));
    const int n = static_cast<int>(keybindsDesc_.size());
    out.reserve(n);

    for (int i = 0; i < n; ++i) {
        if (needle.empty()) {
            out.push_back(i);
            continue;
        }

        std::string name = keybindsDesc_[i].first;
        for (char& ch : name) {
            if (ch == '_') ch = ' ';
        }

        const std::string hay = toLower(name + " " + keybindsDesc_[i].second);
        if (hay.find(needle) != std::string::npos) {
            out.push_back(i);
        }
    }
}

void Game::keybindsToggleSearchMode() {
    if (!keybindsOpen) return;
    if (keybindsCapture) return; // don't toggle while capturing a chord
    keybindsSearchMode_ = !keybindsSearchMode_;
}

void Game::keybindsTextInput(const char* utf8) {
    if (!keybindsOpen || !keybindsSearchMode_) return;
    if (!utf8) return;

    // Basic cap so the overlay stays sane.
    if (keybindsSearch_.size() > 120) return;

    keybindsSearch_ += utf8;
    keybindsSel = 0;
    keybindsScroll_ = 0;
}

void Game::keybindsBackspace() {
    if (!keybindsOpen) return;
    if (keybindsSearch_.empty()) return;

    utf8PopBackKeybinds(keybindsSearch_);
    keybindsSel = 0;
    keybindsScroll_ = 0;
}

void Game::keybindsClearSearch() {
    if (!keybindsOpen) return;
    keybindsSearch_.clear();
    keybindsSel = 0;
    keybindsScroll_ = 0;
}

void Game::keybindsUnbindSelected() {
    if (!keybindsOpen) return;
    if (keybindsCapture) return;

    std::vector<int> vis;
    keybindsBuildVisibleIndices(vis);
    const int n = static_cast<int>(vis.size());
    if (n <= 0) {
        pushSystemMessage("NO KEYBINDS MATCH.");
        return;
    }

    keybindsSel = clampi(keybindsSel, 0, n - 1);
    const int idx = vis[keybindsSel];

    if (idx < 0 || idx >= static_cast<int>(keybindsDesc_.size())) {
        pushSystemMessage("INVALID KEYBIND TARGET.");
        return;
    }

    const std::string actionName = keybindsDesc_[idx].first;
    const std::string bindKey = std::string("bind_") + actionName;

    if (settingsPath_.empty()) {
        pushSystemMessage("NO SETTINGS PATH; CANNOT WRITE BIND.");
        return;
    }

    if (!updateIniKey(settingsPath_, bindKey, "none")) {
        pushSystemMessage("FAILED TO WRITE " + bindKey + ".");
        return;
    }

    // Optimistic UI update; main.cpp will reload and refresh the cache shortly.
    keybindsDesc_[idx].second = "none";

    requestKeyBindsReload();
    pushSystemMessage("UNBOUND " + bindKey + ".");
}


