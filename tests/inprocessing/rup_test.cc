#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>
#include <vector>

class RupCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit RupCounter(GPMC::Configuration& config) : PBcounter(config) {}

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

	bool removesFirst() {
		if (rows_.size() < 3) return false;
		const std::vector<rs::Lit> trail_before = trail;
		const std::vector<rs::CRef> reasons_before = Reason;
		const size_t constraints_before = constraints.size();
		const uint32_t allocator_before = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED)
			return false;
		const GPMC::SimpRow* removed = simp_rows_.find(rows_[0].version);
		return removed != nullptr && !removed->active &&
				trail == trail_before && Reason == reasons_before &&
				constraints.size() == constraints_before && ca.at == allocator_before &&
				simp_stats.rup_proofs == 1 && simp_stats.rup_revalidations == 1 &&
				simp_stats.rup_removed_rows == 1 && simp_stats.rup_commits == 1;
	}

	bool noFalseDelete() {
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.rup_checks > 0 && simp_stats.rup_proofs == 0 &&
				simp_stats.rup_commits == 0 && simp_rows_.size() == 0;
	}

	bool equalityDirection() {
		if (groups_.size() != 1 || rows_.size() != 4 ||
				simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED)
			return false;
		const EqGroup& group = groups_[0];
		const GPMC::SimpRow* lower = simp_rows_.find(group.lower);
		const GPMC::SimpRow* upper = simp_rows_.find(group.upper);
		return lower != nullptr && upper != nullptr && !lower->active &&
				upper->active && lower->transaction != 0 &&
				lower->transaction == upper->transaction &&
				simp_stats.rup_equality_rows >= 2;
	}

	bool branchRollback() {
		if (rows_.size() != 3) return false;
		decide(5);
		if (runPropagationWithoutLearnt(true)) return false;
		decide(-4);
		if (runPropagationWithoutLearnt(true) || decisionLevel() != 2)
			return false;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED)
			return false;
		const GPMC::SimpRow* removed = simp_rows_.find(rows_[0].version);
		if (removed == nullptr || removed->active || removed->level != 2)
			return false;
		backjumpTo(0);
		return simp_rows_.find(rows_[0].version) == nullptr &&
				decisionLevel() == 0;
	}

	bool liveReasonRollback() {
		if (rows_.size() != 3) return false;
		const rs::CRef attached = solverRef(rows_[0].version);
		const rs::ID id = ca[attached].id;
		decide(-7);
		if (runPropagationWithoutLearnt(true) || !rs::isTrue(Level, 1) ||
				Reason[1] != attached)
			return false;
		const std::vector<rs::CRef> reasons = Reason;
		const uint32_t allocator = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED)
			return false;
		const GPMC::SimpRow* removed = simp_rows_.find(rows_[0].version);
		if (removed == nullptr || removed->active || Reason != reasons ||
				Reason[1] != attached || ca.at != allocator || ca[attached].id != id)
			return false;
		backjumpTo(0);
		return simp_rows_.find(rows_[0].version) == nullptr &&
				solverRef(rows_[0].version) == attached && ca[attached].id == id;
	}

	bool overlayHelperBlocked() {
		if (rows_.size() != 3) return false;
		const RowInfo& helper = rows_[1];
		const PBconstraint content = contentOf(
				ca[solverRef(helper.version)], helper.source);
		if (!simp_rows_.set(helper, content, true, decisionLevel())) return false;
		const GPMC::SimpResult result = simplifyNode();
		const GPMC::SimpRow* target = simp_rows_.find(rows_[0].version);
		return result.status == GPMC::SimpStatus::UNCHANGED &&
				(target == nullptr || target->active) &&
				simp_stats.rup_proofs == 0 && simp_stats.rup_commits == 0;
	}

	bool learnedIgnored() {
		PBconstraint learned;
		learned.lits = {1, -2};
		learned.coeffs = {1, 1};
		learned.rhs = 1;
		rs::CeArb constraint = makeCe(learned);
		constraint->orig = rs::Origin::LEARNED;
		constraint->postProcess(Level, Pos, true, rs::stats);
		attachConstraint(constraint, false);
		if (runPropagationWithoutLearnt(true)) return false;
		return noFalseDelete();
	}

	bool budgetIsUnknown() {
		return simplifyNode().status == GPMC::SimpStatus::BUDGET &&
				simp_stats.rup_budgets == 1 && simp_stats.rup_proofs == 0 &&
				simp_stats.rup_commits == 0 && simp_rows_.size() == 0;
	}

	bool legacyProposal() {
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.rup_calls == 1 && simp_stats.rup_proofs == 1 &&
				simp_stats.rup_revalidations == 0 &&
				simp_stats.rup_commits == 0 && simp_rows_.size() == 0;
	}

	bool ablationSkipsRup() {
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.rup_calls == 0 && simp_rows_.size() == 0;
	}
};

constexpr const char* kPositive =
		"* #variable= 4 #constraint= 3\n"
		"+1 x1 +1 x3 >= 1 ;\n"
		"+1 x1 +1 x2 >= 1 ;\n"
		"+1 x1 +1 ~x2 >= 1 ;\n";
constexpr const char* kNegative =
		"* #variable= 4 #constraint= 3\n"
		"+1 ~x1 +1 x3 >= 1 ;\n"
		"+1 ~x1 +1 x2 >= 1 ;\n"
		"+1 ~x1 +1 ~x2 >= 1 ;\n";
constexpr const char* kCoefficients =
		"* #variable= 4 #constraint= 3\n"
		"+2 x1 +1 x3 +1 x4 >= 2 ;\n"
		"+1 x1 +1 x2 >= 1 ;\n"
		"+1 x1 +1 ~x2 >= 1 ;\n";
constexpr const char* kNoProof =
		"* #variable= 3 #constraint= 2\n"
		"+1 x1 +1 x3 >= 1 ;\n"
		"+1 x1 +1 x2 >= 1 ;\n";
constexpr const char* kEquality =
		"* #variable= 3 #constraint= 3\n"
		"+1 x1 +1 x3 = 1 ;\n"
		"+1 x1 +1 x2 >= 1 ;\n"
		"+1 x1 +1 ~x2 >= 1 ;\n";
constexpr const char* kBranch =
		"* #variable= 5 #constraint= 3\n"
		"+1 x1 +1 x3 >= 1 ;\n"
		"+1 x1 +1 x2 >= 1 ;\n"
		"+1 x1 +1 ~x2 +1 x4 >= 1 ;\n";
constexpr const char* kReason =
		"* #variable= 8 #constraint= 3\n"
		"+5 x1 +1 x2 +1 x3 +1 x4 +1 x5 +1 x6 +5 x7 >= 6 ;\n"
		"+1 x2 +1 x8 >= 1 ;\n"
		"+1 x2 +1 ~x8 >= 1 ;\n";

std::string budgetFormula() {
	std::ostringstream formula;
	formula << "* #variable= 35 #constraint= 33\n";
	for (int variable = 2; variable <= 34; ++variable)
		formula << "+1 x1 +1 x" << variable << " >= 1 ;\n";
	return formula.str();
}

GPMC::Configuration config(GPMC::CacheMode cache, bool disable_rup = false) {
	GPMC::Configuration result;
	result.cntr.inprocess = true;
	result.cm.cacheMode = cache;
	result.cntr.ablation.insert(GPMC::SimpRule::BASIC);
	result.cntr.ablation.insert(GPMC::SimpRule::BICP);
	result.cntr.ablation.insert(GPMC::SimpRule::CS);
	result.cntr.ablation.insert(GPMC::SimpRule::SC);
	result.cntr.ablation.insert(GPMC::SimpRule::GCD);
	result.cntr.ablation.insert(GPMC::SimpRule::CARD);
	result.cntr.ablation.insert(GPMC::SimpRule::RIR);
	result.cntr.ablation.insert(GPMC::SimpRule::RPR);
	result.cntr.ablation.insert(GPMC::SimpRule::RSR);
	if (disable_rup) result.cntr.ablation.insert(GPMC::SimpRule::RUP);
	result.cntr.ablation.insert(GPMC::SimpRule::SBLP);
	return result;
}

int main() {
	for (const char* formula : {kPositive, kNegative, kCoefficients}) {
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RupCounter counter(options);
		counter.init();
		if (!counter.prepare(formula) || !counter.removesFirst()) return 1;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::RESIDUAL);
		RupCounter counter(options);
		counter.init();
		if (!counter.prepare(kNoProof) || !counter.noFalseDelete()) return 2;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RupCounter counter(options);
		counter.init();
		if (!counter.prepare(kEquality) || !counter.equalityDirection()) return 3;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::RESIDUAL);
		RupCounter counter(options);
		counter.init();
		if (!counter.prepare(kBranch) || !counter.branchRollback()) return 4;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::RESIDUAL);
		RupCounter counter(options);
		counter.init();
		if (!counter.prepare(kReason) || !counter.liveReasonRollback()) return 5;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RupCounter counter(options);
		counter.init();
		if (!counter.prepare(kPositive) || !counter.overlayHelperBlocked()) return 6;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RupCounter counter(options);
		counter.init();
		if (!counter.prepare(kNoProof) || !counter.learnedIgnored()) return 7;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::RESIDUAL);
		RupCounter counter(options);
		counter.init();
		if (!counter.prepare(budgetFormula()) || !counter.budgetIsUnknown()) return 8;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::LEGACY);
		RupCounter counter(options);
		counter.init();
		if (!counter.prepare(kPositive) || !counter.legacyProposal()) return 9;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS, true);
		RupCounter counter(options);
		counter.init();
		if (!counter.prepare(kPositive) || !counter.ablationSkipsRup()) return 10;
	}
	return 0;
}
