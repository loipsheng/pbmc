#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

class RirCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit RirCounter(GPMC::Configuration& config) : PBcounter(config) {}

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

	GPMC::ReferenceKey key() {
		syncAssign();
		return cmpmgr.referenceKey(cmpmgr.topComponent(), assigns);
	}

	bool singleCommitAndRollback(rs::Lit literal) {
		if (rows_.size() != 1 || rows_.front().state != RowState::ATTACHED)
			return false;
		const GPMC::ReferenceKey root = key();
		const RowVersionID version = rows_.front().version;
		const rs::CRef cref = solverRef(version);
		const rs::ID id = ca[cref].id;
		const rs::BigVal degree = ca[cref].degree();
		std::vector<std::pair<rs::Lit, rs::BigCoef>> terms;
		for (size_t i = 0; i < ca[cref].size(); ++i)
			terms.emplace_back(ca[cref].lit(i), ca[cref].coef(i));

		decide(literal);
		if (runPropagationWithoutLearnt(true)) return false;
		const GPMC::ReferenceKey satisfied = key();
		const std::vector<rs::CRef> reasons = Reason;
		const uint32_t allocator = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED) return false;
		const GPMC::SimpRow* removed = simp_rows_.find(version);
		if (removed == nullptr || removed->active || removed->transaction != 0 ||
				removed->level != decisionLevel() || key() != satisfied)
			return false;
		if (simp_stats.rir_calls != 1 || simp_stats.rir_rows != 1 ||
				simp_stats.rir_equality_rows != 0 ||
				simp_stats.rir_proposals != 1 ||
				simp_stats.rir_removed_rows != 1 || simp_stats.rir_commits != 1)
			return false;
		if (Reason != reasons || ca.at != allocator || solverRef(version) != cref ||
				ca[cref].id != id || ca[cref].degree() != degree ||
				ca[cref].size() != terms.size())
			return false;
		for (size_t i = 0; i < terms.size(); ++i)
			if (ca[cref].lit(i) != terms[i].first || ca[cref].coef(i) != terms[i].second)
				return false;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED ||
				simp_stats.rir_commits != 1)
			return false;

		backjumpTo(0);
		return simp_rows_.find(version) == nullptr && key() == root &&
				solverRef(version) == cref && ca[cref].id == id;
	}

	bool equalityReasonAndRollback() {
		if (groups_.size() != 1 || rows_.size() != 2) return false;
		decide(1);
		if (runPropagationWithoutLearnt(true)) return false;
		if (!rs::isFalse(Level, 2) || Reason[2] == rs::CRef_Undef) return false;
		const rs::CRef reason = Reason[2];
		const rs::ID reason_id = ca[reason].id;
		const std::vector<rs::CRef> reasons = Reason;
		const uint32_t allocator = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_REMOVED) return false;

		const EqGroup& group = groups_.front();
		const GPMC::SimpRow* lower = simp_rows_.find(group.lower);
		const GPMC::SimpRow* upper = simp_rows_.find(group.upper);
		if (lower == nullptr || upper == nullptr || lower->active || upper->active ||
				lower->transaction == 0 || lower->transaction != upper->transaction ||
				lower->level != decisionLevel() || upper->level != decisionLevel())
			return false;
		if (simp_stats.rir_equality_rows != 1 ||
				simp_stats.rir_removed_rows != 2 || simp_stats.rir_commits != 1 ||
				Reason != reasons || ca.at != allocator || ca[Reason[2]].id != reason_id)
			return false;

		backjumpTo(0);
		return simp_rows_.find(group.lower) == nullptr &&
				simp_rows_.find(group.upper) == nullptr && ca[reason].id == reason_id;
	}

	bool unchanged(rs::Lit literal) {
		decide(literal);
		if (runPropagationWithoutLearnt(true)) return false;
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_rows_.size() == 0 && simp_stats.rir_calls == 1 &&
				simp_stats.rir_proposals == 0 && simp_stats.rir_commits == 0;
	}

	bool legacyProposal(rs::Lit literal) {
		decide(literal);
		if (runPropagationWithoutLearnt(true)) return false;
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_rows_.size() == 0 && simp_stats.rir_calls == 1 &&
				simp_stats.rir_proposals == 1 &&
				simp_stats.rir_removed_rows == 0 && simp_stats.rir_commits == 0;
	}

	bool ablationSkipsRir(rs::Lit literal) {
		decide(literal);
		if (runPropagationWithoutLearnt(true)) return false;
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_rows_.size() == 0 && simp_stats.rir_calls == 0;
	}
};

constexpr const char* kPositive =
		"* #variable= 3 #constraint= 1\n"
		"+2 x1 +1 x2 +1 x3 >= 2 ;\n";
constexpr const char* kNegative =
		"* #variable= 3 #constraint= 1\n"
		"+2 ~x1 +1 x2 +1 x3 >= 2 ;\n";
constexpr const char* kOpen =
		"* #variable= 3 #constraint= 1\n"
		"+1 x1 +1 x2 +1 x3 >= 2 ;\n";
constexpr const char* kEquality =
		"* #variable= 2 #constraint= 1\n"
		"+1 x1 +1 x2 = 1 ;\n";

GPMC::Configuration config(GPMC::CacheMode cache, bool disable_rir = false) {
	GPMC::Configuration result;
	result.cntr.inprocess = true;
	result.cm.cacheMode = cache;
	result.cntr.ablation.insert(GPMC::SimpRule::BASIC);
	result.cntr.ablation.insert(GPMC::SimpRule::BICP);
	result.cntr.ablation.insert(GPMC::SimpRule::CS);
	result.cntr.ablation.insert(GPMC::SimpRule::SC);
	result.cntr.ablation.insert(GPMC::SimpRule::GCD);
	result.cntr.ablation.insert(GPMC::SimpRule::CARD);
	if (disable_rir) result.cntr.ablation.insert(GPMC::SimpRule::RIR);
	result.cntr.ablation.insert(GPMC::SimpRule::RPR);
	result.cntr.ablation.insert(GPMC::SimpRule::RSR);
	result.cntr.ablation.insert(GPMC::SimpRule::RUP);
	result.cntr.ablation.insert(GPMC::SimpRule::SBLP);
	return result;
}

int main() {
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RirCounter counter(options);
		counter.init();
		if (!counter.prepare(kPositive) || !counter.singleCommitAndRollback(1))
			return 1;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::RESIDUAL);
		RirCounter counter(options);
		counter.init();
		if (!counter.prepare(kNegative) || !counter.singleCommitAndRollback(-1))
			return 2;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RirCounter counter(options);
		counter.init();
		if (!counter.prepare(kEquality) || !counter.equalityReasonAndRollback())
			return 3;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS);
		RirCounter counter(options);
		counter.init();
		if (!counter.prepare(kOpen) || !counter.unchanged(1)) return 4;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::LEGACY);
		RirCounter counter(options);
		counter.init();
		if (!counter.prepare(kPositive) || !counter.legacyProposal(1)) return 5;
	}
	{
		GPMC::Configuration options = config(GPMC::CacheMode::BYPASS, true);
		RirCounter counter(options);
		counter.init();
		if (!counter.prepare(kPositive) || !counter.ablationSkipsRir(1)) return 6;
	}
	return 0;
}
