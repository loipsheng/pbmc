#include <gmpxx.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

#include "mpfr/mpreal.h"
#include "core/Counter.h"

namespace GPMC {

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
SimpResult Simplifier<T_data>::run(PBcounter<T_data>& counter) {
	const SimpResult checked = basic(counter);
	if (checked.status != SimpStatus::UNCHANGED) return checked;
	return bicp(counter);
}

template class Simplifier<mpz_class>;
template class Simplifier<mpfr::mpreal>;

} // namespace GPMC
