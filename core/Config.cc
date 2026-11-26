/*
Copyright (c) 2025 Grabtaxi Holdings Pte Ltd (GRAB), All rights reserved.

Use of this source code is governed by an MIT-style license that can be found in the LICENSE file
*/

#include "core/Config.h"

#include <stdexcept>

using namespace GPMC;

//=================================================================================================
// Options:

// static const char* omain = "GPMC -- MAIN";
static const char* omain = "PBMC -- MAIN";
static IntOption		opt_mode		(omain, "mode", "Counting mode (0=mc, 1=wmc, 2=pmc, 3=wpmc).", 0, IntRange(0, 3));
static IntOption		opt_precision	(omain, "prec", "Precision of output of weighted model counting", 15, IntRange(15,INT32_MAX));
static IntOption		opt_vs			(omain, "vs", "Variable Selection Heuristics", 1, IntRange(0,1));
static IntOption		opt_cachesize	(omain, "cs", "Maximum component cache size (MB) (not strict)", 4000, IntRange(1, INT32_MAX));
static BoolOption		opt_bj			(omain, "bj", "Backjumping", true);
static DoubleOption	opt_bj_thd		(omain, "bjthd", "Backjumping threshold", 0.5, DoubleRange(0, false, 1, false));
static BoolOption		opt_inprocess	(omain, "inprocess", "Enable recursive PB simplification (exact MC only)", false);
static StringOption	opt_ablation	(omain, "ablation", "Disabled simplification IDs separated by ':' (1=Basic, 2=BICP, 3=CS, 4=SC, 5=GCD, 6=Cardinality)", "");
static StringOption	opt_simpcache	(omain, "simp-cache", "Component cache used by recursive simplification (legacy, bypass, or residual)", "legacy");
static DoubleOption	opt_coef		(omain, "coef", "TDscore coefficient", 100, DoubleRange(0, true, 10000000, true));
static BoolOption		opt_natw		(omain, "natw", "Natural number weight. If off, real number weight ([0, 1])", false);
static BoolOption		opt_ddnnf		(omain, "ddnnf", "Constructing a d-DNNF", false);
static StringOption	opt_nnfout		(omain, "nnfout", "Outfile for d-DNNF", "NULL");
static StringOption	opt_varscore	(omain, "varscore", "Input file for giving scores of variables", "NULL");

// static const char* opp = "GPMC -- Preprocessor";
static const char* opp = "PBMC -- Preprocessor";
static StringOption	opt_ppout		(opp, "ppout", "Outfile for Simplified CNF", "NULL");
static IntOption		opt_varlimit	(opp, "varlim", "limit on #Vars in Preprocessing.", 200000, IntRange(0, INT32_MAX));
static DoubleOption	opt_pptimelim	(opp, "pptimelim", "Time threshold of preprocessing (not precise).", 120, DoubleRange(0, true, DBL_MAX, true));
static IntOption		opt_pprep		(opp, "ppreps",	"#reps of the main loop of preprocessing.", 20, IntRange(1, INT32_MAX));
static IntOption		opt_ppverb		(opp, "ppverb", "Preprocessing verbosity level (0=some, 1=more).", 0, IntRange(0, 1));
static BoolOption		opt_ee			(opp, "pp_ee", "Use equivalent literal elimination.", true);
static IntOption		opt_ee_varlim	(opp, "ee-varlim", "limit on #Vars in equivalent literal elimination.", 150000, IntRange(0, INT32_MAX));
static BoolOption		opt_ve			(opp, "pp_ve", "Use variable elimination.", true);
static IntOption		opt_vereps		(opp, "ve_reps", "VE: the number of repetitions", 400, IntRange(0, INT32_MAX));
static IntOption		opt_dvereps	(opp, "dve_reps", "DefVE: the number of repetitions", 10, IntRange(0, INT32_MAX));
static BoolOption		opt_vemore		(opp, "ve_more", "VE: target more variables", true);
static DoubleOption	opt_dvetimelim(opp, "dve_timelim", "DefVE: time limit", 60.0, DoubleRange(0, true, DBL_MAX, true));
static BoolOption		opt_cs			(opp, "pp_cs", "Clause Strengthening", true);

// static const char* otd = "GPMC -- Tree Decomposition";
static const char* otd = "PBMC -- Tree Decomposition";
static BoolOption		opt_td			(otd, "td", "Tree Decomposition", true);
static BoolOption		opt_alwtd		(otd, "alwtd", "Tree Decomposition (definitely, without any conditions)", false);
static IntOption		opt_td_varlim	(otd, "tdvarlim", "Limit on #Vars in Tree Decomposition", 150000, IntRange(0, INT32_MAX));
static DoubleOption	opt_td_dlim	(otd, "tddenlim", "Limit on density of graph in Tree Decomposition", 0.10, DoubleRange(0, true, 1, true));
static DoubleOption	opt_td_rlim	(otd, "tdratiolim", "Limit on ratio (edges/vars) of graph in Tree Decomposition", 30.0, DoubleRange(0, true, 1000, true));
static DoubleOption	opt_td_twvar	(otd, "twvarlim", "Limit on tw/vars", 0.25, DoubleRange(0, true, 1, true));
static DoubleOption	opt_td_to		(otd, "tdtime", "Time Limit on Tree Decomposition", 0, DoubleRange(0, true, INT32_MAX, true));

static StringOption	opt_tdout		(otd, "tdout", "Outfile for Tree Decomposition", "NULL");

//=================================================================================================
static Mode int2Mode(int mode) {
	switch(mode) {
	case 0: return MC;break;
	case 1: return WMC;break;
	case 2: return PMC;break;
	case 3: return WPMC;break;
	default:
		assert(false);
		return MC;
	}
}

static std::unordered_set<SimpRule> parseAblation(const char* value) {
	std::unordered_set<SimpRule> result;
	const std::string text = value == nullptr ? "" : value;
	if (text.empty()) return result;

	size_t begin = 0;
	while (begin <= text.size()) {
		const size_t end = text.find(':', begin);
		const std::string token = text.substr(
				begin, end == std::string::npos ? std::string::npos : end - begin);
		if (token.empty())
			throw std::invalid_argument("-ablation contains an empty rule ID");

		size_t parsed = 0;
		int id = 0;
		try {
			id = std::stoi(token, &parsed);
		} catch (...) {
			throw std::invalid_argument("invalid -ablation rule ID: " + token);
		}
		if (parsed != token.size())
			throw std::invalid_argument("invalid -ablation rule ID: " + token);
		switch(id) {
		case static_cast<int>(SimpRule::BASIC):
			result.insert(SimpRule::BASIC);
			break;
		case static_cast<int>(SimpRule::BICP):
			result.insert(SimpRule::BICP);
			break;
		case static_cast<int>(SimpRule::CS):
			result.insert(SimpRule::CS);
			break;
		case static_cast<int>(SimpRule::SC):
			result.insert(SimpRule::SC);
			break;
		case static_cast<int>(SimpRule::GCD):
			result.insert(SimpRule::GCD);
			break;
		case static_cast<int>(SimpRule::CARD):
			result.insert(SimpRule::CARD);
			break;
		default:
			throw std::invalid_argument("unknown -ablation rule ID: " + token);
		}

		if (end == std::string::npos) break;
		begin = end + 1;
	}
	return result;
}

static CacheMode parseCacheMode(const char* value) {
	const std::string mode = value == nullptr ? "" : value;
	if (mode == "legacy") return CacheMode::LEGACY;
	if (mode == "bypass") return CacheMode::BYPASS;
	if (mode == "residual") return CacheMode::RESIDUAL;
	throw std::invalid_argument("unknown -simp-cache mode: " + mode);
}

Configuration::Configuration() {
	cntr.mode = int2Mode(opt_mode);
	cntr.ddnnf = opt_ddnnf;
	cntr.precision = opt_precision;
	cntr.coef_tdscore = opt_coef;
	cntr.backjump = opt_bj;
	cntr.bj_threshold = opt_bj_thd;
	cntr.inprocess = opt_inprocess;
	cntr.ablation = parseAblation(opt_ablation);
	cntr.natw = opt_natw;
	cntr.pp_outfile = opt_ppout;
	cntr.td_outfile = opt_tdout;
	cntr.nnf_outfile = opt_nnfout;
	cntr.doPreprocss = true;
	cntr.doTD = opt_td || opt_alwtd;
	cntr.alwTD = opt_alwtd;
	cntr.watchCand = (opt_mode == WMC || opt_mode == WPMC || opt_ddnnf);
	cntr.vs_infile = opt_varscore;
	cntr.keepVarMap = opt_ddnnf || (cntr.vs_infile != "NULL");


	cm.weighted = (opt_mode == WMC || opt_mode == WPMC);
	cm.ddnnf = opt_ddnnf;
	cm.varSelectionHueristics = opt_vs;
	cm.cachesize = opt_cachesize;
	cm.cacheMode = parseCacheMode(opt_simpcache);
	if (cm.cacheMode != CacheMode::LEGACY &&
			(!cntr.inprocess || cntr.mode != MC || cntr.ddnnf))
		throw std::invalid_argument(
				"non-legacy -simp-cache requires -inprocess, exact MC, and -no-ddnnf");

	pp.varlimit = opt_varlimit;
	pp.timelim = opt_pptimelim;
	pp.reps = opt_pprep;
	pp.verb = opt_ppverb;
	pp.ee = opt_ee;
	pp.ee_varlim = opt_ee_varlim;
	pp.ve = opt_ve;
	pp.ve_reps = opt_vereps;
	pp.dve_reps = opt_dvereps;
	pp.ve_more = opt_vemore;
	pp.dve_timelimit = opt_dvetimelim;
	pp.cs = opt_cs;

	td.varlim = opt_td_varlim;
	td.denselim = opt_td_dlim;
	td.ratiolim = opt_td_rlim;
	td.twvarlim = opt_td_twvar;
	td.timelim = opt_td_to;
}
