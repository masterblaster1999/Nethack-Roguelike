#include "game_internal.hpp"

namespace {

// Keep marker labels short and UI-friendly.
constexpr size_t kMaxMarkerLabelLen = 64;
constexpr size_t kMaxMarkersPerLevel = 250;

static std::string sanitizeMarkerLabel(const std::string& in) {
    std::string s = trim(in);
    if (s.size() > kMaxMarkerLabelLen) s.resize(kMaxMarkerLabelLen);
    return s;
}

} // namespace

const MapMarker* Game::markerAt(Vec2i p) const {
    for (const auto& m : mapMarkers_) {
        if (m.pos.x == p.x && m.pos.y == p.y) return &m;
    }
    return nullptr;
}

bool Game::setMarker(Vec2i p, MarkerKind kind, const std::string& label, bool verbose) {
    if (isFinished()) return false;

    if (!dung.inBounds(p.x, p.y)) {
        if (verbose) pushMsg("CAN'T MARK: OUT OF BOUNDS.", MessageKind::System, true);
        return false;
    }

    // Avoid letting players place notes on unknown space: it's confusing for navigation.
    if (!dung.at(p.x, p.y).explored) {
        if (verbose) pushMsg("CAN'T MARK UNEXPLORED TILE.", MessageKind::System, true);
        return false;
    }

    const std::string lab = sanitizeMarkerLabel(label);
    if (lab.empty()) {
        if (verbose) pushMsg("MARK LABEL REQUIRED.", MessageKind::System, true);
        return false;
    }

    // Update existing marker on this tile.
    for (auto& m : mapMarkers_) {
        if (m.pos.x == p.x && m.pos.y == p.y) {
            m.kind = kind;
            m.label = lab;
            if (verbose) {
                pushMsg(std::string("MARK UPDATED: ") + markerKindName(kind) + " \"" + lab + "\".", MessageKind::System, true);
            }
            return true;
        }
    }

    if (mapMarkers_.size() >= kMaxMarkersPerLevel) {
        if (verbose) pushMsg("TOO MANY MARKS ON THIS FLOOR.", MessageKind::Warning, true);
        return false;
    }

    MapMarker m;
    m.pos = p;
    m.kind = kind;
    m.label = lab;
    mapMarkers_.push_back(std::move(m));

    if (verbose) {
        pushMsg(std::string("MARK ADDED: ") + markerKindName(kind) + " \"" + lab + "\".", MessageKind::System, true);
    }
    return true;
}

bool Game::clearMarker(Vec2i p, bool verbose) {
    if (isFinished()) return false;

    const size_t before = mapMarkers_.size();
    mapMarkers_.erase(std::remove_if(mapMarkers_.begin(), mapMarkers_.end(), [&](const MapMarker& m) {
        return m.pos.x == p.x && m.pos.y == p.y;
    }), mapMarkers_.end());

    const bool removed = mapMarkers_.size() != before;
    if (verbose) {
        if (removed) pushMsg("MARK CLEARED.", MessageKind::System, true);
        else pushMsg("NO MARK HERE.", MessageKind::System, true);
    }
    return removed;
}

void Game::clearAllMarkers(bool verbose) {
    if (isFinished()) return;
    mapMarkers_.clear();
    if (verbose) pushMsg("ALL MARKS CLEARED ON THIS FLOOR.", MessageKind::System, true);
}
