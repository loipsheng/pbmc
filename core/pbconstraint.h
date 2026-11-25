/*
Copyright (c) 2025 Grabtaxi Holdings Pte Ltd (GRAB), All rights reserved.

Use of this source code is governed by an MIT-style license that can be found in the LICENSE file
*/

#pragma once

#include <cstdint>
#include <algorithm>
#include <limits>
#include <vector>
#include <gmpxx.h>
#include <sstream>
#include <assert.h>

using std::vector;
// using Int = int_fast64_t;
// using Int = mpz_class; // alising for mpz_class
using Int = long;

struct PBconstraint {
	/* assuming always coefficients * lits >=  or == clauseConsVal */
	/* if ==, set equals bool to true */
	vector<Int> lits;
	vector<Int> coeffs;
	Int rhs;
	bool equals = false;
	/* adding an id just for debug print, to see which clauses are compiled in benchmarks*/
	Int constraintId = 0;
	// defining a get opb string representation method
	// std::string getOpbString();

	std::string getOpbString() {
		assert(lits.size() == coeffs.size());
		std::stringstream ss;
		Int cur_lit, cur_coeff;
		for (int i=0; i < lits.size(); i++) {
			cur_lit = lits[i];
			cur_coeff = coeffs[i];
			if (cur_coeff > 0) {
				// +ve coefficient
				if (i > 0) ss << "+";
				ss << cur_coeff << " ";
			} else {
				// -ve coefficient
				ss << cur_coeff << " ";
			}

			if (cur_lit < 0) {
				ss << "~x" << abs(cur_lit) << " ";
			} else {
				ss << "x" << abs(cur_lit) << " ";
			}
		}
		if(equals) {
			ss << "= ";
		} else {
			ss << ">= ";
		}
		ss << rhs << " ;";
		return ss.str();
	}

	void sortDescendingCoeff() {
		vector<std::pair<Int, Int>> lit_coeff_pair_vec;
		lit_coeff_pair_vec.clear();
		assert(lits.size() == coeffs.size());
		for (int i = 0; i < lits.size(); i++) {
			lit_coeff_pair_vec.push_back(std::make_pair(lits.at(i), coeffs.at(i)));
		}
		std::sort(
			lit_coeff_pair_vec.begin(),
			lit_coeff_pair_vec.end(),
			[&](const auto& a, const auto& b){
				return a.second > b.second;
			}
		);
		assert(lit_coeff_pair_vec.size() == lits.size());
		for (int i = 0; i < lit_coeff_pair_vec.size(); i++) {
			lits[i] = lit_coeff_pair_vec.at(i).first;
			coeffs[i] = lit_coeff_pair_vec.at(i).second;
		}
		return;
	}
};

using SourceRowID = uint64_t;
using EqualityGroupID = uint64_t;
using RowVersionID = uint64_t;

constexpr EqualityGroupID NO_EQUALITY_GROUP = 0;
constexpr RowVersionID NO_ROW_VERSION = 0;
constexpr uint64_t NO_SOLVER_ID = std::numeric_limits<uint64_t>::max();

enum class RowSide : uint8_t { SINGLE, LOWER, UPPER };
enum class RowState : uint8_t { ATTACHED, ROOT_TRUE };

struct RowInfo {
	SourceRowID source = 0;
	EqualityGroupID group = NO_EQUALITY_GROUP;
	RowSide side = RowSide::SINGLE;
	RowVersionID version = NO_ROW_VERSION;
	RowState state = RowState::ROOT_TRUE;
	uint64_t solver_id = NO_SOLVER_ID;
	PBconstraint content;
};

struct EqGroup {
	EqualityGroupID id = NO_EQUALITY_GROUP;
	SourceRowID source = 0;
	RowVersionID lower = NO_ROW_VERSION;
	RowVersionID upper = NO_ROW_VERSION;
	uint64_t revision = 0;
};

inline bool validGroup(const EqGroup& group, const vector<RowInfo>& rows) {
	if (group.id == NO_EQUALITY_GROUP || group.source == 0 ||
			group.lower == NO_ROW_VERSION || group.upper == NO_ROW_VERSION ||
			group.lower == group.upper)
		return false;

	const RowInfo* lower = nullptr;
	const RowInfo* upper = nullptr;
	size_t group_rows = 0;
	for (const RowInfo& row : rows) {
		if (row.version == group.lower) lower = &row;
		if (row.version == group.upper) upper = &row;
		if (row.group == group.id) group_rows++;
	}
	if (lower == nullptr || upper == nullptr || group_rows != 2)
		return false;
	if (lower->source != group.source || upper->source != group.source ||
			lower->group != group.id || upper->group != group.id ||
			lower->side != RowSide::LOWER || upper->side != RowSide::UPPER)
		return false;

	for (const RowInfo* row : {lower, upper}) {
		if (row->state == RowState::ATTACHED) {
			if (row->solver_id == NO_SOLVER_ID) return false;
		} else if (row->state == RowState::ROOT_TRUE) {
			if (row->solver_id != NO_SOLVER_ID) return false;
		} else {
			return false;
		}
	}
	return lower->state != RowState::ATTACHED ||
			upper->state != RowState::ATTACHED ||
			lower->solver_id != upper->solver_id;
}
