#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

class SCTestCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit SCTestCounter(GPMC::Configuration& config) : PBcounter(config) {}

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

	static Int coefficient(const GPMC::SimpRow& row, rs::Lit literal) {
		for (size_t i = 0; i < row.content.lits.size(); ++i)
			if (row.content.lits[i] == literal) return row.content.coeffs[i];
		return 0;
	}

	bool commit(rs::Lit literal, Int replacement, bool check_fixpoint = true) {
		if (rows_.size() < 2 || rows_.front().state != RowState::ATTACHED)
			return false;
		const RowVersionID version = rows_.front().version;
		const rs::CRef cref = solverRef(version);
		const rs::ID id = ca[cref].id;
		const rs::BigVal degree = ca[cref].degree();
		const uint32_t allocator_before = ca.at;
		const std::vector<rs::CRef> reasons_before = Reason;
		std::vector<std::pair<rs::Lit, rs::BigCoef>> attached;
		for (size_t i = 0; i < ca[cref].size(); ++i)
			attached.emplace_back(ca[cref].lit(i), ca[cref].coef(i));

		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED) return false;
		const GPMC::SimpRow* view = simp_rows_.find(version);
		if (view == nullptr || !view->active || view->transaction != 0 ||
				coefficient(*view, literal) != replacement)
			return false;
		if (simp_stats.sc_proposals != 1 || simp_stats.sc_coefficients != 1 ||
				simp_stats.sc_revalidations != 1 ||
				simp_stats.sc_changed_rows != 1 || simp_stats.sc_commits != 1)
			return false;
		if (Reason != reasons_before || ca.at != allocator_before ||
				solverRef(version) != cref || ca[cref].id != id ||
				ca[cref].degree() != degree || ca[cref].size() != attached.size())
			return false;
		for (size_t i = 0; i < attached.size(); ++i)
			if (ca[cref].lit(i) != attached[i].first ||
					ca[cref].coef(i) != attached[i].second)
				return false;
		return !check_fixpoint ||
				simplifyNode().status == GPMC::SimpStatus::UNCHANGED;
	}

	bool legacyProposal(rs::Lit literal) {
		const rs::CRef cref = solverRef(rows_.front().version);
		const uint32_t allocator_before = ca.at;
		const std::vector<rs::CRef> reasons_before = Reason;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		return simp_rows_.size() == 0 && solverRef(rows_.front().version) == cref &&
				ca.at == allocator_before && Reason == reasons_before &&
				simp_stats.sc_proposals == 1 && simp_stats.sc_coefficients == 1 &&
				simp_stats.sc_revalidations == 0 && simp_stats.sc_commits == 0 &&
				literal != 0;
	}

	bool unchanged(bool expect_pair = true) {
		const uint32_t allocator_before = ca.at;
		const std::vector<rs::CRef> reasons_before = Reason;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		return ca.at == allocator_before && Reason == reasons_before &&
				simp_rows_.size() == 0 && simp_stats.sc_calls == 1 &&
				simp_stats.sc_proposals == 0 && simp_stats.sc_commits == 0 &&
				(expect_pair == (simp_stats.sc_pairs > 0));
	}

	bool batchIsSequential() {
		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED) return false;
		const GPMC::SimpRow* view = simp_rows_.find(rows_.front().version);
		if (view == nullptr || view->content.rhs != 7 ||
				view->content.coeffs != std::vector<Int>({5, 5, 2, 1}))
			return false;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		return simp_stats.sc_commits == 1 && simp_stats.sc_changed_rows == 1;
	}

	bool equalityCommit() {
		if (groups_.size() != 1 || rows_.size() != 3) return false;
		std::vector<rs::CRef> refs;
		for (const RowInfo& row : rows_)
			if (row.state == RowState::ATTACHED) refs.push_back(solverRef(row.version));
		const uint32_t allocator_before = ca.at;
		const std::vector<rs::CRef> reasons_before = Reason;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED) return false;
		const GPMC::SimpRow* lower = simp_rows_.find(groups_.front().lower);
		const GPMC::SimpRow* upper = simp_rows_.find(groups_.front().upper);
		if (lower == nullptr || upper == nullptr || lower->transaction == 0 ||
				lower->transaction != upper->transaction ||
				coefficient(*lower, 1) != 3)
			return false;
		if (Reason != reasons_before || ca.at != allocator_before)
			return false;
		for (size_t i = 0; i < refs.size(); ++i)
			if (solverRef(rows_[i].version) != refs[i]) return false;
		return simp_stats.sc_commits == 1 && simp_stats.sc_changed_rows == 1 &&
				simp_rows_.size() == 2;
	}

	bool reasonAndRollback() {
		if (rows_.size() != 2) return false;
		decide(-5);
		if (runPropagationWithoutLearnt(true)) return false;
		const RowVersionID version = rows_.front().version;
		const rs::CRef cref = solverRef(version);
		bool pinned = false;
		for (rs::Lit literal : trail)
			if (Reason[rs::toVar(literal)] == cref) pinned = true;
		if (!pinned) return false;
		const std::vector<rs::CRef> reasons_before = Reason;
		const uint32_t allocator_before = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED) return false;
		const GPMC::SimpRow* view = simp_rows_.find(version);
		if (view == nullptr || coefficient(*view, 2) != 3 ||
				Reason != reasons_before || ca.at != allocator_before ||
				solverRef(version) != cref)
			return false;
		backjumpTo(0);
		return simp_rows_.size() == 0 && solverRef(version) == cref;
	}

	bool ablationSkipsSC() {
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		return simp_stats.sc_calls == 0 && simp_stats.sc_rows == 0 &&
				simp_rows_.size() == 0;
	}
};

GPMC::Configuration scConfig(
		GPMC::CacheMode mode = GPMC::CacheMode::RESIDUAL) {
	GPMC::Configuration config;
	config.cntr.inprocess = true;
	config.cm.cacheMode = mode;
	config.cntr.ablation.insert(GPMC::SimpRule::BASIC);
	config.cntr.ablation.insert(GPMC::SimpRule::BICP);
	config.cntr.ablation.insert(GPMC::SimpRule::CS);
	config.cntr.ablation.insert(GPMC::SimpRule::GCD);
	config.cntr.ablation.insert(GPMC::SimpRule::CARD);
	return config;
}

int main() {
	const std::string positive =
			"* #variable= 3 #constraint= 2\n"
			"+8 x1 +3 x2 +2 x3 >= 5 ;\n"
			"+1 x2 +1 x3 >= 1 ;\n";
	{
		GPMC::Configuration config = scConfig(GPMC::CacheMode::BYPASS);
		SCTestCounter counter(config);
		counter.init();
		if (!counter.prepare(positive) || !counter.commit(1, 3)) return 1;
	}
	{
		GPMC::Configuration config = scConfig();
		SCTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 3 #constraint= 2\n"
				"+8 ~x1 +3 x2 +2 ~x3 >= 5 ;\n"
				"+1 x2 +1 ~x3 >= 1 ;\n";
		if (!counter.prepare(formula) || !counter.commit(-1, 3)) return 2;
	}
	{
		GPMC::Configuration config = scConfig();
		SCTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 3 #constraint= 2\n"
				"+800000000000000003 x1 +300000000000000002 x2 "
				"+200000000000000001 x3 >= 500000000000000003 ;\n"
				"+1 x2 +1 x3 >= 1 ;\n";
		if (!counter.prepare(formula) ||
				!counter.commit(1, 300000000000000002, false)) return 3;
	}
	{
		GPMC::Configuration config = scConfig();
		SCTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 5 #constraint= 2\n"
				"+8 x1 +4 x2 +3 x3 +2 x4 >= 6 ;\n"
				"+2 x2 +2 x3 +1 x5 >= 3 ;\n";
		if (!counter.prepare(formula) || !counter.commit(1, 3, false)) return 4;
	}
	{
		GPMC::Configuration config = scConfig();
		SCTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 2\n"
				"+10 x1 +3 x2 +2 x3 +4 x4 >= 7 ;\n"
				"+1 x2 +1 x3 +1 x4 >= 2 ;\n";
		if (!counter.prepare(formula) || !counter.commit(1, 2, false)) return 5;
	}
	{
		GPMC::Configuration config = scConfig();
		SCTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 2\n"
				"+8 x1 +3 x2 +2 x3 >= 5 ;\n"
				"+1 x2 +1 x4 >= 1 ;\n";
		if (!counter.prepare(formula) || !counter.unchanged()) return 6;
	}
	{
		GPMC::Configuration config = scConfig();
		SCTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 3 #constraint= 1\n"
				"+8 x1 +3 x2 +2 x3 >= 5 ;\n";
		if (!counter.prepare(formula) || !counter.unchanged(false)) return 7;
	}
	{
		GPMC::Configuration config = scConfig();
		SCTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 2\n"
				"+5 x1 +5 x2 +5 x3 +1 x4 >= 7 ;\n"
				"+1 x1 +1 x2 +1 x3 >= 2 ;\n";
		if (!counter.prepare(formula) || !counter.batchIsSequential()) return 8;
	}
	{
		GPMC::Configuration config = scConfig();
		SCTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 3 #constraint= 2\n"
				"+5 x1 +3 x2 +2 x3 = 5 ;\n"
				"+1 x2 +1 x3 >= 1 ;\n";
		if (!counter.prepare(formula) || !counter.equalityCommit()) return 9;
	}
	{
		GPMC::Configuration config = scConfig();
		SCTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 5 #constraint= 2\n"
				"+6 x1 +5 x2 +3 x3 +2 x4 +1 x5 >= 11 ;\n"
				"+1 x3 +1 x4 >= 1 ;\n";
		if (!counter.prepare(formula) || !counter.reasonAndRollback()) return 10;
	}
	{
		GPMC::Configuration config = scConfig(GPMC::CacheMode::LEGACY);
		SCTestCounter counter(config);
		counter.init();
		if (!counter.prepare(positive) || !counter.legacyProposal(1)) return 11;
	}
	{
		GPMC::Configuration config = scConfig();
		config.cntr.ablation.insert(GPMC::SimpRule::SC);
		SCTestCounter counter(config);
		counter.init();
		if (!counter.prepare(positive) || !counter.ablationSkipsSC()) return 12;
	}
	return 0;
}
