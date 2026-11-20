#ifndef __ADAPTIVE_K_H__
#define __ADAPTIVE_K_H__

#include "linear_constraint.h"

namespace adaptive_k {

struct Metrics {
  int n0 = 0;
  int m0 = 0;
  double mean_jaccard = 0.0;
  double jaccard_confidence = 0.0;
  int k = 0;
};

struct Decision {
  Metrics metrics;
};

// For original input-row supports S_i, compute
//   J = mean_{i<r} |S_i intersection S_r| / |S_i union S_r|.
// Empty-union pairs contribute zero. Inputs with fewer than two rows have J=0.
double ComputeMeanJaccard(const LinearConstraint &constraint);

// Let P=m0*(m0-1)/2. The final adaptive score is
//   J_conf = J*P/(P+1) = sum_{i<r} J_ir/(P+1).
// K is nearest_integer_half_up(n0*(1-J_conf)), clamped to [1,n0].
// The calculation remains exact rational arithmetic through K selection.
double ComputeJaccardConfidence(const LinearConstraint &constraint);
int SelectJaccardConfidenceThreshold(const LinearConstraint &constraint);

// Select one fixed K from the original input. This input-only calculation does
// not run SB, RIR, or any other simplification probe.
Decision Compute(const LinearConstraint &root);

}  // namespace adaptive_k

#endif
