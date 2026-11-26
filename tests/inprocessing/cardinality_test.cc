#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

class CardTestCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit CardTestCounter(GPMC::Configuration& config) : PBcounter(config) {}

	void reservePrefix() {
		const uint32_t padding = static_cast<uint32_t>(
				rs::ConstraintAllocator::alignedWordSize(17));
		ca.capacity(padding);
		ca.at = padding;
	}

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

	bool normalization(rs::Lit assignment, Int rhs, size_t terms) {
		if (rows_.size() != 1 || rows_.front().state != RowState::ATTACHED)
			return false;
		const RowInfo& row = rows_.front();
		const rs::CRef cref = solverRef(row.version);
		const rs::ID id = ca[cref].id;
		const rs::BigVal degree = ca[cref].degree();
		std::vector<std::pair<rs::Lit, rs::BigCoef>> attached;
		for (size_t i = 0; i < ca[cref].size(); ++i)
			attached.emplace_back(ca[cref].lit(i), ca[cref].coef(i));

		decide(assignment);
		if (runPropagationWithoutLearnt(true)) return false;
		const std::vector<rs::Lit> trail_before = trail;
		const std::vector<rs::CRef> reasons_before = Reason;
		const uint32_t allocator_before = ca.at;

		const PBconstraint original = contentOf(ca[cref], row.source);
		rs::BigVal gap(original.rhs);
		rs::CeArb direct = cePools.takeArb();
		for (size_t i = 0; i < original.lits.size(); ++i) {
			const rs::Lit literal = static_cast<rs::Lit>(original.lits[i]);
			const rs::BigCoef coefficient(original.coeffs[i]);
			if (rs::isTrue(Level, literal)) {
				gap -= coefficient;
			} else if (!rs::isFalse(Level, literal)) {
				direct->addLhs(coefficient, literal);
			}
		}
		direct->addRhs(gap);
		rs::Stats direct_stats;
		direct->postProcess(Level, Pos, true, direct_stats);
		const PBconstraint expected = contentOf(direct, row.source);
		if (direct_stats.NCARDDETECT != 1) return false;

		const bool legacy = cmpmgr.cache().legacy();
		const GPMC::SimpStatus status = simplifyNode().status;
		if (legacy) {
			return status == GPMC::SimpStatus::UNCHANGED && simp_rows_.size() == 0 &&
					simp_stats.card_calls == 1 && simp_stats.card_rows == 1 &&
					simp_stats.card_changed_rows == 1 &&
					simp_stats.card_commits == 0;
		}
		if (status != GPMC::SimpStatus::ROW_CHANGED || trail != trail_before ||
				Reason != reasons_before || ca.at != allocator_before)
			return false;

		const GPMC::SimpRow* view = simp_rows_.find(row.version);
		if (view == nullptr || !view->active || view->content.rhs != rhs ||
				view->content.lits.size() != terms ||
				view->content.lits != expected.lits ||
				view->content.coeffs != expected.coeffs ||
				view->content.rhs != expected.rhs)
			return false;
		for (Int coefficient : view->content.coeffs)
			if (coefficient != 1) return false;
		for (Int literal : view->content.lits)
			if (literal == assignment) return false;

		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		const rs::CRef after = solverRef(row.version);
		if (after != cref || ca[after].id != id || ca[after].degree() != degree ||
				ca[after].size() != attached.size() || ca.at != allocator_before ||
				Reason != reasons_before)
			return false;
		for (size_t i = 0; i < attached.size(); ++i)
			if (ca[after].lit(i) != attached[i].first ||
					ca[after].coef(i) != attached[i].second)
				return false;
		if (simp_stats.card_calls != 2 || simp_stats.card_rows != 2 ||
				simp_stats.card_changed_rows != 1 || simp_stats.card_commits != 1)
			return false;
		backjumpTo(0);
		return simp_rows_.size() == 0 && solverRef(row.version) == cref;
	}

	bool nonCardinality() {
		decide(-4);
		if (runPropagationWithoutLearnt(true)) return false;
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.card_calls == 1 && simp_stats.card_rows == 1 &&
				simp_stats.card_changed_rows == 0 && simp_stats.card_commits == 0 &&
				simp_rows_.size() == 0;
	}

	bool equalityCommit() {
		if (groups_.size() != 1 || rows_.size() != 2) return false;
		decide(-4);
		if (runPropagationWithoutLearnt(true)) return false;
		std::vector<rs::CRef> refs;
		for (const RowInfo& row : rows_) refs.push_back(solverRef(row.version));
		const std::vector<rs::CRef> reasons_before = Reason;
		const uint32_t allocator_before = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED) return false;

		const GPMC::SimpRow* lower = simp_rows_.find(groups_.front().lower);
		const GPMC::SimpRow* upper = simp_rows_.find(groups_.front().upper);
		if (lower == nullptr || upper == nullptr || lower->transaction == 0 ||
				lower->transaction != upper->transaction || lower->content.rhs != 2 ||
				lower->content.coeffs != std::vector<Int>({1, 1, 1}) ||
				upper->content.rhs != 3 ||
				upper->content.coeffs != std::vector<Int>({3, 3, 2}))
			return false;
		if (Reason != reasons_before || ca.at != allocator_before ||
				solverRef(rows_[0].version) != refs[0] ||
				solverRef(rows_[1].version) != refs[1])
			return false;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		if (simp_stats.card_equality_rows != 4 ||
				simp_stats.card_changed_rows != 1 || simp_stats.card_commits != 1 ||
				simp_rows_.size() != 2)
			return false;
		backjumpTo(0);
		return simp_rows_.size() == 0;
	}

	bool reasonAndGC() {
		if (rows_.size() != 1 || rows_.front().state != RowState::ATTACHED)
			return false;
		decide(-5);
		if (runPropagationWithoutLearnt(true)) return false;
		const RowVersionID version = rows_.front().version;
		const rs::CRef before = solverRef(version);
		rs::Var implied = 0;
		for (rs::Lit literal : trail)
			if (Reason[rs::toVar(literal)] == before) implied = rs::toVar(literal);
		if (implied == 0) return false;

		const std::vector<rs::CRef> reasons_before = Reason;
		const uint32_t allocator_before = ca.at;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED) return false;
		const GPMC::SimpRow* view = simp_rows_.find(version);
		if (view == nullptr || view->content.rhs != 2 ||
				view->content.coeffs != std::vector<Int>({1, 1, 1}) ||
				Reason != reasons_before || ca.at != allocator_before)
			return false;

		garbage_collect();
		validateRows(true);
		const rs::CRef moved = solverRef(version);
		if (moved == before || Reason[implied] != moved) return false;
		view = simp_rows_.find(version);
		if (view == nullptr || view->content.rhs != 2 ||
				view->content.coeffs != std::vector<Int>({1, 1, 1}))
			return false;
		backjumpTo(0);
		return simp_rows_.size() == 0 && solverRef(version) == moved;
	}

	bool gcdThenCard() {
		decide(-4);
		if (runPropagationWithoutLearnt(true)) return false;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED ||
				simp_stats.gcd_commits != 1 || simp_stats.card_calls != 0)
			return false;
		const GPMC::SimpRow* view = simp_rows_.find(rows_.front().version);
		if (view == nullptr || view->content.rhs != 5 ||
				view->content.coeffs != std::vector<Int>({3, 3, 2}))
			return false;
		if (simplifyNode().status != GPMC::SimpStatus::ROW_CHANGED ||
				simp_stats.card_commits != 1)
			return false;
		view = simp_rows_.find(rows_.front().version);
		if (view == nullptr || view->content.rhs != 2 ||
				view->content.coeffs != std::vector<Int>({1, 1, 1}))
			return false;
		if (simplifyNode().status != GPMC::SimpStatus::UNCHANGED) return false;
		backjumpTo(0);
		return simp_rows_.size() == 0;
	}

	bool ablationSkipsCard() {
		decide(-4);
		if (runPropagationWithoutLearnt(true)) return false;
		return simplifyNode().status == GPMC::SimpStatus::UNCHANGED &&
				simp_stats.card_calls == 0 && simp_rows_.size() == 0;
	}
};

GPMC::Configuration cardConfig(
		GPMC::CacheMode mode = GPMC::CacheMode::RESIDUAL,
		bool gcd = false) {
	GPMC::Configuration config;
	config.cntr.inprocess = true;
	config.cm.cacheMode = mode;
	config.cntr.ablation.insert(GPMC::SimpRule::BASIC);
	config.cntr.ablation.insert(GPMC::SimpRule::BICP);
	config.cntr.ablation.insert(GPMC::SimpRule::CS);
	config.cntr.ablation.insert(GPMC::SimpRule::SC);
	if (!gcd) config.cntr.ablation.insert(GPMC::SimpRule::GCD);
	return config;
}

int main() {
	const std::string simple =
			"* #variable= 4 #constraint= 1\n"
			"+3 x1 +3 x2 +2 x3 +1 x4 >= 5 ;\n";
	{
		GPMC::Configuration config = cardConfig(GPMC::CacheMode::BYPASS);
		CardTestCounter counter(config);
		counter.init();
		if (!counter.prepare(simple) || !counter.normalization(-4, 2, 3)) return 1;
	}
	{
		rs::options.propCard.parse("0");
		GPMC::Configuration config = cardConfig();
		CardTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 1\n"
				"+3 ~x1 +3 x2 +2 x3 +1 x4 >= 5 ;\n";
		if (!counter.prepare(formula) || !counter.normalization(-4, 2, 3)) return 2;
		rs::options.propCard.parse("1");
	}
	{
		GPMC::Configuration config = cardConfig();
		CardTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 1\n"
				"+3000000000000000000 x1 +3000000000000000000 x2 "
				"+2000000000000000000 x3 +1 x4 >= 5000000000000000000 ;\n";
		if (!counter.prepare(formula) || !counter.normalization(-4, 2, 3)) return 3;
	}
	{
		GPMC::Configuration config = cardConfig();
		CardTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 1\n"
				"+4 x1 +2 x2 +1 x3 +1 x4 >= 5 ;\n";
		if (!counter.prepare(formula) || !counter.nonCardinality()) return 4;
	}
	{
		GPMC::Configuration config = cardConfig();
		CardTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 1\n"
				"+3 x1 +3 x2 +2 x3 +1 x4 = 5 ;\n";
		if (!counter.prepare(formula) || !counter.equalityCommit()) return 5;
	}
	{
		GPMC::Configuration config = cardConfig();
		CardTestCounter counter(config);
		counter.init();
		counter.reservePrefix();
		const std::string formula =
				"* #variable= 5 #constraint= 1\n"
				"+4 x1 +3 x2 +3 x3 +2 x4 +1 x5 >= 9 ;\n";
		if (!counter.prepare(formula) || !counter.reasonAndGC()) return 6;
	}
	{
		GPMC::Configuration config = cardConfig(GPMC::CacheMode::RESIDUAL, true);
		CardTestCounter counter(config);
		counter.init();
		const std::string formula =
				"* #variable= 4 #constraint= 1\n"
				"+6 x1 +6 x2 +4 x3 +1 x4 >= 10 ;\n";
		if (!counter.prepare(formula) || !counter.gcdThenCard()) return 7;
	}
	{
		GPMC::Configuration config = cardConfig(GPMC::CacheMode::LEGACY);
		CardTestCounter counter(config);
		counter.init();
		if (!counter.prepare(simple) || !counter.normalization(-4, 2, 3)) return 8;
	}
	{
		GPMC::Configuration config = cardConfig();
		config.cntr.ablation.insert(GPMC::SimpRule::CARD);
		CardTestCounter counter(config);
		counter.init();
		if (!counter.prepare(simple) || !counter.ablationSkipsCard()) return 9;
	}
	return 0;
}
