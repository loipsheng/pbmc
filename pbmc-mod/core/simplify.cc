#include <gmpxx.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "mpfr/mpreal.h"
#include "core/Counter.h"

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
typename Simplifier<T_data>::CurrentRow Simplifier<T_data>::current(
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
	auto commit = [&](rs::Lit implied) {
		if (implied == 0 || counter.Pos[rs::toVar(implied)] != rs::INF)
			throw std::logic_error("BICP tried to submit a non-new implication");

		rs::CeArb reason = counter.cePools.takeArb();
		for (int position : counter.trail_lim)
			reason->addLhs(rs::BigCoef(1), -counter.trail[position]);
		reason->addLhs(rs::BigCoef(1), implied);
		reason->addRhs(rs::BigCoef(1));
		reason->orig = rs::Origin::LEARNED;
		reason->postProcess(counter.Level, counter.Pos, true, rs::stats);
		if (reason->isTautology() || reason->isInconsistency())
			throw std::logic_error("BICP produced an invalid guarded reason");

		const rs::CRef cref = counter.attachConstraint(reason, false);
		rs::Constr& attached = counter.ca[cref];
		attached.setLBD(attached.size());
		if (!rs::isTrue(counter.Level, implied))
			throw std::logic_error("BICP reason did not propagate its implication");
		if (counter.decisionLevel() > 0 &&
				counter.Reason[rs::toVar(implied)] != cref)
			throw std::logic_error("BICP implication has no live RoundingSat Reason");
		counter.simp_stats.bicp_implied++;
		return SimpResult{SimpStatus::IMPLIED};
	};

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
		if (implied != 0) return commit(implied);

		if (budget_limited && node_work >= max_node_work) break;
	}
	return {budget_limited ? SimpStatus::BUDGET : SimpStatus::UNCHANGED};
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
SimpResult Simplifier<T_data>::run(PBcounter<T_data>& counter) {
	const SimpResult checked = basic(counter); // Basic
	if (checked.status != SimpStatus::UNCHANGED) return checked;
	const SimpResult probed = bicp(counter); // BICP (AP + FLP)
	if (probed.status == SimpStatus::IMPLIED ||
			probed.status == SimpStatus::CONFLICT)
		return probed;
	const SimpResult saturated = cs(counter); // CS
	if (saturated.status != SimpStatus::UNCHANGED) return saturated;
	const SimpResult divided = gcd(counter); // GCD
	if (divided.status != SimpStatus::UNCHANGED) return divided;
	const SimpResult cardinality = card(counter); // Cardinality
	if (cardinality.status != SimpStatus::UNCHANGED) return cardinality;
	const SimpResult strengthened = sc(counter); // SC
	if (strengthened.status != SimpStatus::UNCHANGED) return strengthened;
	return probed;
}

template class Simplifier<mpz_class>;
template class Simplifier<mpfr::mpreal>;
template void PBcounter<mpz_class>::backjumpTo(int);
template void PBcounter<mpfr::mpreal>::backjumpTo(int);

} // namespace GPMC
