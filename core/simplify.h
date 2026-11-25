#ifndef PBMC_SIMPLIFY_H
#define PBMC_SIMPLIFY_H

#include <cstdint>

namespace GPMC {

enum class SimpStatus : std::uint8_t {
	UNCHANGED,
	IMPLIED,
	ROW_CHANGED,
	ROW_REMOVED,
	CONFLICT,
	BUDGET
};

struct SimpResult {
	SimpStatus status = SimpStatus::UNCHANGED;
};

struct SimpStats {
	std::uint64_t calls = 0;
	std::uint64_t changed = 0;
	std::uint64_t conflicts = 0;
	std::uint64_t budgets = 0;
	double seconds = 0.0;
};

template <typename T_data>
class PBcounter;

template <typename T_data>
class Simplifier {
public:
	static SimpResult run(PBcounter<T_data>& counter);
};

} // namespace GPMC

#endif
