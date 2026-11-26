#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>
#include <vector>

class CSTestCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit CSTestCounter(GPMC::Configuration& config) : PBcounter(config) {}

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
		return true;
	}

	bool residualSaturation(rs::Lit assignment) {
		if (rows_.size() != 1 || rows_.front().state != RowState::ATTACHED)
			return false;
		const rs::CRef cref = solverRef(rows_.front().version);
		const rs::ID id = ca[cref].id;
		const rs::BigVal degree = ca[cref].degree();
		std::vector<std::pair<rs::Lit, rs::BigCoef>> before;
		for (size_t i = 0; i < ca[cref].size(); ++i)
			before.emplace_back(ca[cref].lit(i), ca[cref].coef(i));

		decide(assignment);
		if (runPropagationWithoutLearnt(true)) return false;
		const std::vector<rs::Lit> trail_before = trail;
		const std::vector<rs::CRef> reasons_before = Reason;
		const uint32_t allocator_before = ca.at;
		const bool legacy = cmpmgr.cache().legacy();
		const GPMC::SimpStatus status = simplifyNode().status;
		if (legacy) {
			return status == GPMC::SimpStatus::UNCHANGED && simp_rows_.size() == 0 &&
					simp_stats.cs_calls == 1 && simp_stats.cs_rows == 1 &&
					simp_stats.cs_changed_rows == 1 &&
					simp_stats.cs_coefficients == 1 && simp_stats.cs_commits == 0;
		}
		if (status != GPMC::SimpStatus::ROW_CHANGED) return false;
		if (trail != trail_before || Reason != reasons_before || ca.at != allocator_before)
			return false;
		const GPMC::SimpRow* view = simp_rows_.find(rows_.front().version);
		if (view == nullptr || !view->active || view->content.rhs <= 0 ||
				view->content.lits.size() != view->content.coeffs.size())
			return false;
		for (size_t i = 0; i < view->content.lits.size(); ++i) {
			if (view->content.lits[i] == assignment ||
					view->content.coeffs[i] > view->content.rhs)
				return false;
		}
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		const rs::CRef after = solverRef(rows_.front().version);
		if (after != cref || ca[after].id != id || ca[after].degree() != degree ||
				ca[after].size() != before.size())
			return false;
		for (size_t i = 0; i < before.size(); ++i) {
			if (ca[after].lit(i) != before[i].first ||
					ca[after].coef(i) != before[i].second)
				return false;
		}
		if (simp_stats.cs_calls != 2 || simp_stats.cs_rows != 2 ||
				simp_stats.cs_changed_rows != 1 || simp_stats.cs_coefficients != 1 ||
				simp_stats.cs_commits != 1)
			return false;
		backjumpTo(0);
		return simp_rows_.size() == 0 && solverRef(rows_.front().version) == cref;
	}

	bool equalityDirections() {
		if (groups_.size() != 1 || rows_.size() != 2) return false;
		const std::vector<rs::CRef> reasons_before = Reason;
		const uint32_t allocator_before = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		return Reason == reasons_before && ca.at == allocator_before &&
				simp_stats.cs_calls == 1 && simp_stats.cs_rows == 2 &&
				simp_stats.cs_equality_rows == 2 &&
				simp_stats.cs_changed_rows == 0 && simp_stats.cs_commits == 0;
	}

	bool reasonPinned() {
		if (rows_.size() != 1 || rows_.front().state != RowState::ATTACHED)
			return false;
		decide(-4);
		if (runPropagationWithoutLearnt(true)) return false;
		const rs::CRef cref = solverRef(rows_.front().version);
		bool pinned = false;
		for (rs::Lit literal : trail)
			if (Reason[rs::toVar(literal)] == cref) pinned = true;
		if (!pinned) return false;

		const std::vector<rs::CRef> reasons_before = Reason;
		const uint32_t allocator_before = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED) return false;
		const GPMC::SimpRow* view = simp_rows_.find(rows_.front().version);
		if (view == nullptr || view->transaction != 0 || view->content.rhs != 4)
			return false;
		if (Reason != reasons_before || ca.at != allocator_before ||
				solverRef(rows_.front().version) != cref)
			return false;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		if (simp_stats.cs_changed_rows != 1 || simp_stats.cs_coefficients != 1 ||
				simp_stats.cs_commits != 1 || simp_rows_.size() != 1)
			return false;
		backjumpTo(0);
		return simp_rows_.size() == 0 && solverRef(rows_.front().version) == cref;
	}

	bool ablationSkipsCS() {
		decide(3);
		if (runPropagationWithoutLearnt(true)) return false;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		return simp_stats.cs_calls == 0 && simp_stats.cs_rows == 0;
	}
};

GPMC::Configuration csConfig(
		GPMC::CacheMode mode = GPMC::CacheMode::RESIDUAL) {
	GPMC::Configuration config;
	config.cntr.inprocess = true;
	config.cm.cacheMode = mode;
	config.cntr.ablation.insert(GPMC::SimpRule::BASIC);
	config.cntr.ablation.insert(GPMC::SimpRule::BICP);
	config.cntr.ablation.insert(GPMC::SimpRule::GCD);
	config.cntr.ablation.insert(GPMC::SimpRule::CARD);
	config.cntr.ablation.insert(GPMC::SimpRule::SC);
	return config;
}

int main() {
	{
		GPMC::Configuration config = csConfig(GPMC::CacheMode::BYPASS);
		CSTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 3 #constraint= 1\n"
				"+5 x1 +3 x2 +2 x3 >= 5 ;\n";
		if (!counter.prepare(formula) || !counter.residualSaturation(3)) return 1;
	}
	{
		GPMC::Configuration config = csConfig();
		CSTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 3 #constraint= 1\n"
				"+5 ~x1 +3 x2 +2 x3 >= 5 ;\n";
		if (!counter.prepare(formula) || !counter.residualSaturation(3)) return 2;
	}
	{
		GPMC::Configuration config = csConfig();
		CSTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 3 #constraint= 1\n"
				"+4000000000000000000 x1 +3999999999999999999 x2 +1 x3 "
				">= 4000000000000000000 ;\n";
		if (!counter.prepare(formula) || !counter.residualSaturation(3)) return 3;
	}
	{
		GPMC::Configuration config = csConfig();
		CSTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 3 #constraint= 1\n"
				"+2 x1 +1 x2 +1 x3 = 2 ;\n";
		if (!counter.prepare(formula) || !counter.equalityDirections()) return 4;
	}
	{
		GPMC::Configuration config = csConfig();
		CSTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 1\n"
				"+6 x1 +5 x2 +4 x3 +1 x4 >= 10 ;\n";
		if (!counter.prepare(formula) || !counter.reasonPinned()) return 5;
	}
	{
		GPMC::Configuration config = csConfig(GPMC::CacheMode::LEGACY);
		CSTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 3 #constraint= 1\n"
				"+5 x1 +3 x2 +2 x3 >= 5 ;\n";
		if (!counter.prepare(formula) || !counter.residualSaturation(3)) return 6;
	}
	{
		GPMC::Configuration config = csConfig();
		config.cntr.ablation.insert(GPMC::SimpRule::CS);
		CSTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 3 #constraint= 1\n"
				"+5 x1 +3 x2 +2 x3 >= 5 ;\n";
		if (!counter.prepare(formula) || !counter.ablationSkipsCS()) return 7;
	}
	return 0;
}
