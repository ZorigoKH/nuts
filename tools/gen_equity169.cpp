// Computes the exact 169 x 169 preflop equity matrix and writes it to a file.
//
// Usage: nuts_gen_equity169 [--threads N] OUTPUT
//
// N is 1 to 256 (nuts::kMaxThreads); the default is one thread per hardware thread.
//
// The committed data/equity169.bin was made with (from the repository root, Release build):
//
//     ./build/nuts_gen_equity169 data/equity169.bin
//     (cd data && shasum -a 256 equity169.bin > equity169.sha256)
//
// and anyone can check that a fresh run reproduces it byte for byte with
//
//     ./build/nuts_gen_equity169 /tmp/equity169.bin && cmp /tmp/equity169.bin data/equity169.bin
//
// The result does not depend on the number of threads. See compute_equity169() in
// src/pushfold.cpp for the method and data/README.md for the file format.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "nuts/equity.hpp"  // kMaxThreads
#include "nuts/pushfold.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// The system load, to judge how much other programs may have slowed the run down.
void print_load_average(const char* when) {
#if defined(__APPLE__) || defined(__linux__)
  double load[3] = {};
  if (getloadavg(load, 3) == 3) {
    std::printf("load average %s: %.2f %.2f %.2f (1, 5, 15 min)\n", when, load[0], load[1],
                load[2]);
  }
#else
  (void)when;
#endif
}

int usage(const char* program) {
  std::fprintf(stderr, "usage: %s [--threads N] OUTPUT   (N: 1 to %d)\n", program,
               nuts::kMaxThreads);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  int threads = 0;  // 0 = one per hardware thread
  std::string output;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
      threads = std::atoi(argv[++i]);
      if (threads <= 0 || threads > nuts::kMaxThreads) return usage(argv[0]);
    } else if (argv[i][0] != '-' && output.empty()) {
      output = argv[i];
    } else {
      return usage(argv[0]);
    }
  }
  if (output.empty()) return usage(argv[0]);

  const unsigned hardware_threads = std::thread::hardware_concurrency();
  const unsigned used_threads =
      threads > 0 ? static_cast<unsigned>(threads)
                  : std::clamp(hardware_threads, 1u, static_cast<unsigned>(nuts::kMaxThreads));
  std::printf("threads: %u (hardware threads: %u)\n", used_threads, hardware_threads);
  print_load_average("at start");
  std::fflush(stdout);

  const auto start = Clock::now();
  auto last_report = start;
  const nuts::Equity169 equity =
      nuts::compute_equity169(threads, [&](std::size_t done, std::size_t total) {
        // Report every 30 seconds and at the end.
        const auto now = Clock::now();
        if (done < total && now - last_report < std::chrono::seconds(30)) return;
        last_report = now;
        const double seconds = std::chrono::duration<double>(now - start).count();
        std::printf("  %6zu / %zu matchups (%5.1f%%), %7.1f s\n", done, total,
                    100.0 * static_cast<double>(done) / static_cast<double>(total), seconds);
        std::fflush(stdout);
      });
  const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
  std::printf("computed in %.1f s (%.1f min)\n", seconds, seconds / 60);
  print_load_average("at end");

  std::string error;
  if (!nuts::save_equity169(equity, output, &error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  // Read the file back through the checking loader, so a bad file is caught right away.
  if (!nuts::load_equity169(output, &error)) {
    std::fprintf(stderr, "error: the file written does not load back: %s\n", error.c_str());
    return 1;
  }
  std::printf("wrote %s\n", output.c_str());

  std::printf("some entries (equity of the first class against the second):\n");
  const char* examples[][2] = {{"AA", "KK"},  {"AA", "72o"},  {"AKs", "QQ"},
                               {"AKo", "22"}, {"KQs", "AJo"}, {"72o", "32o"}};
  for (const auto& [first, second] : examples) {
    const int i = *nuts::parse_hand_class(first);
    const int j = *nuts::parse_hand_class(second);
    std::printf("  %-3s vs %-3s  %.6f  (%u pairs of hands)\n", first, second, equity.equity(i, j),
                equity.weight(i, j));
  }
  return 0;
}
