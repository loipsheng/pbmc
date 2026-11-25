#include "core/pbconstraint.h"

#include <cassert>
#include <vector>

namespace {

RowInfo row(SourceRowID source, EqualityGroupID group, RowSide side,
		RowVersionID version, RowState state, uint64_t solver_id) {
	RowInfo result;
	result.source = source;
	result.group = group;
	result.side = side;
	result.version = version;
	result.state = state;
	result.solver_id = solver_id;
	return result;
}

} // namespace

int main() {
	EqGroup group{1, 7, 11, 12, 0};
	std::vector<RowInfo> rows{
		row(7, 1, RowSide::LOWER, 11, RowState::ATTACHED, 101),
		row(7, 1, RowSide::UPPER, 12, RowState::ROOT_TRUE, NO_SOLVER_ID),
	};
	assert(validGroup(group, rows));

	auto broken = rows;
	broken.pop_back();
	assert(!validGroup(group, broken));

	broken = rows;
	broken[1].side = RowSide::LOWER;
	assert(!validGroup(group, broken));

	broken = rows;
	broken[1].solver_id = 102;
	assert(!validGroup(group, broken));

	broken = rows;
	broken[1].state = RowState::ATTACHED;
	broken[1].solver_id = 101;
	assert(!validGroup(group, broken));

	broken = rows;
	broken.push_back(row(7, 1, RowSide::SINGLE, 13,
			RowState::ROOT_TRUE, NO_SOLVER_ID));
	assert(!validGroup(group, broken));

	return 0;
}
