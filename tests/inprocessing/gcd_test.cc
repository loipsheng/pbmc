#include "core/Counter.h"

#include <algorithm>
#include <gmpxx.h>
#include <sstream>
#include <string>
#include <vector>

class GCDTestCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit GCDTestCounter(GPMC::Configuration& config) : PBcounter(config) {}

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

	bool division(rs::Lit assignment, Int rhs, std::vector<Int> coefficients) {
		if (rows_.size() != 1 || rows_.front().state != RowState::ATTACHED)
			return false;
		const rs::CRef cref = solverRef(rows_.front().version);
		const rs::ID id = ca[cref].id;
		const rs::BigVal degree = ca[cref].degree();
		const uint32_t allocator_before = ca.at;
		const std::vector<rs::CRef> reasons_before = Reason;

		decide(assignment);
		if (runPropagationWithoutLearnt(true)) return false;
		const std::vector<rs::Lit> trail_before = trail;
		const bool legacy = cmpmgr.cache().legacy();
		const GPMC::SimpStatus status = simplifyNode().status;
		if (legacy) {
			return status == GPMC::SimpStatus::UNCHANGED && simp_rows_.size() == 0 &&
					simp_stats.gcd_calls == 1 && simp_stats.gcd_rows == 1 &&
					simp_stats.gcd_changed_rows == 1 && simp_stats.gcd_commits == 0;
		}
		if (status != GPMC::SimpStatus::ROW_CHANGED || trail != trail_before ||
				Reason != reasons_before || ca.at != allocator_before)
			return false;

		const GPMC::SimpRow* view = simp_rows_.find(rows_.front().version);
		if (view == nullptr || !view->active || view->content.rhs != rhs ||
				view->content.lits.size() != coefficients.size())
			return false;
		std::sort(coefficients.begin(), coefficients.end(), std::greater<Int>());
		if (view->content.coeffs != coefficients) return false;
		for (Int literal : view->content.lits)
			if (literal == assignment) return false;

		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		const rs::CRef after = solverRef(rows_.front().version);
		if (after != cref || ca[after].id != id || ca[after].degree() != degree ||
				ca.at != allocator_before || Reason != reasons_before)
			return false;
		if (simp_stats.gcd_calls != 2 || simp_stats.gcd_rows != 2 ||
				simp_stats.gcd_changed_rows != 1 || simp_stats.gcd_commits != 1)
			return false;
		backjumpTo(0);
		return simp_rows_.size() == 0 && solverRef(rows_.front().version) == cref;
	}

	bool reasonPinned() {
		if (rows_.size() != 1 || rows_.front().state != RowState::ATTACHED)
			return false;
		decide(-5);
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
		if (view == nullptr || view->content.rhs != 2 ||
				view->content.coeffs != std::vector<Int>({2, 1, 1}))
			return false;
		if (Reason != reasons_before || ca.at != allocator_before ||
				solverRef(rows_.front().version) != cref)
			return false;
		backjumpTo(0);
		return simp_rows_.size() == 0 && solverRef(rows_.front().version) == cref;
	}

	bool equalityCommit() {
		if (groups_.size() != 1 || rows_.size() != 2) return false;
		decide(-4);
		if (runPropagationWithoutLearnt(true)) return false;
		std::vector<rs::CRef> refs;
		for (const RowInfo& row : rows_) {
			if (row.state != RowState::ATTACHED) return false;
			refs.push_back(solverRef(row.version));
		}
		const std::vector<rs::CRef> reasons_before = Reason;
		const uint32_t allocator_before = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED) return false;
		const GPMC::SimpRow* lower = simp_rows_.find(groups_.front().lower);
		const GPMC::SimpRow* upper = simp_rows_.find(groups_.front().upper);
		if (lower == nullptr || upper == nullptr || lower->transaction == 0 ||
				lower->transaction != upper->transaction || lower->content.rhs != 2 ||
				upper->content.rhs != 2 ||
				lower->content.coeffs != std::vector<Int>({2, 1, 1}) ||
				upper->content.coeffs != std::vector<Int>({2, 1, 1}))
			return false;
		if (Reason != reasons_before || ca.at != allocator_before ||
				solverRef(rows_[0].version) != refs[0] ||
				solverRef(rows_[1].version) != refs[1])
			return false;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		if (simp_stats.gcd_changed_rows != 2 || simp_stats.gcd_commits != 1 ||
				simp_rows_.size() != 2)
			return false;
		backjumpTo(0);
		return simp_rows_.size() == 0;
	}

	bool sequence() {
		if (rows_.size() != 1) return false;
		const rs::CRef cref = solverRef(rows_.front().version);
		decide(4);
		if (runPropagationWithoutLearnt(true)) return false;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED ||
				simp_stats.cs_commits != 1 || simp_stats.gcd_calls != 0)
			return false;
		const GPMC::SimpRow* view = simp_rows_.find(rows_.front().version);
		if (view == nullptr || view->content.rhs != 6 ||
				view->content.coeffs != std::vector<Int>({6, 4, 2}))
			return false;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED ||
				simp_stats.gcd_commits != 1)
			return false;
		view = simp_rows_.find(rows_.front().version);
		if (view == nullptr || view->content.rhs != 3 ||
				view->content.coeffs != std::vector<Int>({3, 2, 1}))
			return false;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		backjumpTo(0);
		return simp_rows_.size() == 0 && solverRef(rows_.front().version) == cref;
	}

	bool ablationSkipsGCD() {
		decide(-5);
		if (runPropagationWithoutLearnt(true)) return false;
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.gcd_calls == 0 && simp_rows_.size() == 0;
	}
};

GPMC::Configuration gcdConfig(
		GPMC::CacheMode mode = GPMC::CacheMode::RESIDUAL, bool cs = false) {
	GPMC::Configuration config;
	config.cntr.inprocess = true;
	config.cm.cacheMode = mode;
	config.cntr.ablation.insert(GPMC::SimpRule::BASIC);
	config.cntr.ablation.insert(GPMC::SimpRule::BICP);
	config.cntr.ablation.insert(GPMC::SimpRule::CARD);
	config.cntr.ablation.insert(GPMC::SimpRule::SC);
	if (!cs) config.cntr.ablation.insert(GPMC::SimpRule::CS);
	return config;
}

int main() {
	const std::string simple =
			"* #variable= 5 #constraint= 1\n"
			"+4 x1 +2 x2 +2 x3 +2 x4 +1 x5 >= 5 ;\n";
	{
		GPMC::Configuration config = gcdConfig(GPMC::CacheMode::BYPASS);
		GCDTestCounter counter(config);
		counter.init();
		if (!counter.prepare(simple) ||
				!counter.division(-5, 3, {2, 1, 1, 1})) return 1;
	}
	{
		GPMC::Configuration config = gcdConfig();
		GCDTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 5 #constraint= 1\n"
				"+4 ~x1 +2 x2 +2 x3 +2 x4 +1 x5 >= 5 ;\n";
		if (!counter.prepare(formula) ||
				!counter.division(-5, 3, {2, 1, 1, 1})) return 2;
	}
	{
		GPMC::Configuration config = gcdConfig();
		GCDTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 1\n"
				"+3000000000000000000 x1 +2000000000000000000 x2 "
				"+2000000000000000000 x3 +1 x4 >= 3000000000000000001 ;\n";
		if (!counter.prepare(formula) ||
				!counter.division(-4, 4, {3, 2, 2})) return 3;
	}
	{
		GPMC::Configuration config = gcdConfig();
		GCDTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 1\n"
				"+4 x1 +2 x2 +2 x3 +1 x4 = 4 ;\n";
		if (!counter.prepare(formula) || !counter.equalityCommit()) return 4;
	}
	{
		GPMC::Configuration config = gcdConfig();
		GCDTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 5 #constraint= 1\n"
				"+6 x1 +4 x2 +2 x3 +2 x4 +3 x5 >= 10 ;\n";
		if (!counter.prepare(formula) || !counter.reasonPinned()) return 5;
	}
	{
		GPMC::Configuration config = gcdConfig(GPMC::CacheMode::RESIDUAL, true);
		GCDTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 1\n"
				"+8 x1 +4 x2 +2 x3 +3 x4 >= 9 ;\n";
		if (!counter.prepare(formula) || !counter.sequence()) return 6;
	}
	{
		GPMC::Configuration config = gcdConfig(GPMC::CacheMode::LEGACY);
		GCDTestCounter counter(config);
		counter.init();
		if (!counter.prepare(simple) ||
				!counter.division(-5, 3, {2, 1, 1, 1})) return 7;
	}
	{
		GPMC::Configuration config = gcdConfig();
		config.cntr.ablation.insert(GPMC::SimpRule::GCD);
		GCDTestCounter counter(config);
		counter.init();
		if (!counter.prepare(simple) || !counter.ablationSkipsGCD()) return 8;
	}
	return 0;
}
