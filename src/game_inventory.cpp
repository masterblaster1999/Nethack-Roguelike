#include "game_internal.hpp"

#include "fishing_gen.hpp"
#include "butcher_gen.hpp"

#include "crafting_gen.hpp"

#include "bounty_gen.hpp"
#include "proc_spells.hpp"
#include "shop_profile_gen.hpp"


static ChestContainer* findChestContainer(std::vector<ChestContainer>& containers, int chestId) {
    for (auto& c : containers) {
        if (c.chestId == chestId) return &c;
    }
    return nullptr;
}

static const ChestContainer* findChestContainer(const std::vector<ChestContainer>& containers, int chestId) {
    for (const auto& c : containers) {
        if (c.chestId == chestId) return &c;
    }
    return nullptr;
}


void Game::openInventory() {
    // Close other overlays
    targeting = false;
    // Cancel any in-progress fishing fight prompt (UI-only).
    fishingFightActive_ = false;
    fishingFightRodItemId_ = 0;
    fishingFightFishSeed_ = 0u;
    fishingFightLabel_.clear();
    helpOpen = false;
    looking = false;
    minimapOpen = false;
    statsOpen = false;
    msgScroll = 0;

    // Close other modal overlays.
    chestOpen = false;
    chestOpenId = 0;
    chestSel = 0;
    chestPaneChest = true;
    chestOpenTier_ = 0;
    chestOpenMaxStacks_ = 0;

    invOpen = true;
    invIdentifyMode = false;
    invEnchantRingMode = false;
    invPrompt_ = InvPromptKind::None;
    invCraftMode = false;
    invCraftFirstId_ = 0;
    invCraftPreviewLines_.clear();
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
}

void Game::closeInventory() {
    invOpen = false;
    invIdentifyMode = false;
    invEnchantRingMode = false;
    invPrompt_ = InvPromptKind::None;
    invCraftMode = false;
    invCraftFirstId_ = 0;
    invCraftPreviewLines_.clear();
}

void Game::beginCrafting() {
    // Requires a Crafting Kit in inventory.
    bool haveKit = false;
    for (const auto& it : inv) {
        if (it.kind == ItemKind::CraftingKit) { haveKit = true; break; }
    }
    if (!haveKit) {
        pushMsg("YOU DON'T HAVE A CRAFTING KIT.", MessageKind::Info, true);
        return;
    }

    // Ensure the inventory overlay is open (beginCrafting can be called from #craft).
    if (!invOpen) openInventory();

    // Cancel other modal inventory prompts.
    invIdentifyMode = false;
    invEnchantRingMode = false;
    invPrompt_ = InvPromptKind::None;

    invCraftMode = true;
    invCraftFirstId_ = 0;
    invCraftPreviewLines_.clear();

    // Move selection to a sensible first ingredient (skip the kit itself).
    int first = -1;
    int eligible = 0;
    for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
        if (!isCraftIngredientKind(inv[static_cast<size_t>(i)].kind)) continue;
        ++eligible;
        if (first < 0) first = i;
    }
    if (first >= 0) invSel = first;
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));

    if (eligible < 2) {
        pushMsg("YOU NEED TWO INGREDIENTS TO CRAFT.", MessageKind::Info, true);
    }

    pushMsg("CRAFTING: SELECT INGREDIENT 1 (ENTER). ESC TO EXIT.", MessageKind::System, true);

    rebuildCraftingPreview();
}

void Game::beginFishing() {
    // Convenience for #fish: start fishing targeting using your first Fishing Rod.
    // Cancel any in-progress fishing fight prompt (UI-only).
    fishingFightActive_ = false;
    fishingFightRodItemId_ = 0;
    fishingFightFishSeed_ = 0u;
    fishingFightLabel_.clear();

    int rodId = 0;
    for (const auto& it : inv) {
        if (it.kind == ItemKind::FishingRod) { rodId = it.id; break; }
    }

    if (rodId == 0) {
        pushMsg("YOU DON'T HAVE A FISHING ROD.", MessageKind::Info, true);
        return;
    }

    beginFishingTargeting(rodId);
}

Game::CraftComputed Game::computeCraftComputed(const Item& a0, const Item& b0) const {
    CraftComputed cc;

    // Room type can act as an implicit "workstation": crafting in themed rooms shifts outcomes
    // while remaining deterministic within a run.
    const RoomType rt = roomTypeAt(dung, player().pos);
    cc.workstation = rt;

    uint32_t envSalt = 0u;
    switch (rt) {
        case RoomType::Armory:     envSalt = 0xA11C0B1Du; break;
        case RoomType::Library:    envSalt = 0x0B00B1E5u; break;
        case RoomType::Laboratory: envSalt = 0x1AB0B0A5u; break;
        case RoomType::Shrine:     envSalt = 0x5A1B1E01u; break;
        case RoomType::Camp:       envSalt = 0xCA9F0001u; break;
        default: break;
    }

    // Mix branch so Camp crafting doesn't accidentally mirror dungeon crafting exactly.
    envSalt ^= static_cast<uint32_t>(branch_) * 0x9E3779B9u;

    const uint32_t craftSeed = seed_ ^ hash32(envSalt);

    const craftgen::Outcome o = craftgen::craft(craftSeed, a0, b0);
    cc.tagA = o.tagA;
    cc.tagB = o.tagB;
    cc.tier = o.tier;
    cc.out = o.out;

    cc.hasByproduct = o.hasByproduct;
    cc.byproduct = o.byproduct;

    Item& out = cc.out;

    // Workstation flavor: slight quality nudges by room type (deterministic).
    auto qualityRoll = [&](uint32_t salt, int pct) -> bool {
        const uint32_t r = hash32(out.spriteSeed ^ salt);
        return (r % 100u) < static_cast<uint32_t>(clampi(pct, 0, 100));
    };

    if (rt == RoomType::Shrine) {
        // Shrines tend to purify bad outcomes and sometimes bless.
        if (out.buc < 0) out.buc = 0;
        if (out.buc == 0 && qualityRoll(0xB1E55EEDu, 30)) out.buc = 1;
    } else if (rt == RoomType::Laboratory) {
        // Labs are potent but risky.
        if (out.buc == 0 && qualityRoll(0x0C0FF0DEu, 20)) out.buc = -1;
        if (out.charges > 0 && qualityRoll(0xC4A26E99u, 35)) out.charges += 1;
    } else if (rt == RoomType::Library) {
        // Libraries: more "clean" outcomes (fewer curses).
        if (out.buc < 0 && qualityRoll(0xFA9E0001u, 60)) out.buc = 0;
    } else if (rt == RoomType::Armory) {
        // Armories: gear is more likely to be well-made.
        if (isWearableGear(out.kind) && out.buc == 0 && qualityRoll(0xA4A0B011u, 35)) out.buc = 1;
        if ((isWeapon(out.kind) || isArmor(out.kind)) && out.enchant == 0 && qualityRoll(0xEAC114E1u, 30)) out.enchant = 1;
    } else if (rt == RoomType::Camp) {
        // Camp crafting is safe: never worsens BUC.
        if (out.buc < 0) out.buc = 0;
    }

    return cc;
}

void Game::recordCraftRecipe(const CraftComputed& cc) {
    const uint32_t sig = cc.out.spriteSeed;
    if (sig == 0u) return;

    for (auto& e : craftRecipeBook_) {
        if (e.sig == sig) {
            e.times += 1;
            return;
        }
    }

    CraftRecipeEntry e;
    e.sig = sig;
    e.outKind = cc.out.kind;
    e.firstTurn = turnCount;
    e.times = 1;
    e.tier = cc.tier;
    e.workstation = cc.workstation;
    e.tagA = cc.tagA;
    e.tagB = cc.tagB;

    craftRecipeBook_.push_back(std::move(e));

    // Cap to keep UI sane (UI-only).
    constexpr size_t kMaxRecipes = 96;
    if (craftRecipeBook_.size() > kMaxRecipes) {
        craftRecipeBook_.erase(craftRecipeBook_.begin(),
                               craftRecipeBook_.begin() + (craftRecipeBook_.size() - kMaxRecipes));
    }
}

void Game::showCraftRecipes() {
    auto wsShort = [](RoomType rt) -> const char* {
        switch (rt) {
            case RoomType::Armory:     return "ARMORY";
            case RoomType::Library:    return "LIBRARY";
            case RoomType::Laboratory: return "LAB";
            case RoomType::Shrine:     return "SHRINE";
            case RoomType::Camp:       return "CAMP";
            default: return "NONE";
        }
    };

    if (craftRecipeBook_.empty()) {
        pushSystemMessage("NO CRAFT RECIPES LEARNED YET.");
        pushSystemMessage("TIP: USE A CRAFTING KIT (#CRAFT) AND COMBINE TWO INGREDIENTS.");
        return;
    }

    std::ostringstream hdr;
    hdr << "CRAFT RECIPES (" << craftRecipeBook_.size() << "):";
    pushSystemMessage(hdr.str());

    const size_t maxShow = 20;
    size_t shown = 0;

    // Show newest-first.
    for (size_t i = craftRecipeBook_.size(); i-- > 0 && shown < maxShow; ) {
        const auto& r = craftRecipeBook_[i];

        std::ostringstream ss;
        ss << "  " << craftgen::sigilName(r.sig);
        ss << " | " << (r.tagA.empty() ? "MUNDANE" : r.tagA);
        ss << " + " << (r.tagB.empty() ? "MUNDANE" : r.tagB);
        ss << " T" << r.tier;
        ss << " @" << wsShort(r.workstation);
        ss << " -> " << displayItemNameSingle(r.outKind);
        if (r.times > 1) ss << " x" << r.times;

        pushSystemMessage(ss.str());
        ++shown;
    }

    if (craftRecipeBook_.size() > maxShow) {
        pushSystemMessage("  ...");
    }

    pushSystemMessage("TIP: DIFFERENT ROOMS ACT AS WORKSTATIONS AND CAN YIELD NEW SIGILS.");
}

void Game::rebuildCraftingPreview() {
    invCraftPreviewLines_.clear();
    if (!invOpen || !invCraftMode) return;

    auto wsName = [](RoomType rt) -> const char* {
        switch (rt) {
            case RoomType::Armory:     return "ARMORY";
            case RoomType::Library:    return "LIBRARY";
            case RoomType::Laboratory: return "LABORATORY";
            case RoomType::Shrine:     return "SHRINE";
            case RoomType::Camp:       return "CAMP";
            default: return "NONE";
        }
    };

    auto wsEffect = [](RoomType rt) -> const char* {
        switch (rt) {
            case RoomType::Armory:     return "+GEAR QUALITY";
            case RoomType::Library:    return "+CLEAN RESULTS";
            case RoomType::Laboratory: return "+POTENCY / +RISK";
            case RoomType::Shrine:     return "+PURIFY / +BLESS";
            case RoomType::Camp:       return "+SAFE";
            default: return "";
        }
    };

    const RoomType rt = roomTypeAt(dung, player().pos);
    {
        std::ostringstream ss;
        ss << "WORKSTATION: " << wsName(rt);
        const char* eff = wsEffect(rt);
        if (eff && eff[0]) ss << " (" << eff << ")";
        invCraftPreviewLines_.push_back(ss.str());
    }

    if (inv.empty() || invSel < 0 || invSel >= static_cast<int>(inv.size())) {
        invCraftPreviewLines_.push_back("NO ITEMS.");
        return;
    }

    const Item& selIt = inv[static_cast<size_t>(invSel)];

    auto essenceLine = [&](const Item& it) -> std::string {
        const craftgen::Essence e = craftgen::essenceFor(it);
        std::ostringstream ss;
        ss << "ESSENCE: " << (e.tag.empty() ? "MUNDANE" : e.tag);
        ss << "  TIER " << e.tier;
        if (e.shiny) ss << " {SHINY}";
        return ss.str();
    };

    auto singleName = [&](const Item& it) -> std::string {
        Item t = it;
        t.count = 1;
        return displayItemName(t);
    };

    auto knownTimesForSig = [&](uint32_t sig) -> int {
        for (const auto& r : craftRecipeBook_) {
            if (r.sig == sig) return r.times;
        }
        return 0;
    };

    if (invCraftFirstId_ == 0) {
        invCraftPreviewLines_.push_back("STEP 1/2: PICK INGREDIENT 1");
        if (!isCraftIngredientKind(selIt.kind)) {
            invCraftPreviewLines_.push_back("SELECTED ITEM IS NOT AN INGREDIENT.");
            return;
        }
        invCraftPreviewLines_.push_back("ING1: " + singleName(selIt));
        invCraftPreviewLines_.push_back(essenceLine(selIt));
        invCraftPreviewLines_.push_back("ENTER: SET ING1");
        return;
    }

    int idxA = findItemIndexById(inv, invCraftFirstId_);
    if (idxA < 0) {
        invCraftFirstId_ = 0;
        invCraftPreviewLines_.push_back("ING1 LOST. PICK A NEW INGREDIENT.");
        invCraftPreviewLines_.push_back("ENTER: SET ING1");
        return;
    }

    const Item& a0 = inv[static_cast<size_t>(idxA)];
    invCraftPreviewLines_.push_back("ING1: " + singleName(a0));
    invCraftPreviewLines_.push_back(essenceLine(a0));
    invCraftPreviewLines_.push_back("STEP 2/2: PICK INGREDIENT 2");

    if (!isCraftIngredientKind(selIt.kind)) {
        invCraftPreviewLines_.push_back("SELECTED ITEM IS NOT AN INGREDIENT.");
        return;
    }

    // Same stack requires at least 2 units.
    if (selIt.id == invCraftFirstId_) {
        if (!isStackable(selIt.kind) || selIt.count < 2) {
            invCraftPreviewLines_.push_back("NEED TWO UNITS TO USE THE SAME STACK TWICE.");
            return;
        }
    }

    const Item& b0 = selIt;
    invCraftPreviewLines_.push_back("ING2: " + singleName(b0));
    invCraftPreviewLines_.push_back(essenceLine(b0));

    const CraftComputed cc = computeCraftComputed(a0, b0);

    invCraftPreviewLines_.push_back("SIGIL: " + craftgen::sigilName(cc.out.spriteSeed));

    const int kt = knownTimesForSig(cc.out.spriteSeed);
    if (kt > 0) {
        invCraftPreviewLines_.push_back("KNOWN: YES (x" + std::to_string(kt) + ")");
    } else {
        invCraftPreviewLines_.push_back("KNOWN: NO");
    }

    Item outNamed = cc.out;
    outNamed.id = 0;
    invCraftPreviewLines_.push_back("RESULT: " + displayItemName(outNamed));

    if (cc.hasByproduct) {
        Item byp = cc.byproduct;
        byp.id = 0;
        invCraftPreviewLines_.push_back("BYPRODUCT: " + displayItemName(byp));
    }


    // Extra outcome details for procedural crafting/forging.
    if (outNamed.kind == ItemKind::RuneTablet) {
        const ProcSpell ps = generateProcSpell(outNamed.spriteSeed);
        std::string runeLine = "RUNE: ";
        runeLine += ps.name;
        if (!ps.tags.empty()) {
            runeLine += " (";
            runeLine += ps.tags;
            runeLine += ")";
        }
        invCraftPreviewLines_.push_back(runeLine);
    } else if (isWearableGear(outNamed.kind)) {
        if (itemIsArtifact(outNamed)) {
            const artifactgen::Power p = artifactgen::artifactPower(outNamed);
            std::string artLine = "ARTIFACT: ";
            artLine += artifactgen::powerTag(p);
            artLine += " — ";
            artLine += artifactgen::powerDesc(p);
            invCraftPreviewLines_.push_back(artLine);
        } else if (outNamed.ego != ItemEgo::None) {
            std::string egoLine = "EGO: ";
            egoLine += egoPrefix(outNamed.ego);
            egoLine += " — ";
            egoLine += egoShortDesc(outNamed.ego);
            invCraftPreviewLines_.push_back(egoLine);
        }
    }


    std::ostringstream tierLine;
    tierLine << "TIER: " << cc.tier;
    invCraftPreviewLines_.push_back(tierLine.str());
}

bool Game::craftCombineById(int itemAId, int itemBId) {
    if (itemAId == 0 || itemBId == 0) return false;

    // Validate the player still has a crafting kit.
    bool haveKit = false;
    for (const auto& it : inv) {
        if (it.kind == ItemKind::CraftingKit) { haveKit = true; break; }
    }
    if (!haveKit) {
        pushMsg("YOU LACK THE TOOLS TO CRAFT.", MessageKind::Warning, true);
        return false;
    }

    const int idxA0 = findItemIndexById(inv, itemAId);
    const int idxB0 = findItemIndexById(inv, itemBId);

    if (idxA0 < 0 || idxB0 < 0) {
        pushMsg("YOUR INGREDIENTS ARE GONE.", MessageKind::Info, true);
        return false;
    }

    const Item a0 = inv[static_cast<size_t>(idxA0)];
    const Item b0 = inv[static_cast<size_t>(idxB0)];

    if (!isCraftIngredientKind(a0.kind) || !isCraftIngredientKind(b0.kind)) {
        pushMsg("THAT CANNOT BE USED AS A CRAFTING INGREDIENT.", MessageKind::Info, true);
        return false;
    }

    // Same stack requires at least 2 units.
    if (itemAId == itemBId) {
        if (!isStackable(a0.kind) || a0.count < 2) {
            pushMsg("YOU NEED TWO OF THOSE.", MessageKind::Info, true);
            return false;
        }
    }

    const CraftComputed cc0 = computeCraftComputed(a0, b0);

    Item out = cc0.out;
    out.id = nextItemId++;
    out.shopPrice = 0;
    out.shopDepth = 0;

    const bool wasKnown = [&]() -> bool {
        for (const auto& r : craftRecipeBook_) {
            if (r.sig == out.spriteSeed) return true;
        }
        return false;
    }();

    // Determine names for messaging before consuming ingredients.
    Item aNamed = a0; aNamed.count = 1;
    Item bNamed = b0; bNamed.count = 1;

    const std::string aName = displayItemName(aNamed);
    const std::string bName = displayItemName(bNamed);

    // Crafting reveals the true nature of what you just made.
    (void)markIdentified(out.kind, false);

    auto recordDebtForConsumedUnit = [&](const Item& it, int units) {
        if (units <= 0) return;
        if (it.shopPrice <= 0 || it.shopDepth <= 0) return;
        const int sd = it.shopDepth;
        if (sd >= 1 && sd <= DUNGEON_MAX_DEPTH) {
            shopDebtLedger_[sd] += it.shopPrice * units;
        }
    };

    // Consume ingredients (1 unit each; 2 units if same stack).
    if (itemAId == itemBId) {
        const int idx = findItemIndexById(inv, itemAId);
        if (idx >= 0) {
            Item& it = inv[static_cast<size_t>(idx)];
            recordDebtForConsumedUnit(it, 2);
            it.count -= 2;
        }
    } else {
        // Remove higher index first so indices remain valid.
        int idxA = findItemIndexById(inv, itemAId);
        int idxB = findItemIndexById(inv, itemBId);
        if (idxA >= 0 && idxB >= 0) {
            const int firstIdx = std::max(idxA, idxB);
            const int secondIdx = std::min(idxA, idxB);

            auto consumeAt = [&](int idx) {
                if (idx < 0 || idx >= static_cast<int>(inv.size())) return;
                Item& it = inv[static_cast<size_t>(idx)];
                recordDebtForConsumedUnit(it, 1);
                if (isStackable(it.kind)) {
                    it.count -= 1;
                } else {
                    it.count = 0;
                }
            };

            consumeAt(firstIdx);
            consumeAt(secondIdx);
        }
    }

    // Remove emptied stackables / non-stackables consumed above.
    inv.erase(std::remove_if(inv.begin(), inv.end(), [](const Item& v) {
        return v.count <= 0;
    }), inv.end());

    // Now add the crafted output.
    // Inventory capacity: crafting consumes 2 items, then produces 1, so it usually fits.
    // However, if both ingredients are stackable and remain in inventory, we may still need a slot.
    const int maxInv = 26;
    bool stacked = tryStackItem(inv, out);
    if (!stacked) {
        if (static_cast<int>(inv.size()) >= maxInv) {
            dropGroundItemItem(player().pos, out);
            pushMsg("YOUR PACK IS FULL; YOU DROP " + displayItemName(out) + ".", MessageKind::Loot, true);
        } else {
            inv.push_back(out);
        }
    }

    // Add deterministic byproduct (if any).
    bool bypDropped = false;
    Item byp;
    if (cc0.hasByproduct) {
        byp = cc0.byproduct;
        byp.id = nextItemId++;
        byp.shopPrice = 0;
        byp.shopDepth = 0;

        bool stackedB = tryStackItem(inv, byp);
        if (!stackedB) {
            if (static_cast<int>(inv.size()) >= maxInv) {
                dropGroundItemItem(player().pos, byp);
                pushMsg("YOUR PACK IS FULL; YOU DROP " + displayItemName(byp) + ".", MessageKind::Loot, true);
                bypDropped = true;
            } else {
                inv.push_back(byp);
            }
        }
    }

    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));

    // Record the recipe in the run's journal (UI-only; not serialized).
    recordCraftRecipe(cc0);

    // Message (include the abstract "essence tags" when present).
    {
        std::ostringstream ss;
        ss << "YOU CRAFT " << displayItemName(out) << " FROM " << aName << " + " << bName << ".";
        pushMsg(ss.str(), MessageKind::Success, true);
    }

    if (cc0.hasByproduct && !bypDropped) {
        pushMsg("BYPRODUCT: YOU HARVEST " + displayItemName(byp) + ".", MessageKind::Loot, true);
    }

    if (!cc0.tagA.empty() || !cc0.tagB.empty()) {
        std::ostringstream ss2;
        ss2 << "ESSENCE: ";
        ss2 << (cc0.tagA.empty() ? "MUNDANE" : cc0.tagA);
        ss2 << " + ";
        ss2 << (cc0.tagB.empty() ? "MUNDANE" : cc0.tagB);
        pushMsg(ss2.str(), MessageKind::System, true);
    }

    {
        std::ostringstream ss3;
        ss3 << "SIGIL: " << craftgen::sigilName(out.spriteSeed);
        if (!wasKnown) ss3 << " {NEW}";
        pushMsg(ss3.str(), MessageKind::System, true);
    }

    rebuildCraftingPreview();
    return true;
}

void Game::moveInventorySelection(int dy) {
    if (inv.empty()) { invSel = 0; return; }
    invSel = clampi(invSel + dy, 0, static_cast<int>(inv.size()) - 1);
    if (invCraftMode) rebuildCraftingPreview();
}


void Game::sortInventory() {
    if (inv.empty()) {
        pushMsg("NOTHING TO SORT.", MessageKind::Info, true);
        return;
    }

    // Remember the currently selected item (by id) so we can restore selection after sort.
    int selectedId = 0;
    if (invSel >= 0 && invSel < static_cast<int>(inv.size())) {
        selectedId = inv[static_cast<size_t>(invSel)].id;
    }

    auto category = [&](const Item& it) -> int {
        // 0 = quest/special
        if (it.kind == ItemKind::AmuletYendor) return 0;

        // 1 = equipped gear
        if (it.id == equipMeleeId || it.id == equipRangedId || it.id == equipArmorId || it.id == equipRing1Id || it.id == equipRing2Id) return 1;

        // 2 = other equipment
        const ItemDef& d = itemDef(it.kind);
        if (d.slot != EquipSlot::None) return 2;

        // 3 = consumables (potions/scrolls)
        if (d.consumable) return 3;

        // 4 = ammo
        if (it.kind == ItemKind::Arrow || it.kind == ItemKind::Rock) return 4;

        // 5 = gold
        if (it.kind == ItemKind::Gold) return 5;

        return 6;
    };

    std::stable_sort(inv.begin(), inv.end(), [&](const Item& a, const Item& b) {
        const int ca = category(a);
        const int cb = category(b);
        if (ca != cb) return ca < cb;

        const std::string na = displayItemName(a);
        const std::string nb = displayItemName(b);
        if (na != nb) return na < nb;

        // Tie-breaker for stability.
        return a.id < b.id;
    });

    if (selectedId != 0) {
        int idx = findItemIndexById(inv, selectedId);
        if (idx >= 0) invSel = idx;
    }
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));

    pushMsg("INVENTORY SORTED.", MessageKind::System, true);
    if (invCraftMode) rebuildCraftingPreview();
}

void Game::sortChestContents(int chestId, int* selInOut) {
    ChestContainer* c = findChestContainer(chestContainers_, chestId);
    if (!c) return;

    if (c->items.empty()) {
        if (selInOut) *selInOut = 0;
        pushMsg("CHEST IS EMPTY.", MessageKind::Info, true);
        return;
    }

    // Remember selection by id (best-effort for stacked items).
    int selectedId = 0;
    if (selInOut && *selInOut >= 0 && *selInOut < static_cast<int>(c->items.size())) {
        selectedId = c->items[static_cast<size_t>(*selInOut)].id;
    }

    auto category = [&](const Item& it) -> int {
        // 0 = quest/special
        if (it.kind == ItemKind::AmuletYendor) return 0;

        // 1 = equipment
        const ItemDef& d = itemDef(it.kind);
        if (d.slot != EquipSlot::None) return 1;

        // 2 = consumables
        if (d.consumable) return 2;

        // 3 = ammo
        if (it.kind == ItemKind::Arrow || it.kind == ItemKind::Rock) return 3;

        // 4 = gold
        if (it.kind == ItemKind::Gold) return 4;

        return 5;
    };

    std::stable_sort(c->items.begin(), c->items.end(), [&](const Item& a, const Item& b) {
        const int ca = category(a);
        const int cb = category(b);
        if (ca != cb) return ca < cb;

        const std::string na = displayItemName(a);
        const std::string nb = displayItemName(b);
        if (na != nb) return na < nb;

        return a.id < b.id;
    });

    if (selInOut) {
        if (selectedId != 0) {
            int idx = findItemIndexById(c->items, selectedId);
            if (idx >= 0) *selInOut = idx;
        }
        *selInOut = clampi(*selInOut, 0, std::max(0, static_cast<int>(c->items.size()) - 1));
    }

    pushMsg("CHEST SORTED.", MessageKind::System, true);
}

bool Game::autoPickupAtPlayer() {
    const Vec2i pos = player().pos;
    const int maxInv = 26;

    if (autoPickup == AutoPickupMode::Off) return false;

    int pickedCount = 0;
    std::vector<std::string> sampleNames;

    // Item mimics: if auto-pickup would grab a bait item, trigger the reveal
    // before collecting anything else (prevents partial pick-ups and lost messages).
    for (size_t i = 0; i < ground.size(); ++i) {
        if (ground[i].pos != pos) continue;
        const Item& it = ground[i].item;
        if (it.shopPrice > 0) continue;
        if (!autoPickupWouldPick(it.kind)) continue;
        if (!itemIsMimicBait(it)) continue;

        Item loot = it;
        setItemMimicBait(loot, false);
        loot.shopPrice = 0;
        loot.shopDepth = 0;

        ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(i));

        revealMimicFromBait(pos, "THE " + displayItemName(loot) + " WAS A MIMIC!", &loot);

        if (autoMode != AutoMoveMode::None) {
            stopAutoMove(true);
            pushMsg("AUTO-MOVE STOPPED (MIMIC!).", MessageKind::Warning, true);
        }
        return true;
    }

    for (size_t i = 0; i < ground.size();) {
        if (ground[i].pos == pos && ground[i].item.shopPrice <= 0 && autoPickupWouldPick(ground[i].item.kind)) {
            Item it = ground[i].item;

            // Merge into existing stacks if possible.
            if (!tryStackItem(inv, it)) {
                if (static_cast<int>(inv.size()) >= maxInv) {
                    // Silent failure (avoid spam while walking).
                    ++i;
                    continue;
                }
                inv.push_back(it);
            }

            ++pickedCount;
            if (sampleNames.size() < 3) sampleNames.push_back(displayItemName(it));

            ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(i));
            continue;
        }
        ++i;
    }

    if (pickedCount <= 0) return false;

    // Aggregate to reduce log spam during auto-travel.
    if (pickedCount == 1) {
        pushMsg("YOU PICK UP " + sampleNames[0] + ".", MessageKind::Loot, true);
    } else {
        std::ostringstream ss;
        ss << "YOU PICK UP " << sampleNames[0];
        if (sampleNames.size() >= 2) ss << ", " << sampleNames[1];
        if (sampleNames.size() >= 3) ss << ", " << sampleNames[2];
        if (pickedCount > static_cast<int>(sampleNames.size())) {
            ss << " (+" << (pickedCount - static_cast<int>(sampleNames.size())) << " MORE)";
        }
        ss << ".";
        pushMsg(ss.str(), MessageKind::Loot, true);
    }

    return true;
}

void Game::revealMimicFromBait(Vec2i baitPos, const std::string& revealMsg, const Item* lootToDrop) {
    pushMsg(revealMsg, MessageKind::Warning, true);

    // A mimic reveal is loud.
    emitNoise(baitPos, 14);

    // Prefer spawning adjacent so we don't overlap the player (bait is interacted with underfoot).
    static const int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    Vec2i spawn = {-1, -1};

    // Randomize direction order a bit.
    int order[8] = {0,1,2,3,4,5,6,7};
    for (int i = 7; i > 0; --i) {
        int j = rng.range(0, i);
        std::swap(order[i], order[j]);
    }
    for (int ii = 0; ii < 8; ++ii) {
        int di = order[ii];
        int nx = baitPos.x + dirs[di][0];
        int ny = baitPos.y + dirs[di][1];
        if (!dung.inBounds(nx, ny)) continue;
        if (!dung.isWalkable(nx, ny)) continue;
        if (entityAt(nx, ny)) continue;
        Vec2i cand{nx, ny};
        if (cand == dung.stairsUp || cand == dung.stairsDown) continue;
        spawn = cand;
        break;
    }

    // Worst-case: if surrounded, shove the player to a nearby free tile and spawn in place.
    if (spawn.x < 0) {
        if (baitPos == player().pos) {
            Vec2i dst = baitPos;
            for (int r = 2; r <= 6 && dst == baitPos; ++r) {
                for (int y = baitPos.y - r; y <= baitPos.y + r; ++y) {
                    for (int x = baitPos.x - r; x <= baitPos.x + r; ++x) {
                        if (!dung.inBounds(x, y)) continue;
                        if (!dung.isWalkable(x, y)) continue;
                        if (entityAt(x, y)) continue;
                        Vec2i cand{x, y};
                        if (cand == dung.stairsUp || cand == dung.stairsDown) continue;
                        dst = cand;
                        break;
                    }
                    if (dst != baitPos) break;
                }
            }
            if (dst != baitPos) {
                playerMut().pos = dst;
                pushMsg("THE MIMIC SHOVES YOU BACK!", MessageKind::Warning, true);
            }
        }
        spawn = baitPos;
    }

    // Spawn using monster factory so scaling stays consistent with normal spawns.
    Entity m = makeMonster(EntityKind::Mimic, spawn, /*groupId=*/0, /*allowGear=*/false);

    // Ambush mimics are slightly tougher than baseline mimics.
    m.hpMax += 2;
    m.hp += 2;

    if (lootToDrop && lootToDrop->id != 0 && lootToDrop->count > 0) {
        Item loot = *lootToDrop;
        // This is real loot, not another mimic trap.
        setItemMimicBait(loot, false);
        loot.shopPrice = 0;
        loot.shopDepth = 0;

        // Tougher mimics tend to masquerade as more valuable items.
        const int value = std::max(0, itemDef(loot.kind).value);
        if (value >= 250) {
            m.hpMax += 4; m.hp += 4;
            m.baseAtk += 1;
            m.baseDef += 1;
        } else if (value >= 120) {
            m.hpMax += 2; m.hp += 2;
            m.baseAtk += 1;
        }

        m.pocketConsumable = loot;
    }

    m.alerted = true;
    m.lastKnownPlayerPos = player().pos;
    m.lastKnownPlayerAge = 0;

    ents.push_back(m);
}


bool Game::openChestAtPlayer() {
    const Vec2i pos = player().pos;

    // Find a closed chest at the player's position.
    GroundItem* chestGi = nullptr;
    for (auto& gi : ground) {
        if (gi.pos == pos && gi.item.kind == ItemKind::Chest) {
            chestGi = &gi;
            break;
        }
    }
    if (!chestGi) return false;

    Item& chest = chestGi->item;

    // Mimic: a fake chest that turns into a monster when you try to open it.
    if (chestMimic(chest)) {
        // Remove the chest first.
        const Vec2i chestPos = chestGi->pos;
        const int chestId = chest.id;
        ground.erase(std::remove_if(ground.begin(), ground.end(), [&](const GroundItem& gi) {
            return gi.pos == chestPos && gi.item.id == chestId;
        }), ground.end());

        revealMimicFromBait(chestPos, "THE CHEST WAS A MIMIC!", nullptr);
        return true; // Opening costs a turn.
    }

    // Locked chest: consume a key or attempt lockpick.
    if (chestLocked(chest)) {
        if (keyCount() > 0) {
            (void)consumeKeys(1);
            setChestLocked(chest, false);
            pushMsg("YOU UNLOCK THE CHEST.", MessageKind::Info, true);
            emitNoise(pos, 10);
        } else if (lockpickCount() > 0) {
            // Lockpicking chance scales with character level, but higher-tier chests are harder.
            float chance = 0.35f + 0.05f * static_cast<float>(charLevel);
            chance -= 0.05f * static_cast<float>(chestTier(chest));

            if (rng.chance(chance)) {
                setChestLocked(chest, false);
                pushMsg("YOU PICK THE CHEST'S LOCK.", MessageKind::Info, true);
                emitNoise(pos, 10);
            } else {
                // Failed pick still costs a turn.
                pushMsg("YOU FAIL TO PICK THE CHEST'S LOCK.", MessageKind::Info, true);
                emitNoise(pos, 10);
                // Chance to break a lockpick.
                float breakChance = 0.10f + 0.05f * static_cast<float>(chestTier(chest));
                if (rng.chance(breakChance)) {
                    (void)consumeLockpicks(1);
                    pushMsg("YOUR LOCKPICK BREAKS!", MessageKind::Warning, true);
                }
                return true;
            }
        } else {
            pushMsg("THE CHEST IS LOCKED.", MessageKind::Info, true);
            return false;
        }
    }

    // Opening the chest consumes a turn.
    pushMsg("YOU OPEN THE CHEST.", MessageKind::Loot, true);
    emitNoise(pos, 12);

    // Trigger trap if present.
    if (chestTrapped(chest)) {
        const TrapKind tk = chestTrapKind(chest);
        setChestTrapped(chest, false);
        setChestTrapKnown(chest, true);

        Entity& p = playerMut();
        switch (tk) {
            case TrapKind::Spike: {
                int dmg = rng.range(2, 5) + std::min(3, depth_ / 2);
                p.hp -= dmg;
                std::ostringstream ss;
                ss << "A NEEDLE TRAP JABS YOU! YOU TAKE " << dmg << ".";
                pushMsg(ss.str(), MessageKind::Combat, false);
                if (p.hp <= 0) {
                    pushMsg("YOU DIE.", MessageKind::Combat, false);
                    if (endCause_.empty()) endCause_ = "KILLED BY CHEST TRAP";
                    gameOver = true;
                }
                break;
            }
            case TrapKind::PoisonDart: {
                int dmg = rng.range(1, 2);
                p.hp -= dmg;
                p.effects.poisonTurns = std::max(p.effects.poisonTurns, rng.range(6, 12));
                std::ostringstream ss;
                ss << "POISON NEEDLES HIT YOU! YOU TAKE " << dmg << ".";
                pushMsg(ss.str(), MessageKind::Combat, false);
                pushMsg("YOU ARE POISONED!", MessageKind::Warning, false);
                if (p.hp <= 0) {
                    pushMsg("YOU DIE.", MessageKind::Combat, false);
                    if (endCause_.empty()) endCause_ = "KILLED BY POISON CHEST TRAP";
                    gameOver = true;
                }
                break;
            }
            case TrapKind::Teleport: {
                pushMsg("A TELEPORT GLYPH FLARES FROM THE CHEST!", MessageKind::Warning, false);
                Vec2i dst = dung.randomFloor(rng, true);
                for (int tries = 0; tries < 200; ++tries) {
                    dst = dung.randomFloor(rng, true);
                    if (!entityAt(dst.x, dst.y) && dst != dung.stairsUp && dst != dung.stairsDown) break;
                }
                p.pos = dst;
                recomputeFov();
                break;
            }
            case TrapKind::Alarm: {
                pushMsg("AN ALARM BLARES FROM THE CHEST!", MessageKind::Warning, false);
                // The alarm reveals the chest's location to the whole floor.
                alertMonstersTo(pos, 0);
                break;
            }
            case TrapKind::Web: {
                const int turns = rng.range(4, 7) + std::min(6, depth_ / 2);
                p.effects.webTurns = std::max(p.effects.webTurns, turns);
                pushMsg("STICKY WEBBING EXPLODES OUT OF THE CHEST!", MessageKind::Warning, true);
                break;
            }
            case TrapKind::ConfusionGas: {
                const int turns = rng.range(8, 14) + std::min(6, depth_ / 2);
                p.effects.confusionTurns = std::max(p.effects.confusionTurns, turns);
                pushMsg("A NOXIOUS GAS BURSTS FROM THE CHEST!", MessageKind::Warning, true);
                pushMsg("YOU FEEL CONFUSED!", MessageKind::Warning, true);
                emitNoise(pos, 8);
                break;
            }
            case TrapKind::PoisonGas: {
                const int turns = rng.range(6, 10) + std::min(6, depth_ / 2);
                p.effects.poisonTurns = std::max(p.effects.poisonTurns, turns);
                pushMsg("A CLOUD OF TOXIC VAPOR BURSTS FROM THE CHEST!", MessageKind::Warning, true);
                pushMsg("YOU ARE POISONED!", MessageKind::Warning, true);
                emitNoise(pos, 8);
                break;
            }
            case TrapKind::CorrosiveGas: {
                const int turns = rng.range(6, 10) + std::min(6, depth_ / 2);
                p.effects.corrosionTurns = std::max(p.effects.corrosionTurns, turns);
                pushMsg("A HISSING CLOUD OF ACRID VAPOR BURSTS FROM THE CHEST!", MessageKind::Warning, true);
                emitNoise(pos, 8);
                break;
            }
            default:
                break;
        }
    }

    if (gameOver) {
        // Don't generate loot if the trap killed the player.
        return true;
    }

    const int tier = chestTier(chest);

    // Ensure this chest has an associated container entry.
    ChestContainer* cont = findChestContainer(chestContainers_, chest.id);
    if (!cont) {
        chestContainers_.push_back(ChestContainer{ chest.id, {} });
        cont = &chestContainers_.back();
    } else {
        // Defensive: closed chests shouldn't have saved containers, but older saves might.
        cont->items.clear();
    }

    // Loot: generate gold + a few items based on tier and depth into the chest.
    // If the chest stack-limit is exceeded, we spill the overflow to the ground so loot is never lost.
    const int stackLimit = chestStackLimitForTier(tier);

    auto addItemToChest = [&](ItemKind k, int count = 1, int enchant = 0) {
        Item it;
        it.id = nextItemId++;
        it.kind = k;
        it.count = std::max(1, count);
        it.spriteSeed = rng.nextU32();
        it.enchant = enchant;

        const ItemDef& d = itemDef(k);
        if (d.maxCharges > 0) it.charges = d.maxCharges;

        // Roll BUC (blessed/uncursed/cursed) for gear; and light enchant chance on deeper floors.
        if (isWearableGear(k)) {
            const RoomType rt = roomTypeAt(dung, pos);
            it.buc = rollBucForGear(rng, depth_, rt);

            if (it.enchant == 0 && depth_ >= 3) {
                float enchChance = 0.15f;
                if (rt == RoomType::Treasure || rt == RoomType::Vault || rt == RoomType::Secret) enchChance += 0.10f;
                if (rt == RoomType::Lair) enchChance -= 0.05f;
                enchChance = std::max(0.05f, std::min(0.35f, enchChance));

                if (rng.chance(enchChance)) {
                    it.enchant = 1;
                    if (depth_ >= 6 && rng.chance(0.08f)) {
                        it.enchant = 2;
                    }
                }
            }
        }

        // Try to merge into existing stacks inside the chest.
        if (!tryStackItem(cont->items, it)) {
            if (static_cast<int>(cont->items.size()) < stackLimit) {
                cont->items.push_back(it);
            } else {
                // Last-resort fallback so we never delete generated loot.
                ground.push_back({ it, pos });
            }
        }
    };

    int goldBase = rng.range(8, 16) + depth_ * 4;
    if (tier == 1) goldBase = static_cast<int>(goldBase * 1.5f);
    if (tier >= 2) goldBase = goldBase * 2;
    addItemToChest(ItemKind::Gold, goldBase);

    int rolls = 1 + tier;
    if (depth_ >= 4 && rng.chance(0.50f)) rolls += 1;

    for (int r = 0; r < rolls; ++r) {
        int roll = rng.range(0, 143);

        if (roll < 16) {
            // Weapons
            // Weighted: swords/axes are most common, pickaxes are rarer.
            int wroll = rng.range(0, 99);
            ItemKind wk = (wroll < 45) ? ItemKind::Sword : (wroll < 80) ? ItemKind::Axe : ItemKind::Pickaxe;
            int ench = (rng.chance(0.25f + 0.10f * tier)) ? rng.range(1, 1 + tier) : 0;
            addItemToChest(wk, 1, ench);
        } else if (roll < 34) {
            // Armor
            ItemKind ak = (roll < 26) ? ItemKind::ChainArmor : ItemKind::PlateArmor;
            int ench = (rng.chance(0.25f + 0.10f * tier)) ? rng.range(1, 1 + tier) : 0;
            addItemToChest(ak, 1, ench);
        } else if (roll < 38) {
            // Rings (rare)
            int rr = rng.range(0, 99);
            ItemKind rk = ItemKind::RingProtection;
            if (rr < 28) rk = ItemKind::RingProtection;
            else if (rr < 50) rk = ItemKind::RingMight;
            else if (rr < 70) rk = ItemKind::RingAgility;
            else if (rr < 85) rk = ItemKind::RingFocus;
            else if (rr < 95) rk = ItemKind::RingSearching;
            else rk = ItemKind::RingSustenance;
            int ench = (rng.chance(0.20f + 0.08f * tier)) ? rng.range(1, 1 + tier) : 0;
            addItemToChest(rk, 1, ench);
        } else if (roll < 48) {
            ItemKind wk;
            if (depth_ >= 6 && tier >= 1 && rng.chance(0.12f)) {
                wk = ItemKind::WandFireball;
            } else {
                wk = rng.chance(0.30f) ? ItemKind::WandDigging : ItemKind::WandSparks;
            }
            addItemToChest(wk, 1);
        } else if (roll < 60) {
            addItemToChest(ItemKind::PotionStrength, rng.range(1, 2));
        } else if (roll < 78) {
            addItemToChest(ItemKind::PotionHealing, rng.range(1, 2));
        } else if (roll < 90) {
            addItemToChest(ItemKind::PotionAntidote, rng.range(1, 2));
        } else if (roll < 100) {
            addItemToChest(ItemKind::PotionRegeneration, 1);
        } else if (roll < 108) {
            addItemToChest(ItemKind::PotionShielding, 1);
        } else if (roll < 116) {
            addItemToChest(ItemKind::PotionHaste, 1);
        } else if (roll < 124) {
            const ItemKind pk = rng.chance(0.25f) ? ItemKind::PotionInvisibility : ItemKind::PotionVision;
            addItemToChest(pk, 1);
        } else if (roll < 128) {
            addItemToChest(ItemKind::ScrollMapping, 1);
        } else if (roll < 132) {
            addItemToChest(ItemKind::ScrollTeleport, 1);
        } else if (roll < 134) {
            addItemToChest(ItemKind::ScrollEnchantWeapon, 1);
        } else if (roll < 136) {
            addItemToChest(ItemKind::ScrollEnchantArmor, 1);
        } else if (roll < 138) {
            addItemToChest(ItemKind::ScrollEnchantRing, 1);
        } else if (roll < 142) {
            addItemToChest(ItemKind::ScrollRemoveCurse, 1);
        } else {
            int pick = rng.range(0, 3);
            ItemKind sk = (pick == 0) ? ItemKind::ScrollIdentify
                                      : (pick == 1) ? ItemKind::ScrollDetectTraps
                                      : (pick == 2) ? ItemKind::ScrollDetectSecrets
                                                    : ItemKind::ScrollKnock;
            addItemToChest(sk, 1);
        }
    }

    // Mark chest as opened and render it differently.
    chest.kind = ItemKind::ChestOpen;
    chest.charges = CHEST_FLAG_OPENED;

    // Auto-open the chest container UI unless a trap moved the player away.
    if (player().pos == pos) {
        chestOpen = true;
        chestOpenId = chest.id;
        chestSel = 0;
        chestPaneChest = true;
        chestOpenTier_ = tier;
        chestOpenMaxStacks_ = stackLimit;

        chestSel = clampi(chestSel, 0, std::max(0, static_cast<int>(cont->items.size()) - 1));
        invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
    }

    return true;
}

const std::vector<Item>& Game::chestOpenItems() const {
    static const std::vector<Item> empty;
    if (!chestOpen || chestOpenId == 0) return empty;

    const ChestContainer* c = findChestContainer(chestContainers_, chestOpenId);
    if (!c) return empty;
    return c->items;
}

bool Game::openChestOverlayAtPlayer() {
    if (gameOver || gameWon) return false;

    const Vec2i pos = player().pos;

    GroundItem* giChest = nullptr;
    for (auto& gi : ground) {
        if (gi.pos == pos && gi.item.kind == ItemKind::ChestOpen) {
            giChest = &gi;
            break;
        }
    }
    if (!giChest) return false;

    Item& chest = giChest->item;
    const int tier = chestTier(chest);

    // Ensure a container entry exists so open chests can be used as a stash even if
    // they were opened in an older save (before containers existed).
    ChestContainer* cont = findChestContainer(chestContainers_, chest.id);
    if (!cont) {
        chestContainers_.push_back(ChestContainer{ chest.id, {} });
        cont = &chestContainers_.back();
    }

    chestOpen = true;
    chestOpenId = chest.id;
    chestSel = clampi(chestSel, 0, std::max(0, static_cast<int>(cont->items.size()) - 1));
    chestPaneChest = true;
    chestOpenTier_ = tier;
    chestOpenMaxStacks_ = chestStackLimitForTier(tier);

    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
    msgScroll = 0;

    return true;
}

void Game::closeChestOverlay() {
    chestOpen = false;
    chestOpenId = 0;
    chestSel = 0;
    chestPaneChest = true;
    chestOpenTier_ = 0;
    chestOpenMaxStacks_ = 0;
}

void Game::moveChestSelection(int dy) {
    if (!chestOpen) return;

    if (chestPaneChest) {
        const int n = static_cast<int>(chestOpenItems().size());
        if (n <= 0) { chestSel = 0; return; }
        chestSel = clampi(chestSel + dy, 0, n - 1);
    } else {
        const int n = static_cast<int>(inv.size());
        if (n <= 0) { invSel = 0; return; }
        invSel = clampi(invSel + dy, 0, n - 1);
    }
}

bool Game::chestMoveSelected(bool moveAll) {
    if (!chestOpen || chestOpenId == 0) return false;

    const int maxInv = 26;

    // Ensure container exists.
    ChestContainer* cont = findChestContainer(chestContainers_, chestOpenId);
    if (!cont) {
        chestContainers_.push_back(ChestContainer{ chestOpenId, {} });
        cont = &chestContainers_.back();
    }
    auto& chestItems = cont->items;

    auto isEquipped = [&](int itemId) -> bool {
        return itemId != 0 && (itemId == equipMeleeId || itemId == equipRangedId || itemId == equipArmorId ||
                               itemId == equipRing1Id || itemId == equipRing2Id);
    };

    const int chestLimit = (chestOpenMaxStacks_ > 0) ? chestOpenMaxStacks_ : chestStackLimitForTier(chestOpenTier_);

    if (chestPaneChest) {
        if (chestItems.empty()) {
            pushMsg("CHEST IS EMPTY.", MessageKind::Info, true);
            return false;
        }

        chestSel = clampi(chestSel, 0, static_cast<int>(chestItems.size()) - 1);
        const Item& src = chestItems[static_cast<size_t>(chestSel)];

        Item moved = src;
        const bool splitOne = (!moveAll && isStackable(moved.kind) && moved.count > 1);
        if (splitOne) {
            moved.count = 1;
        }

        // Can we add this to the inventory?
        bool stacked = tryStackItem(inv, moved);
        if (!stacked) {
            if (static_cast<int>(inv.size()) >= maxInv) {
                pushMsg("YOUR PACK IS FULL.", MessageKind::Info, true);
                return false;
            }
            // If we split a stack (moving only one unit), ensure the moved stack has a unique id.
            if (splitOne) moved.id = nextItemId++;
            inv.push_back(moved);
        }

        // Remove from chest.
        if (!moveAll && isStackable(src.kind) && chestItems[static_cast<size_t>(chestSel)].count > 1) {
            chestItems[static_cast<size_t>(chestSel)].count -= 1;
        } else {
            chestItems.erase(chestItems.begin() + chestSel);
        }

        if (chestSel >= static_cast<int>(chestItems.size())) {
            chestSel = std::max(0, static_cast<int>(chestItems.size()) - 1);
        }

        pushMsg("YOU TAKE " + displayItemName(moved) + ".", MessageKind::Loot, true);
        return true;
    } else {
        if (inv.empty()) {
            pushMsg("YOU HAVE NOTHING TO STASH.", MessageKind::Info, true);
            return false;
        }

        invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
        const Item& src = inv[static_cast<size_t>(invSel)];

        if (src.shopPrice > 0) {
            pushMsg("YOU CAN'T STASH UNPAID GOODS.", MessageKind::Warning, true);
            return false;
        }

        if (isEquipped(src.id) && src.buc < 0) {
            pushMsg("YOU CAN'T LET GO OF CURSED GEAR.", MessageKind::Warning, true);
            return false;
        }

        Item moved = src;
        const bool splitOne = (!moveAll && isStackable(moved.kind) && moved.count > 1);
        if (splitOne) {
            moved.count = 1;
        }

        // Can we add this to the chest?
        bool stacked = tryStackItem(chestItems, moved);
        if (!stacked) {
            if (static_cast<int>(chestItems.size()) >= chestLimit) {
                pushMsg("THE CHEST IS FULL.", MessageKind::Info, true);
                return false;
            }
            // If we split a stack (moving only one unit), ensure the moved stack has a unique id.
            if (splitOne) moved.id = nextItemId++;
            chestItems.push_back(moved);
        }

        // Remove from inventory (and unequip if needed).
        if (!moveAll && isStackable(src.kind) && inv[static_cast<size_t>(invSel)].count > 1) {
            inv[static_cast<size_t>(invSel)].count -= 1;
        } else {
            if (src.id == equipMeleeId) equipMeleeId = 0;
            if (src.id == equipRangedId) equipRangedId = 0;
            if (src.id == equipArmorId) equipArmorId = 0;
            if (src.id == equipRing1Id) equipRing1Id = 0;
            if (src.id == equipRing2Id) equipRing2Id = 0;
            inv.erase(inv.begin() + invSel);
        }

        if (invSel >= static_cast<int>(inv.size())) {
            invSel = std::max(0, static_cast<int>(inv.size()) - 1);
        }

        pushMsg("YOU PUT " + displayItemName(moved) + " IN THE CHEST.", MessageKind::Loot, true);
        return true;
    }
}

bool Game::chestMoveAll() {
    if (!chestOpen || chestOpenId == 0) return false;

    const int maxInv = 26;

    ChestContainer* cont = findChestContainer(chestContainers_, chestOpenId);
    if (!cont) {
        chestContainers_.push_back(ChestContainer{ chestOpenId, {} });
        cont = &chestContainers_.back();
    }
    auto& chestItems = cont->items;

    auto isEquipped = [&](int itemId) -> bool {
        return itemId != 0 && (itemId == equipMeleeId || itemId == equipRangedId || itemId == equipArmorId ||
                               itemId == equipRing1Id || itemId == equipRing2Id);
    };

    const int chestLimit = (chestOpenMaxStacks_ > 0) ? chestOpenMaxStacks_ : chestStackLimitForTier(chestOpenTier_);

    bool movedAny = false;

    if (chestPaneChest) {
        // Take everything from the chest.
        size_t i = 0;
        while (i < chestItems.size()) {
            Item moved = chestItems[i];

            bool stacked = tryStackItem(inv, moved);
            if (!stacked) {
                if (static_cast<int>(inv.size()) >= maxInv) break;
                inv.push_back(moved);
            }

            chestItems.erase(chestItems.begin() + i);
            movedAny = true;
        }

        if (!movedAny) {
            if (chestItems.empty()) pushMsg("CHEST IS EMPTY.", MessageKind::Info, true);
            else pushMsg("YOUR PACK IS FULL.", MessageKind::Info, true);
            return false;
        }

        pushMsg("YOU LOOT THE CHEST.", MessageKind::Loot, true);
    } else {
        // Put everything (except equipped/unpaid) into the chest.
        size_t i = 0;
        while (i < inv.size()) {
            const Item& src = inv[i];

            if (isEquipped(src.id) || src.shopPrice > 0) {
                ++i;
                continue;
            }

            Item moved = src;

            bool stacked = tryStackItem(chestItems, moved);
            if (!stacked) {
                if (static_cast<int>(chestItems.size()) >= chestLimit) {
                    ++i;
                    continue;
                }
                chestItems.push_back(moved);
            }

            inv.erase(inv.begin() + i);
            movedAny = true;
        }

        if (!movedAny) {
            pushMsg("NOTHING TO STASH.", MessageKind::Info, true);
            return false;
        }

        pushMsg("YOU STASH YOUR SUPPLIES.", MessageKind::Loot, true);
    }

    chestSel = clampi(chestSel, 0, std::max(0, static_cast<int>(chestItems.size()) - 1));
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
    return true;
}

bool Game::pickupAtPlayer() {
    Vec2i ppos = player().pos;

    std::vector<size_t> idxs;
    for (size_t i = 0; i < ground.size(); ++i) {
        if (ground[i].pos == ppos) idxs.push_back(i);
    }
    if (idxs.empty()) {
        pushMsg("NOTHING HERE.", MessageKind::Info, true);
        return false;
    }

    // Chests are not pick-up items.
    bool hasPickable = false;
    for (size_t gi : idxs) {
        if (gi < ground.size() && !isChestKind(ground[gi].item.kind)) {
            hasPickable = true;
            break;
        }
    }
    if (!hasPickable) {
        pushMsg("NOTHING TO PICK UP.", MessageKind::Info, true);
        return false;
    }

    // Item mimics: some bait items turn into a Mimic when you try to pick them up.
    // If present on this tile, trigger the reveal before picking anything else.
    for (size_t gi : idxs) {
        if (gi >= ground.size()) continue;
        const Item& it = ground[gi].item;
        if (isChestKind(it.kind)) continue;
        if (it.shopPrice > 0) continue; // should never happen (we do not seed shop mimics)
        if (!itemIsMimicBait(it)) continue;

        Item loot = it;
        setItemMimicBait(loot, false);
        loot.shopPrice = 0;
        loot.shopDepth = 0;

        ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(gi));

        revealMimicFromBait(ppos, "THE " + displayItemName(loot) + " WAS A MIMIC!", &loot);
        return true;
    }

    const int maxInv = 26;
    bool pickedAny = false;

    // Pick up in reverse order so erase indices stay valid.
    for (size_t k = idxs.size(); k-- > 0;) {
        size_t gi = idxs[k];
        if (gi >= ground.size()) continue;

        Item it = ground[gi].item;

        if (isChestKind(it.kind)) {
            // Skip non-pickable world items.
            continue;
        }

        std::string msg;
        const bool inShop = playerInShop();
        const bool isShopStockHere = (inShop && it.shopPrice > 0 && it.shopDepth == depth_);
        if (isShopStockHere && anyPeacefulShopkeeper(ents, playerId_)) {
            const int cost = totalShopPrice(it);
            Item named = it;
            named.shopPrice = 0;
            named.shopDepth = 0;

            if (spendGoldFromInv(inv, cost)) {
                it.shopPrice = 0;
                it.shopDepth = 0;
                msg = "YOU BUY " + displayItemName(it) + " FOR " + std::to_string(cost) + " GOLD.";
            } else {
                // Not enough gold: you can still pick up, but you now OWE the shop.
                msg = "YOU PICK UP " + displayItemName(named) + ". YOU OWE " + std::to_string(cost) + " GOLD.";
            }
        } else {
            msg = "YOU PICK UP " + displayItemName(it) + ".";
        }

        if (tryStackItem(inv, it)) {
            // stacked
            pickedAny = true;
            pushMsg(msg, MessageKind::Loot, true);
            if (it.kind == ItemKind::AmuletYendor) {
                pushMsg("YOU HAVE FOUND THE AMULET OF YENDOR! RETURN TO THE EXIT (<) TO WIN.", MessageKind::Success, true);
                onAmuletAcquired();
            }
            ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(gi));
            continue;
        }

        if (static_cast<int>(inv.size()) >= maxInv) {
            pushMsg("YOUR PACK IS FULL.", MessageKind::Warning, true);
            break;
        }

        inv.push_back(it);
        pickedAny = true;
        pushMsg(msg, MessageKind::Loot, true);
        if (it.kind == ItemKind::AmuletYendor) {
            pushMsg("YOU HAVE FOUND THE AMULET OF YENDOR! RETURN TO THE EXIT (<) TO WIN.", MessageKind::Success, true);
            onAmuletAcquired();
        }
        ground.erase(ground.begin() + static_cast<std::vector<GroundItem>::difference_type>(gi));
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

    // Cursed equipped items can't be removed/dropped (NetHack-style "welded" gear).
    if (it.buc < 0 && (it.id == equipMeleeId || it.id == equipRangedId || it.id == equipArmorId || it.id == equipRing1Id || it.id == equipRing2Id)) {
        if (it.id == equipMeleeId) pushMsg("YOUR WEAPON IS CURSED AND WELDED TO YOUR HAND!", MessageKind::Warning, true);
        else if (it.id == equipRangedId) pushMsg("YOUR RANGED WEAPON IS CURSED AND WON'T LET GO!", MessageKind::Warning, true);
        else if (it.id == equipArmorId) pushMsg("YOUR ARMOR IS CURSED AND WON'T COME OFF!", MessageKind::Warning, true);
        else pushMsg("YOUR RING IS CURSED AND STUCK TO YOUR FINGER!", MessageKind::Warning, true);
        return false;
    }

    // Unequip if needed
    if (it.id == equipMeleeId) equipMeleeId = 0;
    if (it.id == equipRangedId) equipRangedId = 0;
    if (it.id == equipArmorId) equipArmorId = 0;
    if (it.id == equipRing1Id) equipRing1Id = 0;
    if (it.id == equipRing2Id) equipRing2Id = 0;

    Item drop = it;
    if (isStackable(it.kind) && it.count > 1) {
        drop.count = 1;
        it.count -= 1;
    } else {
        // remove whole item
        inv.erase(inv.begin() + invSel);
        invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
    }

    std::string msg;
    const bool inShop = playerInShop();
    const bool peacefulShop = inShop && anyPeacefulShopkeeper(ents, playerId_);

    if (inShop && drop.shopPrice > 0 && drop.shopDepth == depth_) {
        // Returning unpaid goods to the same shop reduces your debt automatically.
        Item named = drop;
        named.shopPrice = 0;
        named.shopDepth = 0;
        msg = "YOU RETURN " + displayItemName(named) + ".";
    } else if (peacefulShop && drop.shopPrice <= 0 && itemCanBeSoldToShop(drop)) {
        const Room* shopRoom = shopgen::shopRoomAt(dung, player().pos);
        const shopgen::ShopProfile prof = shopRoom ? shopgen::profileFor(seed_, depth_, *shopRoom) : shopgen::ShopProfile{};

        const int basePerUnit = shopSellPricePerUnit(drop, depth_);
        const int perUnit = shopgen::adjustedShopSellPricePerUnit(basePerUnit, prof, drop);
        const int gold = std::max(0, perUnit) * stackUnitsForPrice(drop);
        if (gold > 0) gainGoldToInv(inv, gold, nextItemId, rng);

        // The shop now owns the item and will resell it.
        const int baseBuy = shopBuyPricePerUnit(drop, depth_);
        drop.shopPrice = shopgen::adjustedShopBuyPricePerUnit(baseBuy, prof, drop);
        drop.shopDepth = depth_;

        Item named = drop;
        named.shopPrice = 0;
        named.shopDepth = 0;
        msg = "YOU SELL " + displayItemName(named) + " FOR " + std::to_string(gold) + " GOLD.";
    } else {
        msg = "YOU DROP " + displayItemName(drop) + ".";
    }

    // If you're somehow standing over a chasm (levitation), dropped items should fall.
    // This avoids leaving unreachable loot on a non-walkable tile.
    const Vec2i pos = player().pos;
    if (dung.inBounds(pos.x, pos.y) && dung.at(pos.x, pos.y).type == TileType::Chasm) {
        pushMsg(msg, MessageKind::Loot, true);
        pushMsg("IT FALLS INTO THE CHASM!", MessageKind::Warning, true);
        return true;
    }

    // Use the shared ground-drop helper so stackables merge and item ids remain unique.
    dropGroundItemItem(pos, drop);

    pushMsg(msg);
    if (invCraftMode) rebuildCraftingPreview();
    return true;
}

bool Game::dropSelectedAll() {
    if (inv.empty()) {
        pushMsg("NOTHING TO DROP.");
        return false;
    }

    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
    Item& it = inv[static_cast<size_t>(invSel)];

    // Cursed equipped items can't be removed/dropped (NetHack-style "welded" gear).
    if (it.buc < 0 && (it.id == equipMeleeId || it.id == equipRangedId || it.id == equipArmorId || it.id == equipRing1Id || it.id == equipRing2Id)) {
        if (it.id == equipMeleeId) pushMsg("YOUR WEAPON IS CURSED AND WELDED TO YOUR HAND!", MessageKind::Warning, true);
        else if (it.id == equipRangedId) pushMsg("YOUR RANGED WEAPON IS CURSED AND WON'T LET GO!", MessageKind::Warning, true);
        else if (it.id == equipArmorId) pushMsg("YOUR ARMOR IS CURSED AND WON'T COME OFF!", MessageKind::Warning, true);
        else pushMsg("YOUR RING IS CURSED AND STUCK TO YOUR FINGER!", MessageKind::Warning, true);
        return false;
    }

    // Unequip if needed
    if (it.id == equipMeleeId) equipMeleeId = 0;
    if (it.id == equipRangedId) equipRangedId = 0;
    if (it.id == equipArmorId) equipArmorId = 0;
    if (it.id == equipRing1Id) equipRing1Id = 0;
    if (it.id == equipRing2Id) equipRing2Id = 0;

    Item drop = it;

    // Remove whole item/stack.
    inv.erase(inv.begin() + invSel);
    invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));

    std::string msg;
    const bool inShop = playerInShop();
    const bool peacefulShop = inShop && anyPeacefulShopkeeper(ents, playerId_);

    if (inShop && drop.shopPrice > 0 && drop.shopDepth == depth_) {
        Item named = drop;
        named.shopPrice = 0;
        named.shopDepth = 0;
        msg = "YOU RETURN " + displayItemName(named) + ".";
    } else if (peacefulShop && drop.shopPrice <= 0 && itemCanBeSoldToShop(drop)) {
        const Room* shopRoom = shopgen::shopRoomAt(dung, player().pos);
        const shopgen::ShopProfile prof = shopRoom ? shopgen::profileFor(seed_, depth_, *shopRoom) : shopgen::ShopProfile{};

        const int basePerUnit = shopSellPricePerUnit(drop, depth_);
        const int perUnit = shopgen::adjustedShopSellPricePerUnit(basePerUnit, prof, drop);
        const int gold = std::max(0, perUnit) * stackUnitsForPrice(drop);
        if (gold > 0) gainGoldToInv(inv, gold, nextItemId, rng);

        const int baseBuy = shopBuyPricePerUnit(drop, depth_);
        drop.shopPrice = shopgen::adjustedShopBuyPricePerUnit(baseBuy, prof, drop);
        drop.shopDepth = depth_;

        Item named = drop;
        named.shopPrice = 0;
        named.shopDepth = 0;
        msg = "YOU SELL " + displayItemName(named) + " FOR " + std::to_string(gold) + " GOLD.";
    } else {
        msg = "YOU DROP " + displayItemName(drop) + ".";
    }

    const Vec2i pos = player().pos;
    if (dung.inBounds(pos.x, pos.y) && dung.at(pos.x, pos.y).type == TileType::Chasm) {
        pushMsg(msg, MessageKind::Loot, true);
        pushMsg("IT FALLS INTO THE CHASM!", MessageKind::Warning, true);
        return true;
    }

    // Use the shared ground-drop helper so stackables merge and item ids remain unique.
    dropGroundItemItem(pos, drop);

    pushMsg(msg);
    if (invCraftMode) rebuildCraftingPreview();
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

    auto equippedItemCursed = [&](int id) -> bool {
        if (id == 0) return false;
        const int idx = findItemIndexById(inv, id);
        if (idx < 0) return false;
        return inv[static_cast<size_t>(idx)].buc < 0;
    };

    if (d.slot == EquipSlot::MeleeWeapon) {
        if (equipMeleeId == it.id) {
            if (it.buc < 0) {
                pushMsg("YOUR WEAPON IS CURSED AND WELDED TO YOUR HAND!", MessageKind::Warning, true);
                return false;
            }
            equipMeleeId = 0;
            pushMsg("YOU UNWIELD " + displayItemName(it) + ".");
        } else {
            if (equippedItemCursed(equipMeleeId)) {
                pushMsg("YOUR CURSED WEAPON WON'T LET GO!", MessageKind::Warning, true);
                return false;
            }
            equipMeleeId = it.id;
            pushMsg("YOU WIELD " + displayItemName(it) + ".");
        }
        return true;
    }

    if (d.slot == EquipSlot::RangedWeapon) {
        if (equipRangedId == it.id) {
            if (it.buc < 0) {
                pushMsg("YOUR RANGED WEAPON IS CURSED AND WON'T LET GO!", MessageKind::Warning, true);
                return false;
            }
            equipRangedId = 0;
            pushMsg("YOU UNEQUIP " + displayItemName(it) + ".");
        } else {
            if (equippedItemCursed(equipRangedId)) {
                pushMsg("YOUR CURSED RANGED WEAPON WON'T LET GO!", MessageKind::Warning, true);
                return false;
            }
            equipRangedId = it.id;
            pushMsg("YOU READY " + displayItemName(it) + ".");
        }
        return true;
    }

    if (d.slot == EquipSlot::Armor) {
        if (equipArmorId == it.id) {
            if (it.buc < 0) {
                pushMsg("YOUR ARMOR IS CURSED AND WON'T COME OFF!", MessageKind::Warning, true);
                return false;
            }
            equipArmorId = 0;
            pushMsg("YOU REMOVE " + displayItemName(it) + ".");
        } else {
            if (equippedItemCursed(equipArmorId)) {
                pushMsg("YOUR CURSED ARMOR WON'T COME OFF!", MessageKind::Warning, true);
                return false;
            }
            equipArmorId = it.id;
            pushMsg("YOU WEAR " + displayItemName(it) + ".");
        }
        return true;
    }

    if (d.slot == EquipSlot::Ring) {
        auto ringNameById = [&](int id) -> std::string {
            if (id == 0) return std::string("(NONE)");
            const int idx = findItemIndexById(inv, id);
            if (idx < 0) return std::string("(MISSING)");
            return displayItemName(inv[static_cast<size_t>(idx)]);
        };

        auto removeRing = [&](int& slotId) -> bool {
            if (slotId == 0) return false;
            const int idx = findItemIndexById(inv, slotId);
            if (idx < 0) {
                slotId = 0;
                return true;
            }
            const Item& worn = inv[static_cast<size_t>(idx)];
            if (worn.buc < 0) {
                pushMsg("YOUR RING IS CURSED AND STUCK TO YOUR FINGER!", MessageKind::Warning, true);
                return false;
            }
            slotId = 0;
            pushMsg("YOU REMOVE " + displayItemName(worn) + ".");
            return true;
        };

        // Toggle off if the selected ring is already worn.
        if (equipRing1Id == it.id) {
            return removeRing(equipRing1Id);
        }
        if (equipRing2Id == it.id) {
            return removeRing(equipRing2Id);
        }

        // Prefer an empty slot.
        if (equipRing1Id == 0) {
            equipRing1Id = it.id;
            pushMsg("YOU PUT ON " + displayItemName(it) + ".");
            (void)markIdentified(it.kind, false);
            return true;
        }
        if (equipRing2Id == 0) {
            equipRing2Id = it.id;
            pushMsg("YOU PUT ON " + displayItemName(it) + ".");
            (void)markIdentified(it.kind, false);
            return true;
        }

        // Both slots are filled: replace the first removable ring.
        if (!equippedItemCursed(equipRing1Id)) {
            std::string oldName = ringNameById(equipRing1Id);
            equipRing1Id = it.id;
            pushMsg("YOU SWAP " + oldName + " FOR " + displayItemName(it) + ".");
            (void)markIdentified(it.kind, false);
            return true;
        }
        if (!equippedItemCursed(equipRing2Id)) {
            std::string oldName = ringNameById(equipRing2Id);
            equipRing2Id = it.id;
            pushMsg("YOU SWAP " + oldName + " FOR " + displayItemName(it) + ".");
            (void)markIdentified(it.kind, false);
            return true;
        }

        pushMsg("BOTH YOUR RINGS ARE CURSED AND WON'T BUDGE!", MessageKind::Warning, true);
        return false;
    }

    pushMsg("YOU CAN'T EQUIP THAT.");
    return false;
}


static bool canButcherWith(ItemKind k) {
    return k == ItemKind::Dagger || k == ItemKind::Sword || k == ItemKind::Axe || k == ItemKind::Pickaxe;
}

bool Game::butcherSelected() {
    if (invSel < 0 || invSel >= static_cast<int>(inv.size())) {
        pushMsg("NOTHING TO BUTCHER.", MessageKind::Bad);
        return false;
    }

    Item* tool = equippedMelee();
    if (!tool || !canButcherWith(tool->kind)) {
        pushMsg("YOU NEED A SHARP TOOL EQUIPPED TO BUTCHER.", MessageKind::Bad);
        return false;
    }

    Item corpse = inv[invSel];
    if (!isCorpseKind(corpse.kind)) {
        pushMsg("THAT IS NOT A CORPSE.", MessageKind::Bad);
        return false;
    }

    const uint32_t baseSeed = (corpse.spriteSeed != 0u)
        ? corpse.spriteSeed
        : hash32(static_cast<uint32_t>(corpse.id) ^ 0xB007C0DEu);

    // Include tool kind so different tools carve the same corpse differently (deterministically).
    const uint32_t seed = hash32(baseSeed
        ^ (static_cast<uint32_t>(corpse.count) * 0x9E3779B9u)
        ^ (static_cast<uint32_t>(tool->kind) * 0x85EBCA6Bu));

    const auto y = butchergen::generate(corpse.kind, seed, corpse.charges, tool->kind);

    // Consume exactly one corpse from the stack.
    if (inv[invSel].count > 1) {
        inv[invSel].count -= 1;
    } else {
        inv.erase(inv.begin() + invSel);
        if (invSel >= static_cast<int>(inv.size())) invSel = static_cast<int>(inv.size()) - 1;
    }

    const int noise = clampi(itemDef(corpse.kind).weight / 4, 6, 16);
    emitNoise(player().pos, noise);

    {
        std::ostringstream ss;
        ss << "YOU BUTCHER THE " << itemDef(corpse.kind).name << ".";
        pushMsg(ss.str(), MessageKind::Loot);
    }

    if (y.meat.empty()) {
        pushMsg("YOU CAN'T SALVAGE ANY EDIBLE MEAT.", MessageKind::Bad);
    }

    static constexpr int maxInv = 26;

    auto grantOrDrop = [&](Item out) {
        if (out.count <= 0) return;
        out.id = nextItemId++;
        out.shopPrice = 0;
        out.shopDepth = 0;
        out.ownerId = 0;

        if (!tryStackItem(inv, out)) {
            if (static_cast<int>(inv.size()) < maxInv) {
                inv.push_back(out);
            } else {
                dropGroundItemItem(player().pos, out);
                std::ostringstream ss;
                ss << "YOUR PACK IS FULL. YOU DROP " << itemDisplayName(out) << ".";
                pushMsg(ss.str(), MessageKind::Bad);
            }
        }
    };

    auto meatSpriteSeed = [&](int cutId, int tagId) -> uint32_t {
        const uint32_t base = hash32(seed ^ 0x4D454154u ^ (static_cast<uint32_t>(cutId) * 0x9E3779B9u) ^ (static_cast<uint32_t>(tagId) * 0x85EBCA6Bu)); // 'MEAT'
        const uint32_t lo = static_cast<uint32_t>((cutId & 0xF) | ((tagId & 0xF) << 4));
        return (base & ~0xFFu) | lo;
    };

    auto materialSpriteSeed = [&](uint32_t domain, int variant, int quality) -> uint32_t {
        const int qTier = butcherQualityTierFromQuality(quality);
        const uint32_t base = hash32(seed ^ domain ^ (static_cast<uint32_t>(variant) * 0x9E3779B9u) ^ (static_cast<uint32_t>(quality) * 0x85EBCA6Bu));
        const uint32_t lo = static_cast<uint32_t>((variant & 0xF) | ((qTier & 0xF) << 4));
        return (base & ~0xFFu) | lo;
    };

    // Meat stacks
    for (const auto& ms : y.meat) {
        if (ms.pieces <= 0) continue;

        Item meat;
        meat.kind = ItemKind::ButcheredMeat;
        meat.count = ms.pieces;
        meat.charges = corpse.charges;
        meat.enchant = packButcherMeatEnchant(
            ms.hungerPerPiece,
            ms.healPerPiece,
            static_cast<int>(corpse.kind),
            butchergen::tagIndex(ms.tag),
            butchergen::cutIndex(ms.cut));
        meat.spriteSeed = meatSpriteSeed(butchergen::cutIndex(ms.cut), butchergen::tagIndex(ms.tag));
        grantOrDrop(meat);
    }

    // Hide
    if (y.hidePieces > 0) {
        Item hide;
        hide.kind = ItemKind::ButcheredHide;
        hide.count = y.hidePieces;
        hide.enchant = packButcherMaterialEnchant(static_cast<int>(corpse.kind), y.hideQuality, butchergen::hideTypeIndex(y.hideType));
        hide.spriteSeed = materialSpriteSeed(0x48494445u, butchergen::hideTypeIndex(y.hideType), y.hideQuality); // 'HIDE'
        grantOrDrop(hide);
    }

    // Bones
    if (y.bonePieces > 0) {
        Item bones;
        bones.kind = ItemKind::ButcheredBones;
        bones.count = y.bonePieces;
        bones.enchant = packButcherMaterialEnchant(static_cast<int>(corpse.kind), y.boneQuality, butchergen::boneTypeIndex(y.boneType));
        bones.spriteSeed = materialSpriteSeed(0x424F4E45u, butchergen::boneTypeIndex(y.boneType), y.boneQuality); // 'BONE'
        grantOrDrop(bones);
    }

    return true;
}

bool Game::butcherAtFeetOrPrompt() {
    Item* tool = equippedMelee();
    if (!tool || !canButcherWith(tool->kind)) {
        pushMsg("YOU NEED A SHARP TOOL EQUIPPED TO BUTCHER.", MessageKind::Bad);
        return false;
    }

    const Vec2i ppos = player().pos;

    // Prefer a corpse at your feet (freshest first).
    int bestIdx = -1;
    int bestFresh = -999999;
    for (int i = 0; i < static_cast<int>(ground.size()); ++i) {
        if (ground[i].pos == ppos && isCorpseKind(ground[i].item.kind)) {
            if (ground[i].item.charges > bestFresh) {
                bestFresh = ground[i].item.charges;
                bestIdx = i;
            }
        }
    }

    if (bestIdx >= 0) {
        // Butcher one corpse from the ground stack.
        GroundItem gi = ground[bestIdx];
        Item corpse = gi.item;
        corpse.count = 1;

        if (ground[bestIdx].item.count > 1) {
            ground[bestIdx].item.count -= 1;
        } else {
            ground.erase(ground.begin() + bestIdx);
        }

        const uint32_t baseSeed = (corpse.spriteSeed != 0u)
            ? corpse.spriteSeed
            : hash32(static_cast<uint32_t>(corpse.id) ^ 0xB007C0DEu);

        // Domain-separated, and tool-dependent.
        const uint32_t seed = hash32(baseSeed ^ 0xC0DEC0DEu ^ (static_cast<uint32_t>(tool->kind) * 0x85EBCA6Bu));

        const auto y = butchergen::generate(corpse.kind, seed, corpse.charges, tool->kind);

        const int noise = clampi(itemDef(corpse.kind).weight / 4, 6, 16);
        emitNoise(player().pos, noise);

        {
            std::ostringstream ss;
            ss << "YOU BUTCHER THE " << itemDef(corpse.kind).name << ".";
            pushMsg(ss.str(), MessageKind::Loot);
        }

        if (y.meat.empty()) {
            pushMsg("YOU CAN'T SALVAGE ANY EDIBLE MEAT.", MessageKind::Bad);
        }

        static constexpr int maxInv = 26;
        auto grantOrDrop = [&](Item out) {
            if (out.count <= 0) return;
            out.id = nextItemId++;
            out.shopPrice = 0;
            out.shopDepth = 0;
            out.ownerId = 0;
            if (!tryStackItem(inv, out)) {
                if (static_cast<int>(inv.size()) < maxInv) {
                    inv.push_back(out);
                } else {
                    dropGroundItemItem(ppos, out);
                    std::ostringstream ss;
                    ss << "YOUR PACK IS FULL. YOU DROP " << itemDisplayName(out) << ".";
                    pushMsg(ss.str(), MessageKind::Bad);
                }
            }
        };

        auto meatSpriteSeed = [&](int cutId, int tagId) -> uint32_t {
            const uint32_t base = hash32(seed ^ 0x4D454154u ^ (static_cast<uint32_t>(cutId) * 0x9E3779B9u) ^ (static_cast<uint32_t>(tagId) * 0x85EBCA6Bu));
            const uint32_t lo = static_cast<uint32_t>((cutId & 0xF) | ((tagId & 0xF) << 4));
            return (base & ~0xFFu) | lo;
        };

        auto materialSpriteSeed = [&](uint32_t domain, int variant, int quality) -> uint32_t {
            const int qTier = butcherQualityTierFromQuality(quality);
            const uint32_t base = hash32(seed ^ domain ^ (static_cast<uint32_t>(variant) * 0x9E3779B9u) ^ (static_cast<uint32_t>(quality) * 0x85EBCA6Bu));
            const uint32_t lo = static_cast<uint32_t>((variant & 0xF) | ((qTier & 0xF) << 4));
            return (base & ~0xFFu) | lo;
        };

        for (const auto& ms : y.meat) {
            if (ms.pieces <= 0) continue;

            Item meat;
            meat.kind = ItemKind::ButcheredMeat;
            meat.count = ms.pieces;
            meat.charges = corpse.charges;
            meat.enchant = packButcherMeatEnchant(
                ms.hungerPerPiece,
                ms.healPerPiece,
                static_cast<int>(corpse.kind),
                butchergen::tagIndex(ms.tag),
                butchergen::cutIndex(ms.cut));
            meat.spriteSeed = meatSpriteSeed(butchergen::cutIndex(ms.cut), butchergen::tagIndex(ms.tag));
            grantOrDrop(meat);
        }

        if (y.hidePieces > 0) {
            Item hide;
            hide.kind = ItemKind::ButcheredHide;
            hide.count = y.hidePieces;
            hide.enchant = packButcherMaterialEnchant(static_cast<int>(corpse.kind), y.hideQuality, butchergen::hideTypeIndex(y.hideType));
            hide.spriteSeed = materialSpriteSeed(0x48494445u, butchergen::hideTypeIndex(y.hideType), y.hideQuality);
            grantOrDrop(hide);
        }

        if (y.bonePieces > 0) {
            Item bones;
            bones.kind = ItemKind::ButcheredBones;
            bones.count = y.bonePieces;
            bones.enchant = packButcherMaterialEnchant(static_cast<int>(corpse.kind), y.boneQuality, butchergen::boneTypeIndex(y.boneType));
            bones.spriteSeed = materialSpriteSeed(0x424F4E45u, butchergen::boneTypeIndex(y.boneType), y.boneQuality);
            grantOrDrop(bones);
        }

        return true;
    }

    // Otherwise butcher from inventory (prompt if multiple).
    std::vector<int> corpseIdx;
    for (int i = 0; i < static_cast<int>(inv.size()); ++i) {
        if (isCorpseKind(inv[i].kind)) corpseIdx.push_back(i);
    }

    if (corpseIdx.empty()) {
        pushMsg("NO CORPSES TO BUTCHER.", MessageKind::Bad);
        return false;
    }

    if (corpseIdx.size() == 1) {
        invSel = corpseIdx[0];
        return butcherSelected();
    }

    openInventory();
    invPrompt_ = InvPromptKind::Butcher;
    invSel = corpseIdx[0];
    pushMsg("SELECT A CORPSE TO BUTCHER.", MessageKind::Info);
    return false;
}
bool Game::useSelected() {
    if (inv.empty()) {
        pushMsg("NOTHING TO USE.", MessageKind::Info, true);
        return false;
    }
    invSel = clampi(invSel, 0, static_cast<int>(inv.size()) - 1);
    Item& it = inv[static_cast<size_t>(invSel)];

    auto consumeOneStackable = [&]() {
        if (!isStackable(it.kind)) return;

        // Using up unpaid shop goods still leaves you owing the shopkeeper.
        // Record the per-unit cost into the shop debt ledger before consuming.
        if (it.shopPrice > 0 && it.shopDepth > 0) {
            const int sd = it.shopDepth;
            if (sd >= 1 && sd <= DUNGEON_MAX_DEPTH) {
                shopDebtLedger_[sd] += it.shopPrice;
            }
        }

        it.count -= 1;
        if (it.count <= 0) {
            inv.erase(inv.begin() + invSel);
            invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
        }
    };

    auto consumeOneNonStackable = [&]() {
        if (invSel < 0 || invSel >= static_cast<int>(inv.size())) return;

        // Reading/using an unpaid item still leaves you owing the shopkeeper.
        if (it.shopPrice > 0 && it.shopDepth > 0) {
            const int sd = it.shopDepth;
            if (sd >= 1 && sd <= DUNGEON_MAX_DEPTH) {
                shopDebtLedger_[sd] += it.shopPrice;
            }
        }

        inv.erase(inv.begin() + invSel);
        invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));
    };


    // ------------------------------------------------------------
    // Capture spheres (Palworld/Pokemon-like companion system)
    // ------------------------------------------------------------
    // Empty spheres open a targeter and only consume a turn once thrown.
    if (isCaptureSphereEmptyKind(it.kind)) {
        beginCaptureTargeting(it.id);
        return false;
    }

    // Full spheres: recall if the matching companion is already out; otherwise target a tile to release.
    if (isCaptureSphereFullKind(it.kind)) {
        const int rawKind = it.enchant;
        if (rawKind < 0 || rawKind >= ENTITY_KIND_COUNT) {
            pushMsg("THE SPHERE FEELS WRONG.", MessageKind::Warning, true);
            return false;
        }
        const EntityKind k = static_cast<EntityKind>(rawKind);
        const uint32_t seed = it.spriteSeed;

        if (seed != 0u) {
            for (size_t i = 0; i < ents.size(); ++i) {
                Entity& e = ents[i];
                if (e.hp <= 0) continue;
                if (!e.friendly) continue;
                if (e.id == player().id) continue;
                if (e.kind != k) continue;
                if (e.spriteSeed != seed) continue;

                const int hpPct = (e.hpMax > 0) ? clampi((e.hp * 100 + e.hpMax / 2) / e.hpMax, 0, 100) : 0;
                it.charges = withCaptureSphereHpPct(it.charges, hpPct);

                std::ostringstream ss;
                ss << "YOU RECALL " << petGivenNameFor(e) << ".";
                pushMsg(ss.str(), MessageKind::Info, true);

                // Remove without killing (no corpse/loot).
                ents.erase(ents.begin() + static_cast<long>(i));
                return true;
            }
        }

        // Not currently out: place the companion by targeting a valid tile.
        beginCaptureTargeting(it.id);
        return false;
    }

    // Fishing rods: open a targeter (cast only consumes a turn on release).
    if (it.kind == ItemKind::FishingRod) {
        beginFishingTargeting(it.id);
        return false;
    }

    // Bounty contracts: show progress, and pay out once complete.
    if (it.kind == ItemKind::BountyContract) {
        const int rawTarget = bountyTargetKindFromCharges(it.charges);
        EntityKind target = EntityKind::Goblin;
        if (rawTarget >= 0 && rawTarget < ENTITY_KIND_COUNT) target = static_cast<EntityKind>(rawTarget);

        const int req = clampi(bountyRequiredKillsFromCharges(it.charges), 1, 255);
        const int prog = clampi(bountyProgressFromEnchant(it.enchant), 0, 255);
        const int shown = std::min(req, prog);

        if (shown < req) {
            std::ostringstream ss;
            ss << "BOUNTY: KILL " << req << " " << bountygen::pluralizeEntityName(target, req)
               << " (" << shown << "/" << req << ").";
            pushMsg(ss.str(), MessageKind::Info, true);
            return false;
        }

        // Completed: pay out deterministically from the stored contract data.
        const int rawReward = bountyRewardKindFromCharges(it.charges);
        ItemKind rewardK = ItemKind::Gold;
        if (rawReward >= 0 && rawReward < ITEM_KIND_COUNT) rewardK = static_cast<ItemKind>(rawReward);

        int rewardC = clampi(bountyRewardCountFromCharges(it.charges), 0, 255);
        if (rewardC <= 0) rewardC = 1;

        if (rewardK == ItemKind::Gold) {
            Item gold;
            gold.id = nextItemId++;
            gold.kind = ItemKind::Gold;
            gold.count = rewardC;
            gold.spriteSeed = rng.nextU32();

            if (!tryStackItem(inv, gold)) inv.push_back(gold);

            std::ostringstream ss;
            ss << "GUILD PAYS YOU " << rewardC << " GOLD.";
            pushMsg(ss.str(), MessageKind::Success, true);
        } else {
            Item reward;
            reward.id = nextItemId++;
            reward.kind = rewardK;
            reward.count = (isStackable(rewardK) ? rewardC : 1);
            reward.spriteSeed = rng.nextU32();

            const ItemDef& rd = itemDef(rewardK);
            if (rd.maxCharges > 0) reward.charges = rd.maxCharges;

            if (!tryStackItem(inv, reward)) inv.push_back(reward);

            std::ostringstream ss;
            ss << "BOUNTY REDEEMED. YOU RECEIVE " << itemDisplayName(reward) << ".";
            pushMsg(ss.str(), MessageKind::Success, true);
        }

        consumeOneNonStackable();
        return true;
    }

    // ------------------------------------------------------------
    // Rune Tablets (procedural rune magic)
    // ------------------------------------------------------------
    // Rune Tablets encode a deterministic procedural spell id in spriteSeed. When used,
    // they either cast immediately (self/ward spells) or open the targeting overlay.
    if (it.kind == ItemKind::RuneTablet) {
        uint32_t procId = it.spriteSeed;
        if (procId == 0u) procId = hash32(static_cast<uint32_t>(it.id) ^ 0x52C39A7Bu);

        const ProcSpell ps = generateProcSpell(procId);

        std::string reason;
        if (!canCastProcSpell(procId, &reason)) {
            if (!reason.empty()) pushMsg(reason + ".", MessageKind::Warning, true);
            return false;
        }

        if (ps.needsTarget) {
            beginRuneTabletTargeting(it.id);
            return false; // targeting will consume the turn on cast
        }

        const bool casted = castProcSpell(procId);
        if (casted) {
            consumeOneNonStackable();
            return true;
        }
        return false;
    }

    if (it.kind == ItemKind::PotionHealing) {
        Entity& p = playerMut();        int heal = itemDef(it.kind).healAmount;
        int before = p.hp;
        p.hp = std::min(p.hpMax, p.hp + heal);

        std::ostringstream ss;
        ss << "YOU DRINK A POTION. HP " << before << "->" << p.hp << ".";
        pushMsg(ss.str(), MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionStrength) {
        // Potions can be blessed/uncursed/cursed; reflect that in how many talent points are gained.
        int delta = 1;
        if (it.buc > 0) delta = 2;
        else if (it.buc < 0) delta = -1;

        talentMight_ = clampi(talentMight_ + delta, -5, 50);

        std::ostringstream ss;
        if (delta > 0) {
            ss << "YOU FEEL STRONGER! MIGHT IS NOW " << talentMight_ << ".";
            pushMsg(ss.str(), MessageKind::Success, true);
        } else {
            ss << "YOU FEEL WEAKER... MIGHT IS NOW " << talentMight_ << ".";
            pushMsg(ss.str(), MessageKind::Warning, true);
        }

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionEnergy) {
        const int maxMana = std::max(0, playerManaMax());
        int before = mana_;

        int gain = std::max(2, maxMana / 2);
        if (it.buc > 0) gain = maxMana;
        else if (it.buc < 0) gain = std::max(1, maxMana / 4);

        mana_ = clampi(mana_ + gain, 0, maxMana);

        std::ostringstream ss;
        if (it.buc > 0) {
            ss << "ARCANE POWER SURGES THROUGH YOU! MANA " << before << "->" << mana_ << ".";
            pushMsg(ss.str(), MessageKind::Success, true);
        } else if (it.buc < 0) {
            ss << "THE POTION TASTES FLAT... MANA " << before << "->" << mana_ << ".";
            pushMsg(ss.str(), MessageKind::Info, true);
        } else {
            ss << "YOU FEEL ENERGIZED. MANA " << before << "->" << mana_ << ".";
            pushMsg(ss.str(), MessageKind::Info, true);
        }

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    // Spellbooks (WIP): learn a spell and consume the book.
    if (it.kind == ItemKind::SpellbookMagicMissile || it.kind == ItemKind::SpellbookBlink ||
        it.kind == ItemKind::SpellbookMinorHeal || it.kind == ItemKind::SpellbookDetectTraps ||
        it.kind == ItemKind::SpellbookFireball || it.kind == ItemKind::SpellbookStoneskin ||
        it.kind == ItemKind::SpellbookHaste || it.kind == ItemKind::SpellbookInvisibility ||
        it.kind == ItemKind::SpellbookPoisonCloud) {

        SpellKind sk = SpellKind::MagicMissile;
        switch (it.kind) {
            case ItemKind::SpellbookMagicMissile: sk = SpellKind::MagicMissile; break;
            case ItemKind::SpellbookBlink: sk = SpellKind::Blink; break;
            case ItemKind::SpellbookMinorHeal: sk = SpellKind::MinorHeal; break;
            case ItemKind::SpellbookDetectTraps: sk = SpellKind::DetectTraps; break;
            case ItemKind::SpellbookFireball: sk = SpellKind::Fireball; break;
            case ItemKind::SpellbookStoneskin: sk = SpellKind::Stoneskin; break;
            case ItemKind::SpellbookHaste: sk = SpellKind::Haste; break;
            case ItemKind::SpellbookInvisibility: sk = SpellKind::Invisibility; break;
            case ItemKind::SpellbookPoisonCloud: sk = SpellKind::PoisonCloud; break;
            default: break;
        }

        const uint32_t idx = static_cast<uint32_t>(sk);
        const uint32_t bit = (idx < 32u) ? (1u << idx) : 0u;
        const bool already = (bit != 0u) && ((knownSpellsMask_ & bit) != 0u);

        if (!already && bit != 0u) {
            knownSpellsMask_ |= bit;
            std::ostringstream ss;
            ss << "YOU LEARN " << spellName(sk) << ".";
            pushMsg(ss.str(), MessageKind::Success, true);
        } else {
            pushMsg("YOU STUDY THE BOOK, BUT LEARN NOTHING NEW.", MessageKind::Info, true);
        }

        consumeOneNonStackable();
        return true;
    }


    if (it.kind == ItemKind::ScrollTeleport) {
        // Teleport to a random free floor.
        //
        // Obscure twist: while confused, this becomes a short-range blink to a random
        // *visible* tile (line-of-sight) near the player, instead of a full random teleport.
        Vec2i prevPos = player().pos;
        Vec2i dst = prevPos;

        const bool confused = (player().effects.confusionTurns > 0);
        if (confused) {
            constexpr int R = 6;
            std::vector<uint8_t> mask;
            dung.computeFovMask(prevPos.x, prevPos.y, R, mask);

            std::vector<Vec2i> opts;
            opts.reserve((2 * R + 1) * (2 * R + 1));

            const int x0 = std::max(0, prevPos.x - R);
            const int x1 = std::min(dung.width - 1, prevPos.x + R);
            const int y0 = std::max(0, prevPos.y - R);
            const int y1 = std::min(dung.height - 1, prevPos.y + R);

            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    const int i = y * dung.width + x;
                    if (i < 0 || i >= static_cast<int>(mask.size())) continue;
                    if (mask[i] == 0) continue;
                    if (!dung.isWalkable(x, y)) continue;
                    if (entityAt(x, y)) continue;
                    if (x == prevPos.x && y == prevPos.y) continue;
                    opts.push_back({x, y});
                }
            }

            if (!opts.empty()) {
                dst = opts[rng.range(0, static_cast<int>(opts.size()) - 1)];
            } else {
                // Fallback: if somehow boxed in, allow a normal random teleport.
                for (int tries = 0; tries < 2000; ++tries) {
                    Vec2i p = dung.randomFloor(rng, true);
                    if (entityAt(p.x, p.y)) continue;
                    dst = p;
                    break;
                }
            }

            pushMsg("YOU READ A SCROLL. YOU BLINK ERRATICALLY!", MessageKind::Info, true);
        } else {
            for (int tries = 0; tries < 2000; ++tries) {
                Vec2i p = dung.randomFloor(rng, true);
                if (entityAt(p.x, p.y)) continue;
                dst = p;
                break;
            }

            pushMsg("YOU READ A SCROLL. YOU VANISH!", MessageKind::Info, true);
        }

        playerMut().pos = dst;

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        recomputeFov();

        const bool wasInShop = (roomTypeAt(dung, prevPos) == RoomType::Shop);
        const bool nowInShop = (roomTypeAt(dung, dst) == RoomType::Shop);
        if (wasInShop && !nowInShop) {
            const int debt = shopDebtThisDepth();
            if (debt > 0 && anyPeacefulShopkeeper(ents, playerId_)) {
                triggerShopTheftAlarm(prevPos, dst);
            }
        }
        return true;
    }

    if (it.kind == ItemKind::ScrollMapping) {
        // While confused, your mind mis-reads the patterns: you get the inverse of mapping.
        if (player().effects.confusionTurns > 0) {
            pushMsg("THE SIGNS SWIM... AND YOUR MEMORY UNRAVELS!", MessageKind::Warning, true);
            applyAmnesiaShock(4);
        } else {
            dung.revealAll();
            pushMsg("THE DUNGEON MAP IS REVEALED.", MessageKind::Info, true);
            recomputeFov();
        }
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollDetectTraps) {
        (void)markIdentified(it.kind, false);

        int newly = 0;
        int total = 0;

        for (auto& tr : trapsCur) {
            total += 1;
            if (!tr.discovered) newly += 1;
            tr.discovered = true;
        }

        // Chests can also be trapped; reveal those too.
        for (auto& gi : ground) {
            if (gi.item.kind != ItemKind::Chest) continue;
            if (!chestTrapped(gi.item)) continue;
            total += 1;
            if (!chestTrapKnown(gi.item)) newly += 1;
            setChestTrapKnown(gi.item, true);
        }

        if (total == 0) {
            pushMsg("YOU SENSE NO TRAPS.", MessageKind::Info, true);
        } else if (newly == 0) {
            pushMsg("YOU SENSE NO NEW TRAPS.", MessageKind::Info, true);
        } else {
            std::ostringstream ss;
            ss << "YOU SENSE " << newly << " TRAP" << (newly == 1 ? "" : "S") << "!";
            pushMsg(ss.str(), MessageKind::System, true);
        }

        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollDetectSecrets) {
        (void)markIdentified(it.kind, false);

        int newly = 0;
        for (auto& t : dung.tiles) {
            if (t.type == TileType::DoorSecret) {
                t.type = TileType::DoorClosed;
                t.explored = true; // show on the map once discovered
                newly++;
            }
        }

        if (newly == 0) {
            pushMsg("YOU SENSE NO SECRET DOORS.", MessageKind::Info, true);
        } else {
            std::ostringstream ss;
            ss << "YOU SENSE " << newly << " SECRET DOOR" << (newly == 1 ? "" : "S") << "!";
            pushMsg(ss.str(), MessageKind::System, true);
        }

        consumeOneStackable();
        return true;
    }


    if (it.kind == ItemKind::ScrollKnock) {
        (void)markIdentified(it.kind, false);

        Entity& p = playerMut();        const int radius = 6;
        int opened = 0;

        for (int y = p.pos.y - radius; y <= p.pos.y + radius; ++y) {
            for (int x = p.pos.x - radius; x <= p.pos.x + radius; ++x) {
                if (!dung.inBounds(x, y)) continue;
                int dx = std::abs(x - p.pos.x);
                int dy = std::abs(y - p.pos.y);
                int cheb = std::max(dx, dy);
                if (cheb > radius) continue;

                if (dung.isDoorLocked(x, y)) {
                    dung.unlockDoor(x, y);
                    dung.openDoor(x, y);
                    onDoorOpened({x, y}, true);
                    opened++;
                }
            }
        }

        // Also unlock nearby chests.
        for (auto& gi : ground) {
            if (gi.item.kind != ItemKind::Chest) continue;
            if (!chestLocked(gi.item)) continue;
            int dx = std::abs(gi.pos.x - p.pos.x);
            int dy = std::abs(gi.pos.y - p.pos.y);
            int cheb = std::max(dx, dy);
            if (cheb > radius) continue;
            setChestLocked(gi.item, false);
            opened += 1;
        }

        if (opened == 0) {
            pushMsg("NOTHING SEEMS TO HAPPEN.", MessageKind::Info, true);
        } else if (opened == 1) {
            pushMsg("YOU HEAR A LOCK CLICK OPEN.", MessageKind::System, true);
        } else {
            pushMsg("YOU HEAR A CHORUS OF LOCKS CLICK OPEN.", MessageKind::System, true);
        }

        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollRemoveCurse) {
        (void)markIdentified(it.kind, false);

        int uncursed = 0;
        for (auto& invIt : inv) {
            if (!isWearableGear(invIt.kind)) continue;
            if (invIt.buc < 0) {
                invIt.buc = 0;
                uncursed++;
            }
        }

        if (uncursed == 0) {
            pushMsg("NOTHING SEEMS TO HAPPEN.", MessageKind::Info, true);
        } else {
            pushMsg("YOU FEEL A MALEVOLENT WEIGHT LIFT FROM YOUR GEAR.", MessageKind::System, true);
        }

        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollConfusion) {
        Entity& p = playerMut();

        int affected = 0;
        for (Entity& e : ents) {
            if (e.id == p.id) continue;
            if (e.kind == EntityKind::Shopkeeper) continue;
            if (e.hp <= 0) continue;
            if (!dung.inBounds(e.pos.x, e.pos.y)) continue;
            if (!dung.at(e.pos.x, e.pos.y).visible) continue;

            const int turns = rng.range(6, 12) + std::min(6, depth_ / 2);
            e.effects.confusionTurns = std::max(e.effects.confusionTurns, turns);
            e.alerted = true;
            affected++;
        }

        if (affected > 0) pushMsg("THE AIR SHIMMERS. YOUR FOES LOOK CONFUSED!", MessageKind::Success, true);
        else pushMsg("NOTHING SEEMS TO HAPPEN.", MessageKind::Info, true);

        emitNoise(p.pos, 4);

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollFear) {
        Entity& p = playerMut();

        auto immuneToFear = [&](EntityKind k) -> bool {
            // Simple immunity list: mindless or already-dead fear targets.
            // (Keeps the scroll useful without trivializing undead/bosses.)
            switch (k) {
                case EntityKind::SkeletonArcher:
                case EntityKind::Ghost:
                case EntityKind::Slime:
                    return true;
                default:
                    return false;
            }
        };

        int affected = 0;
        int immune = 0;
        for (Entity& e : ents) {
            if (e.id == p.id) continue;
            if (e.friendly) continue;
            if (e.kind == EntityKind::Shopkeeper) continue;
            if (e.hp <= 0) continue;
            if (!dung.inBounds(e.pos.x, e.pos.y)) continue;
            if (!dung.at(e.pos.x, e.pos.y).visible) continue;

            if (immuneToFear(e.kind)) {
                immune++;
                continue;
            }

            const int turns = rng.range(6, 12) + std::min(6, depth_ / 2);
            e.effects.fearTurns = std::max(e.effects.fearTurns, turns);
            e.alerted = true;
            e.lastKnownPlayerPos = p.pos;
            e.lastKnownPlayerAge = 0;
            affected++;
        }

        if (affected > 0) {
            if (immune > 0) {
                pushMsg("A WAVE OF TERROR RADIATES OUTWARD. SOME FOES TREMBLE!", MessageKind::Success, true);
            } else {
                pushMsg("A WAVE OF TERROR RADIATES OUTWARD. YOUR FOES TREMBLE!", MessageKind::Success, true);
            }
        } else if (immune > 0) {
            pushMsg("A CHILL RUNS THROUGH THE DUNGEON, BUT YOUR FOES STAND FIRM.", MessageKind::Info, true);
        } else {
            pushMsg("NOTHING SEEMS TO HAPPEN.", MessageKind::Info, true);
        }

        emitNoise(p.pos, 4);

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollTaming) {
        Entity& p = playerMut();

        // NetHack-inspired: tame monsters adjacent to the player. While confused,
        // the "taming aura" expands to an 11x11 area (chebyshev radius 5).
        const int radius = (p.effects.confusionTurns > 0) ? 5 : 1;

        auto immuneToTaming = [&](EntityKind k) -> bool {
            // Undead are immune to charm.
            if (entityIsUndead(k)) return true;
            // Shops should remain stable.
            if (k == EntityKind::Shopkeeper) return true;
            // Bosses resist mind control.
            if (k == EntityKind::Minotaur) return true;
            return false;
        };

        int candidates = 0;
        int tamed = 0;
        int immune = 0;
        int resisted = 0;

        for (Entity& e : ents) {
            if (e.id == p.id) continue;
            if (e.hp <= 0) continue;
            if (e.friendly) continue;

            const int dist = chebyshev(p.pos, e.pos);
            if (dist > radius) continue;

            // Only affect monsters the player could plausibly "address".
            if (!dung.hasLineOfSight(p.pos.x, p.pos.y, e.pos.x, e.pos.y)) continue;

            if (immuneToTaming(e.kind)) {
                immune++;
                continue;
            }

            candidates++;

            // No monster MR in ProcRogue; approximate resistance using XP value + depth.
            // Higher-focus characters are better at bending wills.
            int chance = 70;
            chance += playerFocus() * 4;
            chance += playerAgility() * 2;
            chance -= std::min(30, xpFor(e.kind));
            chance -= depth_ * 2;

            // Clamp so it's never guaranteed at depth, but remains usable as an "escape" item.
            chance = clampi(chance, 10, 90);

            const int roll = rng.range(1, 100);
            if (roll <= chance) {
                e.friendly = true;
                e.allyOrder = AllyOrder::Follow;
                // Reset alert state so they immediately flip behavior.
                e.alerted = false;
                e.lastKnownPlayerPos = {-1, -1};
                e.lastKnownPlayerAge = 9999;
                // Being charmed dispels fear.
                e.effects.fearTurns = 0;
                tamed++;
            } else {
                // A resisted charm still puts the monster on edge.
                e.alerted = true;
                e.lastKnownPlayerPos = p.pos;
                e.lastKnownPlayerAge = 0;
                resisted++;
            }
        }

        if (tamed > 0) {
            if (resisted > 0 || immune > 0) {
                pushMsg("THE NEIGHBORHOOD SEEMS FRIENDLIER.", MessageKind::Success, true);
            } else {
                pushMsg("THE NEIGHBORHOOD IS FRIENDLIER.", MessageKind::Success, true);
            }
        } else if (candidates > 0 || immune > 0) {
            pushMsg("NOTHING INTERESTING SEEMS TO HAPPEN.", MessageKind::Info, true);
        } else {
            pushMsg("NOTHING INTERESTING HAPPENS.", MessageKind::Info, true);
        }

        emitNoise(p.pos, 4);

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollEarth) {
        Entity& p = playerMut();

        int boulders = 0;
        int bridged = 0;
        int slammed = 0;

        // Raise boulders in the 8 surrounding tiles. This is mainly a tactical
        // fortification tool, but it can also bridge adjacent chasms.
        const int dirs8[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};

        for (int i = 0; i < 8; ++i) {
            const int x = p.pos.x + dirs8[i][0];
            const int y = p.pos.y + dirs8[i][1];
            if (!dung.inBounds(x, y)) continue;

            // Don't clobber stairs.
            if (Vec2i{x, y} == dung.stairsUp || Vec2i{x, y} == dung.stairsDown) continue;

            Tile& t = dung.at(x, y);

            // If there's a chasm, the "falling earth" fills it in.
            if (t.type == TileType::Chasm) {
                t.type = TileType::Floor;
                bridged++;
                continue;
            }

            // Only place boulders on walkable terrain; do not overwrite doors/walls.
            if (!dung.isWalkable(x, y)) continue;

            // If an enemy is in the way, slam it with falling rock. Friendly units
            // (including your dog) are spared for QoL.
            Entity* e = entityAtMut(x, y);
            if (e && e->id != p.id) {
                if (!e->friendly && e->kind != EntityKind::Shopkeeper) {
                    const int dmg = rng.range(6, 10) + std::min(4, depth_ / 3);
                    e->hp = std::max(0, e->hp - dmg);
                    slammed++;

                    if (t.visible) {
                        std::ostringstream ss;
                        if (e->hp <= 0) {
                            ss << "A BOULDER CRUSHES " << kindName(e->kind) << "!";
                        } else {
                            ss << "ROCKS PELT " << kindName(e->kind) << "!";
                        }
                        pushMsg(ss.str(), MessageKind::Combat, false);
                    }

                    // Only place the boulder if the enemy was killed (so we don't
                    // create impossible overlaps).
                    if (e->hp > 0) continue;
                } else {
                    continue;
                }
            }

            // If the tile is now empty, raise a boulder.
            if (!entityAt(x, y) && t.type != TileType::Boulder) {
                t.type = TileType::Boulder;
                boulders++;
            }
        }

        if (boulders == 0 && bridged == 0 && slammed == 0) {
            pushMsg("THE GROUND RUMBLES, BUT NOTHING HAPPENS.", MessageKind::Info, true);
        } else {
            pushMsg("THE EARTH TREMBLES!", MessageKind::Warning, true);
            if (boulders > 0) pushMsg("BOULDERS RISE FROM THE STONE.", MessageKind::System, true);
            if (bridged > 0) pushMsg("DEBRIS FILLS IN THE CHASM.", MessageKind::System, true);
        }

        emitNoise(p.pos, 8);

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionAntidote) {
        Entity& p = playerMut();        if (p.effects.poisonTurns > 0) {
            p.effects.poisonTurns = 0;
            pushMsg("YOU FEEL THE POISON LEAVE YOUR BODY.", MessageKind::Success, true);
        } else {
            pushMsg("YOU FEEL CLEAN.", MessageKind::Info, true);
        }
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionRegeneration) {
        Entity& p = playerMut();        p.effects.regenTurns = std::max(p.effects.regenTurns, 18);
        pushMsg("YOUR WOUNDS BEGIN TO KNIT.", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionShielding) {
        Entity& p = playerMut();        p.effects.shieldTurns = std::max(p.effects.shieldTurns, 14);
        pushMsg("YOU FEEL PROTECTED.", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionHaste) {
        Entity& p = playerMut();        p.effects.hasteTurns = std::min(40, p.effects.hasteTurns + 6);
        hastePhase = false; // ensure the next action is the "free" haste action
        pushMsg("YOU FEEL QUICK!", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionVision) {
        Entity& p = playerMut();        p.effects.visionTurns = std::min(60, p.effects.visionTurns + 20);
        pushMsg("YOUR EYES SHINE WITH INNER LIGHT.", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        recomputeFov();
        return true;
    }

    if (it.kind == ItemKind::PotionInvisibility) {
        Entity& p = playerMut();        p.effects.invisTurns = std::min(60, p.effects.invisTurns + 18);
        pushMsg("YOU FADE FROM SIGHT!", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionLevitation) {
        Entity& p = playerMut();

        // Base duration (blessed/cursed can modify in the future, and stacking preserves the longest).
        int dur = 14 + rng.range(0, 6);
        if (it.buc > 0) dur += 10;
        if (it.buc < 0) dur = std::max(4, dur / 2);

        p.effects.levitationTurns = std::max(p.effects.levitationTurns, dur);
        pushMsg("YOU FEEL LIGHTER THAN AIR!", MessageKind::Success, true);
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionClarity) {
        Entity& p = playerMut();
        const bool wasConfused = (p.effects.confusionTurns > 0);
        const bool wasHallucinating = (p.effects.hallucinationTurns > 0);
        p.effects.confusionTurns = 0;
        p.effects.hallucinationTurns = 0;

        if (wasConfused || wasHallucinating) pushMsg("YOUR MIND CLEARS.", MessageKind::Success, true);
        else pushMsg("YOU FEEL FOCUSED.", MessageKind::Info, true);

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::PotionHallucination) {
        Entity& p = playerMut();

        // Base duration. This effect is mostly a perception hazard, but we still let
        // blessed/cursed modify it for variety.
        int dur = 28 + rng.range(0, 24);
        if (it.buc > 0) {
            // Blessed: shorter trip, and a brief "lucid" boost.
            dur = std::max(8, dur / 2);
            p.effects.visionTurns = std::max(p.effects.visionTurns, 8);
            pushMsg("REALITY BUCKLES... THEN SNAPS INTO STRANGE CLARITY.", MessageKind::Info, true);
        } else if (it.buc < 0) {
            // Cursed: longer, plus some confusion.
            dur += 18;
            p.effects.confusionTurns = std::max(p.effects.confusionTurns, 6);
            pushMsg("THE WORLD TURNS KALEIDOSCOPIC!", MessageKind::Warning, true);
        } else {
            pushMsg("THE WORLD SWIMS BEFORE YOUR EYES!", MessageKind::Warning, true);
        }

        p.effects.hallucinationTurns = std::max(p.effects.hallucinationTurns, dur);

        // Blessed hallucinations also grant a brief Vision boost.
        if (it.buc > 0) {
            recomputeFov();
        }

        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::Torch) {
        // Light a torch: consumes one TORCH from the stack and creates a LIT TORCH item that burns over time.
        // (The LIT TORCH can be dropped to create a stationary light source.)
        const int fuel = 180 + rng.range(0, 120);

        // Using up unpaid shop goods still leaves you owing the shopkeeper.
        if (it.shopPrice > 0 && it.shopDepth > 0) {
            const int sd = it.shopDepth;
            if (sd >= 1 && sd <= DUNGEON_MAX_DEPTH) {
                shopDebtLedger_[sd] += it.shopPrice;
            }
        }

        // Consume one torch from the selected stack first (to avoid reference invalidation from inv push_back).
        if (it.count > 1) {
            it.count -= 1;
        } else {
            inv.erase(inv.begin() + static_cast<std::vector<Item>::difference_type>(invSel));
            if (invSel >= static_cast<int>(inv.size())) invSel = static_cast<int>(inv.size()) - 1;
        }

        Item lit;
        lit.id = nextItemId++;
        lit.kind = ItemKind::TorchLit;
        lit.count = 1;
        lit.enchant = 0;
        lit.charges = fuel;
        lit.spriteSeed = rng.nextU32();

        inv.push_back(lit);

        pushMsg("YOU LIGHT A TORCH.", MessageKind::System, true);
        // The flare is small but noticeable.
        emitNoise(player().pos, 4);

        // Lighting changes sight in dark levels.
        recomputeFov();
        return true;
    }

    if (it.kind == ItemKind::ScrollEnchantWeapon) {
        int idx = equippedMeleeIndex();
        if (idx < 0) {
            pushMsg("YOUR HANDS TINGLE... BUT NOTHING HAPPENS.", MessageKind::Info, true);
        } else {
            Item& w = inv[static_cast<size_t>(idx)];
            w.enchant += 1;
            pushMsg("YOUR WEAPON GLOWS BRIEFLY.", MessageKind::Success, true);
        }
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::ScrollEnchantArmor) {
        int idx = equippedArmorIndex();
        if (idx < 0) {
            pushMsg("YOUR SKIN TINGLES... BUT NOTHING HAPPENS.", MessageKind::Info, true);
        } else {
            Item& a = inv[static_cast<size_t>(idx)];
            a.enchant += 1;
            pushMsg("YOUR ARMOR GLOWS BRIEFLY.", MessageKind::Success, true);
        }
        (void)markIdentified(it.kind, false);
        consumeOneStackable();
        return true;
    }


    if (it.kind == ItemKind::ScrollEnchantRing) {
        (void)markIdentified(it.kind, false);

        // Gather ring candidates.
        std::vector<int> ringIds;
        ringIds.reserve(8);
        for (const auto& invIt : inv) {
            if (!isRingKind(invIt.kind)) continue;
            ringIds.push_back(invIt.id);
        }

        if (ringIds.empty()) {
            pushMsg("YOU FEEL A FAINT TINGLE... BUT NOTHING HAPPENS.", MessageKind::Info, true);
            consumeOneStackable();
            return true;
        }

        auto enchantRingById = [&](int itemId) {
            const int idx = findItemIndexById(inv, itemId);
            if (idx < 0) return;
            inv[static_cast<size_t>(idx)].enchant += 1;
            pushMsg("YOUR RING GLOWS BRIEFLY.", MessageKind::Success, true);
        };

        if (ringIds.size() == 1) {
            enchantRingById(ringIds[0]);
            consumeOneStackable();
            return true;
        }

        // Multiple rings: consume the scroll now (reading takes the turn regardless).
        consumeOneStackable();

        // Enter a temporary inventory sub-mode so the player can choose.
        invEnchantRingMode = true;

        // Move selection to the first ring to reduce friction.
        const int idx0 = findItemIndexById(inv, ringIds[0]);
        if (idx0 >= 0) invSel = idx0;
        else invSel = clampi(invSel, 0, std::max(0, static_cast<int>(inv.size()) - 1));

        pushMsg("SELECT A RING TO ENCHANT (ENTER = CHOOSE, ESC = RANDOM).", MessageKind::System, true);
        return true;
    }

    if (it.kind == ItemKind::ScrollIdentify) {
        // Using an identify scroll reveals the true name of one unidentified potion/scroll.
        // If multiple candidates exist, the player can choose which one to learn.
        (void)markIdentified(it.kind, false);

        if (!identifyItemsEnabled) {
            pushMsg("YOUR MIND FEELS CLEAR.", MessageKind::Info, true);
            consumeOneStackable();
            return true;
        }

        std::vector<ItemKind> candidates;
        candidates.reserve(16);
        auto seen = [&](ItemKind k) {
            for (ItemKind x : candidates) if (x == k) return true;
            return false;
        };

        for (const auto& invIt : inv) {
            if (!isIdentifiableKind(invIt.kind)) continue;
            if (invIt.kind == ItemKind::ScrollIdentify) continue;
            if (isIdentified(invIt.kind)) continue;
            if (!seen(invIt.kind)) candidates.push_back(invIt.kind);
        }

        if (candidates.empty()) {
            pushMsg("YOU STUDY THE SCROLL, BUT LEARN NOTHING NEW.", MessageKind::Info, true);
            consumeOneStackable();
            return true;
        }

        if (candidates.size() == 1) {
            (void)markIdentified(candidates[0], false);
            consumeOneStackable();
            return true;
        }

        // Multiple unknown kinds: consume the scroll now (reading takes the turn regardless).
        consumeOneStackable();

        // Enter a temporary inventory sub-mode so the player can choose.
        invIdentifyMode = true;

        // Move selection to the first eligible item to reduce friction.
        for (size_t i = 0; i < inv.size(); ++i) {
            const Item& cand = inv[i];
            if (!isIdentifiableKind(cand.kind)) continue;
            if (cand.kind == ItemKind::ScrollIdentify) continue;
            if (isIdentified(cand.kind)) continue;
            invSel = static_cast<int>(i);
            break;
        }

        pushMsg("SELECT AN ITEM TO IDENTIFY (ENTER = CHOOSE, ESC = RANDOM).", MessageKind::System, true);
        return true;
    }

    if (it.kind == ItemKind::Fish) {
        Entity& p = playerMut();

        // Decode fish seed + meta.
        uint32_t fishSeed = 0u;
        if (it.charges != 0) {
            fishSeed = fishSeedFromCharges(it.charges);
        } else if (it.spriteSeed != 0u) {
            fishSeed = it.spriteSeed;
        } else {
            fishSeed = hash32(static_cast<uint32_t>(it.id) ^ 0xF15B00Fu);
        }

        const bool hasMeta = (it.enchant != 0);
        const int rarityHint = hasMeta ? fishRarityFromEnchant(it.enchant) : -1;
        const int sizeHint   = hasMeta ? fishSizeClassFromEnchant(it.enchant) : -1;
        const int shinyHint  = hasMeta ? (fishIsShinyFromEnchant(it.enchant) ? 1 : 0) : -1;

        const fishgen::FishSpec fs = fishgen::makeFish(fishSeed, rarityHint, sizeHint, shinyHint);
        const int beforeState = hungerStateFor(hunger, hungerMax);

        // Core nourishment.
        if (fs.healAmount > 0 && p.hp < p.hpMax) {
            p.hp = std::min(p.hpMax, p.hp + fs.healAmount);
        }
        if (hungerEnabled_) {
            if (hungerMax <= 0) hungerMax = 800;
            hunger = std::min(hungerMax, hunger + fs.hungerRestore);
        }

        pushMsg("YOU EAT " + fs.name + ".", MessageKind::Loot, true);

        // Bonus tag effects (NetHack-ish: some things are weird/dangerous).
        const std::string tag = (fs.bonusTag ? std::string(fs.bonusTag) : std::string());
        if (!tag.empty()) {
            const int wt = fs.weight10;
            const int dur = clampi(8 + (wt / 25), 4, 22);
            if (tag == "REGEN") {
                p.effects.regenTurns = std::max(p.effects.regenTurns, dur);
                pushMsg("YOU FEEL A GENTLE VITALITY.", MessageKind::Success, true);
            } else if (tag == "HASTE") {
                p.effects.hasteTurns = std::max(p.effects.hasteTurns, dur);
                pushMsg("YOUR BLOOD RUNS QUICK.", MessageKind::Success, true);
            } else if (tag == "SHIELD") {
                p.effects.shieldTurns = std::max(p.effects.shieldTurns, dur + 2);
                pushMsg("YOUR SKIN FEELS HARDER.", MessageKind::Success, true);
            } else if (tag == "AURORA") {
                p.effects.visionTurns = std::max(p.effects.visionTurns, dur + 4);
                pushMsg("YOUR EYES CATCH THE LIGHT.", MessageKind::Success, true);
                recomputeFov();
            } else if (tag == "CLARITY") {
                const bool wasConf = (p.effects.confusionTurns > 0);
                const bool wasHall = (p.effects.hallucinationTurns > 0);
                p.effects.confusionTurns = 0;
                p.effects.hallucinationTurns = 0;
                if (wasConf || wasHall) pushMsg("YOUR MIND CLEARS.", MessageKind::Success, true);
                else pushMsg("YOU FEEL FOCUSED.", MessageKind::Info, true);
            } else if (tag == "VENOM") {
                p.effects.poisonTurns = std::max(p.effects.poisonTurns, 4 + (wt / 60));
                pushMsg("UGH... YOU FEEL SICK.", MessageKind::Warning, true);
            } else if (tag == "EMBER") {
                p.effects.burnTurns = std::max(p.effects.burnTurns, 3 + (wt / 70));
                pushMsg("YOUR THROAT BURNS!", MessageKind::Warning, true);
            }
        }

        // Hunger feedback (mirrors Food Ration/corpse).
        const int afterState = hungerStateFor(hunger, hungerMax);
        if (hungerEnabled_) {
            if (beforeState >= 2 && afterState < 2) {
                pushMsg("YOU FEEL LESS STARVED.", MessageKind::System, true);
            } else if (beforeState >= 1 && afterState == 0) {
                pushMsg("YOU FEEL SATIATED.", MessageKind::System, true);
            }
        }
        hungerStatePrev = hungerStateFor(hunger, hungerMax);

        consumeOneStackable();
        return true;
    }

    if (it.kind == ItemKind::FoodRation) {
        Entity& p = playerMut();        const ItemDef& d = itemDef(it.kind);

        const int beforeState = hungerStateFor(hunger, hungerMax);

        // Small heal (always), plus hunger restoration if enabled.
        if (d.healAmount > 0 && p.hp < p.hpMax) {
            p.hp = std::min(p.hpMax, p.hp + d.healAmount);
        }

        if (hungerEnabled_) {
            if (hungerMax <= 0) hungerMax = 800;
            hunger = std::min(hungerMax, hunger + d.hungerRestore);
        }

        const int afterState = hungerStateFor(hunger, hungerMax);
        if (hungerEnabled_) {
            if (beforeState >= 2 && afterState < 2) {
                pushMsg("YOU FEEL LESS STARVED.", MessageKind::System, true);
            } else if (beforeState >= 1 && afterState == 0) {
                pushMsg("YOU FEEL SATIATED.", MessageKind::System, true);
            }
        }

        // Sync the throttling state so we don't immediately re-announce hunger next tick.
        hungerStatePrev = hungerStateFor(hunger, hungerMax);

        pushMsg("YOU EAT A FOOD RATION.", MessageKind::Loot, true);
        consumeOneStackable();
        return true;
    }


    if (it.kind == ItemKind::ButcheredMeat) {
        Entity& p = playerMut();

        if (hungerEnabled_ && hungerMax <= 0) hungerMax = 800;

        const int beforeState = hungerStateFor(hunger, hungerMax);

        const bool rotten = (it.charges <= 60);
        const bool stale = (it.charges <= 160);

        int restore = butcherMeatHungerFromEnchant(it.enchant);
        int heal = butcherMeatHealFromEnchant(it.enchant);

        // Spoilage reduces nutrition.
        if (rotten) {
            restore = restore / 2;
            heal = std::max(0, heal - 1);
        } else if (stale) {
            restore = (restore * 3) / 4;
        }

        if (heal > 0 && p.hp < p.hpMax) p.hp = std::min(p.hpMax, p.hp + heal);
        if (hungerEnabled_) hunger = std::min(hungerMax, hunger + restore);

        // Scale effect duration loosely with the source creature weight.
        int wt = 0;
        const int srcRaw = butcherSourceKindFromEnchant(it.enchant);
        if (srcRaw >= 0 && srcRaw < ITEM_KIND_COUNT) wt = itemDef(static_cast<ItemKind>(srcRaw)).weight;
        const int dur = 8 + (wt / 40);

        const char* tag = butchergen::tagToken(butchergen::tagFromIndex(butcherMeatTagFromEnchant(it.enchant)));
        if (tag && tag[0]) {
            const std::string t = tag;
            if (t == "REGEN") {
                p.effects.regenTurns = std::max(p.effects.regenTurns, dur);
                pushMsg("THE MEAT MAKES YOU FEEL HEALTHIER.", MessageKind::Loot, true);
            } else if (t == "HASTE") {
                p.effects.hasteTurns = std::max(p.effects.hasteTurns, dur);
                pushMsg("THE MEAT MAKES YOU FEEL QUICKER.", MessageKind::Loot, true);
            } else if (t == "SHIELD") {
                p.effects.shieldTurns = std::max(p.effects.shieldTurns, dur);
                pushMsg("YOU FEEL PROTECTED.", MessageKind::Loot, true);
            } else if (t == "AURORA") {
                p.effects.visionTurns = std::max(p.effects.visionTurns, dur);
                pushMsg("YOUR VISION SHARPENS.", MessageKind::Loot, true);
                recomputeFov();
            } else if (t == "CLARITY") {
                if (p.effects.confuseTurns > 0 || p.effects.halluTurns > 0) {
                    p.effects.confuseTurns = 0;
                    p.effects.halluTurns = 0;
                    pushMsg("YOUR MIND CLEARS.", MessageKind::Loot, true);
                } else {
                    pushMsg("YOU FEEL A LITTLE MORE FOCUSED.", MessageKind::Loot, true);
                }
            } else if (t == "VENOM") {
                p.effects.poisonTurns = std::max(p.effects.poisonTurns, 6 + (wt / 80));
                pushMsg("UGH... YOU FEEL SICK.", MessageKind::Warning, true);
            } else if (t == "EMBER") {
                p.effects.burnTurns = std::max(p.effects.burnTurns, 4 + (wt / 80));
                pushFxParticle(FXParticlePreset::EmberBurst, player().pos);
                pushMsg("THE MEAT BURNS YOUR THROAT!", MessageKind::Warning, true);
            }
        }

        if (rotten) {
            // A little extra sickness risk.
            p.effects.poisonTurns = std::max(p.effects.poisonTurns, 3 + (wt / 100));
            pushMsg("YOU EAT ROTTEN MEAT.", MessageKind::Warning, true);
        } else if (stale) {
            pushMsg("YOU EAT STALE MEAT.", MessageKind::Loot, true);
        } else {
            pushMsg("YOU EAT SOME MEAT.", MessageKind::Loot, true);
        }

        const int afterState = hungerStateFor(hunger, hungerMax);

        if (hungerEnabled_) {
            if (afterState < beforeState) {
                pushMsg("YOU FEEL HUNGRIER.", MessageKind::Bad, true);
            } else if (afterState > beforeState) {
                pushMsg("YOU FEEL FULLER.", MessageKind::Loot, true);
            }
        }

        hungerStatePrev = afterState;
        consumeOneStackable();
        return true;
    }
    if (isCorpseKind(it.kind)) {
        Entity& p = playerMut();
        const ItemDef& d = itemDef(it.kind);

        // Corpse decay state (charges = remaining freshness in turns).
        const int ch = it.charges;
        const bool rotten = (ch <= 60);
        const bool stale = (ch <= 160);

        const int beforeState = hungerStateFor(hunger, hungerMax);

        if (ch <= 0) {
            pushMsg("THE CORPSE CRUMBLES INTO ROT.", MessageKind::Warning, true);
            consumeOneStackable();
            return true;
        }

        // Base nourishment/heal from ItemDef, scaled by freshness.
        int heal = d.healAmount;
        int restore = d.hungerRestore;
        if (rotten) {
            heal = std::max(0, heal - 1);
            restore = std::max(0, restore / 2);
        } else if (stale) {
            restore = std::max(0, (restore * 3) / 4);
        }

        // Apply the basic food effects.
        if (heal > 0 && p.hp < p.hpMax) {
            p.hp = std::min(p.hpMax, p.hp + heal);
        }
        if (hungerEnabled_) {
            if (hungerMax <= 0) hungerMax = 800;
            hunger = std::min(hungerMax, hunger + restore);
        }

        // Risk/bonus table.
        float poisonChance = 0.0f;
        int poisonTurns = 0;
        float confuseChance = 0.0f;
        int confuseTurns = 0;

        enum class Bonus { None, Regen, Haste, Vision, Shield, Strength };
        Bonus bonus = Bonus::None;
        float bonusChance = 0.0f;
        int bonusTurns = 0;
        int strengthInc = 0;

        switch (it.kind) {
            case ItemKind::CorpseGoblin:
                poisonChance = 0.10f; poisonTurns = 6;
                break;
            case ItemKind::CorpseOrc:
                poisonChance = 0.15f; poisonTurns = 6;
                break;
            case ItemKind::CorpseBat:
                poisonChance = 0.08f; poisonTurns = 5;
                bonus = Bonus::Haste; bonusChance = 0.18f; bonusTurns = 10;
                break;
            case ItemKind::CorpseSlime:
                poisonChance = 0.50f; poisonTurns = 10;
                confuseChance = 0.25f; confuseTurns = 12;
                break;
            case ItemKind::CorpseKobold:
                poisonChance = 0.12f; poisonTurns = 6;
                break;
            case ItemKind::CorpseWolf:
                poisonChance = 0.08f; poisonTurns = 6;
                bonus = Bonus::Regen; bonusChance = 0.20f; bonusTurns = 12;
                break;
            case ItemKind::CorpseTroll:
                poisonChance = 0.12f; poisonTurns = 8;
                bonus = Bonus::Regen; bonusChance = 1.00f; bonusTurns = 18;
                break;
            case ItemKind::CorpseWizard:
                poisonChance = 0.06f; poisonTurns = 6;
                confuseChance = 0.20f; confuseTurns = 12;
                bonus = Bonus::Vision; bonusChance = 0.35f; bonusTurns = 18;
                break;
            case ItemKind::CorpseSnake:
                poisonChance = 0.35f; poisonTurns = 10;
                break;
            case ItemKind::CorpseSpider:
                poisonChance = 0.40f; poisonTurns = 11;
                break;
            case ItemKind::CorpseOgre:
                poisonChance = 0.20f; poisonTurns = 8;
                bonus = Bonus::Strength; bonusChance = 0.08f; strengthInc = 1;
                break;
            case ItemKind::CorpseMimic:
                poisonChance = 0.22f; poisonTurns = 8;
                confuseChance = 0.18f; confuseTurns = 10;
                bonus = Bonus::Shield; bonusChance = 0.18f; bonusTurns = 14;
                break;
            case ItemKind::CorpseMinotaur:
                poisonChance = 0.25f; poisonTurns = 9;
                bonus = Bonus::Strength; bonusChance = 0.15f; strengthInc = 1;
                break;
            default:
                poisonChance = 0.18f; poisonTurns = 7;
                break;
        }

        // Freshness modifies risk/benefit.
        if (rotten) {
            poisonChance += 0.35f;
            confuseChance += 0.20f;
            bonusChance *= 0.25f;
        } else if (stale) {
            poisonChance += 0.15f;
            bonusChance *= 0.75f;
        }

        poisonChance = std::min(0.95f, poisonChance);
        confuseChance = std::min(0.80f, confuseChance);

        // Messaging.
        pushMsg("YOU EAT THE " + itemDisplayNameSingle(it.kind) + ".", MessageKind::Loot, true);
        if (rotten) {
            pushMsg("IT TASTES RANCID.", MessageKind::Warning, true);
        }

        // Apply negative effects.
        bool poisoned = false;
        if (poisonChance > 0.0f && rng.chance(poisonChance)) {
            const int extra = rotten ? 4 : (stale ? 1 : 0);
            const int turns = std::max(1, poisonTurns + extra);
            p.effects.poisonTurns = std::max(p.effects.poisonTurns, turns);
            poisoned = true;
        }
        if (confuseChance > 0.0f && rng.chance(confuseChance)) {
            const int extra = rotten ? 4 : 0;
            const int turns = std::max(1, confuseTurns + extra);
            p.effects.confusionTurns = std::max(p.effects.confusionTurns, turns);
            pushMsg("YOU FEEL CONFUSED!", MessageKind::Warning, true);
        }

        if (poisoned) {
            pushMsg("UGH... YOU FEEL SICK.", MessageKind::Warning, true);
        }

        // Apply a possible positive bonus.
        if (bonus != Bonus::None && bonusChance > 0.0f && rng.chance(bonusChance)) {
            switch (bonus) {
                case Bonus::Regen:
                    p.effects.regenTurns = std::max(p.effects.regenTurns, bonusTurns);
                    pushMsg("YOU FEEL A STRANGE VITALITY.", MessageKind::Success, true);
                    break;
                case Bonus::Haste:
                    p.effects.hasteTurns = std::max(p.effects.hasteTurns, bonusTurns);
                    pushMsg("YOUR BLOOD RUNS HOT.", MessageKind::Success, true);
                    break;
                case Bonus::Vision:
                    p.effects.visionTurns = std::max(p.effects.visionTurns, bonusTurns);
                    pushMsg("YOUR EYES SHARPEN.", MessageKind::Success, true);
                    break;
                case Bonus::Shield:
                    p.effects.shieldTurns = std::max(p.effects.shieldTurns, bonusTurns);
                    pushMsg("A PROTECTIVE AURA SURROUNDS YOU.", MessageKind::Success, true);
                    break;
                case Bonus::Strength:
                    if (strengthInc != 0) {
                        p.baseAtk += strengthInc;
                        std::ostringstream ss;
                        ss << "YOU FEEL STRONGER! ATK IS NOW " << p.baseAtk << ".";
                        pushMsg(ss.str(), MessageKind::Success, true);
                    }
                    break;
                default:
                    break;
            }
        }

        // Special: minotaur meat is powerful, but dangerous.
        if (it.kind == ItemKind::CorpseMinotaur && !rotten) {
            p.effects.shieldTurns = std::max(p.effects.shieldTurns, 16);
        }

        // Hunger feedback (mirrors Food Ration).
        const int afterState = hungerStateFor(hunger, hungerMax);
        if (hungerEnabled_) {
            if (beforeState >= 2 && afterState < 2) {
                pushMsg("YOU FEEL LESS STARVED.", MessageKind::System, true);
            } else if (beforeState >= 1 && afterState == 0) {
                pushMsg("YOU FEEL SATIATED.", MessageKind::System, true);
            }
        }

        // Sync the throttling state so we don't immediately re-announce hunger next tick.
        hungerStatePrev = hungerStateFor(hunger, hungerMax);

        consumeOneStackable();
        return true;
    }

    pushMsg("NOTHING HAPPENS.", MessageKind::Info, true);
    return false;
}



void Game::showBountyContracts() {
    int count = 0;
    for (const auto& it : inv) if (it.kind == ItemKind::BountyContract) ++count;

    if (count <= 0) {
        pushMsg("NO ACTIVE BOUNTY CONTRACTS.", MessageKind::Info, true);
        return;
    }

    std::ostringstream header;
    header << "BOUNTY CONTRACTS: " << count << ".";
    pushMsg(header.str(), MessageKind::Info, true);

    for (const auto& it : inv) {
        if (it.kind != ItemKind::BountyContract) continue;

        const uint32_t seed = (it.spriteSeed != 0u) ? it.spriteSeed : hash32(static_cast<uint32_t>(it.id) ^ 0xB01DCAFEu);
        const std::string code = bountygen::codename(seed);

        const int rawTarget = bountyTargetKindFromCharges(it.charges);
        EntityKind target = EntityKind::Goblin;
        if (rawTarget >= 0 && rawTarget < ENTITY_KIND_COUNT) target = static_cast<EntityKind>(rawTarget);

        const int req = clampi(bountyRequiredKillsFromCharges(it.charges), 1, 255);
        const int prog = clampi(bountyProgressFromEnchant(it.enchant), 0, 255);
        const int shown = std::min(req, prog);

        const int rawReward = bountyRewardKindFromCharges(it.charges);
        ItemKind rewardK = ItemKind::Gold;
        if (rawReward >= 0 && rawReward < ITEM_KIND_COUNT) rewardK = static_cast<ItemKind>(rawReward);

        int rewardC = clampi(bountyRewardCountFromCharges(it.charges), 0, 255);

        std::ostringstream ss;
        ss << "- " << code << ": KILL " << req << " " << bountygen::pluralizeEntityName(target, req)
           << " [" << shown << "/" << req << "]";

        if (shown >= req) ss << " {COMPLETE}";

        if (rewardK == ItemKind::Gold) {
            if (rewardC > 0) ss << " -> " << rewardC << "G";
        } else {
            const ItemDef& rd = itemDef(rewardK);
            if (isStackable(rewardK) && rewardC > 1) {
                ss << " -> " << rewardC << "x " << rd.name;
            } else {
                ss << " -> " << rd.name;
            }
        }

        pushMsg(ss.str(), MessageKind::Info, true);
    }
}
