/*
Copyright (c) 2025 Grabtaxi Holdings Pte Ltd (GRAB), All rights reserved.

Use of this source code is governed by an MIT-style license that can be found in the LICENSE file
*/

#include "core/Component.h"
#include "mtl/Sort.h"
#include <utility>
#include <limits.h>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

using namespace GPMC;

#define CACHE

template <typename T_data>
void ComponentManager<T_data>::init(int nvars, int npvars, const vec<CRef>& sclauses, const ClauseAllocator& sca){
	vec< vec<Var> > binlinks(nvars);
	vec< vec<ClID> > occ(nvars);

	occlists_.growTo(nvars);
	var_frequency_.growTo(nvars);

	for(int i = 0; i < sclauses.size(); i++) {
		Clause& sc = (Clause &)sca[sclauses[i]];
		if(sc.size() == 2) {
			binlinks[var(sc[0])].push(var(sc[1]));
			binlinks[var(sc[1])].push(var(sc[0]));
			var_frequency_[var(sc[0])]++;
			var_frequency_[var(sc[1])]++;
		}
		else {	// sc.size() > 2 (because bcp was already done and satisfied clauses were removed)
			clauses_.push(ca_.alloc(sc));
			for(int j = 0; j < sc.size(); j++){
				occ[var(sc[j])].push(clauses_.size()-1);
				var_frequency_[var(sc[j])]++;
			}
		}
	}

	occ_pool_.clear();
	for(Var v = 0; v < nvars; v++) {
		occlists_[v] = occ_pool_.size();

		vec<Var>& bl = binlinks[v];
		sort(bl);
		Var prev = var_Undef;
		for(int i = 0; i < bl.size(); i++)
			if(bl[i] != prev) {
				occ_pool_.push(bl[i]);
				prev = bl[i];
			}
		occ_pool_.push(var_Undef);

		vec<ClID>& ocl = occ[v];
		for(int i = 0; i < ocl.size(); i++)
			occ_pool_.push(ocl[i]);
		occ_pool_.push(clid_Undef);
	}

	varseen_.growTo(nvars, SC_CANDIDATE);
	clseen_.growTo(clauses_.size(), SC_CANDIDATE);
	npvars_ = npvars;

#ifdef CACHE
	cache_.init(config.cachesize, config.cacheMode);
	CachedComponent<T_data>::adjustPackSize(nvars, clauses_.size());
#endif

	nodeMgr.init(npvars_);

	initComponentStack(nvars, clauses_.size());
	initDecisionStack();

	components = 1;
	num_try_split = 0;
}


// component manager init for roundingsat pb data structures
#define CACHE

template <typename T_data>
void ComponentManager<T_data>::init(int nvars, int npvars,
		const vector<rs::CRef>& sconstraints, const rs::ConstraintAllocator& sca,
		const vector<RowInfo>& rows) {
	vec< vec<Var> > binlinks(nvars);
	vec< vec<ClID> > occ(nvars); // using clid here because rs::ID is unsigned and we use negative values as seperator values
	unordered_map<uint64_t, RowVersionID> versions;
	unordered_map<RowVersionID, const RowInfo*> row_info;
	row_binds_.clear();
	clid_binds_.clear();
	for (const RowInfo& row : rows) {
		if (row.version == NO_ROW_VERSION || !row_info.emplace(row.version, &row).second)
			throw logic_error("invalid or duplicate row version");
		if (row.state == RowState::ROOT_TRUE) {
			if (row.solver_id != NO_SOLVER_ID)
				throw logic_error("ROOT_TRUE row unexpectedly has a solver ID");
			continue;
		}
		if (row.version == NO_ROW_VERSION || row.solver_id == NO_SOLVER_ID ||
				!versions.emplace(row.solver_id, row.version).second)
			throw logic_error("invalid or duplicate attached row identity");
	}

	occlists_.growTo(nvars); 
	var_frequency_.growTo(nvars);

	// pool expressions should have n+1, 0 is unused for lit
	cepools_.resize(nvars + 1);

	bits_of_weights_.growTo(sconstraints.size());
	for (int i = 0; i < sconstraints.size(); i++) {
		const rs::Constr& C = sca[sconstraints[i]];
		rs::ID comp_cr_id = C.id; // use the same constraint ID as in counter
		const rs::CeSuper& ce = C.toExpanded(cepools_); // expand using component manager pool
		// now to attach the expanded constraint to the component manager constraint allocator
		rs::CRef comp_cr = ce->toConstr(pbca_, true, comp_cr_id); // write the constraint into component manager constraint allocator
		constraints_.push_back(comp_cr);
		auto version = versions.find(C.id);
		ComponentBind binding;
		binding.clid = static_cast<ClID>(constraints_.size() - 1);
		binding.cref = comp_cr;
		if (version != versions.end()) {
			auto metadata = row_info.find(version->second);
			if (metadata == row_info.end() || metadata->second->state != RowState::ATTACHED)
				throw logic_error("attached constraint has invalid row metadata");
			binding.version = version->second;
			binding.group = metadata->second->group;
			binding.side = metadata->second->side;
			if (!row_binds_.emplace(binding.version, binding).second)
				throw logic_error("duplicate ComponentManager row binding");
		}
		clid_binds_.push_back(binding);
		// still need to fill up the var_frequency_ and occ
		for (size_t j = 0; j < C.size(); j++) {
			var_frequency_[rs::toVar(C.lit(j))-1]++;
			occ[rs::toVar(C.lit(j))-1].push(constraints_.size()-1); 
		}
		bits_of_weights_[i] = (unsigned int) ceil(log((double) C.degree()) / log(2.0));
	}
	for (const RowInfo& row : rows) {
		if (row.state == RowState::ATTACHED && !hasRowBind(row.version))
			throw logic_error("attached row is missing its ComponentManager binding");
		if (row.state == RowState::ROOT_TRUE && hasRowBind(row.version))
			throw logic_error("ROOT_TRUE row unexpectedly has a ComponentManager binding");
	}

	occ_pool_.clear();
	for(Var v = 0; v < nvars; v++) {
		occlists_[v] = occ_pool_.size();
		vec<Var>& bl = binlinks[v];
		sort(bl);
		Var prev = var_Undef;
		for(int i = 0; i < bl.size(); i++)
			if(bl[i] != prev) {
				occ_pool_.push(bl[i]);
				prev = bl[i];
			}
		occ_pool_.push(var_Undef);

		vec<ClID>& ocl = occ[v];
		for(int i = 0; i < ocl.size(); i++)
			occ_pool_.push(ocl[i]);
		occ_pool_.push(clid_Undef);
	}

	varseen_.growTo(nvars, SC_CANDIDATE);
	clseen_.growTo(constraints_.size(), SC_CANDIDATE);
	gaps_.resize(constraints_.size());
	npvars_ = npvars;

#ifdef CACHE
	cache_.init(config.cachesize, config.cacheMode);
	CachedComponent<T_data>::adjustPackSize(nvars, constraints_.size());
#endif

	nodeMgr.init(npvars_);

	initComponentStack(nvars, constraints_.size());
	initDecisionStack();

	components = 1;
	num_try_split = 0;
}
// end of init for roundingsat pb data structures

template <typename T_data>
bool ComponentManager<T_data>::hasRowBind(RowVersionID version) const {
	return row_binds_.find(version) != row_binds_.end();
}

template <typename T_data>
const ComponentBind& ComponentManager<T_data>::rowBind(RowVersionID version) const {
	auto found = row_binds_.find(version);
	if (found == row_binds_.end())
		throw logic_error("unknown row version in ComponentManager");
	return found->second;
}

template <typename T_data>
bool ComponentManager<T_data>::validRowBind(
		RowVersionID version, uint64_t solver_id) const {
	auto found = row_binds_.find(version);
	if (found == row_binds_.end()) return false;
	const ComponentBind& binding = found->second;
	if (binding.clid < 0 || static_cast<size_t>(binding.clid) >= constraints_.size() ||
			constraints_[binding.clid].ofs != binding.cref.ofs)
		return false;
	return pbca_[binding.cref].id == solver_id;
}

template <typename T_data>
ReferenceKey ComponentManager<T_data>::referenceKey(
		Component& component, const vec<lbool>& assigns) const {
	struct ResidualRow {
		bool active = false;
		vector<uint8_t> bytes;
	};

	auto appendNumber = [](uint64_t value, vector<uint8_t>& output) {
		do {
			uint8_t byte = static_cast<uint8_t>(value & 0x7fU);
			value >>= 7;
			if (value != 0) byte |= 0x80U;
			output.push_back(byte);
		} while (value != 0);
	};
	auto appendBig = [&](const rs::BigVal& value, vector<uint8_t>& output) {
		const string text = value.str();
		appendNumber(text.size(), output);
		output.insert(output.end(), text.begin(), text.end());
	};

	unordered_map<Var, uint64_t> local;
	vector<Var> variables;
	variables.reserve(component.nVars());
	for (int i = 0; i < component.nVars(); ++i) {
		const Var variable = component[i];
		if (value(variable, assigns) != l_Undef) continue;
		local.emplace(variable, variables.size());
		variables.push_back(variable);
	}

	unordered_set<ClID> component_rows;
	component_rows.reserve(component.nlongCls());
	for (int i = component.nVars() + 1;
			i < component.nVars() + component.nlongCls() + 1; ++i)
		component_rows.insert(component[i]);

	auto row = [&](ClID clid) {
		ResidualRow result;
		if (clid == clid_Undef) return result;
		if (clid < 0 || static_cast<size_t>(clid) >= constraints_.size())
			throw logic_error("residual key found an invalid component row");

		const rs::Constr& constraint = pbca_[constraints_[clid]];
		rs::BigVal gap = constraint.degree();
		vector<pair<int64_t, rs::BigCoef>> terms;
		terms.reserve(constraint.size());
		for (size_t i = 0; i < constraint.size(); ++i) {
			const rs::Lit literal = constraint.lit(i);
			const Var variable = rs::toVar(literal) - 1;
			const lbool assigned = value(variable, assigns);
			const bool literal_true =
					assigned != l_Undef &&
					((literal > 0 && assigned == l_True) ||
					 (literal < 0 && assigned == l_False));
			if (literal_true) {
				gap -= constraint.coef(i);
			} else if (assigned == l_Undef) {
				auto position = local.find(variable);
				if (position == local.end())
					throw logic_error("residual row escaped its component variables");
				const int64_t signed_local = literal > 0
						? static_cast<int64_t>(position->second + 1)
						: -static_cast<int64_t>(position->second + 1);
				terms.emplace_back(signed_local, constraint.coef(i));
			}
		}
		if (gap <= 0) return result;

		rs::BigVal available = 0;
		for (const auto& term : terms) available += term.second;
		if (terms.empty() || available < gap)
			throw logic_error("residual key reached an unpropagated conflict");

		sort(terms.begin(), terms.end(),
				[](const auto& left, const auto& right) {
					return left.first < right.first;
				});
		result.active = true;
		appendNumber(terms.size(), result.bytes);
		for (const auto& term : terms) {
			appendBig(rs::BigVal(term.first), result.bytes);
			appendBig(term.second, result.bytes);
		}
		appendBig(gap, result.bytes);
		return result;
	};

	vector<vector<uint8_t>> records;
	unordered_set<EqualityGroupID> groups;
	for (ClID clid : component_rows) {
		if (static_cast<size_t>(clid) >= clid_binds_.size())
			throw logic_error("residual key row has no version binding");
		const ComponentBind& binding = clid_binds_[clid];
		if (binding.group == NO_EQUALITY_GROUP) {
			ResidualRow single = row(clid);
			if (!single.active) continue;
			vector<uint8_t> record;
			record.reserve(single.bytes.size() + 1);
			record.push_back(1);
			for (uint8_t byte : single.bytes) record.push_back(byte);
			records.emplace_back(std::move(record));
			continue;
		}
		groups.insert(binding.group);
	}

	for (EqualityGroupID group : groups) {
		ClID lower = clid_Undef;
		ClID upper = clid_Undef;
		for (const auto& entry : row_binds_) {
			const ComponentBind& binding = entry.second;
			if (binding.group != group) continue;
			if (binding.side == RowSide::LOWER) lower = binding.clid;
			else if (binding.side == RowSide::UPPER) upper = binding.clid;
			else throw logic_error("equality row has no direction");
		}
		ResidualRow low = row(lower);
		ResidualRow high = row(upper);
		if (low.active && !component_rows.count(lower))
			throw logic_error("active equality lower row escaped its component");
		if (high.active && !component_rows.count(upper))
			throw logic_error("active equality upper row escaped its component");
		if (!low.active && !high.active) continue;

		vector<uint8_t> record(1, 2);
		record.push_back(low.active ? 1 : 0);
		if (low.active) {
			appendNumber(low.bytes.size(), record);
			record.insert(record.end(), low.bytes.begin(), low.bytes.end());
		}
		record.push_back(high.active ? 1 : 0);
		if (high.active) {
			appendNumber(high.bytes.size(), record);
			record.insert(record.end(), high.bytes.begin(), high.bytes.end());
		}
		records.emplace_back(std::move(record));
	}

	sort(records.begin(), records.end());
	ReferenceKey result;
	appendNumber(1, result.bytes);
	appendNumber(variables.size(), result.bytes);
	for (Var variable : variables)
		appendNumber(static_cast<uint64_t>(variable), result.bytes);
	appendNumber(records.size(), result.bytes);
	for (const vector<uint8_t>& record : records) {
		appendNumber(record.size(), result.bytes);
		result.bytes.insert(result.bytes.end(), record.begin(), record.end());
	}
	result.hash = 14695981039346656037ULL;
	for (uint8_t byte : result.bytes) {
		result.hash ^= byte;
		result.hash *= 1099511628211ULL;
	}
	return result;
}

template <typename T_data>
void ComponentManager<T_data>::initComponentStack(int nvars, int nlongcls) {
	comp_stack_.clear();
	comp_stack_.reserve(nvars + 1);
	comp_stack_.push_back(new Component(nvars, nlongcls, true));

	Component & orig = *(comp_stack_[0]);
	for(Var  v = 0; v < nvars; v++)    orig.pushVar(v);
	orig.closeVar();
	for(ClID c = 0; c < nlongcls; c++) orig.pushClID(c);
	orig.closeClID();

#ifdef CACHE
	vec<lbool> assigns(nvars + 1, l_Undef);
	CacheEntryID id = cache_.createEntryFor(*comp_stack_.back(), comp_stack_.size() - 1, *this, assigns);
	comp_stack_.back()->set_id(id);
#endif
}

template <typename T_data>
void ComponentManager<T_data>::initDecisionStack()
{
	dl_.clear();
	dl_.push_back(Decision<T_data>(0,0));
	dl_.back().changeBranch();
	dl_.back().setNodeIdx(nodeMgr.newNode(DT_AND));
}

template <typename T_data>
int ComponentManager<T_data>::splitComponent(const vec<lbool>& assigns, const vec<T_data>& lit_weight){
	int p; Var v; ClID c;
	T_data tmp_model_count;
	NodeIndex cachedNode = UNDEF_NODE;

	unsigned oldtop = comp_stack_.size();
	unsigned boundary = oldtop;
	Component & targetcomp =  *(comp_stack_.back());

	for(p = 0; (v = targetcomp[p]) != var_Undef; p++) {
		if(value(v, assigns) == l_Undef){
			varseen_[v] = SC_CANDIDATE;
			var_frequency_[v] = 0;
		}
		else varseen_[v] = SC_NOT_CANDIDATE;
	}

	for(++p; (c = targetcomp[p]) != clid_Undef; p++) {
		clseen_[c] = SC_CANDIDATE;
	}


	// Start splitting
	int nvars_in_comp, ncls_in_comp;
	for (p = 0; (v = targetcomp[p]) != var_Undef; p++) {
		if (varseen_[v] == SC_CANDIDATE) {
			searchComponent(v, assigns, nvars_in_comp, ncls_in_comp);
			if (nvars_in_comp == 1) {
				if(isPVar(v)){
					if(config.weighted)
						dl_.back().increaseModels(lit_weight[toInt(mkLit(v, true))]+lit_weight[toInt(mkLit(v, false))], true);
					else
						dl_.back().increaseModels((T_data)2, true);

					if(config.ddnnf) addNode(nodeMgr.getDCNode(v));
				}
				else
					dl_.back().increaseModels((T_data)1, true);
				varseen_[v] = SC_DONE;
			}else{
				comp_stack_.push_back(new Component(nvars_in_comp, ncls_in_comp, isPVar(v)));

				Component & newcomp = *(comp_stack_.back());

				int pp; Var vv; ClID cc;
				for (pp = 0; (vv = targetcomp[pp]) != var_Undef; pp++)
					if (varseen_[vv] == SC_SEEN) {
						newcomp.pushVar(vv);
						varseen_[vv] = SC_DONE;
					}
				newcomp.closeVar();
				for (++pp; (cc = targetcomp[pp]) != clid_Undef; pp++)
					if (clseen_[cc] == SC_SEEN) {
						newcomp.pushClID(cc);
						clseen_[cc] = SC_DONE;
					}
				newcomp.closeClID();
#ifndef CACHE
				if(isPVar(v)) boundary++;
#endif
#ifdef CACHE
				// caching
				CacheEntryID id = this->cache_.createEntryFor(*comp_stack_.back(), comp_stack_.size() - 1, *this);
				if (id != 0) {
					comp_stack_.back()->set_id(id);
					assert(cache_.hasEntry(id));
					assert(cache_.hasEntry(targetcomp.id()));
					if (cache_.requestValueOf(*comp_stack_.back(), tmp_model_count, cachedNode)) {
						dl_.back().increaseModels(tmp_model_count, true);
						if(config.ddnnf && cachedNode != TOP_NODE) {
							addNode(cachedNode);
						}
						cache_.eraseEntry(id);
						delete comp_stack_.back();
						comp_stack_.pop_back();
					} else {
						cache_.entry(id).set_father(targetcomp.id());
						cache_.add_descendant(targetcomp.id(), id);
						if(isPVar(v)) boundary++;
					}
				}
#endif
			}
		}
	}

	Decision<T_data>& curdl = dl_.back();
	curdl.setSplitCompsEnd(comp_stack_.size());

	for (auto i = oldtop; i < boundary; i++)
		for (auto j = i + 1; j < boundary; j++)
			if (comp_stack_[i]->nVars()< comp_stack_[j]->nVars())
				std::swap(comp_stack_[i], comp_stack_[j]);

	components += comp_stack_.size() - oldtop;
	if(comp_stack_.size() != oldtop) num_try_split++;
	return comp_stack_.size() - oldtop;
}

template <class T_data>
void ComponentManager<T_data>::searchComponent(Var seed_var, const vec<lbool>& assigns, int& nvars_in_comp, int& ncls_in_comp){
	vec<Var> vars_in_comp;
	long mincoef_idx = -1; //storing index of unassigned var with min coeff for gap saturation
	rs::BigVal gap;

	ncls_in_comp = 0;
	vars_in_comp.push(seed_var);
	varseen_[seed_var] = SC_SEEN;

	int p; Var v1, v2; ClID c;
	for(p = 0; p < vars_in_comp.size(); p++){
		v1 = vars_in_comp[p];

		uint32_t q;
		for(q = occlists_[v1]; (v2 = occ_pool_[q]) != var_Undef; q++){
			if(varseen_[v2] == SC_CANDIDATE) {
				assert(value(v2,assigns)==l_Undef);
				vars_in_comp.push(v2);
				varseen_[v2] = SC_SEEN;
				var_frequency_[v1]++;
				var_frequency_[v2]++;
			}
		}

		int nlits;
		for(++q; (c = occ_pool_[q]) != clid_Undef; q++) {
			if(clseen_[c] == SC_CANDIDATE) {
				rs::CRef cref = constraints_[c];
				const rs::Constr& C = pbca_[cref];
				gap = C.degree();
				int before = vars_in_comp.size();
				nlits = 0;
				mincoef_idx = -1; //storing index of unassigned var with min coeff for gap saturation
				for(int i = 0; i < C.size(); i++) {
					if (C.lit(i) > 0) {
						if (assigns[rs::toVar(C.lit(i)) - 1] == l_False) continue;
						else if (assigns[rs::toVar(C.lit(i)) - 1] == l_True) gap -= C.coef(i);
						else mincoef_idx = i;
					} else {
						if (assigns[rs::toVar(C.lit(i)) - 1] == l_True) continue;
						else if (assigns[rs::toVar(C.lit(i)) - 1] == l_False) gap -= C.coef(i);
						else mincoef_idx = i;
					}
					if ( gap <= 0 ){
						while(before < vars_in_comp.size()){
							varseen_[vars_in_comp.last()] = SC_CANDIDATE;
							vars_in_comp.pop();
						}
						clseen_[c] = SC_NOT_CANDIDATE;
						for(int j = 0; j < i; j++) {
							if(var_frequency_[rs::toVar(C.lit(j)) - 1] > 0)
								var_frequency_[rs::toVar(C.lit(j)) - 1]--;
						}
						goto NEXT;
					}
					else {
						v2 = rs::toVar(C.lit(i)) - 1;
						if (varseen_[v2] == SC_NOT_CANDIDATE){
							continue;
						}
						assert(value(v2,assigns)==l_Undef);
						nlits++;
						var_frequency_[v2]++;
						if(varseen_[v2] == SC_CANDIDATE){
							varseen_[v2] = SC_SEEN;
							vars_in_comp.push(v2);
						}
					}
				}
				if (gap < C.coef(mincoef_idx)) gap = C.coef(mincoef_idx); // gap saturation
				if (gap < INT_MAX) {
					gaps_[c] = gap.convert_to<int>();
				} else {
					cout << "component manager gap exceeds int size" << endl;
					exit(1);
				}
				clseen_[c] = SC_SEEN;
				ncls_in_comp++;
			}
			NEXT:;
		}
	}

	nvars_in_comp = vars_in_comp.size();

}

template <class T_data>
Var ComponentManager<T_data>::pickBranchVar(const vec<double>& activity, const vec<double>& exscore)
{
	Component& c = topComponent();
	assert(dl_.back().SplitCompsEnd() == comp_stack_.size());

	assert(c.nVars() > 0);
	Var maxv = var_Undef;
	double max_score_a = -1;
	double max_score_f = -1;

	if(config.varSelectionHueristics == 0) {
		int p;
		for(p=0; isPVar(c[p]) && c[p] != var_Undef; p++) {
			double score_f = var_frequency_[c[p]] + exscore[c[p]];
			double score_a = activity[c[p]];
			if( score_f > max_score_f) {
				max_score_f = score_f;
				max_score_a = score_a;
				maxv = c[p];
			} else if ( score_f >= max_score_f * 0.90 && score_a > max_score_a * 1.5) {
				max_score_a = score_a;
				maxv = c[p];
			}
		}
	} else {
		double max_score_td = -1;
		int p;
		for(p=0; isPVar(c[p]) && c[p] != var_Undef; p++) {
			double score_td = exscore[c[p]];
			double score_f = var_frequency_[c[p]];
			double score_a = activity[c[p]];

			if(score_td > max_score_td) {
				max_score_td = score_td;
				max_score_f = score_f;
				max_score_a = score_a;
				maxv = c[p];
			}
			else if( score_td == max_score_td) {
				if(score_f > max_score_f) {
					max_score_f = score_f;
					max_score_a = score_a;
					maxv = c[p];
				} else if (score_f == max_score_f && score_a > max_score_a) {
					max_score_a = score_a;
					maxv = c[p];
				}
			}
		}
	}
	assert(maxv != var_Undef);
	return maxv;
}

// overloading to use activity of rs for pb
template <class T_data>
Var ComponentManager<T_data>::pickBranchVar(const vector<rs::ActValV>& activity, const vec<double>& exscore) {

	Component& c = topComponent();
	assert(dl_.back().SplitCompsEnd() == comp_stack_.size());

	assert(c.nVars() > 0);
	Var maxv = var_Undef;
	double max_score_a = -1;
	double max_score_f = -1;

	if(config.varSelectionHueristics == 0) {
		int p;
		for(p=0; isPVar(c[p]) && c[p] != var_Undef; p++) {
			double score_f = var_frequency_[c[p]] + exscore[c[p]];
			double score_a = (double) activity[c[p]];
			if( score_f > max_score_f) {
				max_score_f = score_f;
				max_score_a = score_a;
				maxv = c[p];
			} else if ( score_f >= max_score_f * 0.90 && score_a > max_score_a * 1.5) {
				max_score_a = score_a;
				maxv = c[p];
			}
		}
	} else {
		double max_score_td = -1;
		int p;
		for(p=0; isPVar(c[p]) && c[p] != var_Undef; p++) {
			double score_td = exscore[c[p]];
			double score_f = var_frequency_[c[p]];
			double score_a = (double) activity[c[p]];

			if(score_td > max_score_td) {
				max_score_td = score_td;
				max_score_f = score_f;
				max_score_a = score_a;
				maxv = c[p];
			}
			else if( score_td == max_score_td) {
				if(score_f > max_score_f) {
					max_score_f = score_f;
					max_score_a = score_a;
					maxv = c[p];
				} else if (score_f == max_score_f && score_a > max_score_a) {
					max_score_a = score_a;
					maxv = c[p];
				}
			}
		}
	}
	assert(maxv != var_Undef);
	return maxv;
}

template <class T_data>
void ComponentManager<T_data>::removeCachePollutions() {
	assert(topDecision().baseComp() == comp_stack_.size()-1);
	cache_.cleanAllDescendantsOf(comp_stack_.back()->id(), topDecision().numComps());
}

template <class T_data>
void ComponentManager<T_data>::setDecisionNode(Lit l)
{
	NodeIndex n = nodeMgr.newNode(DT_AND);
	nodeMgr.addEdge(n, nodeMgr.Literal(l));
	dl_.back().setNodeIdx(n);
}
template <class T_data>
void ComponentManager<T_data>::addNode(NodeIndex x, NodeIndex to)
{
	assert(x != nullptr && !nodeMgr.isTop(x));

	NodeIndex n = to;

	if(x == BOTTOM_NODE) {
		nodeMgr.deleteNodes(n);
		dl_.back().setNodeIdx(BOTTOM_NODE);
	}
	else {
		nodeMgr.addEdge(n, x);
	}
}
template <class T_data>
NodeIndex ComponentManager<T_data>::makeBranchNode(Var x)
{
	NodeIndex nodes[2];
	for(auto branch : {0, 1}) {
		NodeIndex n = nodes[branch] = dl_.back().getBranchNodeIdx(branch);

		if(n != BOTTOM_NODE) {
			DTNode& v = nodeMgr.Node(n);
			assert(v.Type() == DT_AND);
			if(v.Children().size() == 1) {
				// remove a trivial AND Node
				nodes[branch] = v.Children()[0];
				nodeMgr.delEdge(n, v.Children()[0]);
				nodeMgr.removeNode(n);
			}
		}
	}

	return nodeMgr.OR(x, nodes[0], nodes[1]);
}
template <class T_data>
void ComponentManager<T_data>::removeChildComponentNodes()
{
	NodeIndex node = dl_.back().getNodeIdx();
	vector<NodeIndex>& children = nodeMgr.Node(node).Children();
	for(int i=children.size()-1; i>=0; i--) {
		DT_NodeType t = nodeMgr.Node(children[i]).Type();
		if(t == DT_AND || t == DT_OR || t == DT_DC) {
			std::swap(children[i], children.back());
			nodeMgr.delEdge(node, children.back());
			if(nodeMgr.Node(children.back()).Parents().size() == 0) {
				nodeMgr.deleteNodes(children.back());
				children.pop_back();
			}
		}
	}
}

#ifndef LOW_PREC

template <class T_data>
int ComponentManager<T_data>::gap(const vec<lbool>& assigns, int clID)
{
	rs::CRef cref = constraints_[clID];
	const rs::Constr& C = pbca_[cref];

	rs::BigVal gap = C.degree();
	// rs::BigCoef mincoef = C.degree();
	for (size_t j = 0; j < C.size(); j++) {
		if (C.lit(j) > 0) {
			if (assigns[rs::toVar(C.lit(j)) - 1] == l_True) gap -= C.coef(j);
			// else if (mincoef > C.coef(j)) mincoef = C.coef(j);
		} else {
			if (assigns[rs::toVar(C.lit(j)) - 1] == l_False) gap -= C.coef(j);
			// else if (mincoef > C.coef(j)) mincoef = C.coef(j);
		}
	}
	assert(gap > 0);
	// if ( slack < mincoef ) slack = mincoef;
	int intgap;
	if (mpz_fits_sint_p(gap.backend().data())) {
		intgap = gap.convert_to<int>();
	} else {
		cout << "component manager gap exceeds int size" << endl;
		exit(1);
	}
	return intgap;
}
#else

template <class T_data>
int ComponentManager<T_data>::gap(const vec<lbool>& assigns, int clID)
{
	rs::CRef cref = constraints_[clID];
	const rs::Constr& C = pbca_[cref];

	rs::BigVal gap = C.degree();
	// rs::BigCoef mincoef = C.degree();
	for (size_t j = 0; j < C.size(); j++) {
		if (C.lit(j) > 0) {
			if (assigns[rs::toVar(C.lit(j)) - 1] == l_True) gap -= C.coef(j);
			// else if (mincoef > C.coef(j)) mincoef = C.coef(j);
		} else {
			if (assigns[rs::toVar(C.lit(j)) - 1] == l_False) gap -= C.coef(j);
			// else if (mincoef > C.coef(j)) mincoef = C.coef(j);
		}
	}
	assert(gap > 0);
	// if ( slack < mincoef ) slack = mincoef;
	int intgap;
	if (gap < INT_MAX) {
		intgap = gap.convert_to<int>();
	} else {
		cout << "component manager gap exceeds int size" << endl;
		exit(1);
	}
	return intgap;
}
#endif


template class GPMC::ComponentManager<mpz_class>;
template class GPMC::ComponentManager<mpfr::mpreal>;
