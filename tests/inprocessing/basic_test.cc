#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <stdexcept>
#include <string>

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

	bool nativeReasonAndGC() {
		if (rows_.size() != 1) return false;
		decide(-1);
		if (runPropagationWithoutLearnt(true)) return false;
		if (!rs::isTrue(Level, 2) || !rs::isTrue(Level, 3)) return false;
		const rs::CRef reason2 = Reason[2];
		const rs::CRef reason3 = Reason[3];
		if (reason2 == rs::CRef_Undef || reason2 != reason3) return false;
		const rs::ID reason_id = ca[reason2].id;
		const size_t trail_size = trail.size();

		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		if (trail.size() != trail_size || Reason[2] != reason2 || Reason[3] != reason3)
			return false;
		if (simp_stats.basic_calls != 1 || simp_stats.basic_rows != 1 ||
				simp_stats.basic_open != 0 || simp_stats.basic_satisfied != 1)
			return false;

		garbage_collect();
		validateRows(true);
		if (Reason[2] == reason2 || Reason[2] != Reason[3]) return false;
		if (ca[Reason[2]].id != reason_id) return false;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		return simp_stats.basic_calls == 2 && simp_stats.basic_rows == 2 &&
				simp_stats.basic_satisfied == 2;
	}

	bool detectsMissedPropagation() {
		decide(-1);
		qhead = trail.size(); // Deliberately simulate a broken native propagation call.
		try {
			(void)simplifyNode();
		} catch (const std::logic_error& error) {
			return std::string(error.what()) ==
					"Basic found an implication missed by native propagation";
		}
		return false;
	}

	bool ablationSkipsBasic() {
		decide(-1);
		qhead = trail.size();
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		return simp_stats.calls == 1 && simp_stats.basic_calls == 0 &&
				simp_stats.basic_rows == 0;
	}

	bool keepsFormulaDuringCleanup() {
		if (rows_.size() != 1 || rows_.front().state != RowState::ATTACHED)
			return false;
		const rs::ID row_id = rows_.front().solver_id;
		if (addUnitConstraint(1, rs::Origin::FORMULA).second == rs::ID_Unsat)
			return false;
		const rs::CRef before = solverRef(rows_.front().version);
		if (!ca[before].isSatisfiedAtRoot(Level)) return false;
		reduceDB(true);
		validateRows(true);
		return ca[solverRef(rows_.front().version)].id == row_id;
	}
};

namespace {

constexpr const char* kPropagationFormula =
		"* #variable= 3 #constraint= 1\n"
		"+2 x1 +2 x2 +1 x3 >= 3 ;\n";

constexpr const char* kCleanupFormula =
		"* #variable= 2 #constraint= 1\n"
		"+1 x1 +1 x2 >= 1 ;\n";

} // namespace

int main() {
	{
		GPMC::Configuration config;
		config.cntr.inprocess = true;
		config.cntr.ablation.insert(GPMC::SimpRule::BICP);
		TestCounter counter(config);
		counter.init();
		if (!counter.prepare(kPropagationFormula, true) || !counter.nativeReasonAndGC())
			return 1;
	}
	{
		GPMC::Configuration config;
		config.cntr.inprocess = true;
		config.cntr.ablation.insert(GPMC::SimpRule::BICP);
		TestCounter counter(config);
		counter.init();
		if (!counter.prepare(kPropagationFormula) || !counter.detectsMissedPropagation())
			return 2;
	}
	{
		GPMC::Configuration config;
		config.cntr.inprocess = true;
		config.cntr.ablation.insert(GPMC::SimpRule::BASIC);
		config.cntr.ablation.insert(GPMC::SimpRule::BICP);
		TestCounter counter(config);
		counter.init();
		if (!counter.prepare(kPropagationFormula) || !counter.ablationSkipsBasic())
			return 3;
	}
	{
		GPMC::Configuration config;
		config.cntr.inprocess = true;
		config.cntr.ablation.insert(GPMC::SimpRule::BICP);
		TestCounter counter(config);
		counter.init();
		if (!counter.prepare(kCleanupFormula) || !counter.keepsFormulaDuringCleanup())
			return 4;
	}
	return 0;
}
