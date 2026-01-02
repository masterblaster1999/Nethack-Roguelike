#include "replay.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace {

static std::string trimCopy(std::string s) {
    // Reuse trim() if available via common utilities, but keep this TU standalone.
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static bool startsWith(const std::string& s, const char* prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

static std::optional<int> parseInt(const std::string& s) {
    try {
        size_t idx = 0;
        int v = std::stoi(s, &idx, 10);
        if (idx != s.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

static std::optional<uint32_t> parseU32(const std::string& s) {
    try {
        size_t idx = 0;
        unsigned long v = std::stoul(s, &idx, 10);
        if (idx != s.size()) return std::nullopt;
        if (v > 0xFFFFFFFFul) return std::nullopt;
        return static_cast<uint32_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

static bool parseVec2i(std::istringstream& iss, Vec2i& out) {
    int x = 0, y = 0;
    if (!(iss >> x >> y)) return false;
    out = {x, y};
    return true;
}

static void setErr(std::string* err, const std::string& msg) {
    if (err) *err = msg;
}

} // namespace

std::string replayHexEncode(const std::string& bytes) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (unsigned char c : bytes) {
        out.push_back(kHex[(c >> 4) & 0xF]);
        out.push_back(kHex[(c >> 0) & 0xF]);
    }
    return out;
}

bool replayHexDecode(const std::string& hex, std::string& outBytes) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };

    if ((hex.size() % 2) != 0) return false;
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hexVal(hex[i]);
        const int lo = hexVal(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    outBytes.swap(out);
    return true;
}

bool ReplayWriter::open(const std::filesystem::path& path, const ReplayMeta& meta, std::string* err) {
    close();
    path_ = path;
    f_.open(path_, std::ios::out | std::ios::trunc);
    if (!f_) {
        setErr(err, "Failed to open replay for writing: " + path_.string());
        return false;
    }

    // Header
    f_ << "@procrogue_replay " << meta.formatVersion << "\n";
    f_ << "@game_version " << meta.gameVersion << "\n";
    f_ << "@seed " << meta.seed << "\n";
    if (!meta.playerClassId.empty()) {
        f_ << "@class " << meta.playerClassId << "\n";
    }

    f_ << "@auto_pickup " << static_cast<int>(meta.autoPickup) << "\n";
    f_ << "@auto_step_delay_ms " << meta.autoStepDelayMs << "\n";
    f_ << "@auto_explore_search " << (meta.autoExploreSearch ? 1 : 0) << "\n";
    f_ << "@identify_items " << (meta.identifyItems ? 1 : 0) << "\n";
    f_ << "@hunger_enabled " << (meta.hungerEnabled ? 1 : 0) << "\n";
    f_ << "@encumbrance_enabled " << (meta.encumbranceEnabled ? 1 : 0) << "\n";
    f_ << "@lighting_enabled " << (meta.lightingEnabled ? 1 : 0) << "\n";
    f_ << "@yendor_doom_enabled " << (meta.yendorDoomEnabled ? 1 : 0) << "\n";
    f_ << "@bones_enabled " << (meta.bonesEnabled ? 1 : 0) << "\n";
    f_ << "@end_header\n";
    f_.flush();

    return true;
}

void ReplayWriter::close() {
    if (f_) {
        f_.flush();
        f_.close();
    }
    path_.clear();
}

void ReplayWriter::writeLine_(const std::string& line) {
    if (!f_) return;
    f_ << line << "\n";
}

void ReplayWriter::writeAction(uint32_t tMs, Action a) {
    writeLine_(std::to_string(tMs) + " A " + std::to_string(static_cast<int>(a)));
}

void ReplayWriter::writeStateHash(uint32_t tMs, uint32_t turn, uint64_t hash) {
    std::ostringstream ss;
    ss << tMs << " H " << turn << " ";
    ss << std::hex << std::setw(16) << std::setfill('0') << hash;
    writeLine_(ss.str());
}

void ReplayWriter::writeTextInput(uint32_t tMs, const std::string& utf8) {
    writeLine_(std::to_string(tMs) + " TI " + replayHexEncode(utf8));
}

void ReplayWriter::writeCommandBackspace(uint32_t tMs) {
    writeLine_(std::to_string(tMs) + " CB");
}

void ReplayWriter::writeCommandAutocomplete(uint32_t tMs) {
    writeLine_(std::to_string(tMs) + " CA");
}

void ReplayWriter::writeMessageHistoryBackspace(uint32_t tMs) {
    writeLine_(std::to_string(tMs) + " HB");
}

void ReplayWriter::writeMessageHistoryToggleSearchMode(uint32_t tMs) {
    writeLine_(std::to_string(tMs) + " HS");
}

void ReplayWriter::writeMessageHistoryClearSearch(uint32_t tMs) {
    writeLine_(std::to_string(tMs) + " HC");
}

void ReplayWriter::writeAutoTravel(uint32_t tMs, Vec2i p) {
    writeLine_(std::to_string(tMs) + " TR " + std::to_string(p.x) + " " + std::to_string(p.y));
}

void ReplayWriter::writeBeginLook(uint32_t tMs, Vec2i p) {
    writeLine_(std::to_string(tMs) + " BL " + std::to_string(p.x) + " " + std::to_string(p.y));
}

void ReplayWriter::writeTargetCursor(uint32_t tMs, Vec2i p) {
    writeLine_(std::to_string(tMs) + " TC " + std::to_string(p.x) + " " + std::to_string(p.y));
}

void ReplayWriter::writeLookCursor(uint32_t tMs, Vec2i p) {
    writeLine_(std::to_string(tMs) + " LC " + std::to_string(p.x) + " " + std::to_string(p.y));
}

bool loadReplayFile(const std::filesystem::path& path, ReplayFile& out, std::string* err) {
    out = ReplayFile{};

    std::ifstream f(path);
    if (!f) {
        setErr(err, "Failed to open replay for reading: " + path.string());
        return false;
    }

    bool inHeader = true;
    std::string line;
    int lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        line = trimCopy(line);
        if (line.empty()) continue;
        if (line[0] == '#') continue;

        if (inHeader) {
            if (line == "@end_header") {
                inHeader = false;
                continue;
            }

            if (!startsWith(line, "@")) {
                setErr(err, "Replay parse error (expected header @key): line " + std::to_string(lineNo));
                return false;
            }

            // Header line: @key value...
            std::istringstream iss(line);
            std::string key;
            iss >> key;
            std::string value;
            std::getline(iss, value);
            value = trimCopy(value);

            if (key == "@procrogue_replay") {
                auto v = parseInt(value);
                if (!v.has_value() || *v <= 0) {
                    setErr(err, "Replay parse error (bad format version) line " + std::to_string(lineNo));
                    return false;
                }
                out.meta.formatVersion = *v;
                continue;
            }
            if (key == "@game_version") {
                out.meta.gameVersion = value;
                continue;
            }
            if (key == "@seed") {
                auto v = parseU32(value);
                if (!v.has_value()) {
                    setErr(err, "Replay parse error (bad seed) line " + std::to_string(lineNo));
                    return false;
                }
                out.meta.seed = *v;
                continue;
            }
            if (key == "@class") {
                out.meta.playerClassId = value;
                continue;
            }

            if (key == "@auto_pickup") {
                auto v = parseInt(value);
                if (!v.has_value()) continue;
                const int iv = *v;
                if (iv < 0 || iv > 3) continue;
                out.meta.autoPickup = static_cast<AutoPickupMode>(iv);
                continue;
            }
            if (key == "@auto_step_delay_ms") {
                auto v = parseInt(value);
                if (v.has_value()) out.meta.autoStepDelayMs = *v;
                continue;
            }
            if (key == "@auto_explore_search") {
                auto v = parseInt(value);
                if (v.has_value()) out.meta.autoExploreSearch = (*v != 0);
                continue;
            }
            if (key == "@identify_items") {
                auto v = parseInt(value);
                if (v.has_value()) out.meta.identifyItems = (*v != 0);
                continue;
            }
            if (key == "@hunger_enabled") {
                auto v = parseInt(value);
                if (v.has_value()) out.meta.hungerEnabled = (*v != 0);
                continue;
            }
            if (key == "@encumbrance_enabled") {
                auto v = parseInt(value);
                if (v.has_value()) out.meta.encumbranceEnabled = (*v != 0);
                continue;
            }
            if (key == "@lighting_enabled") {
                auto v = parseInt(value);
                if (v.has_value()) out.meta.lightingEnabled = (*v != 0);
                continue;
            }
            if (key == "@yendor_doom_enabled") {
                auto v = parseInt(value);
                if (v.has_value()) out.meta.yendorDoomEnabled = (*v != 0);
                continue;
            }
            if (key == "@bones_enabled") {
                auto v = parseInt(value);
                if (v.has_value()) out.meta.bonesEnabled = (*v != 0);
                continue;
            }

            // Unknown header keys are ignored for forward compat.
            continue;
        }

        // Event line
        // <ms> CODE [payload...]
        std::istringstream iss(line);
        uint32_t tMs = 0;
        if (!(iss >> tMs)) {
            setErr(err, "Replay parse error (missing time) line " + std::to_string(lineNo));
            return false;
        }
        std::string code;
        iss >> code;
        if (code.empty()) {
            setErr(err, "Replay parse error (missing event code) line " + std::to_string(lineNo));
            return false;
        }

        ReplayEvent ev;
        ev.tMs = tMs;

        if (code == "A") {
            int ai = 0;
            if (!(iss >> ai)) {
                setErr(err, "Replay parse error (bad action) line " + std::to_string(lineNo));
                return false;
            }
            if (ai < 0 || ai > 255) {
                setErr(err, "Replay parse error (action out of range) line " + std::to_string(lineNo));
                return false;
            }
            ev.kind = ReplayEventType::Action;
            ev.action = static_cast<Action>(static_cast<uint8_t>(ai));
            out.events.push_back(std::move(ev));
            continue;
        }
        if (code == "H") {
            // State hash checkpoint: <ms> H <turn> <hash64hex>
            int turn = 0;
            std::string hex;
            if (!(iss >> turn >> hex)) {
                setErr(err, "Replay parse error (bad state hash payload) line " + std::to_string(lineNo));
                return false;
            }
            if (turn < 0) {
                setErr(err, "Replay parse error (negative turn) line " + std::to_string(lineNo));
                return false;
            }
            try {
                size_t idx = 0;
                uint64_t hv = std::stoull(hex, &idx, 16);
                if (idx != hex.size()) {
                    setErr(err, "Replay parse error (bad state hash) line " + std::to_string(lineNo));
                    return false;
                }
                ev.kind = ReplayEventType::StateHash;
                ev.turn = static_cast<uint32_t>(turn);
                ev.hash = hv;
                out.events.push_back(std::move(ev));
                continue;
            } catch (...) {
                setErr(err, "Replay parse error (bad state hash) line " + std::to_string(lineNo));
                return false;
            }
        }
        if (code == "TI") {
            std::string hex;
            iss >> hex;
            if (hex.empty()) {
                setErr(err, "Replay parse error (missing text payload) line " + std::to_string(lineNo));
                return false;
            }
            std::string decoded;
            if (!replayHexDecode(hex, decoded)) {
                setErr(err, "Replay parse error (bad hex text) line " + std::to_string(lineNo));
                return false;
            }
            ev.kind = ReplayEventType::TextInput;
            ev.text = std::move(decoded);
            out.events.push_back(std::move(ev));
            continue;
        }

        if (code == "CB") {
            ev.kind = ReplayEventType::CommandBackspace;
            out.events.push_back(std::move(ev));
            continue;
        }
        if (code == "CA") {
            ev.kind = ReplayEventType::CommandAutocomplete;
            out.events.push_back(std::move(ev));
            continue;
        }
        if (code == "HB") {
            ev.kind = ReplayEventType::MessageHistoryBackspace;
            out.events.push_back(std::move(ev));
            continue;
        }
        if (code == "HS") {
            ev.kind = ReplayEventType::MessageHistoryToggleSearch;
            out.events.push_back(std::move(ev));
            continue;
        } else if (code == "HC") {
            ev.kind = ReplayEventType::MessageHistoryClearSearch;
            out.events.push_back(std::move(ev));
            continue;
        }

        if (code == "TR" || code == "BL" || code == "TC" || code == "LC") {
            Vec2i p{};
            if (!parseVec2i(iss, p)) {
                setErr(err, "Replay parse error (bad position payload) line " + std::to_string(lineNo));
                return false;
            }
            if (code == "TR") ev.kind = ReplayEventType::AutoTravel;
            if (code == "BL") ev.kind = ReplayEventType::BeginLook;
            if (code == "TC") ev.kind = ReplayEventType::TargetCursor;
            if (code == "LC") ev.kind = ReplayEventType::LookCursor;
            ev.pos = p;
            out.events.push_back(std::move(ev));
            continue;
        }

        // Unknown event codes are ignored for forward compat.
    }

    if (inHeader) {
        setErr(err, "Replay parse error (missing @end_header)");
        return false;
    }
    if (out.meta.seed == 0) {
        // seed==0 is valid in theory, but in this game it usually means "not initialized".
        // Don't fail hard: allow the file, but it's likely unusable.
    }
    return true;
}