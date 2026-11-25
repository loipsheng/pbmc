#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>

class ReferenceCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit ReferenceCounter(GPMC::Configuration& config) : PBcounter(config) {}

	bool prepare(const std::string& formula) {
		std::istringstream input(formula);
		load(input);
		pbins.renumberVar();
		pbins.makeRenumberedCoeffPositive();
		pbins.simplifyRenumberedConstraint();
		if (pbins.inferredUnsat) return false;
		pbins.computeVarCoeffScore(config.coef_tdscore, false);
		setNbVars(pbins.vars, true);
		copyPBInsNPVar();
		copyPBInsPhase();
		if (!addRows()) return false;
		import();
		cmpmgr.init(nVars(), nPVars(), constraints, ca, rows_);
		validateRows(true);
		return true;
	}

	GPMC::ReferenceKey key() {
		syncAssign();
		return cmpmgr.referenceKey(cmpmgr.topComponent(), assigns);
	}

	GPMC::ReferenceKey keyAfter(rs::Lit literal) {
		decide(literal);
		if (runPropagationWithoutLearnt(true)) return {};
		return key();
	}
};

GPMC::ReferenceKey makeKey(const std::string& formula) {
	GPMC::Configuration config;
	config.cntr.inprocess = true;
	config.cm.cacheMode = GPMC::CacheMode::BYPASS;
	ReferenceCounter counter(config);
	counter.init();
	if (!counter.prepare(formula)) return {};
	return counter.key();
}

int main() {
	const std::string ordered =
			"* #variable= 3 #constraint= 1\n"
			"+2 x1 +1 ~x2 +1 x3 >= 2 ;\n";
	const std::string reordered =
			"* #variable= 3 #constraint= 1\n"
			"+1 x3 +2 x1 +1 ~x2 >= 2 ;\n";
	const GPMC::ReferenceKey first = makeKey(ordered);
	const GPMC::ReferenceKey second = makeKey(reordered);
	if (first.bytes.empty() || first != second) return 1;

	const GPMC::ReferenceKey changed_coefficient = makeKey(
			"* #variable= 3 #constraint= 1\n"
			"+1 x1 +1 ~x2 +1 x3 >= 2 ;\n");
	const GPMC::ReferenceKey changed_gap = makeKey(
			"* #variable= 3 #constraint= 1\n"
			"+2 x1 +1 ~x2 +1 x3 >= 3 ;\n");
	if (first == changed_coefficient || first == changed_gap) return 2;

	const GPMC::ReferenceKey equality = makeKey(
			"* #variable= 2 #constraint= 1\n"
			"+1 x1 +1 x2 = 1 ;\n");
	const GPMC::ReferenceKey separate_directions = makeKey(
			"* #variable= 2 #constraint= 2\n"
			"+1 x1 +1 x2 >= 1 ;\n"
			"+1 x1 +1 x2 <= 1 ;\n");
	if (equality.bytes.empty() || equality == separate_directions) return 3;

	GPMC::Configuration config;
	config.cntr.inprocess = true;
	config.cm.cacheMode = GPMC::CacheMode::BYPASS;
	ReferenceCounter residual(config);
	residual.init();
	if (!residual.prepare(
			"* #variable= 4 #constraint= 1\n"
			"+2 x1 +1 x2 +1 x3 +1 x4 >= 2 ;\n"))
		return 4;
	const GPMC::ReferenceKey before = residual.key();
	const GPMC::ReferenceKey after = residual.keyAfter(-1);
	if (after.bytes.empty() || before == after) return 5;

	const GPMC::ReferenceKey huge = makeKey(
			"* #variable= 3 #constraint= 1\n"
			"+900000000000000000 x1 +700000000000000000 x2 +1 x3 "
			">= 800000000000000000 ;\n");
	if (huge.bytes.empty()) return 6;

	GPMC::ReferenceKey collision_left = first;
	GPMC::ReferenceKey collision_right = changed_gap;
	collision_right.hash = collision_left.hash;
	if (collision_left == collision_right) return 7;
	return 0;
}
