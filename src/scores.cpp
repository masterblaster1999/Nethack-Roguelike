#include "scores.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
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

// CSV parsing with support for quoted fields and escaped quotes ("").
// We keep it intentionally simple; it's only used for ProcRogue's small scoreboard file.
bool splitCsvLine(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    cur.reserve(line.size());

    bool inQuotes = false;

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
                }
                continue;
            }
            cur.push_back(c);
            continue;
        }

        // Not in quotes.
        if (c == '"') {
            inQuotes = true;
            continue;
        }
        if (c == ',') {
            out.push_back(trimStr(cur));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }

    out.push_back(trimStr(cur));
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
    try {
        unsigned long v = std::stoul(trimStr(s));
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

bool atomicWriteTextFile(const std::string& path, const std::string& contents) {
#if __has_include(<filesystem>)
    std::error_code ec;
    const fs::path p(path);
    const fs::path tmp = p.string() + ".tmp";

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
    entries_.resize(maxEntries);
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
        e.slot = getCol(cols, "slot");
        e.cause = getCol(cols, "cause");

        // Support either "game_version" or "version" as a column name.
        e.gameVersion = getCol(cols, "game_version");
        if (e.gameVersion.empty()) e.gameVersion = getCol(cols, "version");

        bool b = false;
        if (parseBool(getCol(cols, "won"), b)) e.won = b;

        uint32_t u = 0;
        if (parseU32(getCol(cols, "score"), u)) e.score = u;

        int i32 = 1;
        if (parseI32(getCol(cols, "depth"), i32)) e.depth = i32;

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
        if (a.score != b.score) return a.score > b.score;
        if (a.won != b.won) return a.won > b.won;
        return a.turns < b.turns;
    });

    trim(60);
    return true;
}

bool ScoreBoard::append(const std::string& path, const ScoreEntry& eIn) {
    ScoreEntry e = eIn;
    if (e.score == 0) e.score = computeScore(e);

    entries_.push_back(e);

    std::sort(entries_.begin(), entries_.end(), [](const ScoreEntry& a, const ScoreEntry& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.won != b.won) return a.won > b.won;
        return a.turns < b.turns;
    });

    trim(60);

    std::ostringstream out;

    // Newer, richer schema. Older files are still readable via header mapping.
    out << "timestamp,name,slot,won,score,depth,turns,kills,level,gold,seed,cause,game_version\n";
    for (const auto& s : entries_) {
        out << csvEscape(s.timestamp) << ','
            << csvEscape(s.name) << ','
            << csvEscape(s.slot) << ','
            << (s.won ? 1 : 0) << ','
            << s.score << ','
            << s.depth << ','
            << s.turns << ','
            << s.kills << ','
            << s.level << ','
            << s.gold << ','
            << s.seed << ','
            << csvEscape(s.cause) << ','
            << csvEscape(s.gameVersion)
            << "\n";
    }

    return atomicWriteTextFile(path, out.str());
}
