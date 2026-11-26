#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>

class SblpCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit SblpCounter(GPMC::Configuration& config) : PBcounter(config) {}

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
		cmpmgr.init(nVars(), nPVars(), constraints, ca, rows_, &simp_rows_);
		validateRows(true);
		return !runPropagationWithoutLearnt(true);
	}

	bool rootBackbone() {
		const GPMC::SimpResult result = simplifyNode();
		return result.status == GPMC::SimpStatus::IMPLIED &&
				rs::isTrue(Level, 1) && simp_stats.sblp_candidates >= 1 &&
				simp_stats.sblp_exact_checks >= 1 &&
				simp_stats.sblp_exact_passed == 1 &&
				simp_stats.sblp_implied == 1;
	}

	bool guardedReasonRollback() {
		decide(1);
		if (runPropagationWithoutLearnt(true) || rs::isTrue(Level, 2)) return false;
		const GPMC::SimpResult result = simplifyNode();
		if (result.status != GPMC::SimpStatus::IMPLIED ||
				!rs::isTrue(Level, 2) || Reason[2] == rs::CRef_Undef ||
				ca[Reason[2]].getOrigin() != rs::Origin::LEARNED)
			return false;
		const rs::CRef reason = Reason[2];
		backjumpTo(0);
		return Pos[1] == rs::INF && Pos[2] == rs::INF &&
				Reason[2] == reason && ca[reason].getOrigin() == rs::Origin::LEARNED;
	}

	bool equalityDirections() {
		if (groups_.size() != 1 || !validGroup(groups_[0], rows_)) return false;
		return rootBackbone() && validGroup(groups_[0], rows_);
	}

	bool rejectsUnprovedOverlay() {
		if (rows_.size() != 1) return false;
		PBconstraint changed = rows_[0].content;
		changed.lits = {1};
		changed.coeffs = {1};
		changed.rhs = 1;
		if (!simp_rows_.set(rows_[0], changed, true, decisionLevel())) return false;
		const std::vector<rs::Lit> trail_before = trail;
		const GPMC::SimpResult result = simplifyNode();
		return result.status == GPMC::SimpStatus::UNCHANGED &&
				trail == trail_before && simp_stats.sblp_candidates == 1 &&
				simp_stats.sblp_exact_checks == 1 &&
				simp_stats.sblp_exact_passed == 0 &&
				simp_stats.sblp_exact_rejected == 1 &&
				simp_stats.sblp_implied == 0;
	}

	bool rejectsInexactDouble() {
		const std::vector<rs::Lit> trail_before = trail;
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				trail == trail_before && simp_stats.sblp_numeric_rejected == 1 &&
				simp_stats.sblp_mip_calls == 0;
	}

	bool budgetIsUnknown() {
		return simplifyNode().status == GPMC::SimpStatus::BUDGET &&
				simp_stats.sblp_budgets == 1 &&
				simp_stats.sblp_mip_calls == 32 &&
				simp_stats.sblp_implied == 0;
	}

	bool ablationSkipsSblp() {
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.sblp_calls == 0;
	}
};

GPMC::Configuration config(bool disabled = false) {
	GPMC::Configuration result;
	result.cntr.inprocess = true;
	result.cm.cacheMode = GPMC::CacheMode::RESIDUAL;
	result.cntr.ablation.insert(GPMC::SimpRule::BASIC);
	result.cntr.ablation.insert(GPMC::SimpRule::BICP);
	result.cntr.ablation.insert(GPMC::SimpRule::CS);
	result.cntr.ablation.insert(GPMC::SimpRule::SC);
	result.cntr.ablation.insert(GPMC::SimpRule::GCD);
	result.cntr.ablation.insert(GPMC::SimpRule::CARD);
	result.cntr.ablation.insert(GPMC::SimpRule::RIR);
	result.cntr.ablation.insert(GPMC::SimpRule::RPR);
	result.cntr.ablation.insert(GPMC::SimpRule::RSR);
	result.cntr.ablation.insert(GPMC::SimpRule::RUP);
	if (disabled) result.cntr.ablation.insert(GPMC::SimpRule::SBLP);
	return result;
}

constexpr const char* kBackbone =
		"* #variable= 3 #constraint= 4\n"
		"+1 x1 +1 x2 +1 x3 >= 1 ;\n"
		"+1 x1 +1 x2 +1 ~x3 >= 1 ;\n"
		"+1 x1 +1 ~x2 +1 x3 >= 1 ;\n"
		"+1 x1 +1 ~x2 +1 ~x3 >= 1 ;\n";

constexpr const char* kGuarded =
		"* #variable= 4 #constraint= 4\n"
		"+1 ~x1 +1 x2 +1 x3 +1 x4 >= 1 ;\n"
		"+1 ~x1 +1 x2 +1 x3 +1 ~x4 >= 1 ;\n"
		"+1 ~x1 +1 x2 +1 ~x3 +1 x4 >= 1 ;\n"
		"+1 ~x1 +1 x2 +1 ~x3 +1 ~x4 >= 1 ;\n";

constexpr const char* kEquality =
		"* #variable= 4 #constraint= 5\n"
		"+1 x1 +1 x2 = 1 ;\n"
		"+1 ~x2 +1 x3 +1 x4 >= 1 ;\n"
		"+1 ~x2 +1 x3 +1 ~x4 >= 1 ;\n"
		"+1 ~x2 +1 ~x3 +1 x4 >= 1 ;\n"
		"+1 ~x2 +1 ~x3 +1 ~x4 >= 1 ;\n";

constexpr const char* kLargeExact =
		"* #variable= 6 #constraint= 6\n"
		"+1 x1 +1 x2 +1 x3 >= 1 ;\n"
		"+1 x1 +1 x2 +1 ~x3 >= 1 ;\n"
		"+1 x1 +1 ~x2 +1 x3 >= 1 ;\n"
		"+1 x1 +1 ~x2 +1 ~x3 >= 1 ;\n"
		"+3000000000 x4 +2999999999 x5 +1 x6 >= 3000000000 ;\n"
		"+1 x5 >= 1 ;\n";

constexpr const char* kInexact =
		"* #variable= 3 #constraint= 1\n"
		"+9007199254740993 x1 +9007199254740992 x2 +1 x3 "
		">= 9007199254740993 ;\n";

std::string budgetFormula() {
	std::ostringstream formula;
	formula << "* #variable= 17 #constraint= 1\n";
	for (int variable = 1; variable <= 17; ++variable)
		formula << "+1 x" << variable << ' ';
	formula << ">= 1 ;\n";
	return formula.str();
}

int main() {
	{
		GPMC::Configuration options = config();
		SblpCounter counter(options);
		counter.init();
		if (!counter.prepare(kBackbone) || !counter.rootBackbone()) return 1;
	}
	{
		GPMC::Configuration options = config();
		SblpCounter counter(options);
		counter.init();
		if (!counter.prepare(kGuarded) || !counter.guardedReasonRollback()) return 2;
	}
	{
		GPMC::Configuration options = config();
		SblpCounter counter(options);
		counter.init();
		if (!counter.prepare(kEquality) || !counter.equalityDirections()) return 3;
	}
	{
		GPMC::Configuration options = config();
		SblpCounter counter(options);
		counter.init();
		if (!counter.prepare("* #variable= 2 #constraint= 1\n"
				"+1 x1 +1 x2 >= 1 ;\n") || !counter.rejectsUnprovedOverlay())
			return 4;
	}
	{
		GPMC::Configuration options = config();
		SblpCounter counter(options);
		counter.init();
		if (!counter.prepare(kLargeExact) || !counter.rootBackbone() ||
				counter.simp_stats.sblp_numeric_rejected != 0)
			return 5;
	}
	{
		GPMC::Configuration options = config();
		SblpCounter counter(options);
		counter.init();
		if (!counter.prepare(kInexact) || !counter.rejectsInexactDouble()) return 6;
	}
	{
		GPMC::Configuration options = config();
		SblpCounter counter(options);
		counter.init();
		if (!counter.prepare(budgetFormula()) || !counter.budgetIsUnknown()) return 7;
	}
	{
		GPMC::Configuration options = config(true);
		SblpCounter counter(options);
		counter.init();
		if (!counter.prepare(kBackbone) || !counter.ablationSkipsSblp()) return 8;
	}
	return 0;
}
