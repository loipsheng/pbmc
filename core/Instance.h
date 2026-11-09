/*
Copyright (c) 2025 Grabtaxi Holdings Pte Ltd (GRAB), All rights reserved.

Use of this source code is governed by an MIT-style license that can be found in the LICENSE file
*/

#ifndef Instance_h
#define Instance_h

#include <vector>
#include "core/SolverTypes.h"

#include <gmpxx.h>
#include "mpfr/mpreal.h"
// importing roundingsat namespace
#include "roundingsatInclude.h"
#include "pbconstraint.h" // for constraint representation in pbinstance

namespace GPMC {

// values for variable map
const Glucose::Var var_Determined = {-2};

template <class T_data>
class Instance {
public:
	Instance();

	void load			(std::istream& in, bool weighted, bool projected, bool ddnnf);
	bool addClause	(std::vector<Glucose::Lit>& lits, bool learnt=false);

	Glucose::lbool value (Glucose::Var x) const;
	Glucose::lbool value (Glucose::Lit p) const;

	// variable correspondence
	void printVarMapStats() const;
	void writeVarMap(std::ostream& out);

	// import extra var score
	void importVarScore(std::istream& in);

	// For Debug
	void toDimacs(std::ostream& out);

	// CNF formula
	int vars;
	std::vector<std::vector<Glucose::Lit>> clauses;
	std::vector<std::vector<Glucose::Lit>> learnts;

	// Counter Mode
	bool weighted;
	bool projected;

	// For WMC/WPMC
	std::vector<T_data> lit_weights;

	// For PMC/WPMC
	int npvars;
	std::vector<bool> ispvars;
	std::vector<Glucose::Var> pvars;

	// Instance keeps temporal fixed literals. Preprocessor will eliminate fixed variables.
	std::vector<Glucose::lbool>	assigns;
	std::vector<Glucose::Lit>	assignedLits;

	// additional information
	int freevars;
	T_data gweight;

	// information about correspondence with a given CNF
	bool keepVarMap;
	std::vector<Glucose::Lit> gmap;								// variable renaming (original (Var) -> simplified (Lit))
	std::vector<Glucose::Lit> fixedLits;						// fixed Literals
	std::vector<Glucose::Var> definedVars;						// variables defined by the other variables
	std::vector<std::vector<Glucose::Lit>> freeLitClasses;	// equivalence classes of free literals

	// extra var score
	std::vector<double> score;

	// State
	bool unsat;

};

template <class T_data>
inline Glucose::lbool Instance<T_data>::value (Glucose::Var x) const { return assigns[x]; }
template <class T_data>
inline Glucose::lbool Instance<T_data>::value (Glucose::Lit p) const { return assigns[var(p)] ^ sign(p); }

// Instance class for PB below

// values for variable map
// const Glucose::Var var_Determined = {-2}; // commented out for now

template <class T_data>
class PBInstance {
public:
	PBInstance();

	void load(std::istream& in, bool weighted, bool projected, bool ddnnf);
	// bool addClause(std::vector<Glucose::Lit>& lits, bool learnt=false);
	bool addClause(PBconstraint& parsedConstraint, bool learnt=false);

	Glucose::lbool value (Glucose::Var x) const;
	Glucose::lbool value (Glucose::Lit p) const;

	// variable correspondence
	void printVarMapStats() const;
	void writeVarMap(std::ostream& out);

	// import extra var score
	void importVarScore(std::istream& in);

	// For Debug
	// void toDimacs(std::ostream& out);
	void toOPB(std::ostream& out);

	// for renumbering variables, moving projection set variables to front, and non projected to back, so that npvar can be used as seperating index
	void renumberVar();
	PBconstraint getRenumConstraint(PBconstraint& origConstraint, std::unordered_map<int, int> oldToNewVarMap);
	std::stringstream getRenumberedOpb();
	std::unordered_map<int, double> varCoeffScores; // renumbered variable : coeff score
	void computeVarCoeffScore(double maxCoeffScore, bool alternating = false);

	// for manipulating constraints
	void makeRenumberedCoeffPositive();
	PBconstraint getPositiveCoeffRenumberedConstraint(PBconstraint& origConstraint);

	void simplifyRenumberedConstraint();
	void inferAssignment(PBconstraint &constraint, std::unordered_map<rs::Var, bool> &inferredAssignmentMap);
	void addCheckInferredAssign(Int inferredLit, std::unordered_map<rs::Var, bool>&currentInferredAssignment);
	bool checkAlwaysTrue(PBconstraint& constraint);
	std::unordered_set<Int> probeInferAssign(std::unordered_map<rs::Var, bool> &inferredAssignmentMap, std::vector<PBconstraint> &constraintVector);
	PBconstraint applyProbe(const PBconstraint &constraint, Int decLit);
	std::pair<Int, bool> inferProbe(PBconstraint &constraint);
	bool inferredUnsat = false;
	std::unordered_map<rs::Var, bool> inferredAssignment;


	// CNF formula
	// int vars;
	// std::vector<std::vector<Glucose::Lit>> clauses; // vector or clauses, each clause is vector of lit
	// std::vector<std::vector<Glucose::Lit>> learnts;

	// PB formula
	int vars;
	// std::vector<rs::CRef> clauses;
	// std::vector<rs::CRef> learnts;
	std::vector<PBconstraint> clauses;
	std::vector<PBconstraint> learnts;

	std::vector<PBconstraint> renumberedClauses; // constraints after renumbering
	bool renumbered_variables = false;

	std::vector<int> phase; // note this is 0 indexed variable
	void setMaxCoeffVarPhase(); // set phase of variable s.t. max coeff lit is true

	// Counter Mode
	bool weighted;
	bool projected;

	// For WMC/WPMC
	std::vector<T_data> lit_weights;
	std::vector<T_data> renumbered_lit_weights; 

	// For PMC/WPMC
	int npvars;
	std::vector<bool> ispvars;
	std::vector<Glucose::Var> pvars;

	// Instance keeps temporal fixed literals. Preprocessor will eliminate fixed variables.
	std::vector<Glucose::lbool>	assigns;
	std::vector<Glucose::Lit>	assignedLits;

	// additional information
	int freevars;
	T_data gweight;

	// information about correspondence with a given CNF
	bool keepVarMap;
	std::vector<Glucose::Lit> gmap;								// variable renaming (original (Var) -> simplified (Lit))
	std::vector<Glucose::Lit> fixedLits;						// fixed Literals
	std::vector<Glucose::Var> definedVars;						// variables defined by the other variables
	std::vector<std::vector<Glucose::Lit>> freeLitClasses;	// equivalence classes of free literals

	// extra var score
	std::vector<double> score;

	// State
	bool unsat;

};

template <class T_data>
inline Glucose::lbool PBInstance<T_data>::value (Glucose::Var x) const { return assigns[x]; }
template <class T_data>
inline Glucose::lbool PBInstance<T_data>::value (Glucose::Lit p) const { return assigns[var(p)] ^ sign(p); }


}

#endif
