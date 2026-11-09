/*
Copyright (c) 2025 Grabtaxi Holdings Pte Ltd (GRAB), All rights reserved.

Use of this source code is governed by an MIT-style license that can be found in the LICENSE file
*/

#include "../core/Instance.h"

#include "mtl/Sort.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>

using namespace Glucose;
using namespace GPMC;
using namespace std;

template <class T_data>
Instance<T_data>::Instance() :
vars(0),
weighted(false),
projected(false),
npvars(0),
freevars(0),
gweight(1),
keepVarMap(false),
unsat(false)
{}

static inline Lit SignedIntToLit(int signed_int) {
	assert(signed_int != 0);
	int v = abs(signed_int)-1;
	return (signed_int > 0) ? mkLit(v) : ~mkLit(v);
}

static void Tokens(string buf, vector<string>& ret) {
	ret.push_back("");
	for (char c : buf) {
		if (std::isspace(c)) {
			if (!ret.back().empty()) {
				ret.push_back("");
			}
		} else {
			ret.back() += c;
		}
	}
	if (ret.back().empty()) ret.pop_back();
}

template <class T_data>
void Instance<T_data>::load(istream& in, bool weighted, bool projected, bool keepVarMap) {
	this->weighted = weighted;
	this->projected = projected;
	this->keepVarMap = keepVarMap;

	string buf;
	vector<Lit> ps;
	vector<string> tokens;
	bool parse_error = false;
	bool typeSpecified = false;
	npvars = 0;
	ispvars.clear();

	while (getline(in, buf)) {
		if (buf.empty()) continue;
		tokens.clear();
		Tokens(buf, tokens);

		if(tokens.size() == 0) continue;
		else if (tokens[0] == "p") {
			if (tokens.size() == 4 && tokens[1] == "cnf") {
				vars = stoi(tokens[2]);
				int ncls = stoi(tokens[3]);
				assigns.resize(vars, l_Undef);
				assignedLits.clear();

				if(projected) {
					if(ispvars.size() < vars) {
						ispvars.resize(vars, false);
					}
				} else {
					ispvars.resize(vars, true);
					npvars = vars;
				}

				if(weighted)
					lit_weights.resize(2*vars, 1);
			}
			else
				cerr << "c c Header Error!" << endl;
		}
		else if (tokens[0][0] == 'c') {
			if(tokens[0] == "c" && tokens[1] == "p") {
				if(tokens[2] == "weight") {	// Read the weight of a literal
					if(weighted && tokens.size() == 6 && tokens.back() == "0") {
						int lit = stoi(tokens[3]);
						Lit l = SignedIntToLit(lit);
						lit_weights[toInt(l)] = tokens[4];
					}
				}
				else if(tokens[2] == "show") { // Read the list of projected vars
					if(projected && tokens.back() == "0") {
						for(int i=3; i<tokens.size()-1; i++) {
							int v = stoi(tokens[i]);
							if(v-1 >= ispvars.size())
								ispvars.resize(v, false);
							ispvars[v-1] = true;
							if(keepVarMap)
								pvars.push_back(v-1);
							npvars++;
						}
					}
				}
				else if(tokens[2] == "gweight") {
					if(weighted && tokens.size() == 5 && tokens.back() == "0") {
						gweight = tokens[3];
					}
				}
			}
			else if(tokens[0] == "cr") {
				if(projected && tokens.back() == "0") {
					for(int i=1; i<tokens.size()-1; i++) {
						int v = stoi(tokens[i]);
						if(v-1 >= ispvars.size())
							ispvars.resize(v, false);
						ispvars[v-1] = true;
						if(keepVarMap)
							pvars.push_back(v-1);
						npvars++;
					}
				}
			}
		}
		else { // Read clause
			int lit = 0;
			assert(tokens.size() > 0);
			for(auto token : tokens) {
				lit = stoi(token);
				if(lit == 0) break;
				ps.push_back(SignedIntToLit(lit));
			}
			if(lit == 0) {
				if(!addClause(ps)) {
					return;
				}
				ps.clear();
			} else {
				cerr << "c c invalid Clause!" << endl;
			}
		}
	}

	learnts.clear();

	if(keepVarMap) {
		gmap.clear();
		if(projected) {
			for(auto v : pvars)
				gmap.push_back(mkLit(v));
		} else {
			for(int i=0; i<npvars; i++)
				gmap.push_back(mkLit(i));
		}
	}
}

template <class T_data>
bool Instance<T_data>::addClause(vector<Lit>& ps, bool learnt) {
	sort(ps.begin(), ps.end());

	vec<Lit> oc;
	oc.clear();

	Lit p; int i, j, flag = 0;

	for (i = j = 0, p = lit_Undef; i < ps.size(); i++)
		if (value(ps[i]) == l_True || ps[i] == ~p)
			return true;
		else if (value(ps[i]) != l_False && ps[i] != p)
			ps[j++] = p = ps[i];
	ps.resize(j);

	if (ps.size() == 0) {
		unsat = true;
		return false;
	}
	else if (ps.size() == 1) {
		assigns[var(ps[0])] = lbool(!sign(p));
		assignedLits.push_back(ps[0]);
		// No check by UP here. Preprocessor will do later.
	}
	else {
		clauses.push_back({});
		copy(ps.begin(), ps.end(), back_inserter(clauses.back()));
	}
	return true;
}

template <class T_data>
void Instance<T_data>::printVarMapStats() const {
	cout << "c o #PVars(Original)   " << gmap.size() << endl;
	cout << "c o #PVars(Simplified) " << npvars << endl;
	cout << "c o #Fixed_Literals    " << fixedLits.size() << endl;
	cout << "c o #FreeLitClasses    " << freeLitClasses.size() << endl;
	cout << "c o #DefinedVars       " << definedVars.size() << endl;
}

template <class T_data>
void Instance<T_data>::writeVarMap(std::ostream& out) {
	using namespace std;

	// c p vmap orignal_vars simplified_vars
	out << "p vmap " << gmap.size() << " " << npvars << " " << fixedLits.size() << " " << freeLitClasses.size() << " " << definedVars.size() << endl;

	// fixed literals:
	//  c fix l1 .... ln 0
	if(fixedLits.size() > 0) {
		out << "a ";
		if(projected) {
			for(Lit l : fixedLits)
				out << (sign(l)?"-":"") << (pvars[var(l)]+1) << " ";
		}
		else {
			for(Lit l : fixedLits)
				out << (sign(l)?"-":"") << (var(l)+1) << " ";
		}

		out << "0" << endl;
	}

	// free literals classes
	//	f l1 .... ln 0
	if(freeLitClasses.size() > 0) {
		if(projected) {
			for(auto flc : freeLitClasses) {
				out << "f ";
				for(Lit l : flc)
					out << (sign(l)?"-":"") << (pvars[var(l)]+1) << " ";
				out << "0" << endl;
			}
		} else {
			for(auto flc : freeLitClasses) {
				out << "f ";
				for(Lit l : flc)
					out << (sign(l)?"-":"") << (var(l)+1) << " ";
				out << "0" << endl;
			}
		}
	}

	// determined vars
	//	c det v1 ... vn 0
	if(definedVars.size() > 0) {
		out << "d ";
		if(projected) {
			for(Var v : definedVars)
				out << (pvars[v]+1) << " ";
		}
		else {
			for(Var v : definedVars)
				out << (v+1) << " ";
		}
		out << "0" << endl;
	}

	// var-to-lit renaming:
	//	v orig_var simplified_lit
	if(projected) {
		for(int i=0; i<gmap.size(); i++) {
			if(gmap[i] != lit_Undef)
				out << "v " << (pvars[i]+1) << " " << (sign(gmap[i])?"-":"") << (var(gmap[i])+1) << endl;
		}
	}
	else {
		for(int i=0; i<gmap.size(); i++)
			if(gmap[i] != lit_Undef)
				out << "v " << (i+1) << " " << (sign(gmap[i])?"-":"") << (var(gmap[i])+1) << endl;;
	}
}

template <typename T_data>
void Instance<T_data>::importVarScore(std::istream& in) {
	score.clear();
	score.resize(npvars, 0);

	vector<Glucose::Var> rvmap;
	if(projected) {
		rvmap.clear();
		rvmap.resize(vars, -1);
		for(int i=0; i<pvars.size(); i++)
			rvmap[pvars[i]] = i;
	}

	// parse
	vector<string> tokens;
	string buf;
	int var;
	while (getline(in, buf)) {
		if (buf.empty()) continue;
		tokens.clear();
		Tokens(buf, tokens);

		if(tokens.size() == 0) continue;
		if(tokens.size() != 2) {
 			cerr << "c c Parse Error! Invalid line in the given varscore file!" << endl;
 			exit(1);
		}
		else {
			int var = stoi(tokens[0])-1;
			double s = stod(tokens[1]);

			assert(var <= vars);
			if(0 <= var && var > vars) {
				cerr << "c c Parse Error! Invalid line in the given varscore file!" << endl;
				exit(1);
			}
			if(projected)
				score[rvmap[var]] = s;
			else
				score[var] = s;
		}
	}
}

// For Debug
template <class T_data>
 void Instance<T_data>::toDimacs(std::ostream& out)
 {
	using namespace std;

	out << "c free vars: " << freevars << endl;
	out << "p cnf " << vars + freevars << " " << clauses.size() << endl;

	if(!weighted && !projected)
		out << "c t mc" << endl;
	else if(weighted && !projected)
		out << "c t wmc" << endl;
	else if(!weighted && projected)
		out << "c t pmc" << endl;
	else if(weighted && projected)
		out << "c t wpmc" << endl;

	if(projected) {
		out << "c p show ";
		for (int i=0; i<npvars; i++)
			out << (i+1) << " ";
		out << "0" << endl;
	}
	if(weighted) {
		int precision = mpfr::bits2digits(mpfr::mpreal::get_default_prec());
		out.precision(precision);

		for (int i=0; i<npvars; i++) {
			out << "c p weight " << (i+1) << " " << lit_weights[toInt(mkLit(i))] << " 0" << endl;
			out << "c p weight -" << (i+1) << " " << lit_weights[toInt(~mkLit(i))] << " 0" << endl;
		}

		if(gweight != 1) {
			out << "c p gweight " << gweight << " 0" << endl;
		}
	}

	for(auto c : clauses) {
		for (auto l : c)
			out << (sign(l) ? "-":"") << (var(l)+1) << " ";
		out << "0" << endl;
	}

	// For Debug
	/*
	for(auto c : learnts) {
		for (auto l : c)
			out << (sign(l) ? "-":"") << (var(l)+1) << " ";
		out << "0" << endl;
	}
	*/
}

template class GPMC::Instance<mpz_class>;
template class GPMC::Instance<mpfr::mpreal>;


// PBInstance below

template <class T_data>
PBInstance<T_data>::PBInstance() :
vars(0),
weighted(false),
projected(false),
npvars(0),
freevars(0),
gweight(1),
keepVarMap(false),
unsat(false)
{}

// static inline Lit SignedIntToLit(int signed_int) {
// 	assert(signed_int != 0);
// 	int v = abs(signed_int)-1;
// 	return (signed_int > 0) ? mkLit(v) : ~mkLit(v);
// }

// static void Tokens(string buf, vector<string>& ret) {
// 	ret.push_back("");
// 	for (char c : buf) {
// 		if (std::isspace(c)) {
// 			if (!ret.back().empty()) {
// 				ret.push_back("");
// 			}
// 		} else {
// 			ret.back() += c;
// 		}
// 	}
// 	if (ret.back().empty()) ret.pop_back();
// }

template <class T_data>
void PBInstance<T_data>::load(istream& in, bool weighted, bool projected, bool keepVarMap) {
	this->weighted = weighted;
	this->projected = projected;
	this->keepVarMap = keepVarMap;

	std::string buf;
	vector<Glucose::Lit> ps;
	vector<std::string> tokens;
	bool parse_error = false;
	bool typeSpecified = false;
	npvars = 0;
	int ncls = 0;
	ispvars.clear();

	// //debug print
	// cout << "Input line prints" << endl;
	// //end debug print
	while (getline(in, buf)) {
		if (buf.empty()) continue;
		// cout << buf << endl;
		tokens.clear();
		Tokens(buf, tokens);


		if(tokens.size() == 0) {
			continue;
		} else if (tokens[0] == "*") {
			if (tokens.size() <= 2) continue;
			if (tokens[1] == "#variable=") {
				if (tokens.size() != 5) { 
					// header issue
					cerr << "c c Header Error!" << endl;
				}
				assert(tokens[3] == "#constraint=");
				vars = stoi(tokens[2]); // num var
				ncls = stoi(tokens[4]); // num clause
				ispvars.resize(vars, false); // init all var to no projection set
				if(weighted) {
					lit_weights.resize(2*vars, 1);
				}
			} else if ( (tokens[1] == "ind" && tokens.back() == "0") || (tokens[1] == "p" && tokens[2] == "show" && tokens.back() == "0") ) {
				// projection set line
					if(projected) {
						int startIndex = -1;
						if (tokens[1] == "ind") {
							startIndex = 2;
						} else {
							// * p show v1 v2 case
							startIndex = 3;
						}
						for(int i=startIndex; i<tokens.size()-1; i++) {
							int v = stoi(tokens[i]);
							// this v-1 is how the variable offset happens, input var x1 will have internal index variable 0
							assert(v <= ispvars.size()); // should not have to resize any more
							if(v-1 >= ispvars.size()) {
								std::cout << "ispvars resizing, probably inconsistency in opb header declared vars vs in formula" << std::endl;
								ispvars.resize(v, false);
							}
							ispvars[v-1] = true;
							// should push to pvars either way because we need for renum.
							// if(keepVarMap)
							// 	pvars.push_back(v-1);
							pvars.push_back(v-1);
							npvars++;
						}
					} else {
						// non projected mode so ignore
						continue;
					}
			} else if (tokens[1] == "p" && tokens[2] == "weight") {
				// weight line
				if(weighted && tokens.size() == 6 && (tokens.back() == "0" || tokens.back() == ";")) {
					int lit = stoi(tokens[3]);
					// Note: using glucose lit as index for lit_weights
					Lit l = SignedIntToLit(lit);
					lit_weights[toInt(l)] = T_data(tokens[4]);
				}
			} else if (tokens[1] == "p" && tokens[2] == "gweight") {
				// global weight line
				if(weighted && tokens.size() == 5 && (tokens.back() == "0" || tokens.back() == ";")) {
					gweight = tokens[3];
				}
			} else {
				// comment line, ignore
				continue;
			}
		} else { // Read clause/constraint
			bool prevIsCoeff = false;
			std::string currentToken;
			PBconstraint currentConstraint;
			int lit = 0;
			assert(tokens.size() > 0);

			bool isEndTokenSpaced = false;
			if (tokens.back() == ";") {
				isEndTokenSpaced = true;
				tokens.pop_back(); // removing ";" at end of token vector
			}
			int numTokens = tokens.size();
			for (int i = 0; i < numTokens; i++) {
				currentToken = tokens.at(i);
				if (i == numTokens - 1 ) {
					// last token
					// removing line end ';' and parsing as Int (int64) only if ";" is not spaced
					if (isEndTokenSpaced) {
						currentConstraint.rhs = std::stol(currentToken);
					} else {
						currentConstraint.rhs = std::stol(currentToken.substr(0, currentToken.size() - 1));
					}
				} else if (i == numTokens - 2) {
					// parsing constraint operator
					// second last word, should be the constraint comparator = or >=
					if (tokens.at(i) == "=") {
						currentConstraint.equals = true;
					} else {
						currentConstraint.equals = false;
					}
				} else {
					if(currentToken.at(0) == 'x') {
						//literal
						if (!prevIsCoeff) {
							currentConstraint.coeffs.push_back(1);
						}
						lit = std::stol(currentToken.substr(1, currentToken.size() - 1));
						currentConstraint.lits.push_back(lit);
						if (lit == 0) {
							cerr << "c c invalid constraint, literal 0 parsed" << endl;
							assert(lit != 0);
						}
						prevIsCoeff = false;
					} else if (currentToken.at(0) == '~') {
						// negated literal
						if (!prevIsCoeff) {
							currentConstraint.coeffs.push_back(1);
						}
						lit = -1 * std::stol(currentToken.substr(2, currentToken.size() - 2));
						if (lit == 0) {
							cerr << "c c invalid constraint, literal 0 parsed" << endl;
							assert(lit != 0);
						}
						currentConstraint.lits.push_back(lit);
						prevIsCoeff = false;
					} else {
						// coefficient
						currentConstraint.coeffs.push_back(std::stol(currentToken));
						prevIsCoeff = true;
					}
				}
			}
			currentConstraint.constraintId = clauses.size() + 1; // 1 indexed id for constraint parsing
			// for(auto token : tokens) {
			// 	lit = stoi(token);
			// 	if(lit == 0) break;
			// 	ps.push_back(SignedIntToLit(lit));
			// }
			if(!addClause(currentConstraint, false)) {
				cerr << "c c invalid constraint!" << endl;
				return;
			}
			// if(lit == 0) {
			// 	if(!addClause(ps)) {
			// 		return;
			// 	}
			// 	ps.clear();
			// } else {
			// 	cerr << "c c invalid Clause!" << endl;
			// }
		}
	}

	if(!projected) {
		// if not projected, every variable is in projection set
		// vars is the header specified num variables
		ispvars.clear();
		ispvars.resize(vars, true);
		npvars = vars;
		for (int i = 0; i < npvars; i++) { //var is int type
			pvars.push_back(i);
		}
	}

	learnts.clear();

	if(keepVarMap) {
		gmap.clear();
		if(projected) {
			for(auto v : pvars)
				gmap.push_back(mkLit(v));
		} else {
			for(int i=0; i<npvars; i++)
				gmap.push_back(mkLit(i));
		}
	}
}

template <class T_data>
bool PBInstance<T_data>::addClause(PBconstraint& parsedConstraint, bool learnt) {
	// consider doing more checks here?
	if (parsedConstraint.lits.size() == 0) {
		// no literals, unsat
		unsat = true;
		return false;
	}
	// do we want to do something with unit constraints?
	// do not really need to sort the parsedConstraint since this is temporary and will get sorted in constraint allocator later on
	clauses.push_back(parsedConstraint);
	return true;
}

// template <class T_data>
// bool PBInstance<T_data>::addClause(vector<Lit>& ps, bool learnt) {
// 	sort(ps.begin(), ps.end());

// 	vec<Lit> oc;
// 	oc.clear();

// 	Lit p; int i, j, flag = 0;

// 	for (i = j = 0, p = lit_Undef; i < ps.size(); i++)
// 		if (value(ps[i]) == l_True || ps[i] == ~p)
// 			return true;
// 		else if (value(ps[i]) != l_False && ps[i] != p)
// 			ps[j++] = p = ps[i];
// 	ps.resize(j);

// 	if (ps.size() == 0) {
// 		unsat = true;
// 		return false;
// 	}
// 	else if (ps.size() == 1) {
// 		assigns[var(ps[0])] = lbool(!sign(p));
// 		assignedLits.push_back(ps[0]);
// 		// No check by UP here. Preprocessor will do later.
// 	}
// 	else {
// 		clauses.push_back({});
// 		copy(ps.begin(), ps.end(), back_inserter(clauses.back()));
// 	}
// 	return true;
// }

template <class T_data>
void PBInstance<T_data>::printVarMapStats() const {
	cout << "c o #PVars(Original)   " << gmap.size() << endl;
	cout << "c o #PVars(Simplified) " << npvars << endl;
	cout << "c o #Fixed_Literals    " << fixedLits.size() << endl;
	cout << "c o #FreeLitClasses    " << freeLitClasses.size() << endl;
	cout << "c o #DefinedVars       " << definedVars.size() << endl;
}

template <class T_data>
void PBInstance<T_data>::writeVarMap(std::ostream& out) {
	using namespace std;
	return;
	//TODO: implement if needed

	// // c p vmap orignal_vars simplified_vars
	// out << "p vmap " << gmap.size() << " " << npvars << " " << fixedLits.size() << " " << freeLitClasses.size() << " " << definedVars.size() << endl;

	// // fixed literals:
	// //  c fix l1 .... ln 0
	// if(fixedLits.size() > 0) {
	// 	out << "a ";
	// 	if(projected) {
	// 		for(Lit l : fixedLits)
	// 			out << (sign(l)?"-":"") << (pvars[var(l)]+1) << " ";
	// 	}
	// 	else {
	// 		for(Lit l : fixedLits)
	// 			out << (sign(l)?"-":"") << (var(l)+1) << " ";
	// 	}

	// 	out << "0" << endl;
	// }

	// // free literals classes
	// //	f l1 .... ln 0
	// if(freeLitClasses.size() > 0) {
	// 	if(projected) {
	// 		for(auto flc : freeLitClasses) {
	// 			out << "f ";
	// 			for(Lit l : flc)
	// 				out << (sign(l)?"-":"") << (pvars[var(l)]+1) << " ";
	// 			out << "0" << endl;
	// 		}
	// 	} else {
	// 		for(auto flc : freeLitClasses) {
	// 			out << "f ";
	// 			for(Lit l : flc)
	// 				out << (sign(l)?"-":"") << (var(l)+1) << " ";
	// 			out << "0" << endl;
	// 		}
	// 	}
	// }

	// // determined vars
	// //	c det v1 ... vn 0
	// if(definedVars.size() > 0) {
	// 	out << "d ";
	// 	if(projected) {
	// 		for(Var v : definedVars)
	// 			out << (pvars[v]+1) << " ";
	// 	}
	// 	else {
	// 		for(Var v : definedVars)
	// 			out << (v+1) << " ";
	// 	}
	// 	out << "0" << endl;
	// }

	// // var-to-lit renaming:
	// //	v orig_var simplified_lit
	// if(projected) {
	// 	for(int i=0; i<gmap.size(); i++) {
	// 		if(gmap[i] != lit_Undef)
	// 			out << "v " << (pvars[i]+1) << " " << (sign(gmap[i])?"-":"") << (var(gmap[i])+1) << endl;
	// 	}
	// }
	// else {
	// 	for(int i=0; i<gmap.size(); i++)
	// 		if(gmap[i] != lit_Undef)
	// 			out << "v " << (i+1) << " " << (sign(gmap[i])?"-":"") << (var(gmap[i])+1) << endl;;
	// }
}

template <typename T_data>
void PBInstance<T_data>::importVarScore(std::istream& in) {
	score.clear();
	score.resize(npvars, 0);

	vector<Glucose::Var> rvmap;
	if(projected) {
		rvmap.clear();
		rvmap.resize(vars, -1);
		for(int i=0; i<pvars.size(); i++)
			rvmap[pvars[i]] = i;
	}

	// parse
	vector<string> tokens;
	string buf;
	int var;
	while (getline(in, buf)) {
		if (buf.empty()) continue;
		tokens.clear();
		Tokens(buf, tokens);

		if(tokens.size() == 0) continue;
		if(tokens.size() != 2) {
 			cerr << "c c Parse Error! Invalid line in the given varscore file!" << endl;
 			exit(1);
		}
		else {
			int var = stoi(tokens[0])-1;
			double s = stod(tokens[1]);

			assert(var <= vars);
			if(0 <= var && var > vars) {
				cerr << "c c Parse Error! Invalid line in the given varscore file!" << endl;
				exit(1);
			}
			if(projected)
				score[rvmap[var]] = s;
			else
				score[var] = s;
		}
	}
}

template <class T_data>
std::stringstream PBInstance<T_data>::getRenumberedOpb() {
	std::stringstream ss; // stream to print to
	// printing opb header
	// * #variable= 30 #constraint= 1
	ss << "* #variable= " << std::to_string(vars) << " #constraint= " << std::to_string(renumberedClauses.size()) << std::endl;
	// printing renumbered projection variable set
	if (projected) {
		ss << "* p show ";
		for (int i=1; i <= npvars; i++) {
			ss << std::to_string(i) << " ";
		}
		ss << "0" << std::endl;
	}
	// printing seperator
	ss << "*" << std::endl;
	// printing each renumbered constraint
	for (PBconstraint& constraint : renumberedClauses) {
		ss << constraint.getOpbString() << std::endl;
	}
	// still need to print the inferred variable assignments from preprocessing as unit constraint
	if (!inferredAssignment.empty()) {
		for (auto& element : inferredAssignment) {
			// var (1 indexed), bool
			if(element.first <= 0) {
				std::cerr << "Instance inferredAssignment var is negative, should not happen" << std::endl;
				std::exit(1);
			}
			if (element.second) {
				// var to true
				ss << "1 x" << element.first << " >= 1 ;" << std::endl;
			} else {
				// var to false
				ss << "1 ~x" << element.first << " >= 1 ;" << std::endl;
			}
		}
	}

	return ss;
}

template <class T_data>
PBconstraint PBInstance<T_data>::getRenumConstraint(PBconstraint& origConstraint, std::unordered_map<int, int> oldToNewVarMap) {
	PBconstraint renumConstraint;
	renumConstraint.constraintId = origConstraint.constraintId;
	renumConstraint.rhs = origConstraint.rhs;
	renumConstraint.equals = origConstraint.equals;
	for (auto coeff : origConstraint.coeffs) {
		renumConstraint.coeffs.push_back(coeff);
	}
	for (int i=0; i < origConstraint.lits.size(); i++) {
		if (origConstraint.lits[i] > 0) {
			renumConstraint.lits.push_back(oldToNewVarMap.at(origConstraint.lits[i]));
		} else {
			renumConstraint.lits.push_back(-oldToNewVarMap.at(abs(origConstraint.lits[i])));
		}
	}
	return renumConstraint;
}

template <class T_data>
void PBInstance<T_data>::renumberVar() {
	std::unordered_map<int, int> origToNewVarMap; // can do this because var is defined as int
	assert(npvars == pvars.size());
	int pcount = 1;
	int nonpcount = npvars + 1;
	for (int i = 0; i < ispvars.size(); i++) {
		if (ispvars[i]) {
			// ispvars 0 maps to x1 originally as offset by 1
			origToNewVarMap[i+1] = pcount;
			pcount++;
		} else {
			origToNewVarMap[i+1] = nonpcount;
			nonpcount++;
		}
	}
	// the above renumbers to 1 indexed variables, i.e. x1, x2... with projection set first
	// npvars should be last of projection set instead of start of nonprojection set.

	// now we have the map, we renumber the constraints as well
	for (auto& constraint : clauses) {
		renumberedClauses.push_back(getRenumConstraint(constraint, origToNewVarMap));
	}
	// check if we want to do anything else, literal weights?
	// handling literal weights
	// creating a new vector of renumbered weights
	renumbered_lit_weights.resize(lit_weights.size(), 1);
	int k = -1;
	int v = -1;
	if (this->weighted) {
		for (auto& kv : origToNewVarMap) {
			k = kv.first - 1;
			v = kv.second - 1;
			assert(k >= 0 && v >= 0);
			renumbered_lit_weights[toInt(mkLit(v))] = lit_weights[toInt(mkLit(k))];
			renumbered_lit_weights[toInt(~mkLit(v))] = lit_weights[toInt(~mkLit(k))];
		}
	}

	// need to modify ispvars and pvars vector
	ispvars.clear();
	ispvars.resize(pcount, true); // push all projected var = true
	ispvars.resize(nonpcount + pcount, false); // push all remaining var = false
	pvars.clear();
	for (int i = 0; i < pcount; i++) {
		pvars.push_back(i);
	}

	// setting flag for debug mode, to ensure make positive is called after and not before
	renumbered_variables = true;
	return;
}

template <class T_data>
void PBInstance<T_data>::computeVarCoeffScore(double maxCoeffScore, bool alternating) {
	/* NOTE: Only to call after renumbering variables */

	varCoeffScores.clear();
	phase.clear(); // also going to set phase here
	phase.resize(vars, -1);
	std::vector<std::vector<Int>> varCoeffScorePhase(vars);
	
	std::vector<double> varScoreVec(vars, 0);
	// varCoeffScores.resize(vars, 0);
	// create vector of vector to store coeff score for each var per constraint
	std::vector<std::vector<double>> varCoeffScoreVec(vars);
	std::vector<double> alternatingScoreVec;

	for (PBconstraint& C : renumberedClauses) {
		assert(C.lits.size() == C.coeffs.size());
		for (int i = 0; i < C.lits.size(); i++) {
			double cvarScore = (double) std::abs(C.coeffs[i]) / (double) std::abs(C.rhs);
			cvarScore = std::min(1.0, cvarScore); // ceil at 1.0
			// varCoeffScoreVec[std::abs(C.lits[i])].push_back(cvarScore);
			varCoeffScoreVec.at(std::abs(C.lits[i]) -1).push_back(cvarScore); //offset by 1 for variable 0
			varCoeffScorePhase.at(std::abs(C.lits[i]) -1).push_back(C.lits[i]);
		}
	}

	// take avg of each internal vector and multiply by scaling factor and round
	double sum;
	double avg;
	double max; // deciding between avg and max to use
	double current_score;
	double max_varscore = 0;
	for (int i = 0; i < vars; i++ ) {
		sum = 0;
		avg = 0;
		max = 0;
		current_score = 0;
		for (int j=0; j < varCoeffScoreVec[i].size(); j++){
			double element = varCoeffScoreVec[i][j];
			sum += element;
			if (element > max) {
				max = element;
				if (varCoeffScorePhase[i][j] > 0) {
					phase[i] = 1;
				} else {
					phase[i] = -1;
				}
			}
		}
		// for (double element : varCoeffScoreVec[i]) {
		// 	sum += element;
		// 	if (element > max) {
		// 		max = element;
		// 	}
		// }
		if (!varCoeffScoreVec.empty()) {
			avg = sum / (double) varCoeffScoreVec.size();
		}
		// decide if score should be avg or max
		current_score = avg;
		// current_score = max;
		// varCoeffScores[i] = (int) std::round(avg * maxCoeffScore);
		varScoreVec[i] = current_score;
		if (current_score > max_varscore) {
			max_varscore = current_score;
		}
	}
	// normalize variable scores
	if (max_varscore > 0) {
		for (int i = 0; i < varScoreVec.size(); i++) {
			varScoreVec[i] = varScoreVec[i] / max_varscore;
		}
	}
	// if alternating, then we alternate the variables Highest, Lowest, Highest -1 etc.
	if (alternating) {
		std::vector<std::pair<double, rs::Var>> temp_score_variable_vector;
		for (int v=0; v < varScoreVec.size() ; v++) { // 1 indexed variables
			temp_score_variable_vector.push_back(std::make_pair(varScoreVec[v], v)); // exscore is 0 indexed
		}
		// sorting in ascending order of score, so to pop back first
		std::sort(
			temp_score_variable_vector.begin(),
			temp_score_variable_vector.end(),
			[&](const auto& a, const auto& b){
				return a.first < b.first;
			}
		);
		int start_idx = 0;
		int end_idx = temp_score_variable_vector.size() - 1;
		bool insertStart = false;
		alternatingScoreVec.clear();
		while (alternatingScoreVec.size() < temp_score_variable_vector.size()) {
			if (insertStart) {
				alternatingScoreVec.push_back(temp_score_variable_vector[start_idx].first);
				start_idx++;
			} else {
				alternatingScoreVec.push_back(temp_score_variable_vector[end_idx].first);
				end_idx--;
			}
			insertStart = !insertStart;
		}
		assert(alternatingScoreVec.size() == temp_score_variable_vector.size());
		for (int i = 0; i < alternatingScoreVec.size(); i++){
			varScoreVec[temp_score_variable_vector[i].second] = alternatingScoreVec[i];
		}
	}
	// store in map
	for (int i = 0; i < vars; i++) {
		// varCoeffScores[i] = (int) std::round(varScoreVec[i] * maxCoeffScore);
		varCoeffScores[i] = varScoreVec[i] * maxCoeffScore;
	}	
}

template <class T_data>
void PBInstance<T_data>::makeRenumberedCoeffPositive() {
	assert(renumbered_variables);
	// Note: call after renumberVar
	for (int i = 0; i < renumberedClauses.size(); i++) {
		// make each constraint's coefficients positive
		renumberedClauses[i] = getPositiveCoeffRenumberedConstraint(renumberedClauses.at(i));
	}
	return;
}

template <class T_data>
PBconstraint PBInstance<T_data>::getPositiveCoeffRenumberedConstraint(PBconstraint& origConstraint) {
	PBconstraint allPositiveConstraint;
	allPositiveConstraint.constraintId = origConstraint.constraintId;
	allPositiveConstraint.rhs = origConstraint.rhs;
	allPositiveConstraint.equals = origConstraint.equals;
	for (int i = 0; i < origConstraint.coeffs.size(); i++) {
		assert(origConstraint.coeffs[i] != 0);
		if (origConstraint.coeffs[i] < 0) {
			// if coeff -ve, then flip coeff, adjust literal and rhs
			allPositiveConstraint.rhs -= origConstraint.coeffs[i];
			allPositiveConstraint.lits.push_back(-origConstraint.lits[i]);
			allPositiveConstraint.coeffs.push_back(-origConstraint.coeffs[i]);
		} else {
			allPositiveConstraint.lits.push_back(origConstraint.lits[i]);
			allPositiveConstraint.coeffs.push_back(origConstraint.coeffs[i]);
		}
	}
	assert(allPositiveConstraint.lits.size() == origConstraint.lits.size());
	assert(allPositiveConstraint.coeffs.size() == origConstraint.coeffs.size());
	allPositiveConstraint.sortDescendingCoeff(); // sorting according to descending coeff in place
	return allPositiveConstraint;
}

template <class T_data>
void PBInstance<T_data>::simplifyRenumberedConstraint() {
	std::unordered_map<rs::Var, bool> currentInferredAssignment = this->inferredAssignment;
	std::vector<PBconstraint> updatedConstraintVector;
	std::unordered_set<Int> removalSet; // indexes of constraints that needs to be modified (i.e. variable removed)
	bool hasNextIter = true;
	bool hasInfer = true;
	bool hasAssign = true;
	bool hasProbe = true;
	// bool failedLitPerformed = false;

	while(hasNextIter) {
		hasInfer = false;
		hasAssign = false;
		hasProbe = false;
		updatedConstraintVector.clear();
		removalSet.clear();
		
		// loop through each variable
		// set it to positive
		// set it to negative
		// compare inferred literal set
		// if overlap then apply and remain change unchanged to false
		for (Int i = 0; i < this->renumberedClauses.size(); i++) {
			if (this->renumberedClauses.at(i).lits.size() == 1) {
				inferAssignment(this->renumberedClauses.at(i), currentInferredAssignment);
				removalSet.insert(i);
			} else if (this->renumberedClauses.at(i).lits.size() == 0) {
				// fully inferred
				if (this->renumberedClauses.at(i).equals && this->renumberedClauses.at(i).rhs != 0) {
					this->inferredUnsat = true;
				}
				if (!this->renumberedClauses.at(i).equals && this->renumberedClauses.at(i).rhs > 0) {
					this->inferredUnsat = true;
				}
				removalSet.insert(i);
			} else {
				// more than 1 lits
				// check if always sat (only for >= case)
				if (!this->renumberedClauses.at(i).equals) {
					bool clauseAlwaysTrue = checkAlwaysTrue(this->renumberedClauses.at(i));
					if (clauseAlwaysTrue) { removalSet.insert(i); }
				}
			}
		}
		if (!removalSet.empty()) {hasInfer = true;}
		// done inferring for current iteration, now apply to each clause
		for (Int i=0; i < this->renumberedClauses.size(); i++) {
			if (removalSet.find(i) != removalSet.end()) {
				continue; // going to remove, so ignore for now
			} else {
				PBconstraint updatedConstraint;
				updatedConstraint.constraintId = this->renumberedClauses.at(i).constraintId;
				updatedConstraint.equals = this->renumberedClauses.at(i).equals;
				Int updatedRhs = this->renumberedClauses.at(i).rhs;
				for (Int j=0; j < this->renumberedClauses.at(i).lits.size(); j++) {
					if ( currentInferredAssignment.find(std::abs(this->renumberedClauses.at(i).lits.at(j))) == currentInferredAssignment.end() ) {
						Int updatedCoeff = this->renumberedClauses.at(i).coeffs.at(j);
						Int updatedLit = this->renumberedClauses.at(i).lits.at(j);
						updatedConstraint.coeffs.push_back(updatedCoeff);
						updatedConstraint.lits.push_back(updatedLit);
					} else {
						Int currentVar = std::abs(this->renumberedClauses.at(i).lits.at(j));
						Int appliedValue = this->renumberedClauses.at(i).coeffs.at(j);
						if (currentInferredAssignment.at(currentVar) && (this->renumberedClauses.at(i).lits.at(j) < 0)) {
							appliedValue = 0;
						}
						if (!currentInferredAssignment.at(currentVar) && (this->renumberedClauses.at(i).lits.at(j) > 0)) {
							appliedValue = 0;
						}
						updatedRhs -= appliedValue;
						hasAssign = true;
					}
					updatedConstraint.rhs = updatedRhs;
				}
				updatedConstraintVector.push_back(updatedConstraint);
			}
		}
		this->renumberedClauses = updatedConstraintVector;

		// adding probing infer here since that does not involve immediately modifying clauses, we just add to inferred assignments and propagate next iteration
		std::unordered_set<Int> probeInferredAssignments = probeInferAssign(currentInferredAssignment, this->renumberedClauses);
		if (!probeInferredAssignments.empty()) {
			hasProbe = true;
			for (Int inferredLit : probeInferredAssignments) {
				addCheckInferredAssign(inferredLit, currentInferredAssignment);
			}
		}
		//
		// could potentially do failed literal test, but requires call to solver, at this stage solver is not initialized
		//
		if (!hasAssign && !hasInfer && !hasProbe) {
			hasNextIter = false;
		}
	}
	this->inferredAssignment = currentInferredAssignment;
	return;
}

template <class T_data>
void PBInstance<T_data>::inferAssignment(PBconstraint &constraint, std::unordered_map<rs::Var, bool> &inferredAssignmentMap) {
	// assumption is that constraint is sorted according to descending order of coeff, meaning constraint.coeff[0] is the largest coeff. Done in getPositiveCoeffRenumberedConstraint.
	double normConsVal = (double) constraint.rhs / (double) constraint.coeffs.at(0);
	if (constraint.equals) {
		if (normConsVal > 1.0) {
			this->inferredUnsat = true;
		} else {
			if (constraint.lits.at(0) > 0 && normConsVal == 1) {
				addCheckInferredAssign(std::abs(constraint.lits.at(0)), inferredAssignmentMap);
			} else if (constraint.lits.at(0) > 0 && normConsVal == 0) {
				addCheckInferredAssign(-std::abs(constraint.lits.at(0)), inferredAssignmentMap);
			} else if (constraint.lits.at(0) < 0 && normConsVal == 1) {
				addCheckInferredAssign(-std::abs(constraint.lits.at(0)), inferredAssignmentMap);
			} else if (constraint.lits.at(0) < 0 && normConsVal == 0) {
				addCheckInferredAssign(std::abs(constraint.lits.at(0)), inferredAssignmentMap);
			} else {
				this->inferredUnsat = true;
			}
		}
	} else {
		// unit clause not equals so >=
		if (normConsVal > 1.0 && constraint.coeffs.at(0) > 0) {
			// remains as >= after division
			this->inferredUnsat = true;
		} else if (normConsVal < 0.0 && constraint.coeffs.at(0) < 0) {
			// <= after division
			this->inferredUnsat = true;
		} else {
			if (constraint.coeffs.at(0) > 0) {
				// >=
				if (constraint.lits.at(0) > 0 && normConsVal > 0) { 
					addCheckInferredAssign(std::abs(constraint.lits.at(0)), inferredAssignmentMap);
				} else if (constraint.lits.at(0) < 0 && normConsVal > 0) {
					addCheckInferredAssign(-std::abs(constraint.lits.at(0)), inferredAssignmentMap);
				} else {
					// cannot infer anything, so redundant unit clause can just ignore
					;
				}
			} else {
				// <= after division
				if (constraint.lits.at(0) < 0 && normConsVal < 1.0){ 
					addCheckInferredAssign(std::abs(constraint.lits.at(0)), inferredAssignmentMap);
				} else if (constraint.lits.at(0) > 0 && normConsVal < 1.0) {
					addCheckInferredAssign(-std::abs(constraint.lits.at(0)), inferredAssignmentMap);
				} else {
					// cannot infer anything, so redundant unit clause can just ignore
					;
				}
			}
		}
	}
	return;
}

template <class T_data>
void PBInstance<T_data>::addCheckInferredAssign(Int inferredLit, std::unordered_map<rs::Var, bool>&currentInferredAssignment) {
	if (currentInferredAssignment.find(std::abs(inferredLit)) != currentInferredAssignment.end()) {
		if (currentInferredAssignment[std::abs(inferredLit)] != (inferredLit > 0)) {
			this->inferredUnsat = true;
		}
	} else {
		currentInferredAssignment[std::abs(inferredLit)] = inferredLit > 0;
	}
	return;
}

template <class T_data>
bool PBInstance<T_data>::checkAlwaysTrue(PBconstraint &constraint) {
	// use only on >= clauses
	assert(!constraint.equals);
	bool alwaysTrue = false;
	Int slack = constraint.rhs;
	for (int i=0; i < constraint.coeffs.size(); i++) {
		// loop through coefficients, change all to positive
		if (constraint.coeffs.at(i) < 0) {
			slack -= constraint.coeffs.at(i);
		}
	}
	if (slack <= 0) {
		alwaysTrue = true;
	}
	return alwaysTrue;
}

template <class T_data>
PBconstraint PBInstance<T_data>::applyProbe(const PBconstraint &constraint, Int decLit) {
	PBconstraint probeConstraint;
	probeConstraint.equals = constraint.equals;
	std::vector<Int> probeLits;
	std::vector<Int> probeCoeffs;
	Int probeRhs = constraint.rhs;
	for (int i=0; i < constraint.lits.size(); i++) {
		if (std::abs(constraint.lits.at(i)) != std::abs(decLit)) {
			probeLits.push_back(constraint.lits.at(i));
			probeCoeffs.push_back(constraint.coeffs.at(i));
		} else {
			if (constraint.lits.at(i) == decLit) {
				probeRhs -= constraint.coeffs.at(i);
			}
		}
	}
	probeConstraint.rhs = probeRhs;
	probeConstraint.lits = probeLits;
	probeConstraint.coeffs = probeCoeffs;
	return probeConstraint;
}

template <class T_data>
std::pair<Int, bool> PBInstance<T_data>::inferProbe(PBconstraint &constraint) {
	// should be unit clause here
	bool isSat = true;
	Int inferredLit = 0;

	double normConsVal = (double) constraint.rhs / (double) constraint.coeffs.at(0);
	if (constraint.equals) {
		if (normConsVal > 1.0) {
			isSat = false;
		} else {
			if (constraint.lits.at(0) > 0 && normConsVal == 1) {
				inferredLit = std::abs(constraint.lits.at(0));
			} else if (constraint.lits.at(0) > 0 && normConsVal == 0) {
				inferredLit = -std::abs(constraint.lits.at(0));
			} else if (constraint.lits.at(0) < 0 && normConsVal == 1) {
				inferredLit = -std::abs(constraint.lits.at(0));
			} else if (constraint.lits.at(0) < 0 && normConsVal == 0) {
				inferredLit = std::abs(constraint.lits.at(0));
			} else {
				isSat = false;
			}
		}
	} else {
		// unit clause not equals so >=
		if (normConsVal > 1.0 && constraint.coeffs.at(0) > 0) {
			isSat = false;
		} else if (normConsVal < 0.0 && constraint.coeffs.at(0) < 0) {
			isSat = false;
		} else {
			if (constraint.coeffs.at(0) > 0) {
				// >=
				if (constraint.lits.at(0) > 0 && normConsVal > 0){ 
					inferredLit = std::abs(constraint.lits.at(0));
				} else if (constraint.lits.at(0) < 0 && normConsVal > 0) {
					inferredLit = -std::abs(constraint.lits.at(0));
				} else {
					// cannot infer
					inferredLit = 0;
				}
			} else {
				// <= after division
				if (constraint.lits.at(0) < 0 && normConsVal < 1.0){ 
					inferredLit = std::abs(constraint.lits.at(0));
				} else if (constraint.lits.at(0) > 0 && normConsVal < 1.0) {
					inferredLit = -std::abs(constraint.lits.at(0));
				} else {
					// cannot infer
					inferredLit = 0;
				}
			}
		}
	}
	return std::make_pair(inferredLit, isSat);
}

template <class T_data>
std::unordered_set<Int> PBInstance<T_data>::probeInferAssign(std::unordered_map<rs::Var, bool> &inferredAssignmentMap, std::vector<PBconstraint> &constraintVector) {
	std::unordered_set<Int> probeInferredAssignment;
	for (Int var = 1; var <= this->vars; var++) {
		if (inferredAssignmentMap.find(var) == inferredAssignmentMap.end()) {
			for (const PBconstraint &constraint : constraintVector) {
				// only need to look at clauses with exactly 2 literals (set 1 and infer the other)
				if (constraint.lits.size() == 2) {
					// check if worth probing
					if (std::abs(constraint.lits.at(0)) == var || std::abs(constraint.lits.at(1)) == var) {
						// probe pos literal
						PBconstraint posProbeConstraint = applyProbe(constraint, var);
						std::pair<Int, bool> posProbeResult = inferProbe(posProbeConstraint);
						Int posProbeLit = posProbeResult.first;
						bool posSat = posProbeResult.second;
						// probe neg literal
						PBconstraint negProbeConstraint = applyProbe(constraint, -var);
						std::pair<Int, bool> negProbeResult = inferProbe(negProbeConstraint);
						Int negProbeLit = negProbeResult.first;
						bool negSat = negProbeResult.second;
						// infer from probe results
						if (!posSat && !negSat) {
							this->inferredUnsat = true;
						} else if (!posSat) {
							if (this->inferredAssignment.find(var) == this->inferredAssignment.end()) {
								this->inferredAssignment[var] = false;
							} else {
								if (this->inferredAssignment[var] == true) {
									this->inferredUnsat = true;
								}
							}
						} else if (!negSat) {
							if (this->inferredAssignment.find(var) == this->inferredAssignment.end()) {
								this->inferredAssignment[var] = true;
							} else {
								if (this->inferredAssignment[var] == false) {
									this->inferredUnsat = true;
								}
							}
						} else {
							// both probes sat so can infer
							if (posProbeLit != 0 && negProbeLit != 0) {
								// using 0 to mean undecidable
								if (posProbeLit == negProbeLit) {
									probeInferredAssignment.insert(posProbeLit);
								}
							}
						}
					}
				}
			}
		}
	}
	return probeInferredAssignment;
}

template <class T_data>
void PBInstance<T_data>::setMaxCoeffVarPhase() {
	phase.clear();
	phase.resize(vars, -1);
	return;
}
// For Debug
template <class T_data>
void PBInstance<T_data>::toOPB(std::ostream& out)
{
	using namespace std;

	out << "c free vars: " << freevars << endl;
	out << "p cnf " << vars + freevars << " " << clauses.size() << endl;

	if(!weighted && !projected)
		out << "c t mc" << endl;
	else if(weighted && !projected)
		out << "c t wmc" << endl;
	else if(!weighted && projected)
		out << "c t pmc" << endl;
	else if(weighted && projected)
		out << "c t wpmc" << endl;

	if(projected) {
		out << "c p show ";
		for (int i=0; i<npvars; i++)
			out << (i+1) << " ";
		out << "0" << endl;
	}
	if(weighted) {
		int precision = mpfr::bits2digits(mpfr::mpreal::get_default_prec());
		out.precision(precision);

		for (int i=0; i<npvars; i++) {
			out << "c p weight " << (i+1) << " " << lit_weights[toInt(mkLit(i))] << " 0" << endl;
			out << "c p weight -" << (i+1) << " " << lit_weights[toInt(~mkLit(i))] << " 0" << endl;
		}

		if(gweight != 1) {
			out << "c p gweight " << gweight << " 0" << endl;
		}
	}

	for(auto c : clauses) {
		// for (auto l : c)
		// 	out << (sign(l) ? "-":"") << (var(l)+1) << " ";
		// out << "0" << endl;
		out << c.getOpbString() << endl;
	}

	// For Debug
	/*
	for(auto c : learnts) {
		for (auto l : c)
			out << (sign(l) ? "-":"") << (var(l)+1) << " ";
		out << "0" << endl;
	}
	*/
}

template class GPMC::PBInstance<mpz_class>;
template class GPMC::PBInstance<mpfr::mpreal>;
