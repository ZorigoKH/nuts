// The C++ half of the Python package: the extension module nuts._core, built with nanobind.
// python/nuts/__init__.py wraps it and holds the documented API; this file only converts
// between Python and C++ values and calls the library.
//
//   * Errors: the library reports bad input with std::nullopt plus a message; here that becomes
//     a Python ValueError (nb::value_error) with the same message.
//   * The GIL (global interpreter lock: only the thread holding it may run Python code or touch
//     Python objects). The long computations (equity, the solvers) release it while they run,
//     so other Python threads are not blocked meanwhile. Released code must not touch any Python
//     object, so each such function first converts its arguments to C++ values, then releases
//     the GIL around the library call only, and builds the Python result after taking it back.
#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "nuts/card.hpp"
#include "nuts/embedded_data.hpp"
#include "nuts/equity.hpp"
#include "nuts/evaluator.hpp"
#include "nuts/kuhn.hpp"
#include "nuts/pushfold.hpp"
#include "nuts/range.hpp"

namespace nb = nanobind;
using namespace nb::literals;  // "name"_a for keyword argument names

namespace {

[[noreturn]] void throw_value_error(const std::string& message) {
  throw nb::value_error(message.c_str());
}

std::vector<nuts::Card> parse_cards_or_throw(const std::string& text, const char* what) {
  std::optional<std::vector<nuts::Card>> cards = nuts::parse_cards(text);
  if (!cards) {
    throw_value_error(std::string("cannot read ") + what + " \"" + text +
                      "\" (cards look like \"As Kd 7c\"; each card at most once)");
  }
  return std::move(*cards);
}

// ---- Hand evaluation ------------------------------------------------------------------------

int evaluate(const std::string& text) {
  const std::vector<nuts::Card> cards = parse_cards_or_throw(text, "the cards");
  if (cards.size() < 5 || cards.size() > 7) {
    throw_value_error("need 5, 6 or 7 cards, got " + std::to_string(cards.size()));
  }
  return nuts::evaluate(nuts::mask_of(cards));
}

std::string category(int rank) {
  if (rank < nuts::kWorstHandRank || rank > nuts::kBestHandRank) {
    throw_value_error("a hand rank is 1..7462, got " + std::to_string(rank));
  }
  return std::string(nuts::category_name(nuts::category(static_cast<nuts::HandRank>(rank))));
}

// ---- Ranges ---------------------------------------------------------------------------------

// "AsKs" if the text is two specific cards (in any accepted spelling, e.g. "as ks" or "Ks As"),
// otherwise None.
std::optional<std::string> parse_hole_cards(const std::string& text) {
  const std::optional<nuts::HoleCards> cards = nuts::parse_hole_cards(text);
  if (!cards) return std::nullopt;
  return nuts::hole_cards_to_string(*cards);
}

nuts::Range parse_range_or_throw(const std::string& text) {
  std::string error;
  std::optional<nuts::Range> range = nuts::parse_range(text, &error);
  if (!range) throw_value_error(error);
  return std::move(*range);
}

// A player's hand for Monte Carlo: two specific cards, in any spelling that exact equity accepts
// (parse_range() would read "As Ks" as two separate items), or else a range.
nuts::Range hand_to_range(const std::string& hand) {
  if (const std::optional<nuts::HoleCards> cards = nuts::parse_hole_cards(hand)) {
    return nuts::single_combo_range(*cards);
  }
  return parse_range_or_throw(hand);
}

std::vector<std::string> parse_range(const std::string& text) {
  std::vector<std::string> combos;
  for (const nuts::Combo& combo : parse_range_or_throw(text)) {
    combos.push_back(nuts::hole_cards_to_string(combo.cards));
  }
  return combos;
}

// ---- Equity ---------------------------------------------------------------------------------

// One dict per player: {"hand", "equity", "win", "tie", "std_error"}.
nb::list equity_to_python(const nuts::EquityResult& result, const std::vector<std::string>& hands) {
  nb::list players;
  for (std::size_t p = 0; p < result.players.size(); ++p) {
    const nuts::PlayerEquity& player = result.players[p];
    nb::dict entry;
    entry["hand"] = hands[p];
    entry["equity"] = player.equity;
    entry["win"] = player.win;
    entry["tie"] = player.tie;
    entry["std_error"] = player.std_error;
    players.append(entry);
  }
  return players;
}

nb::list equity_exact(const std::vector<std::string>& hands, const std::string& board_text) {
  std::vector<nuts::HoleCards> hole_cards;
  for (const std::string& hand : hands) {
    const std::optional<nuts::HoleCards> cards = nuts::parse_hole_cards(hand);
    if (!cards) {
      throw_value_error(
          "exact equity needs two specific cards per player, such as \"AsKd\"; got \"" + hand +
          "\"");
    }
    hole_cards.push_back(*cards);
  }
  const std::vector<nuts::Card> board = parse_cards_or_throw(board_text, "the board");

  std::optional<nuts::EquityResult> result;
  std::string error;
  {
    nb::gil_scoped_release release;  // up to 1.7 million boards to enumerate
    result = nuts::equity_exact(hole_cards, board, &error);
  }
  if (!result) throw_value_error(error);
  return equity_to_python(*result, hands);
}

nb::list equity_monte_carlo(const std::vector<std::string>& hands, const std::string& board_text,
                            std::uint64_t trials, std::uint64_t seed, int threads) {
  std::vector<nuts::Range> ranges;
  for (const std::string& hand : hands) ranges.push_back(hand_to_range(hand));
  const std::vector<nuts::Card> board = parse_cards_or_throw(board_text, "the board");

  std::optional<nuts::EquityResult> result;
  std::string error;
  {
    nb::gil_scoped_release release;
    result = nuts::equity_monte_carlo(ranges, board, trials, seed, threads, &error);
  }
  if (!result) throw_value_error(error);
  return equity_to_python(*result, hands);
}

// ---- Solvers --------------------------------------------------------------------------------

nb::dict kuhn_cfr(int iterations) {
  if (iterations < 1) throw_value_error("need at least 1 iteration");
  nuts::KuhnResult result;
  {
    nb::gil_scoped_release release;
    result = nuts::kuhn_cfr(iterations);
  }
  nb::dict strategy;  // information set name -> probability of "b" (bet or call)
  for (int i = 0; i < nuts::kKuhnNumInfoSets; ++i) {
    strategy[nuts::kuhn_info_set_name(i).c_str()] =
        result.average_strategy[static_cast<std::size_t>(i)];
  }
  nb::dict out;
  out["iterations"] = result.iterations;
  out["game_value"] = result.game_value;
  out["exploitability"] = result.exploitability;
  out["strategy"] = strategy;
  return out;
}

// A 13 x 13 chart as a list of rows: cell (row, column) is cell_value(13 * row + column), the
// hand class at that position (see include/nuts/pushfold.hpp).
template <typename CellValue>
nb::list chart(CellValue cell_value) {
  nb::list rows;
  for (int row = 0; row < nuts::kGridSize; ++row) {
    nb::list cells;
    for (int column = 0; column < nuts::kGridSize; ++column) {
      cells.append(cell_value(nuts::kGridSize * row + column));
    }
    rows.append(cells);
  }
  return rows;
}

nb::dict pushfold(double stack_bb) {
  std::optional<nuts::PushFoldSolution> solution;
  std::string error;
  {
    nb::gil_scoped_release release;
    const nuts::Equity169* equity = nuts::embedded_equity169(&error);
    if (equity) solution = nuts::solve_pushfold(*equity, stack_bb, {}, &error);
  }
  if (!solution) throw_value_error(error);
  // Cannot happen with the default options (every allowed stack converges), but an unconverged
  // strategy must never be returned as if it were an equilibrium.
  if (!solution->converged) throw std::runtime_error("the push/fold solver did not converge");

  const nuts::PushFoldStrategy& strategy = solution->strategy;
  nb::dict out;
  out["stack_bb"] = solution->stack_bb;
  out["iterations"] = solution->iterations;
  out["exploitability"] = solution->evaluation.exploitability;
  out["sb_ev"] = solution->evaluation.sb_value;
  out["hands"] = chart([](int c) { return nb::str(nuts::hand_class_name(c).c_str()); });
  out["shove"] =
      chart([&](int c) { return nb::float_(strategy.shove[static_cast<std::size_t>(c)]); });
  out["call"] =
      chart([&](int c) { return nb::float_(strategy.call[static_cast<std::size_t>(c)]); });
  return out;
}

}  // namespace

NB_MODULE(_core, m) {
  m.doc() = "C++ core of the nuts package; use the functions in `nuts` instead.";
  m.attr("MAX_THREADS") = nuts::kMaxThreads;    // the most threads equity_monte_carlo accepts
  m.attr("MAX_STACK_BB") = nuts::kMaxStackBb;  // the largest stack pushfold accepts

  m.def("evaluate", &evaluate, "cards"_a, "HandRank (1..7462, higher is better) of 5-7 cards.");
  m.def("category", &category, "rank"_a, "Category name of a HandRank.");
  m.def("parse_hole_cards", &parse_hole_cards, "text"_a,
        "Two specific cards as \"AsKs\", or None if the text is not two cards.");
  m.def("parse_range", &parse_range, "text"_a, "The combos of a range, as \"AsKs\" strings.");
  m.def("equity_exact", &equity_exact, "hands"_a, "board"_a,
        "Exact equity by enumerating every completion of the board.");
  m.def("equity_monte_carlo", &equity_monte_carlo, "hands"_a, "board"_a, "trials"_a, "seed"_a,
        "threads"_a, "Monte Carlo equity for ranges.");
  m.def("kuhn_cfr", &kuhn_cfr, "iterations"_a, "Vanilla CFR for Kuhn poker.");
  m.def("pushfold", &pushfold, "stack_bb"_a, "Heads-up push/fold equilibrium (CFR+).");
}
