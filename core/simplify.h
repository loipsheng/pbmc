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
	std::uint64_t basic_calls = 0;
	std::uint64_t basic_rows = 0;
	std::uint64_t basic_open = 0;
	std::uint64_t basic_satisfied = 0;
	std::uint64_t bicp_calls = 0;
	std::uint64_t bicp_candidates = 0;
	std::uint64_t bicp_probes = 0;
	std::uint64_t bicp_complete = 0;
	std::uint64_t bicp_conflicts = 0;
	std::uint64_t bicp_unknown = 0;
	std::uint64_t bicp_failed = 0;
	std::uint64_t bicp_common = 0;
	std::uint64_t bicp_unsat = 0;
	std::uint64_t bicp_implied = 0;
	std::uint64_t bicp_work = 0;
	double seconds = 0.0;
};

template <typename T_data>
class PBcounter;

template <typename T_data>
class Simplifier {
public:
	static SimpResult run(PBcounter<T_data>& counter);

private:
	static SimpResult basic(PBcounter<T_data>& counter);
	static SimpResult bicp(PBcounter<T_data>& counter);
};

} // namespace GPMC

#endif
