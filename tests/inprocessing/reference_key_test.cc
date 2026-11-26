#include "core/Counter.h"

#include <gmpxx.h>
#include <sstream>
#include <string>

class ReferenceCounter final : public GPMC::PBcounter<mpz_class> {
public:
	explicit ReferenceCounter(GPMC::Configuration& config) : PBcounter(config) {}

	bool prepare(const std::string& formula) {
		std::istringstream input(formula);
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
		cmpmgr.init(nVars(), nPVars(), constraints, ca, rows_, &simp_rows_);
		validateRows(true);
		return true;
	}

	GPMC::ReferenceKey key() {
		syncAssign();
		return cmpmgr.referenceKey(cmpmgr.topComponent(), assigns);
	}

	GPMC::ReferenceKey keyAfter(rs::Lit literal) {
		decide(literal);
		if (runPropagationWithoutLearnt(true)) return {};
		return key();
	}

	bool setSingle(size_t index, PBconstraint content, bool active) {
		if (index >= rows_.size() || rows_[index].group != NO_EQUALITY_GROUP)
			return false;
		return simp_rows_.set(rows_[index], content, active, decisionLevel());
	}

	bool setEquality(PBconstraint lower, PBconstraint upper) {
		if (groups_.size() != 1) return false;
		return simp_rows_.setGroup(
				groups_.front(), lower, true, upper, true, decisionLevel());
	}

	const PBconstraint& row(size_t index) const { return rows_.at(index).content; }
	const EqGroup& group() const { return groups_.front(); }

	PBconstraint groupRow(RowVersionID version) const {
		for (const RowInfo& row : rows_)
			if (row.version == version) return row.content;
		return {};
	}

	int split() {
		syncAssign();
		return cmpmgr.splitComponent(assigns);
	}

	bool scopedRestore() {
		const GPMC::ReferenceKey before = key();
		decide(1);
		PBconstraint changed = rows_.front().content;
		changed.coeffs.front()++;
		if (!simp_rows_.set(rows_.front(), changed, true, decisionLevel()))
			return false;
		backjumpTo(0);
		return key() == before;
	}

	bool cacheSnapshot() {
		if (rows_.size() != 1) return false;
		syncAssign();
		GPMC::ComponentCache<mpz_class>& cache = cmpmgr.cache();
		if (!cache.residual()) return false;
		GPMC::Component& component = cmpmgr.topComponent();
		const GPMC::CacheEntryID root = component.id();
		if (root == 0 || !cache.entry(root).hasPendingKey() ||
				cache.entry(root).hasCompactKey() || cache.templateReferences() != 0)
			return false;

		PBconstraint changed = rows_.front().content;
		changed.coeffs.front()++;
		if (!simp_rows_.set(rows_.front(), changed, true, 0)) return false;
		cmpmgr.cacheModelCountOf(root, mpz_class(7), UNDEF_NODE);
		if (cache.entry(root).hasPendingKey() ||
				!cache.entry(root).hasCompactKey() || cache.templateReferences() != 1)
			return false;

		auto lookup = [&](bool expected) {
			const GPMC::CacheEntryID temporary = cache.createEntryFor(
					component, 0, cmpmgr, assigns);
			component.set_id(temporary);
			mpz_class count = 0;
			NodeIndex node = UNDEF_NODE;
			const bool hit = cache.requestValueOf(component, count, node);
			component.set_id(root);
			cache.eraseEntry(temporary);
			return hit == expected && (!hit || count == 7) &&
					cache.templateReferences() == 1;
		};
		if (!lookup(false)) return false;

		if (!simp_rows_.set(rows_.front(), rows_.front().content, true, 0))
			return false;
		if (!lookup(true)) return false;

		bool duplicate_store_rejected = false;
		try {
			cmpmgr.cacheModelCountOf(root, mpz_class(7), UNDEF_NODE);
		} catch (const std::logic_error&) {
			duplicate_store_rejected = true;
		}
		return duplicate_store_rejected && cache.templateReferences() == 1;
	}
};

GPMC::ReferenceKey makeKey(const std::string& formula) {
	GPMC::Configuration config;
	config.cntr.inprocess = true;
	config.cm.cacheMode = GPMC::CacheMode::BYPASS;
	ReferenceCounter counter(config);
	counter.init();
	if (!counter.prepare(formula)) return {};
	return counter.key();
}

int main() {
	const std::string ordered =
			"* #variable= 3 #constraint= 1\n"
			"+2 x1 +1 ~x2 +1 x3 >= 2 ;\n";
	const std::string reordered =
			"* #variable= 3 #constraint= 1\n"
			"+1 x3 +2 x1 +1 ~x2 >= 2 ;\n";
	const GPMC::ReferenceKey first = makeKey(ordered);
	const GPMC::ReferenceKey second = makeKey(reordered);
	if (first.bytes.empty() || first != second) return 1;

	const GPMC::ReferenceKey changed_coefficient = makeKey(
			"* #variable= 3 #constraint= 1\n"
			"+1 x1 +1 ~x2 +1 x3 >= 2 ;\n");
	const GPMC::ReferenceKey changed_gap = makeKey(
			"* #variable= 3 #constraint= 1\n"
			"+2 x1 +1 ~x2 +1 x3 >= 3 ;\n");
	if (first == changed_coefficient || first == changed_gap) return 2;

	const GPMC::ReferenceKey equality = makeKey(
			"* #variable= 2 #constraint= 1\n"
			"+1 x1 +1 x2 = 1 ;\n");
	const GPMC::ReferenceKey separate_directions = makeKey(
			"* #variable= 2 #constraint= 2\n"
			"+1 x1 +1 x2 >= 1 ;\n"
			"+1 x1 +1 x2 <= 1 ;\n");
	if (equality.bytes.empty() || equality == separate_directions) return 3;

	GPMC::Configuration config;
	config.cntr.inprocess = true;
	config.cm.cacheMode = GPMC::CacheMode::BYPASS;
	ReferenceCounter residual(config);
	residual.init();
	if (!residual.prepare(
			"* #variable= 4 #constraint= 1\n"
			"+2 x1 +1 x2 +1 x3 +1 x4 >= 2 ;\n"))
		return 4;
	const GPMC::ReferenceKey before = residual.key();
	const GPMC::ReferenceKey after = residual.keyAfter(-1);
	if (after.bytes.empty() || before == after) return 5;

	const GPMC::ReferenceKey huge = makeKey(
			"* #variable= 3 #constraint= 1\n"
			"+900000000000000000 x1 +700000000000000000 x2 +1 x3 "
			">= 800000000000000000 ;\n");
	if (huge.bytes.empty()) return 6;

	GPMC::ReferenceKey collision_left = first;
	GPMC::ReferenceKey collision_right = changed_gap;
	collision_right.hash = collision_left.hash;
	if (collision_left == collision_right) return 7;

	GPMC::RowTemplatePool templates;
	GPMC::ReferenceKey template_a{{1, 2, 3}, 17};
	GPMC::ReferenceKey template_b{{1, 2, 4}, 17};
	if (templates.probe(template_a) != 0) return 8;
	const GPMC::RowTemplateID id_a = templates.acquire(template_a);
	if (id_a == 0 || templates.probe(template_a) != id_a) return 9;
	templates.release(id_a);
	if (templates.acquire(template_a) != id_a || templates.wakes() != 1)
		return 10;
	templates.release(id_a);
	const GPMC::RowTemplateID id_b = templates.acquire(template_b);
	if (id_b == 0 || id_b == id_a || templates.probe(template_b) != id_b)
		return 11;
	templates.release(id_b);
	templates.reclaim(true);
	if (templates.probe(template_a) != 0 || templates.probe(template_b) != 0 ||
			templates.reclaimed() != 2)
		return 12;

	ReferenceCounter overlay(config);
	overlay.init();
	if (!overlay.prepare(ordered)) return 13;
	const GPMC::ReferenceKey frozen = overlay.key();
	PBconstraint changed = overlay.row(0);
	changed.coeffs.front()++;
	if (!overlay.setSingle(0, changed, true) || overlay.key() == frozen)
		return 14;

	ReferenceCounter equality_overlay(config);
	equality_overlay.init();
	if (!equality_overlay.prepare(
			"* #variable= 2 #constraint= 1\n"
			"+1 x1 +1 x2 = 1 ;\n"))
		return 15;
	const GPMC::ReferenceKey equality_frozen = equality_overlay.key();
	const EqGroup& eq = equality_overlay.group();
	PBconstraint low = equality_overlay.groupRow(eq.lower);
	PBconstraint high = equality_overlay.groupRow(eq.upper);
	low.coeffs.front()++;
	if (!equality_overlay.setEquality(low, high) ||
			equality_overlay.key() == equality_frozen)
		return 16;

	const std::string bridge =
			"* #variable= 4 #constraint= 3\n"
			"+1 x1 +1 x2 >= 1 ;\n"
			"+1 x3 +1 x4 >= 1 ;\n"
			"+1 x2 +1 x3 >= 1 ;\n";
	ReferenceCounter connected(config);
	connected.init();
	if (!connected.prepare(bridge) || connected.split() != 1) return 17;
	ReferenceCounter separated(config);
	separated.init();
	if (!separated.prepare(bridge) ||
			!separated.setSingle(2, separated.row(2), false) ||
			separated.split() != 2)
		return 18;

	ReferenceCounter scoped(config);
	scoped.init();
	if (!scoped.prepare(ordered) || !scoped.scopedRestore()) return 19;

	GPMC::Configuration legacy_config;
	legacy_config.cntr.inprocess = true;
	legacy_config.cm.cacheMode = GPMC::CacheMode::LEGACY;
	ReferenceCounter legacy_overlay(legacy_config);
	legacy_overlay.init();
	if (!legacy_overlay.prepare(ordered)) return 20;
	PBconstraint legacy_change = legacy_overlay.row(0);
	legacy_change.coeffs.front()++;
	if (!legacy_overlay.setSingle(0, legacy_change, true)) return 21;
	bool legacy_rejected = false;
	try {
		legacy_overlay.key();
	} catch (const std::logic_error&) {
		legacy_rejected = true;
	}
	if (!legacy_rejected) return 22;

	GPMC::Configuration residual_config;
	residual_config.cntr.inprocess = true;
	residual_config.cm.cacheMode = GPMC::CacheMode::RESIDUAL;
	ReferenceCounter snapshot(residual_config);
	snapshot.init();
	if (!snapshot.prepare(ordered) || !snapshot.cacheSnapshot()) return 23;
	return 0;
}
