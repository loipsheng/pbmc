#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>
#include <vector>

class RsrCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit RsrCounter(GPMC::Configuration& config) : PBcounter(config) {}

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

	bool removesSecond() {
		if (rows_.size() != 2 ||
				simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED ||
				simplifyNode().status != GPMC::SimpStatus::UNCHANGED)
			return false;
		const GPMC::SimpRow* removed = simp_rows_.find(rows_[1].version);
		return removed != nullptr && !removed->active &&
				simp_rows_.find(rows_[0].version) == nullptr &&
				simp_stats.rsr_proposals == 1 &&
				simp_stats.rsr_revalidations == 1 &&
				simp_stats.rsr_removed_rows == 1 && simp_stats.rsr_commits == 1;
	}

	bool noFalseDelete() {
		return rows_.size() == 2 &&
				simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.rsr_pairs == 1 && simp_stats.rsr_checks == 2 &&
				simp_stats.rsr_proposals == 0 && simp_rows_.size() == 0;
	}

	bool equalityDirection() {
		if (groups_.size() != 1 || rows_.size() != 3 ||
				simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED)
			return false;
		const EqGroup& group = groups_[0];
		const GPMC::SimpRow* lower = simp_rows_.find(group.lower);
		const GPMC::SimpRow* upper = simp_rows_.find(group.upper);
		return lower != nullptr && upper != nullptr && !lower->active &&
				upper->active && lower->transaction != 0 &&
				lower->transaction == upper->transaction &&
				simp_stats.rsr_equality_rows >= 2 &&
				simp_stats.rsr_removed_rows == 1;
	}

	bool liveReasonRollback() {
		if (rows_.size() != 2) return false;
		const RowVersionID target = rows_[0].version;
		const rs::CRef attached = solverRef(target);
		const rs::ID id = ca[attached].id;
		decide(-1);
		if (runPropagationWithoutLearnt(true) || !rs::isTrue(Level, 2) ||
				Reason[2] != attached)
			return false;
		const std::vector<rs::CRef> reasons = Reason;
		const uint32_t allocator = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED) return false;
		const GPMC::SimpRow* removed = simp_rows_.find(target);
		if (removed == nullptr || removed->active || Reason != reasons ||
				Reason[2] != attached || ca.at != allocator || ca[attached].id != id)
			return false;
		backjumpTo(0);
		return simp_rows_.find(target) == nullptr && solverRef(target) == attached &&
				ca[attached].id == id;
	}

	bool overlayHelperBlocked() {
		if (rows_.size() != 2) return false;
		const RowInfo& helper = rows_[0];
		const PBconstraint content = contentOf(
				ca[solverRef(helper.version)], helper.source);
		if (!simp_rows_.set(helper, content, true, decisionLevel())) return false;
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.rsr_proposals == 0 && simp_stats.rsr_commits == 0 &&
				simp_rows_.find(rows_[1].version) == nullptr;
	}

	bool legacyProposal() {
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.rsr_calls == 1 && simp_stats.rsr_proposals == 1 &&
				simp_stats.rsr_revalidations == 0 &&
				simp_stats.rsr_commits == 0 && simp_rows_.size() == 0;
	}

	bool ablationSkipsRsr() {
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.rsr_calls == 0 && simp_rows_.size() == 0;
	}

	int split() {
		syncAssign();
		return cmpmgr.splitComponent(assigns);
	}

	bool removeBridgeAndSplit() {
		return simplifyNode().status == GPMC::SimpStatus::ROW_REMOVED &&
				split() == 2;
	}
};

constexpr const char* kPositive =
		"* #variable= 4 #constraint= 2\n"
		"+1 x1 +1 x2 +1 x3 >= 2 ;\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 >= 2 ;\n";
constexpr const char* kNegative =
		"* #variable= 4 #constraint= 2\n"
		"+1 x1 +1 x2 +1 x3 >= 2 ;\n"
		"+1 x1 +1 x2 +1 x3 +1 ~x4 >= 2 ;\n";
constexpr const char* kCoefficients =
		"* #variable= 4 #constraint= 2\n"
		"+2 x1 +1 x2 +1 x3 >= 2 ;\n"
		"+2 x1 +2 x2 +2 x3 +1 x4 >= 2 ;\n";
constexpr const char* kLessEqual =
		"* #variable= 4 #constraint= 2\n"
		"+1 x1 +1 x2 +1 x3 <= 1 ;\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 <= 2 ;\n";
constexpr const char* kDifferentRhs =
		"* #variable= 4 #constraint= 2\n"
		"+1 x1 +1 x2 +1 x3 >= 2 ;\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 >= 1 ;\n";
constexpr const char* kCounterexample =
		"* #variable= 3 #constraint= 2\n"
		"+2 x1 +1 x2 +1 x3 >= 2 ;\n"
		"+1 x1 +2 x2 +1 x3 >= 2 ;\n";
constexpr const char* kPolarityCounterexample =
		"* #variable= 3 #constraint= 2\n"
		"+1 x1 +1 x2 +1 x3 >= 2 ;\n"
		"+1 ~x1 +1 x2 +1 x3 >= 2 ;\n";
constexpr const char* kEquality =
		"* #variable= 4 #constraint= 2\n"
		"+1 x1 +1 x2 +1 x3 >= 2 ;\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 = 2 ;\n";
constexpr const char* kLiveReason =
		"* #variable= 7 #constraint= 2\n"
		"+2 x1 +3 x2 +1 x3 +1 x4 +1 x5 +1 x6 >= 5 ;\n"
		"+2 x1 +3 x2 +1 x3 +1 x4 +1 x5 +1 x6 +1 x7 >= 6 ;\n";
constexpr const char* kBridge =
		"* #variable= 5 #constraint= 3\n"
		"+1 x1 +1 x2 +1 x3 >= 2 ;\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 >= 2 ;\n"
		"+1 x4 +1 x5 >= 1 ;\n";

GPMC::Configuration config(GPMC::CacheMode cache, bool disable_rsr = false) {
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
	if (disable_rsr) result.cntr.ablation.insert(GPMC::SimpRule::RSR);
	result.cntr.ablation.insert(GPMC::SimpRule::RUP);
	result.cntr.ablation.insert(GPMC::SimpRule::SBLP);
	return result;
}

int main() {
	for (const char* formula : {
			kPositive, kNegative, kCoefficients, kLessEqual, kDifferentRhs}) {
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RsrCounter counter(options);
		counter.init();
		if (!counter.prepare(formula) || !counter.removesSecond()) return 1;
	}
	for (const char* formula : {kCounterexample, kPolarityCounterexample}) {
		GPMC::Configuration options = config(GPMC::CacheMode::RESIDUAL);
		RsrCounter counter(options);
		counter.init();
		if (!counter.prepare(formula) || !counter.noFalseDelete()) return 2;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RsrCounter counter(options);
		counter.init();
		if (!counter.prepare(kEquality) || !counter.equalityDirection()) return 3;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::RESIDUAL);
		RsrCounter counter(options);
		counter.init();
		if (!counter.prepare(kLiveReason) || !counter.liveReasonRollback()) return 4;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RsrCounter counter(options);
		counter.init();
		if (!counter.prepare(kPositive) || !counter.overlayHelperBlocked()) return 5;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::LEGACY);
		RsrCounter counter(options);
		counter.init();
		if (!counter.prepare(kPositive) || !counter.legacyProposal()) return 6;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS, true);
		RsrCounter counter(options);
		counter.init();
		if (!counter.prepare(kPositive) || !counter.ablationSkipsRsr()) return 7;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::RESIDUAL);
		RsrCounter connected(options);
		connected.init();
		if (!connected.prepare(kBridge) || connected.split() != 1) return 8;
		RsrCounter separated(options);
		separated.init();
		if (!separated.prepare(kBridge) || !separated.removeBridgeAndSplit())
			return 9;
	}
	return 0;
}
