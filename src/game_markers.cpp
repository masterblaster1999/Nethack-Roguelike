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

void Game::applyAmnesiaShock(int keepRadiusCheb) {
    // This is intentionally a "state-only" helper: the caller is responsible for messaging.
    if (isFinished()) return;

    keepRadiusCheb = std::max(-1, keepRadiusCheb);

    // Cancel automation first so we don't immediately re-path based on stale map state.
    stopAutoMove(true);

    Dungeon& dung = this->dung;
    const Vec2i center = player().pos;

    // 1) Forget explored tiles (optionally keeping a local patch).
    for (int y = 0; y < dung.height; ++y) {
        for (int x = 0; x < dung.width; ++x) {
            const int dist = chebyshev(center, {x, y});
            if (keepRadiusCheb > 0 && dist <= keepRadiusCheb) continue;
            dung.at(x, y).explored = false;
        }
    }

    // 2) Forget discovered traps outside the local patch.
    for (auto& tr : trapsCur) {
        const int dist = chebyshev(center, tr.pos);
        if (keepRadiusCheb > 0 && dist <= keepRadiusCheb) continue;
        tr.discovered = false;
    }

    // 3) Forget map markers outside the local patch.
    mapMarkers_.erase(std::remove_if(mapMarkers_.begin(), mapMarkers_.end(), [&](const MapMarker& mm) {
                         const int dist = chebyshev(center, mm.pos);
                         return !(keepRadiusCheb > 0 && dist <= keepRadiusCheb);
                     }),
        mapMarkers_.end());

    // 4) Reset auto-explore "already tried searching" bookkeeping for forgotten tiles.
    if (autoExploreSearchTriedTurns.size() == (size_t)dung.width * (size_t)dung.height) {
        for (int y = 0; y < dung.height; ++y) {
            for (int x = 0; x < dung.width; ++x) {
                const int dist = chebyshev(center, {x, y});
                if (keepRadiusCheb > 0 && dist <= keepRadiusCheb) continue;
                autoExploreSearchTriedTurns[(size_t)y * (size_t)dung.width + (size_t)x] = 0;
            }
        }
    }

    // 5) Unseen hostile monsters may lose the thread.
    for (auto& e : ents) {
        if (e.hp <= 0) continue;
        if (e.id == playerId_) continue;
        if (e.friendly) continue;

        if (dung.inBounds(e.pos.x, e.pos.y) && dung.at(e.pos.x, e.pos.y).visible) continue;

        e.alerted = false;
        e.lastKnownPlayerPos = {-1, -1};
        e.lastKnownPlayerAge = 9999;
    }

    // 6) Recompute visibility so the player at least remembers what they can currently see.
    recomputeFov();
}
