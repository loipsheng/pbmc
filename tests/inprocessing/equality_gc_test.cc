#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <utility>
#include <vector>

class TestCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit TestCounter(GPMC::Configuration& config) : PBcounter(config) {}

	void reservePrefix() {
		const uint32_t padding = static_cast<uint32_t>(
				rs::ConstraintAllocator::alignedWordSize(17));
		ca.capacity(padding);
		ca.at = padding;
	}

	bool compactAndCheck() {
		if (groups_.size() != 1 || !validGroup(groups_.front(), rows_)) return false;
		std::vector<std::pair<RowVersionID, rs::CRef>> before;
		for (const RowInfo& row : rows_) {
			if (row.state == RowState::ATTACHED)
				before.emplace_back(row.version, solverRef(row.version));
		}
		if (before.size() != 2) return false;

		garbage_collect();
		validateRows(false);
		bool moved = false;
		for (const auto& old : before) {
			const rs::CRef current = solverRef(old.first);
			if (current != old.second) moved = true;
		}
		return moved;
	}
};

int main() {
	GPMC::Configuration config;
	TestCounter counter(config);
	counter.init();
	counter.reservePrefix();

	std::istringstream input(
			"* #variable= 3 #constraint= 1\n"
			"+1 x1 +1 x2 +1 x3 = 1 ;\n");
	counter.load(input);
	counter.pbins.renumberVar();
	counter.pbins.makeRenumberedCoeffPositive();
	counter.pbins.simplifyRenumberedConstraint();
	if (counter.pbins.inferredUnsat) return 1;
	counter.pbins.computeVarCoeffScore(counter.config.coef_tdscore, false);
	counter.setNbVars(counter.pbins.vars, true);
	counter.copyPBInsNPVar();
	counter.copyPBInsPhase();
	if (!counter.addRows()) return 1;
	return counter.compactAndCheck() ? 0 : 1;
}
