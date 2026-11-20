// solver.h
#ifndef __SOLVER_H__
#define __SOLVER_H__

#include "big_int.h"
#include "linear_constraint.h"

class Solver {
public:
    static long long decompose_count;
    static long long select_count;
    static bool simplification_timing_enabled;
    static long long simplification_time_ns;

    static Int count(LinearConstraint &lc);
    static Int count(LinearConstraint &lc, int level);
    static void resetSimplificationTiming();
    static double simplificationTimeSeconds();
};

#endif
