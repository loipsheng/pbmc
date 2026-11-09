/*
Copyright (c) 2025 Grabtaxi Holdings Pte Ltd (GRAB), All rights reserved.

Use of this source code is governed by an MIT-style license that can be found in the LICENSE file
*/

#pragma once

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