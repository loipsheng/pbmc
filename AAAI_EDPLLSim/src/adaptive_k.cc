#include "adaptive_k.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace adaptive_k {
namespace {

double ClampUnit(double value) {
  if (!std::isfinite(value))
    return value > 0.0 ? 1.0 : 0.0;
  return std::max(0.0, std::min(1.0, value));
}

int ThresholdFromExactScore(int n, mpq_srcptr score) {
  if (n <= 0)
    return 0;

  // raw = n*(1-score). Nearest-integer, half-up rounding is exactly
  // floor((2*n*(den-num)+den)/(2*den)).
  mpz_t complement;
  mpz_t rounded_numerator;
  mpz_t rounded_denominator;
  mpz_t quotient;
  mpz_inits(complement, rounded_numerator, rounded_denominator, quotient,
            nullptr);
  mpz_sub(complement, mpq_denref(score), mpq_numref(score));
  mpz_mul_ui(rounded_numerator, complement,
             static_cast<unsigned long>(n));
  mpz_mul_ui(rounded_numerator, rounded_numerator, 2);
  mpz_add(rounded_numerator, rounded_numerator, mpq_denref(score));
  mpz_mul_ui(rounded_denominator, mpq_denref(score), 2);
  mpz_fdiv_q(quotient, rounded_numerator, rounded_denominator);

  const long exact_threshold = mpz_get_si(quotient);
  mpz_clears(complement, rounded_numerator, rounded_denominator, quotient,
             nullptr);
  return std::max(1, std::min(n, static_cast<int>(exact_threshold)));
}

struct JaccardResult {
  double mean = 0.0;
  double confidence = 0.0;
  int threshold = 0;
};

JaccardResult CalculateJaccardConfidence(
    const LinearConstraint &constraint) {
  JaccardResult result;
  const int n = constraint.getN();
  const int m = constraint.getM();
  if (n <= 0)
    return result;

  result.threshold = n;
  if (m < 2)
    return result;

  const std::size_t word_count =
      (static_cast<std::size_t>(n) + 63U) / 64U;
  std::vector<std::vector<std::uint64_t>> supports(
      static_cast<std::size_t>(m),
      std::vector<std::uint64_t>(word_count, 0));
  for (int row = 0; row < m; ++row) {
    const int coefficient_count = std::min(
        n, static_cast<int>(constraint.matrix[row].size()));
    for (int column = 0; column < coefficient_count; ++column) {
      if (constraint.matrix[row][column] != 0) {
        supports[static_cast<std::size_t>(row)]
                [static_cast<std::size_t>(column) / 64U] |=
            std::uint64_t{1} << (static_cast<unsigned>(column) % 64U);
      }
    }
  }

  mpq_t pair_sum;
  mpq_t term;
  mpq_t pair_count;
  mpq_t confidence_denominator;
  mpq_t mean;
  mpq_t confidence;
  mpq_inits(pair_sum, term, pair_count, confidence_denominator, mean,
            confidence, nullptr);

  for (int first = 0; first < m; ++first) {
    for (int second = first + 1; second < m; ++second) {
      unsigned long intersection_size = 0;
      unsigned long union_size = 0;
      for (std::size_t word = 0; word < word_count; ++word) {
        const std::uint64_t first_word =
            supports[static_cast<std::size_t>(first)][word];
        const std::uint64_t second_word =
            supports[static_cast<std::size_t>(second)][word];
        intersection_size += static_cast<unsigned long>(
            __builtin_popcountll(first_word & second_word));
        union_size += static_cast<unsigned long>(
            __builtin_popcountll(first_word | second_word));
      }
      if (union_size > 0) {
        mpq_set_ui(term, intersection_size, union_size);
        mpq_add(pair_sum, pair_sum, term);
      }
    }
  }

  mpz_bin_uiui(mpq_numref(pair_count), static_cast<unsigned long>(m), 2);
  mpz_set_ui(mpq_denref(pair_count), 1);
  mpq_div(mean, pair_sum, pair_count);

  // Add one zero-similarity reference pair: J_conf=J*P/(P+1).
  mpq_set(confidence_denominator, pair_count);
  mpz_add_ui(mpq_numref(confidence_denominator),
             mpq_numref(confidence_denominator), 1);
  mpq_canonicalize(confidence_denominator);
  mpq_div(confidence, pair_sum, confidence_denominator);

  result.mean = ClampUnit(mpq_get_d(mean));
  result.confidence = ClampUnit(mpq_get_d(confidence));
  result.threshold = ThresholdFromExactScore(n, confidence);
  mpq_clears(pair_sum, term, pair_count, confidence_denominator, mean,
             confidence, nullptr);
  return result;
}

}  // namespace

double ComputeMeanJaccard(const LinearConstraint &constraint) {
  return CalculateJaccardConfidence(constraint).mean;
}

double ComputeJaccardConfidence(const LinearConstraint &constraint) {
  return CalculateJaccardConfidence(constraint).confidence;
}

int SelectJaccardConfidenceThreshold(const LinearConstraint &constraint) {
  return CalculateJaccardConfidence(constraint).threshold;
}

Decision Compute(const LinearConstraint &root) {
  Decision decision;
  decision.metrics.n0 = root.getN();
  decision.metrics.m0 = root.getM();

  const JaccardResult result = CalculateJaccardConfidence(root);
  decision.metrics.mean_jaccard = result.mean;
  decision.metrics.jaccard_confidence = result.confidence;
  decision.metrics.k = result.threshold;
  return decision;
}

}  // namespace adaptive_k
