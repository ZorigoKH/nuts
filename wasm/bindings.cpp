// JavaScript bindings for the WebAssembly build, made with embind (Emscripten's C++ <-> JS
// binding library). Built into nuts.mjs + nuts.wasm; see wasm/README.md for the API.
//
// Conventions:
//   * Names match the Python API (evaluate, equity, parse_range, pushfold, ...).
//   * Results are plain JavaScript objects and arrays, built with emscripten::val (a handle to a
//     JavaScript value that C++ can create and fill in).
//   * The build has C++ exceptions turned off (Emscripten's default: smaller and faster), so
//     errors are returned, not thrown, as in the C++ library: a function that can fail returns
//     an object with an `error` string instead of its result.
//   * Single-threaded: WebAssembly threads would need SharedArrayBuffer, which web pages only get
//     with special HTTP headers. So Monte Carlo runs on one thread (its results are still
//     reproducible: the same seed gives the same numbers).
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nuts/card.hpp"
#include "nuts/embedded_data.hpp"
#include "nuts/equity.hpp"
#include "nuts/evaluator.hpp"
#include "nuts/pushfold.hpp"
#include "nuts/range.hpp"

namespace {

using emscripten::val;

val error_result(const std::string& message) {
  val result = val::object();
  result.set("error", message);
  return result;
}

// A JavaScript number that must be a whole number from `min` to 2^53 (the largest integer a
// double holds exactly).
std::optional<std::uint64_t> to_count(double number, double min) {
  if (!std::isfinite(number) || number != std::floor(number) || number < min ||
      number > 9007199254740992.0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(number);
}

// ---- Hand evaluation ------------------------------------------------------------------------

// evaluate("As Ks Qs Js Ts") -> 7462. 1..7462 (higher is better) for 5 to 7 valid cards, 0 for
// anything else.
int evaluate(const std::string& text) {
  const std::optional<std::vector<nuts::Card>> cards = nuts::parse_cards(text);
  if (!cards || cards->size() < 5 || cards->size() > 7) return nuts::kInvalidHandRank;
  return nuts::evaluate(nuts::mask_of(*cards));
}

// category(7462) -> "Straight Flush"; "" if the rank is not 1..7462.
std::string category(int rank) {
  if (rank < nuts::kWorstHandRank || rank > nuts::kBestHandRank) return "";
  return std::string(nuts::category_name(nuts::category(static_cast<nuts::HandRank>(rank))));
}

// ---- Ranges ---------------------------------------------------------------------------------

// parse_range("AKs") -> {combos: ["AsKs", "AhKh", "AdKd", "AcKc"]} (weights are not returned).
val parse_range(const std::string& text) {
  std::string error;
  const std::optional<nuts::Range> range = nuts::parse_range(text, &error);
  if (!range) return error_result(error);
  val combos = val::array();
  for (const nuts::Combo& combo : *range) {
    combos.call<void>("push", nuts::hole_cards_to_string(combo.cards));
  }
  val result = val::object();
  result.set("combos", combos);
  return result;
}

// ---- Equity ---------------------------------------------------------------------------------

// equity(["AhAs", "KdKc"], "", 100000, 1) ->
//   {exact: true, deals: 1712304, players: [{hand, equity, win, tie, std_error}, ...]}
//
// hands: one string per player, two specific cards ("AsKd") or a range ("TT+, AKs"). When every
// player has two specific cards the answer is exact (every completion of the board, at most
// 1.7 million); otherwise it is a Monte Carlo estimate from `trials` random deals with `seed`.
val equity(const val& hands_js, const std::string& board_text, double trials_js, double seed_js) {
  const std::vector<std::string> hands = emscripten::vecFromJSArray<std::string>(hands_js);
  const std::optional<std::vector<nuts::Card>> board = nuts::parse_cards(board_text);
  if (!board) return error_result("cannot read the board \"" + board_text + "\"");

  // Exact if every hand is two specific cards.
  std::vector<nuts::HoleCards> hole_cards;
  for (const std::string& hand : hands) {
    const std::optional<nuts::HoleCards> cards = nuts::parse_hole_cards(hand);
    if (!cards) break;
    hole_cards.push_back(*cards);
  }
  const bool exact = hole_cards.size() == hands.size();

  std::optional<nuts::EquityResult> result;
  std::string error;
  if (exact) {
    result = nuts::equity_exact(hole_cards, *board, &error);
  } else {
    const std::optional<std::uint64_t> trials = to_count(trials_js, 1);
    const std::optional<std::uint64_t> seed = to_count(seed_js, 0);
    if (!trials) return error_result("trials must be a whole number, at least 1");
    if (!seed) return error_result("seed must be a whole number, at least 0");
    std::vector<nuts::Range> ranges;
    for (const std::string& hand : hands) {
      // Two specific cards in any spelling that exact equity accepts (parse_range() would read
      // "As Ks" as two separate items), or else a range.
      if (const std::optional<nuts::HoleCards> cards = nuts::parse_hole_cards(hand)) {
        ranges.push_back(nuts::single_combo_range(*cards));
        continue;
      }
      std::optional<nuts::Range> range = nuts::parse_range(hand, &error);
      if (!range) return error_result(error);
      ranges.push_back(std::move(*range));
    }
    result = nuts::equity_monte_carlo(ranges, *board, *trials, *seed, /*threads=*/1, &error);
  }
  if (!result) return error_result(error);

  val players = val::array();
  for (std::size_t p = 0; p < result->players.size(); ++p) {
    const nuts::PlayerEquity& player = result->players[p];
    val entry = val::object();
    entry.set("hand", hands[p]);
    entry.set("equity", player.equity);
    entry.set("win", player.win);
    entry.set("tie", player.tie);
    entry.set("std_error", player.std_error);
    players.call<void>("push", entry);
  }
  val out = val::object();
  out.set("exact", result->exact);
  out.set("deals", static_cast<double>(result->deals));
  out.set("players", players);
  return out;
}

// ---- Push/fold ------------------------------------------------------------------------------

// A 13 x 13 chart (array of rows); cell (row, column) is cell_value(13 * row + column).
template <typename CellValue>
val chart(CellValue cell_value) {
  val rows = val::array();
  for (int row = 0; row < nuts::kGridSize; ++row) {
    val cells = val::array();
    for (int column = 0; column < nuts::kGridSize; ++column) {
      cells.call<void>("push", cell_value(nuts::kGridSize * row + column));
    }
    rows.call<void>("push", cells);
  }
  return rows;
}

// pushfold(10) -> {stack_bb, iterations, exploitability, sb_ev, hands, shove, call}: the
// heads-up push/fold equilibrium (see python/nuts/__init__.py). hands, shove and call are
// 13 x 13 charts: hands[0][0] = "AA", hands[0][1] = "AKs", hands[1][0] = "AKo".
val pushfold(double stack_bb) {
  std::string error;
  const nuts::Equity169* equity = nuts::embedded_equity169(&error);
  if (!equity) return error_result(error);
  const std::optional<nuts::PushFoldSolution> solution =
      nuts::solve_pushfold(*equity, stack_bb, {}, &error);
  if (!solution) return error_result(error);
  // Cannot happen with the default options (every allowed stack converges), but an unconverged
  // strategy must never be returned as if it were an equilibrium.
  if (!solution->converged) return error_result("the push/fold solver did not converge");

  const nuts::PushFoldStrategy& strategy = solution->strategy;
  val out = val::object();
  out.set("stack_bb", solution->stack_bb);
  out.set("iterations", solution->iterations);
  out.set("exploitability", solution->evaluation.exploitability);
  out.set("sb_ev", solution->evaluation.sb_value);
  out.set("hands", chart([](int c) { return nuts::hand_class_name(c); }));
  out.set("shove", chart([&](int c) { return strategy.shove[static_cast<std::size_t>(c)]; }));
  out.set("call", chart([&](int c) { return strategy.call[static_cast<std::size_t>(c)]; }));
  return out;
}

// ---- The exhaustive check -------------------------------------------------------------------

// Evaluates all C(52, 7) = 133,784,560 seven-card hands, in increasing order of card ids (the
// same loop as tools/bench.cpp), and counts each category. Returns
//   {hands, distinct_values, royal_flushes, elapsed_ms, categories: {"High Card": n, ...}}.
// Takes a few seconds; the page that calls it should run it in a web worker.
val count_all_seven_card_hands() {
  const auto start = std::chrono::steady_clock::now();
  // Hands per HandRank. On the heap: 30 KB would be a big share of WebAssembly's 64 KB stack.
  std::vector<std::uint32_t> count_of_rank(nuts::kBestHandRank + 1, 0);
  std::array<nuts::Card, 7> h{};
  constexpr int n = nuts::kNumCards;
  for (int a = 0; a < n; ++a) {
    h[0] = static_cast<nuts::Card>(a);
    for (int b = a + 1; b < n; ++b) {
      h[1] = static_cast<nuts::Card>(b);
      for (int c = b + 1; c < n; ++c) {
        h[2] = static_cast<nuts::Card>(c);
        for (int d = c + 1; d < n; ++d) {
          h[3] = static_cast<nuts::Card>(d);
          for (int e = d + 1; e < n; ++e) {
            h[4] = static_cast<nuts::Card>(e);
            for (int f = e + 1; f < n; ++f) {
              h[5] = static_cast<nuts::Card>(f);
              for (int g = f + 1; g < n; ++g) {
                h[6] = static_cast<nuts::Card>(g);
                ++count_of_rank[nuts::evaluate7(h)];
              }
            }
          }
        }
      }
    }
  }
  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

  // Add up the counts per category.
  std::array<double, nuts::kNumCategories> per_category{};
  double hands = 0;
  int distinct_values = 0;
  for (int rank = nuts::kWorstHandRank; rank <= nuts::kBestHandRank; ++rank) {
    const std::uint32_t count = count_of_rank[static_cast<std::size_t>(rank)];
    if (count == 0) continue;
    per_category[static_cast<std::size_t>(nuts::category(static_cast<nuts::HandRank>(rank)))] +=
        count;
    hands += count;
    ++distinct_values;
  }
  val categories = val::object();
  for (int c = 0; c < nuts::kNumCategories; ++c) {
    categories.set(std::string(nuts::category_name(static_cast<nuts::Category>(c))),
                   per_category[static_cast<std::size_t>(c)]);
  }
  val out = val::object();
  out.set("hands", hands);
  out.set("distinct_values", distinct_values);
  out.set("royal_flushes", count_of_rank[nuts::kBestHandRank]);
  out.set("elapsed_ms", elapsed_ms);
  out.set("categories", categories);
  return out;
}

}  // namespace

EMSCRIPTEN_BINDINGS(nuts) {
  emscripten::function("evaluate", &evaluate);
  emscripten::function("category", &category);
  emscripten::function("parse_range", &parse_range);
  emscripten::function("equity", &equity);
  emscripten::function("pushfold", &pushfold);
  emscripten::function("count_all_seven_card_hands", &count_all_seven_card_hands);
}
