#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "sim.h"

namespace fs = std::filesystem;

namespace {

  struct Summary {
    std::size_t files = 0;
    std::size_t simplified_files = 0;
    std::size_t unchanged_files = 0;
  };

  bool readKnownCountComment(std::istream &in, Int &known_count,
    bool &found) {
    found = false;
    in >> std::ws;
    if (in.peek() != '#') return true;

    std::string line, marker, name;
    Int value;
    std::getline(in, line);
    std::istringstream comment(line);
    if (!(comment >> marker >> name >> value) ||
      marker != "#" || name != "known_count") {
      return false;
    }

    known_count += value;
    found = true;
    return true;
  }

  bool readCases(const fs::path &path,
    std::vector<LinearConstraint> &cases, Int &known_count,
    bool &has_known_count) {
    std::ifstream in(path);
    if (!readKnownCountComment(in, known_count, has_known_count))
      return false;

    if (has_known_count) {
      in >> std::ws;
      return in.eof();
    }

    LinearConstraint lc;
    lc.read(in, false);
    if (!in) return false;
    in >> std::ws;
    if (!in.eof()) return false;
    cases.push_back(std::move(lc));
    return true;
  }

  void writeSolvedResult(std::ostream &out, const Int &known_count) {
    out << "# known_count " << known_count << '\n';
  }

  bool processFile(const fs::path &input, const fs::path &output,
    int variable_limit, Summary &summary) {
    std::vector<LinearConstraint> cases;
    Int known_count(0);
    bool has_known_count = false;
    if (!readCases(input, cases, known_count, has_known_count)) {
      std::cerr << "Sim: cannot parse " << input << '\n';
      return false;
    }

    bool was_simplified = has_known_count;
    std::vector<std::size_t> unresolved;
    for (std::size_t i = 0; i < cases.size(); ++i) {
      const Sim::Result result = Sim::preprocess(cases[i], variable_limit);
      switch (result.status) {
      case Sim::Status::kUnsat:
        known_count += result.count;
        was_simplified = true;
        break;
      case Sim::Status::kDirectCount:
        known_count += result.count;
        was_simplified = true;
        break;
      case Sim::Status::kSimplified:
        was_simplified = true;
        unresolved.push_back(i);
        break;
      case Sim::Status::kUnchanged:
        unresolved.push_back(i);
        break;
      }
    }

    if (was_simplified) {
      ++summary.simplified_files;
    }
    else {
      ++summary.unchanged_files;
    }

    const fs::path output_dir = output.parent_path();
    if (!output_dir.empty()) fs::create_directories(output_dir);
    std::ofstream out(output);
    if (!out) {
      std::cerr << "Sim: cannot write " << output << '\n';
      return false;
    }

    if (unresolved.empty()) {
      writeSolvedResult(out, known_count);
    }
    else {
      cases[unresolved.front()].output(out);
    }
    ++summary.files;
    return true;
  }

  fs::path defaultOutput(const fs::path &input) {
    std::string value = fs::absolute(input).lexically_normal().string();
    for (const std::string marker : {"benchmarks/origin", "benchmarks/Origin"}) {
      const auto pos = value.find(marker);
      if (pos != std::string::npos) {
        value.replace(pos, marker.size(), "benchmarks/preprocessing");
        return value;
      }
    }
    return input.string() + ".preprocessed";
  }

  void usage() {
    std::cerr << "Usage: Sim [-varlimit K] [-output PATH] INPUT\n";
  }

}  // namespace

int main(int argc, char **argv) {
  int variable_limit = Sim::kVariableLimit;
  fs::path input, output;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-varlimit" && i + 1 < argc) {
      try {
        variable_limit = std::stoi(argv[++i]);
      }
      catch (...) {
        usage(); return 2;
      }
      if (variable_limit < 0) { usage(); return 2; }
    }
    else if (arg == "-output" && i + 1 < argc) output = argv[++i];
    else if (!arg.empty() && arg[0] == '-') { usage(); return 2; }
    else if (input.empty()) input = arg;
    else { usage(); return 2; }
  }

  if (input.empty() || !fs::exists(input)) { usage(); return 2; }
  if (output.empty()) output = defaultOutput(input);

  const auto start_time = std::chrono::steady_clock::now();
  bool ok = true;
  Summary summary;
  if (fs::is_regular_file(input)) {
    ok = processFile(input, output, variable_limit, summary);
  }
  else {
    for (const auto &entry : fs::recursive_directory_iterator(input)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".in") continue;
      const fs::path relative = fs::relative(entry.path(), input);
      const bool processed = processFile(entry.path(), output / relative,
        variable_limit, summary);
      ok = processed && ok;
    }
  }
  const auto end_time = std::chrono::steady_clock::now();
  const std::chrono::duration<double> elapsed = end_time - start_time;

  std::cerr << "Sim: processed " << summary.files << " file(s)\n";
  std::cerr << "Sim: simplified " << summary.simplified_files << " file(s)\n";
  std::cerr << "Sim: unchanged " << summary.unchanged_files << " file(s)\n";
  std::cerr << "Sim: preprocessing_time " << elapsed.count() << " s\n";
  return ok ? 0 : 1;
}
