// Solves heads-up push/fold for many stack sizes.
//
// Usage:
//   nuts_gen_pushfold EQUITY_FILE OUTPUT_JSON
//       Solves S = 1.0, 1.5, ..., 20.0 big blinds and writes the strategies to OUTPUT_JSON.
//       The committed data/pushfold.json was made with (from the repository root):
//           ./build/nuts_gen_pushfold data/equity169.bin data/pushfold.json
//
//   nuts_gen_pushfold --thresholds EQUITY_FILE
//       Solves S = 1.00, 1.05, ..., 20.00 and prints, for every hand class, the largest stack at
//       which the SB shoves it and the BB calls it (probability at least 1/2). Used for
//       docs/pushfold_comparison.md.
//
// The stacks are solved in parallel, one per thread at a time; each solve is independent and
// deterministic, so the output does not depend on the number of threads.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nuts/pushfold.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using nuts::kGridSize;
using nuts::kNumHandClasses;

// Solves every stack in `stacks`, several at a time. Exits the program on an error, or if a solve
// stops at max_iterations before reaching the target exploitability.
std::vector<nuts::PushFoldSolution> solve_all(const nuts::Equity169& equity,
                                              const std::vector<double>& stacks,
                                              const nuts::PushFoldOptions& options) {
  std::vector<nuts::PushFoldSolution> solutions(stacks.size());
  std::atomic<std::size_t> next{0};
  std::atomic<bool> failed{false};
  auto work = [&] {
    for (std::size_t k = next++; k < stacks.size(); k = next++) {
      const std::optional<nuts::PushFoldSolution> solution =
          nuts::solve_pushfold(equity, stacks[k], options);
      if (!solution || !solution->converged) {
        failed = true;
        return;
      }
      solutions[k] = *solution;
    }
  };
  std::vector<std::thread> workers;
  const unsigned num_threads = std::max(1u, std::thread::hardware_concurrency());
  for (unsigned t = 0; t < num_threads; ++t) workers.emplace_back(work);
  for (std::thread& worker : workers) worker.join();
  if (failed) {
    std::fprintf(stderr, "error: a solve failed or did not converge\n");
    std::exit(1);
  }
  return solutions;
}

// Probabilities are written with 6 decimals, without trailing zeros ("1", "0.5", "0.333333").
double round6(double p) { return std::round(p * 1e6) / 1e6; }

std::string format_probability(double p) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.6f", p);
  std::string s = text;
  while (s.back() == '0') s.pop_back();
  if (s.back() == '.') s.pop_back();
  return s;
}

// The share of all 1,326 hands in which the strategy takes the action, in percent.
double percent_of_hands(const std::vector<double>& probability) {
  double hands = 0;
  for (int c = 0; c < kNumHandClasses; ++c) {
    hands += probability[static_cast<std::size_t>(c)] * nuts::hand_class_num_combos(c);
  }
  return 100 * hands / nuts::kNumHoleCardCombos;
}

// Writes a 13 x 13 grid as a JSON array of rows.
void write_grid(std::FILE* out, const std::vector<double>& probability) {
  std::fprintf(out, "[\n");
  for (int row = 0; row < kGridSize; ++row) {
    std::fprintf(out, "        [");
    for (int column = 0; column < kGridSize; ++column) {
      const double p = probability[static_cast<std::size_t>(kGridSize * row + column)];
      std::fprintf(out, "%s%s", column == 0 ? "" : ", ", format_probability(p).c_str());
    }
    std::fprintf(out, "]%s\n", row + 1 < kGridSize ? "," : "");
  }
  std::fprintf(out, "      ]");
}

int write_json(const nuts::Equity169& equity, const char* path) {
  nuts::PushFoldOptions options;  // the defaults: stop at exploitability 1e-6 bb
  std::vector<double> stacks;
  for (int half_bb = 2; half_bb <= 40; ++half_bb) stacks.push_back(half_bb / 2.0);  // 1.0..20.0

  const auto start = Clock::now();
  std::vector<nuts::PushFoldSolution> solutions = solve_all(equity, stacks, options);
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();

  // Round the probabilities to what the file will hold, and evaluate exactly those.
  for (nuts::PushFoldSolution& solution : solutions) {
    for (double& p : solution.strategy.shove) p = round6(p);
    for (double& p : solution.strategy.call) p = round6(p);
    solution.evaluation = nuts::evaluate_pushfold(equity, solution.stack_bb, solution.strategy);
  }

  std::FILE* out = std::fopen(path, "w");
  if (!out) {
    std::fprintf(stderr, "error: cannot write %s\n", path);
    return 1;
  }
  std::fprintf(out, "{\n");
  std::fprintf(out,
               "  \"description\": \"Heads-up push/fold Nash equilibrium: no ante, chip EV, "
               "solved with CFR+ by tools/gen_pushfold.cpp\",\n");
  std::fprintf(out,
               "  \"game\": {\"small_blind_bb\": 0.5, \"big_blind_bb\": 1, \"ante_bb\": 0, "
               "\"stack\": \"effective stack in big blinds, before the blinds are posted\", "
               "\"sb_actions\": [\"fold\", \"shove\"], \"bb_actions\": [\"fold\", \"call\"]},\n");
  std::fprintf(out, "  \"equity_matrix\": \"data/equity169.bin\",\n");
  std::fprintf(out,
               "  \"solver\": {\"method\": \"CFR+ (regret matching+, alternating updates, "
               "linear averaging); the average strategy\", \"target_exploitability_bb\": %g},\n",
               options.target_exploitability);
  std::fprintf(out,
               "  \"exploitability\": \"the average over the two players of what a best response "
               "gains against the other player's strategy, in big blinds per hand, computed "
               "exactly for the probabilities as written in this file\",\n");
  std::fprintf(out,
               "  \"grid\": {\"ranks\": \"AKQJT98765432\", \"layout\": \"row r, column c: the "
               "pair on the diagonal; suited above it (row = higher rank), offsuit below it "
               "(column = higher rank)\", \"hands\": [\n");
  for (int row = 0; row < kGridSize; ++row) {
    std::fprintf(out, "    [");
    for (int column = 0; column < kGridSize; ++column) {
      std::fprintf(out, "%s\"%s\"", column == 0 ? "" : ", ",
                   nuts::hand_class_name(kGridSize * row + column).c_str());
    }
    std::fprintf(out, "]%s\n", row + 1 < kGridSize ? "," : "");
  }
  std::fprintf(out, "  ]},\n");
  std::fprintf(out, "  \"solutions\": [\n");
  for (std::size_t k = 0; k < solutions.size(); ++k) {
    const nuts::PushFoldSolution& s = solutions[k];
    std::fprintf(out, "    {\n");
    std::fprintf(out, "      \"stack_bb\": %.1f,\n", s.stack_bb);
    std::fprintf(out, "      \"iterations\": %d,\n", s.iterations);
    std::fprintf(out, "      \"exploitability_bb\": %.3e,\n", s.evaluation.exploitability);
    std::fprintf(out, "      \"sb_ev_bb\": %.6f,\n", s.evaluation.sb_value);
    std::fprintf(out, "      \"shove_percent\": %.2f,\n", percent_of_hands(s.strategy.shove));
    std::fprintf(out, "      \"call_percent\": %.2f,\n", percent_of_hands(s.strategy.call));
    std::fprintf(out, "      \"shove\": ");
    write_grid(out, s.strategy.shove);
    std::fprintf(out, ",\n      \"call\": ");
    write_grid(out, s.strategy.call);
    std::fprintf(out, "\n    }%s\n", k + 1 < solutions.size() ? "," : "");
  }
  std::fprintf(out, "  ]\n}\n");
  if (std::fclose(out) != 0) {
    std::fprintf(stderr, "error: cannot write %s\n", path);
    return 1;
  }

  std::printf("solved %zu stacks in %.1f s (%u threads)\n", solutions.size(), seconds,
              std::max(1u, std::thread::hardware_concurrency()));
  std::printf("%8s %10s %16s %10s %8s %8s\n", "stack", "iterations", "exploitability", "SB EV",
              "shove%", "call%");
  for (const nuts::PushFoldSolution& s : solutions) {
    std::printf("%8.1f %10d %16.3e %10.6f %8.2f %8.2f\n", s.stack_bb, s.iterations,
                s.evaluation.exploitability, s.evaluation.sb_value,
                percent_of_hands(s.strategy.shove), percent_of_hands(s.strategy.call));
  }
  std::printf("wrote %s\n", path);
  return 0;
}

// The stacks at which a hand class is played (probability at least 1/2), as intervals of
// consecutive stacks in the list: {first stack, last stack}.
std::vector<std::pair<double, double>> played_intervals(
    const std::vector<nuts::PushFoldSolution>& solutions, int hand_class, bool shove) {
  std::vector<std::pair<double, double>> intervals;
  bool in_interval = false;
  for (const nuts::PushFoldSolution& s : solutions) {
    const auto& probability = shove ? s.strategy.shove : s.strategy.call;
    const bool played = probability[static_cast<std::size_t>(hand_class)] >= 0.5;
    if (played && in_interval) {
      intervals.back().second = s.stack_bb;
    } else if (played) {
      intervals.push_back({s.stack_bb, s.stack_bb});
    }
    in_interval = played;
  }
  return intervals;
}

// The largest stack at which the hand is played ("20.00+" if at every stack up to the largest,
// "never" if at none), followed by all the intervals if there is more than one, e.g.
// "7.15 (1.00-2.25, 5.10-7.15)".
std::string format_threshold(const std::vector<std::pair<double, double>>& intervals,
                             double largest_stack) {
  if (intervals.empty()) return "never";
  char text[64];
  const double top = intervals.back().second;
  std::snprintf(text, sizeof(text), top >= largest_stack ? "%.2f+" : "%.2f", top);
  std::string out = text;
  if (intervals.size() > 1) {
    out += " (";
    for (std::size_t k = 0; k < intervals.size(); ++k) {
      std::snprintf(text, sizeof(text), "%s%.2f-%.2f", k == 0 ? "" : ", ", intervals[k].first,
                    intervals[k].second);
      out += text;
    }
    out += ")";
  }
  return out;
}

int print_thresholds(const nuts::Equity169& equity) {
  std::vector<double> stacks;
  for (int hundredths = 100; hundredths <= 2000; hundredths += 5) {
    stacks.push_back(hundredths / 100.0);
  }
  const auto start = Clock::now();
  const std::vector<nuts::PushFoldSolution> solutions = solve_all(equity, stacks, {});
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();

  double worst = 0;
  int most_iterations = 0;
  for (const nuts::PushFoldSolution& s : solutions) {
    worst = std::max(worst, s.evaluation.exploitability);
    most_iterations = std::max(most_iterations, s.iterations);
  }
  std::printf("# %zu stacks from 1.00 to 20.00 bb in steps of 0.05, solved in %.1f s\n",
              solutions.size(), seconds);
  std::printf("# largest exploitability %.3e bb, most iterations %d\n", worst, most_iterations);
  std::printf("# largest stack at which the hand is shoved / called with probability >= 1/2\n");
  std::printf("# (\"20.00+\": at every stack up to 20; in parentheses: all the stack intervals\n");
  std::printf("# where the hand is played, when there is more than one)\n");
  std::printf("%-5s %-32s %-32s\n", "hand", "shove", "call");
  for (int c = 0; c < kNumHandClasses; ++c) {
    std::printf("%-5s %-32s %-32s\n", nuts::hand_class_name(c).c_str(),
                format_threshold(played_intervals(solutions, c, true), stacks.back()).c_str(),
                format_threshold(played_intervals(solutions, c, false), stacks.back()).c_str());
  }
  return 0;
}

int usage(const char* program) {
  std::fprintf(stderr, "usage: %s EQUITY_FILE OUTPUT_JSON\n       %s --thresholds EQUITY_FILE\n",
               program, program);
  return 2;
}

nuts::Equity169 load_or_exit(const char* path) {
  std::string error;
  std::optional<nuts::Equity169> equity = nuts::load_equity169(path, &error);
  if (!equity) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    std::exit(1);
  }
  return std::move(*equity);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--thresholds") == 0) {
    return print_thresholds(load_or_exit(argv[2]));
  }
  if (argc == 3 && argv[1][0] != '-') return write_json(load_or_exit(argv[1]), argv[2]);
  return usage(argv[0]);
}
