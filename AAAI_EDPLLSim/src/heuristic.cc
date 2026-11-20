// Implementation of the variable selecting heuristics in Algorithm 2. 
#include "heuristic.h"
#include "big_int.h"
#include "patoh.h"

#include <cmath>
#include <vector>
#include <limits>
#include <unordered_map>
#include <algorithm>
#include <cstdlib>
#include <iostream>

using namespace std;

namespace {
    void reset_dtree_cache();

}

extern vector<Int> weight;
BranchHeuristic Heuristic::heuristic = BH_HP_P;
bool Heuristic::global_state_enabled = true;

const int Heuristic::inf = 0x3f3f3f3f;
queue<int> Heuristic::Q;
vector<int> Heuristic::tmp;
vector<vector<bool>> Heuristic::e;
vector<vector<int>> Heuristic::dis;
vector<vector<double>> Heuristic::num;

bool Heuristic::needs_global_state(BranchHeuristic h) {
    switch (h) {
    case BH_BC_P:
    case BH_DomWdeg:
    case BH_DomWdeg_P:
    case BH_Wdeg:
    case BH_HP_P:
    case BH_dTreeHP:
        return true;
    default:
        return false;
    }
}

void Heuristic::set_heuristic(BranchHeuristic h) {
    heuristic = h;
    global_state_enabled = needs_global_state(h);
    if (!global_state_enabled) {
        reset_dtree_cache();
    }
}

int Heuristic::findCID(LinearConstraint &lc, unordered_map<int, int> &CID) {
    if (!global_state_enabled)
        return -1;

    int pid = -1;
    CID.clear();

    for (int i = 0; i < (int)lc.PID.size(); i++) {
        CID.emplace(lc.PID[i], i);
    }

    while (!lc.kbest.empty()) {
        pid = lc.kbest[0];
        lc.kbest.erase(lc.kbest.begin());
        if (CID.find(pid) != CID.end())
            return CID[pid];
    }
    return -1;
}

int Heuristic::select_variable(LinearConstraint &lc) {
    switch (heuristic) {

    case BH_BC:
        return Heuristic::select_by_centrality(lc, false);

    case BH_BC_P:
        return Heuristic::select_by_centrality(lc, true);

    case BH_Dom:
        return Heuristic::select_by_dom(lc);

    case BH_Wdeg:
        return Heuristic::select_by_wdeg(lc);

    case BH_DomWdeg:
        return Heuristic::select_by_domwdeg(lc, false);

    case BH_DomWdeg_P:
        return Heuristic::select_by_domwdeg(lc, true);

    case BH_HP:
        return Heuristic::select_by_hp(lc, false);

    case BH_HP_P:
        return Heuristic::select_by_hp(lc, true);

    case BH_dTreeHP:
        return Heuristic::select_by_dtree_hp(lc);

    case BH_Random:
        return Heuristic::select_by_random(lc);

    default:
        return -1;
    }
}

int Heuristic::select_by_centrality(LinearConstraint &lc, bool Pmodel) {//BC
    int cid = -1;
    unordered_map<int, int> CID;

    if (Pmodel) {
        cid = findCID(lc, CID);
        if (cid != -1)
            return cid;
    }

    int n = lc.getN();
    e.assign(n, vector<bool>(n, false));
    dis.assign(n, vector<int>(n, inf));
    num.assign(n, vector<double>(n, 0));

    for (const auto &v : lc.matrix) {
        tmp.clear();
        for (int i = 0; i < n; i++) {
            if (!v[i].cmp(0))
                continue;
            for (int u : tmp)
                e[u][i] = e[i][u] = true;
            tmp.emplace_back(i);
        }
    }

    for (int s = 0; s < n; s++) {
        dis[s][s] = 0;
        num[s][s] = 1;
        Q.push(s);
        while (!Q.empty()) {
            int u = Q.front();
            Q.pop();
            for (int i = 0; i < n; i++) {
                if (!e[u][i])
                    continue;
                if (dis[s][i] == inf) {
                    dis[s][i] = dis[s][u] + 1;
                    Q.push(i);
                }
                if (dis[s][i] == dis[s][u] + 1) {
                    num[s][i] += num[s][u];
                }
            }
        }
    }

    int id = -1;
    double mx = 0;
    for (int i = 0; i < n; i++) {
        double res = 0;
        for (int j = 0; j < n; j++) {
            if (i == j)
                continue;
            assert(dis[i][j] != inf);
            assert(std::fabs(num[i][j] - num[j][i]) < 1e-9);
            assert(dis[i][j] == dis[j][i]);
            for (int k = 0; k < n; k++) {
                if (k == i || k == j)
                    continue;
                if (dis[j][k] < dis[j][i] + dis[i][k])
                    continue;
                assert(num[j][i] * num[i][k] <= num[j][k]);
                res += num[j][i] * num[i][k] / num[j][k];
            }
        }

        if (res < mx)
            continue;
        if (res == mx) {
            if (Pmodel)
                lc.kbest.push_back(i);
            continue;
        }
        if (Pmodel) {
            lc.kbest.clear();
            lc.kbest.push_back(i);
        }
        mx = res;
        id = i;
    }

    if (!Pmodel) {
        return id;
    }
    lc.domWdeg_Heur(); // update D/W ratio
    sort(lc.kbest.begin(), lc.kbest.end(),
        [&](int a, int b) {
            return lc.domWdegs[a] < lc.domWdegs[b];
        });
    id = lc.kbest[0];
    lc.kbest.erase(lc.kbest.begin());
    for (int i = 0; i < (int)lc.kbest.size(); i++) {
        lc.kbest[i] = lc.PID[lc.kbest[i]];
    }
    return id;
}

int Heuristic::select_by_dom(LinearConstraint &lc) {//Dom
    return lc.select();
}

int Heuristic::select_by_wdeg(LinearConstraint &lc) {//Wdeg
    int n = lc.getN();
    int m = lc.getM();

    vector<Int> wdegs(n, Int(0));
    vector<int> forBack;

    for (int i = 0; i < m; ++i) {
        forBack.clear();

        for (int j = 0; j < n; ++j) {
            if (lc.matrix[i][j] == 0)
                continue;
            wdegs[j] += weight[lc.mPID[i]];
            forBack.emplace_back(j);
        }

        if (forBack.size() > 1)
            continue;

        for (int b : forBack) {
            wdegs[b] -= weight[lc.mPID[i]];
        }
    }

    int best = -1;
    Int bestW;

    for (int i = 0; i < n; ++i) {
        if (wdegs[i].cmp(0) <= 0) {
            continue;
        }
        if (best == -1 || wdegs[i].cmp(bestW) > 0) {
            best = i;
            bestW = wdegs[i];
        }
    }

    return best;
}

int Heuristic::select_by_domwdeg(LinearConstraint &lc, bool Pmodel) {// D/W ratio
    int n = lc.getN();
    int m = lc.getM();

    if (Pmodel) {
        unordered_map<int, int> CID;
        int cid = Heuristic::findCID(lc, CID);
        if (cid != -1) {
            return cid;
        }
    }

    lc.domWdegs.clear();
    lc.domWdegs.resize(n);

    vector<Int> wdegs(n, Int(0));
    vector<int> forBack;
    for (int i = 0; i < m; ++i) {
        forBack.clear();

        for (int j = 0; j < n; ++j) {
            if (lc.matrix[i][j] == 0)
                continue;
            wdegs[j] += weight[lc.mPID[i]];
            forBack.emplace_back(j);
        }

        if (forBack.size() > 1)
            continue;

        for (int b : forBack) {
            wdegs[b] -= weight[lc.mPID[i]];
        }
    }

    int id = -1;
    double mn = numeric_limits<double>::max();
    vector<int> ties;
    if (Pmodel)
        ties.reserve(n);

    for (int i = 0; i < n; ++i) {
        double dom_d = lc.getRange(i).to_double();
        double wdeg_d = wdegs[i].to_double();

        lc.domWdegs[i] = dom_d / wdeg_d;
        double val = lc.domWdegs[i];

        if (val < mn) {
            mn = val;
            id = i;

            if (Pmodel) {
                ties.clear();
                ties.push_back(i);
            }
        }
        else if (Pmodel && val == mn) {
            ties.push_back(i);
        }
    }

    if (!Pmodel) {
        return id;
    }

    if (ties.empty()) {

        return id;
    }

    sort(ties.begin(), ties.end(),
        [&](int a, int b) {
            Int da = lc.getRange(a);
            Int db = lc.getRange(b);
            int c = da.cmp(db);
            if (c != 0)
                return c < 0;
            return a < b;
        });

    lc.kbest = ties;
    id = lc.kbest[0];
    lc.kbest.erase(lc.kbest.begin());

    for (int i = 0; i < (int)lc.kbest.size(); ++i) {
        int cid = lc.kbest[i];
        lc.kbest[i] = lc.PID[cid];
    }

    return id;
}

namespace {

    void build_hypergraph(const LinearConstraint &lc,
        int &c, int &n, int &nconst,
        int *&cwghts, int *&nwghts,
        int *&xpins, int *&pins) {
        int m = lc.getM();
        int nv = lc.getN();

        c = m;
        n = nv;
        nconst = 1;

        if (c == 0 || n == 0) {
            cwghts = nullptr;
            nwghts = nullptr;
            xpins = nullptr;
            pins = nullptr;
            return;
        }

        cwghts = new int[c];
        for (int i = 0; i < c; ++i)
            cwghts[i] = 1;

        nwghts = new int[n];
        for (int j = 0; j < n; ++j)
            nwghts[j] = 1;
        int nnz = 0;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < c; ++i)
                if (lc.matrix[i][j].cmp(0) != 0)
                    ++nnz;

        xpins = new int[n + 1];
        pins = new int[nnz];

        int idx = 0;
        for (int j = 0; j < n; ++j) {
            xpins[j] = idx;
            for (int i = 0; i < c; ++i) {
                if (lc.matrix[i][j].cmp(0) != 0) {
                    pins[idx++] = i;
                }
            }
        }
        xpins[n] = idx;
    }

    vector<int> compute_cutset(const LinearConstraint &lc) {
        vector<int> cutset;

        int c, n, nconst;
        int *cwghts = nullptr, *nwghts = nullptr;
        int *xpins = nullptr, *pins = nullptr;

        build_hypergraph(lc, c, n, nconst, cwghts, nwghts, xpins, pins);

        if (c == 0 || n == 0 || cwghts == nullptr) {
            delete[] cwghts;
            delete[] nwghts;
            delete[] xpins;
            delete[] pins;
            return cutset;
        }

        PaToH_Parameters args;
        PaToH_Initialize_Parameters(&args, PATOH_CONPART, PATOH_SUGPARAM_DEFAULT);
        args._k = 2;

        int *partvec = (int *)std::malloc(sizeof(int) * c);
        int *partweights = (int *)std::malloc(sizeof(int) * args._k * nconst);
        int cut = 0;

        PaToH_Alloc(&args, c, n, nconst, cwghts, nwghts, xpins, pins);
        PaToH_Part(&args,
            c, n, nconst,
            0,
            cwghts, nwghts,
            xpins, pins,
            nullptr,
            partvec, partweights,
            &cut);

        for (int j = 0; j < n; ++j) {
            int start = xpins[j];
            int end = xpins[j + 1];
            if (start >= end)
                continue;

            int firstPart = partvec[pins[start]];
            bool isCut = false;
            for (int p = start + 1; p < end; ++p) {
                if (partvec[pins[p]] != firstPart) {
                    isCut = true;
                    break;
                }
            }
            if (isCut)
                cutset.push_back(j);
        }

        std::free(partvec);
        std::free(partweights);
        delete[] cwghts;
        delete[] nwghts;
        delete[] xpins;
        delete[] pins;
        PaToH_Free();

        return cutset;
    }

}

int Heuristic::select_by_hp(LinearConstraint &lc, bool Pmodel) {//DHP
    int cid = -1;
    if (Pmodel) {
        unordered_map<int, int> CID;
        cid = Heuristic::findCID(lc, CID);
        if (cid != -1)
            return cid;
    }

    vector<int> cutset = compute_cutset(lc);
    if (cutset.empty())
        return -1;

    if (!Pmodel)
        return cutset[0];

    lc.domWdeg_Heur();

    lc.kbest.clear();
    for (int x : cutset)
        lc.kbest.push_back(x);

    sort(lc.kbest.begin(), lc.kbest.end(),
        [&](int a, int b) {
            if (lc.domWdegs[a] != lc.domWdegs[b])
                return lc.domWdegs[a] < lc.domWdegs[b];
            return lc.PID[a] < lc.PID[b];
        });

    int bestCid = lc.kbest[0];
    lc.kbest.erase(lc.kbest.begin());

    for (int i = 0; i < (int)lc.kbest.size(); ++i) {
        int cid2 = lc.kbest[i];
        lc.kbest[i] = lc.PID[cid2];
    }

    return bestCid;
}

namespace {
    vector<int> g_dtree_order;
    bool g_dtree_built = false;

    void reset_dtree_cache() {
        g_dtree_order.clear();
        g_dtree_built = false;
    }

    struct CutResult {
        vector<int> cut_pids;
        vector<int> left_pids;
        vector<int> right_pids;
    };

    CutResult compute_cut_and_partition_on_vars(
        const LinearConstraint &base_lc,
        const unordered_map<int, int> &basePID2cid,
        const vector<int> &vars) {
        CutResult res;
        int m = base_lc.getM();
        int n = (int)vars.size();
        int nconst = 1;

        if (m == 0 || n == 0) {
            return res;
        }

        vector<int> cids(n);
        for (int j = 0; j < n; ++j) {
            auto it = basePID2cid.find(vars[j]);
            if (it == basePID2cid.end()) {

                cids[j] = -1;
            }
            else {
                cids[j] = it->second;
            }
        }

        int nnz = 0;
        for (int j = 0; j < n; ++j) {
            int cid = cids[j];
            if (cid < 0)
                continue;
            for (int i = 0; i < m; ++i) {
                if (base_lc.matrix[i][cid].cmp(0) != 0)
                    ++nnz;
            }
        }

        if (nnz == 0) {
            return res;
        }

        int c = m;

        int *cwghts = new int[c];
        for (int i = 0; i < c; ++i)
            cwghts[i] = 1;

        int *nwghts = new int[n];
        for (int j = 0; j < n; ++j)
            nwghts[j] = 1;

        int *xpins = new int[n + 1];
        int *pins = new int[nnz];

        int idx = 0;
        for (int j = 0; j < n; ++j) {
            xpins[j] = idx;
            int cid = cids[j];
            if (cid < 0)
                continue;
            for (int i = 0; i < m; ++i) {
                if (base_lc.matrix[i][cid].cmp(0) != 0) {
                    pins[idx++] = i; 
                }
            }
        }
        xpins[n] = idx;

        PaToH_Parameters args;
        PaToH_Initialize_Parameters(&args, PATOH_CONPART, PATOH_SUGPARAM_DEFAULT);
        args._k = 2;

        int *partvec = (int *)std::malloc(sizeof(int) * c);
        int *partweights = (int *)std::malloc(sizeof(int) * args._k * nconst);
        int cut = 0;

        PaToH_Alloc(&args, c, n, nconst, cwghts, nwghts, xpins, pins);
        PaToH_Part(&args,
            c, n, nconst,
            0,
            cwghts, nwghts,
            xpins, pins,
            nullptr,
            partvec, partweights,
            &cut);

        vector<char> isCutLocal(n, 0);
        for (int j = 0; j < n; ++j) {
            int start = xpins[j];
            int end = xpins[j + 1];
            if (start >= end)
                continue;

            int firstPart = partvec[pins[start]];
            bool isCut = false;
            for (int p = start + 1; p < end; ++p) {
                if (partvec[pins[p]] != firstPart) {
                    isCut = true;
                    break;
                }
            }
            if (isCut) {
                isCutLocal[j] = 1;
                res.cut_pids.push_back(vars[j]);
            }
        }

        for (int j = 0; j < n; ++j) {
            if (isCutLocal[j])
                continue;
            int cid = cids[j];
            if (cid < 0)
                continue;

            int side = -1;
            for (int i = 0; i < m; ++i) {
                if (base_lc.matrix[i][cid].cmp(0) != 0) {
                    int p = partvec[i];
                    if (side == -1)
                        side = p;
                    else if (side != p) {
                        side = -2;
                        break;
                    }
                }
            }

            if (side == 0) {
                res.left_pids.push_back(vars[j]);
            }
            else if (side == 1) {
                res.right_pids.push_back(vars[j]);
            }
            else if (side == -2) {
                res.cut_pids.push_back(vars[j]);
            }
            else {
                res.left_pids.push_back(vars[j]);
            }
        }

        std::free(partvec);
        std::free(partweights);
        delete[] cwghts;
        delete[] nwghts;
        delete[] xpins;
        delete[] pins;
        PaToH_Free();

        return res;
    }

    void build_dtree_rec(
        const LinearConstraint &base_lc,
        const unordered_map<int, int> &basePID2cid,
        const vector<int> &vars_root,
        vector<int> &order) {
        if (vars_root.empty())
            return;

        std::queue<vector<int>> q;
        q.push(vars_root);

        while (!q.empty()) {
            vector<int> vars = std::move(q.front());
            q.pop();
            if (vars.empty())
                continue;

            if (vars.size() == 1) {
                order.push_back(vars[0]);
                continue;
            }

            CutResult r = compute_cut_and_partition_on_vars(base_lc, basePID2cid, vars);

            if (r.cut_pids.empty()) {
                for (int pid : vars) {
                    order.push_back(pid);
                }
                continue;
            }

            for (int pid : r.cut_pids) {
                order.push_back(pid);
            }

            vector<int> remain;
            remain.reserve(r.left_pids.size() + r.right_pids.size());
            for (int pid : r.left_pids)
                remain.push_back(pid);
            for (int pid : r.right_pids)
                remain.push_back(pid);

            if (remain.empty()) {
                continue;
            }

            if (!r.left_pids.empty() && !r.right_pids.empty()) {
                q.push(r.left_pids);
                q.push(r.right_pids);
            }
            else {
                q.push(remain);
            }
        }
    }
    void build_dtree(const LinearConstraint &lc) {
        g_dtree_order.clear();

        int n = lc.getN();
        if (n == 0) {
            g_dtree_built = true;
            return;
        }

        unordered_map<int, int> basePID2cid;
        basePID2cid.reserve(n * 2);
        for (int cid = 0; cid < n; ++cid) {
            basePID2cid[lc.PID[cid]] = cid;
        }

        vector<int> vars = lc.PID;
        build_dtree_rec(lc, basePID2cid, vars, g_dtree_order);

        g_dtree_built = true;
    }

}

int Heuristic::select_by_dtree_hp(LinearConstraint &lc) {//SHP
    if (!global_state_enabled)
        return -1;

    int n = lc.getN();
    if (n <= 0) {
        return -1;
    }

    if (!g_dtree_built || g_dtree_order.empty()) {
        build_dtree(lc);
    }

    unordered_map<int, int> pid2cid;
    pid2cid.reserve(n * 2);
    for (int cid = 0; cid < n; ++cid) {
        pid2cid[lc.PID[cid]] = cid;
    }

    int &pos = lc.dtree_pos;
    int order_n = (int)g_dtree_order.size();

    while (pos < order_n) {
        int pid = g_dtree_order[pos];
        ++pos;

        auto it = pid2cid.find(pid);
        if (it != pid2cid.end()) {
            int cid = it->second;
            return cid;
        }
    }

    return -1;
}

void Heuristic::init_dtree_root(LinearConstraint &lc) {
    if (heuristic != BH_dTreeHP || !global_state_enabled)
        return;

    build_dtree(lc);

    lc.dtree_pos = 0;
}

int Heuristic::select_by_random(LinearConstraint &lc) {
    int n = lc.getN();
    if (n <= 0)
        return -1;
    return rand() % n;
}
