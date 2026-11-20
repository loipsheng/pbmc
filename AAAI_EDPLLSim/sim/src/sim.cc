// Implementation of the preprocessor Sim
#include "sim.h"

#include "heuristic.h"

Sim::Result Sim::preprocess(LinearConstraint &constraint, int variable_limit) {
  Result result;
  const std::vector<std::vector<Int>> input_matrix = constraint.matrix;

  LinearConstraint::variable_limit = variable_limit;
  LinearConstraint::ablation.clear();
  LinearConstraint::ablation.insert(LinearConstraint::kParallelRows);
  LinearConstraint::ablation.insert(LinearConstraint::kRedundancyDetectionLP);
  Heuristic::set_heuristic(BH_BC);

  // Preserve the simplifier's previous canonical input while retaining the
  // caller's row order for a genuinely unchanged result.
  constraint.sortMatrix();
  const LinearConstraint original = constraint;

  while (true) {
    const LinearConstraint before_round = constraint;
    result.count = constraint.simplify();
    if (result.count.cmp(0) >= 0 || constraint == before_round)
      break;
  }

  if (result.count == 0)
    result.status = Status::kUnsat;
  else if (result.count.cmp(0) > 0)
    result.status = Status::kDirectCount;
  else if (constraint == original) {
    result.status = Status::kUnchanged;
    constraint.matrix = input_matrix;
  }
  else
    result.status = Status::kSimplified;
  return result;
}
