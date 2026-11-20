#ifndef __LINEAR_CONSTRAINT_H__
#define __LINEAR_CONSTRAINT_H__

#include <assert.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <iostream>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>
#include <glpk.h>

#include "big_int.h"
#include "cache.h"

#define VARIABLE_LIMIT 14
#define MAX_MATRIX 20

using namespace std;

/*
struct constraint{
    vector<Int> row;
    int id;
};
*/

class LinearConstraint {
public:
  struct SimplificationStats {
    long long sb_bound_updates = 0;
    long long sb_lp_bound_updates = 0;
    Int domain_values_removed = Int(0);
    long long rv_variables_removed = 0;
    long long sc_coefficient_updates = 0;
    long long sr_constraints_removed = 0;
    long long pr_constraints_removed = 0;
    long long rr_lp_constraints_removed = 0;
    long long rr_constraints_removed = 0;
  };

  vector<vector<Int>> matrix; // coefficient matrix
  // vector<constraint> matrix;
  vector<pair<Int, Int>> ranges; // variable value range
  vector<int> PID;
  vector<int> mPID;
  vector<int> kbest;
  vector<double> domWdegs;
  int dtree_pos = 0;
  SimplificationStats simplification_stats;

  struct VIHash {
    static const int mod = 1000173169;
    static const long long base = 13331;
    size_t operator()(const vector<Int> &vi) const;
  };

  static RowTemplatePool row_template_pool;
  static ComponentCache component_cache;
  static int CD, CC;
  static unsigned long long cache_lookups;
  static unsigned long long cache_hits;
  static unsigned long long cache_misses;

  static unsigned long long cache_max_age_inserts;
  static int cache_matrix_limit;
  static vector<Int> tmp;
  const static double EPS;

  void initPID();
  int domWdeg_Heur();
  void sortMatrix();

  int read(const string &file_name);
  int read(const string &file_name, bool sort_constraints);
  void read(istream &infile);
  void read(istream &infile, bool sort_constraints);
  void output() const;
  void output(ostream &out) const;

  int getN() const;
  int getM() const;
  bool hasEmptyDomain() const;
  int size();
  Int getRange(int id);
  Int getSpace();
  int select();
  vector<LinearConstraint> decompose();
  Int simplify(int level = 1);

  Int redundancyDetectionSingle();
  bool deleteVariable(int id);
  bool removeFixedVariables();
  int boundStrengtheningSingle();
  bool coefficientStrengtheningSingle();
  bool redundancyDetectionSubset();
  int parallelRows();

  enum LPMode {
    kLPBoundOnly = 1,
    kLPRedundancyOnly = 2,
    kLPBoth = 3,
  };

  Int LP(int mode = kLPBoth);
  int boundStrengtheningLP(glp_prob *lp, glp_smcp *lp_parm, glp_iocp *mip_parm);
  Int redundancyDetectionLP(glp_prob *lp, glp_smcp *lp_parm, glp_iocp *mip_parm);

  void adjust();
  Int free_variable();

  bool operator==(const LinearConstraint &lc) const;
  void insertCache(const Int &v);
  Int findCache();
  static void resetCacheStats();
  static double cacheHitRate();
  static unsigned long long cacheCleanLimitEntries();
  static void printCacheStats(ostream &out);
  static void initializeCacheForInstance();
  static void clearCachesForTest();

  enum Ablation_Func {
    // kRedundancyDetectionSingle = 1,
    kRemoveFixedVariables = 1,
    kBoundStrengtheningSingle = 2,
    kCoefficientStrengtheningSingle = 3,
    kRedundancyDetectionSubset = 4,
    kParallelRows = 5,
    kBoundStrengtheningLP = 6,
    kRedundancyDetectionLP = 7,
  };

  static unordered_set<Ablation_Func> ablation;
  static int variable_limit;
  static bool sb_iterate_to_fixed_point;
};

#endif
