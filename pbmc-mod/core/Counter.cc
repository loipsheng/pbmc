/*
Copyright (c) 2025 Grabtaxi Holdings Pte Ltd (GRAB), All rights reserved.

Use of this source code is governed by an MIT-style license that can be found in the LICENSE file
*/

#include <gmpxx.h>
#include "mpfr/mpreal.h"
#include "core/Counter.h"
#include "mtl/Sort.h"
#include "utils/System.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>

// using namespace rs;
using namespace Glucose;
using namespace GPMC;


/**************************************************************************************************/

/**
 * Implementation of PBcounter class below
*/

//=================================================================================================
// Constructor

template <typename T_data>
PBcounter<T_data>::PBcounter(Configuration& config_) :
  sat                (false)
, npmodels           (0)
, config             (config_.cntr)
, tdconfig           (config_.td)
, conflicts_pre      (0)
, decisions_pre      (0)
, propagations_pre   (0)
, conflicts_sg       (0)
, decisions_sg       (0)
, propagations_sg    (0)
, sats               (0)
, nbackjumps         (0)
, nbackjumps_sp      (0)
, reduce_dbs_pre     (0)
, simp_dbs           (0)
, simplify_time      (0.0)
, npvars             (0)
, npvars_isolated    (0)
, limlevel           (0)
, last_suc           (false)
, last_bklevel       (0)
, last_cr            (rs::CRef_Undef)
, last_lit           (0) // 0 should not be able valid lit in rs since 1 indexed 
, gweight            (1)
, on_bj              (config_.cntr.backjump)
, on_simp            (config_.cntr.remove_sat_cls)
, progress           (INIT)
, verbosity_c        (1)
{
	// verbosity = 0; // no verbosity here since that is a glucose member
	// showModel = false; // also a glucose thing

	switch(config_.cntr.mode) {
	case MC:
		wc = false, mc = true; break;
	case WMC:
		wc = true, mc = true; break;
	case PMC:
		wc = false, mc = false; break;
	case WPMC:
		wc = true, mc = false; break;
	}

	pp.setConfig(config_.pp);
	cmpmgr.setConfig(config_.cm);
}
//=================================================================================================
// Load instance
template <typename T_data>
void PBcounter<T_data>::load(std::istream& in)
{
	// ins.load(in, wc, !mc, config.keepVarMap);
	pbins.load(in, wc, !mc, config.keepVarMap);

	if(config.vs_infile != "NULL") {
		std::ifstream vin(config.vs_infile);
		if (vin) {
			pbins.importVarScore(vin);
			vin.close();
		}
		else {
			std::cerr << "Cannot open file:" << config.vs_infile << std::endl;
			exit(1);
		}
	}

	progress = LOADED;
}
//=================================================================================================
// Preprocessing
template <typename T_data>
bool PBcounter<T_data>::preprocess()
{
	// NOTE/TODO: we are not calling preprocess in main_pbmc, so consider moving some relevant parts to main_pbmc

	bool done = pbins.unsat || pbins.vars == 0;

	if(pbins.unsat) {
		npmodels = 0;
		if(config.ddnnf) cmpmgr.setRoot(BOTTOM_NODE);
		progress = COMPLETED_BYPP;
	}
	else if (pbins.vars == 0) {
		if(wc) {
			npmodels = pbins.gweight;
			gweight = pbins.gweight;
		}
		else {
			npmodels = ((T_data)1) << pbins.freevars;
			npvars_isolated = pbins.freevars;
		}

		if(config.ddnnf)
			cmpmgr.setRoot(TOP_NODE);

		progress = COMPLETED_BYPP;
	}
	else {
		progress = PREPROCESSED;
		if (config.pp_outfile != "NULL") {
			printf("c o outputing simplified CNF...");
			ofstream out(config.pp_outfile);
			if (out) {
				// pbins.toDimacs(out); // not implemented
				out.close();
				printf("done\n");
			}
			else {
				std::cerr << "Cannot open file:" << config.pp_outfile << std::endl;
			}
		}
	}

	return done;
}


// NOTE: this is not called, consider moving parts to main_pbmc if needed
template <typename T_data>
bool PBcounter<T_data>::simplify()
{
	return true;
}


//=================================================================================================
// Auxilary helper methods:
template <typename T_data>
Glucose::Lit PBcounter<T_data>::rsLitToGlucoseLit(rs::Lit l) {
	assert(l != 0);
	Glucose::Lit gl;
	if (l > 0) {
		gl = Glucose::mkLit(rs::toVar(l), true);
	} else {
		gl = Glucose::mkLit(rs::toVar(l), false);
	}
	return gl;
}

template <typename T_data>
Glucose::Lit PBcounter<T_data>::rsLitToGlucoseLit(rs::Lit l, bool offset) {
	assert(l != 0);
	Glucose::Lit gl;
	if (l > 0) {
		if (offset) {
			gl = Glucose::mkLit(rs::toVar(l)-1, true);
		} else {
			gl = Glucose::mkLit(rs::toVar(l), true);
		}
	} else {
		if (offset) {
			gl = Glucose::mkLit(rs::toVar(l)-1, false);
		} else {
			gl = Glucose::mkLit(rs::toVar(l), false);
		}
	}
	return gl;
}

template <typename T_data>
rs::Lit PBcounter<T_data>::glucoseLitToRSLit(Glucose::Lit l) {
	int glInt = Glucose::toInt(l);
	bool sign = (glInt % 2) != 0; // if 1 means it is a positive lit, else negative
	rs::Lit rsLit;
	if (sign) {
		rsLit = (glInt - 1) / 2;
	} else {
		rsLit = -(glInt / 2);
	}
	return rsLit;
}

template <typename T_data>
void PBcounter<T_data>::syncAssign() {
	// off by 1 because assign should be 0 index as passing to cmpmgr
	// Pos index according to variables
	for (rs::Var v=1; v < Pos.size(); v++) {
		if (Pos[v] == rs::INF) {
			// assigns[v] = l_Undef;
			assigns[v-1] = l_Undef;
		} else {
			// assigns[v] = Glucose::lbool(!(trail[Pos[v]] > 0));
			// 0 is true, 1 is false for lbool
			if (trail[Pos[v]] > 0 ) {
				assigns[v-1] = l_True;
			} else if (trail[Pos[v]] < 0 ) {
				assigns[v-1] = l_False;
			} else {
				// cannot be 0 in trail
				assert(false);
			}
		}
	}
	return;
}

template <typename T_data>
void PBcounter<T_data>::copyPBInsNPVar() {
	npvars = pbins.npvars;
}

template<typename T_data>
void PBcounter<T_data>::copyPBInsPhase() {
	for (int i = 0; i < pbins.phase.size(); i++) {
		// phase[i+1] = pbins.phase[i]; // phase in counter is 1 indexed, pashe in pbins is 0 indexed
		if (pbins.phase[i] > 0) {
			phase[i+1] = i+1;
		} else {
			phase[i+1] = -(i+1);
		}
	}
	return;
}

//=================================================================================================
// RS solver state checkpoint helper methods:
// for backjump simulations

template <typename T_data>
template <typename V_data>
void PBcounter<T_data>::checkpointVector(std::vector<V_data> &source, std::vector<V_data> &target) {
	for (int i=0; i < source.size(); i++) {
		if (i >= target.size()) {
			// target.push_back(source[i]);
			target.emplace_back(source[i]);
			continue;
		}
		if (!(source[i] == target[i])) {
			target[i] = source[i];
		}
	}
	while (target.size() > source.size()) {
		target.pop_back();
	}
}

template <typename T_data>
void PBcounter<T_data>::checkpointSolverState(){
	assert(has_checkpoint_simulated_state == false);
	// copy _Level and Level
	assert(_Sim_Level.size() == _Level.size());
	for (int i=0; i < _Level.size(); i++) {
		if (_Sim_Level[i] != _Level[i]) {
			_Sim_Level[i] = _Level[i];
		}
	}
	// copy _adj and adj
	assert(_adj.size() == _sim_adj.size());
	for (int i=0; i < _adj.size(); i++) {
		if (!(_adj[i] == _sim_adj[i])) {
			checkpointVector(_adj[i], _sim_adj[i]); // for each literal watch vector, check if same, otherwise checkpoint
		}
	}
	checkpointVector(trail, sim_trail);
	checkpointVector(trail_lim, sim_trail_lim);
	checkpointVector(Pos, sim_Pos);
	has_checkpoint_simulated_state = true;
	return;
}

template <typename T_data>
void PBcounter<T_data>::resumeSolverCheckpoint(){
	assert(has_checkpoint_simulated_state == true);
	// resume slack from backjumping
	assert(trail.size() <= sim_trail.size());
	for (int i=trail.size(); i < sim_trail.size(); i++) {
		rs::Lit cur_lit = sim_trail[i];
	}
	// resume trail, trail_lim, pos
	checkpointVector(sim_trail, trail);
	checkpointVector(sim_trail_lim, trail_lim);
	checkpointVector(sim_Pos, Pos);
	// resume level
	assert(_Sim_Level.size() == _Level.size());
	for (int i=0; i < _Level.size(); i++) {
		if (_Sim_Level[i] != _Level[i]) {
			_Level[i] = _Sim_Level[i];
		}
	}
	assert(_adj.size() == _sim_adj.size());
	for (int i=0; i < _adj.size(); i++) {
		if (!(_adj[i] == _sim_adj[i])) {
			checkpointVector(_sim_adj[i], _adj[i]); // for each literal watch vector, check if same, otherwise checkpoint
		}
	}
	has_checkpoint_simulated_state = false;
	return;
}

//=================================================================================================
// RS solver modified methods:

// from rs solver, just removed the processLearntStack call, to call processLearntStack in conflict analysis routine instead
template <typename T_data>
rs::CeSuper PBcounter<T_data>::runPropagationWithoutLearnt(bool onlyUnitPropagation){
	while (qhead < (int)trail.size()) {
		rs::Lit p = trail[qhead++];
		std::vector<rs::Watch>& ws = adj[-p];
		for (int it_ws = 0; it_ws < (int)ws.size(); ++it_ws) {
			int idx = ws[it_ws].idx;
			if (idx < 0 && rs::isTrue(Level, idx + rs::INF)) {
				assert(dynamic_cast<rs::Clause*>(&(ca[ws[it_ws].cref])) != nullptr);
				continue;
			}  // blocked literal check
			rs::CRef cr = ws[it_ws].cref;
			rs::WatchStatus wstat = checkForPropagation(cr, ws[it_ws].idx, -p);
			if (wstat == rs::WatchStatus::DROPWATCH)
				rs::aux::swapErase(ws, it_ws--);
			else if (wstat == rs::WatchStatus::CONFLICTING) {  // clean up current level and stop propagation
				++rs::stats.NTRAILPOPS;
				for (int i = 0; i <= it_ws; ++i) {
					const rs::Watch& w = ws[i];
					if (w.idx >= rs::INF) ca[w.cref].undoFalsified(w.idx);
				}
				--qhead;
				rs::Constr& C = ca[cr];
				if (!C.isLocked()) {
					cBumpActivity(C);
					recomputeLBD(C);
				}

				rs::stats.NENCFORMULA += C.getOrigin() == rs::Origin::FORMULA;
				rs::stats.NENCLEARNED += C.getOrigin() == rs::Origin::LEARNED;
				rs::stats.NENCBOUND += (C.getOrigin() == rs::Origin::LOWERBOUND || C.getOrigin() == rs::Origin::UPPERBOUND ||
									C.getOrigin() == rs::Origin::HARDENEDBOUND);
				rs::stats.NENCCOREGUIDED += C.getOrigin() == rs::Origin::COREGUIDED;
				rs::stats.NLPENCGOMORY += C.getOrigin() == rs::Origin::GOMORY;
				rs::stats.NLPENCLEARNEDFARKAS += C.getOrigin() == rs::Origin::LEARNEDFARKAS;
				rs::stats.NLPENCFARKAS += C.getOrigin() == rs::Origin::FARKAS;

				return C.toExpanded(cePools);
			}
		}
	}
	if (onlyUnitPropagation) return rs::CeNull();
	return rs::CeNull();
}

//=================================================================================================
// Major methods:
template <typename T_data>
bool PBcounter<T_data>::countModels()
{

	if (progress != PREPROCESSED) {
		printf("c o The instance is not preprocessed, or model counting is already done.\n");
		return false;
	}

	import(); 
	setSearchBelowNonProjectionVarOrder(); // setting variable ordering for solveSATFixedOrder
	solves = starts = 0;
	count_main();
	// cancelUntil(0);
	backjumpTo(0);

	progress = rs::asynch_interrupt ? FAILED : COMPLETED;
	return !rs::asynch_interrupt;
}

template <typename T_data>
void PBcounter<T_data>::count_main()
{
	int          backtrack_level;
	vec<Lit>     learnt_clause,selectors;
	// vec<Lit>     selectors;
	vector<rs::CeSuper> out_constraint_vec; // added to pass around cesuper objects from function calls
	unsigned int nblevels,szWoutSelectors;

	btStateT bstate = RESOLVED;
	limlevel = 0;
	cmpmgr.init(nVars(),nPVars(),this->constraints,this->ca);

	for(;;) {
		assert(!cmpmgr.topDecision().hasUnprocessedSplitComp());
		if(rs::asynch_interrupt) return;

		int bpos = trail.size();
		for(int i = 0; i < unitcls.size(); i++){
			decide(unitcls[i]);
			activity[rs::toVar(unitcls[i])] = 0; // activitiy here calls rs:Solver:::activity
		}
		if(decisionLevel() == 0) unitcls.clear();
		rs::CeSuper confl = runPropagationWithoutLearnt(true);
		if(config.watchCand) {
			for(int i = bpos; i < trail.size(); i++){
				// offset by 1 when passing to cmpmgr
				if( (rs::toVar(trail[i]) -1) < npvars && cmpmgr.isDecCand((rs::toVar(trail[i])) -1)) {
					if(wc) cmpmgr.topDecision().mulBranchWeight(lit_weight[toInt(rsLitToGlucoseLit(trail[i],true))]);
					if(config.ddnnf) cmpmgr.addLitNode(rsLitToGlucoseLit(trail[i], true));
				}
			}
		}

		if(confl){
			// CONFLICT
			if(config.ddnnf) cmpmgr.addNode(BOTTOM_NODE);

			conflicts++;

			if(decisionLevel() == 0) break;

			// selectors just used for selecting literals in conflict analysis, to put into learnt_clause, which is just a single clause from conflict analysis.
			learnt_clause.clear();
			selectors.clear();

			out_constraint_vec.clear();

			bool suc = analyzePBMC2(confl, out_constraint_vec, backtrack_level, nblevels);
			if(!suc) {
				nbackjumps_sp++;
				bjResolve(backtrack_level);
				bstate = RESOLVED;
				continue;
			}
			rs::CRef cr = rs::CRef_Undef; // rs CRef_Undef same as Glucose CRef_Undef

			assert(out_constraint_vec.size() == 1);
			rs::CeSuper learned = out_constraint_vec.back();
			out_constraint_vec.pop_back();

			learned->heuristicWeakening(Sim_Level, sim_Pos, rs::stats);
			learned->postProcess(Sim_Level, sim_Pos, false, rs::stats);
			assert(learned->isSaturated());

			int lim_backtrack_level = std::max(backtrack_level, limlevel);
			bstate = ResolveWithoutLit(backtrack_level, learned);

			vDecayActivity();
			cDecayActivity();
		} else {
			// NO CONFLICT
			assert(!cmpmgr.topDecision().hasUnprocessedSplitComp());

			// SPLIT THE CURRENT COMPONENT
			syncAssign(); // syncing assign with current Pos and trail for splitComponent below
			int nsplitcomps = cmpmgr.splitComponent(assigns, lit_weight); 
			if(nsplitcomps == 0) {
				// NO NEW COMPONENT (#MODELS of the current component is found)
				cmpmgr.topDecision().increaseModels((T_data)1, true);
				bstate = backtrack();
			}
			else {
				bstate = GO_TO_NEXT_COMP;

				// PROCESS COMPONENTS WITHOUT PROJECTION VARS FIRST
				lbool sat_status = l_Undef;
				while(cmpmgr.topDecision().hasUnprocessedSplitComp()
						&& !cmpmgr.topComponent().hasPVar()) {
					// sat_status = solveSAT(); // SAT solving, i.e., try to find only one model
					sat_status = solveSATFixedOrder(); // SAT solving, i.e., try to find only one model

					if(sat_status == l_True) {
						sats++;
						cmpmgr.topDecision().increaseModels((T_data)1, true);
						if(!cmpmgr.topComponent().hasPVar())
							cmpmgr.cacheModelCountOf(cmpmgr.topComponent().id(),1,TOP_NODE);
						cmpmgr.eraseComponentStackID();
						cmpmgr.popComponent();
					}
					else if(sat_status == l_False) break;
					else { assert(rs::asynch_interrupt); rs::asynch_interrupt = true; return;}
				}

				if(sat_status == l_False){
					while(cmpmgr.topDecision().hasUnprocessedSplitComp())
						cmpmgr.popComponent();
					cmpmgr.removeCachePollutions();

					if(!last_suc) {
						nbackjumps_sp++;
						bjResolve(last_bklevel);
						bstate = RESOLVED;
						continue;
					}

					bstate = Resolve(last_bklevel, last_cr, last_lit);
				} else if(!cmpmgr.topDecision().hasUnprocessedSplitComp()){
					bstate = backtrack();
				}
			}
		}

		if(bstate == EXIT) break;
		else if(bstate == RESOLVED) continue;

		assert(cmpmgr.topComponent().hasPVar());

		if (on_simp && decisionLevel() <= limlevel && cmpmgr.checkfixedDL() && !simplify()) {
			cmpmgr.topDecision().increaseModels((T_data)0, false);
			if(config.ddnnf) cmpmgr.addNode(BOTTOM_NODE);
			bstate = backtrack();
			if(bstate == EXIT) break;
			else { assert(false); abort(); }
		}

		// cleanup from rs solver
		if (rs::stats.NCONFL >= (rs::stats.NCLEANUP + 1) * nconfl_to_reduce) {
			++rs::stats.NCLEANUP;
			reduceDB();
			while (rs::stats.NCONFL >= rs::stats.NCLEANUP * nconfl_to_reduce) nconfl_to_reduce += rs::options.dbCleanInc.get();
		}

		// DECIDE A LITERAL FROM TOP COMPONENT
		Var dec_var = cmpmgr.pickBranchVar(activity, exscore);
		decisions++;

		Lit dlit = mkLit(dec_var+1, (phase.at(dec_var +1) > 0) ); 
		Lit offsetDlit = mkLit(dec_var, (phase.at(dec_var +1) > 0) ); 
		decide(glucoseLitToRSLit(dlit));
		cmpmgr.pushDecision(trail.size());
		if(config.watchCand) {
			if(wc) cmpmgr.topDecision().setBranchWeight(lit_weight[toInt(offsetDlit)]);
			if(config.ddnnf) cmpmgr.setDecisionNode(offsetDlit);
			cmpmgr.setDecCand();
		}
	}
	sat = cmpmgr.topDecision().hasModel();
	if(config.ddnnf) {
		cmpmgr.setRoot(cmpmgr.makeBranchNode(UNDEF_VAL)); // abuse makeBranchNode ...
		cmpmgr.NodeManager().compressANDs();
	}

	if(wc)
		npmodels = cmpmgr.topDecision().totalModels() * gweight;
	else
		npmodels = cmpmgr.topDecision().totalModels() << nIsoPVars();
}

template <typename T_data>
bool PBcounter<T_data>::analyzePBMC2(rs::CeSuper conflict, vector<rs::CeSuper>& out_learnt, int& out_btlevel, unsigned int& lbd) {
	assert(conflict->hasNegativeSlack(Level));

	if (logger) logger->logComment("Analyze", rs::stats);
	rs::stats.NADDEDLITERALS += conflict->vars.size();

	rs::CeSuper confl = prepareConflictConstraint(conflict);
	confl->sortInDecreasingCoefOrder(); // so that asserts in getAssertionLevel() are correct
	assignActiveSet(confl);

	int sim_decision_level = decisionLevel();
	checkpointVector(_Level, _Sim_Level); // cloning _Level to _Sim_Level
	checkpointVector(Pos, sim_Pos); // cloning Pos to sim_Pos
	int dl_trail_sim_idx_pointer_start = trail_lim[sim_decision_level-1];
	int trail_end_sim_idx_pointer = trail.size() -1;
	while (sim_decision_level > 0) {
		dl_trail_sim_idx_pointer_start = trail_lim[sim_decision_level-1];
		rs::Lit l = trail[trail_end_sim_idx_pointer];
		if (confl->hasLit(-l)) {
			assert(confl->hasNegativeSlack(Sim_Level));
			rs::AssertionStatus status = confl->isAssertingBefore(Sim_Level, sim_decision_level);
			// Conflict constraint could now be asserting after removing some assignments.
			if (status == rs::AssertionStatus::ASSERTING) break;
			// Constraint is already falsified by before last decision on trail.
			else if (status == rs::AssertionStatus::FALSIFIED) {
				assert(sim_decision_level >= 0);
				for (int i = dl_trail_sim_idx_pointer_start; i < trail.size(); i++) {
					rs::Lit c_lit = trail[i];
					rs::Var c_var = rs::toVar(c_lit);
					if (Sim_Level[c_lit] == rs::INF && sim_Pos[c_var] == rs::INF) {
						break; // break for loop, reached previous round simulated remove
					}
					Sim_Level[c_lit] = rs::INF;
					sim_Pos[c_var] = rs::INF;
				}
				trail_end_sim_idx_pointer = dl_trail_sim_idx_pointer_start - 1;
				sim_decision_level--;
				continue;
			}

			rs::Constr& reasonC = getReasonConstraint(l);
			reasonC.resolveWith(confl, l, &actSet, *this, sim_Pos, Sim_Level); // resolve with simulated state
		}
		assert(!(sim_decision_level < 0));
		rs::Var v = rs::toVar(l);
		Sim_Level[l] = rs::INF;
		sim_Pos[v] = rs::INF;
		trail_end_sim_idx_pointer--;
		if (dl_trail_sim_idx_pointer_start > trail_end_sim_idx_pointer) {
			sim_decision_level--;
		}
	}
	bumpLiteralActivity();

	if(sim_decision_level <= 0) {
		// cannot be resolved, failure
		out_btlevel = 0; // restart
		return false;
	}

	assert(confl->hasNegativeSlack(Sim_Level));

	rs::CeSuper learned = confl->clone(cePools);
	learned->orig = rs::Origin::LEARNED;
	
	learned->removeUnitsAndZeroes(Sim_Level, sim_Pos);
	learned->sortInDecreasingCoefOrder();
	int assertionLevel = learned->getAssertionLevel(Sim_Level, sim_Pos); 
	if (assertionLevel < 0) {
		out_btlevel = 0;
		return false; // failed
	}
	out_btlevel = assertionLevel;
	out_learnt.push_back(learned);
	return true;
}


template <typename T_data>
inline void PBcounter<T_data>::bjResolve(int level) {
	backjumpTo(level); // this is roundingsat backtrack function
	cmpmgr.backjumpTo(level); // backjumpto is the component manager's backtrack function
	cmpmgr.removeCachePollutions();
	cmpmgr.topDecision().increaseModels((T_data)0, false);	// reset
	if(config.ddnnf) cmpmgr.removeChildComponentNodes();
}

template <typename T_data>
inline btStateT PBcounter<T_data>::Resolve(int bk_level, rs::CRef cr, rs::Lit lit) {
	int level = std::max(bk_level, limlevel);
	bool canjump = on_bj && level+1 < decisionLevel() && (double)level/decisionLevel() < config.bj_threshold;
	if(canjump) {
		nbackjumps++;
		bjResolve(level);
		if(cr != rs::CRef_Undef) {
			uncheckedEnqueue(lit, cr);
			if(config.watchCand) {
				if((rs::toVar(lit) -1 ) < npvars && cmpmgr.isDecCand((rs::toVar(lit))-1 )) {
					if(wc) cmpmgr.topDecision().mulBranchWeight(lit_weight[toInt(rsLitToGlucoseLit(lit, true))]);
					// if(config.ddnnf) cmpmgr.addLitNode(rsLitToGlucoseLit(lit));
					if(config.ddnnf) cmpmgr.addLitNode(rsLitToGlucoseLit(lit, true));
				}
			}
		}
		if(nbackjumps > nPVars()) {
			on_bj = false;
		}
		return RESOLVED;
	}
	else {
		cmpmgr.topDecision().increaseModels((T_data)0, false);	// set 0 (no model found at the current branch)
		if(config.ddnnf) cmpmgr.addNode(BOTTOM_NODE);
		return backtrack();
	}
}

template <typename T_data>
// this version of resolve does not have asserting lit but instead flags the situation so that propagation next round can handle the lit
inline btStateT PBcounter<T_data>::ResolveWithoutLit(int bk_level, rs::CeSuper& constraint) {
	int level = std::max(bk_level, limlevel);
	bool canjump = on_bj && level+1 < decisionLevel() && (double)level/decisionLevel() < config.bj_threshold;
	if(canjump) {
		nbackjumps++;
		bjResolve(level);
		// backjump done, so we can add constraint to ca
		rs::CRef cr = attachConstraint(constraint, false);
		rs::Constr& C = ca[cr];
		if (bk_level < rs::INF) {
			recomputeLBD(C);
		} else {
			C.setLBD(C.size());
		}
		cBumpActivity(ca[cr]);

		if(nbackjumps > nPVars()) {
			on_bj = false;
		}
		return RESOLVED;
	}
	else {
		cmpmgr.topDecision().increaseModels((T_data)0, false);	// set 0 (no model found at the current branch)
		if(config.ddnnf) cmpmgr.addNode(BOTTOM_NODE);
		return backtrack();
	}
}

template <typename T_data>
btStateT PBcounter<T_data>::backtrack() {
	if (decisionLevel() == 0)
		return EXIT;
	for (;;) {
		assert(decisionLevel() > 0);
		assert(!cmpmgr.topDecision().hasUnprocessedSplitComp());

		if(wc) cmpmgr.topDecision().mulLitsWeights();
		if (cmpmgr.topDecision().isFirstBranch()) {
			// FirstBranch
			rs::Lit dlit = trail[trail_lim.back()];
			cancelCurDL();
			decide(-dlit);

			limlevel = decisionLevel();
			cmpmgr.topDecision().changeBranch();
			if(wc) cmpmgr.topDecision().setBranchWeight(lit_weight[toInt(rsLitToGlucoseLit(-dlit, true))]); 
			if(config.ddnnf) cmpmgr.setDecisionNode(rsLitToGlucoseLit(-dlit, true));
			return RESOLVED;
		} else {
			// SecondBranch
			cmpmgr.prevDecision().increaseModels(
					cmpmgr.topDecision().totalModels(), cmpmgr.topDecision().hasModel());
			NodeIndex snode = UNDEF_NODE;
			if(config.ddnnf) {
				// Var x = var(trail[trail_lim.last()]);
				rs::Var x = rs::toVar(trail[trail_lim.back()]);
				// snode = cmpmgr.makeBranchNode(x);
				assert(x-1 >= 0);
				snode = cmpmgr.makeBranchNode(x - 1);
				if(snode != BOTTOM_NODE)
					cmpmgr.addNode(snode, cmpmgr.prevDecision().getNodeIdx());
			}
			cmpmgr.cacheModelCountOf(cmpmgr.topComponent().id(), cmpmgr.topDecision().totalModels(), snode);
			cmpmgr.eraseComponentStackID();

			cancelCurDL();
			cmpmgr.popDecision();
			cmpmgr.popComponent();

			if (cmpmgr.topDecision().isUnSAT()) {
				while (cmpmgr.topDecision().hasUnprocessedSplitComp())
					cmpmgr.popComponent();
				cmpmgr.removeCachePollutions();
				if(config.ddnnf) cmpmgr.addNode(BOTTOM_NODE);
			} else if (cmpmgr.topDecision().hasUnprocessedSplitComp()) {
				limlevel = decisionLevel() + 1;
				return GO_TO_NEXT_COMP;
			}

			if (decisionLevel() == 0)
				return EXIT;
		}
	}

	assert(false);
	return EXIT;
}

template <typename T_data>
void PBcounter<T_data>::cancelCurDL() {
	backjumpTo(decisionLevel()-1);
	syncAssign();
}

// included for convenience to match rs with glucose, to use when solving in solveSAT
template <typename T_data>
void PBcounter<T_data>::newDecisionLevel() {
	trail_lim.push_back(trail.size());
}

template <typename T_data>
lbool PBcounter<T_data>::solveSAT(void) {
	int sat_start_dl = decisionLevel();

	vec<Var> vs; // coming from component, this should be glucose 0 index var
	Var v;
	Component & c = cmpmgr.topComponent();
	syncAssign();
	for (auto i = 0; (v = c[i]) != var_Undef; i++) {
		assert(assigns[v] == l_Undef);
		if (assigns[v] == l_Undef) {vs.push(v);}
	}

	// saving what is in the order heap at the moment
	std::vector<rs::Var> temp_order_heap_elements = order_heap.tree;
	order_heap.clear();
	std::vector<rs::ActValV> temp_activity = activity;

	// filling counter order heap with subset of variables from component manager
	for(int i = 0; i < vs.size(); i++) {
		order_heap.insert(vs[i] + 1); // since vs is 0 indexed variables and order heap is 1 indexed variables 
	}


	solves++;

	conflictsRestarts = 1;

	uint64_t old_conflicts = conflicts;
	uint64_t old_decisions = decisions;
	uint64_t old_propagations = propagations;

	lbool status = l_Undef;
	while (status == l_Undef && !rs::asynch_interrupt) {
		status = searchBelow(sat_start_dl);
	}

	conflicts_sg += conflicts - old_conflicts;
	decisions_sg += decisions - old_decisions;
	propagations_sg += propagations - old_propagations;

	backjumpTo(sat_start_dl);

	// restoring activity values
	for(int i=0; i<activity.size(); i++) {
		activity[i] = temp_activity[i];
	}
	// restoring order heap elements here
	for (rs::Var var : temp_order_heap_elements) {
		order_heap.insert(var);
	}
	return status;
}


template <typename T_data>
lbool PBcounter<T_data>::solveSATFixedOrder(void) {
	int sat_start_dl = decisionLevel();
	search_below_variable_order_stack.clear();
	solve_sat_stack_sorting_set.clear();

	Var v;
	Component & c = cmpmgr.topComponent();
	syncAssign();
	for (auto i = 0; (v = c[i]) != var_Undef; i++) {
		assert(assigns[v] == l_Undef);
		if (assigns[v] == l_Undef) {
			solve_sat_stack_sorting_set.insert(v+1); // ordering stack should be 1 indexed variables
		}
	}
	for (int i=0; i < search_below_variable_order.size(); i++) {
		if (solve_sat_stack_sorting_set.find(search_below_variable_order[i]) != solve_sat_stack_sorting_set.end()) {
			search_below_variable_order_stack.push_back(search_below_variable_order[i]);
		}
	}

	solves++;
	conflictsRestarts = 1;

	uint64_t old_conflicts = conflicts;
	uint64_t old_decisions = decisions;
	uint64_t old_propagations = propagations;

	lbool status = l_Undef;
	while (status == l_Undef && !rs::asynch_interrupt) {
		// status = searchBelow(sat_start_dl);
		status = searchBelowFixedOrder(sat_start_dl);
	}

	conflicts_sg += conflicts - old_conflicts;
	decisions_sg += decisions - old_decisions;
	propagations_sg += propagations - old_propagations;

	backjumpTo(sat_start_dl);
	return status;
}

/**
 * - runpropagationwithlimitlevel
 * - processLearnStackWithLimit
 * 		- point is to return failure if resolution backtracks to the threshold limit decision level
 * -> the runpropagationwithlimit should then detect if processlearnedstack failed, if failed then return some dummy conflict object so that the main loop detects decisionlevel <= limitlevel and returns unsat
*/

template <typename T_data>
lbool PBcounter<T_data>::searchBelowFixedOrder(int start_dl) {
	firstRun = false;
	std::vector<int> assumptions_lim = {0};
	assumptions_lim.reserve((int)assumptions.size() + 1);
	bool runLP = false;
	while (true) {
		if (rs::asynch_interrupt || (rs::options.time_limit.get() != -1.0 && rs::stats.getTime() > rs::options.time_limit.get()))
			throw rs::asynchInterrupt;
		std::pair<rs::CeSuper, bool> prop_result = rs::aux::timeCall<std::pair<rs::CeSuper, bool>>([&] { return runPropagationWithLimitLevel(runLP, start_dl); }, rs::stats.PROPTIME);
		rs::CeSuper confl = prop_result.first;
		bool prop_suc = prop_result.second;
		runLP = !confl;
		if (confl) {
			assert(confl->hasNegativeSlack(Level));
			vDecayActivity();
			cDecayActivity();
			rs::stats.NCONFL++;
			nconfl_to_restart--;
			if (decisionLevel() <= start_dl) {
				assert(!prop_suc); // must have had a conflict from failing to backtrack to assertion level which is above start_dl.
				return l_False;
			} else if (decisionLevel() >= (int)assumptions_lim.size()) {
				std::pair<rs::CeSuper, bool> analyze_result = rs::aux::timeCall<std::pair<rs::CeSuper, bool>>([&] { return analyzeWithLimitLevel(confl, start_dl); }, rs::stats.CATIME);
				rs::CeSuper analyzed = analyze_result.first;
				bool analyze_suc = analyze_result.second;
				if (!analyze_suc) {
					return l_False; // unsat since assertion level needs to go above start_dl
				}
				assert(analyzed);
				assert(analyzed->hasNegativeSlack(getLevel()));
				assert(analyzed->isSaturated());
				if (learnedStack.size() > 0 && learnedStack.back()->orig == rs::Origin::FARKAS)
					learnConstraint(analyzed, rs::Origin::LEARNEDFARKAS);
				else
					learnConstraint(analyzed, rs::Origin::LEARNED);
			} else {
				return l_False;
			}
		} else {  // no conflict
			if (nconfl_to_restart <= 0) {
				// restarts, just start from start_dl again
				backjumpTo(start_dl);
				double rest_base = luby(rs::options.lubyBase.get(), ++rs::stats.NRESTARTS);
				nconfl_to_restart = (long long)rest_base * rs::options.lubyMult.get();
			}
			if (rs::stats.NCONFL >= (rs::stats.NCLEANUP + 1) * nconfl_to_reduce) {
				if (rs::options.verbosity.get() > 0) puts("c INPROCESSING");
				++rs::stats.NCLEANUP;
				reduceDB();
				while (rs::stats.NCONFL >= rs::stats.NCLEANUP * nconfl_to_reduce) nconfl_to_reduce += rs::options.dbCleanInc.get();
				if (lpSolver) rs::aux::timeCall<void>([&] { lpSolver->inProcess(); }, rs::stats.LPTOTALTIME);
				return l_Undef; // signalling not completed to the main solveSAT loop
			}
			rs::Lit next = 0;
			if ((int)assumptions_lim.size() > decisionLevel() + 1) assumptions_lim.resize(decisionLevel() + 1);
			if (assumptions_lim.back() < (int)assumptions.size()) {
				for (int i = (decisionLevel() == 0 ? 0 : trail_lim.back()); i < (int)trail.size(); ++i) {
					rs::Lit l = trail[i];
					if (assumptions.has(-l)) {  // found conflicting assumption
						return l_False; // signalling unsat
					}
				}
			}
			while (assumptions_lim.back() < (int)assumptions.size()) {
				assert(decisionLevel() == (int)assumptions_lim.size() - 1);
				rs::Lit l_assump = assumptions.getKeys()[assumptions_lim.back()];
				assert(!rs::isFalse(Level, l_assump));  // otherwise above check should have caught this
				if (rs::isTrue(Level, l_assump)) {      // assumption already propagated
					++assumptions_lim.back();
				} else {  // unassigned assumption
					next = l_assump;
					assumptions_lim.push_back(assumptions_lim.back() + 1);
					break;
				}
			}
			if (next == 0) next = pickBranchLitFixedOrder(assumptions.isEmpty() && rs::options.cgSolutionPhase);
			if (next == 0) {
				assert(search_below_variable_order_stack.empty());
				lastSol.clear();
				lastSol.resize(getNbVars() + 1);
				lastSol[0] = 0;
				for (Var v = 1; v <= getNbVars(); ++v) lastSol[v] = rs::isTrue(Level, v) ? v : -v;
				backjumpTo(start_dl);
				return l_True;
			}
			decide(next);
		}
	}
}

template<typename T_data>
rs::Lit PBcounter<T_data>::pickBranchLitFixedOrder(bool lastSolPhase) {
	rs::Var next = 0;
	while (next == 0 || !rs::isUnknown(Pos, next)) {
		if (search_below_variable_order_stack.empty()) {
			return 0;
		} else {
			next = search_below_variable_order_stack.back();
			search_below_variable_order_stack.pop_back();
		}
	}
	assert(phase[0] == 0);
	assert(lastSol[0] == 0);
	return (lastSolPhase && (int)lastSol.size() > next) ? lastSol[next] : phase[next];
	// return next;
}


template <typename T_data>
lbool PBcounter<T_data>::searchBelow(int start_dl) {
	firstRun = false;
	std::vector<int> assumptions_lim = {0};
	assumptions_lim.reserve((int)assumptions.size() + 1);
	bool runLP = false;
	while (true) {
		if (rs::asynch_interrupt || (rs::options.time_limit.get() != -1.0 && rs::stats.getTime() > rs::options.time_limit.get()))
			throw rs::asynchInterrupt;
		std::pair<rs::CeSuper, bool> prop_result = rs::aux::timeCall<std::pair<rs::CeSuper, bool>>([&] { return runPropagationWithLimitLevel(runLP, start_dl); }, rs::stats.PROPTIME);
		rs::CeSuper confl = prop_result.first;
		bool prop_suc = prop_result.second;
		runLP = !confl;
		if (confl) {
			assert(confl->hasNegativeSlack(Level));
			vDecayActivity();
			cDecayActivity();
			rs::stats.NCONFL++;
			nconfl_to_restart--;
			if (decisionLevel() <= start_dl) {
				assert(!prop_suc); // must have had a conflict from failing to backtrack to assertion level which is above start_dl.
				return l_False;
			} else if (decisionLevel() >= (int)assumptions_lim.size()) {
				std::pair<rs::CeSuper, bool> analyze_result = rs::aux::timeCall<std::pair<rs::CeSuper, bool>>([&] { return analyzeWithLimitLevel(confl, start_dl); }, rs::stats.CATIME);
				rs::CeSuper analyzed = analyze_result.first;
				bool analyze_suc = analyze_result.second;
				if (!analyze_suc) {
					return l_False; // unsat since assertion level needs to go above start_dl
				}
				assert(analyzed);
				assert(analyzed->hasNegativeSlack(getLevel()));
				assert(analyzed->isSaturated());
				if (learnedStack.size() > 0 && learnedStack.back()->orig == rs::Origin::FARKAS)
					learnConstraint(analyzed, rs::Origin::LEARNEDFARKAS);
				else
					learnConstraint(analyzed, rs::Origin::LEARNED);
			} else {
				return l_False;
			}
		} else {  // no conflict
			if (nconfl_to_restart <= 0) {
				// restarts, just start from start_dl again
				backjumpTo(start_dl);
				double rest_base = luby(rs::options.lubyBase.get(), ++rs::stats.NRESTARTS);
				nconfl_to_restart = (long long)rest_base * rs::options.lubyMult.get();
			}
			if (rs::stats.NCONFL >= (rs::stats.NCLEANUP + 1) * nconfl_to_reduce) {
				if (rs::options.verbosity.get() > 0) puts("c INPROCESSING");
				++rs::stats.NCLEANUP;
				reduceDB();
				while (rs::stats.NCONFL >= rs::stats.NCLEANUP * nconfl_to_reduce) nconfl_to_reduce += rs::options.dbCleanInc.get();
				if (lpSolver) rs::aux::timeCall<void>([&] { lpSolver->inProcess(); }, rs::stats.LPTOTALTIME);
				return l_Undef; // signalling not completed to the main loop
			}
			rs::Lit next = 0;
			if ((int)assumptions_lim.size() > decisionLevel() + 1) assumptions_lim.resize(decisionLevel() + 1);
			if (assumptions_lim.back() < (int)assumptions.size()) {
				for (int i = (decisionLevel() == 0 ? 0 : trail_lim.back()); i < (int)trail.size(); ++i) {
					rs::Lit l = trail[i];
					if (assumptions.has(-l)) {  // found conflicting assumption
						return l_False; // signalling unsat
					}
				}
			}
			while (assumptions_lim.back() < (int)assumptions.size()) {
				assert(decisionLevel() == (int)assumptions_lim.size() - 1);
				rs::Lit l_assump = assumptions.getKeys()[assumptions_lim.back()];
				assert(!rs::isFalse(Level, l_assump));  // otherwise above check should have caught this
				if (rs::isTrue(Level, l_assump)) {      // assumption already propagated
					++assumptions_lim.back();
				} else {  // unassigned assumption
					next = l_assump;
					assumptions_lim.push_back(assumptions_lim.back() + 1);
					break;
				}
			}
			if (next == 0) next = pickBranchLit(assumptions.isEmpty() && rs::options.cgSolutionPhase);
			if (next == 0) {
				assert(order_heap.empty());
				// assert(checkSAT()); // might not be correct assert since component might not contain all variables
				lastSol.clear();
				// We want to keep track of the full solution for e.g. branching heuristics based on last found solution
				lastSol.resize(getNbVars() + 1);
				lastSol[0] = 0;
				for (Var v = 1; v <= getNbVars(); ++v) lastSol[v] = rs::isTrue(Level, v) ? v : -v;
				backjumpTo(start_dl);
				return l_True;
			}
			decide(next);
		}
	}
}


template<typename T_data>
std::pair<rs::CeSuper, bool> PBcounter<T_data>::analyzeWithLimitLevel(rs::CeSuper conflict, int limit_level) {
	assert(conflict->hasNegativeSlack(Level));

	rs::CeSuper confl = prepareConflictConstraint(conflict);
	confl->sortInDecreasingCoefOrder();
	assignActiveSet(confl);

	// setting up temp data structure for simulated backjumping and analysis
	int sim_decision_level = decisionLevel();
	checkpointVector(_Level, _Sim_Level); // cloning _Level to _Sim_Level
	checkpointVector(Pos, sim_Pos); // cloning Pos to sim_Pos
	int dl_trail_sim_idx_pointer_start = trail_lim[sim_decision_level-1];
	int trail_end_sim_idx_pointer = trail.size() -1;

	while (sim_decision_level > 0) {
		dl_trail_sim_idx_pointer_start = trail_lim[sim_decision_level-1];
		rs::Lit l = trail[trail_end_sim_idx_pointer];
		if (confl->hasLit(-l)) {
			assert(confl->hasNegativeSlack(Sim_Level));
			rs::AssertionStatus status = confl->isAssertingBefore(Sim_Level, sim_decision_level);
			// Conflict constraint could now be asserting after removing some assignments.
			if (status == rs::AssertionStatus::ASSERTING) break;
			// Constraint is already falsified by before last decision on trail.
			else if (status == rs::AssertionStatus::FALSIFIED) {
				assert(sim_decision_level >= 0);
				for (int i = dl_trail_sim_idx_pointer_start; i < trail.size(); i++) {
					rs::Lit c_lit = trail[i];
					rs::Var c_var = rs::toVar(c_lit);
					if (Sim_Level[c_lit] == rs::INF && sim_Pos[c_var] == rs::INF) {
						break; // break for loop, reached previous round simulated remove
					}
					Sim_Level[c_lit] = rs::INF;
					sim_Pos[c_var] = rs::INF;
				}
				trail_end_sim_idx_pointer = dl_trail_sim_idx_pointer_start - 1;
				sim_decision_level--;
				continue;
			}

			rs::Constr& reasonC = getReasonConstraint(l);
			reasonC.resolveWith(confl, l, &actSet, *this, sim_Pos, Sim_Level); // resolve with simulated state
		}
		assert(!(sim_decision_level < 0));
		rs::Var v = rs::toVar(l);
		Sim_Level[l] = rs::INF;
		sim_Pos[v] = rs::INF;
		trail_end_sim_idx_pointer--;
		if (dl_trail_sim_idx_pointer_start > trail_end_sim_idx_pointer) {
			sim_decision_level--;
		}
	}
	bumpLiteralActivity();


	if (sim_decision_level < limit_level) {
		return {confl, false};
	}
	if (sim_decision_level == limit_level && !confl->hasNegativeSlack(Sim_Level)) {
		return {confl, false};
	}
	assert(confl->hasNegativeSlack(Sim_Level));
	return {confl, true};
}


template <typename T_data>
std::pair<rs::CeSuper, bool> PBcounter<T_data>::runPropagationWithLimitLevel(bool onlyUnitPropagation, int limit_level) {
	rs::CeSuper confl = processLearnedStackWithLimitLevel(limit_level);
	if (confl) {
		// essentially assertion level of learn clause < limit level, so thats why confl is returned by processLearnedStackWithLimitLevel, this indicates failure to resolve and learn conflict
		return {confl, false};
	}
	while (qhead < (int)trail.size()) {
		rs::Lit p = trail[qhead++];
		std::vector<rs::Watch>& ws = adj[-p];
		for (int it_ws = 0; it_ws < (int)ws.size(); ++it_ws) {
			int idx = ws[it_ws].idx;
			if (idx < 0 && rs::isTrue(Level, idx + rs::INF)) {
				assert(dynamic_cast<rs::Clause*>(&(ca[ws[it_ws].cref])) != nullptr);
				continue;
			}  // blocked literal check
			rs::CRef cr = ws[it_ws].cref;
			rs::WatchStatus wstat = checkForPropagation(cr, ws[it_ws].idx, -p);
			if (wstat == rs::WatchStatus::DROPWATCH)
				rs::aux::swapErase(ws, it_ws--);
			else if (wstat == rs::WatchStatus::CONFLICTING) {  // clean up current level and stop propagation
				++rs::stats.NTRAILPOPS;
				for (int i = 0; i <= it_ws; ++i) {
					const rs::Watch& w = ws[i];
					if (w.idx >= rs::INF) ca[w.cref].undoFalsified(w.idx);
				}
				--qhead;
				rs::Constr& C = ca[cr];
				if (!C.isLocked()) {
					cBumpActivity(C);
					recomputeLBD(C);
				}

				rs::stats.NENCFORMULA += C.getOrigin() == rs::Origin::FORMULA;
				rs::stats.NENCLEARNED += C.getOrigin() == rs::Origin::LEARNED;
				rs::stats.NENCBOUND += (C.getOrigin() == rs::Origin::LOWERBOUND || C.getOrigin() == rs::Origin::UPPERBOUND ||
									C.getOrigin() == rs::Origin::HARDENEDBOUND);
				rs::stats.NENCCOREGUIDED += C.getOrigin() == rs::Origin::COREGUIDED;
				rs::stats.NLPENCGOMORY += C.getOrigin() == rs::Origin::GOMORY;
				rs::stats.NLPENCLEARNEDFARKAS += C.getOrigin() == rs::Origin::LEARNEDFARKAS;
				rs::stats.NLPENCFARKAS += C.getOrigin() == rs::Origin::FARKAS;

				return {C.toExpanded(cePools), true};
			}
		}
	}
	if (onlyUnitPropagation) return {rs::CeNull(), true};
	return {rs::CeNull(), true};
}

template <typename T_data>
rs::CeSuper PBcounter<T_data>::processLearnedStackWithLimitLevel(int limit_level) {
	while (learnedStack.size() > 0) {
		rs::CeSuper learned = learnedStack.back()->toExpanded(cePools);
		learnedStack.pop_back();
		learned->removeUnitsAndZeroes(Level, Pos);
		learned->sortInDecreasingCoefOrder();
		int assertionLevel = learned->getAssertionLevel(Level, Pos);
		if (assertionLevel < limit_level) {
			// actually should still learn the constraint, just backtrack to limit level instead
			backjumpTo(limit_level);
			learned->heuristicWeakening(Level, Pos, rs::stats);
			learned->postProcess(Level, Pos, false, rs::stats);
			if (learned->isTautology()) {
				continue;
			}
			rs::CRef cr = attachConstraint(learned, false);
			rs::Constr& C = ca[cr];
			// make shift learn constraint but backtrack to limit level instead of assertionlevel above
			return learned;
		}
		backjumpTo(assertionLevel);
		assert(!learned->hasNegativeSlack(Level));
		learned->heuristicWeakening(Level, Pos, rs::stats);
		learned->postProcess(Level, Pos, false, rs::stats);
		assert(learned->isSaturated());
		if (learned->isTautology()) {
			continue;
		}
		rs::CRef cr = attachConstraint(learned, false);
		rs::Constr& C = ca[cr];
		if (assertionLevel < rs::INF)
			recomputeLBD(C);
		else
			C.setLBD(C.size());  // the LBD of non-asserting constraints is undefined, so we take a safe upper bound
	}
	return rs::CeNull();
}

//======================= methods to help set searchBelow decision variable ordering
template <typename T_data>
void PBcounter<T_data>::setSearchBelowNonProjectionVarOrder(){
	search_below_variable_order.clear(); // clearing the initial order vector
	std::vector<std::pair<double, rs::Var>> temp_exscore_variable_vector;
	for (rs::Var v=nPVars() + 1; v <= nVars(); v++) { // 1 indexed variables
		temp_exscore_variable_vector.push_back(std::make_pair(exscore[v-1], v)); // exscore is 0 indexed
	}
	// sorting in ascending order of score, so to pop back first
	std::sort(
		temp_exscore_variable_vector.begin(),
		temp_exscore_variable_vector.end(),
		[&](const auto& a, const auto& b){
			return a.first < b.first;
		}
	);
	for (const auto& element:temp_exscore_variable_vector) {
		search_below_variable_order.push_back(element.second);
	}
	return;
}

//=================================================================================================
// Print methods:
template <typename T_data>
void PBcounter<T_data>::printStats() const
{
	if(verbosity_c) {
		if(progress == COMPLETED || progress == FAILED) {
			printf("c o [Statistics]\n");
			printf("c o conflicts             = %-11"PRIu64" (count %"PRIu64", sat %"PRIu64")\n", conflicts, conflicts-conflicts_sg, conflicts_sg);
			printf("c o decisions             = %-11"PRIu64" (count %"PRIu64", sat %"PRIu64")\n", decisions, decisions-decisions_sg, decisions_sg);
			// printf("c o propagations          = %-11"PRIu64" (count %"PRIu64", sat %"PRIu64")\n", propagations, propagations-propagations_sg, propagations_sg);
			// printf("c o simp dbs              = %-11"PRIu64" (%.3f s)\n", simp_dbs, simplify_time);
			// printf("c o reduce dbs            = %-11"PRIu64"\n", nbReduceDB);
			// printf("c o learnts (uni/bin/lbd2)= %"PRIu64"/%"PRIu64"/%"PRIu64"\n", nbUn, nbBin, nbDL2);

			printStatsOfCM();
			// printf("c o isolated_pvars        = %"PRIu64"\n", nIsoPVars());
			printf("c o SAT calls             = %-11"PRIu64" (SAT %"PRIu64", UNSAT %"PRIu64")\n", solves, sats, solves-sats);
			printf("c o SAT starts            = %"PRIu64"\n", starts);
			// printf("c o backjumps             = %-11"PRIu64" (sp %"PRIu64") [init %s / final %s]\n", nbackjumps+nbackjumps_sp, nbackjumps_sp, config.backjump ? "on" : "off", on_bj ? "on" : "off");
			printf("c o\n");
		}
		if(config.ddnnf) {
			printf("c o [d-DNNF Stats]\n");
			pbins.printVarMapStats();
			const DTNodeManager& nmgr = cmpmgr.NodeManagerConst();
			nmgr.printStats();
			printf("c o\n");
		}

		fflush(stdout);
	}
}

template <typename T_data>
void PBcounter<T_data>::writeNNF() {
	if(!config.ddnnf) {
		cout << "c o No d-DNNF is constructed." << endl;
		return;
	}

	ofstream out(config.nnf_outfile);
	if (out) {
		cout << "c o writing d-DNNF to file " << config.nnf_outfile << " ... "; fflush(stdout);
		pbins.writeVarMap(out);
		cmpmgr.NodeManager().printNNF(out);
		cout << "done" << endl;;
		out.close();
	}
	else {
		cerr << "Cannot open file:" << config.nnf_outfile << endl;
	}
}

template <typename T_data>
T_data PBcounter<T_data>::mcDDNNF() {
	if(!config.ddnnf) {
		cout << "c o No d-DNNF is constructed." << endl;
		return 0;
	}

	DTNodeManager& nm = cmpmgr.NodeManager();
	if(wc)
		return nm.countModel(lit_weight, wc) * gweight;
	else
		return nm.countModel(lit_weight, wc) << nIsoPVars();
}

//=================================================================================================
// Methods for Importing instance and additional information
template <class T_data>
void PBcounter<T_data>::loadWeight()
{
	if(!wc) return;

	gweight = pbins.gweight;
	lit_weight.clear();
	lit_weight.growTo(2*npvars);
	for(int i=0; i<npvars; i++) {
		lit_weight[toInt(mkLit(i))] = pbins.renumbered_lit_weights[toInt(mkLit(i))];
		lit_weight[toInt(~mkLit(i))] = pbins.renumbered_lit_weights[toInt(~mkLit(i))];
	}
}

template <typename T_data>
void PBcounter<T_data>::import()
{
	int nvars = pbins.vars;
	assigns  .growTo(nvars, l_Undef);
	npvars = pbins.npvars;
	npvars_isolated = pbins.freevars;
	sat = !pbins.unsat;
	loadWeight();
}

template <typename T_data>
void PBcounter<T_data>::setExtraVarScore()
{
	exscore.clear();
	exscore.growTo(getNbVars(), 0); // for pb

	if(config.vs_infile != "NULL")
		setGivenVarScore();
	else if(config.doTD) {
		computeTDScore();
		// adding coefficient score
		addCoeffVarScore();
	}
}

template <typename T_data>
void PBcounter<T_data>::addCoeffVarScore()
{
	/* method to add score to exscore for each variable based on their abs coefficient impact to satisfiability */
	for (int i = 0; i < getNbVars(); i++) {
		exscore[i] = (exscore[i] + (double) pbins.varCoeffScores.at(i)) / 2;
	}
}

template <typename T_data>
void PBcounter<T_data>::computeTDScore()
{
	int num_var = getNbVars();
	int num_pvar = this->npvars;

	bool conditionOnCNF = num_var > 20 && num_var <= tdconfig.varlim;
	if(!(conditionOnCNF || config.alwTD)) {
		printf("c o skip td\n");
		return;
	}


	Graph Primal(num_var, this->constraints, this->ca); // primal for PB

	printf("c o Primal graph: nodes %d, edges %d\n", num_var, Primal.numEdges());

	bool conditionOnPrimalGraph = (double)Primal.numEdges()/((long) num_var * num_var) <= tdconfig.denselim && (double)Primal.numEdges()/num_var <= tdconfig.ratiolim;
	if(!(conditionOnPrimalGraph || config.alwTD)) {
		printf("c o skip td\n");
		return;
	}

	// run FlowCutter
	printf("c o FlowCutter is running...\n");fflush(stdout);
	// IFlowCutter FC(ins.vars, Primal.numEdges(), tdconfig.timelim);
	IFlowCutter FC(num_var, Primal.numEdges(), tdconfig.timelim);
	FC.importGraph(Primal);
	Primal.clear();
	TreeDecomposition td = FC.constructTD();

	bool uselessTD = true;
	if(td.numNodes() > 0) {	// if TD construction is successful
		// find a centroid of the constructed TD
		int centroid = td.centroid(num_pvar);

		// write the the constructed TD (if needed)
		if(config.td_outfile != "NULL") {
			ofstream out(config.td_outfile);
			if(out) {
				td.toDimacs(out, centroid+1, num_pvar);
				out.close();
			} else {
				std::cerr << "Cannot open file:" << config.td_outfile << std::endl;
			}
		}

		bool conditionOnTreeWidth = (double)td.width()/num_pvar < tdconfig.twvarlim;
		if(conditionOnTreeWidth || config.alwTD) {
			std::vector<int> dists = td.distanceFromCentroid(num_pvar);
			if(!dists.empty()) {
				int max_dst = 0;
				for(int i=0; i<num_var; i++)
					max_dst = max(max_dst, dists[i]);
				if(max_dst > 0) {
					for(int i=0; i<num_pvar; i++)
						exscore[i] = config.coef_tdscore * ((double)(max_dst - dists[i])) / (double)max_dst;
					uselessTD = false;
				}
			}
		}
	}

	if(uselessTD)
		printf("c o ignore td\n");
}

template <typename T_data>
void PBcounter<T_data>::setGivenVarScore() {
	if(!config.keepVarMap || pbins.score.size() == 0) return;

	for(int i=0; i<pbins.gmap.size(); i++) {
		if(pbins.gmap[i] == lit_Undef) continue;

		Var x = var(pbins.gmap[i]);
		assert(x < pbins.npvars);
		if(exscore[x] < pbins.score[i])
			exscore[x] = pbins.score[i];
	}
	pbins.score.clear();
	pbins.score.shrink_to_fit();
}

template class GPMC::PBcounter<mpz_class>;
template class GPMC::PBcounter<mpfr::mpreal>;