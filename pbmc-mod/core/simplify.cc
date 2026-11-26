#include <gmpxx.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "mpfr/mpreal.h"
#include "core/Counter.h"

#ifdef PBMC_USE_GLPK
#include <glpk.h>
#endif

namespace GPMC {

bool SimpRows::same(
		const SimpRow& row, const PBconstraint& content, bool active) {
	return row.active == active && row.content.lits == content.lits &&
			row.content.coeffs == content.coeffs && row.content.rhs == content.rhs &&
			row.content.equals == content.equals &&
			row.content.constraintId == content.constraintId;
}

void SimpRows::checkContent(const PBconstraint& content) {
	if (content.lits.size() != content.coeffs.size() || content.equals)
		throw std::invalid_argument("invalid simplification row content");
	for (Int coefficient : content.coeffs)
		if (coefficient <= 0)
			throw std::invalid_argument(
					"simplification row has a non-positive coefficient");
}

void SimpRows::clear() {
	rows_.clear();
	undo_.clear();
	next_revision_ = 1;
	next_transaction_ = 1;
	changes_ = 0;
	undos_ = 0;
}

void SimpRows::setOne(RowVersionID row, EqualityGroupID group, RowSide side,
		const PBconstraint& content, bool active, int level,
		std::uint64_t transaction) {
	auto current = rows_.find(row);
	if (current != rows_.end() &&
			(current->second.group != group || current->second.side != side))
		throw std::logic_error("simplification row changed equality identity");

	SimpUndo change;
	change.row = row;
	change.level = level;
	change.transaction = transaction;
	change.existed = current != rows_.end();
	if (change.existed) change.previous = current->second;
	undo_.emplace_back(std::move(change));

	SimpRow replacement;
	replacement.row = row;
	replacement.group = group;
	replacement.side = side;
	replacement.content = content;
	replacement.active = active;
	replacement.revision = next_revision_++;
	replacement.transaction = transaction;
	replacement.level = level;
	rows_[row] = std::move(replacement);
	changes_++;
}

bool SimpRows::set(const RowInfo& row, const PBconstraint& content,
		bool active, int level) {
	if (row.version == NO_ROW_VERSION || level < 0)
		throw std::invalid_argument("invalid simplification row update");
	if (row.group != NO_EQUALITY_GROUP || row.side != RowSide::SINGLE)
		throw std::invalid_argument(
				"an equality row requires an atomic group update");
	checkContent(content);
	if (!undo_.empty() && undo_.back().level > level)
		throw std::logic_error("simplification row update escaped its decision scope");

	auto current = rows_.find(row.version);
	if (current != rows_.end() && same(current->second, content, active))
		return false;
	setOne(row.version, NO_EQUALITY_GROUP, RowSide::SINGLE,
			content, active, level, 0);
	return true;
}

bool SimpRows::setGroup(const EqGroup& group,
		const PBconstraint& lower, bool lower_active,
		const PBconstraint& upper, bool upper_active, int level) {
	if (group.id == NO_EQUALITY_GROUP || group.lower == NO_ROW_VERSION ||
			group.upper == NO_ROW_VERSION || group.lower == group.upper || level < 0)
		throw std::invalid_argument("invalid simplification equality update");
	checkContent(lower);
	checkContent(upper);
	if (!undo_.empty() && undo_.back().level > level)
		throw std::logic_error("simplification equality update escaped its decision scope");

	auto low = rows_.find(group.lower);
	auto high = rows_.find(group.upper);
	if ((low == rows_.end()) != (high == rows_.end()))
		throw std::logic_error("half-published simplification equality group");
	if (low != rows_.end()) {
		if (low->second.group != group.id || low->second.side != RowSide::LOWER ||
				high->second.group != group.id || high->second.side != RowSide::UPPER ||
				low->second.transaction == 0 ||
				low->second.transaction != high->second.transaction)
			throw std::logic_error("invalid simplification equality identity");
		if (same(low->second, lower, lower_active) &&
				same(high->second, upper, upper_active))
			return false;
	}

	const std::uint64_t transaction = next_transaction_++;
	undo_.reserve(undo_.size() + 2);
	rows_.reserve(rows_.size() + 2);
	setOne(group.lower, group.id, RowSide::LOWER,
			lower, lower_active, level, transaction);
	setOne(group.upper, group.id, RowSide::UPPER,
			upper, upper_active, level, transaction);
	return true;
}

void SimpRows::undo(int level) {
	if (level < 0)
		throw std::invalid_argument("invalid simplification undo level");
	while (!undo_.empty() && undo_.back().level > level) {
		const std::uint64_t transaction = undo_.back().transaction;
		const int count = transaction == 0 ? 1 : 2;
		for (int i = 0; i < count; ++i) {
			if (undo_.empty() || undo_.back().level <= level ||
					undo_.back().transaction != transaction)
				throw std::logic_error(
						"split simplification equality transaction on undo");
			const SimpUndo change = undo_.back();
			if (change.existed)
				rows_[change.row] = change.previous;
			else
				rows_.erase(change.row);
			undo_.pop_back();
			undos_++;
		}
	}
}

const SimpRow* SimpRows::find(RowVersionID row) const {
	auto found = rows_.find(row);
	return found == rows_.end() ? nullptr : &found->second;
}

void SimpRows::validate(int level) const {
	if (level < 0)
		throw std::logic_error("invalid simplification validation level");
	int previous_level = 0;
	std::unordered_map<std::uint64_t, int> undo_transactions;
	for (const SimpUndo& change : undo_) {
		if (change.row == NO_ROW_VERSION || change.level < previous_level ||
				change.level > level)
			throw std::logic_error("invalid simplification undo stack");
		if (change.transaction != 0) undo_transactions[change.transaction]++;
		previous_level = change.level;
	}
	for (const auto& entry : undo_transactions)
		if (entry.second != 2)
			throw std::logic_error("split simplification equality undo transaction");

	struct GroupState {
		const SimpRow* lower = nullptr;
		const SimpRow* upper = nullptr;
	};
	std::unordered_map<EqualityGroupID, GroupState> groups;
	for (const auto& entry : rows_) {
		const SimpRow& row = entry.second;
		if (entry.first != row.row || row.row == NO_ROW_VERSION ||
				row.revision == 0 || row.revision >= next_revision_ ||
				row.level < 0 || row.level > level || row.content.equals ||
				row.content.lits.size() != row.content.coeffs.size())
			throw std::logic_error("invalid active simplification row");
		if (row.group == NO_EQUALITY_GROUP) {
			if (row.side != RowSide::SINGLE || row.transaction != 0)
				throw std::logic_error("invalid standalone simplification row identity");
		} else {
			if (row.side == RowSide::SINGLE || row.transaction == 0 ||
					row.transaction >= next_transaction_)
				throw std::logic_error("invalid equality simplification row identity");
			GroupState& group = groups[row.group];
			const SimpRow*& side = row.side == RowSide::LOWER
					? group.lower : group.upper;
			if (side != nullptr)
				throw std::logic_error("duplicate equality simplification side");
			side = &row;
		}
		for (Int coefficient : row.content.coeffs)
			if (coefficient <= 0)
				throw std::logic_error("invalid active simplification coefficient");
	}
	for (const auto& entry : groups) {
		const GroupState& group = entry.second;
		if (group.lower == nullptr || group.upper == nullptr ||
				group.lower->transaction != group.upper->transaction ||
				group.lower->level != group.upper->level)
			throw std::logic_error("half-published simplification equality group");
	}
}

template <typename T_data>
void PBcounter<T_data>::backjumpTo(int level) {
	if (level < 0 || level > decisionLevel())
		throw std::invalid_argument("invalid PBMC backjump level");
	rs::Solver::backjumpTo(level);
	simp_rows_.undo(level);
#ifndef NDEBUG
	simp_rows_.validate(level);
#endif
}

template <typename T_data>
const RowInfo& Simplifier<T_data>::findRow(
		PBcounter<T_data>& counter, RowVersionID version) {
	const RowInfo* result = nullptr;
	for (const RowInfo& row : counter.rows_) {
		if (row.version != version) continue;
		if (result != nullptr)
			throw std::logic_error("duplicate simplification row version");
		result = &row;
	}
	if (result == nullptr)
		throw std::logic_error("unknown simplification row version");
	return *result;
}

template <typename T_data>
const EqGroup& Simplifier<T_data>::findGroup(
		PBcounter<T_data>& counter, EqualityGroupID id) {
	const EqGroup* result = nullptr;
	for (const EqGroup& group : counter.groups_) {
		if (group.id != id) continue;
		if (result != nullptr)
			throw std::logic_error("duplicate simplification equality group");
		result = &group;
	}
	if (result == nullptr)
		throw std::logic_error("unknown simplification equality group");
	return *result;
}

template <typename T_data>
void Simplifier<T_data>::deactivate(
		PBcounter<T_data>& counter, const RowInfo& row) {
	if (row.group == NO_EQUALITY_GROUP) {
		const CurrentRow staged = effective(counter, row);
		if (!staged.active || !counter.simp_rows_.set(
				row, staged.content, false, counter.decisionLevel()))
			throw std::logic_error("failed to deactivate a simplification row");
		return;
	}

	const EqGroup& group = findGroup(counter, row.group);
	const RowInfo& lower_info = findRow(counter, group.lower);
	const RowInfo& upper_info = findRow(counter, group.upper);
	CurrentRow lower = effective(counter, lower_info);
	CurrentRow upper = effective(counter, upper_info);
	if (row.side == RowSide::LOWER) {
		if (!lower.active) throw std::logic_error("inactive equality lower row");
		lower.active = false;
	} else if (row.side == RowSide::UPPER) {
		if (!upper.active) throw std::logic_error("inactive equality upper row");
		upper.active = false;
	} else {
		throw std::logic_error("invalid equality row side");
	}
	if (!counter.simp_rows_.setGroup(group,
			lower.content, lower.active, upper.content, upper.active,
			counter.decisionLevel()))
		throw std::logic_error("failed to publish an equality transaction");
}

template <typename T_data>
typename Simplifier<T_data>::CurrentRow Simplifier<T_data>::effective(
		PBcounter<T_data>& counter, const RowInfo& row, bool overlay) {
	CurrentRow result;
	const SimpRow* changed = overlay
			? counter.simp_rows_.find(row.version) : nullptr;
	if (changed != nullptr) {
		if (counter.cmpmgr.cache().legacy())
			throw std::logic_error("legacy cache cannot consume a residual overlay");
		if (changed->group != row.group || changed->side != row.side)
			throw std::logic_error("residual overlay changed the row identity");
		result.content = changed->content;
		result.active = changed->active;
	} else {
		result.content = row.state == RowState::ATTACHED
				? counter.contentOf(
						counter.ca[counter.solverRef(row.version)], row.source)
				: row.content;
		result.active = row.state == RowState::ATTACHED;
	}
	return result;
}

template <typename T_data>
typename Simplifier<T_data>::CurrentRow Simplifier<T_data>::current(
		PBcounter<T_data>& counter, const RowInfo& row, bool overlay) {
	CurrentRow result = effective(counter, row, overlay);
	if (!result.active) return result;

	rs::BigVal gap(result.content.rhs);
	rs::BigVal available = 0;
	rs::CeArb residual = counter.cePools.takeArb();
	for (size_t i = 0; i < result.content.lits.size(); ++i) {
		const rs::Lit literal = static_cast<rs::Lit>(result.content.lits[i]);
		const rs::BigCoef coefficient(result.content.coeffs[i]);
		if (rs::isTrue(counter.Level, literal)) {
			gap -= coefficient;
		} else if (!rs::isFalse(counter.Level, literal)) {
			available += coefficient;
			residual->addLhs(coefficient, literal);
		}
	}
	if (gap <= 0) return result;
	if (available < gap)
		throw std::logic_error(
				"simplification found a conflict missed by native propagation");
	residual->addRhs(gap);
	if (residual->getDegree() != gap)
		throw std::logic_error("simplification built a residual row with the wrong gap");
	result.content = counter.contentOf(residual, row.source);
	result.open = true;
	return result;
}

template <typename T_data>
SimpResult Simplifier<T_data>::basic(PBcounter<T_data>& counter) {
	if (counter.config.disabled(SimpRule::BASIC)) return {};
	if (counter.qhead != static_cast<int>(counter.trail.size()))
		throw std::logic_error("Basic requires a RoundingSat propagation fixpoint");

	counter.simp_stats.basic_calls++;
	Component& component = counter.cmpmgr.topComponent();
	std::unordered_set<ClID> current_rows;
	current_rows.reserve(component.nlongCls());
	for (int i = component.nVars() + 1;
			i < component.nVars() + component.nlongCls() + 1; ++i)
		current_rows.insert(component[i]);

	for (const RowInfo& row : counter.rows_) {
		if (row.state != RowState::ATTACHED) continue;
		const ComponentBind& bind = counter.cmpmgr.rowBind(row.version);
		if (!current_rows.count(bind.clid)) continue;

		counter.simp_stats.basic_rows++;
		const rs::Constr& constraint = counter.ca[counter.solverRef(row.version)];
		rs::BigVal true_sum = 0;
		rs::BigVal unknown_sum = 0;
		std::vector<rs::BigCoef> unknown_coeffs;
		unknown_coeffs.reserve(constraint.size());
		for (size_t i = 0; i < constraint.size(); ++i) {
			const rs::BigCoef coefficient = constraint.coef(i);
			const rs::Lit literal = constraint.lit(i);
			if (coefficient <= 0)
				throw std::logic_error("Basic found a non-positive canonical coefficient");
			if (rs::isTrue(counter.Level, literal)) {
				true_sum += coefficient;
			} else if (!rs::isFalse(counter.Level, literal)) {
				unknown_sum += coefficient;
				unknown_coeffs.push_back(coefficient);
			}
		}

		const rs::BigVal gap = constraint.degree() - true_sum;
		if (gap <= 0) {
			counter.simp_stats.basic_satisfied++;
			continue;
		}
		if (unknown_sum < gap)
			throw std::logic_error(
					"Basic found a conflict missed by native propagation");

		const rs::BigVal slack = unknown_sum - gap;
		for (const rs::BigCoef& coefficient : unknown_coeffs) {
			if (coefficient > slack)
				throw std::logic_error(
						"Basic found an implication missed by native propagation");
		}
		counter.simp_stats.basic_open++;
	}

	return {};
}

template <typename T_data>
SimpResult Simplifier<T_data>::imply(
		PBcounter<T_data>& counter, rs::Lit literal) {
	if (literal == 0 || counter.Pos[rs::toVar(literal)] != rs::INF)
		throw std::logic_error(
				"simplification tried to submit a non-new implication");

	rs::CeArb reason = counter.cePools.takeArb();
	for (int position : counter.trail_lim)
		reason->addLhs(rs::BigCoef(1), -counter.trail[position]);
	reason->addLhs(rs::BigCoef(1), literal);
	reason->addRhs(rs::BigCoef(1));
	reason->orig = rs::Origin::LEARNED;
	reason->postProcess(counter.Level, counter.Pos, true, rs::stats);
	if (reason->isTautology() || reason->isInconsistency())
		throw std::logic_error("simplification produced an invalid guarded reason");

	const rs::CRef cref = counter.attachConstraint(reason, false);
	rs::Constr& attached = counter.ca[cref];
	attached.setLBD(attached.size());
	if (!rs::isTrue(counter.Level, literal))
		throw std::logic_error(
				"simplification reason did not propagate its implication");
	if (counter.decisionLevel() > 0 &&
			counter.Reason[rs::toVar(literal)] != cref)
		throw std::logic_error(
				"simplification implication has no live RoundingSat Reason");
	return {SimpStatus::IMPLIED};
}

template <typename T_data>
SimpResult Simplifier<T_data>::bicp(PBcounter<T_data>& counter) {
	constexpr std::uint64_t max_candidates = 32;
	constexpr std::uint64_t max_probe_work = 50000;
	constexpr std::uint64_t max_node_work = 200000;
	enum class ProbeState : std::uint8_t { COMPLETE, CONFLICT, UNKNOWN };
	struct ProbeResult {
		ProbeState state = ProbeState::UNKNOWN;
		std::vector<rs::Lit> implied;
		std::uint64_t work = 0;
	};

	if (counter.config.disabled(SimpRule::BICP)) return {};
	if (counter.qhead != static_cast<int>(counter.trail.size()))
		throw std::logic_error("BICP requires a RoundingSat propagation fixpoint");

	counter.simp_stats.bicp_calls++;
	Component& component = counter.cmpmgr.topComponent();
	std::vector<rs::Var> candidates;
	std::unordered_set<rs::Var> component_vars;
	candidates.reserve(component.nVars());
	component_vars.reserve(component.nVars());
	for (int i = 0; i < component.nVars(); ++i) {
		const rs::Var variable = component[i] + 1;
		component_vars.insert(variable);
		if (counter.Pos[variable] == rs::INF) candidates.push_back(variable);
	}
	const bool candidate_limited = candidates.size() > max_candidates;
	if (candidate_limited) candidates.resize(max_candidates);

	std::uint64_t node_work = 0;
	bool budget_limited = candidate_limited;
	for (rs::Var variable : candidates) {
		if (node_work >= max_node_work) {
			budget_limited = true;
			break;
		}
		counter.simp_stats.bicp_candidates++;

		auto probe = [&](rs::Lit assumption) {
			ProbeResult result;
			const int base_level = counter.decisionLevel();
			const int base_qhead = counter.qhead;
			const std::vector<rs::Lit> trail_before = counter.trail;
			const std::vector<int> limits_before = counter.trail_lim;
			const std::vector<int> levels_before = counter._Level;
			const std::vector<int> pos_before = counter.Pos;
			const std::vector<rs::CRef> reasons_before = counter.Reason;
			const std::vector<rs::Lit> phase_before = counter.phase;
			const std::vector<rs::ActValV> activity_before = counter.activity;
			const size_t constraints_before = counter.constraints.size();
			const size_t external_before = counter.external.size();
			const size_t learned_before = counter.learnedStack.size();
			const uint32_t allocator_at = counter.ca.at;
			const uint32_t allocator_wasted = counter.ca.wasted;
			const rs::ActValC activity_inc = counter.c_vsids_inc;
			const rs::Stats stats_before = rs::stats;

			struct ConstraintState {
				rs::CRef cref;
				rs::ActValC activity;
				unsigned lbd;
			};
			std::vector<ConstraintState> constraint_state;
			constraint_state.reserve(counter.constraints.size());
			for (rs::CRef cref : counter.constraints) {
				const rs::Constr& constraint = counter.ca[cref];
				constraint_state.push_back({cref, constraint.act, constraint.lbd()});
			}

			bool conflict = false;
			try {
				counter.decide(assumption);
				counter.simp_stats.bicp_probes++;
				conflict = static_cast<bool>(counter.runPropagationWithoutLearnt(true));
				if (!conflict) {
					for (size_t i = trail_before.size() + 1;
							i < counter.trail.size(); ++i)
						result.implied.push_back(counter.trail[i]);
				}
			} catch (...) {
				counter.backjumpTo(base_level);
				counter.Reason = reasons_before;
				counter.phase = phase_before;
				counter.activity = activity_before;
				counter.c_vsids_inc = activity_inc;
				for (const ConstraintState& saved : constraint_state) {
					counter.ca[saved.cref].act = saved.activity;
					counter.ca[saved.cref].setLBD(saved.lbd);
				}
				throw;
			}

			counter.backjumpTo(base_level);
			const long long elapsed = rs::stats.getDetTime() - stats_before.getDetTime();
			result.work = static_cast<std::uint64_t>(std::max(1LL, elapsed));
			counter.Reason = reasons_before;
			counter.phase = phase_before;
			counter.activity = activity_before;
			counter.c_vsids_inc = activity_inc;
			for (const ConstraintState& saved : constraint_state) {
				counter.ca[saved.cref].act = saved.activity;
				counter.ca[saved.cref].setLBD(saved.lbd);
			}
			// NTRAILPOPS is a monotone propagation generation used by watched
			// constraints to invalidate their internal scan index.  Probe work is
			// therefore retained in RoundingSat's counters instead of rewinding it.

			if (counter.decisionLevel() != base_level || counter.qhead != base_qhead ||
					counter.trail != trail_before || counter.trail_lim != limits_before ||
					counter._Level != levels_before || counter.Pos != pos_before ||
					counter.Reason != reasons_before || counter.phase != phase_before ||
					counter.activity != activity_before ||
					counter.constraints.size() != constraints_before ||
					counter.external.size() != external_before ||
					counter.learnedStack.size() != learned_before ||
					counter.ca.at != allocator_at || counter.ca.wasted != allocator_wasted)
				throw std::logic_error("BICP failed to restore the RoundingSat probe state");

			if (result.work > max_probe_work ||
					result.work > max_node_work - std::min(node_work, max_node_work)) {
				result.state = ProbeState::UNKNOWN;
				result.implied.clear();
			} else {
				result.state = conflict ? ProbeState::CONFLICT : ProbeState::COMPLETE;
			}
			return result;
		};

		ProbeResult negative = probe(-variable);
		node_work += negative.work;
		counter.simp_stats.bicp_work += negative.work;
		if (negative.state == ProbeState::COMPLETE)
			counter.simp_stats.bicp_complete++;
		else if (negative.state == ProbeState::CONFLICT)
			counter.simp_stats.bicp_conflicts++;
		else {
			counter.simp_stats.bicp_unknown++;
			budget_limited = true;
		}

		ProbeResult positive;
		if (node_work < max_node_work) {
			positive = probe(variable);
			node_work += positive.work;
			counter.simp_stats.bicp_work += positive.work;
			if (positive.state == ProbeState::COMPLETE)
				counter.simp_stats.bicp_complete++;
			else if (positive.state == ProbeState::CONFLICT)
				counter.simp_stats.bicp_conflicts++;
			else {
				counter.simp_stats.bicp_unknown++;
				budget_limited = true;
			}
		} else {
			budget_limited = true;
		}

		const bool negative_conflict = negative.state == ProbeState::CONFLICT;
		const bool positive_conflict = positive.state == ProbeState::CONFLICT;
		rs::Lit implied = 0;
		if (negative_conflict && positive_conflict) {
			counter.simp_stats.bicp_unsat++;
			implied = variable;
		} else if (negative_conflict || positive_conflict) {
			counter.simp_stats.bicp_failed++;
			implied = negative_conflict ? variable : -variable;
		}
		if (negative.state == ProbeState::COMPLETE &&
				positive.state == ProbeState::COMPLETE) {
			std::unordered_set<rs::Lit> left(
					negative.implied.begin(), negative.implied.end());
			for (rs::Lit literal : positive.implied) {
				if (!left.count(literal)) continue;
				counter.simp_stats.bicp_common++;
				if (implied == 0 && component_vars.count(rs::toVar(literal)) &&
						counter.Pos[rs::toVar(literal)] == rs::INF)
					implied = literal;
			}
		}
		if (implied != 0) {
			counter.simp_stats.bicp_implied++;
			return imply(counter, implied);
		}

		if (budget_limited && node_work >= max_node_work) break;
	}
	return {budget_limited ? SimpStatus::BUDGET : SimpStatus::UNCHANGED};
}

template <typename T_data>
SimpResult Simplifier<T_data>::sblp(PBcounter<T_data>& counter) {
	if (counter.config.disabled(SimpRule::SBLP)) return {};
#ifndef PBMC_USE_GLPK
	return {};
#else
	constexpr std::uint64_t max_candidates = 16;
	constexpr std::uint64_t max_mip_calls = 32;
	constexpr std::uint64_t max_input_work = 50000;
	constexpr std::uint64_t max_exact_work = 200000;
	constexpr size_t max_exact_variables = 64;
	constexpr double exact_seconds = 0.05;
	constexpr double integer_tolerance = 1e-7;
	const rs::BigVal exact_limit("9007199254740992");

	if (counter.qhead != static_cast<int>(counter.trail.size()))
		throw std::logic_error("SB-LP requires a RoundingSat propagation fixpoint");
	counter.simp_stats.sblp_calls++;
	struct Clock {
		SimpStats& stats;
		double start = Glucose::cpuTime();
		~Clock() { stats.sblp_seconds += Glucose::cpuTime() - start; }
	} clock{counter.simp_stats};

	Component& component = counter.cmpmgr.topComponent();
	std::unordered_set<ClID> current_rows;
	current_rows.reserve(component.nlongCls());
	for (int i = component.nVars() + 1;
			i < component.nVars() + component.nlongCls() + 1; ++i)
		current_rows.insert(component[i]);

	std::vector<rs::Var> variables;
	std::unordered_map<rs::Var, int> columns;
	variables.reserve(component.nVars());
	for (int i = 0; i < component.nVars(); ++i) {
		const rs::Var variable = component[i] + 1;
		if (counter.Pos[variable] != rs::INF) continue;
		columns.emplace(variable, static_cast<int>(variables.size()) + 1);
		variables.push_back(variable);
	}
	counter.simp_stats.sblp_variables += variables.size();
	if (variables.empty()) return {};

	std::vector<PBconstraint> residual_rows;
	std::vector<PBconstraint> proof_rows;
	std::uint64_t input_work = variables.size() + counter.trail.size();
	for (const RowInfo& info : counter.rows_) {
		if (info.state != RowState::ATTACHED) continue;
		const ComponentBind& bind = counter.cmpmgr.rowBind(info.version);
		if (!current_rows.count(bind.clid)) continue;

		const CurrentRow mother = effective(counter, info, false);
		if (mother.active) {
			proof_rows.push_back(mother.content);
			input_work += mother.content.lits.size();
		}
		const CurrentRow residual = current(counter, info);
		if (!residual.active || !residual.open) continue;
		residual_rows.push_back(residual.content);
		input_work += residual.content.lits.size();
	}
	counter.simp_stats.sblp_rows += residual_rows.size();
	if (residual_rows.empty()) return {};
	if (input_work > max_input_work) {
		counter.simp_stats.sblp_budgets++;
		return {SimpStatus::BUDGET};
	}

	struct ModelRow {
		std::map<int, rs::BigVal> terms;
		rs::BigVal degree = 0;
	};
	std::vector<ModelRow> model;
	model.reserve(residual_rows.size());
	std::uint64_t nonzeros = 0;
	auto in_exact_range = [&](rs::BigVal value) {
		if (value < 0) value = -value;
		return value <= exact_limit;
	};
	bool numeric_rejected = false;
	for (const PBconstraint& row : residual_rows) {
		ModelRow converted;
		converted.degree = row.rhs;
		for (size_t i = 0; i < row.lits.size(); ++i) {
			const rs::Lit literal = static_cast<rs::Lit>(row.lits[i]);
			const auto found = columns.find(rs::toVar(literal));
			if (found == columns.end())
				throw std::logic_error("SB-LP residual row escaped its component");
			const rs::BigVal coefficient(row.coeffs[i]);
			if (!in_exact_range(coefficient)) {
				numeric_rejected = true;
				break;
			}
			if (literal > 0) {
				converted.terms[found->second] += coefficient;
			} else {
				converted.terms[found->second] -= coefficient;
				converted.degree -= coefficient;
			}
		}
		if (numeric_rejected) break;
		rs::BigVal maximum = 0;
		for (auto it = converted.terms.begin(); it != converted.terms.end();) {
			if (it->second == 0) {
				it = converted.terms.erase(it);
				continue;
			}
			rs::BigVal magnitude = it->second;
			if (magnitude < 0) magnitude = -magnitude;
			maximum += magnitude;
			if (!in_exact_range(it->second) || !in_exact_range(maximum)) {
				numeric_rejected = true;
				break;
			}
			++it;
		}
		if (numeric_rejected || !in_exact_range(converted.degree)) {
			numeric_rejected = true;
			break;
		}
		nonzeros += converted.terms.size();
		model.push_back(std::move(converted));
	}
	if (numeric_rejected) {
		counter.simp_stats.sblp_numeric_rejected++;
		return {};
	}

	std::unordered_map<rs::Var, rs::Var> local_variable;
	for (const PBconstraint& row : proof_rows)
		for (Int literal : row.lits) {
			const rs::Var variable = rs::toVar(static_cast<rs::Lit>(literal));
			if (!local_variable.count(variable))
				local_variable.emplace(variable, local_variable.size() + 1);
		}
	std::vector<PBconstraint> local_rows = proof_rows;
	for (PBconstraint& row : local_rows)
		for (Int& literal : row.lits) {
			const rs::Lit global = static_cast<rs::Lit>(literal);
			const rs::Var local = local_variable.at(rs::toVar(global));
			literal = global > 0 ? local : -local;
		}
	std::vector<rs::Lit> local_trail;
	for (rs::Lit literal : counter.trail) {
		const auto found = local_variable.find(rs::toVar(literal));
		if (found != local_variable.end())
			local_trail.push_back(literal > 0 ? found->second : -found->second);
	}

	enum class ExactState : std::uint8_t {
		PROVED,
		REJECTED,
		BASE_UNSAT,
		UNKNOWN
	};
	struct ExactResult {
		ExactState state = ExactState::UNKNOWN;
		std::uint64_t work = 0;
	};
	struct ProofSolver final : rs::Solver {
		bool add(const PBconstraint& row) {
			rs::CeArb constraint = cePools.takeArb();
			for (size_t i = 0; i < row.lits.size(); ++i)
				constraint->addLhs(rs::BigCoef(row.coeffs[i]),
						static_cast<rs::Lit>(row.lits[i]));
			constraint->addRhs(rs::BigCoef(row.rhs));
			constraint->orig = rs::Origin::FORMULA;
			return addInputConstraint(constraint).second != rs::ID_Unsat;
		}
	};
	auto exact = [&](rs::Lit implied) {
		ExactResult result;
		const auto local = local_variable.find(rs::toVar(implied));
		if (local == local_variable.end() || local_variable.size() > max_exact_variables)
			return result;

		struct RestoreGlobals {
			rs::Stats stats = rs::stats;
			double time_limit = rs::options.time_limit.get();
			~RestoreGlobals() {
				rs::stats = stats;
				rs::options.time_limit.get() = time_limit;
			}
		} restore;
		const double deadline = restore.stats.getTime() + exact_seconds;
		rs::Stats proof_stats{};
		proof_stats.STARTTIME = restore.stats.STARTTIME;
		proof_stats.RUNSTARTTIME = Glucose::cpuTime();
		rs::stats = proof_stats;
		if (restore.time_limit < 0 || deadline < restore.time_limit)
			rs::options.time_limit.get() = deadline;

		try {
			ProofSolver proof;
			proof.setNbVars(local_variable.size(), true);
			for (const PBconstraint& row : local_rows)
				if (!proof.add(row)) {
					result.state = ExactState::BASE_UNSAT;
					result.work = rs::stats.getDetTime();
					return result;
				}
			for (rs::Lit literal : local_trail)
				if (proof.addUnitConstraint(literal, rs::Origin::FORMULA).second ==
						rs::ID_Unsat) {
					result.state = ExactState::BASE_UNSAT;
					result.work = rs::stats.getDetTime();
					return result;
				}

			const rs::Lit opposite = implied > 0 ? -local->second : local->second;
			rs::IntSet assumption(local_variable.size(), {opposite});
			proof.setAssumptions(assumption);
			while (true) {
				const rs::SolveState state = proof.solve().state;
				if (state == rs::SolveState::INPROCESSED ||
						state == rs::SolveState::RESTARTED)
					continue;
				if (state == rs::SolveState::INCONSISTENT)
					result.state = ExactState::PROVED;
				else if (state == rs::SolveState::SAT)
					result.state = ExactState::REJECTED;
				else if (state == rs::SolveState::UNSAT)
					result.state = ExactState::BASE_UNSAT;
				break;
			}
		} catch (const rs::AsynchronousInterrupt&) {
			if (rs::asynch_interrupt) throw;
			result.state = ExactState::UNKNOWN;
		}
		result.work = rs::stats.getDetTime();
		return result;
	};

	std::unique_ptr<glp_prob, decltype(&glp_delete_prob)>
			lp(glp_create_prob(), &glp_delete_prob);
	struct GlpkOutput {
		int previous = glp_term_out(GLP_OFF);
		~GlpkOutput() { glp_term_out(previous); }
	} glpk_output;
	glp_add_rows(lp.get(), model.size());
	for (size_t row = 0; row < model.size(); ++row)
		glp_set_row_bnds(lp.get(), row + 1, GLP_LO,
				model[row].degree.template convert_to<double>(), 0.0);
	glp_add_cols(lp.get(), variables.size());
	for (size_t column = 0; column < variables.size(); ++column) {
		glp_set_col_bnds(lp.get(), column + 1, GLP_DB, 0.0, 1.0);
		glp_set_col_kind(lp.get(), column + 1, GLP_IV);
	}
	std::vector<int> row_index(1, 0);
	std::vector<int> column_index(1, 0);
	std::vector<double> values(1, 0.0);
	row_index.reserve(nonzeros + 1);
	column_index.reserve(nonzeros + 1);
	values.reserve(nonzeros + 1);
	for (size_t row = 0; row < model.size(); ++row)
		for (const auto& term : model[row].terms) {
			row_index.push_back(row + 1);
			column_index.push_back(term.first);
			values.push_back(term.second.template convert_to<double>());
		}
	glp_load_matrix(lp.get(), nonzeros,
			row_index.data(), column_index.data(), values.data());

	glp_smcp simplex;
	glp_init_smcp(&simplex);
	simplex.msg_lev = GLP_MSG_OFF;
	simplex.tm_lim = 50;
	glp_iocp integer;
	glp_init_iocp(&integer);
	integer.msg_lev = GLP_MSG_OFF;
	integer.tm_lim = 50;
	enum class MipState : std::uint8_t { OPTIMAL, INFEASIBLE, UNKNOWN };
	struct MipResult {
		MipState state = MipState::UNKNOWN;
		double objective = 0.0;
	};
	auto optimize = [&](int direction) {
		MipResult result;
		counter.simp_stats.sblp_mip_calls++;
		glp_set_obj_dir(lp.get(), direction);
		const int simplex_result = glp_simplex(lp.get(), &simplex);
		if (simplex_result == GLP_ENOPFS || simplex_result == GLP_ENOFEAS ||
				glp_get_status(lp.get()) == GLP_NOFEAS) {
			result.state = MipState::INFEASIBLE;
			return result;
		}
		if (simplex_result != 0 || glp_get_status(lp.get()) != GLP_OPT)
			return result;
		const int integer_result = glp_intopt(lp.get(), &integer);
		if (integer_result == GLP_ENOPFS || integer_result == GLP_ENOFEAS ||
				glp_mip_status(lp.get()) == GLP_NOFEAS) {
			result.state = MipState::INFEASIBLE;
			return result;
		}
		if (integer_result != 0 || glp_mip_status(lp.get()) != GLP_OPT)
			return result;
		result.state = MipState::OPTIMAL;
		result.objective = glp_mip_obj_val(lp.get());
		return result;
	};
	auto integer_value = [&](double value, int expected) {
		return std::isfinite(value) &&
				std::fabs(value - static_cast<double>(expected)) <= integer_tolerance;
	};

	std::vector<rs::Var> candidates = variables;
	bool budget = candidates.size() > max_candidates;
	if (budget) candidates.resize(max_candidates);
	std::uint64_t mip_calls = 0;
	std::uint64_t exact_work = 0;
	for (rs::Var variable : candidates) {
		if (mip_calls + 2 > max_mip_calls || exact_work >= max_exact_work) {
			budget = true;
			break;
		}
		const int column = columns.at(variable);
		glp_set_obj_coef(lp.get(), column, 1.0);

		mip_calls++;
		const MipResult minimum = optimize(GLP_MIN);
		rs::Lit implied = 0;
		if (minimum.state == MipState::INFEASIBLE) {
			counter.simp_stats.sblp_unknown++;
			glp_set_obj_coef(lp.get(), column, 0.0);
			break;
		}
		if (minimum.state == MipState::UNKNOWN) {
			counter.simp_stats.sblp_unknown++;
			glp_set_obj_coef(lp.get(), column, 0.0);
			continue;
		}
		if (integer_value(minimum.objective, 1)) {
			implied = variable;
		} else {
			mip_calls++;
			const MipResult maximum = optimize(GLP_MAX);
			if (maximum.state == MipState::OPTIMAL &&
					integer_value(maximum.objective, 0))
				implied = -variable;
			else if (maximum.state != MipState::OPTIMAL)
				counter.simp_stats.sblp_unknown++;
		}
		glp_set_obj_coef(lp.get(), column, 0.0);
		if (implied == 0) continue;

		counter.simp_stats.sblp_candidates++;
		counter.simp_stats.sblp_exact_checks++;
		const ExactResult checked = exact(implied);
		exact_work += checked.work;
		counter.simp_stats.sblp_work += checked.work;
		if (checked.work > max_exact_work || exact_work > max_exact_work)
			budget = true;
		if (checked.state == ExactState::PROVED) {
			counter.simp_stats.sblp_exact_passed++;
			counter.simp_stats.sblp_implied++;
			return imply(counter, implied);
		}
		if (checked.state == ExactState::REJECTED)
			counter.simp_stats.sblp_exact_rejected++;
		else {
			counter.simp_stats.sblp_unknown++;
			if (checked.state == ExactState::UNKNOWN) budget = true;
		}
	}
	if (budget) {
		counter.simp_stats.sblp_budgets++;
		return {SimpStatus::BUDGET};
	}
	return {};
#endif
}

template <typename T_data>
SimpResult Simplifier<T_data>::cs(PBcounter<T_data>& counter) {
	if (counter.config.disabled(SimpRule::CS)) return {};
	if (counter.qhead != static_cast<int>(counter.trail.size()))
		throw std::logic_error("CS requires a RoundingSat propagation fixpoint");

	struct Candidate {
		CurrentRow row;
		std::uint64_t coefficients = 0;
	};

	counter.simp_stats.cs_calls++;
	Component& component = counter.cmpmgr.topComponent();
	std::unordered_set<ClID> current_rows;
	current_rows.reserve(component.nlongCls());
	for (int i = component.nVars() + 1;
			i < component.nVars() + component.nlongCls() + 1; ++i)
		current_rows.insert(component[i]);

	auto build = [&](const RowInfo& info) {
		Candidate result;
		result.row = current(counter, info);
		if (!result.row.active) return result;

		counter.simp_stats.cs_rows++;
		if (info.group != NO_EQUALITY_GROUP)
			counter.simp_stats.cs_equality_rows++;
		if (!result.row.open) return result;

		rs::CeArb residual = counter.makeCe(result.row.content);
		const rs::BigVal gap = residual->getDegree();
		for (rs::Var variable : residual->vars) {
			const rs::Lit literal = residual->getLit(variable);
			if (residual->getCoef(literal) > gap) result.coefficients++;
		}
		if (result.coefficients != 0) residual->saturate();
		if (!residual->isSaturated())
			throw std::logic_error("RoundingSat failed to saturate the CS residual row");
		if (result.coefficients != 0)
			result.row.content = counter.contentOf(residual, info.source);
		return result;
	};

	std::unordered_set<EqualityGroupID> groups;
	for (const RowInfo& row : counter.rows_) {
		if (row.state != RowState::ATTACHED) continue;
		const ComponentBind& bind = counter.cmpmgr.rowBind(row.version);
		if (!current_rows.count(bind.clid)) continue;

		if (row.group == NO_EQUALITY_GROUP) {
			Candidate candidate = build(row);
			if (candidate.coefficients == 0) continue;
			counter.simp_stats.cs_changed_rows++;
			counter.simp_stats.cs_coefficients += candidate.coefficients;
			if (counter.cmpmgr.cache().legacy()) continue;
			if (!counter.simp_rows_.set(
					row, candidate.row.content, candidate.row.active,
					counter.decisionLevel()))
				throw std::logic_error("CS failed to publish a changed residual row");
			counter.simp_stats.cs_commits++;
			return {SimpStatus::ROW_CHANGED};
		}

		if (!groups.insert(row.group).second) continue;
		const EqGroup& group = findGroup(counter, row.group);
		Candidate low = build(findRow(counter, group.lower));
		Candidate high = build(findRow(counter, group.upper));
		const std::uint64_t changed_rows =
				(low.coefficients != 0) + (high.coefficients != 0);
		if (changed_rows == 0) continue;
		counter.simp_stats.cs_changed_rows += changed_rows;
		counter.simp_stats.cs_coefficients +=
				low.coefficients + high.coefficients;
		if (counter.cmpmgr.cache().legacy()) continue;
		if (!counter.simp_rows_.setGroup(group,
				low.row.content, low.row.active,
				high.row.content, high.row.active,
				counter.decisionLevel()))
			throw std::logic_error("CS failed to publish an equality transaction");
		counter.simp_stats.cs_commits++;
		return {SimpStatus::ROW_CHANGED};
	}
	return {};
}

template <typename T_data>
SimpResult Simplifier<T_data>::gcd(PBcounter<T_data>& counter) {
	if (counter.config.disabled(SimpRule::GCD)) return {};
	if (counter.qhead != static_cast<int>(counter.trail.size()))
		throw std::logic_error("GCD requires a RoundingSat propagation fixpoint");

	struct Candidate {
		CurrentRow row;
		bool changed = false;
	};

	counter.simp_stats.gcd_calls++;
	Component& component = counter.cmpmgr.topComponent();
	std::unordered_set<ClID> current_rows;
	current_rows.reserve(component.nlongCls());
	for (int i = component.nVars() + 1;
			i < component.nVars() + component.nlongCls() + 1; ++i)
		current_rows.insert(component[i]);

	auto build = [&](const RowInfo& info) {
		Candidate result;
		result.row = current(counter, info);
		if (!result.row.active) return result;
		counter.simp_stats.gcd_rows++;
		if (info.group != NO_EQUALITY_GROUP)
			counter.simp_stats.gcd_equality_rows++;
		if (!result.row.open) return result;

		rs::CeArb residual = counter.makeCe(result.row.content);
		if (!residual->isSaturated()) return result;
		if (!residual->divideByGCD()) return result;
		if (!residual->isSaturated())
			throw std::logic_error("RoundingSat produced a non-saturated GCD row");
		result.row.content = counter.contentOf(residual, info.source);
		result.changed = true;
		return result;
	};

	std::unordered_set<EqualityGroupID> groups;
	for (const RowInfo& row : counter.rows_) {
		if (row.state != RowState::ATTACHED) continue;
		const ComponentBind& bind = counter.cmpmgr.rowBind(row.version);
		if (!current_rows.count(bind.clid)) continue;

		if (row.group == NO_EQUALITY_GROUP) {
			Candidate candidate = build(row);
			if (!candidate.changed) continue;
			counter.simp_stats.gcd_changed_rows++;
			if (counter.cmpmgr.cache().legacy()) continue;
			if (!counter.simp_rows_.set(
					row, candidate.row.content, candidate.row.active,
					counter.decisionLevel()))
				throw std::logic_error("GCD failed to publish a changed residual row");
			counter.simp_stats.gcd_commits++;
			return {SimpStatus::ROW_CHANGED};
		}

		if (!groups.insert(row.group).second) continue;
		const EqGroup& group = findGroup(counter, row.group);
		Candidate low = build(findRow(counter, group.lower));
		Candidate high = build(findRow(counter, group.upper));
		const std::uint64_t changed_rows = low.changed + high.changed;
		if (changed_rows == 0) continue;
		counter.simp_stats.gcd_changed_rows += changed_rows;
		if (counter.cmpmgr.cache().legacy()) continue;
		if (!counter.simp_rows_.setGroup(group,
				low.row.content, low.row.active,
				high.row.content, high.row.active,
				counter.decisionLevel()))
			throw std::logic_error("GCD failed to publish an equality transaction");
		counter.simp_stats.gcd_commits++;
		return {SimpStatus::ROW_CHANGED};
	}
	return {};
}

template <typename T_data>
SimpResult Simplifier<T_data>::card(PBcounter<T_data>& counter) {
	if (counter.config.disabled(SimpRule::CARD)) return {};
	if (counter.qhead != static_cast<int>(counter.trail.size()))
		throw std::logic_error(
				"Cardinality normalization requires a RoundingSat propagation fixpoint");

	struct Candidate {
		CurrentRow row;
		bool changed = false;
	};

	counter.simp_stats.card_calls++;
	Component& component = counter.cmpmgr.topComponent();
	std::unordered_set<ClID> current_rows;
	current_rows.reserve(component.nlongCls());
	for (int i = component.nVars() + 1;
			i < component.nVars() + component.nlongCls() + 1; ++i)
		current_rows.insert(component[i]);

	auto build = [&](const RowInfo& info) {
		Candidate result;
		result.row = current(counter, info);
		if (!result.row.active) return result;
		counter.simp_stats.card_rows++;
		if (info.group != NO_EQUALITY_GROUP)
			counter.simp_stats.card_equality_rows++;
		if (!result.row.open) return result;

		rs::CeArb residual = counter.makeCe(result.row.content);
		if (!residual->isSaturated()) return result;
		residual->sortInDecreasingCoefOrder();
		if (!residual->simplifyToCardinality(true)) return result;
		if (!residual->isCardinality() || !residual->isSaturated())
			throw std::logic_error(
					"RoundingSat produced an invalid cardinality residual row");
		result.row.content = counter.contentOf(residual, info.source);
		result.changed = true;
		return result;
	};

	std::unordered_set<EqualityGroupID> groups;
	for (const RowInfo& row : counter.rows_) {
		if (row.state != RowState::ATTACHED) continue;
		const ComponentBind& bind = counter.cmpmgr.rowBind(row.version);
		if (!current_rows.count(bind.clid)) continue;

		if (row.group == NO_EQUALITY_GROUP) {
			Candidate candidate = build(row);
			if (!candidate.changed) continue;
			counter.simp_stats.card_changed_rows++;
			if (counter.cmpmgr.cache().legacy()) continue;
			if (!counter.simp_rows_.set(
					row, candidate.row.content, candidate.row.active,
					counter.decisionLevel()))
				throw std::logic_error(
						"Cardinality normalization failed to publish a residual row");
			counter.simp_stats.card_commits++;
			return {SimpStatus::ROW_CHANGED};
		}

		if (!groups.insert(row.group).second) continue;
		const EqGroup& group = findGroup(counter, row.group);
		Candidate low = build(findRow(counter, group.lower));
		Candidate high = build(findRow(counter, group.upper));
		const std::uint64_t changed_rows = low.changed + high.changed;
		if (changed_rows == 0) continue;
		counter.simp_stats.card_changed_rows += changed_rows;
		if (counter.cmpmgr.cache().legacy()) continue;
		if (!counter.simp_rows_.setGroup(group,
				low.row.content, low.row.active,
				high.row.content, high.row.active,
				counter.decisionLevel()))
			throw std::logic_error(
					"Cardinality normalization failed to publish an equality transaction");
		counter.simp_stats.card_commits++;
		return {SimpStatus::ROW_CHANGED};
	}
	return {};
}

template <typename T_data>
SimpResult Simplifier<T_data>::sc(PBcounter<T_data>& counter) {
	constexpr std::uint64_t max_pairs = 128;
	constexpr std::uint64_t max_work = 200000;
	constexpr size_t max_helpers = 8;
	constexpr size_t max_ratios = 4;
	struct Term {
		rs::Lit literal = 0;
		rs::BigCoef coefficient = 0;
	};
	struct ResidualRow {
		RowVersionID version = NO_ROW_VERSION;
		PBconstraint content;
		std::vector<Term> terms;
		rs::BigVal gap = 0;
	};
	struct RowViews {
		ResidualRow target;
		ResidualRow helper;
	};
	struct Ratio {
		rs::BigVal numerator = 0;
		rs::BigVal denominator = 1;
	};
	struct Proposal {
		bool found = false;
		RowVersionID target = NO_ROW_VERSION;
		RowVersionID helper = NO_ROW_VERSION;
		rs::Lit literal = 0;
		Ratio ratio;
	};

	if (counter.config.disabled(SimpRule::SC)) return {};
	if (counter.qhead != static_cast<int>(counter.trail.size()))
		throw std::logic_error("SC requires a RoundingSat propagation fixpoint");
	counter.simp_stats.sc_calls++;

	Component& component = counter.cmpmgr.topComponent();
	std::unordered_set<ClID> current_rows;
	current_rows.reserve(component.nlongCls());
	for (int i = component.nVars() + 1;
			i < component.nVars() + component.nlongCls() + 1; ++i)
		current_rows.insert(component[i]);

	std::uint64_t work = 0;
	bool budget = false;
	auto build = [&](const RowInfo& info, bool overlay, bool target) {
		ResidualRow result;
		const CurrentRow live = current(counter, info);
		if (!live.active || !live.open) return result;
		const CurrentRow view = overlay ? live : current(counter, info, false);
		if (!view.active || !view.open) return result;

		result.version = info.version;
		result.content = view.content;
		result.gap = view.content.rhs;
		for (size_t i = 0; i < view.content.lits.size(); ++i) {
			if (++work > max_work) {
				budget = true;
				return ResidualRow{};
			}
			rs::BigCoef coefficient(view.content.coeffs[i]);
			if (coefficient > result.gap) {
				if (target) return ResidualRow{};
				coefficient = result.gap;
			}
			result.terms.push_back({
					static_cast<rs::Lit>(view.content.lits[i]), coefficient});
		}
		return result;
	};

	std::vector<RowViews> rows;
	for (const RowInfo& row : counter.rows_) {
		if (row.state != RowState::ATTACHED) continue;
		const ComponentBind& bind = counter.cmpmgr.rowBind(row.version);
		if (!current_rows.count(bind.clid)) continue;
		RowViews views;
		views.target = build(row, true, true);
		if (budget) break;
		if (views.target.version == NO_ROW_VERSION) continue;
		views.helper = build(row, false, false);
		if (budget) break;
		rows.emplace_back(std::move(views));
	}
	counter.simp_stats.sc_rows += rows.size();

	auto add_ratio = [](std::vector<Ratio>& ratios,
			rs::BigVal numerator, rs::BigVal denominator) {
		if (numerator <= 0 || denominator <= 0) return;
		const rs::BigVal divisor = rs::aux::gcd(numerator, denominator);
		numerator /= divisor;
		denominator /= divisor;
		for (const Ratio& ratio : ratios) {
			if (ratio.numerator == numerator && ratio.denominator == denominator)
				return;
		}
		ratios.push_back({numerator, denominator});
	};

	auto lower_bound = [&](const ResidualRow& target, size_t excluded,
			const ResidualRow& helper, const Ratio& ratio) {
		std::map<rs::Var, rs::BigVal> coefficients;
		rs::BigVal constant = 0;
		auto add_literal = [&](const Term& term, const rs::BigVal& multiplier) {
			const rs::BigVal value = multiplier * term.coefficient;
			const rs::Var variable = rs::toVar(term.literal);
			if (term.literal > 0) {
				coefficients[variable] += value;
			} else {
				constant += value;
				coefficients[variable] -= value;
			}
		};
		for (size_t i = 0; i < target.terms.size(); ++i) {
			if (i == excluded) continue;
			add_literal(target.terms[i], ratio.denominator);
			work++;
		}
		for (const Term& term : helper.terms) {
			add_literal(term, -ratio.numerator);
			work++;
		}
		for (const auto& entry : coefficients) {
			if (entry.second < 0) constant += entry.second;
		}
		const rs::BigVal numerator =
				ratio.numerator * helper.gap + constant;
		rs::BigVal bound = rs::aux::ceildiv_safe(
				numerator, ratio.denominator);
		if (bound < 0) bound = 0;
		return bound;
	};

	std::uint64_t pairs = 0;
	Proposal proposal;
	for (const RowViews& target_views : rows) {
		const ResidualRow& target = target_views.target;
		size_t helpers = 0;
		for (const RowViews& helper_views : rows) {
			const ResidualRow& helper = helper_views.helper;
			if (helper.version == NO_ROW_VERSION ||
					target.version == helper.version)
				continue;
			if (helpers >= max_helpers || pairs >= max_pairs || work >= max_work) {
				budget = true;
				break;
			}

			bool support_overlap = false;
			for (const Term& left : target.terms) {
				for (const Term& right : helper.terms) {
					work++;
					if (rs::toVar(left.literal) == rs::toVar(right.literal))
						support_overlap = true;
				}
			}
			if (!support_overlap) continue;
			helpers++;
			pairs++;
			counter.simp_stats.sc_pairs++;

			for (size_t excluded = 0; excluded < target.terms.size(); ++excluded) {
				std::vector<Ratio> ratios;
				add_ratio(ratios, 1, 1);
				for (size_t i = 0; i < target.terms.size(); ++i) {
					if (i == excluded) continue;
					for (const Term& term : helper.terms) {
						if (target.terms[i].literal == term.literal)
							add_ratio(ratios, target.terms[i].coefficient,
									term.coefficient);
					}
				}
				std::sort(ratios.begin(), ratios.end(),
						[](const Ratio& left, const Ratio& right) {
							return left.numerator * right.denominator <
									right.numerator * left.denominator;
						});
				if (ratios.size() > max_ratios) ratios.resize(max_ratios);

				rs::BigVal best = 0;
				Ratio best_ratio;
				bool have_bound = false;
				for (const Ratio& ratio : ratios) {
					if (work >= max_work) {
						budget = true;
						break;
					}
					counter.simp_stats.sc_lambdas++;
					const rs::BigVal bound = lower_bound(
							target, excluded, helper, ratio);
					if (!have_bound || bound > best) {
						best = bound;
						best_ratio = ratio;
						have_bound = true;
					}
				}
				if (budget) break;
				if (best >= target.gap) continue;
				const rs::BigVal replacement = target.gap - best;
				if (replacement < target.terms[excluded].coefficient) {
					counter.simp_stats.sc_proposals++;
					counter.simp_stats.sc_coefficients++;
					proposal.found = true;
					proposal.target = target.version;
					proposal.helper = helper.version;
					proposal.literal = target.terms[excluded].literal;
					proposal.ratio = best_ratio;
					break;
				}
			}
			if (budget || proposal.found) break;
		}
		if (budget || proposal.found) break;
	}
	if (!proposal.found) {
		counter.simp_stats.sc_work += work;
		if (!budget) return {};
		counter.simp_stats.sc_budgets++;
		return {SimpStatus::BUDGET};
	}
	if (counter.cmpmgr.cache().legacy()) {
		counter.simp_stats.sc_work += work;
		return {};
	}

	counter.simp_stats.sc_revalidations++;
	const RowInfo& target_info = findRow(counter, proposal.target);
	const RowInfo& helper_info = findRow(counter, proposal.helper);
	ResidualRow target = build(target_info, true, true);
	ResidualRow helper = build(helper_info, false, false);
	if (budget || target.version == NO_ROW_VERSION ||
			helper.version == NO_ROW_VERSION || target.version == helper.version) {
		counter.simp_stats.sc_work += work;
		if (budget) {
			counter.simp_stats.sc_budgets++;
			return {SimpStatus::BUDGET};
		}
		return {};
	}

	size_t excluded = target.terms.size();
	for (size_t i = 0; i < target.terms.size(); ++i)
		if (target.terms[i].literal == proposal.literal) excluded = i;
	if (excluded == target.terms.size() ||
			work + target.terms.size() + helper.terms.size() > max_work) {
		counter.simp_stats.sc_work += work;
		if (excluded != target.terms.size()) {
			counter.simp_stats.sc_budgets++;
			return {SimpStatus::BUDGET};
		}
		return {};
	}

	const rs::BigVal bound = lower_bound(
			target, excluded, helper, proposal.ratio);
	if (bound >= target.gap) {
		counter.simp_stats.sc_work += work;
		return {};
	}
	const rs::BigVal replacement = target.gap - bound;
	if (replacement >= target.terms[excluded].coefficient) {
		counter.simp_stats.sc_work += work;
		return {};
	}

	PBconstraint changed = target.content;
	if (excluded >= changed.coeffs.size() ||
			changed.lits[excluded] != proposal.literal)
		throw std::logic_error("SC target content changed during revalidation");
	changed.coeffs[excluded] = replacement.convert_to<Int>();
	changed.sortDescendingCoeff();

	if (target_info.group == NO_EQUALITY_GROUP) {
		if (!counter.simp_rows_.set(
				target_info, changed, true, counter.decisionLevel()))
			throw std::logic_error("SC failed to publish a changed residual row");
	} else {
		const EqGroup& group = findGroup(counter, target_info.group);
		const RowInfo& lower_info = findRow(counter, group.lower);
		const RowInfo& upper_info = findRow(counter, group.upper);
		CurrentRow lower = current(counter, lower_info);
		CurrentRow upper = current(counter, upper_info);
		if (target_info.side == RowSide::LOWER)
			lower.content = changed;
		else if (target_info.side == RowSide::UPPER)
			upper.content = changed;
		else
			throw std::logic_error("SC found an invalid equality target side");
		if (!counter.simp_rows_.setGroup(group,
				lower.content, lower.active, upper.content, upper.active,
				counter.decisionLevel()))
			throw std::logic_error("SC failed to publish an equality transaction");
	}
	counter.simp_stats.sc_changed_rows++;
	counter.simp_stats.sc_commits++;
	counter.simp_stats.sc_work += work;
	return {SimpStatus::ROW_CHANGED};
}

template <typename T_data>
SimpResult Simplifier<T_data>::rir(PBcounter<T_data>& counter) {
	if (counter.config.disabled(SimpRule::RIR)) return {};
	if (counter.qhead != static_cast<int>(counter.trail.size()))
		throw std::logic_error("RIR requires a RoundingSat propagation fixpoint");
	counter.simp_stats.rir_calls++;

	Component& component = counter.cmpmgr.topComponent();
	std::unordered_set<ClID> current_rows;
	current_rows.reserve(component.nlongCls());
	for (int i = component.nVars() + 1;
			i < component.nVars() + component.nlongCls() + 1; ++i)
		current_rows.insert(component[i]);

	for (const RowInfo& row : counter.rows_) {
		if (row.state != RowState::ATTACHED) continue;
		const ComponentBind& bind = counter.cmpmgr.rowBind(row.version);
		if (!current_rows.count(bind.clid)) continue;

		const CurrentRow residual = current(counter, row);
		if (!residual.active) continue;
		counter.simp_stats.rir_rows++;
		if (row.group != NO_EQUALITY_GROUP)
			counter.simp_stats.rir_equality_rows++;
		if (residual.open) continue;

		counter.simp_stats.rir_proposals++;
		if (counter.cmpmgr.cache().legacy()) return {};

		std::uint64_t removed = 0;
		if (row.group == NO_EQUALITY_GROUP) {
			const CurrentRow staged = effective(counter, row);
			if (!staged.active || !counter.simp_rows_.set(
					row, staged.content, false, counter.decisionLevel()))
				throw std::logic_error("RIR failed to deactivate a satisfied row");
			removed = 1;
		} else {
			const EqGroup& group = findGroup(counter, row.group);
			const RowInfo& lower_info = findRow(counter, group.lower);
			const RowInfo& upper_info = findRow(counter, group.upper);
			CurrentRow lower = effective(counter, lower_info);
			CurrentRow upper = effective(counter, upper_info);
			const CurrentRow lower_residual = current(counter, lower_info);
			const CurrentRow upper_residual = current(counter, upper_info);
			if (lower.active && !lower_residual.open) {
				lower.active = false;
				removed++;
			}
			if (upper.active && !upper_residual.open) {
				upper.active = false;
				removed++;
			}
			if (removed == 0 || !counter.simp_rows_.setGroup(group,
					lower.content, lower.active, upper.content, upper.active,
					counter.decisionLevel()))
				throw std::logic_error(
						"RIR failed to deactivate a satisfied equality direction");
		}
		counter.simp_stats.rir_removed_rows += removed;
		counter.simp_stats.rir_commits++;
		return {SimpStatus::ROW_REMOVED};
	}
	return {};
}

template <typename T_data>
SimpResult Simplifier<T_data>::rpr(PBcounter<T_data>& counter) {
	if (counter.config.disabled(SimpRule::RPR)) return {};
	if (counter.qhead != static_cast<int>(counter.trail.size()))
		throw std::logic_error("RPR requires a RoundingSat propagation fixpoint");
	counter.simp_stats.rpr_calls++;

	struct NormalizedRow {
		RowVersionID version = NO_ROW_VERSION;
		EqualityGroupID group = NO_EQUALITY_GROUP;
		std::vector<std::pair<Int, Int>> terms;
		Int degree = 0;
		std::uint64_t hash = 1469598103934665603ULL;
		bool helper_safe = false;
	};
	struct Proposal {
		RowVersionID target = NO_ROW_VERSION;
		RowVersionID helper = NO_ROW_VERSION;
	};

	auto normalize = [&](const RowInfo& info, NormalizedRow& result) {
		const CurrentRow row = current(counter, info);
		if (!row.active || !row.open || row.content.coeffs.empty()) return false;
		if (row.content.lits.size() != row.content.coeffs.size() ||
				row.content.rhs <= 0)
			throw std::logic_error("RPR found a malformed residual row");

		Int divisor = 0;
		for (Int coefficient : row.content.coeffs) {
			if (coefficient <= 0)
				throw std::logic_error("RPR found a non-positive coefficient");
			divisor = std::gcd(divisor, coefficient);
		}
		if (divisor <= 0)
			throw std::logic_error("RPR failed to normalize a residual row");

		result = {};
		result.version = info.version;
		result.group = info.group;
		result.helper_safe = counter.simp_rows_.find(info.version) == nullptr;
		result.terms.reserve(row.content.lits.size());
		for (size_t i = 0; i < row.content.lits.size(); ++i)
			result.terms.emplace_back(
					row.content.lits[i], row.content.coeffs[i] / divisor);
		std::sort(result.terms.begin(), result.terms.end(),
				[](const auto& left, const auto& right) {
					return left.first < right.first;
				});
		result.degree = row.content.rhs / divisor +
				(row.content.rhs % divisor == 0 ? 0 : 1);

		auto mix = [&](std::uint64_t value) {
			result.hash ^= value;
			result.hash *= 1099511628211ULL;
		};
		mix(result.terms.size());
		for (const auto& term : result.terms)
			mix(static_cast<std::uint64_t>(term.first));
		return true;
	};
	auto sameLhs = [](const NormalizedRow& left,
			const NormalizedRow& right) {
		return left.terms == right.terms;
	};

	Component& component = counter.cmpmgr.topComponent();
	std::unordered_set<ClID> current_rows;
	current_rows.reserve(component.nlongCls());
	for (int i = component.nVars() + 1;
			i < component.nVars() + component.nlongCls() + 1; ++i)
		current_rows.insert(component[i]);

	std::vector<NormalizedRow> rows;
	std::unordered_map<std::uint64_t, std::vector<size_t>> buckets;
	Proposal proposal;
	for (const RowInfo& info : counter.rows_) {
		if (info.state != RowState::ATTACHED) continue;
		const ComponentBind& bind = counter.cmpmgr.rowBind(info.version);
		if (!current_rows.count(bind.clid)) continue;

		NormalizedRow row;
		if (!normalize(info, row)) continue;
		counter.simp_stats.rpr_rows++;
		if (info.group != NO_EQUALITY_GROUP)
			counter.simp_stats.rpr_equality_rows++;

		std::vector<size_t>& bucket = buckets[row.hash];
		for (size_t index : bucket) {
			const NormalizedRow& previous = rows[index];
			counter.simp_stats.rpr_pairs++;
			if (!sameLhs(previous, row) ||
					(previous.group != NO_EQUALITY_GROUP &&
					 previous.group == row.group))
				continue;

			if (previous.degree > row.degree && previous.helper_safe) {
				proposal.target = row.version;
				proposal.helper = previous.version;
			} else if (row.degree > previous.degree && row.helper_safe) {
				proposal.target = previous.version;
				proposal.helper = row.version;
			} else if (previous.degree == row.degree &&
					(previous.helper_safe || row.helper_safe)) {
				const bool keep_previous = previous.helper_safe &&
						(!row.helper_safe || previous.version < row.version);
				proposal.target = keep_previous ? row.version : previous.version;
				proposal.helper = keep_previous ? previous.version : row.version;
			}
			if (proposal.target != NO_ROW_VERSION) {
				counter.simp_stats.rpr_proposals++;
				break;
			}
		}
		if (proposal.target != NO_ROW_VERSION) break;
		bucket.push_back(rows.size());
		rows.emplace_back(std::move(row));
	}
	if (proposal.target == NO_ROW_VERSION || counter.cmpmgr.cache().legacy())
		return {};

	counter.simp_stats.rpr_revalidations++;
	const RowInfo& target_info = findRow(counter, proposal.target);
	const RowInfo& helper_info = findRow(counter, proposal.helper);
	NormalizedRow target;
	NormalizedRow helper;
	if (!normalize(target_info, target) || !normalize(helper_info, helper) ||
			target.version == helper.version || !sameLhs(target, helper) ||
			(target.group != NO_EQUALITY_GROUP && target.group == helper.group) ||
			!helper.helper_safe ||
			helper.degree < target.degree ||
			(helper.degree == target.degree && target.helper_safe &&
			 helper.version > target.version))
		return {};

	deactivate(counter, target_info);
	counter.simp_stats.rpr_removed_rows++;
	counter.simp_stats.rpr_commits++;
	return {SimpStatus::ROW_REMOVED};
}

template <typename T_data>
SimpResult Simplifier<T_data>::rsr(PBcounter<T_data>& counter) {
	constexpr std::uint64_t max_pairs = 4096;
	constexpr std::uint64_t max_work = 200000;
	if (counter.config.disabled(SimpRule::RSR)) return {};
	if (counter.qhead != static_cast<int>(counter.trail.size()))
		throw std::logic_error("RSR requires a RoundingSat propagation fixpoint");
	counter.simp_stats.rsr_calls++;

	struct Row {
		RowVersionID version = NO_ROW_VERSION;
		EqualityGroupID group = NO_EQUALITY_GROUP;
		PBconstraint content;
		bool helper_safe = false;
	};
	struct Proposal {
		RowVersionID target = NO_ROW_VERSION;
		RowVersionID helper = NO_ROW_VERSION;
	};

	auto build = [&](const RowInfo& info, Row& result) {
		const CurrentRow current_row = current(counter, info);
		if (!current_row.active || !current_row.open ||
				current_row.content.coeffs.empty())
			return false;
		result.version = info.version;
		result.group = info.group;
		result.content = current_row.content;
		result.helper_safe = counter.simp_rows_.find(info.version) == nullptr;
		return true;
	};
	auto entails = [&](const Row& helper, const Row& target,
			std::uint64_t& work, bool& budget) {
		const std::uint64_t cost = helper.content.lits.size() +
				target.content.lits.size();
		if (cost > max_work || work > max_work - cost) {
			budget = true;
			return false;
		}
		std::map<rs::Var, rs::BigVal> difference;
		rs::BigVal constant = 0;
		auto add = [&](const PBconstraint& row, int sign) {
			for (size_t i = 0; i < row.lits.size(); ++i) {
				const rs::Lit literal = static_cast<rs::Lit>(row.lits[i]);
				const rs::BigVal value = sign * rs::BigVal(row.coeffs[i]);
				const rs::Var variable = rs::toVar(literal);
				if (literal > 0) {
					difference[variable] += value;
				} else {
					constant += value;
					difference[variable] -= value;
				}
			}
		};
		add(target.content, 1);
		add(helper.content, -1);
		work += cost;
		counter.simp_stats.rsr_checks++;

		rs::BigVal minimum = constant;
		for (const auto& entry : difference)
			if (entry.second < 0) minimum += entry.second;
		return minimum >= rs::BigVal(target.content.rhs) -
				rs::BigVal(helper.content.rhs);
	};

	Component& component = counter.cmpmgr.topComponent();
	std::unordered_set<ClID> current_rows;
	current_rows.reserve(component.nlongCls());
	for (int i = component.nVars() + 1;
			i < component.nVars() + component.nlongCls() + 1; ++i)
		current_rows.insert(component[i]);

	std::vector<Row> rows;
	for (const RowInfo& info : counter.rows_) {
		if (info.state != RowState::ATTACHED) continue;
		const ComponentBind& bind = counter.cmpmgr.rowBind(info.version);
		if (!current_rows.count(bind.clid)) continue;
		Row row;
		if (!build(info, row)) continue;
		counter.simp_stats.rsr_rows++;
		if (info.group != NO_EQUALITY_GROUP)
			counter.simp_stats.rsr_equality_rows++;
		rows.emplace_back(std::move(row));
	}

	std::uint64_t work = 0;
	std::uint64_t pairs = 0;
	bool budget = false;
	Proposal proposal;
	for (size_t i = 0; i < rows.size() && !budget; ++i) {
		for (size_t j = i + 1; j < rows.size(); ++j) {
			if (rows[i].group != NO_EQUALITY_GROUP &&
					rows[i].group == rows[j].group)
				continue;
			if (pairs >= max_pairs) {
				budget = true;
				break;
			}
			pairs++;
			counter.simp_stats.rsr_pairs++;
			const bool first = rows[i].helper_safe &&
					entails(rows[i], rows[j], work, budget);
			if (budget) break;
			const bool second = rows[j].helper_safe &&
					entails(rows[j], rows[i], work, budget);
			if (budget) break;
			if (!first && !second) continue;

			if (first && second) {
				const bool keep_first = rows[i].version < rows[j].version;
				proposal.helper = keep_first ? rows[i].version : rows[j].version;
				proposal.target = keep_first ? rows[j].version : rows[i].version;
			} else if (first) {
				proposal.helper = rows[i].version;
				proposal.target = rows[j].version;
			} else {
				proposal.helper = rows[j].version;
				proposal.target = rows[i].version;
			}
			counter.simp_stats.rsr_proposals++;
			break;
		}
		if (proposal.target != NO_ROW_VERSION) break;
	}
	counter.simp_stats.rsr_work += work;
	if (proposal.target == NO_ROW_VERSION) {
		if (!budget) return {};
		counter.simp_stats.rsr_budgets++;
		return {SimpStatus::BUDGET};
	}
	if (counter.cmpmgr.cache().legacy()) return {};

	counter.simp_stats.rsr_revalidations++;
	const RowInfo& target_info = findRow(counter, proposal.target);
	const RowInfo& helper_info = findRow(counter, proposal.helper);
	Row target;
	Row helper;
	if (!build(target_info, target) || !build(helper_info, helper) ||
			target.version == helper.version || !helper.helper_safe ||
			(target.group != NO_EQUALITY_GROUP && target.group == helper.group))
		return {};
	std::uint64_t verify_work = 0;
	bool verify_budget = false;
	if (!entails(helper, target, verify_work, verify_budget)) {
		counter.simp_stats.rsr_work += verify_work;
		if (verify_budget) {
			counter.simp_stats.rsr_budgets++;
			return {SimpStatus::BUDGET};
		}
		return {};
	}
	counter.simp_stats.rsr_work += verify_work;

	deactivate(counter, target_info);
	counter.simp_stats.rsr_removed_rows++;
	counter.simp_stats.rsr_commits++;
	return {SimpStatus::ROW_REMOVED};
}

template <typename T_data>
SimpResult Simplifier<T_data>::rup(PBcounter<T_data>& counter) {
	constexpr std::uint64_t max_targets = 32;
	constexpr std::uint64_t max_work = 200000;
	if (counter.config.disabled(SimpRule::RUP)) return {};
	if (counter.qhead != static_cast<int>(counter.trail.size()))
		throw std::logic_error("RUP requires a RoundingSat propagation fixpoint");
	counter.simp_stats.rup_calls++;

	struct Row {
		RowVersionID version = NO_ROW_VERSION;
		EqualityGroupID group = NO_EQUALITY_GROUP;
		PBconstraint target;
		PBconstraint mother;
		bool helper_safe = false;
	};
	struct ProofSolver final : rs::Solver {
		bool add(const PBconstraint& row, bool negate) {
			rs::CeArb constraint = cePools.takeArb();
			rs::BigCoef degree(row.rhs);
			if (negate) {
				rs::BigCoef total = 0;
				for (Int coefficient : row.coeffs) total += coefficient;
				degree = total - degree + 1;
			}
			for (size_t i = 0; i < row.lits.size(); ++i)
				constraint->addLhs(rs::BigCoef(row.coeffs[i]),
						negate ? -static_cast<rs::Lit>(row.lits[i])
						       : static_cast<rs::Lit>(row.lits[i]));
			constraint->addRhs(degree);
			constraint->orig = rs::Origin::FORMULA;
			constraint->postProcess(Level, Pos, true, rs::stats);
			if (constraint->isTautology()) return false;
			if (constraint->hasNegativeSlack(Level)) return true;
			attachConstraint(constraint, true);
			return static_cast<bool>(runPropagation(true));
		}
	};

	Component& component = counter.cmpmgr.topComponent();
	std::unordered_set<ClID> current_rows;
	current_rows.reserve(component.nlongCls());
	for (int i = component.nVars() + 1;
			i < component.nVars() + component.nlongCls() + 1; ++i)
		current_rows.insert(component[i]);

	auto collect = [&]() {
		std::vector<Row> result;
		for (const RowInfo& info : counter.rows_) {
			if (info.state != RowState::ATTACHED) continue;
			const ComponentBind& bind = counter.cmpmgr.rowBind(info.version);
			if (!current_rows.count(bind.clid)) continue;
			const CurrentRow target = current(counter, info);
			if (!target.active || !target.open || target.content.coeffs.empty())
				continue;
			Row row;
			row.version = info.version;
			row.group = info.group;
			row.target = target.content;
			row.helper_safe = counter.simp_rows_.find(info.version) == nullptr;
			if (row.helper_safe) row.mother = effective(counter, info, false).content;
			result.emplace_back(std::move(row));
		}
		return result;
	};
	auto cost = [&](const std::vector<Row>& rows, size_t target) {
		std::uint64_t result = counter.trail.size() + rows[target].target.lits.size();
		for (size_t i = 0; i < rows.size(); ++i) {
			if (i == target || !rows[i].helper_safe) continue;
			if (rows[i].mother.lits.size() > max_work ||
					result > max_work - rows[i].mother.lits.size())
				return max_work + 1;
			result += rows[i].mother.lits.size();
		}
		return result;
	};
	auto prove = [&](const std::vector<Row>& rows, size_t target) {
		struct RestoreStats {
			rs::Stats saved;
			~RestoreStats() { rs::stats = saved; }
		} restore{rs::stats};
		ProofSolver proof;
		proof.setNbVars(counter.getNbVars(), true);
		for (rs::Lit literal : counter.trail)
			if (proof.addUnitConstraint(literal, rs::Origin::FORMULA).second ==
					rs::ID_Unsat)
				throw std::logic_error("RUP copied an inconsistent current trail");
		for (size_t i = 0; i < rows.size(); ++i) {
			if (i == target || !rows[i].helper_safe) continue;
			if (proof.add(rows[i].mother, false))
				throw std::logic_error("RUP helper formula is already inconsistent");
		}
		return proof.add(rows[target].target, true);
	};

	std::vector<Row> rows = collect();
	for (const Row& row : rows) {
		counter.simp_stats.rup_rows++;
		if (row.group != NO_EQUALITY_GROUP)
			counter.simp_stats.rup_equality_rows++;
	}
	std::uint64_t work = 0;
	bool budget = rows.size() > max_targets;
	RowVersionID proposal = NO_ROW_VERSION;
	const size_t targets = std::min<std::uint64_t>(rows.size(), max_targets);
	for (size_t i = 0; i < targets; ++i) {
		counter.simp_stats.rup_targets++;
		const std::uint64_t check_work = cost(rows, i);
		if (check_work > max_work || work > max_work - check_work) {
			budget = true;
			break;
		}
		work += check_work;
		counter.simp_stats.rup_checks++;
		if (!prove(rows, i)) continue;
		proposal = rows[i].version;
		counter.simp_stats.rup_proofs++;
		break;
	}
	counter.simp_stats.rup_work += work;
	if (proposal == NO_ROW_VERSION) {
		if (!budget) return {};
		counter.simp_stats.rup_budgets++;
		return {SimpStatus::BUDGET};
	}
	if (counter.cmpmgr.cache().legacy()) return {};

	rows = collect();
	size_t target = rows.size();
	for (size_t i = 0; i < rows.size(); ++i)
		if (rows[i].version == proposal) {
			target = i;
			break;
		}
	if (target == rows.size()) return {};
	const std::uint64_t verify_work = cost(rows, target);
	if (verify_work > max_work || work > max_work - verify_work) {
		counter.simp_stats.rup_budgets++;
		return {SimpStatus::BUDGET};
	}
	counter.simp_stats.rup_revalidations++;
	counter.simp_stats.rup_checks++;
	counter.simp_stats.rup_work += verify_work;
	if (!prove(rows, target)) return {};

	const RowInfo& target_info = findRow(counter, proposal);
	deactivate(counter, target_info);
	counter.simp_stats.rup_removed_rows++;
	counter.simp_stats.rup_commits++;
	return {SimpStatus::ROW_REMOVED};
}

template <typename T_data>
SimpResult Simplifier<T_data>::run(PBcounter<T_data>& counter) {
	const SimpResult checked = basic(counter); // Basic
	if (checked.status != SimpStatus::UNCHANGED) return checked;
	const SimpResult probed = bicp(counter); // BICP (AP + FLP)
	if (probed.status == SimpStatus::IMPLIED ||
			probed.status == SimpStatus::CONFLICT)
		return probed;
	const SimpResult bounded = sblp(counter); // SB-LP
	if (bounded.status == SimpStatus::IMPLIED ||
			bounded.status == SimpStatus::CONFLICT)
		return bounded;
	const SimpResult saturated = cs(counter); // CS
	if (saturated.status != SimpStatus::UNCHANGED) return saturated;
	const SimpResult divided = gcd(counter); // GCD
	if (divided.status != SimpStatus::UNCHANGED) return divided;
	const SimpResult cardinality = card(counter); // Cardinality
	if (cardinality.status != SimpStatus::UNCHANGED) return cardinality;
	const SimpResult strengthened = sc(counter); // SC
	if (strengthened.status != SimpStatus::UNCHANGED) return strengthened;
	const SimpResult removed = rir(counter); // RIR
	if (removed.status != SimpStatus::UNCHANGED) return removed;
	const SimpResult parallel = rpr(counter); // RPR
	if (parallel.status != SimpStatus::UNCHANGED) return parallel;
	const SimpResult subset = rsr(counter); // RSR
	if (subset.status != SimpStatus::UNCHANGED) return subset;
	const SimpResult propagated = rup(counter); // RUP
	if (propagated.status != SimpStatus::UNCHANGED) return propagated;
	if (bounded.status == SimpStatus::BUDGET) return bounded;
	return probed;
}

template class Simplifier<mpz_class>;
template class Simplifier<mpfr::mpreal>;
template void PBcounter<mpz_class>::backjumpTo(int);
template void PBcounter<mpfr::mpreal>::backjumpTo(int);

} // namespace GPMC
