/*
Copyright (c) 2025 Grabtaxi Holdings Pte Ltd (GRAB), All rights reserved.

Use of this source code is governed by an MIT-style license that can be found in the LICENSE file
*/

#ifndef Counter_h
#define Counter_h

#include "core/Solver.h"
#include "core/Component.h"
#include "core/Config.h"
#include "core/Instance.h"
#include "preprocessor/Preprocessor.h"
#include "ddnnf/DecisionTree.h"

// importing roundingsat and all its namespace (same imports as roundingsat.cpp)
#include "roundingsatInclude.h"

using namespace Glucose;

namespace GPMC {

// #define GPMC_VERSION "gpmc-1.1.1"
// #define GPMC_VERSION "gpmc-pb-0.0"
#define GPMC_VERSION "PBMC-0.0"

enum progressT {
	INIT, LOADED, PREPROCESSED, COMPLETED_BYPP, COMPLETED, FAILED
};

enum btStateT {
	EXIT, RESOLVED, GO_TO_NEXT_COMP
};


// modifying gpmc counter class handle PB
// NOTE: not directly using namespace rs since there are delarations of same names as Glucose, which makes it ambiguous.
// Explicitly use rs::Class when modifying below
template <class T_data> // either mpz_class (gmp int for unweighted) or mpfr::mpreal (weighted counting)

class PBcounter: public rs::Solver {
public:
	// Constructor/Destructor
	//
	// PBcounter();
	PBcounter(Configuration& config);
	~PBcounter() { };

	// Methods
	//
	// maybe we do not need this, because parsing and preprocessing is done at one go
	void load(std::istream& in); 					// load an input instance
	bool preprocess();								// preprocessing for simplification
	Glucose::Lit rsLitToGlucoseLit(rs::Lit l);
	Glucose::Lit rsLitToGlucoseLit(rs::Lit l, bool offset);
	rs::Lit glucoseLitToRSLit(Glucose::Lit l);
	//
	void setExtraVarScore();							// set exscore
	bool countModels();		    					// main count method
	void printStats() const;							// print statistics
	const T_data& getMC() { return npmodels; }	// get the computed count

	void writeNNF();
	T_data mcDDNNF();

	int nPVars() const;				// The number of non-isolated projection variables
	int nIsoPVars() const;			// The number of isolated projection variables
	int nVars() const; 				// The number of variables declared by #variables in opb input file

	void copyPBInsNPVar();			// Copying parsed number of PB variables to counter internal settings
	void copyPBInsPhase();			// copying optimal phase for each pb variable from pbins (phase defaults to assignment that subtracts coefficient from slack)

	// Data members
	//
	bool sat;
	T_data npmodels;

	// options
	ConfigCounter config;
	ConfigTreeDecomposition tdconfig;

	/// statistics
	uint64_t conflicts_pre=0, decisions_pre=0, propagations_pre=0;
	uint64_t conflicts_sg=0, decisions_sg=0, propagations_sg=0;
	uint64_t sats=0, nbackjumps=0, nbackjumps_sp=0;
	uint64_t reduce_dbs_pre=0, simp_dbs=0;
	double simplify_time=0;

	// Statistics: (read-only member variable)
	// (copied over from Glucose solver.h) since we using rs::solver instead.
	uint64_t nbRemovedClauses=0,nbReducedClauses=0,nbDL2=0,nbBin=0,nbUn=0,nbReduceDB=0,solves=0, starts=0, decisions=0, rnd_decisions=0, propagations=0, conflicts=0,conflictsRestarts=0,nbstopsrestarts=0,nbstopsrestartssame=0,lastblockatrestart=0;
	uint64_t dec_vars=0, clauses_literals=0, learnts_literals=0, max_literals=0, tot_literals=0;

	// moving some members into public so that accessible from main.cc
	PBInstance<T_data> pbins; // pb instance (for reading input and renumbering)
	progressT progress;		// counter progress

	// bool alternativeCoeffScore = true;
	bool alternativeCoeffScore = false; // true gives bad performance

protected:
	// Methods
	//
	bool simplify();

	// import a CNF instance
	void import();
	void loadWeight();

	// set extra var scores
	void computeTDScore();		// compute var score from tree decomposition
	void setGivenVarScore();		// set extra var score from file (from ins.score, consistently with ins.gmap)

	void addCoeffVarScore(); // to add coefficient var score to exscore

	/// count main method
	void count_main();

	/// backtrack
	btStateT backtrack(); // int backtrack_level = -1); //, Lit lit = lit_Undef, CRef cr = CRef_Undef);
	void cancelCurDL();
	void bjResolve(int level);
	btStateT Resolve(int bk_level, rs::CRef cr, rs::Lit lit);
	btStateT ResolveWithoutLit(int bk_level, rs::CeSuper& constraint);

	/// SAT solving for the current component
	void newDecisionLevel(); // to match rs and glucose
	lbool solveSAT();
	lbool searchBelow(int start_level);

	lbool solveSATFixedOrder();
	rs::Lit pickBranchLitFixedOrder(bool lastSolPhase);
	lbool searchBelowFixedOrder(int start_level); // for following fixed variable decision order
	// runPropagationWithLimitLevel returns a pair, CeSuper is the conflict or CeNull if no conlict, the bool is suppose to be success or failure (with respect to limit_level). Failure means the processing of learnt conflict to assertion level requires backtracking above limit_level.
	std::pair<rs::CeSuper, bool> runPropagationWithLimitLevel(bool onlyUnitPropagation, int limit_level);
	rs::CeSuper processLearnedStackWithLimitLevel(int limit_level);
	std::pair<rs::CeSuper, bool> analyzeWithLimitLevel(rs::CeSuper conflict, int limit_level);

	bool analyzePBMC2(rs::CeSuper conflict, vector<rs::CeSuper>& out_learnt, int& out_btlevel, unsigned int& lbd);
	
	rs::CeSuper runPropagationWithoutLearnt(bool onlyUnitPropagation);

	// for simulating backjump and then resuming previous rs solver checkpoint state
	void checkpointSolverState();
	void resumeSolverCheckpoint();
	
	template <typename V_data>
	void checkpointVector(std::vector<V_data> &source, std::vector<V_data> &target);
	//
	void syncAssign();  // used to synchronise Pos (rs) and Assign (Glucose indexing)

	/// Print stats of the component manager.
	void printStatsOfCM() const;

	// Data members
	//
	int npvars;			// boundary number between projection vars and non-projection vars
	int npvars_isolated;	// Number of isolated projection vars

	ComponentManager<T_data> cmpmgr;	// The manger for processing components
	vec<vec<int>> occ_lists;

	vec<rs::Lit> unitcls;			// The list of learnt unit clauses

	int limlevel;				// for limited backjump
	bool last_suc;
	int  last_bklevel;
	rs::CRef last_cr;
	rs::Lit  last_lit;

	vec<T_data> lit_weight;	// literal weight
	unordered_map<rs::Lit, T_data> lit_weight_map;	// literal weight map
	T_data gweight;			// global weight

	vec<double> exscore;		// extra score for variable selection
	
	vector<rs::Var> search_below_variable_order;	// ordering for non projection set variables when calling searchBelow, this is a subset of exscore sorted in ascending order from lowest to highest score in exscore.
	vector<rs::Var> search_below_variable_order_stack; // to use as stack, pop the next decision variable until empty
	unordered_set<rs::Var> solve_sat_stack_sorting_set; // for sorting variables in stack

	void setSearchBelowNonProjectionVarOrder();

	bool on_bj;				// limited bachjump on/off
	bool on_simp;				// in-processing simplification on/off

	// progressT progress;		// counter progress

	int verbosity_c;			// verbosity level
	bool mc;					// mc or pmc
	bool wc;					// weighted or no-weighted

	vec<lbool> assigns;			// The current assignments. used to replicate glucose solver datastructure

	PPMC::Preprocessor<T_data> pp;	// preprocessor

	bool has_checkpoint_simulated_state = false; // to indicate if simulation backjump in progresss (do not clear the data in simulation datastructures when this is true)
};

template <typename T_data>
inline int PBcounter<T_data>::nPVars() const {
	return npvars;
}
template <typename T_data>
inline int PBcounter<T_data>::nVars() const {
	return pbins.vars;
}
template <typename T_data>
inline int PBcounter<T_data>::nIsoPVars() const {
	return npvars_isolated;
}
template <typename T_data>
inline void PBcounter<T_data>::printStatsOfCM() const {
	cmpmgr.printStats();
}



}

#endif

