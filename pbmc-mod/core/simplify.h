#ifndef PBMC_SIMPLIFY_H
#define PBMC_SIMPLIFY_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/pbconstraint.h"

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

struct SimpRow {
	RowVersionID row = NO_ROW_VERSION;
	EqualityGroupID group = NO_EQUALITY_GROUP;
	RowSide side = RowSide::SINGLE;
	PBconstraint content;
	bool active = true;
	std::uint64_t revision = 0;
	std::uint64_t transaction = 0;
	int level = 0;
};

struct SimpUndo {
	RowVersionID row = NO_ROW_VERSION;
	bool existed = false;
	SimpRow previous;
	std::uint64_t transaction = 0;
	int level = 0;
};

class SimpRows {
	std::unordered_map<RowVersionID, SimpRow> rows_;
	std::vector<SimpUndo> undo_;
	std::uint64_t next_revision_ = 1;
	std::uint64_t next_transaction_ = 1;
	std::uint64_t changes_ = 0;
	std::uint64_t undos_ = 0;

	static bool same(const SimpRow& row, const PBconstraint& content, bool active);
	static void checkContent(const PBconstraint& content);
	void setOne(RowVersionID row, EqualityGroupID group, RowSide side,
			const PBconstraint& content, bool active, int level,
			std::uint64_t transaction);

public:
	void clear();
	bool set(const RowInfo& row, const PBconstraint& content, bool active, int level);
	bool setGroup(const EqGroup& group,
			const PBconstraint& lower, bool lower_active,
			const PBconstraint& upper, bool upper_active, int level);
	void undo(int level);
	const SimpRow* find(RowVersionID row) const;
	void validate(int level) const;

	std::size_t size() const { return rows_.size(); }
	std::size_t undoSize() const { return undo_.size(); }
	std::uint64_t nextRevision() const { return next_revision_; }
	std::uint64_t nextTransaction() const { return next_transaction_; }
	std::uint64_t changes() const { return changes_; }
	std::uint64_t undos() const { return undos_; }
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
	std::uint64_t cs_calls = 0;
	std::uint64_t cs_rows = 0;
	std::uint64_t cs_equality_rows = 0;
	std::uint64_t cs_changed_rows = 0;
	std::uint64_t cs_coefficients = 0;
	std::uint64_t cs_commits = 0;
	std::uint64_t gcd_calls = 0;
	std::uint64_t gcd_rows = 0;
	std::uint64_t gcd_equality_rows = 0;
	std::uint64_t gcd_changed_rows = 0;
	std::uint64_t gcd_commits = 0;
	std::uint64_t card_calls = 0;
	std::uint64_t card_rows = 0;
	std::uint64_t card_equality_rows = 0;
	std::uint64_t card_changed_rows = 0;
	std::uint64_t card_commits = 0;
	std::uint64_t sc_calls = 0;
	std::uint64_t sc_rows = 0;
	std::uint64_t sc_pairs = 0;
	std::uint64_t sc_lambdas = 0;
	std::uint64_t sc_proposals = 0;
	std::uint64_t sc_coefficients = 0;
	std::uint64_t sc_revalidations = 0;
	std::uint64_t sc_changed_rows = 0;
	std::uint64_t sc_commits = 0;
	std::uint64_t sc_work = 0;
	std::uint64_t sc_budgets = 0;
	double seconds = 0.0;
};

template <typename T_data>
class PBcounter;

template <typename T_data>
class Simplifier {
	struct CurrentRow {
		PBconstraint content;
		bool active = false;
		bool open = false;
	};

public:
	static SimpResult run(PBcounter<T_data>& counter);

private:
	static CurrentRow current(
			PBcounter<T_data>& counter, const RowInfo& row, bool overlay = true);
	static const RowInfo& findRow(
			PBcounter<T_data>& counter, RowVersionID version);
	static const EqGroup& findGroup(
			PBcounter<T_data>& counter, EqualityGroupID group);
	static SimpResult basic(PBcounter<T_data>& counter);
	static SimpResult bicp(PBcounter<T_data>& counter);
	static SimpResult cs(PBcounter<T_data>& counter);
	static SimpResult gcd(PBcounter<T_data>& counter);
	static SimpResult card(PBcounter<T_data>& counter);
	static SimpResult sc(PBcounter<T_data>& counter);
};

} // namespace GPMC

#endif
