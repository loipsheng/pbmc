//Implementation of Algorithm 2 in the paper.
#include "solver.h"
#include "heuristic.h"
#include <chrono>

long long Solver::decompose_count = 0;
long long Solver::select_count = 0;
bool Solver::simplification_timing_enabled = false;
long long Solver::simplification_time_ns = 0;

void Solver::resetSimplificationTiming() {
    simplification_time_ns = 0;
}

double Solver::simplificationTimeSeconds() {
    return static_cast<double>(simplification_time_ns) * 1e-9;
}

Int Solver::count(LinearConstraint &lc) {
    // Project convention: a top-level instance with no declared variables
    // does not count the empty assignment as a solution.
    if (lc.getN() == 0)
        return 0;

    return count(lc, 1);
}

Int Solver::count(LinearConstraint &lc, int level) {
    if (lc.hasEmptyDomain())
        return 0;

    if (level == 1) {
        lc.initPID();
        Heuristic::init_dtree_root(lc);
    }

    std::chrono::steady_clock::time_point simplify_start;
    if (simplification_timing_enabled)
        simplify_start = std::chrono::steady_clock::now();
    Int res = lc.simplify(level);
    if (simplification_timing_enabled) {
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - simplify_start);
        simplification_time_ns += elapsed.count();
    }
    if (res.cmp(0) >= 0) {
        lc.insertCache(res);
        return res;
    }

    Int factor = lc.free_variable();

    Int t = lc.findCache();
    if (t.cmp(-1) > 0) {
        lc.CC++;
        return t * factor;
    }

    auto subs = lc.decompose();
    if (!subs.empty()) {
        decompose_count++;
        lc.CD++;
        res = 1;
        for (auto &sub : subs) {
            res *= count(sub, level + 1);
            if (res.cmp(0) == 0) {
                lc.insertCache(0);
                return 0;
            }
        }
        lc.insertCache(res);
        return res * factor;
    }

    select_count++;
    int id = Heuristic::select_variable(lc);
    if (id < 0) id = lc.select();

    res = 0;
    LinearConstraint tlc;
    for (Int i = lc.ranges[id].first; i.cmp(lc.ranges[id].second) <= 0; i++) {
        tlc = lc;
        tlc.ranges[id].first = tlc.ranges[id].second = i;
        tlc.deleteVariable(id);
        res += count(tlc, level + 1);
    }

    lc.insertCache(res);
    return res * factor;
}
