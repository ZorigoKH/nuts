// Poker hand evaluation: 5, 6 and 7 cards -> a HandRank where higher is better.
//
// There are exactly 7,462 different 5-card poker hand values (all 2,598,960 five-card hands fall
// into these equivalence classes). A HandRank numbers them 1..7462:
//
//     1    = 7-5-4-3-2 offsuit (the worst high-card hand)
//     7462 = royal flush
//
// A 6- or 7-card hand is worth its best 5-card subset, so it gets the same kind of HandRank.
// Comparing two hands is comparing two integers.
//
// Two evaluators are provided:
//   * nuts::evaluate5/6/7 and nuts::evaluate(mask): the fast table-driven evaluator
//     (see src/evaluator.cpp for how the tables work).
//   * nuts::reference::evaluate5/6/7: a slow, obviously-correct evaluator that classifies hands
//     directly. It is used to generate the fast evaluator's tables and to test it.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "nuts/card.hpp"

namespace nuts {

using HandRank = std::uint16_t;

inline constexpr HandRank kInvalidHandRank = 0;  // returned for invalid input (see below)
inline constexpr HandRank kWorstHandRank = 1;
inline constexpr HandRank kBestHandRank = 7462;

enum class Category : std::uint8_t {
  HighCard,
  OnePair,
  TwoPair,
  ThreeOfAKind,
  Straight,
  Flush,
  FullHouse,
  FourOfAKind,
  StraightFlush,
};

inline constexpr int kNumCategories = 9;

// How many distinct HandRank values each category has, weakest category first. HandRanks are
// assigned in blocks: HighCard gets 1..1277, OnePair 1278..4137, and so on up to StraightFlush
// 7453..7462. The sizes add up to 7,462.
inline constexpr std::array<int, kNumCategories> kCategorySize = {
    1277,  // HighCard: C(13,5) rank sets minus the 10 straights
    2860,  // OnePair: 13 pair ranks * C(12,3) kickers
    858,   // TwoPair: C(13,2) pairs * 11 kickers
    858,   // ThreeOfAKind: 13 * C(12,2)
    10,    // Straight: 5-high (the wheel A-2-3-4-5) up to ace-high
    1277,  // Flush: same rank sets as HighCard
    156,   // FullHouse: 13 * 12
    156,   // FourOfAKind: 13 * 12
    10,    // StraightFlush: including the royal flush
};

// The smallest HandRank of each category.
inline constexpr std::array<HandRank, kNumCategories> kCategoryFirstRank = [] {
  std::array<HandRank, kNumCategories> first{};
  int next = kWorstHandRank;
  for (int i = 0; i < kNumCategories; ++i) {
    first[static_cast<std::size_t>(i)] = static_cast<HandRank>(next);
    next += kCategorySize[static_cast<std::size_t>(i)];
  }
  return first;
}();

static_assert(kCategoryFirstRank.back() + kCategorySize.back() - 1 == kBestHandRank);

// The category of a valid HandRank (1..7462).
constexpr Category category(HandRank rank) noexcept {
  int c = kNumCategories - 1;
  while (c > 0 && rank < kCategoryFirstRank[static_cast<std::size_t>(c)]) --c;
  return static_cast<Category>(c);
}

// "High Card", "One Pair", "Two Pair", "Three of a Kind", "Straight", "Flush", "Full House",
// "Four of a Kind", "Straight Flush".
std::string_view category_name(Category category) noexcept;

// ---------------------------------------------------------------------------------------------
// Fast evaluator.
//
// All functions return kInvalidHandRank (0) instead of a rank if the input is not a valid hand:
// a card id >= 52, a repeated card, or (for evaluate(mask)) fewer than 5 or more than 7 cards.
// They never throw. The lookup tables are built the first time any of them is called (about
// 40 ms on an Apple M3 in a Release build; see the "table build" line of nuts_bench) and are
// read-only afterwards, so the functions are safe to call from any thread.
// ---------------------------------------------------------------------------------------------
HandRank evaluate5(std::span<const Card, 5> cards) noexcept;
HandRank evaluate6(std::span<const Card, 6> cards) noexcept;
HandRank evaluate7(std::span<const Card, 7> cards) noexcept;

// Evaluates the cards in a 64-bit mask (bit `id` set = card `id` present); 5 to 7 cards.
HandRank evaluate(std::uint64_t mask) noexcept;

// ---------------------------------------------------------------------------------------------
// Reference evaluator: slow but simple. Same results and same invalid-input rule as above.
// ---------------------------------------------------------------------------------------------
namespace reference {

// Classifies the hand directly (category + kickers, including the A-2-3-4-5 "wheel" straight).
HandRank evaluate5(std::span<const Card, 5> cards) noexcept;

// The best of the 6 five-card subsets.
HandRank evaluate6(std::span<const Card, 6> cards) noexcept;

// The best of the 21 five-card subsets.
HandRank evaluate7(std::span<const Card, 7> cards) noexcept;

}  // namespace reference

// ---------------------------------------------------------------------------------------------
// Internals of the fast evaluator, exposed so the tests can check them directly.
// ---------------------------------------------------------------------------------------------
namespace detail {

// Number of ways to spread `num_cards` cards over the 13 ranks with at most 4 cards per rank,
// i.e. the number of different rank multisets of that size (6,175 for 5 cards, 18,395 for 6,
// 49,205 for 7). Valid for 0 <= num_cards <= 7.
std::uint32_t num_rank_multisets(int num_cards) noexcept;

// The "perfect hash" of a rank multiset: a dense index in 0..num_rank_multisets(n)-1, where n is
// the total of `counts` (counts[r] = number of cards of rank r, each 0..4, total 0..7). Returns
// UINT32_MAX if a count is above 4 or the total is above 7.
std::uint32_t rank_multiset_index(const std::array<std::uint8_t, kNumRanks>& counts) noexcept;

}  // namespace detail

}  // namespace nuts
