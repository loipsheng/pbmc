#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "adaptive_k.h"

namespace {

LinearConstraint ReadConstraint(const std::string &text) {
  std::istringstream input(text);
  LinearConstraint constraint;
  constraint.read(input);
  if (!input) {
    std::cerr << "failed to parse adaptive K test constraint\n";
    std::exit(1);
  }
  return constraint;
}

void Expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void ExpectNear(double actual, double expected, double tolerance,
                const std::string &message) {
  if (std::fabs(actual - expected) > tolerance) {
    std::cerr << message << ": expected " << expected
              << ", got " << actual << '\n';
    std::exit(1);
  }
}

void TestFormulaAndDecision() {
  LinearConstraint constraint = ReadConstraint(
      "4 3\n"
      "0 1\n"
      "0 1\n"
      "0 1\n"
      "0 1\n"
      "1 1 0 0 1\n"
      "1 -1 1 0 1\n"
      "0 1 1 1 1\n");
  const LinearConstraint original = constraint;

  // Pair similarities are 2/3, 1/4, and 1/2. Thus J=17/36, P=3,
  // J_conf=J*P/(P+1)=17/48, and raw K=4*(1-17/48)=31/12 -> 3.
  ExpectNear(adaptive_k::ComputeMeanJaccard(constraint), 17.0 / 36.0,
             1e-12, "mean Jaccard is incorrect");
  ExpectNear(adaptive_k::ComputeJaccardConfidence(constraint), 17.0 / 48.0,
             1e-12, "Jaccard-Confidence is incorrect");
  Expect(adaptive_k::SelectJaccardConfidenceThreshold(constraint) == 3,
         "Jaccard-Confidence threshold is incorrect");

  const adaptive_k::Decision decision = adaptive_k::Compute(constraint);
  Expect(decision.metrics.n0 == 4 && decision.metrics.m0 == 3,
         "decision did not retain original dimensions");
  ExpectNear(decision.metrics.mean_jaccard, 17.0 / 36.0, 1e-12,
             "decision did not retain J");
  ExpectNear(decision.metrics.jaccard_confidence, 17.0 / 48.0, 1e-12,
             "decision did not retain J_conf");
  Expect(decision.metrics.k == 3, "decision selected the wrong K");
  Expect(constraint == original,
         "input-only adaptive K modified the original constraint");
}

void TestExactHalfUpRounding() {
  LinearConstraint two_identical_rows = ReadConstraint(
      "3 2\n"
      "0 1\n"
      "0 1\n"
      "0 1\n"
      "1 1 0 1\n"
      "1 1 0 1\n");
  // J=1, P=1, J_conf=1/2, and raw K=3/2. Half-up must return 2.
  ExpectNear(adaptive_k::ComputeJaccardConfidence(two_identical_rows), 0.5,
             1e-12, "two-row confidence correction is incorrect");
  Expect(adaptive_k::SelectJaccardConfidenceThreshold(
             two_identical_rows) == 2,
         "exact half-up rounding returned the wrong K");
}

void TestSparseAndSmallInputs() {
  LinearConstraint empty_pair = ReadConstraint(
      "2 2\n"
      "0 1\n"
      "0 1\n"
      "0 0 1\n"
      "0 0 1\n");
  ExpectNear(adaptive_k::ComputeJaccardConfidence(empty_pair), 0.0, 1e-12,
             "empty supports should contribute zero");
  Expect(adaptive_k::SelectJaccardConfidenceThreshold(empty_pair) == 2,
         "zero score should retain K=n0");

  LinearConstraint single_row = ReadConstraint(
      "2 1\n"
      "0 1\n"
      "0 1\n"
      "1 1 1\n");
  ExpectNear(adaptive_k::ComputeJaccardConfidence(single_row), 0.0, 1e-12,
             "single-row score should be zero");
  Expect(adaptive_k::SelectJaccardConfidenceThreshold(single_row) == 2,
         "single-row input should retain K=n0");

  LinearConstraint no_rows = ReadConstraint(
      "2 0\n"
      "-1 1\n"
      "4 5\n");
  Expect(adaptive_k::Compute(no_rows).metrics.k == 2,
         "constraint-free input should retain K=n0");
}

void TestStnStructure() {
  // STN_T1_13 structure: one row for every ordered pair i!=j, each with
  // support {i,j}. J=47/465 and P=156*155/2=12090.
  std::ostringstream text;
  text << "13 156\n";
  for (int variable = 0; variable < 13; ++variable)
    text << "-100 100\n";
  for (int first = 0; first < 13; ++first) {
    for (int second = 0; second < 13; ++second) {
      if (first == second)
        continue;
      for (int variable = 0; variable < 13; ++variable) {
        int coefficient = 0;
        if (variable == first)
          coefficient = 1;
        else if (variable == second)
          coefficient = -1;
        text << coefficient << ' ';
      }
      text << "5\n";
    }
  }

  LinearConstraint stn = ReadConstraint(text.str());
  const double expected = (47.0 / 465.0) * (12090.0 / 12091.0);
  ExpectNear(adaptive_k::ComputeJaccardConfidence(stn), expected, 1e-12,
             "STN Jaccard-Confidence score is incorrect");
  Expect(adaptive_k::SelectJaccardConfidenceThreshold(stn) == 12,
         "STN Jaccard-Confidence threshold should be 12");
}

}  // namespace

int main() {
  TestFormulaAndDecision();
  TestExactHalfUpRounding();
  TestSparseAndSmallInputs();
  TestStnStructure();
  std::cout << "Adaptive K Jaccard-Confidence tests passed\n";
  return 0;
}
