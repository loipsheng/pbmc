#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "linear_constraint.h"
#include "solver.h"

namespace {

LinearConstraint ReadConstraint(const std::string &text) {
  std::istringstream input(text);
  LinearConstraint constraint;
  constraint.read(input);
  if (!input) {
    std::cerr << "failed to parse test constraint\n";
    std::exit(1);
  }
  return constraint;
}

void ExpectEqual(const Int &actual, const Int &expected,
                 const std::string &test_name) {
  if (actual != expected) {
    std::cerr << test_name << ": expected " << expected
              << ", got " << actual << '\n';
    std::exit(1);
  }
}

Int BruteForceCount(const LinearConstraint &constraint, int variable,
                    std::vector<Int> &assignment) {
  if (variable != constraint.getN()) {
    Int result(0);
    for (Int value = constraint.ranges[variable].first;
         value.cmp(constraint.ranges[variable].second) <= 0; value++) {
      assignment[variable] = value;
      result += BruteForceCount(constraint, variable + 1, assignment);
    }
    return result;
  }

  for (const auto &row : constraint.matrix) {
    Int lhs(0);
    for (int column = 0; column < constraint.getN(); ++column)
      lhs += row[column] * assignment[column];
    if (lhs.cmp(row.back()) > 0)
      return Int(0);
  }
  return Int(1);
}

void TestInfeasibleLpRelaxationIsNotIgnored() {
  // With either row relaxed, the duplicate remaining row makes the LP
  // relaxation infeasible.  The old code called glp_intopt anyway, received
  // GLP_EROOT, and silently kept both impossible rows.
  LinearConstraint constraint = ReadConstraint(
      "1 2\n"
      "0 1\n"
      "1 -1\n"
      "1 -1\n");
  ExpectEqual(constraint.LP(LinearConstraint::kLPRedundancyOnly), Int(0),
              "infeasible LP relaxation");
}

void TestConstantContradictionIsDetected() {
  LinearConstraint constraint = ReadConstraint(
      "2 2\n"
      "0 1\n"
      "0 1\n"
      "0 0 -1\n"
      "0 0 -1\n");
  ExpectEqual(constraint.LP(LinearConstraint::kLPRedundancyOnly), Int(0),
              "constant contradiction");
}

void TestRedundantRowIsRemoved() {
  LinearConstraint constraint = ReadConstraint(
      "1 2\n"
      "0 1\n"
      "1 0\n"
      "1 1\n");
  ExpectEqual(constraint.LP(LinearConstraint::kLPRedundancyOnly), Int(-1),
              "redundant row return code");
  if (constraint.getM() != 1 || constraint.matrix[0].back() != 0) {
    std::cerr << "redundant row was not removed safely\n";
    std::exit(1);
  }
}

void TestUnrepresentableIntegerModelFailsClosed() {
  LinearConstraint constraint = ReadConstraint(
      "2 2\n"
      "-32 31\n"
      "0 1\n"
      "1 18446744073709551583 18446744073709551614\n"
      "-1 18446744073709551648 32\n");
  const LinearConstraint original = constraint;
  ExpectEqual(constraint.LP(LinearConstraint::kLPRedundancyOnly), Int(-2),
              "unrepresentable integer model");
  if (!(constraint == original)) {
    std::cerr << "unsafe GLPK model changed the exact constraint\n";
    std::exit(1);
  }
}

void TestInexactObjectiveRangeFailsClosed() {
  // Every scalar below is individually representable as a double, but the
  // possible LHS 2^53 + 1 is not.  Treating that rounded objective as exact
  // could incorrectly remove the first row.
  LinearConstraint constraint = ReadConstraint(
      "2 2\n"
      "0 1\n"
      "0 1\n"
      "9007199254740992 1 9007199254740992\n"
      "0 1 1\n");
  const LinearConstraint original = constraint;
  ExpectEqual(constraint.LP(LinearConstraint::kLPRedundancyOnly), Int(-2),
              "inexact objective range");
  if (!(constraint == original)) {
    std::cerr << "inexact objective range changed the constraint\n";
    std::exit(1);
  }
}

void TestObjectiveOutsideNativeIntIsNotTruncated() {
  LinearConstraint constraint = ReadConstraint(
      "1 1\n"
      "0 3000000000\n"
      "1 2999999999\n");
  ExpectEqual(constraint.LP(LinearConstraint::kLPRedundancyOnly), Int(-2),
              "objective outside native int");
  if (constraint.getM() != 1 ||
      constraint.matrix[0].back() != Int("2999999999")) {
    std::cerr << "large exact objective caused an unsafe row removal\n";
    std::exit(1);
  }
}

void TestDecomposePreservesConstantRows() {
  LinearConstraint constraint = ReadConstraint(
      "2 2\n"
      "0 1\n"
      "0 1\n"
      "1 0 1\n"
      "0 0 -1\n");
  constraint.initPID();
  const auto components = constraint.decompose();
  int row_count = 0;
  bool found_contradiction = false;
  for (const LinearConstraint &component : components) {
    row_count += component.getM();
    for (const auto &row : component.matrix) {
      bool constant = true;
      for (int i = 0; i < component.getN(); ++i)
        constant = constant && row[i] == 0;
      found_contradiction = found_contradiction ||
          (constant && row.back().cmp(0) < 0);
    }
  }
  if (row_count != constraint.getM() || !found_contradiction) {
    std::cerr << "decompose dropped a constant contradictory row\n";
    std::exit(1);
  }
}

void TestEndToEndRirLpCount() {
  LinearConstraint::ablation = {
      LinearConstraint::kRemoveFixedVariables,
      LinearConstraint::kBoundStrengtheningSingle,
      LinearConstraint::kCoefficientStrengtheningSingle,
      LinearConstraint::kRedundancyDetectionSubset,
      LinearConstraint::kParallelRows,
      LinearConstraint::kBoundStrengtheningLP,
  };
  LinearConstraint::variable_limit = 0;
  LinearConstraint::cache_matrix_limit = 0;
  LinearConstraint::initializeCacheForInstance();

  LinearConstraint constraint = ReadConstraint(
      "1 2\n"
      "0 1\n"
      "1 -1\n"
      "1 -1\n");
  ExpectEqual(Solver::count(constraint), Int(0), "end-to-end RIR-LP count");
}

void TestGeneratedInstancesAgainstBruteForce() {
  LinearConstraint::ablation = {
      LinearConstraint::kRemoveFixedVariables,
      LinearConstraint::kBoundStrengtheningSingle,
      LinearConstraint::kCoefficientStrengtheningSingle,
      LinearConstraint::kRedundancyDetectionSubset,
      LinearConstraint::kParallelRows,
      LinearConstraint::kBoundStrengtheningLP,
  };
  LinearConstraint::variable_limit = 0;
  LinearConstraint::cache_matrix_limit = 0;

  std::mt19937 generator(20261017);
  std::uniform_int_distribution<int> coefficient(-3, 3);
  std::uniform_int_distribution<int> rhs(-5, 5);
  std::uniform_int_distribution<int> row_count(1, 5);

  for (int test_id = 0; test_id < 250; ++test_id) {
    LinearConstraint constraint;
    const int variables = 1 + test_id % 3;
    constraint.ranges.assign(variables, {Int(-1), Int(1)});
    const int rows = row_count(generator);
    constraint.matrix.assign(rows, vector<Int>(variables + 1, Int(0)));
    for (auto &row : constraint.matrix) {
      for (int column = 0; column < variables; ++column)
        row[column] = coefficient(generator);
      row.back() = rhs(generator);
    }

    std::vector<Int> assignment(variables);
    const Int expected = BruteForceCount(constraint, 0, assignment);
    LinearConstraint candidate = constraint;
    LinearConstraint::initializeCacheForInstance();
    const Int actual = Solver::count(candidate);
    if (actual != expected) {
      std::cerr << "generated instance " << test_id << ": expected "
                << expected << ", got " << actual << '\n';
      constraint.output(std::cerr);
      std::exit(1);
    }
  }
}

void TestVariableLimitSelectsRirImplementation() {
  const auto disabled_other_passes = std::initializer_list<
      LinearConstraint::Ablation_Func>{
      LinearConstraint::kBoundStrengtheningSingle,
      LinearConstraint::kCoefficientStrengtheningSingle,
      LinearConstraint::kRedundancyDetectionSubset,
      LinearConstraint::kParallelRows,
      LinearConstraint::kBoundStrengtheningLP,
  };

  LinearConstraint::ablation = disabled_other_passes;
  LinearConstraint::variable_limit = 1;
  LinearConstraint small = ReadConstraint(
      "2 1\n"
      "0 0\n"
      "0 1\n"
      "0 1 1\n");
  ExpectEqual(small.simplify(), Int(2), "K selects RIR");
  if (small.simplification_stats.rr_constraints_removed != 1 ||
      small.simplification_stats.rr_lp_constraints_removed != 0) {
    std::cerr << "post-RV N <= K did not select interval-based RIR\n";
    std::exit(1);
  }

  LinearConstraint::ablation = disabled_other_passes;
  LinearConstraint::variable_limit = 0;
  LinearConstraint large = ReadConstraint(
      "1 1\n"
      "0 1\n"
      "1 1\n");
  ExpectEqual(large.simplify(), Int(2), "K selects RIR-LP");
  if (large.simplification_stats.rr_constraints_removed != 0 ||
      large.simplification_stats.rr_lp_constraints_removed != 1) {
    std::cerr << "N > K did not select RIR-LP\n";
    std::exit(1);
  }
}

void TestDisabledRirLpFallsBackToRir() {
  LinearConstraint::ablation = {
      LinearConstraint::kBoundStrengtheningSingle,
      LinearConstraint::kCoefficientStrengtheningSingle,
      LinearConstraint::kRedundancyDetectionSubset,
      LinearConstraint::kParallelRows,
      LinearConstraint::kBoundStrengtheningLP,
      LinearConstraint::kRedundancyDetectionLP,
  };
  LinearConstraint::variable_limit = 0;

  LinearConstraint constraint = ReadConstraint(
      "1 1\n"
      "0 1\n"
      "1 1\n");
  ExpectEqual(constraint.simplify(), Int(2),
              "disabled RIR-LP falls back to RIR");
  if (constraint.simplification_stats.rr_constraints_removed != 1 ||
      constraint.simplification_stats.rr_lp_constraints_removed != 0) {
    std::cerr << "disabled RIR-LP did not fall back to interval-based RIR\n";
    std::exit(1);
  }
}

void TestVariableLimitSelectsBothAlgorithmPairs() {
  LinearConstraint::ablation.clear();
  LinearConstraint::variable_limit = 1;
  LinearConstraint small = ReadConstraint(
      "1 1\n"
      "0 2\n"
      "1 1\n");
  ExpectEqual(small.simplify(), Int(2), "K selects SB and RIR");
  if (small.simplification_stats.sb_bound_updates != 1 ||
      small.simplification_stats.sb_lp_bound_updates != 0 ||
      small.simplification_stats.rr_constraints_removed != 1 ||
      small.simplification_stats.rr_lp_constraints_removed != 0) {
    std::cerr << "N <= K did not select the SB/RIR pair\n";
    std::exit(1);
  }

  LinearConstraint::ablation.clear();
  LinearConstraint::variable_limit = 0;
  LinearConstraint large = ReadConstraint(
      "1 1\n"
      "0 2\n"
      "1 1\n");
  ExpectEqual(large.simplify(), Int(2), "K selects SB-LP and RIR-LP");
  if (large.simplification_stats.sb_bound_updates != 0 ||
      large.simplification_stats.sb_lp_bound_updates != 1 ||
      large.simplification_stats.rr_constraints_removed != 0 ||
      large.simplification_stats.rr_lp_constraints_removed != 1) {
    std::cerr << "N > K did not select the SB-LP/RIR-LP pair\n";
    std::exit(1);
  }
}

}  // namespace

int main() {
  TestInfeasibleLpRelaxationIsNotIgnored();
  TestConstantContradictionIsDetected();
  TestRedundantRowIsRemoved();
  TestUnrepresentableIntegerModelFailsClosed();
  TestInexactObjectiveRangeFailsClosed();
  TestObjectiveOutsideNativeIntIsNotTruncated();
  TestDecomposePreservesConstantRows();
  TestEndToEndRirLpCount();
  TestGeneratedInstancesAgainstBruteForce();
  TestVariableLimitSelectsRirImplementation();
  TestDisabledRirLpFallsBackToRir();
  TestVariableLimitSelectsBothAlgorithmPairs();
  std::cout << "RIR-LP regression tests passed\n";
  return 0;
}
