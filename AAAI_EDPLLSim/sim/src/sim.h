#ifndef EDPLLSIM_SIM_H
#define EDPLLSIM_SIM_H

#include "linear_constraint.h"

class Sim {
public:
  enum class Status { kSimplified, kUnchanged, kUnsat, kDirectCount };

  struct Result {
    Status status = Status::kUnchanged;
    Int count = Int(-1);
  };

  // Default preprocessing configuration.
  static constexpr int kVariableLimit = 14;
  static Result preprocess(LinearConstraint &constraint,
    int variable_limit = kVariableLimit);
};

#endif
