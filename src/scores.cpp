#include "scores.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <numeric>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>

#if __has_include(<filesystem>)
    #include <filesystem>
    namespace fs = std::filesystem;
#endif

namespace {

std::string trimStr(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void stripUtf8Bom(std::string& s) {
    // Some editors (notably Windows tools) may write a UTF-8 BOM at the start of text files.
    // If present, strip it so header/key parsing works as expected.
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

// CSV parsing with support for quoted fields and escaped quotes ("").
// We keep it intentionally simple; it's only used for ProcRogue's small scoreboard file.
bool splitCsvLine(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    cur.reserve(line.size());

    bool inQuotes = false;
    bool fieldQuoted = false;
    bool justClosedQuote = false;

    auto pushField = [&]() {
        if (fieldQuoted) {
            out.push_back(cur);
        } else {
            out.push_back(trimStr(cur));
        }
        cur.clear();
        inQuotes = false;
        fieldQuoted = false;
        justClosedQuote = false;
    };

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];

        if (inQuotes) {
            if (c == '"') {
                // Escaped quote inside a quoted field.
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    cur.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                    justClosedQuote = true;
                }
                continue;
            }
            cur.push_back(c);
            continue;
        }

        // Not in quotes.
        if (c == ',') {
            pushField();
            continue;
        }

        if (c == '"') {
            // Start of a quoted field.
            // Some CSV writers allow whitespace before the opening quote; treat it as quoted and
            // discard that leading whitespace.
            if (!fieldQuoted && trimStr(cur).empty()) {
                cur.clear();
                fieldQuoted = true;
                inQuotes = true;
                justClosedQuote = false;
                continue;
            }

            // Otherwise treat it as a literal quote inside an unquoted field.
            cur.push_back(c);
            continue;
        }

        // Ignore whitespace after a closing quote before the comma/end (CSV semantics).
        if (justClosedQuote) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                continue;
            }
            justClosedQuote = false;
        }

        cur.push_back(c);
    }

    // Final field.
    if (fieldQuoted) out.push_back(cur);
    else out.push_back(trimStr(cur));
    return true;
}

std::string csvEscape(const std::string& field) {
    // Quote if we contain a comma, quote, newline, or leading/trailing whitespace.
    bool needsQuotes = false;

    if (!field.empty()) {
        if (std::isspace(static_cast<unsigned char>(field.front())) ||
            std::isspace(static_cast<unsigned char>(field.back()))) {
            needsQuotes = true;
        }
    }

    for (char c : field) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needsQuotes = true;
            break;
        }
    }

    if (!needsQuotes) return field;

    std::string out;
    out.reserve(field.size() + 2);
    out.push_back('"');
    for (char c : field) {
        if (c == '"') out.push_back('"'); // escape by doubling
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

bool parseU32(const std::string& s, uint32_t& out) {
    // Harden parsing against negative numbers, overflow, and partial parses.
    // Accepts decimal or 0x-prefixed hex (base=0).
    const std::string t = trimStr(s);
    if (t.empty()) return false;
    if (!t.empty() && t[0] == '-') return false;

    try {
        size_t pos = 0;
        unsigned long long v = std::stoull(t, &pos, 0);
        if (pos != t.size()) return false;
        if (v > static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) return false;
        out = static_cast<uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}


bool parseI32(const std::string& s, int& out) {
    try {
        out = std::stoi(trimStr(s));
        return true;
    } catch (...) {
        return false;
    }
}

bool parseBool(const std::string& s, bool& out) {
    std::string v = toLower(trimStr(s));
    if (v == "1" || v == "true" || v == "yes" || v == "win" || v == "won") { out = true; return true; }
    if (v == "0" || v == "false" || v == "no" || v == "loss" || v == "dead") { out = false; return true; }
    return false;
}


bool parseBranchToken(const std::string& s, uint8_t& out) {
    std::string v = toLower(trimStr(s));
    if (v.empty()) return false;

    if (v == "camp" || v == "surface" || v == "hub") { out = 0; return true; }
    if (v == "main" || v == "dungeon" || v == "d") { out = 1; return true; }

    uint32_t u = 0;
    if (parseU32(v, u)) {
        out = static_cast<uint8_t>(std::min<uint32_t>(u, 255u));
        return true;
    }

    return false;
}

const char* branchToken(uint8_t b) {
    switch (b) {
        case 0: return "camp";
        case 1: return "main";
        default: return "unknown";
    }
}


bool atomicWriteTextFile(const std::string& path, const std::string& contents) {
#if __has_include(<filesystem>)
    std::error_code ec;
    const fs::path p(path);
    const fs::path tmp = p.string() + ".tmp";

    // Ensure the parent directory exists (helps for custom/portable data dirs).
    if (!p.parent_path().empty()) {
        std::error_code ecDirs;
        fs::create_directories(p.parent_path(), ecDirs);
    }

    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return false;
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out.good()) return false;
    }

    // Try rename; on Windows this fails if destination exists.
    fs::rename(tmp, p, ec);
    if (ec) {
        std::error_code ec2;
        fs::remove(p, ec2);
        ec.clear();
        fs::rename(tmp, p, ec);
    }
    if (ec) {
        // Fallback: copy then remove tmp
        std::error_code ec2;
        fs::copy_file(tmp, p, fs::copy_options::overwrite_existing, ec2);
        fs::remove(tmp, ec2);
        return !ec2;
    }
    return true;
#else
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return out.good();
#endif
}


bool betterScoreEntry(const ScoreEntry& a, const ScoreEntry& b) {
    // Strict ordering for the "top scores" view.
    if (a.score != b.score) return a.score > b.score;
    if (a.won != b.won) return a.won > b.won;
    if (a.turns != b.turns) return a.turns < b.turns;
    if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp; // newest first
    if (a.depth != b.depth) return a.depth > b.depth;
    if (a.kills != b.kills) return a.kills > b.kills;
    if (a.level != b.level) return a.level > b.level;
    if (a.gold != b.gold) return a.gold > b.gold;
    if (a.seed != b.seed) return a.seed > b.seed;
    if (a.name != b.name) return a.name < b.name;
    return a.cause < b.cause;
}

bool newerEntry(const ScoreEntry& a, const ScoreEntry& b) {
    // Strict ordering for the "recent runs" view.
    if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp; // newest first
    if (a.score != b.score) return a.score > b.score;
    if (a.won != b.won) return a.won > b.won;
    if (a.turns != b.turns) return a.turns < b.turns;
    if (a.name != b.name) return a.name < b.name;
    return a.cause < b.cause;
}


} // namespace

uint32_t computeScore(const ScoreEntry& e) {
    // Keep scoring simple and consistent between versions:
    // - depth is the primary driver
    // - kills, gold, and level contribute meaningfully
    // - wins get a large bonus
    // - turns apply a modest penalty (never below 0)
    uint32_t score = 0;
    score += e.kills * 50u;
    score += static_cast<uint32_t>(std::max(0, e.gold));
    score += static_cast<uint32_t>(std::max(0, e.level)) * 200u;
    score += static_cast<uint32_t>(std::max(0, e.depth)) * 1000u;
    if (e.won) score += 10000u;

    const uint32_t penalty = e.turns / 2u;
    score -= std::min(score, penalty);
    return score;
}

void ScoreBoard::trim(size_t maxEntries) {
    if (entries_.size() <= maxEntries) return;
    if (maxEntries == 0) {
        entries_.clear();
        return;
    }

    // ProcRogue uses a single CSV file for both:
    //  1) "Top runs" (score-sorted)
    //  2) "Recent run history" (timestamp-sorted)
    //
    // Trimming purely by score can discard your newest runs (which might be low-scoring).
    // To keep both views useful, keep a mix:
    //   - Top runs by score
    //   - Most recent runs by timestamp
    //
    // The game UI shows up to 60 entries for both #scores and #history, so we keep those.
    constexpr size_t kDefaultKeepTop = 60;
    constexpr size_t kDefaultKeepRecent = 60;

    // Keep a balanced mix under *any* cap:
    //   - top runs by score
    //   - recent runs by timestamp
    //
    // When maxEntries is smaller than our default 60+60, scale the mix down so
    // trimming still preserves recent history (useful for callers that want a
    // smaller "top N" view).
    size_t keepTop = std::min(kDefaultKeepTop, maxEntries);
    size_t keepRecent = std::min(kDefaultKeepRecent, maxEntries);

    if (keepTop + keepRecent > maxEntries) {
        if (maxEntries <= 1) {
            keepTop = maxEntries;
            keepRecent = 0;
        } else {
            keepTop = (maxEntries + 1) / 2; // top gets the extra slot if odd
            keepRecent = maxEntries - keepTop;
        }
    }

    const size_t n = entries_.size();

    std::vector<size_t> byScore(n);
    std::iota(byScore.begin(), byScore.end(), size_t{0});
    std::sort(byScore.begin(), byScore.end(), [&](size_t ia, size_t ib) {
        return betterScoreEntry(entries_[ia], entries_[ib]);
    });

    std::vector<size_t> byTime(n);
    std::iota(byTime.begin(), byTime.end(), size_t{0});
    std::sort(byTime.begin(), byTime.end(), [&](size_t ia, size_t ib) {
        return newerEntry(entries_[ia], entries_[ib]);
    });

    std::vector<uint8_t> keep(n, 0);
    auto markFirst = [&](const std::vector<size_t>& order, size_t count) {
        const size_t c = std::min(count, order.size());
        for (size_t i = 0; i < c; ++i) {
            keep[order[i]] = 1;
        }
    };

    markFirst(byScore, keepTop);
    markFirst(byTime, keepRecent);

    // Fill any remaining capacity with the next-best scores.
    size_t kept = 0;
    for (uint8_t v : keep) kept += (v != 0);
    for (size_t i : byScore) {
        if (kept >= maxEntries) break;
        if (keep[i]) continue;
        keep[i] = 1;
        ++kept;
    }

    // Rebuild in score order so ScoreBoard::entries() stays "top runs" sorted.
    std::vector<ScoreEntry> out;
    out.reserve(maxEntries);
    for (size_t i : byScore) {
        if (!keep[i]) continue;
        out.push_back(entries_[i]);
        if (out.size() >= maxEntries) break;
    }

    entries_.swap(out);
}

bool ScoreBoard::load(const std::string& path) {
    entries_.clear();

    std::ifstream in(path);
    if (!in) {
        // No file yet is not an error.
        return true;
    }

    std::string line;
    std::vector<std::string> cols;

    std::unordered_map<std::string, size_t> idx;
    bool headerReady = false;

    auto setDefaultHeader = [&]() {
        idx.clear();
        idx["timestamp"] = 0;
        idx["won"] = 1;
        idx["score"] = 2;
        idx["depth"] = 3;
        idx["turns"] = 4;
        idx["kills"] = 5;
        idx["level"] = 6;
        idx["gold"] = 7;
        idx["seed"] = 8;
        headerReady = true;
    };

    auto getCol = [&](const std::vector<std::string>& row, const std::string& name) -> std::string {
        auto it = idx.find(name);
        if (it == idx.end()) return {};
        size_t i = it->second;
        if (i >= row.size()) return {};
        return row[i];
    };

    while (std::getline(in, line)) {
        stripUtf8Bom(line);
        line = trimStr(std::move(line));
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        splitCsvLine(line, cols);
        if (cols.empty()) continue;

        if (!headerReady) {
            // Detect header by the first column.
            if (!cols.empty() && toLower(cols[0]) == "timestamp") {
                idx.clear();
                for (size_t i = 0; i < cols.size(); ++i) {
                    std::string name = toLower(trimStr(cols[i]));
                    if (!name.empty() && idx.find(name) == idx.end()) idx[name] = i;
                }
                headerReady = true;
                continue;
            }

            // No header present; assume legacy order and parse this line as data.
            setDefaultHeader();
        }

        ScoreEntry e;

        e.timestamp = getCol(cols, "timestamp");
        e.name = getCol(cols, "name");
        e.playerClass = getCol(cols, "class");
        e.slot = getCol(cols, "slot");
        e.cause = getCol(cols, "cause");

        // Optional: NetHack-style conduct tags (newer versions only).
        e.conducts = getCol(cols, "conducts");
        if (e.conducts.empty()) e.conducts = getCol(cols, "conduct");

        // Support either "game_version" or "version" as a column name.
        e.gameVersion = getCol(cols, "game_version");
        if (e.gameVersion.empty()) e.gameVersion = getCol(cols, "version");

        bool b = false;
        if (parseBool(getCol(cols, "won"), b)) e.won = b;

        uint32_t u = 0;
        if (parseU32(getCol(cols, "score"), u)) e.score = u;

        int i32 = 1;
        if (parseI32(getCol(cols, "depth"), i32)) e.depth = i32;

        uint8_t br = 1;
        if (parseBranchToken(getCol(cols, "branch"), br)) e.branch = br;
        else e.branch = (e.depth <= 0) ? 0 : 1;

        if (parseU32(getCol(cols, "turns"), u)) e.turns = u;
        if (parseU32(getCol(cols, "kills"), u)) e.kills = u;

        if (parseI32(getCol(cols, "level"), i32)) e.level = i32;
        if (parseI32(getCol(cols, "gold"), i32)) e.gold = i32;
        if (parseU32(getCol(cols, "seed"), u)) e.seed = u;

        // Backfill score if file was missing it (or if older tools wrote 0).
        if (e.score == 0) e.score = computeScore(e);

        entries_.push_back(std::move(e));
    }

    // Keep sorted by score desc.
    std::sort(entries_.begin(), entries_.end(), [](const ScoreEntry& a, const ScoreEntry& b) {
        return betterScoreEntry(a, b);
    });

    // Keep both top runs and recent run history.
    trim(120);
    return true;
}

bool ScoreBoard::append(const std::string& path, const ScoreEntry& eIn) {
    ScoreEntry e = eIn;
    if (e.score == 0) e.score = computeScore(e);

    entries_.push_back(e);

    std::sort(entries_.begin(), entries_.end(), [](const ScoreEntry& a, const ScoreEntry& b) {
        return betterScoreEntry(a, b);
    });

    // Keep both top runs and recent run history.
    trim(120);

    std::ostringstream out;

    // Newer, richer schema. Older files are still readable via header mapping.
    out << "timestamp,name,class,slot,won,score,branch,depth,turns,kills,level,gold,seed,conducts,cause,game_version\n";
    for (const auto& s : entries_) {
        out << csvEscape(s.timestamp) << ','
            << csvEscape(s.name) << ','
            << csvEscape(s.playerClass) << ','
            << csvEscape(s.slot) << ','
            << (s.won ? 1 : 0) << ','
            << s.score << ','
            << csvEscape(branchToken(s.branch)) << ','
            << s.depth << ','
            << s.turns << ','
            << s.kills << ','
            << s.level << ','
            << s.gold << ','
            << s.seed << ','
            << csvEscape(s.conducts) << ','
            << csvEscape(s.cause) << ','
            << csvEscape(s.gameVersion)
            << "\n";
    }

    return atomicWriteTextFile(path, out.str());
}
