#include "game_internal.hpp"

#include "noise_localization.hpp"

std::string Game::defaultSavePath() const {
    if (!savePathOverride.empty()) return savePathOverride;
    return "procrogue_save.dat";
}

void Game::setSavePath(const std::string& path) {
    savePathOverride = path;
}

void Game::setActiveSlot(std::string slot) {
    // Normalize/sanitize to keep slot filenames portable.
    slot = trim(slot);
    std::string low = toLower(slot);
    if (slot.empty() || low == "default" || low == "none" || low == "off") {
        slot.clear();
    } else {
        slot = sanitizeSlotName(slot);
    }

    // Compute base paths from the current save directory.
    const std::filesystem::path baseSave = baseSavePathForSlots(*this);
    const std::filesystem::path baseAuto = baseAutosavePathForSlots(*this);

    activeSlot_ = slot;

    if (activeSlot_.empty()) {
        savePathOverride = baseSave.string();
        autosavePathOverride = baseAuto.string();
    } else {
        savePathOverride = makeSlotPath(baseSave.string(), activeSlot_).string();
        autosavePathOverride = makeSlotPath(baseAuto.string(), activeSlot_).string();
    }
}

void Game::setSaveBackups(int count) {
    saveBackups_ = clampi(count, 0, 10);
}

std::string Game::defaultAutosavePath() const {
    if (!autosavePathOverride.empty()) return autosavePathOverride;

    // Default autosave goes next to the normal save file.
    std::filesystem::path basePath = std::filesystem::path(defaultSavePath()).parent_path();
    if (basePath.empty()) return "procrogue_autosave.dat";
    return (basePath / "procrogue_autosave.dat").string();
}

void Game::setAutosavePath(const std::string& path) {
    autosavePathOverride = path;
}

void Game::setAutosaveEveryTurns(int turns) {
    autosaveInterval = std::max(0, std::min(5000, turns));
}

std::string Game::defaultScoresPath() const {
    if (!scoresPathOverride.empty()) return scoresPathOverride;

    std::filesystem::path basePath = std::filesystem::path(defaultSavePath()).parent_path();
    if (basePath.empty()) return "procrogue_scores.csv";
    return (basePath / "procrogue_scores.csv").string();
}

void Game::setScoresPath(const std::string& path) {
    scoresPathOverride = path;
    // Non-fatal if missing; it will be created on first recorded run.
    (void)scores.load(defaultScoresPath());
}

void Game::setSettingsPath(const std::string& path) {
    settingsPath_ = path;
}

int Game::autoStepDelayMs() const {
    // Stored internally in seconds.
    return static_cast<int>(autoStepDelay * 1000.0f + 0.5f);
}

void Game::commandTextInput(const char* utf8) {
    if (!commandOpen) return;
    if (!utf8) return;
    // Basic length cap so the overlay stays sane.
    const size_t addLen = std::strlen(utf8);
    if (commandBuf.size() + addLen > 120) return;

    // Insert at the current cursor (byte) position.
    size_t cur = static_cast<size_t>(std::clamp(commandCursor_, 0, static_cast<int>(commandBuf.size())));
    commandBuf.insert(cur, utf8);
    commandCursor_ = static_cast<int>(cur + addLen);

    // Any manual edits cancel tab-completion cycling state.
    commandAutoBase.clear();
    commandAutoPrefix.clear();
    commandAutoMatches.clear();
    commandAutoHints.clear();
    commandAutoDescs.clear();
    commandAutoIndex = -1;
    commandAutoFuzzy = false;
}

static size_t utf8PrevIndex(const std::string& s, size_t i) {
    if (i == 0 || s.empty()) return 0;
    if (i > s.size()) i = s.size();
    size_t j = i - 1;
    // Walk back over UTF-8 continuation bytes (10xxxxxx).
    while (j > 0 && (static_cast<unsigned char>(s[j]) & 0xC0u) == 0x80u) {
        --j;
    }
    return j;
}

static size_t utf8NextIndex(const std::string& s, size_t i) {
    if (s.empty()) return 0;
    if (i >= s.size()) return s.size();
    size_t j = i + 1;
    // Skip continuation bytes to land on the next codepoint boundary.
    while (j < s.size() && (static_cast<unsigned char>(s[j]) & 0xC0u) == 0x80u) {
        ++j;
    }
    return j;
}

static bool vecContains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

static std::string longestCommonPrefix(const std::vector<std::string>& v) {
    if (v.empty()) return std::string();
    std::string pref = v[0];
    for (size_t i = 1; i < v.size(); ++i) {
        const std::string& s = v[i];
        size_t n = 0;
        const size_t maxn = std::min(pref.size(), s.size());
        while (n < maxn && pref[n] == s[n]) {
            ++n;
        }
        pref.resize(n);
        if (pref.empty()) break;
    }
    return pref;
}

void Game::commandBackspace() {
    if (!commandOpen) return;
    // Delete the codepoint immediately before the cursor.
    size_t cur = static_cast<size_t>(std::clamp(commandCursor_, 0, static_cast<int>(commandBuf.size())));
    if (cur == 0) return;
    const size_t prev = utf8PrevIndex(commandBuf, cur);
    commandBuf.erase(prev, cur - prev);
    commandCursor_ = static_cast<int>(prev);

    // Any manual edits cancel tab-completion cycling state.
    commandAutoBase.clear();
    commandAutoPrefix.clear();
    commandAutoMatches.clear();
    commandAutoHints.clear();
    commandAutoDescs.clear();
    commandAutoIndex = -1;
    commandAutoFuzzy = false;
}

void Game::commandAutocomplete() {
    if (!commandOpen) return;

    // Completion is only defined on the full line; if the user moved the cursor,
    // snap it back to the end (shell-style behaviour).
    if (commandCursor_ < static_cast<int>(commandBuf.size())) {
        commandCursor_ = static_cast<int>(commandBuf.size());
    }

    // Preserve whether the user explicitly typed a trailing whitespace character.
    // We use this to tell "complete the next argument" (e.g., "bind ") from
    // "complete this token" (e.g., "bind inv").
    bool trailingWS = false;
    if (!commandBuf.empty()) {
        const unsigned char last = static_cast<unsigned char>(commandBuf.back());
        trailingWS = (std::isspace(last) != 0);
    }

    std::string raw = trim(commandBuf);
    if (raw.empty()) return;

    // Support pasted NetHack-style inputs like "#quit" even though we open the prompt separately.
    bool hadHash = false;
    if (!raw.empty() && raw[0] == '#') {
        hadHash = true;
        raw = trim(raw.substr(1));
        if (raw.empty()) return;
    }

    // Split into whitespace tokens (command + args).
    const std::vector<std::string> toks = splitWS(raw);
    if (toks.empty()) return;

    // Determine which token to complete.
    // - If there's only one token and no trailing whitespace: complete the command itself.
    // - Otherwise, complete the last token, or the next token if the user ended with whitespace.
    int completeIdx = 0;
    if (toks.size() == 1 && !trailingWS) {
        completeIdx = 0;
    } else {
        completeIdx = trailingWS ? static_cast<int>(toks.size()) : static_cast<int>(toks.size()) - 1;
    }

    auto clearState = [&]() {
        commandAutoBase.clear();
        commandAutoPrefix.clear();
        commandAutoMatches.clear();
        commandAutoHints.clear();
        commandAutoDescs.clear();
        commandAutoIndex = -1;
        commandAutoFuzzy = false;
    };

    auto firstChord = [&](std::string s) -> std::string {
        s = trim(std::move(s));
        const size_t comma = s.find(',');
        if (comma != std::string::npos) s = trim(s.substr(0, comma));
        return s;
    };

    auto prettyChord = [&](std::string s) -> std::string {
        for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return s;
    };

    auto keybindHintForActionToken = [&](const std::string& tok) -> std::string {
        if (tok.empty()) return std::string();
        for (const auto& kv : keybindsDesc_) {
            if (kv.first == tok) {
                const std::string fc = firstChord(kv.second);
                if (fc.empty() || fc == "none") return std::string();
                return prettyChord(fc);
            }
        }
        return std::string();
    };

    // Subsequence fuzzy match with a cheap, stable score (lower is better).
    auto fuzzyScore = [&](const std::string& pat, const std::string& word, int& outScore) -> bool {
        size_t wi = 0;
        int gaps = 0;
        int first = -1;
        int last = -1;

        for (size_t pi = 0; pi < pat.size(); ++pi) {
            const char pc = pat[pi];
            const size_t found = word.find(pc, wi);
            if (found == std::string::npos) return false;
            if (first < 0) first = static_cast<int>(found);
            last = static_cast<int>(found);
            if (static_cast<int>(found) > static_cast<int>(wi)) gaps += static_cast<int>(found - wi);
            wi = found + 1;
        }

        const int span = (first >= 0 && last >= 0) ? (last - first) : 0;
        outScore = first * 2 + span + gaps;
        return true;
    };

    // Helper that resolves a possibly-short/aliased first token to a unique extended command.
    auto resolveCommand = [&]() -> std::string {
        std::string cmdIn = normalizeExtendedCommandAlias(toLower(toks[0]));
        const std::vector<std::string> cmds = extendedCommandList();

        // Exact match first.
        for (const auto& c : cmds) {
            if (c == cmdIn) return c;
        }

        // Unique prefix match.
        std::string match;
        for (const auto& c : cmds) {
            if (c.rfind(cmdIn, 0) == 0) {
                if (!match.empty()) return std::string(); // ambiguous
                match = c;
            }
        }

        return match;
    };

    auto currentTokenLower = [&]() -> std::string {
        if (completeIdx < 0) return std::string();
        if (completeIdx >= static_cast<int>(toks.size())) return std::string();
        return toLower(toks[static_cast<size_t>(completeIdx)]);
    };

    enum class Mode { CommandToken, ArgToken };
    Mode mode = Mode::CommandToken;

    // Prefix inserted before the completed candidate (does not include '#').
    std::string prefix;

    // Current (partial) token we're trying to complete.
    std::string cur = currentTokenLower();

    const std::string resolvedCmd = resolveCommand();

    // If we intended to complete an argument but can't resolve the command uniquely, fall back to
    // completing the command token itself (only when there are no other tokens that we'd destroy).
    if (completeIdx > 0 && resolvedCmd.empty()) {
        if (toks.size() > 1) {
            clearState();
            return;
        }

        mode = Mode::CommandToken;
        prefix.clear();
        cur = toLower(toks[0]);
        completeIdx = 0;
    } else if (completeIdx == 0) {
        mode = Mode::CommandToken;
        prefix.clear();
    } else if (completeIdx == 1) {
        // Context-sensitive argument completion (limited to a small set of commands).
        if (resolvedCmd == "bind" || resolvedCmd == "unbind" || resolvedCmd == "preset" ||
            resolvedCmd == "autopickup" || resolvedCmd == "identify" || resolvedCmd == "mortem") {
            mode = Mode::ArgToken;
            prefix = resolvedCmd + " ";
        } else {
            // Unsupported argument completion.
            clearState();
            return;
        }
    } else {
        // We don't attempt to complete deeper arguments (too context-specific).
        clearState();
        return;
    }

    // If we're already cycling completions (from a previous TAB), advance to the next match.
    if (commandAutoIndex >= 0 && !commandAutoMatches.empty() && !commandAutoBase.empty()) {
        // Keep cycling as long as:
        //  - we're completing the same token position (same prefix), and
        //  - the current token is one of the candidates, and
        //  - for prefix mode: the current token still starts with the original base.
        if (commandAutoPrefix == prefix && vecContains(commandAutoMatches, cur) &&
            (commandAutoFuzzy || cur.rfind(commandAutoBase, 0) == 0)) {
            commandAutoIndex = (commandAutoIndex + 1) % static_cast<int>(commandAutoMatches.size());
            const std::string& cand = commandAutoMatches[static_cast<size_t>(commandAutoIndex)];
            const std::string outLine = prefix + cand;
            commandBuf = hadHash ? ("#" + outLine) : outLine;
            commandCursor_ = static_cast<int>(commandBuf.size());
            return;
        }

        // Buffer changed (history/edit), so drop cycle state.
        clearState();
    }

    // Build the completion universe and compute a match set.
    std::vector<std::string> universe;
    std::vector<std::string> matches;
    bool fuzzyUsed = false;

    // Per-candidate UI extras (aligned 1:1 with matches)
    std::vector<std::string> hints;
    std::vector<std::string> descs;

    if (mode == Mode::CommandToken) {
        universe = extendedCommandList();

        // 1) Prefix matches first (classic NetHack-like behaviour).
        for (const auto& c : universe) {
            if (c.rfind(cur, 0) == 0) matches.push_back(c);
        }

        // 2) Fuzzy fallback when there are no prefix matches.
        if (matches.empty() && cur.size() >= 2) {
            struct Cand { int score; std::string s; };

            std::vector<Cand> cands;
            cands.reserve(universe.size());

            for (const auto& c : universe) {
                int score = 0;
                if (fuzzyScore(cur, c, score)) {
                    cands.push_back(Cand{score, c});
                }
            }

            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
                if (a.score != b.score) return a.score < b.score;
                return a.s < b.s;
            });

            const size_t keep = std::min<size_t>(cands.size(), 24);
            for (size_t i = 0; i < keep; ++i) matches.push_back(cands[i].s);
            fuzzyUsed = !matches.empty();
        }

        if (matches.empty()) {
            clearState();
            return;
        }

        hints.reserve(matches.size());
        descs.reserve(matches.size());

        for (const auto& m : matches) {
            const char* tok = extendedCommandActionToken(m);
            if (tok && tok[0] != '\0') hints.push_back(keybindHintForActionToken(tok));
            else hints.push_back(std::string());

            const char* d = extendedCommandShortDesc(m);
            descs.push_back(d ? std::string(d) : std::string());
        }

        if (matches.size() == 1) {
            const std::string outLine = matches[0] + " ";
            commandBuf = hadHash ? ("#" + outLine) : outLine;
            commandCursor_ = static_cast<int>(commandBuf.size());
            clearState();
            return;
        }

        // If all matches share a longer common *prefix*, extend to that.
        // This is only meaningful for prefix-mode completions.
        if (!fuzzyUsed) {
            const std::string lcp = longestCommonPrefix(matches);
            if (!lcp.empty() && lcp.size() > cur.size()) {
                commandBuf = hadHash ? ("#" + lcp) : lcp;
                commandCursor_ = static_cast<int>(commandBuf.size());

                // Keep the match set around so a subsequent TAB can begin cycling from this new prefix.
                commandAutoBase = lcp;
                commandAutoPrefix.clear();
                commandAutoMatches = matches;
                commandAutoHints = hints;
                commandAutoDescs = descs;
                commandAutoIndex = -1;
                commandAutoFuzzy = false;
                return;
            }
        }

        // Otherwise, start cycling through the available matches.
        commandAutoBase = cur;
        commandAutoPrefix.clear();
        commandAutoMatches = matches;
        commandAutoHints = hints;
        commandAutoDescs = descs;
        commandAutoFuzzy = fuzzyUsed;

        int startIdx = -1;
        for (size_t i = 0; i < matches.size(); ++i) {
            if (matches[i] == cur) {
                startIdx = static_cast<int>(i);
                break;
            }
        }

        if (startIdx >= 0) {
            commandAutoIndex = startIdx;
            commandBuf = hadHash ? ("#" + cur) : cur;
        } else {
            commandAutoIndex = 0;
            commandBuf = hadHash ? ("#" + matches[0]) : matches[0];
        }

        commandCursor_ = static_cast<int>(commandBuf.size());
        return;
    }

    // ArgToken mode: context-sensitive universe.
    if (resolvedCmd == "bind" || resolvedCmd == "unbind") {
        const size_t n = sizeof(actioninfo::kActionInfoTable) / sizeof(actioninfo::kActionInfoTable[0]);
        universe.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            const auto& info = actioninfo::kActionInfoTable[i];
            if (!info.token || info.token[0] == 0) continue;
            std::string tok = info.token;
            if (tok.rfind("bind_", 0) == 0) continue;
            universe.push_back(tok);
        }
    } else if (resolvedCmd == "preset") {
        universe = {"modern", "nethack"};
    } else if (resolvedCmd == "autopickup") {
        universe = {"off", "gold", "smart", "all"};
    } else if (resolvedCmd == "identify") {
        universe = {"on", "off"};
    } else if (resolvedCmd == "mortem") {
        universe = {"now", "on", "off"};
    }

    // Prefix matches first.
    for (const auto& u : universe) {
        if (u.rfind(cur, 0) == 0) matches.push_back(u);
    }

    // Fuzzy fallback (useful for action tokens with underscores).
    if (matches.empty() && cur.size() >= 2) {
        struct Cand { int score; std::string s; };
        std::vector<Cand> cands;
        cands.reserve(universe.size());

        for (const auto& u : universe) {
            int score = 0;
            if (fuzzyScore(cur, u, score)) {
                cands.push_back(Cand{score, u});
            }
        }

        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            if (a.score != b.score) return a.score < b.score;
            return a.s < b.s;
        });

        const size_t keep = std::min<size_t>(cands.size(), 24);
        for (size_t i = 0; i < keep; ++i) matches.push_back(cands[i].s);
        fuzzyUsed = !matches.empty();
    }

    if (matches.empty()) {
        clearState();
        return;
    }

    hints.reserve(matches.size());
    descs.reserve(matches.size());

    if (resolvedCmd == "bind" || resolvedCmd == "unbind") {
        for (const auto& m : matches) {
            hints.push_back(keybindHintForActionToken(m));

            std::string d;
            if (const auto* info = actioninfo::findByToken(m)) {
                if (info->desc && info->desc[0] != '\0') d = info->desc;
            }
            descs.push_back(d);
        }
    } else if (resolvedCmd == "preset") {
        for (const auto& m : matches) {
            hints.push_back(std::string());
            if (m == "modern") descs.push_back("WASD controls preset");
            else if (m == "nethack") descs.push_back("VI-keys NetHack controls preset");
            else descs.push_back(std::string());
        }
    } else if (resolvedCmd == "autopickup") {
        for (const auto& m : matches) {
            hints.push_back(std::string());
            if (m == "off") descs.push_back("Disable auto-pickup");
            else if (m == "gold") descs.push_back("Auto-pickup gold only");
            else if (m == "smart") descs.push_back("Auto-pickup smart set");
            else if (m == "all") descs.push_back("Auto-pickup everything");
            else descs.push_back(std::string());
        }
    } else if (resolvedCmd == "identify") {
        for (const auto& m : matches) {
            hints.push_back(std::string());
            if (m == "on") descs.push_back("Enable identification system");
            else if (m == "off") descs.push_back("Disable identification system");
            else descs.push_back(std::string());
        }
    } else if (resolvedCmd == "mortem") {
        for (const auto& m : matches) {
            hints.push_back(std::string());
            if (m == "now") descs.push_back("Export a run mortem immediately");
            else if (m == "on") descs.push_back("Enable auto-mortem on death");
            else if (m == "off") descs.push_back("Disable auto-mortem on death");
            else descs.push_back(std::string());
        }
    }

    if (matches.size() == 1) {
        const std::string outLine = prefix + matches[0] + " ";
        commandBuf = hadHash ? ("#" + outLine) : outLine;
        commandCursor_ = static_cast<int>(commandBuf.size());
        clearState();
        return;
    }

    // If all matches share a longer common prefix, extend to that (prefix-mode only).
    if (!fuzzyUsed) {
        const std::string lcp = longestCommonPrefix(matches);
        if (!lcp.empty() && lcp.size() > cur.size()) {
            const std::string outLine = prefix + lcp;
            commandBuf = hadHash ? ("#" + outLine) : outLine;
            commandCursor_ = static_cast<int>(commandBuf.size());

            commandAutoBase = lcp;
            commandAutoPrefix = prefix;
            commandAutoMatches = matches;
            commandAutoHints = hints;
            commandAutoDescs = descs;
            commandAutoIndex = -1;
            commandAutoFuzzy = false;
            return;
        }
    }

    // Otherwise, start cycling through the available matches.
    commandAutoBase = cur;
    commandAutoPrefix = prefix;
    commandAutoMatches = matches;
    commandAutoHints = hints;
    commandAutoDescs = descs;
    commandAutoFuzzy = fuzzyUsed;

    int startIdx = -1;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (matches[i] == cur) {
            startIdx = static_cast<int>(i);
            break;
        }
    }

    if (startIdx >= 0) {
        commandAutoIndex = startIdx;
        const std::string outLine = prefix + cur;
        commandBuf = hadHash ? ("#" + outLine) : outLine;
    } else {
        commandAutoIndex = 0;
        const std::string outLine = prefix + matches[0];
        commandBuf = hadHash ? ("#" + outLine) : outLine;
    }

    commandCursor_ = static_cast<int>(commandBuf.size());
}


void Game::commandCursorLeft() {
    if (!commandOpen) return;
    size_t cur = static_cast<size_t>(std::clamp(commandCursor_, 0, static_cast<int>(commandBuf.size())));
    if (cur == 0) return;
    commandCursor_ = static_cast<int>(utf8PrevIndex(commandBuf, cur));
}

void Game::commandCursorRight() {
    if (!commandOpen) return;
    size_t cur = static_cast<size_t>(std::clamp(commandCursor_, 0, static_cast<int>(commandBuf.size())));
    if (cur >= commandBuf.size()) {
        commandCursor_ = static_cast<int>(commandBuf.size());
        return;
    }
    commandCursor_ = static_cast<int>(utf8NextIndex(commandBuf, cur));
}

void Game::commandCursorHome() {
    if (!commandOpen) return;
    commandCursor_ = 0;
}

void Game::commandCursorEnd() {
    if (!commandOpen) return;
    commandCursor_ = static_cast<int>(commandBuf.size());
}


void Game::setAutoPickupMode(AutoPickupMode m) {
    autoPickup = m;
}

int Game::keyCount() const {
    int n = 0;
    for (const auto& it : inv) {
        if (it.kind == ItemKind::Key) n += std::max(0, it.count);
    }
    return n;
}

int Game::lockpickCount() const {
    int n = 0;
    for (const auto& it : inv) {
        if (it.kind == ItemKind::Lockpick) n += std::max(0, it.count);
    }
    return n;
}

int Game::shopDebtTotal() const {
    int total = 0;
    for (const auto& it : inv) {
        if (it.shopPrice <= 0 || it.shopDepth <= 0) continue;
        const int n = isStackable(it.kind) ? std::max(0, it.count) : 1;
        total += it.shopPrice * n;
    }

    // Additional debt recorded for consumed/destroyed unpaid goods (per depth).
    for (int d = 1; d <= DUNGEON_MAX_DEPTH; ++d) {
        total += std::max(0, shopDebtLedger_[d]);
    }

    return total;
}


int Game::shopDebtThisDepth() const {
    const int d = depth_;
    int total = 0;
    for (const auto& it : inv) {
        if (it.shopPrice <= 0 || it.shopDepth != d) continue;
        const int n = isStackable(it.kind) ? std::max(0, it.count) : 1;
        total += it.shopPrice * n;
    }

    if (d >= 1 && d <= DUNGEON_MAX_DEPTH) {
        total += std::max(0, shopDebtLedger_[d]);
    }
    return total;
}


bool Game::playerInShop() const {
    const Entity& p = player();
    return roomTypeAt(dung, p.pos) == RoomType::Shop;
}

bool Game::consumeKeys(int n) {
    if (n <= 0) return true;

    int need = n;
    for (auto& it : inv) {
        if (it.kind != ItemKind::Key) continue;
        int take = std::min(it.count, need);
        if (take <= 0) continue;

        // Using up unpaid shop goods still leaves you owing the shopkeeper.
        if (it.shopPrice > 0 && it.shopDepth > 0) {
            const int sd = it.shopDepth;
            if (sd >= 1 && sd <= DUNGEON_MAX_DEPTH) {
                shopDebtLedger_[sd] += take * it.shopPrice;
            }
        }

        it.count -= take;
        need -= take;
        if (need <= 0) break;
    }

    // Remove emptied stackables.
    inv.erase(std::remove_if(inv.begin(), inv.end(), [](const Item& it) {
        return isStackable(it.kind) && it.count <= 0;
    }), inv.end());

    return need <= 0;
}

bool Game::consumeLockpicks(int n) {
    if (n <= 0) return true;

    int need = n;
    for (auto& it : inv) {
        if (it.kind != ItemKind::Lockpick) continue;
        int take = std::min(it.count, need);
        if (take <= 0) continue;

        // Using up unpaid shop goods still leaves you owing the shopkeeper.
        if (it.shopPrice > 0 && it.shopDepth > 0) {
            const int sd = it.shopDepth;
            if (sd >= 1 && sd <= DUNGEON_MAX_DEPTH) {
                shopDebtLedger_[sd] += take * it.shopPrice;
            }
        }

        it.count -= take;
        need -= take;
        if (need <= 0) break;
    }

    // Remove emptied stackables.
    inv.erase(std::remove_if(inv.begin(), inv.end(), [](const Item& it) {
        return isStackable(it.kind) && it.count <= 0;
    }), inv.end());

    return need <= 0;
}

void Game::alertMonstersTo(Vec2i pos, int radius) {
    // radius<=0 means "global" (all monsters regardless of distance)
    for (auto& m : ents) {
        if (m.id == playerId_) continue;
        if (m.hp <= 0) continue;
        // Peaceful shopkeepers ignore generic alerts/noise.
        if (m.kind == EntityKind::Shopkeeper && !m.alerted) continue;

        if (radius > 0) {
            int dx = std::abs(m.pos.x - pos.x);
            int dy = std::abs(m.pos.y - pos.y);
            int cheb = std::max(dx, dy);
            if (cheb > radius) continue;
        }

        m.alerted = true;
        m.lastKnownPlayerPos = pos;
        m.lastKnownPlayerAge = 0;
        m.lastKnownPlayerUncertainty = 0;
    }
}



void Game::emitNoise(Vec2i pos, int volume) {
    if (volume <= 0) return;

    const int W = dung.width;
    auto idx = [&](int x, int y) { return y * W + x; };

    // Compute the max effective volume we might need for the loudest-hearing monster.
    int maxEff = volume;
    for (const auto& m : ents) {
        if (m.id == playerId_) continue;
        if (m.hp <= 0) continue;
        if (m.kind == EntityKind::Shopkeeper && !m.alerted) continue;
        const int eff = volume + (entityHearing(m.kind) - BASE_HEARING);
        if (eff > maxEff) maxEff = eff;
    }
    maxEff = std::max(0, maxEff);

    // Ensure deterministic substrate cache so sound propagation can incorporate
    // material acoustics (moss/dirt dampen; metal/crystal carry).
    dung.ensureMaterials(seed_, branch_, depth_, dungeonMaxDepth());

    // Dungeon-aware propagation: walls/secret doors block sound; doors + materials muffle/carry.
    const std::vector<int> sound = dung.computeSoundMap(pos.x, pos.y, maxEff);

    // Noise localization model:
    //   - Monsters still get alerted when a sound reaches them, but quiet/far noises
    //     do not necessarily pinpoint the exact source tile.
    //   - We derive a deterministic per-monster offset (no RNG stream consumption).

    auto validInvestigateTile = [&](Vec2i p) {
        if (!dung.inBounds(p.x, p.y)) return false;
        const TileType t = dung.at(p.x, p.y).type;
        switch (t) {
            case TileType::Wall:
            case TileType::Pillar:
            case TileType::DoorSecret:
            case TileType::Chasm:
            case TileType::Boulder:
                return false;
            default:
                return true;
        }
    };

    for (auto& m : ents) {
        if (m.id == playerId_) continue;
        if (m.hp <= 0) continue;
        if (m.kind == EntityKind::Shopkeeper && !m.alerted) continue;
        if (!dung.inBounds(m.pos.x, m.pos.y)) continue;

        const int eff = volume + (entityHearing(m.kind) - BASE_HEARING);
        if (eff <= 0) continue;

        const int d = sound[static_cast<size_t>(idx(m.pos.x, m.pos.y))];
        if (d < 0 || d > eff) continue;

        Vec2i investigatePos = pos;
        const int r = noiseInvestigateRadius(volume, eff, d);
        if (r > 0) {
            const uint32_t base = noiseInvestigateHash(seed_, turnCount, m.id, pos, volume, eff, d);

            // Try a few candidates (deterministic sequence) until we land on a reasonable tile.
            for (int attempt = 0; attempt < 10; ++attempt) {
                const uint32_t h = hashCombine(base, static_cast<uint32_t>(attempt));
                const Vec2i off = noiseInvestigateOffset(h, r);
                const Vec2i cand{pos.x + off.x, pos.y + off.y};
                if (!validInvestigateTile(cand)) continue;
                investigatePos = cand;
                break;
            }
        }

        m.alerted = true;
        m.lastKnownPlayerPos = investigatePos;
        m.lastKnownPlayerAge = 0;
        m.lastKnownPlayerUncertainty = static_cast<uint8_t>(clampi(r, 0, 255));
    }
}


void Game::setPlayerName(std::string name) {
    std::string n = trim(std::move(name));
    if (n.empty()) n = "PLAYER";

    // Strip control chars (keeps the HUD / CSV clean).
    std::string filtered;
    filtered.reserve(n.size());
    for (char c : n) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc == 127) continue;
        filtered.push_back(c);
    }

    filtered = trim(std::move(filtered));
    if (filtered.empty()) filtered = "PLAYER";
    if (filtered.size() > 24) filtered.resize(24);
    playerName_ = std::move(filtered);
}

void Game::setIdentificationEnabled(bool enabled) {
    identifyItemsEnabled = enabled;
}

void Game::setHungerEnabled(bool enabled) {
    hungerEnabled_ = enabled;

    // Initialize reasonable defaults lazily so older paths don't need to know.
    if (hungerMax <= 0) {
        hungerMax = 800 + std::max(0, DUNGEON_MAX_DEPTH - 10) * 40;
    }
    hunger = clampi(hunger, 0, hungerMax);

    hungerStatePrev = hungerStateFor(hunger, hungerMax);
}

std::string Game::hungerTag() const {
    if (!hungerEnabled_) return std::string();
    const int st = hungerStateFor(hunger, hungerMax);
    if (st == 1) return "HUNGRY";
    if (st >= 2) return "STARVING";
    return std::string();
}

namespace {
BurdenState burdenStateForWeights(int weight, int capacity) {
    // Use integer comparisons to avoid float edge cases.
    // Thresholds (ratio = weight/capacity):
    //   <=1.0: unburdened
    //   <=1.2: burdened
    //   <=1.4: stressed
    //   <=1.6: strained
    //   > 1.6: overloaded
    if (capacity <= 0) {
        return (weight > 0) ? BurdenState::Overloaded : BurdenState::Unburdened;
    }

    const int64_t w = static_cast<int64_t>(std::max(0, weight));
    const int64_t cap = static_cast<int64_t>(std::max(1, capacity));

    if (w <= cap) return BurdenState::Unburdened;
    if (w <= (cap * 6) / 5) return BurdenState::Burdened;  // 1.2x
    if (w <= (cap * 7) / 5) return BurdenState::Stressed;  // 1.4x
    if (w <= (cap * 8) / 5) return BurdenState::Strained;  // 1.6x
    return BurdenState::Overloaded;
}
}

void Game::setEncumbranceEnabled(bool enabled) {
    encumbranceEnabled_ = enabled;

    // This setter is called during early boot (main.cpp) before a run is created/loaded.
    // At that point there is no player entity yet, so computing burden would
    // dereference an empty entity list (undefined behavior -> 0xC0000005 on Windows).
    if (ents.empty() || playerId_ == 0) {
        burdenPrev_ = BurdenState::Unburdened;
        return;
    }

    burdenPrev_ = burdenState();
}

int Game::inventoryWeight() const {
    return totalWeight(inv);
}

int Game::carryCapacity() const {
    // Derive a simple carrying capacity from progression.
    // We deliberately reuse baseAtk as a "strength-like" stat to avoid bloating the save format.
    const Entity& p = player();

    const int strLike = std::max(1, p.baseAtk + playerMight());
    int cap = 80 + (strLike * 18) + (std::max(1, charLevel) * 6);

    // PACK MULE companions slightly increase your effective carrying capacity when nearby.
    // This is deliberately modest; it acts as a quality-of-life boon rather than an infinite stash.
    int packBonus = 0;
    for (const auto& e : ents) {
        if (e.id == playerId_) continue;
        if (e.hp <= 0) continue;
        if (!e.friendly) continue;
        if (!petgen::petHasTrait(e.procAffixMask, petgen::PetTrait::PackMule)) continue;

        // Only count companions that are close enough to plausibly help carry.
        const int dx = std::abs(e.pos.x - p.pos.x);
        const int dy = std::abs(e.pos.y - p.pos.y);
        const int cheb = std::max(dx, dy);
        if (cheb > 8) continue;

        // If they're ordered to STAY/GUARD elsewhere, they aren't helping carry your pack.
        if (!(e.allyOrder == AllyOrder::Follow || e.allyOrder == AllyOrder::Fetch)) continue;

        packBonus += 35;
    }

    cap += packBonus;
    cap = clampi(cap, 60, 9999);
    return cap;
}

BurdenState Game::burdenState() const {
    if (!encumbranceEnabled_) return BurdenState::Unburdened;
    return burdenStateForWeights(inventoryWeight(), carryCapacity());
}

std::string Game::burdenTag() const {
    if (!encumbranceEnabled_) return std::string();
    switch (burdenState()) {
        case BurdenState::Unburdened: return std::string();
        case BurdenState::Burdened:   return "BURDENED";
        case BurdenState::Stressed:   return "STRESSED";
        case BurdenState::Strained:   return "STRAINED";
        case BurdenState::Overloaded: return "OVERLOADED";
    }
    return std::string();
}

void Game::setSneakMode(bool enabled, bool quiet) {
    if (sneakMode_ == enabled) return;
    sneakMode_ = enabled;

    if (!quiet) {
        if (sneakMode_) pushMsg("YOU BEGIN SNEAKING.", MessageKind::System, true);
        else pushMsg("YOU STOP SNEAKING.", MessageKind::System, true);
    }
}

void Game::toggleSneakMode(bool quiet) {
    setSneakMode(!sneakMode_, quiet);
}

std::string Game::sneakTag() const {
    return sneakMode_ ? "SNEAK" : std::string();
}

void Game::setLightingEnabled(bool enabled) {
    lightingEnabled_ = enabled;

    // This setter is called during early boot (main.cpp) before a run is created/loaded.
    // FOV/light-map recomputation requires a valid player position.
    if (ents.empty() || playerId_ == 0) {
        lightMap_.clear();
        return;
    }

    // Ensure cached lighting/FOV state matches the new mode.
    // recomputeFov() calls recomputeLightMap() internally.
    recomputeFov();
}

void Game::setYendorDoomEnabled(bool enabled) {
    yendorDoomEnabled_ = enabled;

    // If the system is disabled, we simply pause it (state is preserved so it
    // can be re-enabled later).
    if (!yendorDoomEnabled_) {
        yendorDoomActive_ = false;
        yendorDoomLevel_ = 0;
        return;
    }

    // If the player already has the Amulet, enable immediately.
    if (!gameOver && !gameWon && playerId_ != 0 && playerHasAmulet()) {
        yendorDoomActive_ = true;
        if (yendorDoomStartTurn_ == 0) yendorDoomStartTurn_ = turnCount;
        if (yendorDoomLastPulseTurn_ == 0) yendorDoomLastPulseTurn_ = turnCount;
        if (yendorDoomLastSpawnTurn_ == 0) yendorDoomLastSpawnTurn_ = turnCount;
    }
}

bool Game::darknessActive() const {
    // Keep early floors bright by default; darkness starts deeper.
    return lightingEnabled_ && depth_ >= 4;
}

uint8_t Game::tileLightLevel(int x, int y) const {
    if (!dung.inBounds(x, y)) return 0;
    const size_t i = static_cast<size_t>(y * dung.width + x);
    if (i >= lightMap_.size()) return 255;
    return lightMap_[i];
}

Color Game::tileLightColor(int x, int y) const {
    if (!dung.inBounds(x, y)) return { 0, 0, 0, 255 };
    const size_t i = static_cast<size_t>(y * dung.width + x);
    if (i >= lightColorMap_.size()) {
        // If lighting isn't active (or the cache hasn't been built yet), default to white.
        return darknessActive() ? Color{ 0, 0, 0, 255 } : Color{ 255, 255, 255, 255 };
    }
    return lightColorMap_[i];
}


std::string Game::lightTag() const {
    if (!darknessActive()) return std::string();

    // If carrying a lit torch, show remaining fuel (min across lit torches).
    int best = -1;
    for (const auto& it : inv) {
        if (it.kind == ItemKind::TorchLit && it.charges > 0) {
            if (best < 0) best = it.charges;
            else best = std::min(best, it.charges);
        }
    }
    if (best >= 0) {
        std::ostringstream ss;
        ss << "TORCH(" << best << ")";
        return ss.str();
    }

    // If wielding a flaming weapon, show a simple indicator.
    if (const Item* w = equippedMelee()) {
        if (w->ego == ItemEgo::Flaming) {
            return "GLOW";
        }
    }

    // Warning when you're standing in darkness without a light source.
    const Vec2i p = player().pos;
    if (dung.inBounds(p.x, p.y) && tileLightLevel(p.x, p.y) == 0) {
        return "DARK";
    }
    return std::string();
}

void Game::setAutoStepDelayMs(int ms) {
    // Clamp to sane values to avoid accidental 0ms "teleport walking".
    const int clamped = clampi(ms, 10, 500);
    autoStepDelay = clamped / 1000.0f;
}

namespace {
constexpr uint32_t SAVE_MAGIC = 0x50525356u; // 'PRSV'
constexpr uint32_t SAVE_VERSION = 54u; // v54: parry stance effect

constexpr uint32_t BONES_MAGIC = 0x454E4F42u; // "BONE" (little-endian)
constexpr uint32_t BONES_VERSION = 2u;


// v13+: append CRC32 of the entire payload (all bytes up to but excluding the CRC field).
static uint32_t crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool inited = false;
    if (!inited) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        inited = true;
    }

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t readU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

static void appendU32LE(std::string& s, uint32_t v) {
    char b[4];
    b[0] = static_cast<char>(v & 0xFFu);
    b[1] = static_cast<char>((v >> 8) & 0xFFu);
    b[2] = static_cast<char>((v >> 16) & 0xFFu);
    b[3] = static_cast<char>((v >> 24) & 0xFFu);
    s.append(b, 4);
}

template <typename T>
void writePod(std::ostream& out, const T& v) {
    out.write(reinterpret_cast<const char*>(&v), static_cast<std::streamsize>(sizeof(T)));
}

template <typename T>
bool readPod(std::istream& in, T& v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), static_cast<std::streamsize>(sizeof(T))));
}

void writeString(std::ostream& out, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    writePod(out, len);
    if (len) out.write(s.data(), static_cast<std::streamsize>(len));
}

bool readString(std::istream& in, std::string& s) {
    uint32_t len = 0;
    if (!readPod(in, len)) return false;
    s.assign(len, '\0');
    if (len) {
        if (!in.read(s.data(), static_cast<std::streamsize>(len))) return false;
    }
    return true;
}

void writeItem(std::ostream& out, const Item& it) {
    int32_t id = static_cast<int32_t>(it.id);
    writePod(out, id);
    uint8_t kind = static_cast<uint8_t>(it.kind);
    writePod(out, kind);
    int32_t count = static_cast<int32_t>(it.count);
    writePod(out, count);
    int32_t charges = static_cast<int32_t>(it.charges);
    writePod(out, charges);
    writePod(out, it.spriteSeed);
    int32_t enchant = static_cast<int32_t>(it.enchant);
    writePod(out, enchant);

    // v10: blessed/uncursed/cursed state (-1..1)
    int8_t buc = static_cast<int8_t>(clampi(it.buc, -1, 1));
    writePod(out, buc);

    // v11: shop metadata (per-unit price + owning depth)
    int32_t shopPrice = static_cast<int32_t>(std::max(0, it.shopPrice));
    int32_t shopDepth = static_cast<int32_t>(std::max(0, it.shopDepth));
    writePod(out, shopPrice);
    writePod(out, shopDepth);

    // v37: item ego/brand (append-only)
    uint8_t ego = static_cast<uint8_t>(it.ego);
    writePod(out, ego);

    // v41: item flags (append-only)
    if constexpr (SAVE_VERSION >= 41u) {
        writePod(out, it.flags);
    }
}

bool readItem(std::istream& in, Item& it, uint32_t version) {
    int32_t id = 0;
    uint8_t kind = 0;
    int32_t count = 0;
    int32_t charges = 0;
    uint32_t seed = 0;
    int32_t enchant = 0;
    int8_t buc = 0;
    int32_t shopPrice = 0;
    int32_t shopDepth = 0;
    uint8_t ego = 0;
    uint8_t flags = 0;
    if (!readPod(in, id)) return false;
    if (!readPod(in, kind)) return false;
    if (!readPod(in, count)) return false;
    if (!readPod(in, charges)) return false;
    if (!readPod(in, seed)) return false;
    if (version >= 2u) {
        if (!readPod(in, enchant)) return false;
    }
    if (version >= 10u) {
        if (!readPod(in, buc)) return false;
    }
    if (version >= 11u) {
        if (!readPod(in, shopPrice)) return false;
        if (!readPod(in, shopDepth)) return false;
    }
    if (version >= 37u) {
        if (!readPod(in, ego)) return false;
        if (ego >= static_cast<uint8_t>(ITEM_EGO_COUNT)) ego = 0;
    }
    if (version >= 41u) {
        if (!readPod(in, flags)) return false;
        // Clamp to known bits for safety.
        flags = static_cast<uint8_t>(flags & ITEM_FLAG_MIMIC_BAIT);
    }
    it.id = id;
    it.kind = static_cast<ItemKind>(kind);
    it.count = count;
    it.charges = charges;
    it.spriteSeed = seed;
    it.enchant = enchant;
    it.buc = static_cast<int>(buc);
    it.shopPrice = static_cast<int>(shopPrice);
    it.shopDepth = static_cast<int>(shopDepth);
    it.ego = static_cast<ItemEgo>(ego);
    it.flags = flags;
    return true;
}

void writeEntity(std::ostream& out, const Entity& e) {
    int32_t id = static_cast<int32_t>(e.id);
    writePod(out, id);
    uint8_t kind = static_cast<uint8_t>(e.kind);
    writePod(out, kind);
    int32_t x = e.pos.x;
    int32_t y = e.pos.y;
    writePod(out, x);
    writePod(out, y);
    int32_t hp = e.hp;
    int32_t hpMax = e.hpMax;
    int32_t atk = e.baseAtk;
    int32_t def = e.baseDef;
    writePod(out, hp);
    writePod(out, hpMax);
    writePod(out, atk);
    writePod(out, def);
    writePod(out, e.spriteSeed);
    int32_t groupId = e.groupId;
    writePod(out, groupId);
    uint8_t alerted = e.alerted ? 1 : 0;
    writePod(out, alerted);

    uint8_t canRanged = e.canRanged ? 1 : 0;
    writePod(out, canRanged);
    int32_t rRange = e.rangedRange;
    int32_t rAtk = e.rangedAtk;
    writePod(out, rRange);
    writePod(out, rAtk);
    uint8_t rAmmo = static_cast<uint8_t>(e.rangedAmmo);
    uint8_t rProj = static_cast<uint8_t>(e.rangedProjectile);
    writePod(out, rAmmo);
    writePod(out, rProj);

    uint8_t packAI = e.packAI ? 1 : 0;
    uint8_t willFlee = e.willFlee ? 1 : 0;
    writePod(out, packAI);
    writePod(out, willFlee);

    int32_t regenChance = e.regenChancePct;
    int32_t regenAmt = e.regenAmount;
    writePod(out, regenChance);
    writePod(out, regenAmt);

    // v2+: timed status effects
    int32_t poison = e.effects.poisonTurns;
    int32_t regenTurns = e.effects.regenTurns;
    int32_t shieldTurns = e.effects.shieldTurns;
    writePod(out, poison);
    writePod(out, regenTurns);
    writePod(out, shieldTurns);

    // v3+: additional buffs
    int32_t hasteTurns = e.effects.hasteTurns;
    int32_t visionTurns = e.effects.visionTurns;
    writePod(out, hasteTurns);
    writePod(out, visionTurns);

    // v6+: additional debuffs
    int32_t webTurns = e.effects.webTurns;
    writePod(out, webTurns);

    // v8+: invisibility
    int32_t invisTurns = e.effects.invisTurns;
    writePod(out, invisTurns);

    // v12+: confusion
    int32_t confusionTurns = e.effects.confusionTurns;
    writePod(out, confusionTurns);

    // v22+: burning
    int32_t burnTurns = e.effects.burnTurns;
    writePod(out, burnTurns);

    // v30+: levitation
    int32_t levitationTurns = e.effects.levitationTurns;
    writePod(out, levitationTurns);

    // v32+: fear
    int32_t fearTurns = e.effects.fearTurns;
    writePod(out, fearTurns);

    // v35+: hallucination
    int32_t hallucinationTurns = e.effects.hallucinationTurns;
    writePod(out, hallucinationTurns);

    // v53+: corrosion
    int32_t corrosionTurns = e.effects.corrosionTurns;
    writePod(out, corrosionTurns);

    // v54+: parry stance
    int32_t parryTurns = e.effects.parryTurns;
    writePod(out, parryTurns);

    // v14+: ranged ammo count (ammo-based ranged monsters)
    int32_t ammoCount = e.rangedAmmoCount;
    writePod(out, ammoCount);


    // v17+: monster gear (melee weapon + armor). Player ignores these fields.
    writeItem(out, e.gearMelee);
    writeItem(out, e.gearArmor);

    // v23+: companion flags (friendly + ally order)
    uint8_t friendly = e.friendly ? 1 : 0;
    uint8_t order = static_cast<uint8_t>(e.allyOrder);
    writePod(out, friendly);
    writePod(out, order);

    // v28+: monsters can carry stolen gold (used by Leprechauns, etc.)
    int32_t stolenGold = e.stolenGold;
    writePod(out, stolenGold);

    // v38+: pocket consumable (monsters only; player ignores this field)
    writeItem(out, e.pocketConsumable);

    // v39+: monster AI memory (last known player pos + age) and turn scheduling.
    // These fields affect deterministic simulation and save/load fidelity.
    if constexpr (SAVE_VERSION >= 39u) {
        int32_t lkx = e.lastKnownPlayerPos.x;
        int32_t lky = e.lastKnownPlayerPos.y;
        int32_t lkAge = e.lastKnownPlayerAge;
        int32_t speed = e.speed;
        int32_t energy = e.energy;
        writePod(out, lkx);
        writePod(out, lky);
        writePod(out, lkAge);
        writePod(out, speed);
        writePod(out, energy);
    }

    // v49+: procedural monster variants (rank + affix mask)
    if constexpr (SAVE_VERSION >= 49u) {
        uint8_t procRank = static_cast<uint8_t>(e.procRank);
        uint32_t procMask = e.procAffixMask;
        writePod(out, procRank);
        writePod(out, procMask);
    }

    // v50+: procedural monster abilities (two-slot kit + cooldowns)
    if constexpr (SAVE_VERSION >= 50u) {
        uint8_t a1 = static_cast<uint8_t>(e.procAbility1);
        uint8_t a2 = static_cast<uint8_t>(e.procAbility2);
        int32_t cd1 = e.procAbility1Cd;
        int32_t cd2 = e.procAbility2Cd;
        writePod(out, a1);
        writePod(out, a2);
        writePod(out, cd1);
        writePod(out, cd2);
    }
}

bool readEntity(std::istream& in, Entity& e, uint32_t version) {
    int32_t id = 0;
    uint8_t kind = 0;
    int32_t x = 0, y = 0;
    int32_t hp = 0, hpMax = 0;
    int32_t atk = 0, def = 0;
    uint32_t seed = 0;
    int32_t groupId = 0;
    uint8_t alerted = 0;

    uint8_t canRanged = 0;
    int32_t rRange = 0;
    int32_t rAtk = 0;
    uint8_t rAmmo = 0;
    uint8_t rProj = 0;
    int32_t rangedAmmoCount = 0;

    uint8_t packAI = 0;
    uint8_t willFlee = 0;

    int32_t regenChance = 0;
    int32_t regenAmt = 0;

    int32_t poison = 0;
    int32_t regenTurns = 0;
    int32_t shieldTurns = 0;
    int32_t hasteTurns = 0;
    int32_t visionTurns = 0;
    int32_t webTurns = 0;
    int32_t invisTurns = 0;
    int32_t confusionTurns = 0;
    int32_t burnTurns = 0;
    int32_t levitationTurns = 0;

    int32_t fearTurns = 0;

    int32_t hallucinationTurns = 0;

    int32_t corrosionTurns = 0;

    int32_t parryTurns = 0;

    int32_t stolenGold = 0;

    Item pocketConsumable;

    // v39+: monster AI memory and energy scheduling.
    int32_t lkx = -1;
    int32_t lky = -1;
    int32_t lkAge = 9999;
    int32_t speed = 0;
    int32_t energy = 0;

    // v49+: procedural monster variants.
    uint8_t procRank = 0;
    uint32_t procMask = 0u;

    // v50+: procedural monster abilities
    uint8_t procAbility1 = 0;
    uint8_t procAbility2 = 0;
    int32_t procAbility1Cd = 0;
    int32_t procAbility2Cd = 0;

    Item gearMelee;
    Item gearArmor;

    if (!readPod(in, id)) return false;
    if (!readPod(in, kind)) return false;
    if (!readPod(in, x)) return false;
    if (!readPod(in, y)) return false;
    if (!readPod(in, hp)) return false;
    if (!readPod(in, hpMax)) return false;
    if (!readPod(in, atk)) return false;
    if (!readPod(in, def)) return false;
    if (!readPod(in, seed)) return false;
    if (!readPod(in, groupId)) return false;
    if (!readPod(in, alerted)) return false;

    if (!readPod(in, canRanged)) return false;
    if (!readPod(in, rRange)) return false;
    if (!readPod(in, rAtk)) return false;
    if (!readPod(in, rAmmo)) return false;
    if (!readPod(in, rProj)) return false;

    if (!readPod(in, packAI)) return false;
    if (!readPod(in, willFlee)) return false;

    if (!readPod(in, regenChance)) return false;
    if (!readPod(in, regenAmt)) return false;

    if (version >= 2u) {
        if (!readPod(in, poison)) return false;
        if (!readPod(in, regenTurns)) return false;
        if (!readPod(in, shieldTurns)) return false;

        if (version >= 3u) {
            if (!readPod(in, hasteTurns)) return false;
            if (!readPod(in, visionTurns)) return false;
        }

        if (version >= 6u) {
            if (!readPod(in, webTurns)) return false;
        }

        if (version >= 8u) {
            if (!readPod(in, invisTurns)) return false;
        }

        if (version >= 12u) {
            if (!readPod(in, confusionTurns)) return false;
        }

        if (version >= 22u) {
            if (!readPod(in, burnTurns)) return false;
        }

        if (version >= 30u) {
            if (!readPod(in, levitationTurns)) return false;
        }

        if (version >= 32u) {
            if (!readPod(in, fearTurns)) return false;
        }

        if (version >= 35u) {
            if (!readPod(in, hallucinationTurns)) return false;
        }

        if (version >= 53u) {
            if (!readPod(in, corrosionTurns)) return false;
        }

        if (version >= 54u) {
            if (!readPod(in, parryTurns)) return false;
        }
    }

    if (version >= 14u) {
        if (!readPod(in, rangedAmmoCount)) return false;
    }


    if (version >= 17u) {
        if (!readItem(in, gearMelee, version)) return false;
        if (!readItem(in, gearArmor, version)) return false;
    }

    uint8_t friendly = 0;
    uint8_t order = 0;
    if (version >= 23u) {
        if (!readPod(in, friendly)) return false;
        if (!readPod(in, order)) return false;
    }

    if (version >= 28u) {
        if (!readPod(in, stolenGold)) return false;
    }

    if (version >= 38u) {
        if (!readItem(in, pocketConsumable, version)) return false;
    }

    // v39+: monster AI memory + energy scheduling
    if (version >= 39u) {
        if (!readPod(in, lkx)) return false;
        if (!readPod(in, lky)) return false;
        if (!readPod(in, lkAge)) return false;
        if (!readPod(in, speed)) return false;
        if (!readPod(in, energy)) return false;
    }

    // v49+: procedural monster variants (rank + affix mask)
    if (version >= 49u) {
        if (!readPod(in, procRank)) return false;
        if (!readPod(in, procMask)) return false;
    }

    // v50+: procedural monster abilities (two-slot kit + cooldowns)
    if (version >= 50u) {
        if (!readPod(in, procAbility1)) return false;
        if (!readPod(in, procAbility2)) return false;
        if (!readPod(in, procAbility1Cd)) return false;
        if (!readPod(in, procAbility2Cd)) return false;
    }

    e.id = id;
    e.kind = static_cast<EntityKind>(kind);
    e.pos = { x, y };
    e.hp = hp;
    e.hpMax = hpMax;
    e.baseAtk = atk;
    e.baseDef = def;
    e.spriteSeed = seed;

    if (version >= 49u) {
        if (procRank > static_cast<uint8_t>(ProcMonsterRank::Mythic)) procRank = 0;
        e.procRank = static_cast<ProcMonsterRank>(procRank);
        e.procAffixMask = procMask;
    } else {
        e.procRank = ProcMonsterRank::Normal;
        e.procAffixMask = 0u;
    }

    // v50+: procedural monster abilities
    if (version >= 50u) {
        e.procAbility1 = static_cast<ProcMonsterAbility>(procAbility1);
        e.procAbility2 = static_cast<ProcMonsterAbility>(procAbility2);
        e.procAbility1Cd = procAbility1Cd;
        e.procAbility2Cd = procAbility2Cd;
    } else {
        e.procAbility1 = ProcMonsterAbility::None;
        e.procAbility2 = ProcMonsterAbility::None;
        e.procAbility1Cd = 0;
        e.procAbility2Cd = 0;
    }


    e.groupId = groupId;
    e.alerted = alerted != 0;

    e.canRanged = canRanged != 0;
    e.rangedRange = rRange;
    e.rangedAtk = rAtk;
    e.rangedAmmo = static_cast<AmmoKind>(rAmmo);
    e.rangedProjectile = static_cast<ProjectileKind>(rProj);

    if (version >= 14u) {
        e.rangedAmmoCount = rangedAmmoCount;
    } else {
        // Older saves had implicit infinite ammo; give ammo-based ranged monsters a reasonable default.
        if (e.kind != EntityKind::Player && e.canRanged && e.rangedAmmo != AmmoKind::None) {
            if (e.kind == EntityKind::KoboldSlinger) e.rangedAmmoCount = 18;
            else if (e.kind == EntityKind::SkeletonArcher) e.rangedAmmoCount = 12;
            else e.rangedAmmoCount = 10;
        }
    }

    e.packAI = packAI != 0;
    e.willFlee = willFlee != 0;

    e.regenChancePct = regenChance;
    e.regenAmount = regenAmt;

    e.effects.poisonTurns = poison;
    e.effects.regenTurns = regenTurns;
    e.effects.shieldTurns = shieldTurns;
    e.effects.hasteTurns = hasteTurns;
    e.effects.visionTurns = visionTurns;
    e.effects.webTurns = webTurns;
    e.effects.invisTurns = invisTurns;
    e.effects.confusionTurns = confusionTurns;
    e.effects.burnTurns = burnTurns;
    e.effects.levitationTurns = levitationTurns;
    e.effects.fearTurns = fearTurns;
    e.effects.hallucinationTurns = hallucinationTurns;
    e.effects.corrosionTurns = corrosionTurns;
    e.effects.parryTurns = (version >= 54u) ? parryTurns : 0;


    if (version >= 17u) {
        e.gearMelee = gearMelee;
        e.gearArmor = gearArmor;
    } else {
        // Older saves: monsters had no explicit gear.
        e.gearMelee.id = 0;
        e.gearArmor.id = 0;
    }
    // v23+: companion flags
    if (version >= 23u) {
        e.friendly = (friendly != 0);
        e.allyOrder = static_cast<AllyOrder>(order);
    } else {
        // Older saves: only the starting dog was friendly.
        e.friendly = (e.kind == EntityKind::Dog);
        e.allyOrder = AllyOrder::Follow;
    }

    // v28+: carried/stolen gold
    if (version >= 28u) {
        e.stolenGold = stolenGold;
    } else {
        e.stolenGold = 0;
    }

    // v38+: pocket consumable
    if (version >= 38u) {
        e.pocketConsumable = pocketConsumable;
    } else {
        e.pocketConsumable.id = 0;
    }

    if (version >= 39u) {
        e.lastKnownPlayerPos = Vec2i{lkx, lky};
        e.lastKnownPlayerAge = lkAge;

        // Defensive: keep corrupted saves from creating pathological scheduler state.
        e.speed = speed;
        if (e.speed <= 0) e.speed = baseSpeedFor(e.kind);
        e.energy = energy;
        if (e.energy < 0) e.energy = 0;
    } else {
        // Older saves: these runtime fields were not persisted.
        e.lastKnownPlayerPos = Vec2i{-1, -1};
        e.lastKnownPlayerAge = 9999;
        e.speed = baseSpeedFor(e.kind);
        e.energy = 0;
    }

    return true;
}

} // namespace

bool Game::saveToFile(const std::string& path, bool quiet) {
    // Overworld chunks (Camp depth 0 outside the hub) are not yet serialized.
    // Prevent saving there so loading cannot strand the player on an untracked chunk.
    if (atCamp() && !atHomeCamp()) {
        if (!quiet) {
            pushMsg("YOU CANNOT SAVE WHILE LOST IN THE WILDERNESS.", MessageKind::Warning, true);
        }
        return false;
    }

    // Ensure the currently-loaded level is persisted into `levels`.
    storeCurrentLevel();

    std::filesystem::path p(path);
    std::filesystem::path dir = p.parent_path();
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
    }

    // Build the save payload in-memory so we can append an integrity footer (CRC)
    // while still writing atomically via a temp file.
    std::ostringstream mem(std::ios::binary | std::ios::out);

    writePod(mem, SAVE_MAGIC);
    writePod(mem, SAVE_VERSION);

    uint32_t rngState = rng.state;
    writePod(mem, rngState);

    // v45+: active branch (separate from numeric depth)
    if constexpr (SAVE_VERSION >= 45u) {
        const uint8_t br = static_cast<uint8_t>(branch_);
        writePod(mem, br);
    }

    int32_t depth = depth_;
    writePod(mem, depth);

    int32_t playerId = playerId_;
    writePod(mem, playerId);

    int32_t nextE = nextEntityId;
    int32_t nextI = nextItemId;
    writePod(mem, nextE);
    writePod(mem, nextI);

    int32_t eqM = equipMeleeId;
    int32_t eqR = equipRangedId;
    int32_t eqA = equipArmorId;
    writePod(mem, eqM);
    writePod(mem, eqR);
    writePod(mem, eqA);

    // v19+: ring slots (two fingers)
    if constexpr (SAVE_VERSION >= 19u) {
        int32_t eq1 = equipRing1Id;
        int32_t eq2 = equipRing2Id;
        writePod(mem, eq1);
        writePod(mem, eq2);
    }

    int32_t clvl = charLevel;
    int32_t xpNow = xp;
    int32_t xpNeed = xpNext;
    writePod(mem, clvl);
    writePod(mem, xpNow);
    writePod(mem, xpNeed);

    // v16+: talent allocations
    if constexpr (SAVE_VERSION >= 16u) {
        writePod(mem, static_cast<int32_t>(talentMight_));
        writePod(mem, static_cast<int32_t>(talentAgility_));
        writePod(mem, static_cast<int32_t>(talentVigor_));
        writePod(mem, static_cast<int32_t>(talentFocus_));
        writePod(mem, static_cast<int32_t>(talentPointsPending_));
        writePod(mem, static_cast<int32_t>(levelUpSel));
    }

    uint8_t over = gameOver ? 1 : 0;
    uint8_t won = gameWon ? 1 : 0;
    writePod(mem, over);
    writePod(mem, won);

    // v2+: user/options
    uint8_t autoPick = static_cast<uint8_t>(autoPickup);
    writePod(mem, autoPick);

    // v3+: pacing state
    uint32_t turnsNow = turnCount;
    int32_t natRegen = naturalRegenCounter;
    uint8_t hasteP = hastePhase ? 1 : 0;
    writePod(mem, turnsNow);
    writePod(mem, natRegen);
    writePod(mem, hasteP);

    // v5+: run meta
    uint32_t seedNow = seed_;
    uint32_t killsNow = killCount;
    int32_t maxD = maxDepth;
    writePod(mem, seedNow);
    writePod(mem, killsNow);
    writePod(mem, maxD);

    // v52+: conduct counters (NetHack-style voluntary challenges)
    if constexpr (SAVE_VERSION >= 52u) {
        uint32_t directKillsNow = directKillCount_;
        uint32_t foodNow = conductFoodEaten_;
        uint32_t corpseNow = conductCorpseEaten_;
        uint32_t scrollNow = conductScrollsRead_;
        uint32_t bookNow = conductSpellbooksRead_;
        uint32_t prayNow = conductPrayers_;
        writePod(mem, directKillsNow);
        writePod(mem, foodNow);
        writePod(mem, corpseNow);
        writePod(mem, scrollNow);
        writePod(mem, bookNow);
        writePod(mem, prayNow);
    }

    // v26+: monster codex (seen flags + kill counts; per-run)
    if constexpr (SAVE_VERSION >= 26u) {
        const uint32_t kindCount = static_cast<uint32_t>(ENTITY_KIND_COUNT);
        writePod(mem, kindCount);
        for (uint32_t i = 0; i < kindCount; ++i) {
            writePod(mem, codexSeen_[i]);
            writePod(mem, codexKills_[i]);
        }
    }

    // v6+: item identification tables (run knowledge + randomized appearances)
    uint32_t kindCount = static_cast<uint32_t>(ITEM_KIND_COUNT);
    writePod(mem, kindCount);
    for (uint32_t i = 0; i < kindCount; ++i) {
        const uint8_t known = identKnown[static_cast<size_t>(i)];
        const uint8_t app = identAppearance[static_cast<size_t>(i)];
        writePod(mem, known);
        writePod(mem, app);
    }

    // v48+: per-run "call" labels for unidentified appearances (NetHack-style notes).
    if constexpr (SAVE_VERSION >= 48u) {
        for (uint32_t i = 0; i < kindCount; ++i) {
            writeString(mem, identCall[static_cast<size_t>(i)]);
        }
    }

    // v7+: hunger system state (per-run)
    uint8_t hungerEnabledTmp = hungerEnabled_ ? 1u : 0u;
    int32_t hungerTmp = static_cast<int32_t>(hunger);
    int32_t hungerMaxTmp = static_cast<int32_t>(hungerMax);
    writePod(mem, hungerEnabledTmp);
    writePod(mem, hungerTmp);
    writePod(mem, hungerMaxTmp);

    // v9+: lighting system state (per-run)
    uint8_t lightingEnabledTmp = lightingEnabled_ ? 1u : 0u;
    writePod(mem, lightingEnabledTmp);

    // v18+: sneak mode (per-run)
    if constexpr (SAVE_VERSION >= 18u) {
        uint8_t sneakEnabledTmp = sneakMode_ ? 1u : 0u;
        writePod(mem, sneakEnabledTmp);
    }

    // v20+: player class (per-run)
    if constexpr (SAVE_VERSION >= 20u) {
        uint8_t pc = static_cast<uint8_t>(playerClass_);
        writePod(mem, pc);
    }

    // v21+: Yendor Doom state (per-run)
    if constexpr (SAVE_VERSION >= 21u) {
        const uint8_t doomActiveTmp = yendorDoomActive_ ? 1u : 0u;
        const int32_t doomLevelTmp = static_cast<int32_t>(yendorDoomLevel_);
        const uint32_t doomStartTurnTmp = yendorDoomStartTurn_;
        const uint32_t doomLastPulseTmp = yendorDoomLastPulseTurn_;
        const uint32_t doomLastSpawnTmp = yendorDoomLastSpawnTurn_;
        const int32_t doomMsgStageTmp = static_cast<int32_t>(yendorDoomMsgStage_);
        writePod(mem, doomActiveTmp);
        writePod(mem, doomLevelTmp);
        writePod(mem, doomStartTurnTmp);
        writePod(mem, doomLastPulseTmp);
        writePod(mem, doomLastSpawnTmp);
        writePod(mem, doomMsgStageTmp);
    }

    // Player
    writeEntity(mem, player());

    // Inventory
    uint32_t invCount = static_cast<uint32_t>(inv.size());
    writePod(mem, invCount);
    for (const auto& it : inv) {
        writeItem(mem, it);
    }

    // v31+: Shop debt ledger (consumed/destroyed unpaid goods still billed per shop depth).
    if constexpr (SAVE_VERSION >= 31u) {
        uint32_t billCount = 0u;
        for (int d = 1; d <= DUNGEON_MAX_DEPTH; ++d) {
            if (shopDebtLedger_[d] > 0) ++billCount;
        }
        writePod(mem, billCount);
        for (int d = 1; d <= DUNGEON_MAX_DEPTH; ++d) {
            const int amt = shopDebtLedger_[d];
            if (amt <= 0) continue;
            writePod(mem, static_cast<int32_t>(d));
            writePod(mem, static_cast<int32_t>(amt));
        }
    }

    // v42+: Merchant guild pursuit state (guards can pursue across floors)
    if constexpr (SAVE_VERSION >= 42u) {
        const uint8_t mgTmp = merchantGuildAlerted_ ? 1u : 0u;
        writePod(mem, mgTmp);
    }

    // v43+: Shrine piety + prayer cooldown
    if constexpr (SAVE_VERSION >= 43u) {
        writePod(mem, static_cast<int32_t>(piety_));
        writePod(mem, prayerCooldownUntilTurn_);
    }

    // v44+: Mana + known spells (WIP)
    if constexpr (SAVE_VERSION >= 44u) {
        writePod(mem, static_cast<int32_t>(mana_));
        writePod(mem, knownSpellsMask_);
    }

    // Messages (for convenience)
    uint32_t msgCount = static_cast<uint32_t>(msgs.size());
    writePod(mem, msgCount);
    for (const auto& m : msgs) {
        uint8_t mk = static_cast<uint8_t>(m.kind);
        uint8_t fp = m.fromPlayer ? 1 : 0;
        uint32_t rep = static_cast<uint32_t>(m.repeat);
        uint32_t turn = m.turn;
        uint32_t msgDepth = static_cast<uint32_t>(m.depth);
        uint8_t msgBranch = static_cast<uint8_t>(m.branch);
        writePod(mem, mk);
        writePod(mem, fp);
        if constexpr (SAVE_VERSION >= 24u) {
            writePod(mem, rep);
            writePod(mem, turn);
            writePod(mem, msgDepth);
            if constexpr (SAVE_VERSION >= 46u) {
                writePod(mem, msgBranch);
            }
        }
        writeString(mem, m.text);
    }

    // Levels
    uint32_t lvlCount = static_cast<uint32_t>(levels.size());
    writePod(mem, lvlCount);
    for (const auto& kv : levels) {
        const LevelId id = kv.first;
        const LevelState& st = kv.second;

        // v45+: persist the branch alongside the depth.
        if constexpr (SAVE_VERSION >= 45u) {
            const uint8_t br = static_cast<uint8_t>(id.branch);
            writePod(mem, br);
        }

        int32_t d32 = id.depth;
        writePod(mem, d32);

        // Dungeon
        int32_t w = st.dung.width;
        int32_t h = st.dung.height;
        writePod(mem, w);
        writePod(mem, h);
        int32_t upx = st.dung.stairsUp.x;
        int32_t upy = st.dung.stairsUp.y;
        int32_t dnx = st.dung.stairsDown.x;
        int32_t dny = st.dung.stairsDown.y;
        writePod(mem, upx);
        writePod(mem, upy);
        writePod(mem, dnx);
        writePod(mem, dny);

        uint32_t roomCount = static_cast<uint32_t>(st.dung.rooms.size());
        writePod(mem, roomCount);
        for (const auto& r : st.dung.rooms) {
            int32_t rx = r.x, ry = r.y, rw = r.w, rh = r.h;
            writePod(mem, rx);
            writePod(mem, ry);
            writePod(mem, rw);
            writePod(mem, rh);
            uint8_t rt = static_cast<uint8_t>(r.type);
            writePod(mem, rt);
        }

        uint32_t tileCount = static_cast<uint32_t>(st.dung.tiles.size());
        writePod(mem, tileCount);
        for (const auto& t : st.dung.tiles) {
            uint8_t tt = static_cast<uint8_t>(t.type);
            uint8_t explored = t.explored ? 1 : 0;
            writePod(mem, tt);
            writePod(mem, explored);
        }

        // Monsters
        uint32_t monCount = static_cast<uint32_t>(st.monsters.size());
        writePod(mem, monCount);
        for (const auto& m : st.monsters) {
            writeEntity(mem, m);
        }

        // Ground items
        uint32_t gCount = static_cast<uint32_t>(st.ground.size());
        writePod(mem, gCount);
        for (const auto& gi : st.ground) {
            int32_t gx = gi.pos.x;
            int32_t gy = gi.pos.y;
            writePod(mem, gx);
            writePod(mem, gy);
            writeItem(mem, gi.item);
        }

        // Traps
        uint32_t tCount = static_cast<uint32_t>(st.traps.size());
        writePod(mem, tCount);
        for (const auto& tr : st.traps) {
            uint8_t tk = static_cast<uint8_t>(tr.kind);
            int32_t tx = tr.pos.x;
            int32_t ty = tr.pos.y;
            uint8_t disc = tr.discovered ? 1 : 0;
            writePod(mem, tk);
            writePod(mem, tx);
            writePod(mem, ty);
            writePod(mem, disc);
        }

        // Map markers / notes (v27+)
        if constexpr (SAVE_VERSION >= 27u) {
            uint32_t mCount = static_cast<uint32_t>(st.markers.size());
            writePod(mem, mCount);
            for (const auto& m : st.markers) {
                int32_t mx = m.pos.x;
                int32_t my = m.pos.y;
                uint8_t mk = static_cast<uint8_t>(m.kind);
                writePod(mem, mx);
                writePod(mem, my);
                writePod(mem, mk);
                writeString(mem, m.label);
            }
        }

        // Floor engravings / graffiti (v34+)
        if constexpr (SAVE_VERSION >= 34u) {
            uint32_t eCount = static_cast<uint32_t>(st.engravings.size());
            writePod(mem, eCount);
            for (const auto& e : st.engravings) {
                int32_t ex = e.pos.x;
                int32_t ey = e.pos.y;
                uint8_t strength = e.strength;
                uint8_t flags = 0u;
                if (e.isWard) flags |= 0x1u;
                if (e.isGraffiti) flags |= 0x2u;
                writePod(mem, ex);
                writePod(mem, ey);
                writePod(mem, strength);
                writePod(mem, flags);
                writeString(mem, e.text);
            }
        }

        // Chest containers (v29+)
        if constexpr (SAVE_VERSION >= 29u) {
            uint32_t cCount = static_cast<uint32_t>(st.chestContainers.size());
            writePod(mem, cCount);
            for (const auto& c : st.chestContainers) {
                writePod(mem, c.chestId);
                uint32_t iCount = static_cast<uint32_t>(c.items.size());
                writePod(mem, iCount);
                for (const auto& it : c.items) {
                    writeItem(mem, it);
                }
            }
        }

        // Confusion gas field (v15+)
        // Stored as a per-tile intensity map.
        if constexpr (SAVE_VERSION >= 15u) {
            const uint32_t expected = tileCount;
            uint32_t gasCount = static_cast<uint32_t>(st.confusionGas.size());
            if (gasCount != expected) gasCount = expected;
            writePod(mem, gasCount);
            for (uint32_t gi = 0; gi < gasCount; ++gi) {
                uint8_t v = 0u;
                if (gi < st.confusionGas.size()) v = st.confusionGas[static_cast<size_t>(gi)];
                writePod(mem, v);
            }
        }

        // Poison gas field (v36+)
        // Stored as a per-tile intensity map.
        if constexpr (SAVE_VERSION >= 36u) {
            const uint32_t expected = tileCount;
            uint32_t gasCount = static_cast<uint32_t>(st.poisonGas.size());
            if (gasCount != expected) gasCount = expected;
            writePod(mem, gasCount);
            for (uint32_t gi = 0; gi < gasCount; ++gi) {
                uint8_t v = 0u;
                if (gi < st.poisonGas.size()) v = st.poisonGas[static_cast<size_t>(gi)];
                writePod(mem, v);
            }
        }

        // Corrosive gas field (v53+)
        // Stored as a per-tile intensity map.
        if constexpr (SAVE_VERSION >= 53u) {
            const uint32_t expected = tileCount;
            uint32_t gasCount = static_cast<uint32_t>(st.corrosiveGas.size());
            if (gasCount != expected) gasCount = expected;
            writePod(mem, gasCount);
            for (uint32_t gi = 0; gi < gasCount; ++gi) {
                uint8_t v = 0u;
                if (gi < st.corrosiveGas.size()) v = st.corrosiveGas[static_cast<size_t>(gi)];
                writePod(mem, v);
            }
        }

        // Fire field (v22+)
        // Stored as a per-tile intensity map.
        if constexpr (SAVE_VERSION >= 22u) {
            const uint32_t expected = tileCount;
            uint32_t fireCount = static_cast<uint32_t>(st.fireField.size());
            if (fireCount != expected) fireCount = expected;
            writePod(mem, fireCount);
            for (uint32_t fi = 0; fi < fireCount; ++fi) {
                uint8_t v = 0u;
                if (fi < st.fireField.size()) v = st.fireField[static_cast<size_t>(fi)];
                writePod(mem, v);
            }
        }

        // Scent field (v25+)
        // Stored as a per-tile intensity map.
        if constexpr (SAVE_VERSION >= 25u) {
            const uint32_t expected = tileCount;
            uint32_t scentCount = static_cast<uint32_t>(st.scentField.size());
            if (scentCount != expected) scentCount = expected;
            writePod(mem, scentCount);
            for (uint32_t si = 0; si < scentCount; ++si) {
                uint8_t v = 0u;
                if (si < st.scentField.size()) v = st.scentField[static_cast<size_t>(si)];
                writePod(mem, v);
            }
        }

    }


// v33+: creatures that fell through trap doors to deeper levels but haven't been placed yet.
if constexpr (SAVE_VERSION >= 33u) {
    // v47+: key by (branch, depth) so multiple dungeon branches can safely coexist.
    uint32_t entryCount = 0;
    for (const auto& kv : trapdoorFallers_) {
        if (!kv.second.empty()) ++entryCount;
    }
    writePod(mem, entryCount);

    for (const auto& kv : trapdoorFallers_) {
        const LevelId& id = kv.first;
        const auto& vec = kv.second;
        if (vec.empty()) continue;

        const uint8_t b = static_cast<uint8_t>(id.branch);
        const int32_t d32 = static_cast<int32_t>(id.depth);
        writePod(mem, b);
        writePod(mem, d32);

        uint32_t c = static_cast<uint32_t>(vec.size());
        writePod(mem, c);
        for (const auto& e : vec) {
            writeEntity(mem, e);
        }
    }
}


    // v51+: endless / infinite world options (persisted in the save so reload matches the run).
    if constexpr (SAVE_VERSION >= 51u) {
        uint8_t endlessEnabledTmp = infiniteWorldEnabled_ ? 1u : 0u;
        int32_t endlessKeepWindowTmp = static_cast<int32_t>(infiniteKeepWindow_);
        writePod(mem, endlessEnabledTmp);
        writePod(mem, endlessKeepWindowTmp);
    }

    std::string payload = mem.str();

    // v13+: integrity footer (CRC32 over the entire payload)
    if constexpr (SAVE_VERSION >= 13u) {
        const uint32_t c = crc32(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        appendU32LE(payload, c);
    }

    // Write to a temporary file first, then replace the target.
    std::filesystem::path tmp = p.string() + ".tmp";
    std::ofstream out(tmp, std::ios::binary);
    if (!out) {
        if (!quiet) pushMsg("FAILED TO SAVE (CANNOT OPEN FILE).");
        return false;
    }

    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    out.flush();
    if (!out.good()) {
        if (!quiet) pushMsg("FAILED TO SAVE (WRITE ERROR).");
        out.close();
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    out.close();

    // Rotate backups of the previous file (best-effort).
    rotateFileBackups(p, saveBackups_);

    // Replace the target.
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    if (ec) {
        // On Windows, rename fails if destination exists; remove then retry.
        std::error_code ec2;
        std::filesystem::remove(p, ec2);
        ec.clear();
        std::filesystem::rename(tmp, p, ec);
    }
    if (ec) {
        // Final fallback: copy then remove tmp.
        std::error_code ec2;
        std::filesystem::copy_file(tmp, p, std::filesystem::copy_options::overwrite_existing, ec2);
        std::filesystem::remove(tmp, ec2);
        if (ec2) {
            if (!quiet) pushMsg("FAILED TO SAVE (CANNOT REPLACE FILE).");
            return false;
        }
    }

    if (!quiet) pushMsg("GAME SAVED.", MessageKind::Success, false);
    return true;
}

bool Game::loadFromFile(const std::string& path, bool reportErrors) {
    // Read whole file so we can verify integrity (v13+) and also attempt
    // to recover from a historical v9-v12 layout bug (missing lighting byte).
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (reportErrors) pushMsg("NO SAVE FILE FOUND.");
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streamsize sz = f.tellg();
    if (sz <= 0) {
        if (reportErrors) pushMsg("SAVE FILE IS CORRUPTED OR TRUNCATED.");
        return false;
    }
    f.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), sz)) {
        if (reportErrors) pushMsg("SAVE FILE IS CORRUPTED OR TRUNCATED.");
        return false;
    }

    if (bytes.size() < 8u) {
        if (reportErrors) pushMsg("SAVE FILE IS CORRUPTED OR TRUNCATED.");
        return false;
    }

    const uint32_t magic = readU32LE(bytes.data());
    const uint32_t version = readU32LE(bytes.data() + 4);

    if (magic != SAVE_MAGIC || version == 0u || version > SAVE_VERSION) {
        if (reportErrors) pushMsg("SAVE FILE IS INVALID OR FROM ANOTHER VERSION.");
        return false;
    }

    // v13+: verify CRC32 footer (last 4 bytes).
    std::string payload;
    payload.assign(reinterpret_cast<const char*>(bytes.data()),
                   reinterpret_cast<const char*>(bytes.data() + bytes.size()));

    if (version >= 13u) {
        if (bytes.size() < 12u) {
            if (reportErrors) pushMsg("SAVE FILE IS CORRUPTED OR TRUNCATED.");
            return false;
        }

        const uint32_t storedCrc = readU32LE(bytes.data() + bytes.size() - 4u);
        const uint32_t computedCrc = crc32(bytes.data(), bytes.size() - 4u);

        if (storedCrc != computedCrc) {
            if (reportErrors) pushMsg("SAVE FILE FAILED INTEGRITY CHECK (CRC MISMATCH).");
            return false;
        }

        // Exclude CRC footer from the parser.
        payload.assign(reinterpret_cast<const char*>(bytes.data()),
                       reinterpret_cast<const char*>(bytes.data() + (bytes.size() - 4u)));
    }

    auto tryParse = [&](bool assumeLightingByte, bool reportErrors) -> bool {
        std::istringstream in(payload, std::ios::binary | std::ios::in);

        uint32_t magic2 = 0;
        uint32_t ver2 = 0;
        if (!readPod(in, magic2) || !readPod(in, ver2) || magic2 != SAVE_MAGIC || ver2 == 0u || ver2 > SAVE_VERSION) {
            if (reportErrors) pushMsg("SAVE FILE IS INVALID OR FROM ANOTHER VERSION.");
            return false;
        }

        const uint32_t ver = ver2;

        auto fail = [&]() -> bool {
            if (reportErrors) pushMsg("SAVE FILE IS CORRUPTED OR TRUNCATED.");
            return false;
        };

        uint32_t rngState = 0;
        uint8_t branchU8 = static_cast<uint8_t>(DungeonBranch::Main);
        int32_t depth = 1;
        int32_t pId = 0;
        int32_t nextE = 1;
        int32_t nextI = 1;
        int32_t eqM = 0;
        int32_t eqR = 0;
        int32_t eqA = 0;
        int32_t eq1 = 0;
        int32_t eq2 = 0;
        int32_t clvl = 1;
        int32_t xpNow = 0;
        int32_t xpNeed = 20;
        uint8_t over = 0;
        uint8_t won = 0;
        uint8_t autoPick = 1; // v2+: default enabled (gold). v4+: mode enum (0/1/2)
        uint32_t turnsNow = 0;
        int32_t natRegen = 0;
        uint8_t hasteP = 0;
        uint32_t seedNow = 0;
        uint32_t killsNow = 0;
        int32_t maxD = 1;

        // v52+: conduct counters (NetHack-style voluntary challenges)
        uint32_t directKillsNow = 0;
        uint32_t foodNow = 0;
        uint32_t corpseNow = 0;
        uint32_t scrollNow = 0;
        uint32_t bookNow = 0;
        uint32_t prayNow = 0;

        // v26+: monster codex knowledge (per-run).
        std::array<uint8_t, ENTITY_KIND_COUNT> codexSeenTmp{};
        std::array<uint16_t, ENTITY_KIND_COUNT> codexKillsTmp{};
        codexSeenTmp.fill(0);
        codexKillsTmp.fill(0);

        if (!readPod(in, rngState)) return fail();
        if (ver >= 45u) {
            if (!readPod(in, branchU8)) return fail();
            // Clamp unknown branches to Main for forward/backward compatibility.
            if (branchU8 > static_cast<uint8_t>(DungeonBranch::Main)) {
                branchU8 = static_cast<uint8_t>(DungeonBranch::Main);
            }
        }
        if (!readPod(in, depth)) return fail();
        if (ver < 45u) {
            branchU8 = (depth == 0) ? static_cast<uint8_t>(DungeonBranch::Camp)
                                   : static_cast<uint8_t>(DungeonBranch::Main);
        }
        const DungeonBranch branchTmp = static_cast<DungeonBranch>(branchU8);
        if (!readPod(in, pId)) return fail();
        if (!readPod(in, nextE)) return fail();
        if (!readPod(in, nextI)) return fail();
        if (!readPod(in, eqM)) return fail();
        if (!readPod(in, eqR)) return fail();
        if (!readPod(in, eqA)) return fail();
        if (ver >= 19u) {
            if (!readPod(in, eq1)) return fail();
            if (!readPod(in, eq2)) return fail();
        }
        if (!readPod(in, clvl)) return fail();
        if (!readPod(in, xpNow)) return fail();
        if (!readPod(in, xpNeed)) return fail();

        // v16+: talent allocations
        int32_t tMight = 0, tAgi = 0, tVig = 0, tFoc = 0, tPending = 0, tSel = 0;
        if (ver >= 16u) {
            if (!readPod(in, tMight)) return fail();
            if (!readPod(in, tAgi)) return fail();
            if (!readPod(in, tVig)) return fail();
            if (!readPod(in, tFoc)) return fail();
            if (!readPod(in, tPending)) return fail();
            if (!readPod(in, tSel)) return fail();
        }

        if (!readPod(in, over)) return fail();
        if (!readPod(in, won)) return fail();

        if (ver >= 2u) {
            if (!readPod(in, autoPick)) return fail();
        }

        if (ver >= 3u) {
            if (!readPod(in, turnsNow)) return fail();
            if (!readPod(in, natRegen)) return fail();
            if (!readPod(in, hasteP)) return fail();
        }

        if (ver >= 5u) {
            if (!readPod(in, seedNow)) return fail();
            if (!readPod(in, killsNow)) return fail();
            if (!readPod(in, maxD)) return fail();
        }

        if (ver >= 52u) {
            if (!readPod(in, directKillsNow)) return fail();
            if (!readPod(in, foodNow)) return fail();
            if (!readPod(in, corpseNow)) return fail();
            if (!readPod(in, scrollNow)) return fail();
            if (!readPod(in, bookNow)) return fail();
            if (!readPod(in, prayNow)) return fail();
        }

        if (ver >= 26u) {
            uint32_t mkCount = 0;
            if (!readPod(in, mkCount)) return fail();
            for (uint32_t i = 0; i < mkCount; ++i) {
                uint8_t seen = 0;
                uint16_t kills = 0;
                if (!readPod(in, seen)) return fail();
                if (!readPod(in, kills)) return fail();
                if (i < codexSeenTmp.size()) codexSeenTmp[i] = seen;
                if (i < codexKillsTmp.size()) codexKillsTmp[i] = kills;
            }
        }

        // v6+: item identification tables
        std::array<uint8_t, ITEM_KIND_COUNT> identKnownTmp{};
        std::array<uint8_t, ITEM_KIND_COUNT> identAppTmp{};
        std::array<std::string, ITEM_KIND_COUNT> identCallTmp{};
        identCallTmp.fill(std::string());
        identKnownTmp.fill(1); // older saves had fully-known item names
        identAppTmp.fill(0);

        if (ver >= 6u) {
            uint32_t kindCount = 0;
            if (!readPod(in, kindCount)) return fail();
            for (uint32_t i = 0; i < kindCount; ++i) {
                uint8_t known = 1;
                uint8_t app = 0;
                if (!readPod(in, known)) return fail();
                if (!readPod(in, app)) return fail();
                if (i < static_cast<uint32_t>(ITEM_KIND_COUNT)) {
                    identKnownTmp[static_cast<size_t>(i)] = known;
                    identAppTmp[static_cast<size_t>(i)] = app;
                }
            }

            // v48+: per-run "call" labels for unidentified appearances (NetHack-style notes).
            if (ver >= 48u) {
                for (uint32_t i = 0; i < kindCount; ++i) {
                    std::string note;
                    if (!readString(in, note)) return fail();
                    if (i < static_cast<uint32_t>(ITEM_KIND_COUNT)) {
                        identCallTmp[static_cast<size_t>(i)] = std::move(note);
                    }
                }
            }

            // If this save was made with an older build (fewer ItemKind values),
            // initialize any newly-added identifiable kinds so item-ID stays consistent.
            if (identifyItemsEnabled && kindCount < static_cast<uint32_t>(ITEM_KIND_COUNT)) {
                constexpr size_t POTION_APP_COUNT = sizeof(POTION_APPEARANCES) / sizeof(POTION_APPEARANCES[0]);
                constexpr size_t SCROLL_APP_COUNT = sizeof(SCROLL_APPEARANCES) / sizeof(SCROLL_APPEARANCES[0]);
                constexpr size_t RING_APP_COUNT = sizeof(RING_APPEARANCES) / sizeof(RING_APPEARANCES[0]);
                constexpr size_t WAND_APP_COUNT = sizeof(WAND_APPEARANCES) / sizeof(WAND_APPEARANCES[0]);
                std::vector<bool> usedPotionApps(POTION_APP_COUNT, false);
                std::vector<bool> usedScrollApps(SCROLL_APP_COUNT, false);
                std::vector<bool> usedRingApps(RING_APP_COUNT, false);
                std::vector<bool> usedWandApps(WAND_APP_COUNT, false);

                auto markUsed = [&](ItemKind k, std::vector<bool>& used, size_t maxApps) {
                    const uint32_t idx = static_cast<uint32_t>(k);
                    if (idx >= kindCount || idx >= static_cast<uint32_t>(ITEM_KIND_COUNT)) return;
                    const uint8_t a = identAppTmp[static_cast<size_t>(idx)];
                    if (static_cast<size_t>(a) < maxApps) used[static_cast<size_t>(a)] = true;
                };

                for (ItemKind k : POTION_KINDS) markUsed(k, usedPotionApps, usedPotionApps.size());
                for (ItemKind k : SCROLL_KINDS) markUsed(k, usedScrollApps, usedScrollApps.size());
                for (ItemKind k : RING_KINDS) markUsed(k, usedRingApps, usedRingApps.size());
                for (ItemKind k : WAND_KINDS) markUsed(k, usedWandApps, usedWandApps.size());

                auto takeUnused = [&](std::vector<bool>& used) -> uint8_t {
                    for (size_t j = 0; j < used.size(); ++j) {
                        if (!used[j]) {
                            used[j] = true;
                            return static_cast<uint8_t>(j);
                        }
                    }
                    return 0u;
                };

                for (uint32_t i = kindCount; i < static_cast<uint32_t>(ITEM_KIND_COUNT); ++i) {
                    ItemKind k = static_cast<ItemKind>(i);
                    if (!isIdentifiableKind(k)) continue;

                    // Unknown by default in this run (but keep the save file aligned).
                    identKnownTmp[static_cast<size_t>(i)] = 0u;

                    if (isPotionKind(k)) identAppTmp[static_cast<size_t>(i)] = takeUnused(usedPotionApps);
                    else if (isScrollKind(k)) identAppTmp[static_cast<size_t>(i)] = takeUnused(usedScrollApps);
                    else if (isRingKind(k)) identAppTmp[static_cast<size_t>(i)] = takeUnused(usedRingApps);
                    else if (isWandKind(k)) identAppTmp[static_cast<size_t>(i)] = takeUnused(usedWandApps);
                }
            }
        }

        // v7+: hunger system state (per-run)
        uint8_t hungerEnabledTmp = hungerEnabled_ ? 1u : 0u;
        int32_t hungerTmp = 800;
        int32_t hungerMaxTmp = 800;
        if (ver >= 7u) {
            if (!readPod(in, hungerEnabledTmp)) return fail();
            if (!readPod(in, hungerTmp)) return fail();
            if (!readPod(in, hungerMaxTmp)) return fail();
        }

        // v9+: lighting system state (per-run)
        uint8_t lightingEnabledTmp = lightingEnabled_ ? 1u : 0u;
        if (ver >= 9u) {
            if (assumeLightingByte) {
                if (!readPod(in, lightingEnabledTmp)) return fail();
            } else {
                // Legacy bug: some v9-v12 builds forgot to write this byte.
                // Keep the current setting (from settings.ini) in that case.
                lightingEnabledTmp = lightingEnabled_ ? 1u : 0u;
            }
        }

        // v18+: sneak mode (per-run)
        uint8_t sneakEnabledTmp = 0u;
        if (ver >= 18u) {
            if (!readPod(in, sneakEnabledTmp)) return fail();
        }

        // v20+: player class (per-run)
        uint8_t playerClassTmp = static_cast<uint8_t>(PlayerClass::Adventurer);
        if (ver >= 20u) {
            if (!readPod(in, playerClassTmp)) return fail();
        }

        // v21+: Yendor Doom state (per-run)
        uint8_t doomActiveTmp = 0u;
        int32_t doomLevelTmp = 0;
        uint32_t doomStartTurnTmp = 0u;
        uint32_t doomLastPulseTmp = 0u;
        uint32_t doomLastSpawnTmp = 0u;
        int32_t doomMsgStageTmp = 0;
        if (ver >= 21u) {
            if (!readPod(in, doomActiveTmp)) return fail();
            if (!readPod(in, doomLevelTmp)) return fail();
            if (!readPod(in, doomStartTurnTmp)) return fail();
            if (!readPod(in, doomLastPulseTmp)) return fail();
            if (!readPod(in, doomLastSpawnTmp)) return fail();
            if (!readPod(in, doomMsgStageTmp)) return fail();
        }

        Entity p;
        if (!readEntity(in, p, ver)) return fail();

        // Sanity checks to catch stream misalignment (e.g., legacy missing lighting byte).
        if (p.kind != EntityKind::Player || p.id != pId || p.id == 0) {
            return fail();
        }

        uint32_t invCount = 0;
        if (!readPod(in, invCount)) return fail();
        std::vector<Item> invTmp;
        invTmp.reserve(invCount);
        for (uint32_t i = 0; i < invCount; ++i) {
            Item it;
            if (!readItem(in, it, ver)) return fail();
            invTmp.push_back(it);
        }

        // v31+: Shop debt ledger (consumed/destroyed unpaid goods billed per shop depth).
        std::array<int, DUNGEON_MAX_DEPTH + 1> shopDebtLedgerTmp{};
        shopDebtLedgerTmp.fill(0);
        if (ver >= 31u) {
            uint32_t billCount = 0u;
            if (!readPod(in, billCount)) return fail();
            // Be resilient to future expansions.
            if (billCount > 1024u) return fail();
            for (uint32_t i = 0; i < billCount; ++i) {
                int32_t sd = 0;
                int32_t amt = 0;
                if (!readPod(in, sd)) return fail();
                if (!readPod(in, amt)) return fail();
                if (sd >= 1 && sd <= DUNGEON_MAX_DEPTH && amt > 0) {
                    shopDebtLedgerTmp[static_cast<size_t>(sd)] += amt;
                }
            }
        }

        // v42+: Merchant guild pursuit state (guards can pursue across floors)
        bool merchantGuildAlertedTmp = false;
        if (ver >= 42u) {
            uint8_t mgTmp = 0u;
            if (!readPod(in, mgTmp)) return fail();
            merchantGuildAlertedTmp = (mgTmp != 0u);
        }

        // v43+: Shrine piety + prayer cooldown
        int32_t pietyTmp = 0;
        uint32_t prayerCooldownUntilTurnTmp = 0u;
        if (ver >= 43u) {
            if (!readPod(in, pietyTmp)) return fail();
            if (!readPod(in, prayerCooldownUntilTurnTmp)) return fail();
            if (pietyTmp < 0) pietyTmp = 0;
            if (pietyTmp > 999) pietyTmp = 999;
        }

        // v44+: Mana + known spells (WIP)
        int32_t manaTmp = 0;
        uint32_t knownSpellsMaskTmp = 0u;
        if (ver >= 44u) {
            if (!readPod(in, manaTmp)) return fail();
            if (!readPod(in, knownSpellsMaskTmp)) return fail();
        }
        (void)manaTmp;
        (void)knownSpellsMaskTmp;


        uint32_t msgCount = 0;
        if (!readPod(in, msgCount)) return fail();
        std::vector<Message> msgsTmp;
        msgsTmp.reserve(msgCount);
        for (uint32_t i = 0; i < msgCount; ++i) {
            if (ver >= 2u) {
                uint8_t mk = 0;
                uint8_t fp = 1;
                uint32_t rep = 1;
                uint32_t turn = 0;
                uint32_t msgDepth = 0;
                std::string s;
                uint8_t msgBranchU8 = static_cast<uint8_t>(DungeonBranch::Main);

                if (!readPod(in, mk)) return fail();
                if (!readPod(in, fp)) return fail();
                if (ver >= 24u) {
                    if (!readPod(in, rep)) return fail();
                    if (!readPod(in, turn)) return fail();
                    if (!readPod(in, msgDepth)) return fail();
                    if (ver >= 46u) {
                        if (!readPod(in, msgBranchU8)) return fail();
                        if (msgBranchU8 > static_cast<uint8_t>(DungeonBranch::Main)) {
                            msgBranchU8 = static_cast<uint8_t>(DungeonBranch::Main);
                        }
                    } else {
                        msgBranchU8 = (msgDepth == 0u)
                                        ? static_cast<uint8_t>(DungeonBranch::Camp)
                                        : static_cast<uint8_t>(DungeonBranch::Main);
                    }
                }
                if (!readString(in, s)) return fail();

                Message m;
                m.text = std::move(s);
                m.kind = static_cast<MessageKind>(mk);
                m.fromPlayer = fp != 0;
                m.repeat = static_cast<int>(rep);
                m.turn = turn;
                m.depth = static_cast<int>(msgDepth);
                m.branch = static_cast<DungeonBranch>(msgBranchU8);
                msgsTmp.push_back(std::move(m));
            } else {
                std::string s;
                if (!readString(in, s)) return fail();
                Message m;
                m.text = std::move(s);
                m.kind = MessageKind::Info;
                m.fromPlayer = true;
                msgsTmp.push_back(std::move(m));
            }
        }

        uint32_t lvlCount = 0;
        if (!readPod(in, lvlCount)) return fail();
        std::map<LevelId, LevelState> levelsTmp;

        for (uint32_t li = 0; li < lvlCount; ++li) {
            uint8_t lvlBranchU8 = static_cast<uint8_t>(DungeonBranch::Main);
            if (ver >= 45u) {
                if (!readPod(in, lvlBranchU8)) return fail();
                if (lvlBranchU8 > static_cast<uint8_t>(DungeonBranch::Main)) {
                    lvlBranchU8 = static_cast<uint8_t>(DungeonBranch::Main);
                }
            }

            int32_t d32 = 0;
            if (!readPod(in, d32)) return fail();

            if (ver < 45u) {
                lvlBranchU8 = (d32 == 0) ? static_cast<uint8_t>(DungeonBranch::Camp)
                                        : static_cast<uint8_t>(DungeonBranch::Main);
            }
            const DungeonBranch lvlBranch = static_cast<DungeonBranch>(lvlBranchU8);
            const LevelId lvlId{lvlBranch, static_cast<int>(d32)};

            int32_t w = 0, h = 0;
            int32_t upx = 0, upy = 0, dnx = 0, dny = 0;
            if (!readPod(in, w)) return fail();
            if (!readPod(in, h)) return fail();
            if (!readPod(in, upx)) return fail();
            if (!readPod(in, upy)) return fail();
            if (!readPod(in, dnx)) return fail();
            if (!readPod(in, dny)) return fail();

            LevelState st;
            st.branch = lvlBranch;
            st.depth = d32;
            st.dung = Dungeon(w, h);
            st.dung.stairsUp = { upx, upy };
            st.dung.stairsDown = { dnx, dny };

            uint32_t roomCount = 0;
            if (!readPod(in, roomCount)) return fail();
            st.dung.rooms.clear();
            st.dung.rooms.reserve(roomCount);
            for (uint32_t ri = 0; ri < roomCount; ++ri) {
                int32_t rx = 0, ry = 0, rw = 0, rh = 0;
                uint8_t rt = 0;
                if (!readPod(in, rx)) return fail();
                if (!readPod(in, ry)) return fail();
                if (!readPod(in, rw)) return fail();
                if (!readPod(in, rh)) return fail();
                if (!readPod(in, rt)) return fail();
                Room r;
                r.x = rx;
                r.y = ry;
                r.w = rw;
                r.h = rh;
                r.type = static_cast<RoomType>(rt);
                st.dung.rooms.push_back(r);
            }

            uint32_t tileCount = 0;
            if (!readPod(in, tileCount)) return fail();
            st.dung.tiles.assign(tileCount, Tile{});
            for (uint32_t ti = 0; ti < tileCount; ++ti) {
                uint8_t tt = 0;
                uint8_t explored = 0;
                if (!readPod(in, tt)) return fail();
                if (!readPod(in, explored)) return fail();
                st.dung.tiles[ti].type = static_cast<TileType>(tt);
                st.dung.tiles[ti].visible = false;
                st.dung.tiles[ti].explored = explored != 0;
            }

            uint32_t monCount = 0;
            if (!readPod(in, monCount)) return fail();
            st.monsters.clear();
            st.monsters.reserve(monCount);
            for (uint32_t mi = 0; mi < monCount; ++mi) {
                Entity m;
                if (!readEntity(in, m, ver)) return fail();
                st.monsters.push_back(m);
            }

            uint32_t gCount = 0;
            if (!readPod(in, gCount)) return fail();
            st.ground.clear();
            st.ground.reserve(gCount);
            for (uint32_t gi = 0; gi < gCount; ++gi) {
                int32_t gx = 0, gy = 0;
                if (!readPod(in, gx)) return fail();
                if (!readPod(in, gy)) return fail();
                GroundItem gr;
                gr.pos = { gx, gy };
                if (!readItem(in, gr.item, ver)) return fail();
                st.ground.push_back(gr);
            }

            // Traps (v2+)
            st.traps.clear();
            if (ver >= 2u) {
                uint32_t tCount = 0;
                if (!readPod(in, tCount)) return fail();
                st.traps.reserve(tCount);
                for (uint32_t ti = 0; ti < tCount; ++ti) {
                    uint8_t tk = 0;
                    int32_t tx = 0, ty = 0;
                    uint8_t disc = 0;
                    if (!readPod(in, tk)) return fail();
                    if (!readPod(in, tx)) return fail();
                    if (!readPod(in, ty)) return fail();
                    if (!readPod(in, disc)) return fail();
                    Trap tr;
                    tr.kind = static_cast<TrapKind>(tk);
                    tr.pos = { tx, ty };
                    tr.discovered = disc != 0;
                    st.traps.push_back(tr);
                }
            }

            // Map markers / notes (v27+)
            st.markers.clear();
            if (ver >= 27u) {
                uint32_t mCount = 0;
                if (!readPod(in, mCount)) return fail();
                // Defensive clamp to prevent pathological allocations.
                if (mCount > 5000u) mCount = 5000u;
                st.markers.reserve(mCount);

                for (uint32_t mi = 0; mi < mCount; ++mi) {
                    int32_t mx = 0, my = 0;
                    uint8_t mk = 0;
                    std::string label;
                    if (!readPod(in, mx)) return fail();
                    if (!readPod(in, my)) return fail();
                    if (!readPod(in, mk)) return fail();
                    if (!readString(in, label)) return fail();

                    // Validate basics (skip invalid entries rather than failing the whole load).
                    if (label.empty()) continue;
                    if (!st.dung.inBounds(mx, my)) continue;

                    // Clamp unknown marker kinds to NOTE for forward/backward compatibility.
                    if (mk > static_cast<uint8_t>(MarkerKind::Loot)) mk = 0;

                    MapMarker m;
                    m.pos = { mx, my };
                    m.kind = static_cast<MarkerKind>(mk);
                    // Clamp label to keep UI tidy.
                    if (label.size() > 64) label.resize(64);
                    m.label = std::move(label);

                    // De-dup markers on the same tile (first wins).
                    bool dup = false;
                    for (const auto& ex : st.markers) {
                        if (ex.pos.x == m.pos.x && ex.pos.y == m.pos.y) { dup = true; break; }
                    }
                    if (dup) continue;

                    st.markers.push_back(std::move(m));
                }
            }

            // Floor engravings / graffiti (v34+)
            st.engravings.clear();
            if (ver >= 34u) {
                uint32_t eCount = 0;
                if (!readPod(in, eCount)) return fail();
                if (eCount > 5000u) eCount = 5000u;
                st.engravings.reserve(eCount);

                for (uint32_t ei = 0; ei < eCount; ++ei) {
                    int32_t ex = 0, ey = 0;
                    uint8_t strength = 0;
                    uint8_t flags = 0;
                    std::string text;

                    if (!readPod(in, ex)) return fail();
                    if (!readPod(in, ey)) return fail();
                    if (!readPod(in, strength)) return fail();
                    if (!readPod(in, flags)) return fail();
                    if (!readString(in, text)) return fail();

                    if (text.empty()) continue;
                    if (text.size() > 72) text.resize(72);
                    if (!st.dung.inBounds(ex, ey)) continue;

                    // Avoid duplicate engravings on the same tile (first wins).
                    bool dup = false;
                    for (const auto& exi : st.engravings) {
                        if (exi.pos.x == ex && exi.pos.y == ey) { dup = true; break; }
                    }
                    if (dup) continue;

                    Engraving eg;
                    eg.pos = { ex, ey };
                    eg.strength = strength;
                    eg.isWard = (flags & 0x1u) != 0;
                    eg.isGraffiti = (flags & 0x2u) != 0;
                    eg.text = std::move(text);
                    st.engravings.push_back(std::move(eg));
                }
            }

            // Chest containers (v29+)
            st.chestContainers.clear();
            if (ver >= 29u) {
                uint32_t cCount = 0;
                if (!readPod(in, cCount)) return fail();
                if (cCount > 4096u) cCount = 4096u;
                st.chestContainers.reserve(cCount);

                for (uint32_t ci = 0; ci < cCount; ++ci) {
                    ChestContainer c;
                    if (!readPod(in, c.chestId)) return fail();

                    uint32_t iCount = 0;
                    if (!readPod(in, iCount)) return fail();
                    if (iCount > 8192u) iCount = 8192u;
                    c.items.reserve(iCount);

                    for (uint32_t ii = 0; ii < iCount; ++ii) {
                        Item it;
                        if (!readItem(in, it, ver)) return fail();
                        c.items.push_back(it);
                    }

                    st.chestContainers.push_back(std::move(c));
                }
            }

            // Confusion gas field (v15+)
            st.confusionGas.clear();
            if (ver >= 15u) {
                uint32_t gasCount = 0;
                if (!readPod(in, gasCount)) return fail();

                std::vector<uint8_t> gasTmp;
                gasTmp.assign(gasCount, 0u);
                for (uint32_t gi = 0; gi < gasCount; ++gi) {
                    uint8_t gv = 0;
                    if (!readPod(in, gv)) return fail();
                    gasTmp[gi] = gv;
                }

                // Normalize size to the dungeon tile count when possible (defensive against older/partial saves).
                if (tileCount > 0) {
                    st.confusionGas.assign(tileCount, 0u);
                    const uint32_t copyN = std::min(gasCount, tileCount);
                    for (uint32_t i = 0; i < copyN; ++i) {
                        st.confusionGas[static_cast<size_t>(i)] = gasTmp[static_cast<size_t>(i)];
                    }
                } else {
                    st.confusionGas = std::move(gasTmp);
                }
            }

            // Poison gas field (v36+)
            st.poisonGas.clear();
            if (ver >= 36u) {
                uint32_t gasCount = 0;
                if (!readPod(in, gasCount)) return fail();

                std::vector<uint8_t> gasTmp;
                gasTmp.assign(gasCount, 0u);
                for (uint32_t gi = 0; gi < gasCount; ++gi) {
                    uint8_t gv = 0;
                    if (!readPod(in, gv)) return fail();
                    gasTmp[gi] = gv;
                }

                // Normalize size to the dungeon tile count when possible.
                if (tileCount > 0) {
                    st.poisonGas.assign(tileCount, 0u);
                    const uint32_t copyN = std::min(gasCount, tileCount);
                    for (uint32_t i = 0; i < copyN; ++i) {
                        st.poisonGas[static_cast<size_t>(i)] = gasTmp[static_cast<size_t>(i)];
                    }
                } else {
                    st.poisonGas = std::move(gasTmp);
                }
            }

            // Corrosive gas field (v53+)
            st.corrosiveGas.clear();
            if (ver >= 53u) {
                uint32_t gasCount = 0;
                if (!readPod(in, gasCount)) return fail();
                std::vector<uint8_t> gasTmp;
                gasTmp.assign(gasCount, 0u);
                for (uint32_t gi = 0; gi < gasCount; ++gi) {
                    uint8_t v = 0u;
                    if (!readPod(in, v)) return fail();
                    gasTmp[gi] = v;
                }

                // Normalize size to the dungeon tile count when possible.
                if (tileCount > 0) {
                    st.corrosiveGas.assign(tileCount, 0u);
                    const uint32_t copyN = std::min(gasCount, tileCount);
                    for (uint32_t i = 0; i < copyN; ++i) {
                        st.corrosiveGas[static_cast<size_t>(i)] = gasTmp[static_cast<size_t>(i)];
                    }
                } else {
                    st.corrosiveGas = std::move(gasTmp);
                }
            }

            // Fire field (v22+)
            st.fireField.clear();
            if (ver >= 22u) {
                uint32_t fireCount = 0;
                if (!readPod(in, fireCount)) return fail();

                std::vector<uint8_t> fireTmp;
                fireTmp.assign(fireCount, 0u);
                for (uint32_t fi = 0; fi < fireCount; ++fi) {
                    uint8_t fv = 0;
                    if (!readPod(in, fv)) return fail();
                    fireTmp[fi] = fv;
                }

                if (tileCount > 0) {
                    st.fireField.assign(tileCount, 0u);
                    const uint32_t copyN = std::min(fireCount, tileCount);
                    for (uint32_t i = 0; i < copyN; ++i) {
                        st.fireField[static_cast<size_t>(i)] = fireTmp[static_cast<size_t>(i)];
                    }
                } else {
                    st.fireField = std::move(fireTmp);
                }
            }

            // Scent field (v25+)
            st.scentField.clear();
            if (ver >= 25u) {
                uint32_t scentCount = 0;
                if (!readPod(in, scentCount)) return fail();

                std::vector<uint8_t> scentTmp;
                scentTmp.assign(scentCount, 0u);
                for (uint32_t si = 0; si < scentCount; ++si) {
                    uint8_t sv = 0;
                    if (!readPod(in, sv)) return fail();
                    scentTmp[si] = sv;
                }

                // Normalize size to the dungeon tile count when possible (defensive against older/partial saves).
                if (tileCount > 0) {
                    st.scentField.assign(tileCount, 0u);
                    const uint32_t copyN = std::min(scentCount, tileCount);
                    for (uint32_t i = 0; i < copyN; ++i) {
                        st.scentField[static_cast<size_t>(i)] = scentTmp[static_cast<size_t>(i)];
                    }
                } else {
                    st.scentField = std::move(scentTmp);
                }
            }


            levelsTmp[lvlId] = std::move(st);
        }

// v33+: pending trapdoor fallers (creatures that fell to deeper levels but aren't placed yet).
std::map<LevelId, std::vector<Entity>> trapdoorFallersTmp;

if (ver >= 33u) {
    uint32_t entryCount = 0;
    if (!readPod(in, entryCount)) return fail();
    if (entryCount > 4096u) return fail();

    for (uint32_t di = 0; di < entryCount; ++di) {
        DungeonBranch fallBranch = DungeonBranch::Main;
        int32_t fallDepth = 0;
        uint32_t c = 0;

        // v47+: key by (branch, depth). Older saves stored only depth (implicitly Main branch).
        if (ver >= 47u) {
            uint8_t b = static_cast<uint8_t>(DungeonBranch::Main);
            if (!readPod(in, b)) return fail();
            if (!readPod(in, fallDepth)) return fail();
            if (!readPod(in, c)) return fail();

            if (b > static_cast<uint8_t>(DungeonBranch::Main)) {
                b = static_cast<uint8_t>(DungeonBranch::Main);
            }
            fallBranch = static_cast<DungeonBranch>(b);
        } else {
            if (!readPod(in, fallDepth)) return fail();
            if (!readPod(in, c)) return fail();
            fallBranch = DungeonBranch::Main;
        }

        if (c > 8192u) return fail();

        const bool depthOk = (fallDepth >= 1 && fallDepth <= DUNGEON_MAX_DEPTH)
                          || (fallBranch == DungeonBranch::Camp && fallDepth == 0);
        if (!depthOk) {
            // Skip out-of-range keys for forward/backward compatibility.
            Entity skip;
            for (uint32_t i = 0; i < c; ++i) {
                if (!readEntity(in, skip, ver)) return fail();
            }
            continue;
        }

        const LevelId id{fallBranch, static_cast<int>(fallDepth)};
        auto& vec = trapdoorFallersTmp[id];
        vec.reserve(vec.size() + c);
        for (uint32_t i = 0; i < c; ++i) {
            Entity m;
            if (!readEntity(in, m, ver)) return fail();
            vec.push_back(m);
        }
    }
}

        // v51+: endless / infinite world options
        uint8_t endlessEnabledTmp = infiniteWorldEnabled_ ? 1u : 0u;
        int32_t endlessKeepWindowTmp = static_cast<int32_t>(infiniteKeepWindow_);
        if (ver >= 51u) {
            if (!readPod(in, endlessEnabledTmp)) return fail();
            if (!readPod(in, endlessKeepWindowTmp)) return fail();
        }

        // If we got here, we have a fully parsed save. Commit state.
        rng = RNG(rngState);
        infiniteWorldEnabled_ = (endlessEnabledTmp != 0);
        infiniteKeepWindow_ = clampi(static_cast<int>(endlessKeepWindowTmp), 0, 200);
        branch_ = branchTmp;
        depth_ = depth;
        playerId_ = pId;
        nextEntityId = nextE;
        nextItemId = nextI;
        equipMeleeId = eqM;
        equipRangedId = eqR;
        equipArmorId = eqA;
        equipRing1Id = eq1;
        equipRing2Id = eq2;
        charLevel = clvl;
        xp = xpNow;
        xpNext = xpNeed;
        if (ver >= 16u) {
            talentMight_ = clampi(tMight, -5, 50);
            talentAgility_ = clampi(tAgi, -5, 50);
            talentVigor_ = clampi(tVig, -5, 50);
            talentFocus_ = clampi(tFoc, -5, 50);
            talentPointsPending_ = clampi(tPending, 0, 50);
            levelUpSel = clampi(tSel, 0, 3);
        } else {
            talentMight_ = 0;
            talentAgility_ = 0;
            talentVigor_ = 0;
            talentFocus_ = 0;
            talentPointsPending_ = 0;
            levelUpSel = 0;
        }
        levelUpOpen = (talentPointsPending_ > 0);
        gameOver = over != 0;
        gameWon = won != 0;
        if (ver >= 4u) {
            autoPickup = static_cast<AutoPickupMode>(autoPick);
            // Accept known modes; clamp anything else to Gold.
            if (autoPick > static_cast<uint8_t>(AutoPickupMode::Smart)) autoPickup = AutoPickupMode::Gold;
        } else {
            autoPickup = (autoPick != 0) ? AutoPickupMode::Gold : AutoPickupMode::Off;
        }

        // v3+: pacing state
        turnCount = turnsNow;
        naturalRegenCounter = natRegen;
        hastePhase = (hasteP != 0);

        // v5+: run meta
        seed_ = seedNow;
        killCount = killsNow;
        maxDepth = (maxD > 0) ? maxD : depth_;
        if (ver >= 52u) {
            directKillCount_ = directKillsNow;
            conductFoodEaten_ = foodNow;
            conductCorpseEaten_ = corpseNow;
            conductScrollsRead_ = scrollNow;
            conductSpellbooksRead_ = bookNow;
            conductPrayers_ = prayNow;
        } else {
            directKillCount_ = 0;
            conductFoodEaten_ = 0;
            conductCorpseEaten_ = 0;
            conductScrollsRead_ = 0;
            conductSpellbooksRead_ = 0;
            conductPrayers_ = 0;
        }
        playerClass_ = (ver >= 20u) ? playerClassFromU8(playerClassTmp) : PlayerClass::Adventurer;
        if (maxDepth < depth_) maxDepth = depth_;
        // If we loaded an already-finished run, don't record it again.
        runRecorded = isFinished();

        // v26+: monster codex knowledge (or empty for older saves)
        codexSeen_ = codexSeenTmp;
        codexKills_ = codexKillsTmp;

        lastAutosaveTurn = 0;

        // v6+: identification tables (or default "all known" for older saves)
        identKnown = identKnownTmp;
        identAppearance = identAppTmp;
        identCall = identCallTmp;

        // v7+: hunger state
        if (ver >= 7u) {
            hungerEnabled_ = (hungerEnabledTmp != 0);
            hungerMax = (hungerMaxTmp > 0) ? static_cast<int>(hungerMaxTmp) : 800;
            hunger = clampi(static_cast<int>(hungerTmp), 0, hungerMax);
        } else {
            // Pre-hunger saves: keep the current setting, but start fully fed.
            if (hungerMax <= 0) hungerMax = 800;
            hunger = hungerMax;
        }
        hungerStatePrev = hungerStateFor(hunger, hungerMax);

        // v9+: lighting state
        lightingEnabled_ = (lightingEnabledTmp != 0);

        // v18+: sneak mode
        sneakMode_ = (ver >= 18u) ? (sneakEnabledTmp != 0) : false;

        inv = std::move(invTmp);
        shopDebtLedger_ = shopDebtLedgerTmp;
        merchantGuildAlerted_ = merchantGuildAlertedTmp;
        piety_ = static_cast<int>(pietyTmp);
        prayerCooldownUntilTurn_ = prayerCooldownUntilTurnTmp;

        // v44+: mana and learned spells
        if (ver >= 44u) {
            const int manaMax = playerManaMax();
            mana_ = clampi(static_cast<int>(manaTmp), 0, manaMax);
            const uint32_t validMask = (SPELL_KIND_COUNT >= 32) ? 0xFFFFFFFFu : ((1u << SPELL_KIND_COUNT) - 1u);
            knownSpellsMask_ = knownSpellsMaskTmp & validMask;
        } else {
            mana_ = playerManaMax();
            knownSpellsMask_ = 0u;
        }

        // v21+: Yendor Doom state
        if (ver >= 21u) {
            yendorDoomActive_ = (doomActiveTmp != 0) && yendorDoomEnabled_;
            yendorDoomLevel_ = std::max(0, static_cast<int>(doomLevelTmp));
            yendorDoomStartTurn_ = doomStartTurnTmp;
            yendorDoomLastPulseTurn_ = doomLastPulseTmp;
            yendorDoomLastSpawnTurn_ = doomLastSpawnTmp;
            yendorDoomMsgStage_ = std::max(0, static_cast<int>(doomMsgStageTmp));
        } else {
            // Older saves: the feature didn't exist; start it only if the player already
            // has the Amulet and the setting is enabled.
            yendorDoomActive_ = false;
            yendorDoomLevel_ = 0;
            yendorDoomStartTurn_ = 0u;
            yendorDoomLastPulseTurn_ = 0u;
            yendorDoomLastSpawnTurn_ = 0u;
            yendorDoomMsgStage_ = 0;
        }

        // Gate the system: it only makes sense during an active run with the Amulet.
        const bool canRunDoom = yendorDoomEnabled_ && !gameOver && !gameWon && playerHasAmulet();
        if (!canRunDoom) {
            yendorDoomActive_ = false;
            yendorDoomLevel_ = 0;
        } else if (ver < 21u) {
            // Legacy save that already has the Amulet: start doom "now".
            yendorDoomActive_ = true;
            yendorDoomStartTurn_ = turnCount;
            yendorDoomLastPulseTurn_ = turnCount;
            yendorDoomLastSpawnTurn_ = turnCount;
            yendorDoomMsgStage_ = 0;
            yendorDoomLevel_ = 0;
        }

        // Defensive: clamp timeline to current turn.
        if (yendorDoomStartTurn_ > turnCount) yendorDoomStartTurn_ = turnCount;
        if (yendorDoomLastPulseTurn_ > turnCount) yendorDoomLastPulseTurn_ = turnCount;
        if (yendorDoomLastSpawnTurn_ > turnCount) yendorDoomLastSpawnTurn_ = turnCount;

        msgs = std::move(msgsTmp);
        msgScroll = 0;

        levels = std::move(levelsTmp);
        trapdoorFallers_ = std::move(trapdoorFallersTmp);

        // Rebuild entity list: player + monsters for current depth
        ents.clear();
        ents.push_back(p);

        // Sanity: ensure we have the current level.
        {
            const LevelId cur{branch_, depth_};
            if (levels.find(cur) == levels.end()) {
                // Fallback: if missing, reconstruct from what's available.
                if (!levels.empty()) {
                    const LevelId fb = levels.begin()->first;
                    branch_ = fb.branch;
                    depth_ = fb.depth;
                }
            }
        }

        // Close transient UI and effects.
        invOpen = false;
        invIdentifyMode = false;
        invEnchantRingMode = false;
        invPrompt_ = InvPromptKind::None;
        invCraftMode = false;
        invCraftFirstId_ = 0;
        invCraftPreviewLines_.clear();
        craftRecipeBook_.clear();
        chestOpen = false;
        chestOpenId = 0;
        chestSel = 0;
        chestPaneChest = true;
        chestOpenTier_ = 0;
        chestOpenMaxStacks_ = 0;
        targeting = false;
        helpOpen = false;
        minimapOpen = false;
        minimapCursorActive_ = false;
        minimapCursorPos_ = {0,0};
        statsOpen = false;
        looking = false;
        lookPos = {0,0};
        inputLock = false;
        fx.clear();

        // Auto-move / auto-explore state is treated as transient UI convenience.
        // Reset it to the same default state as newGame() / changeLevel() so a
        // loaded game never resumes "on rails".
        autoMode = AutoMoveMode::None;
        autoPathTiles.clear();
        autoPathIndex = 0;
        autoStepTimer = 0.0f;
        autoExploreGoalIsLoot = false;
        autoExploreGoalPos = Vec2i{-1, -1};
        autoExploreGoalIsSearch = false;
        autoExploreSearchGoalPos = Vec2i{-1, -1};
        autoExploreSearchTurnsLeft = 0;
        autoExploreSearchAnnounced = false;
        autoTravelCautionAnnounced = false;
        // Keep the bookkeeping array initialized for determinism and to avoid
        // out-of-bounds issues in optional secret-hunting logic.
        autoExploreSearchTriedTurns.clear();

        // v40 migration: older saves (v39 and earlier) could generate shop rooms without a shopkeeper.
        // Backfill a peaceful shopkeeper so the buy/sell/#pay loop works mid-run without forcing a new game.
        if (ver < 40u) {
            for (auto& [id, st] : levels) {
                // Find the first shop room on this depth (if any).
                bool hasShopRoom = false;
                Room shopRoom{};
                for (const Room& r : st.dung.rooms) {
                    if (r.type == RoomType::Shop) {
                        shopRoom = r;
                        hasShopRoom = true;
                        break;
                    }
                }
                if (!hasShopRoom) {
                    continue;
                }

                // Don't resurrect shopkeepers: if one exists (alive or dead), leave it alone.
                bool anyShopkeeperEver = false;
                for (const Entity& e : st.monsters) {
                    if (e.kind == EntityKind::Shopkeeper) {
                        anyShopkeeperEver = true;
                        break;
                    }
                }
                if (anyShopkeeperEver) {
                    continue;
                }

                auto occupied = [&](Vec2i pos) {
                    for (const Entity& e : st.monsters) {
                        if (e.hp > 0 && e.pos == pos) {
                            return true;
                        }
                    }
                    return false;
                };

                auto groundAt = [&](Vec2i pos) {
                    for (const GroundItem& g : st.ground) {
                        if (g.pos == pos) {
                            return true;
                        }
                    }
                    return false;
                };

                auto isGoodSpawn = [&](Vec2i pos, bool requireEmptyGround) {
                    if (!st.dung.inBounds(pos.x, pos.y)) {
                        return false;
                    }
                    if (!st.dung.isWalkable(pos.x, pos.y)) {
                        return false;
                    }
                    if (occupied(pos)) {
                        return false;
                    }
                    if (requireEmptyGround && groundAt(pos)) {
                        return false;
                    }
                    return true;
                };

                // Deterministic spawn: prefer the center, then scan for the first usable interior tile.
                Vec2i sp{shopRoom.cx(), shopRoom.cy()};
                bool found = isGoodSpawn(sp, /*requireEmptyGround=*/true);
                if (!found) {
                    for (int y = shopRoom.y + 1; y < shopRoom.y + shopRoom.h - 1 && !found; ++y) {
                        for (int x = shopRoom.x + 1; x < shopRoom.x + shopRoom.w - 1; ++x) {
                            Vec2i cand{x, y};
                            if (isGoodSpawn(cand, /*requireEmptyGround=*/true)) {
                                sp = cand;
                                found = true;
                                break;
                            }
                        }
                    }
                }
                if (!found) {
                    for (int y = shopRoom.y + 1; y < shopRoom.y + shopRoom.h - 1 && !found; ++y) {
                        for (int x = shopRoom.x + 1; x < shopRoom.x + shopRoom.w - 1; ++x) {
                            Vec2i cand{x, y};
                            if (isGoodSpawn(cand, /*requireEmptyGround=*/false)) {
                                sp = cand;
                                found = true;
                                break;
                            }
                        }
                    }
                }

                // Avoid consuming RNG during load: derive a stable sprite seed instead of using rng.nextU32().
                const uint32_t seedA = hash32(static_cast<uint32_t>(id.depth) ^ (static_cast<uint32_t>(id.branch) << 24));
                const uint32_t seedB = hash32(static_cast<uint32_t>(sp.x) ^ (static_cast<uint32_t>(sp.y) << 16));
                const uint32_t spriteSeed = hashCombine(seedA, seedB);

                Entity sk = makeMonster(EntityKind::Shopkeeper, sp, 0, /*allowGear=*/false, spriteSeed);
                sk.alerted = false;
                sk.energy = 0;
                st.monsters.push_back(sk);
            }
        }

        // Overworld chunk state is currently session-only (not serialized).
        overworldX_ = 0;
        overworldY_ = 0;
        overworldChunks_.clear();

        restoreLevel(LevelId{branch_, depth_});

        // Auto-explore bookkeeping is transient per-floor; size it to this dungeon.
        autoExploreSearchTriedTurns.assign(static_cast<size_t>(dung.width * dung.height), 0u);

        recomputeFov();

        // Encumbrance message throttling: avoid spurious "YOU FEEL BURDENED" on the first post-load turn.
        burdenPrev_ = burdenState();

        if (reportErrors) pushMsg("GAME LOADED.");
        return true;
    };

    // Normal parse first. For versions 9-12, some builds accidentally omitted the
    // lighting byte; if so, fall back to the legacy layout.
    const bool canFallback = (version >= 9u && version < 13u);
    if (canFallback) {
        if (tryParse(true, false)) {
            if (reportErrors) pushMsg("GAME LOADED.");
            return true;
        }
        if (tryParse(false, reportErrors)) {
            if (reportErrors) pushMsg("LOADED LEGACY SAVE (FIXED LIGHTING STATE FORMAT).", MessageKind::System);
            return true;
        }
        return false;
    }

    return tryParse(true, reportErrors);
}


bool Game::loadFromFileWithBackups(const std::string& path) {
    namespace fs = std::filesystem;

    // First try the primary file silently, so we can fall back to rotated backups
    // without spamming error messages.
    if (loadFromFile(path, false)) {
        pushMsg("GAME LOADED.", MessageKind::Success, false);
        return true;
    }

    // If it fails, try rotated backups (<file>.bak1..bak10), most-recent first.
    for (int i = 1; i <= 10; ++i) {
        const fs::path bak = fs::path(path).string() + ".bak" + std::to_string(i);
        std::error_code ec;
        if (!fs::exists(bak, ec) || ec) continue;

        if (loadFromFile(bak.string(), false)) {
            std::ostringstream ss;
            ss << "SAVE RECOVERED FROM BACKUP (BAK" << i << ").";
            pushMsg(ss.str(), MessageKind::Warning, false);
            pushMsg("TIP: SAVE NOW TO REWRITE THE PRIMARY FILE.", MessageKind::System, false);
            return true;
        }
    }

    // Nothing worked; re-run the primary load with errors enabled so the player
    // sees a helpful failure reason.
    return loadFromFile(path, true);
}


// ====================================
// Bones files (persistent death remnants)
// ====================================

bool Game::writeBonesFile() {
    if (!bonesEnabled_) return false;
    if (!gameOver || gameWon) return false;
    if (player().hp > 0) return false;
    if (bonesWritten_) return false;
    if (depth_ < 2) return false;

    const std::filesystem::path baseDir = exportBaseDir(*this);
    if (baseDir.empty()) return false;

    // One bones file per (branch, depth). New deaths overwrite old ones.
    //
    // NOTE: Bones payload format is still keyed by numeric depth for now, but the filename
    // includes the branch to prevent collisions once multiple dungeon branches can share
    // the same depth numbers.
    const char* branchTag = (branch_ == DungeonBranch::Camp) ? "camp" : "main";
    const std::filesystem::path path = baseDir / (std::string("procrogue_bones_") + branchTag + "_d" + std::to_string(depth_) + ".dat");

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    writePod(out, BONES_MAGIC);
    writePod(out, BONES_VERSION);

    // Depth + intended placement.
    writePod(out, depth_);
    writePod(out, player().pos.x);
    writePod(out, player().pos.y);

    // Player name (for flavor only).
    std::string nm = playerName_;
    if (nm.size() > 32) nm.resize(32);
    writeString(out, nm);

    // Equipped gear becomes the ghost's gear (if the ghost spawns).
    const Item* melee = equippedMelee();
    const Item* armor = equippedArmor();

    const uint8_t hasMelee = melee ? 1u : 0u;
    const uint8_t hasArmor = armor ? 1u : 0u;
    writePod(out, hasMelee);
    if (hasMelee) {
        Item it = *melee;
        it.id = 0;
        it.shopPrice = 0;
        it.shopDepth = 0;
        writeItem(out, it);
    }
    writePod(out, hasArmor);
    if (hasArmor) {
        Item it = *armor;
        it.id = 0;
        it.shopPrice = 0;
        it.shopDepth = 0;
        writeItem(out, it);
    }

    // Remaining inventory becomes ground loot. (No gold/Yendor.)
    std::vector<Item> loot;
    loot.reserve(inv.size());
    for (const Item& it0 : inv) {
        if (it0.kind == ItemKind::Gold) continue;
        if (it0.kind == ItemKind::AmuletYendor) continue;
        if (melee && it0.id == melee->id) continue;
        if (armor && it0.id == armor->id) continue;

        Item it = it0;
        it.id = 0;
        it.shopPrice = 0;
        it.shopDepth = 0;
        loot.push_back(it);
    }

    const uint32_t lootN = static_cast<uint32_t>(loot.size());
    writePod(out, lootN);
    for (const Item& it : loot) writeItem(out, it);

    out.flush();
    if (!out.good()) return false;

    bonesWritten_ = true;
    return true;
}

bool Game::tryApplyBones() {
    if (!bonesEnabled_) return false;
    if (depth_ < 2) return false;

    const std::filesystem::path baseDir = exportBaseDir(*this);
    if (baseDir.empty()) return false;

    const char* branchTag = (branch_ == DungeonBranch::Camp) ? "camp" : "main";
    std::filesystem::path path = baseDir / (std::string("procrogue_bones_") + branchTag + "_d" + std::to_string(depth_) + ".dat");
    std::filesystem::path pathLegacy = baseDir / (std::string("procrogue_bones_d") + std::to_string(depth_) + ".dat");

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        // Backwards compatibility: older builds used one bones file per depth without a branch tag.
        // Only consider legacy bones on the main dungeon branch.
        if (branch_ != DungeonBranch::Main) return false;
        ec.clear();
        if (!std::filesystem::exists(pathLegacy, ec) || ec) return false;
        path = pathLegacy;
    }

    // Chance to apply, so bones don't appear every single time.
    const int depthBonusI = std::min(10, std::max(0, depth_ - 2));
    const float depthBonus = static_cast<float>(depthBonusI);
    const float chance = std::clamp(0.55f + 0.03f * depthBonus, 0.55f, 0.85f);
    if (!rng.chance(chance)) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    uint32_t magic = 0;
    uint32_t ver = 0;
    if (!readPod(in, magic) || !readPod(in, ver)) return false;
    if (magic != BONES_MAGIC || ver != BONES_VERSION) {
        std::filesystem::remove(path, ec);
        return false;
    }

    int fileDepth = 0;
    int px = 0;
    int py = 0;
    if (!readPod(in, fileDepth)) return false;
    if (fileDepth != depth_) {
        std::filesystem::remove(path, ec);
        return false;
    }
    if (!readPod(in, px) || !readPod(in, py)) return false;

    std::string nm;
    {
        uint32_t len = 0;
        if (!readPod(in, len)) return false;
        // Sanity-check: bones files should never contain huge strings.
        if (len > 1024) {
            std::filesystem::remove(path, ec);
            return false;
        }

        const uint32_t keep = std::min<uint32_t>(len, 32u);
        nm.resize(keep);
        if (keep > 0) in.read(nm.data(), keep);
        if (!in.good()) return false;
        if (len > keep) {
            in.ignore(static_cast<std::streamsize>(len - keep));
            if (!in.good()) return false;
        }
    }

    uint8_t hasMelee = 0;
    uint8_t hasArmor = 0;
    if (!readPod(in, hasMelee)) return false;

    Item melee{};
    Item armor{};
    if (hasMelee) {
        if (!readItem(in, melee, SAVE_VERSION)) return false;
        melee.id = 1;
        melee.shopPrice = 0;
        melee.shopDepth = 0;
        melee.count = 1;
    }

    if (!readPod(in, hasArmor)) return false;
    if (hasArmor) {
        if (!readItem(in, armor, SAVE_VERSION)) return false;
        armor.id = 1;
        armor.shopPrice = 0;
        armor.shopDepth = 0;
        armor.count = 1;
    }

    uint32_t lootN = 0;
    if (!readPod(in, lootN)) return false;
    if (lootN > 512) {
        std::filesystem::remove(path, ec);
        return false;
    }

    std::vector<Item> loot;
    loot.reserve(lootN);
    for (uint32_t i = 0; i < lootN; ++i) {
        Item it{};
        if (!readItem(in, it, SAVE_VERSION)) return false;
        it.id = 0;
        it.shopPrice = 0;
        it.shopDepth = 0;
        loot.push_back(it);
    }

    // Pick a spawn tile near the stored (x,y).
    auto isBadTile = [&](Vec2i p) -> bool {
        if (!dung.inBounds(p.x, p.y)) return true;
        if (!dung.isWalkable(p.x, p.y)) return true;
        const TileType t = dung.at(p.x, p.y).type;
        if (t == TileType::StairsDown || t == TileType::StairsUp) return true;
        if (entityAt(p.x, p.y) != nullptr) return true;
        return false;
    };

    Vec2i spawn{px, py};
    if (isBadTile(spawn)) {
        bool found = false;
        for (int r = 1; r <= 12 && !found; ++r) {
            for (int dy = -r; dy <= r && !found; ++dy) {
                for (int dx = -r; dx <= r && !found; ++dx) {
                    if (std::abs(dx) != r && std::abs(dy) != r) continue;
                    Vec2i p{px + dx, py + dy};
                    if (!isBadTile(p)) { spawn = p; found = true; }
                }
            }
        }

        if (!found) {
            // Fall back to a random floor tile.
            for (int tries = 0; tries < 500; ++tries) {
                Vec2i p = dung.randomFloor(rng, true);
                if (!isBadTile(p)) { spawn = p; found = true; break; }
            }
        }

        if (!found) return false;
    }

    // Spawn the ghost and its loot.
    Entity& g = spawnMonster(EntityKind::Ghost, spawn, /*roomId*/ 0, /*allowGear*/ false);
    g.alerted = true;
    g.lastKnownPlayerPos = player().pos;
    g.lastKnownPlayerAge = 0;
    g.willFlee = false;

    if (hasMelee) g.gearMelee = melee;
    if (hasArmor) g.gearArmor = armor;

    // Scatter loot around the spawn point a bit.
    std::vector<Vec2i> spots;
    spots.reserve(25);
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            Vec2i p{spawn.x + dx, spawn.y + dy};
            if (isBadTile(p)) continue;
            spots.push_back(p);
        }
    }
    if (spots.empty()) spots.push_back(spawn);

    // Shuffle spots using the game's RNG.
    for (int i = static_cast<int>(spots.size()) - 1; i > 0; --i) {
        int j = rng.range(0, i);
        std::swap(spots[i], spots[j]);
    }

    for (size_t i = 0; i < loot.size(); ++i) {
        Item it = loot[i];
        // Extra safety: never duplicate the Amulet.
        if (it.kind == ItemKind::AmuletYendor) continue;
        Vec2i p = spots[i % spots.size()];
        dropGroundItemItem(p, it);
    }

    if (!nm.empty()) {
        pushMsg("YOU FEEL A CHILL. THE BONES OF " + toUpper(nm) + " LIE HERE...", MessageKind::Warning);
    } else {
        pushMsg("YOU FEEL A CHILL. SOMEONE'S BONES LIE HERE...", MessageKind::Warning);
    }

    // Consume the bones file so it can't repeat forever.
    std::filesystem::remove(path, ec);
    return true;
}
