#ifndef __HEURISTIC_H__
#define __HEURISTIC_H__

#include <queue>
#include <vector>
#include <cassert> 
#include <unordered_map>
#include "linear_constraint.h"

using namespace std;

enum BranchHeuristic {
    BH_HP = 1,
    BH_HP_P = 2,
    BH_dTreeHP = 3,
    BH_BC = 4,
    BH_BC_P = 5,
    BH_DomWdeg = 6,
    BH_DomWdeg_P = 7,
    BH_Dom = 8,
    BH_Wdeg = 9,
    BH_Random = 10
};

class Heuristic {
public:
    static const int inf;
    static queue<int> Q;
    static vector<int> tmp;
    static vector<vector<bool>> e;
    static vector<vector<int>> dis;
    static vector<vector<double>> num;
    static BranchHeuristic heuristic;
    static bool global_state_enabled;

    static void set_heuristic(BranchHeuristic h);

    static int findCID(LinearConstraint &lc, unordered_map<int, int> &CID);

    static int select_variable(LinearConstraint &lc);

    static int select_by_centrality(LinearConstraint &lc, bool Pmodel);

    static int select_by_dom(LinearConstraint &lc);

    static int select_by_wdeg(LinearConstraint &lc);

    static int select_by_domwdeg(LinearConstraint &lc, bool Pmodel);

    static int select_by_hp(LinearConstraint &lc, bool Pmodel);

    static void init_dtree_root(LinearConstraint &lc);

    static int select_by_dtree_hp(LinearConstraint &lc);

    static int select_by_random(LinearConstraint &lc);

private:
    static bool needs_global_state(BranchHeuristic h);
};

#endif