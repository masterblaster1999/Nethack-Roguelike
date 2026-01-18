#include "pathfinding.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>

namespace {

inline bool inBounds(int w, int h, int x, int y) {
    return x >= 0 && y >= 0 && x < w && y < h;
}

inline int idxOf(int w, int x, int y) {
    return y * w + x;
}

constexpr int DIRS8[8][2] = {
    {1,0},{-1,0},{0,1},{0,-1},
    {1,1},{1,-1},{-1,1},{-1,-1}
};

using Node = std::pair<int, int>; // (cost, idx)

} // namespace

std::vector<Vec2i> dijkstraPath(
    int width,
    int height,
    Vec2i start,
    Vec2i goal,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk)
{
    if (width <= 0 || height <= 0) return {};
    if (!inBounds(width, height, start.x, start.y)) return {};
    if (!inBounds(width, height, goal.x, goal.y)) return {};
    if (start == goal) return {start};

    const int startI = idxOf(width, start.x, start.y);
    const int goalI = idxOf(width, goal.x, goal.y);

    const int INF = std::numeric_limits<int>::max() / 4;
    std::vector<int> dist(static_cast<size_t>(width * height), INF);
    std::vector<int> prev(static_cast<size_t>(width * height), -1);

    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;

    // Note: we intentionally do NOT require passable(start); the caller may
    // allow starting on non-passable tiles in some edge cases (e.g., standing
    // on an explored-but-locked door that just got unlocked).
    dist[static_cast<size_t>(startI)] = 0;
    pq.push({0, startI});

    while (!pq.empty()) {
        const Node cur = pq.top();
        pq.pop();

        const int costHere = cur.first;
        const int i = cur.second;
        if (i == goalI) break;
        if (costHere != dist[static_cast<size_t>(i)]) continue;

        const int x = i % width;
        const int y = i / width;

        for (const auto& dv : DIRS8) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!inBounds(width, height, nx, ny)) continue;
            if (!passable(nx, ny)) continue;

            if (dv[0] != 0 && dv[1] != 0) {
                if (diagonalOk && !diagonalOk(x, y, dv[0], dv[1])) continue;
            }

            const int step = stepCost(nx, ny);
            if (step <= 0) continue;

            const int ni = idxOf(width, nx, ny);
            const int ncost = costHere + step;
            if (ncost < dist[static_cast<size_t>(ni)]) {
                dist[static_cast<size_t>(ni)] = ncost;
                prev[static_cast<size_t>(ni)] = i;
                pq.push({ncost, ni});
            }
        }
    }

    if (dist[static_cast<size_t>(goalI)] == INF) return {};

    // Reconstruct
    std::vector<Vec2i> path;
    int cur = goalI;
    while (cur != -1) {
        path.push_back({cur % width, cur / width});
        if (cur == startI) break;
        cur = prev[static_cast<size_t>(cur)];
    }

    if (path.empty() || path.back() != start) return {};
    std::reverse(path.begin(), path.end());
    return path;
}


std::vector<int> dijkstraCostToTarget(
    int width,
    int height,
    Vec2i target,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk,
    int maxCost)
{
    std::vector<int> dist(static_cast<size_t>(std::max(0, width) * std::max(0, height)), -1);
    if (width <= 0 || height <= 0) return dist;
    if (!inBounds(width, height, target.x, target.y)) return dist;
    if (!passable(target.x, target.y)) return dist;

    const int targetI = idxOf(width, target.x, target.y);
    dist[static_cast<size_t>(targetI)] = 0;

    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;
    pq.push({0, targetI});

    while (!pq.empty()) {
        const Node cur = pq.top();
        pq.pop();

        const int costHere = cur.first;
        const int i = cur.second;

        if (maxCost >= 0 && costHere > maxCost) continue;
        if (dist[static_cast<size_t>(i)] != costHere) continue;

        const int x = i % width;
        const int y = i / width;

        // When expanding OUTWARD from the target, we add the cost of entering
        // the CURRENT tile, because a path from a neighbor -> target would
        // step into (x,y) first.
        const int enterCostHere = stepCost(x, y);
        if (enterCostHere <= 0) continue;

        for (const auto& dv : DIRS8) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!inBounds(width, height, nx, ny)) continue;
            if (!passable(nx, ny)) continue;

            if (dv[0] != 0 && dv[1] != 0) {
                // Reverse move: neighbor -> current, so flip the direction.
                if (diagonalOk && !diagonalOk(nx, ny, -dv[0], -dv[1])) continue;
            }

            const int ni = idxOf(width, nx, ny);
            const int ncost = costHere + enterCostHere;
            if (maxCost >= 0 && ncost > maxCost) continue;

            int& slot = dist[static_cast<size_t>(ni)];
            if (slot < 0 || ncost < slot) {
                slot = ncost;
                pq.push({ncost, ni});
            }
        }
    }

    return dist;
}

std::vector<int> dijkstraCostToNearestSource(
    int width,
    int height,
    const std::vector<Vec2i>& sources,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk,
    int maxCost)
{
    std::vector<DijkstraSeed> seeds;
    seeds.reserve(sources.size());
    for (const Vec2i& s : sources) {
        seeds.push_back(DijkstraSeed{s, 0});
    }

    return dijkstraCostToNearestSeeded(width, height, seeds, passable, stepCost, diagonalOk, maxCost);
}


std::vector<int> dijkstraCostToNearestSeeded(
    int width,
    int height,
    const std::vector<DijkstraSeed>& seeds,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk,
    int maxCost)
{
    const size_t n = static_cast<size_t>(std::max(0, width) * std::max(0, height));
    std::vector<int> dist(n, -1);
    if (width <= 0 || height <= 0) return dist;
    if (seeds.empty()) return dist;

    const int INF = std::numeric_limits<int>::max() / 4;
    std::vector<int> best(n, INF);

    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;

    // Seed with all valid sources.
    for (const DijkstraSeed& s : seeds) {
        if (!inBounds(width, height, s.pos.x, s.pos.y)) continue;
        if (!passable(s.pos.x, s.pos.y)) continue;

        int init = s.initialCost;
        if (init < 0) init = 0;
        if (maxCost >= 0 && init > maxCost) continue;

        const int si = idxOf(width, s.pos.x, s.pos.y);
        int& slot = best[static_cast<size_t>(si)];
        if (init >= slot) continue;

        slot = init;
        pq.push({init, si});
    }

    if (pq.empty()) return dist;

    while (!pq.empty()) {
        const Node cur = pq.top();
        pq.pop();

        const int costHere = cur.first;
        const int i = cur.second;

        if (maxCost >= 0 && costHere > maxCost) continue;
        if (best[static_cast<size_t>(i)] != costHere) continue;

        const int x = i % width;
        const int y = i / width;

        // While expanding outward (reverse), add the cost of entering the CURRENT tile
        // so a neighbor -> (x,y) -> ... path is priced correctly.
        const int enterCostHere = stepCost(x, y);
        if (enterCostHere <= 0) continue;

        for (const auto& dv : DIRS8) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!inBounds(width, height, nx, ny)) continue;
            if (!passable(nx, ny)) continue;

            if (dv[0] != 0 && dv[1] != 0) {
                // Reverse move: neighbor -> current, so flip the direction.
                if (diagonalOk && !diagonalOk(nx, ny, -dv[0], -dv[1])) continue;
            }

            const int ni = idxOf(width, nx, ny);
            const int ncost = costHere + enterCostHere;
            if (maxCost >= 0 && ncost > maxCost) continue;

            int& slot = best[static_cast<size_t>(ni)];
            if (ncost < slot) {
                slot = ncost;
                pq.push({ncost, ni});
            }
        }
    }

    // Convert to the public -1 sentinel format.
    for (size_t i = 0; i < n; ++i) {
        if (best[i] == INF) continue;
        if (maxCost >= 0 && best[i] > maxCost) continue;
        dist[i] = best[i];
    }

    return dist;
}


DijkstraNearestSeededResult dijkstraCostToNearestSeededWithProvenance(
	int width,
	int height,
	const std::vector<DijkstraSeed>& seeds,
	const PassableFn& passable,
	const StepCostFn& stepCost,
	const DiagonalOkFn& diagonalOk,
	int maxCost)
{
	DijkstraNearestSeededResult out;
	const size_t n = static_cast<size_t>(std::max(0, width) * std::max(0, height));
	out.cost.assign(n, -1);
	out.nearestSeedIndex.assign(n, -1);
	if (width <= 0 || height <= 0) return out;
	if (seeds.empty()) return out;

	const int INF = std::numeric_limits<int>::max() / 4;
	std::vector<int> best(n, INF);
	std::vector<int> bestSeed(n, -1);

	std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;

	// Seed with all valid sources.
	for (int si = 0; si < static_cast<int>(seeds.size()); ++si) {
		const DijkstraSeed& s = seeds[static_cast<size_t>(si)];
		if (!inBounds(width, height, s.pos.x, s.pos.y)) continue;
		if (!passable(s.pos.x, s.pos.y)) continue;

		int init = s.initialCost;
		if (init < 0) init = 0;
		if (maxCost >= 0 && init > maxCost) continue;

		const int idx = idxOf(width, s.pos.x, s.pos.y);
		const size_t ii = static_cast<size_t>(idx);
		if (ii >= n) continue;

		const int cur = best[ii];
		if (init < cur || (init == cur && (bestSeed[ii] < 0 || si < bestSeed[ii]))) {
			best[ii] = init;
			bestSeed[ii] = si;
			pq.push({init, idx});
		}
	}

	if (pq.empty()) return out;

	while (!pq.empty()) {
		const Node cur = pq.top();
		pq.pop();

		const int costHere = cur.first;
		const int i = cur.second;
		const size_t ii = static_cast<size_t>(i);

		if (maxCost >= 0 && costHere > maxCost) continue;
		if (ii >= n) continue;
		if (best[ii] != costHere) continue;

		const int seedHere = bestSeed[ii];
		if (seedHere < 0) continue;

		const int x = i % width;
		const int y = i / width;

		// While expanding outward (reverse), add the cost of entering the CURRENT tile
		// so a neighbor -> (x,y) -> ... path is priced correctly.
		const int enterCostHere = stepCost(x, y);
		if (enterCostHere <= 0) continue;

		for (const auto& dv : DIRS8) {
			const int nx = x + dv[0];
			const int ny = y + dv[1];
			if (!inBounds(width, height, nx, ny)) continue;
			if (!passable(nx, ny)) continue;

			if (dv[0] != 0 && dv[1] != 0) {
				// Reverse move: neighbor -> current, so flip the direction.
				if (diagonalOk && !diagonalOk(nx, ny, -dv[0], -dv[1])) continue;
			}

			const int ni = idxOf(width, nx, ny);
			const int ncost = costHere + enterCostHere;
			if (maxCost >= 0 && ncost > maxCost) continue;

			const size_t nii = static_cast<size_t>(ni);
			if (nii >= n) continue;

			const int prevCost = best[nii];
			const int prevSeed = bestSeed[nii];
			if (ncost < prevCost || (ncost == prevCost && (prevSeed < 0 || seedHere < prevSeed))) {
				best[nii] = ncost;
				bestSeed[nii] = seedHere;
				pq.push({ncost, ni});
			}
		}
	}

	// Convert to the public -1 sentinel format.
	for (size_t i = 0; i < n; ++i) {
		if (best[i] == INF) continue;
		if (maxCost >= 0 && best[i] > maxCost) continue;
		out.cost[i] = best[i];
		out.nearestSeedIndex[i] = bestSeed[i];
	}

	return out;
}


std::vector<int> dijkstraCostFromSources(
    int width,
    int height,
    const std::vector<Vec2i>& sources,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk,
    int maxCost)
{
    std::vector<DijkstraSeed> seeds;
    seeds.reserve(sources.size());
    for (const Vec2i& s : sources) {
        seeds.push_back(DijkstraSeed{s, 0});
    }

    return dijkstraCostFromSeeded(width, height, seeds, passable, stepCost, diagonalOk, maxCost);
}


std::vector<int> dijkstraCostFromSeeded(
    int width,
    int height,
    const std::vector<DijkstraSeed>& seeds,
    const PassableFn& passable,
    const StepCostFn& stepCost,
    const DiagonalOkFn& diagonalOk,
    int maxCost)
{
    const size_t n = static_cast<size_t>(std::max(0, width) * std::max(0, height));
    std::vector<int> dist(n, -1);
    if (width <= 0 || height <= 0) return dist;
    if (seeds.empty()) return dist;

    const int INF = std::numeric_limits<int>::max() / 4;
    std::vector<int> best(n, INF);

    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;

    // Seed with all valid sources.
    for (const DijkstraSeed& s : seeds) {
        if (!inBounds(width, height, s.pos.x, s.pos.y)) continue;
        if (!passable(s.pos.x, s.pos.y)) continue;

        int init = s.initialCost;
        if (init < 0) init = 0;
        if (maxCost >= 0 && init > maxCost) continue;

        const int si = idxOf(width, s.pos.x, s.pos.y);
        int& slot = best[static_cast<size_t>(si)];
        if (init >= slot) continue;

        slot = init;
        pq.push({init, si});
    }

    if (pq.empty()) return dist;

    while (!pq.empty()) {
        const Node cur = pq.top();
        pq.pop();

        const int costHere = cur.first;
        const int i = cur.second;

        if (maxCost >= 0 && costHere > maxCost) continue;
        if (best[static_cast<size_t>(i)] != costHere) continue;

        const int x = i % width;
        const int y = i / width;

        for (const auto& dv : DIRS8) {
            const int nx = x + dv[0];
            const int ny = y + dv[1];
            if (!inBounds(width, height, nx, ny)) continue;
            if (!passable(nx, ny)) continue;

            if (dv[0] != 0 && dv[1] != 0) {
                if (diagonalOk && !diagonalOk(x, y, dv[0], dv[1])) continue;
            }

            const int step = stepCost(nx, ny);
            if (step <= 0) continue;

            const int ncost = costHere + step;
            if (maxCost >= 0 && ncost > maxCost) continue;

            const int ni = idxOf(width, nx, ny);
            int& slot = best[static_cast<size_t>(ni)];
            if (ncost < slot) {
                slot = ncost;
                pq.push({ncost, ni});
            }
        }
    }

    // Convert to the public -1 sentinel format.
    for (size_t i = 0; i < n; ++i) {
        if (best[i] == INF) continue;
        if (maxCost >= 0 && best[i] > maxCost) continue;
        dist[i] = best[i];
    }

    return dist;
}
