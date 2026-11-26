#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>
#include <vector>

class RprCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit RprCounter(GPMC::Configuration& config) : PBcounter(config) {}

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

	GPMC::ReferenceKey key() {
		syncAssign();
		return cmpmgr.referenceKey(cmpmgr.topComponent(), assigns);
	}

	bool active(RowVersionID version) const {
		const GPMC::SimpRow* row = simp_rows_.find(version);
		return row == nullptr || row->active;
	}

	bool exactChain() {
		if (rows_.size() != 3) return false;
		std::vector<rs::CRef> refs;
		std::vector<rs::ID> ids;
		for (const RowInfo& row : rows_) {
			refs.push_back(solverRef(row.version));
			ids.push_back(ca[refs.back()].id);
		}
		const std::vector<rs::CRef> reasons = Reason;
		const uint32_t allocator = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED ||
				simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED ||
				simplifyNode().status != GPMC::SimpStatus::UNCHANGED)
			return false;
		if (simp_stats.rpr_calls != 3 || simp_stats.rpr_proposals != 2 ||
				simp_stats.rpr_revalidations != 2 ||
				simp_stats.rpr_removed_rows != 2 || simp_stats.rpr_commits != 2)
			return false;
		int active_rows = 0;
		for (const RowInfo& row : rows_) active_rows += active(row.version);
		if (active_rows != 1 || Reason != reasons || ca.at != allocator)
			return false;
		for (size_t i = 0; i < rows_.size(); ++i)
			if (solverRef(rows_[i].version) != refs[i] || ca[refs[i]].id != ids[i])
				return false;
		return true;
	}

	bool collisionIsNotProof() {
		return rows_.size() == 2 &&
				simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.rpr_rows == 2 && simp_stats.rpr_pairs == 1 &&
				simp_stats.rpr_proposals == 0 && simp_rows_.size() == 0;
	}

	bool singleRemoval() {
		if (rows_.size() != 2 ||
				simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED ||
				simplifyNode().status != GPMC::SimpStatus::UNCHANGED)
			return false;
		return !active(rows_[0].version) && active(rows_[1].version) &&
				simp_stats.rpr_proposals == 1 &&
				simp_stats.rpr_revalidations == 1 &&
				simp_stats.rpr_removed_rows == 1 && simp_stats.rpr_commits == 1;
	}

	bool scopedRollback() {
		if (rows_.size() != 2 ||
				simplifyNode().status != GPMC::SimpStatus::UNCHANGED)
			return false;
		const GPMC::ReferenceKey root = key();
		decide(-1);
		if (runPropagationWithoutLearnt(true)) return false;
		const GPMC::ReferenceKey before = key();
		const std::vector<rs::CRef> reasons = Reason;
		const uint32_t allocator = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED) return false;
		const GPMC::ReferenceKey after = key();
		const RowVersionID target = rows_[1].version;
		const rs::CRef attached = solverRef(target);
		const rs::ID id = ca[attached].id;
		const GPMC::SimpRow* removed = simp_rows_.find(target);
		if (removed == nullptr || removed->active ||
				removed->level != decisionLevel() || before == after ||
				Reason != reasons || ca.at != allocator)
			return false;
		backjumpTo(0);
		return simp_rows_.find(target) == nullptr && key() == root &&
				solverRef(target) == attached && ca[attached].id == id;
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
		const RowInfo& helper = rows_[1];
		const PBconstraint content = contentOf(
				ca[solverRef(helper.version)], helper.source);
		if (!simp_rows_.set(helper, content, true, decisionLevel())) return false;
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.rpr_proposals == 0 && simp_stats.rpr_commits == 0 &&
				simp_rows_.find(rows_[0].version) == nullptr;
	}

	bool equalityGroups() {
		if (groups_.size() != 2 || rows_.size() != 4) return false;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED ||
				simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED ||
				simplifyNode().status != GPMC::SimpStatus::UNCHANGED)
			return false;
		const EqGroup& keep = groups_[0];
		const EqGroup& drop = groups_[1];
		const GPMC::SimpRow* lower = simp_rows_.find(drop.lower);
		const GPMC::SimpRow* upper = simp_rows_.find(drop.upper);
		return simp_rows_.find(keep.lower) == nullptr &&
				simp_rows_.find(keep.upper) == nullptr && lower != nullptr &&
				upper != nullptr && !lower->active && !upper->active &&
				lower->transaction != 0 &&
				lower->transaction == upper->transaction &&
				simp_stats.rpr_equality_rows >= 4 &&
				simp_stats.rpr_removed_rows == 2 && simp_stats.rpr_commits == 2;
	}

	bool legacyProposal() {
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.rpr_calls == 1 && simp_stats.rpr_proposals == 1 &&
				simp_stats.rpr_revalidations == 0 &&
				simp_stats.rpr_commits == 0 && simp_rows_.size() == 0;
	}

	bool ablationSkipsRpr() {
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.rpr_calls == 0 && simp_rows_.size() == 0;
	}
};

constexpr const char* kChain =
		"* #variable= 4 #constraint= 3\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 >= 1 ;\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 >= 2 ;\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 >= 3 ;\n";
constexpr const char* kProportionalNegative =
		"* #variable= 5 #constraint= 2\n"
		"+2 ~x1 +2 x2 +2 ~x3 +2 x4 +2 x5 >= 5 ;\n"
		"+3 x5 +3 x4 +3 ~x3 +3 x2 +3 ~x1 >= 10 ;\n";
constexpr const char* kHashCollision =
		"* #variable= 4 #constraint= 2\n"
		"+2 x1 +1 x2 +1 x3 +1 x4 >= 2 ;\n"
		"+1 x1 +2 x2 +1 x3 +1 x4 >= 2 ;\n";
constexpr const char* kScoped =
		"* #variable= 3 #constraint= 2\n"
		"+1 x1 +1 x2 +1 x3 >= 1 ;\n"
		"+1 x2 +1 x3 >= 1 ;\n";
constexpr const char* kLiveReason =
		"* #variable= 6 #constraint= 2\n"
		"+2 x1 +3 x2 +1 x3 +1 x4 +1 x5 +1 x6 >= 5 ;\n"
		"+2 x1 +3 x2 +1 x3 +1 x4 +1 x5 +1 x6 >= 6 ;\n";
constexpr const char* kOverlayHelper =
		"* #variable= 4 #constraint= 2\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 >= 1 ;\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 >= 2 ;\n";
constexpr const char* kEquality =
		"* #variable= 4 #constraint= 2\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 = 2 ;\n"
		"+1 x1 +1 x2 +1 x3 +1 x4 = 2 ;\n";

GPMC::Configuration config(GPMC::CacheMode cache, bool disable_rpr = false) {
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
	if (disable_rpr) result.cntr.ablation.insert(GPMC::SimpRule::RPR);
	result.cntr.ablation.insert(GPMC::SimpRule::RSR);
	result.cntr.ablation.insert(GPMC::SimpRule::RUP);
	result.cntr.ablation.insert(GPMC::SimpRule::SBLP);
	return result;
}

int main() {
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RprCounter counter(options);
		counter.init();
		if (!counter.prepare(kChain) || !counter.exactChain()) return 1;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::RESIDUAL);
		RprCounter counter(options);
		counter.init();
		if (!counter.prepare(kProportionalNegative) || !counter.singleRemoval())
			return 2;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RprCounter counter(options);
		counter.init();
		if (!counter.prepare(kHashCollision) || !counter.collisionIsNotProof())
			return 3;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::RESIDUAL);
		RprCounter counter(options);
		counter.init();
		if (!counter.prepare(kScoped) || !counter.scopedRollback()) return 4;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RprCounter counter(options);
		counter.init();
		if (!counter.prepare(kLiveReason) || !counter.liveReasonRollback()) return 5;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RprCounter counter(options);
		counter.init();
		if (!counter.prepare(kOverlayHelper) || !counter.overlayHelperBlocked())
			return 6;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RprCounter counter(options);
		counter.init();
		if (!counter.prepare(kEquality) || !counter.equalityGroups()) return 7;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::LEGACY);
		RprCounter counter(options);
		counter.init();
		if (!counter.prepare(kChain) || !counter.legacyProposal()) return 8;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS, true);
		RprCounter counter(options);
		counter.init();
		if (!counter.prepare(kChain) || !counter.ablationSkipsRpr()) return 9;
	}
	return 0;
}
