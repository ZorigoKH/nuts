// Benchmarks for the hand evaluator (single thread) and the equity calculators.
//
// Usage: nuts_bench [--quick]
//
//   (default)  full run: 10M random hands and 10M Monte Carlo trials, the median of 7 timed runs
//              per benchmark.
//   --quick    smoke run for CI: 1M hands and 1M trials, a single timed run (not for quoting).
//
// Every benchmark runs once untimed first (warm-up: fills caches, and the very first evaluator
// call builds the lookup tables), then the timed runs. Every run turns its results into a
// checksum (the sum of the HandRanks, or the bits of an equity), so the compiler cannot skip the
// work, and all runs must produce the same checksum.
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "nuts/card.hpp"
#include "nuts/equity.hpp"
#include "nuts/evaluator.hpp"
#include "nuts/range.hpp"

#ifndef NUTS_BUILD_TYPE
#define NUTS_BUILD_TYPE "unknown"
#endif
#ifndef NUTS_CXX_FLAGS
#define NUTS_CXX_FLAGS "unknown"
#endif

namespace {

using nuts::Card;
using Clock = std::chrono::steady_clock;

// ---- Machine description --------------------------------------------------------------------

std::string cpu_name() {
#if defined(__APPLE__)
  char name[256] = {};
  std::size_t size = sizeof(name);
  if (sysctlbyname("machdep.cpu.brand_string", name, &size, nullptr, 0) == 0) return name;
#elif defined(__linux__)
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    if (line.rfind("model name", 0) == 0) {
      const std::size_t colon = line.find(':');
      if (colon != std::string::npos) return line.substr(colon + 2);
    }
  }
#endif
  return "unknown";
}

const char* os_name() {
#if defined(__APPLE__)
  return "macOS";
#elif defined(__linux__)
  return "Linux";
#elif defined(_WIN32)
  return "Windows";
#else
  return "unknown";
#endif
}

const char* compiler_name() {
#if defined(__clang__) || defined(__GNUC__)
  return __VERSION__;
#else
  return "unknown";
#endif
}

// ---- Timing ---------------------------------------------------------------------------------

struct Measurement {
  double median_seconds = 0;
  std::uint64_t checksum = 0;
};

// Runs work() once untimed, then `runs` times timed. work() returns a checksum.
template <typename Work>
Measurement measure(int runs, Work&& work) {
  const std::uint64_t checksum = work();  // warm-up
  std::vector<double> seconds;
  for (int run = 0; run < runs; ++run) {
    const auto start = Clock::now();
    const std::uint64_t result = work();
    seconds.push_back(std::chrono::duration<double>(Clock::now() - start).count());
    if (result != checksum) {
      std::fprintf(stderr, "error: checksum changed between runs (%llu vs %llu)\n",
                   static_cast<unsigned long long>(result),
                   static_cast<unsigned long long>(checksum));
      std::exit(1);
    }
  }
  std::sort(seconds.begin(), seconds.end());
  const std::size_t mid = seconds.size() / 2;
  const double median =
      seconds.size() % 2 == 1 ? seconds[mid] : (seconds[mid - 1] + seconds[mid]) / 2;
  return {median, checksum};
}

// ---- Workloads ------------------------------------------------------------------------------

// `count` random 7-card hands, each made of 7 distinct cards (the first 7 steps of a
// Fisher-Yates shuffle of the deck; the deck is not reset between hands, which is fine because
// shuffling any starting order gives a uniformly random result). Fixed seed, so every run of the
// program benchmarks the same hands.
std::vector<std::array<Card, 7>> random_hands(std::size_t count) {
  std::mt19937_64 rng(12345);
  std::vector<std::array<Card, 7>> hands(count);
  std::array<Card, nuts::kNumCards> deck{};
  std::iota(deck.begin(), deck.end(), Card{0});
  for (auto& hand : hands) {
    for (std::size_t i = 0; i < hand.size(); ++i) {
      const std::size_t j = i + rng() % (deck.size() - i);
      std::swap(deck[i], deck[j]);
      hand[i] = deck[i];
    }
  }
  return hands;
}

// Evaluates every 7-card hand once, in increasing order of card ids.
std::uint64_t evaluate_all_seven_card_hands(std::uint64_t& num_hands) {
  constexpr int n = nuts::kNumCards;
  std::array<Card, 7> h{};
  std::uint64_t checksum = 0;
  std::uint64_t count = 0;
  for (int a = 0; a < n; ++a) {
    h[0] = static_cast<Card>(a);
    for (int b = a + 1; b < n; ++b) {
      h[1] = static_cast<Card>(b);
      for (int c = b + 1; c < n; ++c) {
        h[2] = static_cast<Card>(c);
        for (int d = c + 1; d < n; ++d) {
          h[3] = static_cast<Card>(d);
          for (int e = d + 1; e < n; ++e) {
            h[4] = static_cast<Card>(e);
            for (int f = e + 1; f < n; ++f) {
              h[5] = static_cast<Card>(f);
              for (int g = f + 1; g < n; ++g) {
                h[6] = static_cast<Card>(g);
                checksum += nuts::evaluate7(h);
                ++count;
              }
            }
          }
        }
      }
    }
  }
  num_hands = count;
  return checksum;
}

void print_rate(const char* name, std::uint64_t evals, const Measurement& m) {
  std::printf("%-44s %8.1f M evals/s  (median %8.2f ms, checksum %llu)\n", name,
              static_cast<double>(evals) / m.median_seconds / 1e6, m.median_seconds * 1e3,
              static_cast<unsigned long long>(m.checksum));
}

// The checksum of an equity result: the bits of player 1's equity (the same bits every run,
// because the calculations are deterministic).
std::uint64_t equity_checksum(const std::optional<nuts::EquityResult>& result) {
  if (!result) {
    std::fprintf(stderr, "error: equity calculation failed\n");
    std::exit(1);
  }
  return std::bit_cast<std::uint64_t>(result->players[0].equity);
}

// The system load, to judge how much other programs may have slowed the measurements.
void print_load_average() {
#if defined(__APPLE__) || defined(__linux__)
  double load[3] = {};
  if (getloadavg(load, 3) == 3) {
    std::printf("load avg : %.2f %.2f %.2f (1, 5, 15 min)\n", load[0], load[1], load[2]);
  }
#endif
}

}  // namespace

int main(int argc, char** argv) {
  bool quick = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--quick") == 0) {
      quick = true;
    } else {
      std::fprintf(stderr, "usage: %s [--quick]\n", argv[0]);
      return 2;
    }
  }
  const int runs = quick ? 1 : 7;
  const std::size_t num_random = quick ? 1'000'000 : 10'000'000;

  std::printf(
      "nuts_bench (%s: median of %d timed run%s after 1 warm-up run; one thread unless a "
      "line says otherwise)\n",
      quick ? "quick" : "full", runs, runs == 1 ? "" : "s");
  std::printf("cpu      : %s (%u hardware threads)\n", cpu_name().c_str(),
              std::thread::hardware_concurrency());
  std::printf("os       : %s\n", os_name());
  std::printf("compiler : %s\n", compiler_name());
  std::printf("build    : %s, flags: %s\n", NUTS_BUILD_TYPE, NUTS_CXX_FLAGS);
  print_load_average();
  std::printf("\n");

  // The first evaluator call builds the lookup tables; time it on its own (it happens once per
  // process, so there is nothing to take a median of).
  {
    const std::array<Card, 7> hand = {0, 5, 10, 15, 20, 25, 30};
    const auto start = Clock::now();
    const nuts::HandRank rank = nuts::evaluate7(hand);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    std::printf("%-44s %8.2f ms  (one-time, first call; rank %u)\n", "table build", ms,
                static_cast<unsigned>(rank));
  }

  const std::vector<std::array<Card, 7>> hands = random_hands(num_random);
  std::vector<std::uint64_t> masks(hands.size());
  std::transform(hands.begin(), hands.end(), masks.begin(),
                 [](const std::array<Card, 7>& hand) { return nuts::mask_of(hand); });

  const Measurement by_cards = measure(runs, [&] {
    std::uint64_t sum = 0;
    for (const auto& hand : hands) sum += nuts::evaluate7(hand);
    return sum;
  });
  const std::string cards_label = "evaluate7, " + std::to_string(num_random) + " random hands";
  print_rate(cards_label.c_str(), num_random, by_cards);

  const Measurement by_mask = measure(runs, [&] {
    std::uint64_t sum = 0;
    for (const std::uint64_t mask : masks) sum += nuts::evaluate(mask);
    return sum;
  });
  const std::string mask_label = "evaluate(mask), " + std::to_string(num_random) + " random hands";
  print_rate(mask_label.c_str(), num_random, by_mask);
  if (by_mask.checksum != by_cards.checksum) {
    std::fprintf(stderr, "error: evaluate7 and evaluate(mask) disagree\n");
    return 1;
  }

  std::uint64_t num_hands = 0;
  const Measurement all = measure(runs, [&] { return evaluate_all_seven_card_hands(num_hands); });
  if (num_hands != 133'784'560) {
    std::fprintf(stderr, "error: enumerated %llu hands\n",
                 static_cast<unsigned long long>(num_hands));
    return 1;
  }
  print_rate("evaluate7, all 133,784,560 hands in order", num_hands, all);
  std::printf("%-44s %8.3f s\n\n", "  (time for the full enumeration)", all.median_seconds);

  // Exact heads-up equity before the flop: every one of the C(48, 5) = 1,712,304 boards.
  const std::vector<nuts::HoleCards> heads_up = {*nuts::parse_hole_cards("AhAs"),
                                                 *nuts::parse_hole_cards("KdKc")};
  std::uint64_t boards = 0;
  double exact_equity = 0;
  const Measurement exact = measure(runs, [&] {
    const std::optional<nuts::EquityResult> result = nuts::equity_exact(heads_up, {});
    boards = result ? result->deals : 0;
    exact_equity = result ? result->players[0].equity : 0;
    return equity_checksum(result);
  });
  std::printf("%-44s %8.2f ms  (median; %llu boards, %.1f M boards/s, equity %.6f)\n",
              "equity_exact, AhAs vs KdKc preflop", exact.median_seconds * 1e3,
              static_cast<unsigned long long>(boards),
              static_cast<double>(boards) / exact.median_seconds / 1e6, exact_equity);

  // Monte Carlo: AA against a random hand, before the flop, on 1 thread and on all threads.
  const std::vector<nuts::Range> ranges = {*nuts::parse_range("AA"), *nuts::parse_range("random")};
  const std::uint64_t trials = quick ? 1'000'000 : 10'000'000;
  const int all_threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  for (const int threads : {1, all_threads}) {
    double mc_equity = 0;
    double std_error = 0;
    const Measurement mc = measure(runs, [&] {
      const std::optional<nuts::EquityResult> result =
          nuts::equity_monte_carlo(ranges, {}, trials, /*seed=*/1, threads);
      mc_equity = result ? result->players[0].equity : 0;
      std_error = result ? result->players[0].std_error : 0;
      return equity_checksum(result);
    });
    const std::string label = "equity_monte_carlo, AA vs random, " + std::to_string(threads) +
                              (threads == 1 ? " thread" : " threads");
    std::printf("%-44s %8.1f M trials/s (median %8.2f ms, %llu trials, equity %.4f +- %.4f)\n",
                label.c_str(), static_cast<double>(trials) / mc.median_seconds / 1e6,
                mc.median_seconds * 1e3, static_cast<unsigned long long>(trials), mc_equity,
                std_error);
  }
  return 0;
}
