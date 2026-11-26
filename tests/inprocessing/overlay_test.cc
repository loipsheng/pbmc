#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>

class OverlayTestCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit OverlayTestCounter(GPMC::Configuration& config) : PBcounter(config) {}

	bool prepare() {
		std::istringstream input(
				"* #variable= 4 #constraint= 2\n"
				"+1 x1 +1 x2 = 1 ;\n"
				"+1 x3 +1 x4 >= 1 ;\n");
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
		return rows_.size() == 3 && groups_.size() == 1;
	}

	bool singleLifecycle() {
		const RowInfo* info = nullptr;
		for (const RowInfo& row : rows_)
			if (row.group == NO_EQUALITY_GROUP) info = &row;
		if (info == nullptr) return false;
		const RowVersionID row = info->version;
		PBconstraint root = info->content;
		root.rhs = 1;
		if (!simp_rows_.set(*info, root, true, 0)) return false;
		const GPMC::SimpRow* current = simp_rows_.find(row);
		if (current == nullptr || current->level != 0 || current->revision != 1 ||
				current->group != NO_EQUALITY_GROUP ||
				current->side != RowSide::SINGLE || current->transaction != 0)
			return false;
		if (simp_rows_.set(*info, root, true, 0)) return false;

		const std::vector<rs::Lit> trail_before = trail;
		const std::vector<rs::CRef> reasons_before = Reason;
		const uint32_t allocator_before = ca.at;
		const rs::CRef cref_before = solverRef(row);

		decide(3);
		PBconstraint first = root;
		first.coeffs.front() = 2;
		if (!simp_rows_.set(*info, first, true, decisionLevel())) return false;
		const std::uint64_t first_revision = simp_rows_.find(row)->revision;

		decide(4);
		PBconstraint deep = first;
		deep.rhs = 2;
		if (!simp_rows_.set(*info, deep, false, decisionLevel())) return false;
		const std::uint64_t deep_revision = simp_rows_.find(row)->revision;
		if (deep_revision <= first_revision) return false;

		backjumpTo(1);
		current = simp_rows_.find(row);
		if (current == nullptr || current->revision != first_revision ||
				!current->active || current->content.coeffs.front() != 2)
			return false;

		backjumpTo(0);
		current = simp_rows_.find(row);
		if (current == nullptr || current->revision != 1 ||
				current->content.coeffs.front() != root.coeffs.front())
			return false;

		decide(-3);
		PBconstraint second = root;
		second.coeffs.back() = 3;
		if (!simp_rows_.set(*info, second, true, decisionLevel())) return false;
		const std::uint64_t second_revision = simp_rows_.find(row)->revision;
		if (second_revision <= deep_revision) return false;
		backjumpTo(0);

		current = simp_rows_.find(row);
		if (current == nullptr || current->revision != 1 ||
				simp_rows_.size() != 1 || simp_rows_.undoSize() != 1 ||
				simp_rows_.changes() != 4 || simp_rows_.undos() != 3 ||
				simp_rows_.nextRevision() != 5)
			return false;
		simp_rows_.validate(0);

		if (trail != trail_before || Reason != reasons_before ||
				ca.at != allocator_before || solverRef(row) != cref_before)
			return false;
		return true;
	}

	bool groupLifecycle() {
		const EqGroup& group = groups_.front();
		const RowInfo* lower_info = nullptr;
		const RowInfo* upper_info = nullptr;
		for (const RowInfo& row : rows_) {
			if (row.version == group.lower) lower_info = &row;
			if (row.version == group.upper) upper_info = &row;
		}
		if (lower_info == nullptr || upper_info == nullptr) return false;

		bool rejected_half = false;
		try {
			simp_rows_.set(*lower_info, lower_info->content, true, 0);
		} catch (const std::invalid_argument&) {
			rejected_half = true;
		}
		if (!rejected_half) return false;

		PBconstraint lower = lower_info->content;
		PBconstraint upper = upper_info->content;
		if (!simp_rows_.setGroup(group, lower, true, upper, true, 0)) return false;
		const GPMC::SimpRow* low = simp_rows_.find(group.lower);
		const GPMC::SimpRow* high = simp_rows_.find(group.upper);
		if (low == nullptr || high == nullptr || low->group != group.id ||
				high->group != group.id || low->side != RowSide::LOWER ||
				high->side != RowSide::UPPER || low->transaction == 0 ||
				low->transaction != high->transaction)
			return false;
		const std::uint64_t root_transaction = low->transaction;
		const std::uint64_t root_low_revision = low->revision;
		const std::uint64_t root_high_revision = high->revision;
		if (simp_rows_.setGroup(group, lower, true, upper, true, 0)) return false;

		decide(3);
		lower.coeffs.front()++;
		if (!simp_rows_.setGroup(
				group, lower, true, upper, false, decisionLevel()))
			return false;
		low = simp_rows_.find(group.lower);
		high = simp_rows_.find(group.upper);
		if (low == nullptr || high == nullptr ||
				low->transaction == root_transaction ||
				low->transaction != high->transaction || high->active ||
				low->revision <= root_low_revision ||
				high->revision <= root_high_revision)
			return false;
		simp_rows_.validate(decisionLevel());

		backjumpTo(0);
		low = simp_rows_.find(group.lower);
		high = simp_rows_.find(group.upper);
		if (low == nullptr || high == nullptr ||
				low->transaction != root_transaction ||
				high->transaction != root_transaction ||
				low->revision != root_low_revision ||
				high->revision != root_high_revision || !high->active)
			return false;
		if (simp_rows_.nextTransaction() != 3) return false;
		simp_rows_.validate(0);
		return true;
	}
};

int main() {
	GPMC::Configuration config;
	config.cntr.inprocess = true;
	config.cm.cacheMode = GPMC::CacheMode::BYPASS;
	OverlayTestCounter counter(config);
	counter.init();
	return counter.prepare() && counter.singleLifecycle() &&
			counter.groupLifecycle() ? 0 : 1;
}
