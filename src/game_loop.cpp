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
        if (invOpen || chestOpen || targeting || kicking || digging || helpOpen || looking || minimapOpen || statsOpen || msgHistoryOpen || scoresOpen || codexOpen || discoveriesOpen || levelUpOpen || optionsOpen || keybindsOpen || commandOpen || isFinished()) {
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
            return;
        }

        // Allow certain overlays (codex / discoveries / scores) to use LOG UP/DOWN
        // for their own list navigation.
        if (!(codexOpen || discoveriesOpen || scoresOpen)) {
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
        targetLine.clear();
        targetValid = false;
        targetStatusText_.clear();
        spellsOpen = false;
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
        scoresOpen = false;
        discoveriesOpen = false;

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
