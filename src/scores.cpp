#include "scores.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>

#if __has_include(<filesystem>)
    #include <filesystem>
    namespace fs = std::filesystem;
#endif

static std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

static bool splitCsvLine(const std::string& line, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    cur.reserve(line.size());

    bool inQuotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            inQuotes = !inQuotes;
            continue;
        }
        if (c == ',' && !inQuotes) {
            out.push_back(trim(cur));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    out.push_back(trim(cur));
    return true;
}

static bool parseU32(const std::string& s, uint32_t& out) {
    try {
        unsigned long v = std::stoul(trim(s));
        out = static_cast<uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

static bool parseI32(const std::string& s, int& out) {
    try {
        out = std::stoi(trim(s));
        return true;
    } catch (...) {
        return false;
    }
}

static bool parseBool(const std::string& s, bool& out) {
    std::string v = trim(s);
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "1" || v == "true" || v == "yes" || v == "win" || v == "won") { out = true; return true; }
    if (v == "0" || v == "false" || v == "no" || v == "loss" || v == "dead") { out = false; return true; }
    return false;
}

static bool atomicWriteTextFile(const std::string& path, const std::string& contents) {
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

uint32_t computeScore(const ScoreEntry& e) {
    // Simple, transparent scoring:
    // - big win bonus
    // - depth matters a lot
    // - kills and gold are meaningful but secondary
    // - fewer turns is slightly better
    uint32_t s = 0;
    const uint32_t depthPart = static_cast<uint32_t>(std::max(0, e.depth)) * 2000u;
    const uint32_t killPart  = e.kills * 60u;
    const uint32_t goldPart  = static_cast<uint32_t>(std::max(0, e.gold)) * 4u;
    const uint32_t lvlPart   = static_cast<uint32_t>(std::max(0, e.level)) * 250u;

    s += depthPart;
    s += killPart;
    s += goldPart;
    s += lvlPart;

    if (e.won) s += 15000u;

    // Time penalty, capped so it can't go negative.
    const uint32_t penalty = std::min<uint32_t>(8000u, e.turns / 3u);
    if (s > penalty) s -= penalty;
    else s = 0;

    return s;
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

    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        splitCsvLine(line, cols);
        if (cols.empty()) continue;

        // Skip header.
        if (cols[0] == "timestamp") continue;

        // Expected columns:
        // timestamp,won,score,depth,turns,kills,level,gold,seed
        if (cols.size() < 9) continue;

        ScoreEntry e;
        e.timestamp = cols[0];

        bool b = false;
        if (parseBool(cols[1], b)) e.won = b;

        uint32_t u = 0;
        if (parseU32(cols[2], u)) e.score = u;

        int i = 1;
        if (parseI32(cols[3], i)) e.depth = i;

        if (parseU32(cols[4], u)) e.turns = u;
        if (parseU32(cols[5], u)) e.kills = u;

        if (parseI32(cols[6], i)) e.level = i;
        if (parseI32(cols[7], i)) e.gold = i;
        if (parseU32(cols[8], u)) e.seed = u;

        // Backfill score if file was missing it.
        if (e.score == 0) e.score = computeScore(e);

        entries_.push_back(std::move(e));
    }

    // Keep sorted by score desc.
    std::sort(entries_.begin(), entries_.end(), [](const ScoreEntry& a, const ScoreEntry& b) {
        if (a.score != b.score) return a.score > b.score;
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
    out << "timestamp,won,score,depth,turns,kills,level,gold,seed\n";
    for (const auto& s : entries_) {
        // timestamp can contain spaces, but not commas; still quote to be safe.
        out << '"' << s.timestamp << '"' << ','
            << (s.won ? 1 : 0) << ','
            << s.score << ','
            << s.depth << ','
            << s.turns << ','
            << s.kills << ','
            << s.level << ','
            << s.gold << ','
            << s.seed
            << "\n";
    }

    return atomicWriteTextFile(path, out.str());
}
