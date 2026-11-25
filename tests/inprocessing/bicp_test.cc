#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>
#include <vector>

class TestCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit TestCounter(GPMC::Configuration& config) : PBcounter(config) {}

	bool prepare(const std::string& formula, bool move_on_gc = false) {
		if (move_on_gc) {
			const uint32_t padding = static_cast<uint32_t>(
					rs::ConstraintAllocator::alignedWordSize(17));
			ca.capacity(padding);
			ca.at = padding;
		}
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

	bool failedLiteralAndRollback() {
		decide(-1);
		if (runPropagationWithoutLearnt(true)) return false;
		if (!rs::isTrue(Level, 2) || !rs::isTrue(Level, 3)) return false;
		garbage_collect();
		validateRows(true);

		const std::vector<rs::Lit> trail_before = trail;
		const std::vector<int> limits_before = trail_lim;
		const std::vector<rs::CRef> reasons_before = Reason;
		const std::vector<rs::Lit> phase_before = phase;
		const uint32_t allocator_before = ca.at;
		const std::uint64_t failed_before = simp_stats.bicp_failed;

		if (simplifyNode().status != GPMC::SimpStatus::IMPLIED) return false;
		if (trail.size() != trail_before.size() + 1 || trail.back() != 4 ||
				trail_lim != limits_before ||
				Reason[2] != reasons_before[2] || Reason[3] != reasons_before[3] ||
				phase != phase_before || ca.at <= allocator_before)
			return false;
		if (simp_stats.bicp_failed <= failed_before ||
				simp_stats.bicp_probes == 0 || simp_stats.bicp_work == 0 ||
				simp_stats.bicp_implied != 1 || Reason[4] == rs::CRef_Undef ||
				ca[Reason[4]].getOrigin() != rs::Origin::LEARNED)
			return false;

		if (runPropagationWithoutLearnt(true)) return false;
		if (!rs::isTrue(Level, 4)) return false;

		// Repeating the engine after the committed implication must remain safe.
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		return rs::isTrue(Level, 4) && Reason[4] != rs::CRef_Undef;
	}

	bool commonImplication() {
		if (simplifyNode().status != GPMC::SimpStatus::IMPLIED) return false;
		if (runPropagationWithoutLearnt(true)) return false;
		return rs::isTrue(Level, 2) && rs::isTrue(Level, 4) &&
				simp_stats.bicp_common >= 2 && simp_stats.bicp_implied == 1;
	}

	bool equalityDirections() {
		if (groups_.size() != 1 || rows_.size() < 2) return false;
		const std::vector<rs::Lit> trail_before = trail;
		const std::vector<rs::CRef> reasons_before = Reason;
		const std::vector<rs::Lit> phase_before = phase;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		return trail == trail_before && Reason == reasons_before &&
				phase == phase_before && simp_stats.bicp_probes > 0 &&
				validGroup(groups_.front(), rows_);
	}

	bool candidateBudget() {
		const GPMC::SimpResult result = simplifyNode();
		return result.status == GPMC::SimpStatus::BUDGET &&
				simp_stats.budgets == 1 && simp_stats.bicp_candidates == 32;
	}
};

GPMC::Configuration bicpConfig() {
	GPMC::Configuration config;
	config.cntr.inprocess = true;
	config.cntr.ablation.insert(GPMC::SimpRule::BASIC);
	return config;
}

int main() {
	{
		GPMC::Configuration config = bicpConfig();
		TestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 5 #constraint= 3\n"
				"+2 x1 +2 x2 +1 x3 >= 3 ;\n"
				"+1 x4 +1 x5 >= 1 ;\n"
				"+1 x4 +1 ~x5 >= 1 ;\n";
		if (!counter.prepare(formula, true) || !counter.failedLiteralAndRollback())
			return 1;
	}
	{
		GPMC::Configuration config = bicpConfig();
		TestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 3\n"
				"+1 x1 +1 x2 >= 1 ;\n"
				"+1 ~x1 +1 x2 +1 x3 >= 2 ;\n"
				"+1 ~x2 +1 x4 >= 1 ;\n";
		if (!counter.prepare(formula) || !counter.commonImplication()) return 2;
	}
	{
		GPMC::Configuration config = bicpConfig();
		TestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 3 #constraint= 2\n"
				"+1 x1 +1 x2 = 1 ;\n"
				"+1 x2 +1 x3 >= 1 ;\n";
		if (!counter.prepare(formula) || !counter.equalityDirections()) return 3;
	}
	{
		GPMC::Configuration config = bicpConfig();
		TestCounter counter(config);
		counter.init();
		std::ostringstream formula;
		formula << "* #variable= 40 #constraint= 1\n";
		for (int variable = 1; variable <= 40; ++variable)
			formula << "+1 x" << variable << ' ';
		formula << ">= 1 ;\n";
		if (!counter.prepare(formula.str()) || !counter.candidateBudget()) return 4;
	}
	return 0;
}
