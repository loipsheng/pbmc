//Implementation of decomposition function and simplification procedure.

#include "linear_constraint.h"
#include "heuristic.h"

#include <cmath>
#include <cstdint>
#include <ext/pb_ds/assoc_container.hpp>
#include <iomanip>
#include <limits>

vector<Int> weight;

// Definition order matters: static objects are destroyed in reverse order,
// so the cache releases its template references before the pool is destroyed.
RowTemplatePool LinearConstraint::row_template_pool;
ComponentCache LinearConstraint::component_cache;
int LinearConstraint::CD = 0;
int LinearConstraint::CC = 0;
unsigned long long LinearConstraint::cache_lookups = 0;
unsigned long long LinearConstraint::cache_hits = 0;
unsigned long long LinearConstraint::cache_misses = 0;
unsigned long long LinearConstraint::cache_max_age_inserts =
    50ULL * 1024ULL;
int LinearConstraint::cache_matrix_limit = MAX_MATRIX;
vector<Int> LinearConstraint::tmp;

namespace {

ComponentCache::Config CurrentComponentCacheConfig() {
  ComponentCache::Config config;
  config.max_age_inserts = LinearConstraint::cache_max_age_inserts;
  if (LinearConstraint::cache_matrix_limit <= 0)
    config.bucket_count = 1;
  return config;
}

// glp_intopt requires an optimal LP relaxation.  In particular, calling it
// after glp_simplex has proved the relaxation infeasible returns GLP_EROOT;
// that is not an unknown result -- the original MIP is infeasible as well.
// Keep all solver-status handling in one place so LP based simplifications
// never use a stale/undefined MIP status or objective value.
enum class IntegerOptimizationStatus {
  kOptimal,
  kInfeasible,
  kUnknown,
};

struct IntegerOptimizationResult {
  IntegerOptimizationStatus status = IntegerOptimizationStatus::kUnknown;
  double objective = 0.0;
};

IntegerOptimizationResult OptimizeIntegerObjective(
    glp_prob *lp, glp_smcp *lp_parm, glp_iocp *mip_parm) {
  const int simplex_ret = glp_simplex(lp, lp_parm);
  if (simplex_ret == GLP_ENOPFS || simplex_ret == GLP_ENOFEAS)
    return {IntegerOptimizationStatus::kInfeasible, 0.0};
  if (simplex_ret != 0)
    return {};

  const int lp_status = glp_get_status(lp);
  if (lp_status == GLP_NOFEAS)
    return {IntegerOptimizationStatus::kInfeasible, 0.0};
  if (lp_status != GLP_OPT)
    return {};

  const int mip_ret = glp_intopt(lp, mip_parm);
  if (mip_ret == GLP_ENOPFS || mip_ret == GLP_ENOFEAS)
    return {IntegerOptimizationStatus::kInfeasible, 0.0};
  if (mip_ret != 0)
    return {};

  const int mip_status = glp_mip_status(lp);
  if (mip_status == GLP_NOFEAS)
    return {IntegerOptimizationStatus::kInfeasible, 0.0};
  if (mip_status != GLP_OPT)
    return {};

  return {IntegerOptimizationStatus::kOptimal, glp_mip_obj_val(lp)};
}

bool IsExactlyRepresentableAsDouble(const Int &value) {
  const double converted = value.to_double();
  if (!std::isfinite(converted))
    return false;

  Int round_trip;
  mpz_set_d(round_trip.a, converted);
  return round_trip == value;
}

bool IsWithinExactDoubleIntegerRange(const Int &value) {
  static const Int kLargestConsecutiveDoubleInteger("9007199254740992");
  return mpz_cmpabs(value.a, kLargestConsecutiveDoubleInteger.a) <= 0;
}

bool IsGlpkModelExactlyRepresentable(const LinearConstraint &constraint) {
  for (const auto &range : constraint.ranges) {
    if (!IsExactlyRepresentableAsDouble(range.first) ||
        !IsExactlyRepresentableAsDouble(range.second) ||
        !IsWithinExactDoubleIntegerRange(range.first) ||
        !IsWithinExactDoubleIntegerRange(range.second))
      return false;
  }
  for (const auto &row : constraint.matrix) {
    Int maximum_absolute_lhs(0);
    for (int column = 0; column < constraint.getN(); ++column) {
      const Int &coefficient = row[column];
      if (!IsExactlyRepresentableAsDouble(coefficient) ||
          !IsWithinExactDoubleIntegerRange(coefficient))
        return false;

      const Int lower_product = coefficient * constraint.ranges[column].first;
      const Int upper_product = coefficient * constraint.ranges[column].second;
      const Int lower_magnitude = lower_product.cmp(0) < 0
          ? -lower_product : lower_product;
      const Int upper_magnitude = upper_product.cmp(0) < 0
          ? -upper_product : upper_product;
      maximum_absolute_lhs += max(lower_magnitude, upper_magnitude);
      if (!IsWithinExactDoubleIntegerRange(maximum_absolute_lhs))
        return false;
    }
    if (!IsExactlyRepresentableAsDouble(row.back()) ||
        !IsWithinExactDoubleIntegerRange(row.back()))
      return false;
  }
  return true;
}

bool ConvertMipObjectiveToIntegerBound(double objective, bool lower_bound,
                                       Int &result) {
  if (!std::isfinite(objective))
    return false;
  const double rounded = lower_bound
      ? std::ceil(objective - LinearConstraint::EPS)
      : std::floor(objective + LinearConstraint::EPS);
  if (!std::isfinite(rounded) ||
      std::fabs(rounded) > 9007199254740992.0)
    return false;
  mpz_set_d(result.a, rounded);
  return true;
}

}  // namespace

const double LinearConstraint::EPS = 1e-2;

unordered_set<LinearConstraint::Ablation_Func> LinearConstraint::ablation;
int LinearConstraint::variable_limit = VARIABLE_LIMIT;
bool LinearConstraint::sb_iterate_to_fixed_point = true;

size_t LinearConstraint::VIHash::operator()(const vector<Int> &vi) const {
  int res = 0;
  for (const Int &v : vi)
    res = (res * base + v) % mod;
  res = (res + mod) % mod;
  return hash<int>()(res);
}

int LinearConstraint::getN() const { return ranges.size(); }
int LinearConstraint::getM() const { return matrix.size(); }

bool LinearConstraint::hasEmptyDomain() const {
  for (const auto &range : ranges) {
    if (range.first.cmp(range.second) > 0)
      return true;
  }
  return false;
}

int LinearConstraint::read(const string &file_name) {
  return read(file_name, true);
}

int LinearConstraint::read(const string &file_name, bool sort_constraints) {
  ifstream infile(file_name);

  if (!infile.is_open())
    return 0;

  int n, m;
  infile >> n >> m;

  ranges.clear();
  matrix.clear();
  simplification_stats = SimplificationStats{};
  ranges.resize(n);
  matrix.resize(m, vector<Int>(n + 1));
  for (int i = 0; i < n; i++)
    infile >> ranges[i].first >> ranges[i].second;
  for (int i = 0; i < m; i++) {
    for (int j = 0; j < n + 1; j++)
      infile >> matrix[i][j];
  }
  if (sort_constraints)
    sort(matrix.begin(), matrix.end());
  infile.close();

  return 1;
}

void LinearConstraint::read(istream &infile) {
  read(infile, true);
}

void LinearConstraint::read(istream &infile, bool sort_constraints) {
  int n, m;
  infile >> n >> m;

  ranges.clear();
  matrix.clear();
  simplification_stats = SimplificationStats{};
  ranges.resize(n);
  matrix.resize(m, vector<Int>(n + 1));
  for (int i = 0; i < n; i++)
    infile >> ranges[i].first >> ranges[i].second;
  for (int i = 0; i < m; i++) {
    for (int j = 0; j < n + 1; j++)
      infile >> matrix[i][j];
  }
  if (sort_constraints)
    sort(matrix.begin(), matrix.end());
}

void LinearConstraint::output() const {
  output(cerr);
}

void LinearConstraint::output(ostream &out) const {
  int n = getN(), m = getM();
  out << n << ' ' << m << '\n';
  for (int i = 0; i < n; i++)
    out << ranges[i].first << ' ' << ranges[i].second << '\n';
  for (int i = 0; i < m; i++) {
    for (int j = 0; j < n + 1; j++)
      out << matrix[i][j] << (j == n ? '\n' : ' ');
  }
}

int LinearConstraint::size() { return getN() * getM(); }

Int LinearConstraint::getRange(int id) {
  if ((int)ranges.size() <= id)
    return 0;
  if (ranges[id].first.cmp(ranges[id].second) > 0)
    return 0;
  return ranges[id].second - ranges[id].first + 1;
}

Int LinearConstraint::getSpace() {
  int n = getN();
  Int res = 1;
  for (int i = 0; i < n; i++) {
    res *= getRange(i);
  }
  return res;
}

int LinearConstraint::select() {
  int id = -1;
  int n = getN();
  Int mn;
  for (int i = 0; i < n; i++) {
    if (id == -1 || getRange(i).cmp(mn) < 0) {
      mn = getRange(i);
      id = i;
    }
  }
  assert(mn.cmp(0) > 0);
  assert(~id);
  return id;
}

int getId(vector<int> &pa, int x) {
  return x ^ pa[x] ? pa[x] = getId(pa, pa[x]) : x;
}

vector<LinearConstraint> LinearConstraint::decompose() {
  int n = getN();

  int m = getM();
  vector<int> pa(n), num(n);
  for (int i = 0; i < n; i++)
    pa[i] = i;
  for (const auto &v : matrix) {
    int pos = -1;
    for (int i = 0; i < n; i++) {
      if (!v[i].cmp(0))
        continue;
      if (pos == -1)
        pos = i;
      else
        pa[getId(pa, i)] = getId(pa, pos);
    }
  }

  int tot = 0;
  __gnu_pbds::gp_hash_table<int, int> mp;
  // unordered_map<int, int> mp;
  for (int i = 0; i < n; i++) {
    getId(pa, i);
    if (mp.find(pa[i]) == mp.end())
      mp[pa[i]] = tot++;
  }

  if (tot < 2) {
    assert(tot == 1);
    return vector<LinearConstraint>();
  }

  vector<LinearConstraint> res(tot);
  for (int i = 0; i < n; i++) {
    LinearConstraint &t = res[mp[pa[i]]];
    num[i] = t.getN();
    t.ranges.emplace_back(ranges[i]);
    if (Heuristic::global_state_enabled) {
      t.PID.emplace_back(PID[i]);
    }
  }

  for (int i = 0; i < m; i++) {
    int id = -1;
    for (int j = 0; j < n; j++) {
      if (matrix[i][j] == 0)
        continue;
      if (id == -1) {
        id = mp[pa[j]];
        res[id].matrix.emplace_back(vector<Int>(res[id].getN() + 1, 0));
        if (Heuristic::global_state_enabled)
          res[id].mPID.emplace_back(mPID[i]);
      }
      res[id].matrix.back()[num[j]] = matrix[i][j];
    }
    // A constant row does not connect variables, but it still belongs to the
    // formula.  Dropping it is unsound when it is the contradiction 0 <= b,
    // b < 0.  RIR/RIR-LP normally consume constant rows during simplify();
    // assigning one defensively to a component keeps decompose() sound even
    // if an external optimizer declines to classify a numerical model.
    if (id == -1) {
      id = 0;
      res[id].matrix.emplace_back(vector<Int>(res[id].getN() + 1, 0));
      if (Heuristic::global_state_enabled)
        res[id].mPID.emplace_back(mPID[i]);
    }
    res[id].matrix.back().back() = matrix[i].back();
  }
  if (Heuristic::global_state_enabled) {
    for (int c = 0; c < tot; ++c) {
      res[c].kbest = this->kbest;
      res[c].dtree_pos = this->dtree_pos;
    }
  }
  return res;
}

Int LinearConstraint::simplify(int level) {
#if 0
  // Legacy iterative framework (formerly selected by the removed CLI flag).
  // It is intentionally kept here for reference, but is no longer compiled.
  tmp.resize(getM());

  Int legacy_t;
  int legacy_tt;
  bool tag, legacy_ctag = false, vis = true;
  do {
    tag = removeFixedVariables();

    legacy_t = redundancyDetectionSingle();
    if (legacy_t.cmp(0) >= 0)
      return legacy_t;
    else if (!legacy_t.cmp(-1))
      tag = legacy_ctag = true;

    legacy_tt = boundStrengtheningSingle();
    if (!legacy_tt)
      return 0;
    else if (legacy_tt == -1)
      tag = true;

    if (vis && coefficientStrengtheningSingle())
      tag = true;

    if (vis && redundancyDetectionSubset())
      tag = legacy_ctag = true;

    if (vis) {
      legacy_tt = parallelRows();
      if (!legacy_tt)
        return 0;
      else if (legacy_tt == -1)
        tag = legacy_ctag = true;
    }

    if (vis && level <= lp_level_limit) {
      legacy_t = LP(kLPBoth);
      if (legacy_t.cmp(0) >= 0)
        return legacy_t;
      else if (!legacy_t.cmp(-1))
        tag = legacy_ctag = true;
    }
    vis = false;
  } while (tag);

  if (legacy_ctag)
    sortMatrix();
  return -1;
#endif

  (void)level;

  // M = empty: every assignment in the current domains is a model.
  if (getM() == 0)
    return getSpace();

  Int t;
  int tt;
  bool changed;

  // StrengthenBound / StrengthenBoundLP.
  if (getN() <= variable_limit) {
    do {
      changed = false;
      tt = boundStrengtheningSingle();
      if (!tt)
        return 0;
      else if (tt == -1)
        changed = true;
    } while (sb_iterate_to_fixed_point && changed);
  }
  else {
    t = LP(kLPBoundOnly);
    if (t.cmp(0) >= 0)
      return t;
  }

  // RemoveVariable, then StrengthenCoefficient.
  bool ctag = false;
  if (removeFixedVariables())
    ctag = true;

  if (coefficientStrengtheningSingle())
    ctag = true;

  // RemoveIndividualRow / RemoveIndividualRowLP.  RIR is the mandatory
  // fallback for this stage: small components use interval-based RIR, while
  // large components use RIR-LP only when that optional enhancement is
  // enabled.  Disabling RIR-LP must not leave the row-removal stage empty.
  const bool rir_lp_disabled = ablation.count(kRedundancyDetectionLP);
  if (getN() <= variable_limit || rir_lp_disabled)
    t = redundancyDetectionSingle();
  else
    t = LP(kLPRedundancyOnly);
  if (t.cmp(0) >= 0)
    return t;
  else if (!t.cmp(-1))
    ctag = true;

  // RemoveParallelRow first: its GCD-based row normalization can make shared
  // coefficients identical and thereby expose additional subset relations.
  tt = parallelRows();
  if (!tt)
    return 0;
  else if (tt == -1)
    ctag = true;

  // RemoveSubsetRow consumes the normalized rows produced above.
  if (redundancyDetectionSubset())
    ctag = true;

  // The pseudocode returns Phi here.  In this implementation an empty Phi
  // is represented by its direct model count rather than the -1 sentinel.
  if (getM() == 0)
    return getSpace();

  if (ctag)
    sortMatrix();
  return -1;
}

Int LinearConstraint::redundancyDetectionSingle() { // RIR
  bool tag = false;
  int n = getN(), m = getM();
  for (int i = 0; i < m; i++) {
    Int mx = 0, mn = 0;
    for (int j = 0; j < n; j++) {
      const Int &v = matrix[i][j];
      if (v == 0)
        continue;
      mx += max(v * ranges[j].first, v * ranges[j].second);
      mn += min(v * ranges[j].first, v * ranges[j].second);
    }
    if (matrix[i].back() < mn) {
      if (Heuristic::global_state_enabled) {
        if (!mPID.empty()) {
          int id = mPID[i];
          if (id >= 0 && id < (int)weight.size())
            weight[id]++;
        }
      }
      return 0;
    }

    if (mx.cmp(matrix[i].back()) <= 0) {
      swap(matrix[i], matrix.back());
      if (Heuristic::global_state_enabled && !mPID.empty())
        swap(mPID[i], mPID.back());
      matrix.pop_back();
      simplification_stats.rr_constraints_removed++;
      if (Heuristic::global_state_enabled && !mPID.empty())
        mPID.pop_back();
      tag = true;
      m--;
      i--;
    }
  }

  if (matrix.empty()) {
    Int res(1);
    for (int i = 0; i < n; i++)
      res *= getRange(i);
    return res;
  }

  return tag - 2;
}

bool LinearConstraint::deleteVariable(int id) {
  if (id < 0 || id >= getN())
    return false;
  if (getRange(id) != 1)
    return false;
  for (auto &vec : matrix) {
    vec.back() -= vec[id] * ranges[id].first;
    vec.erase(vec.begin() + id);
  }
  if (Heuristic::global_state_enabled && !PID.empty())
    PID.erase(PID.begin() + id);
  ranges.erase(ranges.begin() + id);
  return true;
}

bool LinearConstraint::removeFixedVariables() {//RV
  if (ablation.count(kRemoveFixedVariables))
    return false;

  Int val;
  bool tag = false;
  for (int i = 0; i < getN(); i++) {
    if (deleteVariable(i)) {
      simplification_stats.rv_variables_removed++;
      tag = true;
      i--;
    }
  }
  return tag;
}

int LinearConstraint::boundStrengtheningSingle() {//SB
  if (ablation.count(kBoundStrengtheningSingle))
    return -2;

  int n = getN(), m = getM();
  vector<Int> tmp(m);
  tmp.assign(m, 0);
  for (int i = 0; i < m; i++) {
    tmp[i] = 0;
    for (int j = 0; j < n; j++) {
      const Int &v = matrix[i][j];
      if (v == 0)
        continue;
      tmp[i] += min(v * ranges[j].first, v * ranges[j].second);
    }
  }

  Int val;
  bool tag = false;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < m; j++) {
      const Int &v = matrix[j][i];
      if (!v.cmp(0))
        continue;
      if (v.cmp(0) > 0) {
        val = matrix[j].back() - tmp[j] + v * ranges[i].first;

        mpz_fdiv_q(val.a, val.a, v.a);
        if (val.cmp(ranges[i].first) < 0) {
          if (Heuristic::global_state_enabled) {
            if (!mPID.empty()) {
              int id = mPID[j];
              if (id >= 0 && id < (int)weight.size())
                weight[id]++;
            }
          }
          return 0;
        }
        if (val.cmp(ranges[i].second) < 0) {
          tag = true;
          simplification_stats.domain_values_removed += ranges[i].second - val;
          ranges[i].second = val;
          simplification_stats.sb_bound_updates++;
        }
      }
      else {
        val = -matrix[j].back() + tmp[j] - v * ranges[i].second;

        mpz_cdiv_q(val.a, val.a, (-v).a);
        if (val.cmp(ranges[i].second) > 0) {
          if (Heuristic::global_state_enabled) {
            if (!mPID.empty()) {
              int id = mPID[j];
              if (id >= 0 && id < (int)weight.size())
                weight[id]++;
            }
          }
          return 0;
        }
        if (val.cmp(ranges[i].first) > 0) {
          tag = true;
          simplification_stats.domain_values_removed += val - ranges[i].first;
          ranges[i].first = val;
          simplification_stats.sb_bound_updates++;
        }
      }
    }
  }

  return tag - 2;
}

bool LinearConstraint::coefficientStrengtheningSingle() {//SC
  if (ablation.count(kCoefficientStrengtheningSingle))
    return 0;

  Int mx, u, t, d;
  bool tag = false;
  int n = getN();
  for (auto &v : matrix) {
    mx = 0;
    int num = 0;
    for (int i = 0; i < n; i++) {
      if (v[i] != 0) {
        num++;
        mx += max(v[i] * ranges[i].first, v[i] * ranges[i].second);
      }
    }

    if (num < 2)
      continue;

    for (int i = 0; i < n; i++) {
      if (v[i] == 0)
        continue;

      if (v[i].cmp(0) > 0) {
        u = mx - v[i] * ranges[i].second;
        d = v.back() - u - v[i] * (ranges[i].second - 1);
        if (v[i].cmp(d) >= 0 && d.cmp(0) > 0) {
          mx -= d * ranges[i].second;
          v[i] -= d;
          v.back() -= d * ranges[i].second;
          simplification_stats.sc_coefficient_updates++;
          tag = true;
        }
      }
      else {
        u = mx - v[i] * ranges[i].first;
        d = v.back() - u - v[i] * (ranges[i].first + 1);
        if ((-v[i]).cmp(d) >= 0 && d.cmp(0) > 0) {
          mx += d * ranges[i].first;
          v[i] += d;
          v.back() += d * ranges[i].first;
          simplification_stats.sc_coefficient_updates++;
          tag = true;
        }
      }

      if (v[i] == 0 && --num < 2)
        break;
    }
  }

  return tag;
}

bool LinearConstraint::redundancyDetectionSubset() {//RSR
  if (ablation.count(kRedundancyDetectionSubset))
    return 0;

  bool tag = false;
  int m = getM(), n = getN();
  for (int i = 0; i < m; i++) {
    for (int j = 0; j < m; j++) {
      if (i == j)
        continue;

      Int mn = 0;
      bool flag = true;
      for (int k = 0; k < n; k++) {
        const Int &v1 = matrix[i][k];
        const Int &v2 = matrix[j][k];
        if (v1 == 0)
          mn += min(v2 * ranges[k].first, v2 * ranges[k].second);
        else if (v1 != v2) {
          flag = false;
          break;
        }
      }

      if (flag && mn.cmp(matrix[j].back() - matrix[i].back()) >= 0) {
        tag = true;
        swap(matrix[i], matrix.back());
        if (Heuristic::global_state_enabled && !mPID.empty())
          swap(mPID[i], mPID.back());
        matrix.pop_back();
        simplification_stats.sr_constraints_removed++;
        if (Heuristic::global_state_enabled && !mPID.empty())
          mPID.pop_back();
        i--;
        m--;
        break;
      }
    }
  }

  return tag;
}

int LinearConstraint::parallelRows() {//RPR
  if (ablation.count(kParallelRows))
    return -2;

  bool changed = false;
  int m = getM(), n = getN();
  vector<bool> remove(m, false);
  unordered_map<vector<Int>, int, VIHash> direction_to_row;

  for (int i = 0; i < m; i++) {
    LeqRowNormalizationResult normalized = NormalizeLeqRow(matrix[i]);
    if (normalized.status == LeqRowStatus::kContradiction) {
      if (Heuristic::global_state_enabled && !mPID.empty()) {
        int id = mPID[i];
        if (id >= 0 && id < (int)weight.size())
          weight[id]++;
      }
      return 0;
    }
    if (normalized.status == LeqRowStatus::kTautology) {
      remove[i] = true;
      changed = true;
      continue;
    }

    if (normalized.changed) {
      matrix[i] = std::move(normalized.row);
      changed = true;
    }

    vector<Int> direction(matrix[i].begin(), matrix[i].begin() + n);
    auto inserted = direction_to_row.emplace(direction, i);
    if (!inserted.second) {
      int previous = inserted.first->second;
      if (matrix[i].back() < matrix[previous].back()) {
        remove[previous] = true;
        inserted.first->second = i;
      }
      else {
        remove[i] = true;
      }
      changed = true;
    }
  }

  // For primitive opposite rows a*x <= upper and -a*x <= opposite_rhs,
  // integer feasibility requires -opposite_rhs <= upper.
  for (const auto &entry : direction_to_row) {
    const int row_id = entry.second;
    vector<Int> opposite = entry.first;
    for (Int &coefficient : opposite)
      coefficient = -coefficient;

    auto opposite_it = direction_to_row.find(opposite);
    if (opposite_it == direction_to_row.end())
      continue;

    const int opposite_id = opposite_it->second;
    if ((-matrix[opposite_id].back()).cmp(matrix[row_id].back()) > 0) {
      if (Heuristic::global_state_enabled && !mPID.empty()) {
        int id1 = mPID[row_id];
        int id2 = mPID[opposite_id];
        if (id1 >= 0 && id1 < (int)weight.size())
          weight[id1]++;
        if (id2 >= 0 && id2 < (int)weight.size())
          weight[id2]++;
      }
      return 0;
    }
  }

  if (changed) {
    vector<vector<Int>> kept_rows;
    kept_rows.reserve(m);
    vector<int> kept_mpid;
    if (Heuristic::global_state_enabled && !mPID.empty())
      kept_mpid.reserve(m);

    for (int i = 0; i < m; ++i) {
      if (remove[i]) {
        simplification_stats.pr_constraints_removed++;
        continue;
      }
      kept_rows.emplace_back(std::move(matrix[i]));
      if (Heuristic::global_state_enabled && !mPID.empty())
        kept_mpid.emplace_back(mPID[i]);
    }

    matrix.swap(kept_rows);
    if (Heuristic::global_state_enabled && !mPID.empty())
      mPID.swap(kept_mpid);
  }

  return changed ? -1 : -2;
}

Int LinearConstraint::LP(int mode) {
  const bool ablate_bs = ablation.count(kBoundStrengtheningLP);
  const bool ablate_rd = ablation.count(kRedundancyDetectionLP);

  const bool do_bs = (mode & kLPBoundOnly) && !ablate_bs;
  const bool do_rd = (mode & kLPRedundancyOnly) && !ablate_rd;
  if (!do_bs && !do_rd)
    return -2;

  int n = getN();
  int m = getM();

  if (m <= 0)
    return getSpace();

  // Constant rows are an exact logical base case, not a numerical LP task.
  // Handle them before constructing GLPK's double-precision model so a
  // contradiction cannot be hidden by an unrelated unrepresentable value.
  bool constant_rows_removed = false;
  if (do_rd) {
    int write = 0;
    for (int read = 0; read < m; ++read) {
      bool constant = true;
      for (int col = 0; col < n; ++col) {
        if (matrix[read][col] != 0) {
          constant = false;
          break;
        }
      }
      if (!constant) {
        if (write != read) {
          swap(matrix[write], matrix[read]);
          if (Heuristic::global_state_enabled && !mPID.empty())
            swap(mPID[write], mPID[read]);
        }
        ++write;
        continue;
      }
      if (matrix[read].back().cmp(0) < 0)
        return 0;
      simplification_stats.rr_lp_constraints_removed++;
      constant_rows_removed = true;
    }
    if (write != m) {
      matrix.erase(matrix.begin() + write, matrix.end());
      if (Heuristic::global_state_enabled && !mPID.empty())
        mPID.erase(mPID.begin() + write, mPID.end());
      m = write;
      if (m == 0)
        return getSpace();
    }
  }

  if (n <= 0) {
    for (int i = 0; i < m; i++) {
      if (matrix[i].back().cmp(0) < 0)
        return 0;
    }
    return 1;
  }

  // GLPK stores the model as IEEE-754 doubles.  Rounding a large integer can
  // change an inequality (several Application rows differ only in the low
  // bits around 2^64), which previously allowed RIR-LP to delete a necessary
  // row.  Unknown/no-simplification is the only sound result when the exact
  // integer model cannot be represented; the DPLL core remains exact.
  if (!IsGlpkModelExactlyRepresentable(*this))
    return constant_rows_removed ? -1 : -2;

  glp_term_out(GLP_OFF);

  // Initialize LP
  glp_prob *lp;
  lp = glp_create_prob();

  glp_add_rows(lp, m);
  for (int i = 0; i < m; i++) {
    glp_set_row_bnds(lp, i + 1, GLP_UP, 0, matrix[i].back().to_double());
  }

  glp_add_cols(lp, n);
  for (int i = 0; i < n; i++) {
    glp_set_col_bnds(lp, i + 1, GLP_DB, ranges[i].first.to_double(), ranges[i].second.to_double());
    glp_set_col_kind(lp, i + 1, GLP_IV);
  }

  int *iA = new int[m * n + 1];
  int *jA = new int[m * n + 1];
  double *vA = new double[m * n + 1];
  int idx = 1;
  for (int i = 0; i < m; i++) {
    for (int j = 0; j < n; j++) {
      iA[idx] = i + 1;
      jA[idx] = j + 1;
      vA[idx++] = matrix[i][j].to_double();
    }
  }

  glp_load_matrix(lp, m * n, iA, jA, vA);
  delete[] iA;
  delete[] jA;
  delete[] vA;

  glp_smcp lp_parm;
  glp_init_smcp(&lp_parm);
  lp_parm.msg_lev = GLP_MSG_ERR;

  glp_iocp mip_parm;
  glp_init_iocp(&mip_parm);
  mip_parm.msg_lev = GLP_MSG_ERR;

  int ret = -2;

  if (do_bs) {
    ret = boundStrengtheningLP(lp, &lp_parm, &mip_parm);
  }
  if (ret == 0) {
    for (int r = 0; r < getM(); r++)
      if (Heuristic::global_state_enabled) {
        if (!mPID.empty()) {
          int id = mPID[r];
          if (id >= 0 && id < (int)weight.size())
            weight[id]++;
        }
      }

    glp_delete_prob(lp);
    return 0;
  }

  if (do_rd) {
    Int sol = redundancyDetectionLP(lp, &lp_parm, &mip_parm);
    glp_delete_prob(lp);
    if (!do_bs) {
      if (constant_rows_removed && sol.cmp(-2) == 0)
        return -1;
      return sol;
    }
    if (constant_rows_removed || !sol.cmp(-1) || ret == -1)
      return -1;
    if (sol.cmp(0) >= 0)
      return sol;
    return ret;
  }

  if (do_bs) {
    glp_delete_prob(lp);
    return ret;
  }

  glp_delete_prob(lp);
  return -2;
}

int LinearConstraint::boundStrengtheningLP(glp_prob *lp, glp_smcp *lp_parm, glp_iocp *mip_parm) {//SB-LP
  if (ablation.count(kBoundStrengtheningLP))
    return -2;

  int n = getN();

  if (n <= 0)
    return -2;

  Int l, u;
  bool tag = false;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < n; j++) {
      glp_set_obj_coef(lp, j + 1, i == j);
    }

    glp_set_obj_dir(lp, GLP_MAX);
    IntegerOptimizationResult result =
        OptimizeIntegerObjective(lp, lp_parm, mip_parm);
    if (result.status == IntegerOptimizationStatus::kInfeasible) {
      return 0;
    }
    if (result.status == IntegerOptimizationStatus::kOptimal &&
        ConvertMipObjectiveToIntegerBound(result.objective, false, u)) {
      if (ranges[i].second.cmp(u) > 0) {
        simplification_stats.domain_values_removed += ranges[i].second - u;
        ranges[i].second = u;
        tag = true;
        simplification_stats.sb_lp_bound_updates++;
      }
    }

    glp_set_obj_dir(lp, GLP_MIN);
    result = OptimizeIntegerObjective(lp, lp_parm, mip_parm);
    if (result.status == IntegerOptimizationStatus::kInfeasible) {
      return 0;
    }
    if (result.status == IntegerOptimizationStatus::kOptimal &&
        ConvertMipObjectiveToIntegerBound(result.objective, true, l)) {
      if (ranges[i].first.cmp(l) < 0) {
        simplification_stats.domain_values_removed += l - ranges[i].first;
        ranges[i].first = l;
        tag = true;
        simplification_stats.sb_lp_bound_updates++;
      }
    }

    if (ranges[i].first.cmp(ranges[i].second) > 0) { // lower > upper
      return 0;
    }

    glp_set_col_bnds(lp, i + 1, GLP_DB, ranges[i].first.to_double(), ranges[i].second.to_double());
  }

  return tag - 2;
}

Int LinearConstraint::redundancyDetectionLP(glp_prob *lp, glp_smcp *lp_parm, glp_iocp *mip_parm) {//RIR-LP
  if (ablation.count(kRedundancyDetectionLP))
    return -2;

  int m = getM();
  int n = getN();

  if (m <= 0)
    return getSpace();

  Int l, u;
  unordered_set<int> removeIdx;
  bool tag = false;

  for (int i = 0; i < m; i++) {
    if (removeIdx.count(i) > 0)
      continue;

    glp_set_row_bnds(lp, i + 1, GLP_FR, 0, 0);

    bool constant_row = true;
    for (int j = 0; j < n; ++j) {
      if (matrix[i][j] != 0) {
        constant_row = false;
        break;
      }
    }
    if (constant_row) {
      if (matrix[i].back().cmp(0) < 0)
        return 0;
      removeIdx.insert(i);
      tag = true;
      continue;
    }

    for (int j = 0; j < n; j++) {
      glp_set_obj_coef(lp, j + 1, matrix[i][j].to_double());
    }

    bool can_remove = false;

    // RIR-LP must be able to replace RIR completely.  The minimum detects
    // an inconsistent row, while the maximum proves that a row is implied by
    // the remaining system.  If the remaining system itself is infeasible,
    // then the full system is infeasible as well.
    glp_set_obj_dir(lp, GLP_MIN);
    IntegerOptimizationResult result =
        OptimizeIntegerObjective(lp, lp_parm, mip_parm);
    if (result.status == IntegerOptimizationStatus::kInfeasible) {
      return 0;
    }
    if (result.status == IntegerOptimizationStatus::kOptimal &&
        ConvertMipObjectiveToIntegerBound(result.objective, true, l)) {
      if (l.cmp(matrix[i].back()) > 0)
        return 0;
    }

    glp_set_obj_dir(lp, GLP_MAX);
    result = OptimizeIntegerObjective(lp, lp_parm, mip_parm);
    if (result.status == IntegerOptimizationStatus::kInfeasible) {
      return 0;
    }
    if (result.status == IntegerOptimizationStatus::kOptimal &&
        ConvertMipObjectiveToIntegerBound(result.objective, false, u)) {
      if (u.cmp(matrix[i].back()) <= 0) { // i-th constraint can be removed
        can_remove = true;
      }
    }

    if (can_remove) {
      removeIdx.insert(i);
      tag = true;
    }
    else {
      glp_set_row_bnds(lp, i + 1, GLP_UP, 0, matrix[i].back().to_double());
    }
  }

  int idx = 0;
  for (int i = 0; i < m; i++) {
    if (removeIdx.count(i) > 0)
      continue;
    swap(matrix[idx++], matrix[i]);
    if (Heuristic::global_state_enabled && !mPID.empty())
      swap(mPID[idx - 1], mPID[i]);
  }
  matrix.erase(matrix.begin() + idx, matrix.end());
  simplification_stats.rr_lp_constraints_removed += removeIdx.size();
  if (Heuristic::global_state_enabled && !mPID.empty())
    mPID.erase(mPID.begin() + idx, mPID.end());

  if (matrix.empty())
    return getSpace();
  return tag - 2;
}

Int LinearConstraint::free_variable() {
  int n = getN();
  Int res(1);
  for (int i = 0; i < n; i++) {
    bool tag = true;
    for (const auto &v : matrix) {
      if (v[i].cmp(0)) {
        tag = false;
        break;
      }
    }
    if (tag) {
      assert(ranges[i].first.cmp(ranges[i].second) <= 0);
      res *= getRange(i);
      ranges.erase(ranges.begin() + i);
      if (Heuristic::global_state_enabled && !PID.empty())
        PID.erase(PID.begin() + i);
      for (auto &v : matrix)
        v.erase(v.begin() + i);
      n--;
      i--;
    }
  }
  return res;
}

bool LinearConstraint::operator==(const LinearConstraint &lc) const {
  return ranges == lc.ranges && matrix == lc.matrix;
}

void LinearConstraint::insertCache(const Int &v) {
  const uint64_t cells = static_cast<uint64_t>(getN()) *
      static_cast<uint64_t>(getM());

  if (cache_matrix_limit <= 0)
    return;
  if (cells > static_cast<uint64_t>(cache_matrix_limit))
    return;

  ComponentKeyBuildResult build =
      ComponentKeyBuilder::buildForInsert(*this, row_template_pool);
  if (build.status == CacheKeyBuildStatus::kContradiction)
    return;
  if (build.status == CacheKeyBuildStatus::kInvalid)
    return;

  component_cache.insert(std::move(build.key), v, true);
}

Int LinearConstraint::findCache() {
  const uint64_t cells = static_cast<uint64_t>(getN()) *
      static_cast<uint64_t>(getM());

  if (cache_matrix_limit <= 0)
    return Int(-1);
  if (cells > static_cast<uint64_t>(cache_matrix_limit))
    return Int(-1);

  const Int *cached_value = nullptr;
  Int cached_result;
  ComponentKeyBuildResult build =
      ComponentKeyBuilder::buildForLookup(*this, row_template_pool);
  if (build.status == CacheKeyBuildStatus::kContradiction)
    return 0;
  if (build.status == CacheKeyBuildStatus::kInvalid)
    return Int(-1);

  ++cache_lookups;

  if (!build.template_missing && component_cache.find(build.key, cached_result))
    cached_value = &cached_result;

  if (cached_value == nullptr) {
    ++cache_misses;
    return Int(-1);
  }

  ++cache_hits;
  return *cached_value;
}

void LinearConstraint::resetCacheStats() {
  cache_lookups = 0;
  cache_hits = 0;
  cache_misses = 0;
  row_template_pool.resetStatistics();
  component_cache.resetStatistics();
}

double LinearConstraint::cacheHitRate() {
  if (cache_lookups == 0)
    return 0.0;
  return static_cast<double>(cache_hits) /
      static_cast<double>(cache_lookups);
}

unsigned long long LinearConstraint::cacheCleanLimitEntries() {
  return ComponentCache::cleanLimitForAge(cache_max_age_inserts);
}

void LinearConstraint::printCacheStats(ostream &out) {
  const ComponentCache::Statistics &cache_stats =
      component_cache.statistics();
  const RowTemplatePool::Statistics &template_stats =
      row_template_pool.statistics();

  out << "Cache lookups: " << cache_lookups << '\n';
  out << "Cache hits: " << cache_hits << '\n';
  out << "Cache misses: " << cache_misses << '\n';
  out << "Cache hit rate: ";
  if (cache_lookups == 0)
    out << "N/A\n";
  else
    out << fixed << setprecision(2) << (cacheHitRate() * 100.0)
        << '%' << defaultfloat << '\n';

  out << "Peak number of cache entries: " << cache_stats.entries_peak << '\n';
  out << "Number of cache cleanup: "
      << cache_stats.cleanup_runs << '\n';
  out << "Number of deleted entries: " << cache_stats.deleted_entries << '\n';

  out << "Row template requests: " << template_stats.intern_requests << '\n';
  out << "Row template reuses: " << template_stats.intern_reuses << '\n';
  out << "Row template reuse rate: ";
  if (template_stats.intern_requests == 0)
    out << "N/A\n";
  else
    out << fixed << setprecision(2) << (row_template_pool.reuseRate() * 100.0)
        << '%' << defaultfloat << '\n';
}
void LinearConstraint::clearCachesForTest() {
  // First detach using the old pool so every old entry releases its refs.
  component_cache.configure(CurrentComponentCacheConfig(), nullptr);
  row_template_pool.configure(1);
  component_cache.configure(
      CurrentComponentCacheConfig(),
      cache_matrix_limit > 0 ? &row_template_pool : nullptr);
  resetCacheStats();
}

void LinearConstraint::initializeCacheForInstance() {
  component_cache.configure(CurrentComponentCacheConfig(), nullptr);
  row_template_pool.configure(1);
  component_cache.configure(
      CurrentComponentCacheConfig(),
      cache_matrix_limit > 0 ? &row_template_pool : nullptr);
  resetCacheStats();
}

void LinearConstraint::initPID() {
  int n = getN();
  int m = getM();
  PID.clear();
  mPID.clear();
  weight.clear();

  if (!Heuristic::global_state_enabled)
    return;

  PID.resize(n, 0);
  for (int i = 0; i < n; i++) {
    PID[i] = i;
  }

  weight.resize(m, Int(1));
  mPID.resize(m, 0);
  for (int i = 0; i < m; i++) {
    mPID[i] = i;
  }
}

int LinearConstraint::domWdeg_Heur() {
  if (!Heuristic::global_state_enabled)
    return -1;

  int n = getN();
  int m = getM();
  int n_cid = -1;
  double min = numeric_limits<double>::max();
  domWdegs.clear();
  domWdegs.resize(n);
  vector<Int> wdegs;
  wdegs.resize(n, Int(0));

  vector<int> forBack;
  for (int i = 0; i < m; i++) {
    forBack.clear();
    for (int j = 0; j < n; j++) {
      if (matrix[i][j] == 0)
        continue;
      wdegs[j] += weight[mPID[i]];
      forBack.emplace_back(j);
    }
    if (forBack.size() > 1)
      continue;
    for (int b : forBack)
      wdegs[b] -= weight[mPID[i]];
  }

  for (int i = 0; i < n; i++) {
    domWdegs[i] = getRange(i).to_double() / wdegs[i].to_double();
    if (domWdegs[i] < min) {
      min = domWdegs[i];
      n_cid = i;
    }
  }

  return n_cid;
}

void LinearConstraint::sortMatrix() {
  int m = getM();

  vector<int> order(m);
  for (int i = 0; i < m; i++)
    order[i] = i;
  sort(order.begin(), order.end(),
    [&](int a, int b) {
      return matrix[a] < matrix[b];
    });

  vector<vector<Int>> newMatrix(m);
  for (int i = 0; i < m; i++) {
    newMatrix[i] = matrix[order[i]];
  }

  matrix.swap(newMatrix);
  if (Heuristic::global_state_enabled) {
    vector<int> newPID(m);
    for (int i = 0; i < m; i++)
      newPID[i] = mPID[order[i]];
    mPID.swap(newPID);
  }
}
