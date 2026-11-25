#ifndef CONFIG_H_
#define CONFIG_H_

#include <cstdint>
#include <string>
#include <unordered_set>
#include <float.h>
#include "utils/Options.h"

namespace GPMC {

using namespace Glucose;

enum Mode { MC, WMC, PMC, WPMC };

enum class CacheMode : std::uint8_t {
	LEGACY,
	BYPASS,
	RESIDUAL
};

enum class SimpRule : std::uint8_t {
	BASIC = 1,
	BICP = 2
};

struct ConfigCounter {
	Mode mode;
	bool ddnnf;
	int precision;
	double coef_tdscore;
	bool backjump;
	double bj_threshold;
	bool inprocess;
	std::unordered_set<SimpRule> ablation;
	bool disabled(SimpRule rule) const { return ablation.count(rule) != 0; }
	bool natw;
	std::string pp_outfile;
	std::string td_outfile;
	std::string nnf_outfile;
	bool doPreprocss;
	bool doTD;
	bool alwTD;
	bool watchCand;
	std::string vs_infile;
	bool keepVarMap;
};

struct ConfigComponentManager {
	bool weighted;
	bool ddnnf;
	int varSelectionHueristics;
	int cachesize;
	CacheMode cacheMode;
};

struct ConfigPreprocessor {
	int varlimit;
	double timelim;
	double reps;
	int verb;

	// EE
	bool ee;
	int ee_varlim;

	// VE/DefVE
	bool ve;
	int ve_reps;
	int dve_reps;
	bool ve_more;
	double dve_timelimit;

	// CS
	bool cs;
};

struct ConfigTreeDecomposition {
	double varlim;		// Limit on #Vars
	double denselim;		// Limit on density of graph
	double ratiolim;		// Limit on ratio (edges/vars) of graph
	double twvarlim;		// Limit on tw/vars
	double timelim;		// Time Limit
};

class Configuration {
public:
	ConfigCounter cntr;
	ConfigComponentManager cm;
	ConfigPreprocessor pp;
	ConfigTreeDecomposition td;

	Configuration();
	~Configuration() { }
};
}
#endif /* CONFIG_H_ */
