#include "core/simplify.h"

int main() {
	const GPMC::SimpResult result;
	const GPMC::SimpStats stats;
	if(result.status != GPMC::SimpStatus::UNCHANGED) return 1;
	if(stats.calls != 0 || stats.changed != 0 || stats.conflicts != 0) return 2;
	if(stats.budgets != 0 || stats.seconds != 0.0) return 3;
	return 0;
}
