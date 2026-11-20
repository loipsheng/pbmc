// main.cc
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/resource.h>

#include "adaptive_k.h"
#include "linear_constraint.h"
#include "heuristic.h"
#include "big_int.h"
#include "solver.h"

using namespace std;

const char *HeuristicName(BranchHeuristic h) {
    switch (h) {
    case BH_HP:
        return "DHP";
    case BH_HP_P:
        return "DHP-P";
    case BH_dTreeHP:
        return "SHP";
    case BH_BC:
        return "BC";
    case BH_BC_P:
        return "BC-P";
    case BH_DomWdeg:
        return "D/W";
    case BH_DomWdeg_P:
        return "D/W-P";
    case BH_Dom:
        return "Dom";
    case BH_Wdeg:
        return "Wdeg";
    case BH_Random:
        return "Random";
    default:
        return "Unknown";
    }
}

const char *AblationName(LinearConstraint::Ablation_Func f) {
    switch (f) {
    case LinearConstraint::kRemoveFixedVariables:
        return "RV";
    case LinearConstraint::kBoundStrengtheningSingle:
        return "SB";
    case LinearConstraint::kCoefficientStrengtheningSingle:
        return "SC";
    case LinearConstraint::kRedundancyDetectionSubset:
        return "RSR";
    case LinearConstraint::kParallelRows:
        return "RPR";
    case LinearConstraint::kBoundStrengtheningLP:
        return "SB-LP";
    case LinearConstraint::kRedundancyDetectionLP:
        return "RIR-LP";
    default:
        return "Unknown";
    }
}

string DisabledSimplifications() {
    if (LinearConstraint::ablation.empty())
        return "none (all enabled)";

    ostringstream out;
    bool first = true;
    for (int id = 1; id <= 7; ++id) {
        auto pass = LinearConstraint::Ablation_Func(id);
        if (!LinearConstraint::ablation.count(pass))
            continue;
        if (!first)
            out << ", ";
        out << AblationName(pass);
        first = false;
    }
    return first ? "none (all enabled)" : out.str();
}

int StrToInt(const string &str) {
    int res = 0;
    try {
        res = stoi(str);
    }
    catch (...) {
        cerr << "StrToInt fail: [" << str << "]" << endl;
    }
    return res;
}

bool StrToUnsignedLongLong(const string &str, unsigned long long &value) {
    if (str.empty() || str.front() == '-')
        return false;
    try {
        size_t parsed = 0;
        value = stoull(str, &parsed);
        return parsed == str.size();
    }
    catch (...) {
        return false;
    }
}

bool ReadKnownCountComment(istream &in, Int &ans, bool &found) {
    found = false;
    in >> ws;
    if (in.peek() != '#')
        return true;

    string line, marker, name;
    Int value;
    getline(in, line);
    istringstream comment(line);
    if (!(comment >> marker >> name >> value) ||
        marker != "#" || name != "known_count") {
        return false;
    }

    ans += value;
    found = true;
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        cerr << "Too few parameters" << endl;
        return -1;
    }

    string in_path;
    bool adaptive_k_enabled = false;
    bool timing_enabled = false;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            in_path = argv[i];
            continue;
        }

        string opt(argv[i]);

        string param = opt.substr(1);
        auto pos = param.find('=');
        if (pos == string::npos) {
            cerr << "Error: unknown parameter [" << param << "]" << endl;
            return -1;
        }

        string k = param.substr(0, pos);
        string v = param.substr(pos + 1);

        if (k == "varlimit") {
            LinearConstraint::variable_limit = StrToInt(v);
        }
        else if (k == "adaptive-k") {
            if (v != "0" && v != "1") {
                cerr << "Error: invalid adaptive K mode [" << v
                    << "]. Expected 0 (fixed K) or 1 (adaptive K)."
                    << endl;
                return -1;
            }
            adaptive_k_enabled = (v == "1");
        }
        else if (k == "timing") {
            if (v != "0" && v != "1") {
                cerr << "Error: invalid timing mode [" << v
                    << "]. Expected 0 (disabled) or 1 (enabled)."
                    << endl;
                return -1;
            }
            timing_enabled = (v == "1");
        }
        else if (k == "sb-iterate") {
            if (v != "0" && v != "1") {
                cerr << "Error: invalid SB iteration mode [" << v
                    << "]. Expected 0 (single pass) or 1 (fixed point)."
                    << endl;
                return -1;
            }
            LinearConstraint::sb_iterate_to_fixed_point = (v == "1");
        }
        else if (k == "ablation") {
            size_t p = 0;
            while (p < v.size()) {
                auto next = v.find_first_of(':', p);
                int val;

                if (next == string::npos) {
                    val = StrToInt(v.substr(p));
                    p = v.size();
                }
                else {
                    val = StrToInt(v.substr(p, next - p));
                    p = next + 1;
                }

                LinearConstraint::ablation.insert(
                    LinearConstraint::Ablation_Func(val));
            }
        }
        else if (k == "cache-limit" || k == "cache-admission-max-cells") {
            int limit = StrToInt(v);
            if (limit < 0) {
                cerr << "[WARN] Invalid cache limit = " << limit
                    << ". Using 0 (cache disabled)." << endl;
                limit = 0;
            }
            LinearConstraint::cache_matrix_limit = limit;
        }
        else if (k == "cache-max-age-inserts") {
            unsigned long long age = 0;
            if (!StrToUnsignedLongLong(v, age)) {
                cerr << "Error: invalid cache insertion age [" << v
                    << "]. Expected a non-negative integer." << endl;
                return -1;
            }
            LinearConstraint::cache_max_age_inserts = age;
        }
        else if (k == "heur") {
            int hv = StrToInt(v);
            if (hv < 1 || hv > 10) {
                cerr << "[WARN] Invalid heuristic id = " << hv
                    << ". Valid range = 1~10. Using default: DHP-P." << endl;
                hv = 2; // default to DHP-P
            }
            Heuristic::set_heuristic((BranchHeuristic)hv);
        }
        else {
            cerr << "Error: unknown parameter [" << k << "]" << endl;
            return -1;
        }
    }

    if (in_path.empty()) {
        cerr << "Error: input path not provided.\n";
        return -1;
    }
    cerr << "[INFO] Using heuristic mode = " << (int)Heuristic::heuristic
        << " (" << HeuristicName(Heuristic::heuristic) << ")\n";
    if (adaptive_k_enabled)
        cerr << "[INFO] K mode = adaptive Jaccard-Confidence, Kmin=1\n";
    else
        cerr << "[INFO] K mode = fixed, varlimit = "
            << LinearConstraint::variable_limit << "\n";
    cerr << "[INFO] SB iteration mode = "
        << (LinearConstraint::sb_iterate_to_fixed_point
                ? "fixed-point" : "single-pass") << "\n";
    cerr << "[INFO] Disabled simplifications: "
        << DisabledSimplifications() << "\n";
    cerr << "[INFO] Cache admission max cells (N*M) = "
        << LinearConstraint::cache_matrix_limit << "\n";
    cerr << "[INFO] Cache max insertion age = "
        << LinearConstraint::cache_max_age_inserts << "\n";
    cerr << "[INFO] Cache entry clean limit = "
        << LinearConstraint::cacheCleanLimitEntries() << "\n";
    cerr << "[INFO] Timing instrumentation = "
        << (timing_enabled ? "enabled" : "disabled") << "\n";

    auto start = chrono::high_resolution_clock::now();
    Int ans(0);
    LinearConstraint lc;
    bool solved_by_sim = false;
    double adaptive_k_decision_time = 0.0;
    Solver::decompose_count = 0;
    Solver::select_count = 0;
    Solver::simplification_timing_enabled = timing_enabled;
    Solver::resetSimplificationTiming();
    LinearConstraint::initializeCacheForInstance();

    ifstream in_file(in_path);
    if (!in_file.is_open()) {
        cerr << "Error reading file " << in_path << endl;
        return -2;
    }

    bool has_known_count = false;
    if (!ReadKnownCountComment(in_file, ans, has_known_count)) {
        cerr << "Error reading known_count comment in " << in_path << endl;
        return -2;
    }

    if (has_known_count) {
        in_file >> ws;
        if (!in_file.eof()) {
            cerr << "Error reading solved Sim output " << in_path << endl;
            return -2;
        }
        solved_by_sim = true;
    }
    else {
        lc.read(in_file);
        if (!in_file) {
            cerr << "Error reading file " << in_path << endl;
            return -2;
        }
        in_file >> ws;
        if (!in_file.eof()) {
            cerr << "Error: trailing data in " << in_path << endl;
            return -2;
        }
        if (adaptive_k_enabled) {
            const auto decision_start = chrono::steady_clock::now();
            const adaptive_k::Decision decision = adaptive_k::Compute(lc);
            if (timing_enabled) {
                adaptive_k_decision_time = chrono::duration_cast<
                    chrono::duration<double>>(
                        chrono::steady_clock::now() - decision_start).count();
            }
            const adaptive_k::Metrics &metrics = decision.metrics;
            if (timing_enabled)
                cerr << setprecision(12)
                    << "[INFO] Adaptive K decision time (s): "
                    << adaptive_k_decision_time << "\n";

            const unsigned long long row_count =
                metrics.m0 > 0
                    ? static_cast<unsigned long long>(metrics.m0)
                    : 0ULL;
            const unsigned long long pair_count =
                row_count * (row_count > 0 ? row_count - 1 : 0) / 2;
            cerr << setprecision(12)
                << "[INFO] Adaptive K: n0=" << metrics.n0
                << " m0=" << metrics.m0
                << " J=" << metrics.mean_jaccard
                << " P=" << pair_count
                << " JConf=" << metrics.jaccard_confidence
                << " K=" << metrics.k << "\n";
            LinearConstraint::variable_limit = metrics.k;
            ans = Solver::count(lc);
        }
        else {
            ans = Solver::count(lc);
        }
    }
    in_file.close();

    auto end = chrono::high_resolution_clock::now();
    auto run_time =
        chrono::duration_cast<std::chrono::nanoseconds>(end - start) * 1e-9;

    long peak_kb = 0;
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        peak_kb = usage.ru_maxrss;
    }
    double peak_mb = peak_kb / 1024.0;
    const double search_simplification_time =
        Solver::simplificationTimeSeconds();
    const double total_simplification_time = search_simplification_time;
    const double simplification_time_ratio =
        run_time.count() > 0.0
            ? 100.0 * total_simplification_time / run_time.count()
            : 0.0;

    cout << "*************************************\n";
    cout << "Input constraints: " << in_path << "\n";

    cout << "Number of solutions: " << ans;
    if (solved_by_sim)
        cout << " (solved by Sim)";
    cout << "\n";
    if (!solved_by_sim) {
        cout << "Decompose count: " << Solver::decompose_count << "\n";
        cout << "Select_variable count: " << Solver::select_count << "\n";
        cout << "Total dpll actions: "
            << (Solver::decompose_count + Solver::select_count) << "\n";
    }

    cout << "Run time: " << run_time.count() << "\n";
    cout << "Adaptive K decision time (s): "
         << adaptive_k_decision_time << "\n";
    cout << "Search simplification time (s): "
         << search_simplification_time << "\n";
    cout << "Total simplification time (s): "
         << total_simplification_time << "\n";
    cout << "Simplification time ratio (%): "
         << simplification_time_ratio << "\n";
    cout << "Peak RSS (MB): " << peak_mb << "\n";
    LinearConstraint::printCacheStats(cout);

    cout << "*************************************\n";

    return 0;
}
