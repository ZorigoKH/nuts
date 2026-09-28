// Hand evaluation. The file has four parts:
//
//   1. The reference evaluator: classify a 5-card hand directly; 6/7 cards = best 5-card subset.
//   2. The rank-multiset "perfect hash" used by the fast evaluator.
//   3. Building the fast evaluator's lookup tables (from the reference evaluator).
//   4. The fast evaluator itself.
//
// How the fast evaluator works, in short:
//
//   * Flush hands. If some suit has 5 or more cards, the answer only depends on which ranks that
//     suit has. A 13-bit mask of those ranks indexes an 8,192-entry table (`flush`) holding the
//     best hand those ranks make (a flush, or a straight flush when five of them are in a row).
//
//   * All other hands. Suits no longer matter, only how many cards of each rank there are: a
//     vector of 13 counts, each 0..4, adding up to n (5, 6 or 7). We number all such vectors
//     0, 1, 2, ... in lexicographic order (a "combinatorial ranking of multisets"); there are
//     6,175 of them for n = 5, 18,395 for n = 6 and 49,205 for n = 7. That number is a perfect
//     hash: every possible rank multiset gets its own slot, with no gaps and no collisions, so a
//     plain array (`nonflush[n]`) holds the answer for each one.
//
// The same scheme is used by HenryRLee's PokerHandEvaluator. All tables are computed at startup
// from the reference evaluator, so the repository contains no binary tables.
#include "nuts/evaluator.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace nuts {

std::string_view category_name(Category category) noexcept {
  switch (category) {
    case Category::HighCard:
      return "High Card";
    case Category::OnePair:
      return "One Pair";
    case Category::TwoPair:
      return "Two Pair";
    case Category::ThreeOfAKind:
      return "Three of a Kind";
    case Category::Straight:
      return "Straight";
    case Category::Flush:
      return "Flush";
    case Category::FullHouse:
      return "Full House";
    case Category::FourOfAKind:
      return "Four of a Kind";
    case Category::StraightFlush:
      return "Straight Flush";
  }
  return "Unknown";
}

namespace {

// counts[r] = number of cards of rank r.
using RankCounts = std::array<std::uint8_t, kNumRanks>;

// The largest hand we evaluate.
constexpr int kMaxCards = 7;
// Number of 7-card rank multisets, i.e. the size of the 7-card table; checked below, twice.
constexpr std::uint32_t kNumSevenCardRankMultisets = 49205;

// A check that stays on in Release builds (unlike assert). Only used while building tables at
// startup, never in the per-hand code, so it costs nothing where speed matters.
void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "nuts: internal check failed: %s\n", what);
    std::abort();
  }
}

// True if every card id is < 52 and no card repeats.
bool are_valid_distinct_cards(std::span<const Card> cards) {
  return static_cast<std::size_t>(std::popcount(mask_of(cards))) == cards.size();
}

// Calls visit(counts) for every RankCounts whose entries are 0..4 and add up to num_cards, in
// lexicographic order of (counts[0], counts[1], ..., counts[12]).
template <typename Visit>
void for_each_rank_multiset(int num_cards, Visit&& visit) {
  RankCounts counts{};
  // Choose counts[rank] for rank = 0, 1, ..., 12 in turn; `remaining` cards are still unplaced.
  auto choose = [&](auto& self, int rank, int remaining) -> void {
    if (rank == kNumRanks) {
      if (remaining == 0) visit(counts);
      return;
    }
    for (int count = 0; count <= std::min(4, remaining); ++count) {
      counts[static_cast<std::size_t>(rank)] = static_cast<std::uint8_t>(count);
      self(self, rank + 1, remaining - count);
    }
    counts[static_cast<std::size_t>(rank)] = 0;
  };
  choose(choose, 0, num_cards);
}

// =============================================================================================
// Part 1: the reference evaluator.
// =============================================================================================

// A Score is an integer that orders 5-card hands correctly but is not dense (it has gaps):
//
//   bits 20..23   category (0 = HighCard ... 8 = StraightFlush)
//   bits 16..19   first tie-break rank   (most important)
//   bits 12..15   second tie-break rank
//   ...           ...
//   bits  0..3    fifth tie-break rank   (unused fields are 0)
//
// The category is in the highest bits and each tie-break rank sits in a fixed field, so comparing
// two scores as plain integers compares the two hands.
using Score = std::uint32_t;

// Score of a 5-card hand from its rank counts (adding up to 5) and whether it is a flush.
Score score_from_counts(const RankCounts& counts, bool is_flush) {
  // Group the cards by rank and order the groups by size, then by rank, largest first, e.g.
  //   K-K-K-7-7  ->  {3 x K}, {2 x 7}
  //   Q-Q-9-9-A  ->  {2 x Q}, {2 x 9}, {1 x A}
  //   A-Q-9-5-2  ->  {1 x A}, {1 x Q}, {1 x 9}, {1 x 5}, {1 x 2}
  // This is exactly the order in which the ranks break ties.
  struct Group {
    int size;
    int rank;
  };
  std::array<Group, 5> groups{};
  std::size_t num_groups = 0;
  for (int rank = kAce; rank >= kTwo; --rank) {
    const int size = counts[static_cast<std::size_t>(rank)];
    if (size > 0) groups[num_groups++] = {size, rank};
  }
  std::sort(groups.begin(), groups.begin() + static_cast<std::ptrdiff_t>(num_groups),
            [](const Group& a, const Group& b) {
              return a.size != b.size ? a.size > b.size : a.rank > b.rank;
            });

  // A straight is five different ranks in a row. A-2-3-4-5 (the "wheel") is also a straight,
  // the lowest one: its high card is the five, not the ace.
  bool is_straight = false;
  int straight_high = 0;
  if (num_groups == 5) {
    if (groups[0].rank - groups[4].rank == 4) {
      is_straight = true;
      straight_high = groups[0].rank;
    } else if (groups[0].rank == kAce && groups[1].rank == kFive) {  // the others are 4, 3, 2
      is_straight = true;
      straight_high = kFive;
    }
  }

  Category category;
  if (is_straight && is_flush) {
    category = Category::StraightFlush;
  } else if (groups[0].size == 4) {
    category = Category::FourOfAKind;
  } else if (groups[0].size == 3 && groups[1].size == 2) {
    category = Category::FullHouse;
  } else if (is_flush) {
    category = Category::Flush;
  } else if (is_straight) {
    category = Category::Straight;
  } else if (groups[0].size == 3) {
    category = Category::ThreeOfAKind;
  } else if (groups[0].size == 2 && groups[1].size == 2) {
    category = Category::TwoPair;
  } else if (groups[0].size == 2) {
    category = Category::OnePair;
  } else {
    category = Category::HighCard;
  }

  Score score = static_cast<Score>(category) << 20;
  if (is_straight) {
    // Straights (and straight flushes) are ranked by their high card alone.
    return score | static_cast<Score>(straight_high) << 16;
  }
  for (std::size_t g = 0; g < num_groups; ++g) {
    score |= static_cast<Score>(groups[g].rank) << (16 - 4 * g);
  }
  return score;
}

// All 7,462 possible scores, sorted. A hand's HandRank is 1 + the position of its score here,
// which turns the gappy scores into the dense numbers 1..7462.
const std::vector<Score>& sorted_scores() {
  static const std::vector<Score> scores = [] {
    std::vector<Score> all;
    // A non-flush 5-card hand's value depends only on its rank counts (6,175 possibilities), and
    // a flush's value only on its five different ranks (1,287 possibilities). Between them they
    // produce every hand value exactly once: 6,175 + 1,287 = 7,462.
    for_each_rank_multiset(5, [&](const RankCounts& counts) {
      all.push_back(score_from_counts(counts, /*is_flush=*/false));
      const bool five_different_ranks = std::ranges::all_of(counts, [](int c) { return c <= 1; });
      if (five_different_ranks) all.push_back(score_from_counts(counts, /*is_flush=*/true));
    });
    std::ranges::sort(all);
    check(std::ranges::adjacent_find(all) == all.end(), "5-card scores are distinct");
    check(all.size() == kBestHandRank, "there are 7,462 distinct 5-card hand values");
    return all;
  }();
  return scores;
}

HandRank rank_from_score(Score score) {
  const std::vector<Score>& scores = sorted_scores();
  const auto it = std::ranges::lower_bound(scores, score);
  return static_cast<HandRank>(it - scores.begin() + 1);
}

// Score of five valid, distinct cards.
Score reference_score5(std::span<const Card, 5> cards) {
  RankCounts counts{};
  bool is_flush = true;
  for (const Card card : cards) {
    ++counts[static_cast<std::size_t>(rank_of(card))];
    is_flush = is_flush && suit_of(card) == suit_of(cards[0]);
  }
  return score_from_counts(counts, is_flush);
}

// The ways to choose 5 of N card positions, as lists of positions, computed at compile time:
// 1 for N = 5, 6 for N = 6, 21 for N = 7. Every N-bit number with exactly five 1-bits picks one.
template <std::size_t N>
constexpr auto kFiveCardSubsets = [] {
  constexpr std::size_t kNumSubsets = N * (N - 1) * (N - 2) * (N - 3) * (N - 4) / 120;  // C(N, 5)
  std::array<std::array<std::size_t, 5>, kNumSubsets> subsets{};
  std::size_t s = 0;
  for (unsigned keep = 0; keep < (1u << N); ++keep) {
    if (std::popcount(keep) != 5) continue;
    std::size_t k = 0;
    for (std::size_t i = 0; i < N; ++i) {
      if ((keep >> i) & 1u) subsets[s][k++] = i;
    }
    ++s;
  }
  return subsets;
}();

// Best HandRank among all 5-card subsets of N cards.
template <std::size_t N>
HandRank reference_best_of_subsets(std::span<const Card, N> cards) {
  if (!are_valid_distinct_cards(cards)) return kInvalidHandRank;
  Score best = 0;
  for (const auto& positions : kFiveCardSubsets<N>) {
    const std::array<Card, 5> five = {cards[positions[0]], cards[positions[1]], cards[positions[2]],
                                      cards[positions[3]], cards[positions[4]]};
    best = std::max(best, reference_score5(five));
  }
  // Scores and HandRanks are in the same order, so the best score gives the best HandRank.
  return rank_from_score(best);
}

// Reference HandRank of 5, 6 or 7 cards held in a vector (used while building tables).
HandRank reference_evaluate_any(const std::vector<Card>& cards) {
  switch (cards.size()) {
    case 5:
      return reference_best_of_subsets(std::span<const Card, 5>(cards.data(), 5));
    case 6:
      return reference_best_of_subsets(std::span<const Card, 6>(cards.data(), 6));
    case 7:
      return reference_best_of_subsets(std::span<const Card, 7>(cards.data(), 7));
    default:
      return kInvalidHandRank;
  }
}

// =============================================================================================
// Part 2: the rank-multiset perfect hash.
// =============================================================================================

// kNumRankMultisets[r][k] = number of ways to put k cards into r ranks, 0..4 cards per rank.
// This is the dp table. With r ranks available, the first of them gets c = 0..4 cards and the
// other k - c cards go into the remaining r - 1 ranks:
//
//     kNumRankMultisets[0][0] = 1,   kNumRankMultisets[0][k > 0] = 0
//     kNumRankMultisets[r][k] = sum over c = 0..min(4, k) of kNumRankMultisets[r - 1][k - c]
constexpr auto kNumRankMultisets = [] {
  std::array<std::array<std::uint32_t, kMaxCards + 1>, kNumRanks + 1> ways{};
  ways[0][0] = 1;
  for (std::size_t r = 1; r <= kNumRanks; ++r) {
    for (std::size_t k = 0; k <= kMaxCards; ++k) {
      for (std::size_t c = 0; c <= std::min<std::size_t>(4, k); ++c) {
        ways[r][k] += ways[r - 1][k - c];
      }
    }
  }
  return ways;
}();

static_assert(kNumRankMultisets[kNumRanks][5] == 6175);
static_assert(kNumRankMultisets[kNumRanks][6] == 18395);
static_assert(kNumRankMultisets[kNumRanks][7] == kNumSevenCardRankMultisets);

// The index of a rank multiset is its position in lexicographic order of (counts[0], ...,
// counts[12]), i.e. how many valid count vectors (same total n) come before it. Going through
// the ranks in order, a vector comes before ours if it agrees on all lower ranks and puts fewer
// cards into the current rank; for each such smaller count s, the rest (remaining - s cards) can
// be placed in the higher ranks in kNumRankMultisets[ranks above][remaining - s] ways.
//
// kIndexOffset[rank][remaining][count] precomputes that sum, where `remaining` is the number of
// cards not yet placed in ranks below `rank`. Then:
//
//     index = sum over ranks of kIndexOffset[rank][remaining][counts[rank]]
//
// A rank with count 0 adds nothing (the sum over smaller counts is empty).
constexpr auto kIndexOffset = [] {
  std::array<std::array<std::array<std::uint32_t, 5>, kMaxCards + 1>, kNumRanks> offset{};
  for (std::size_t rank = 0; rank < kNumRanks; ++rank) {
    const std::size_t ranks_above = kNumRanks - 1 - rank;
    for (std::size_t remaining = 0; remaining <= kMaxCards; ++remaining) {
      for (std::size_t count = 0; count <= 4; ++count) {
        for (std::size_t smaller = 0; smaller < count && smaller <= remaining; ++smaller) {
          offset[rank][remaining][count] += kNumRankMultisets[ranks_above][remaining - smaller];
        }
      }
    }
  }
  return offset;
}();

// The index from counts packed 4 bits per rank (bits 4r..4r+3 = count of rank r, each 0..4)
// that add up to num_cards (0..7).
inline std::uint32_t multiset_index(std::uint64_t packed_counts, int num_cards) {
  std::uint32_t index = 0;
  auto remaining = static_cast<std::size_t>(num_cards);
  for (std::size_t rank = 0; rank < kNumRanks; ++rank) {
    const auto count = static_cast<std::size_t>((packed_counts >> (4 * rank)) & 0xF);
    index += kIndexOffset[rank][remaining][count];
    remaining -= count;
  }
  return index;
}

std::uint64_t pack_counts(const RankCounts& counts) {
  std::uint64_t packed = 0;
  for (std::size_t rank = 0; rank < kNumRanks; ++rank) {
    packed |= std::uint64_t{counts[rank]} << (4 * rank);
  }
  return packed;
}

// =============================================================================================
// Part 3: the lookup tables.
// =============================================================================================

struct Tables {
  // flush[m] = best HandRank of 5..7 cards of one suit whose ranks are the bits of the 13-bit
  // mask m. Entries for masks with fewer than 5 or more than 7 bits are never read (0).
  std::array<HandRank, 1 << kNumRanks> flush{};

  // nonflush[n][i] = best HandRank of an n-card hand (n = 5, 6, 7) whose rank multiset has index
  // i, for a hand where no suit has 5 or more cards. nonflush[0..4] are unused.
  std::array<std::vector<HandRank>, kMaxCards + 1> nonflush;
};

Tables build_tables() {
  Tables tables;

  // Flush table: put the ranks of each mask into one suit (clubs) and ask the reference
  // evaluator. Every 5-card subset is then a flush, so it finds the best flush / straight flush.
  for (unsigned mask = 0; mask < tables.flush.size(); ++mask) {
    const int num_cards = std::popcount(mask);
    if (num_cards < 5 || num_cards > kMaxCards) continue;
    std::vector<Card> cards;
    for (int rank = 0; rank < kNumRanks; ++rank) {
      if ((mask >> rank) & 1u) cards.push_back(make_card(rank, kClubs));
    }
    tables.flush[mask] = reference_evaluate_any(cards);
  }

  // Non-flush tables: for every rank multiset of size n, build one concrete hand with those
  // ranks and no flush, and ask the reference evaluator. Suits are handed out c, d, h, s, c, d, h
  // in turn: the copies of one rank are next to each other (at most 4), so they get different
  // suits, and no suit gets more than 2 of the 7 cards, so no 5-card subset is a flush. Any other
  // hand with the same ranks and no 5 cards of one suit has the same value, because without a
  // flush a 5-card hand's value depends only on its ranks.
  for (int num_cards = 5; num_cards <= kMaxCards; ++num_cards) {
    const std::uint32_t table_size =
        kNumRankMultisets[kNumRanks][static_cast<std::size_t>(num_cards)];
    std::vector<HandRank>& table = tables.nonflush[static_cast<std::size_t>(num_cards)];
    table.assign(table_size, kInvalidHandRank);

    std::uint32_t num_visited = 0;
    for_each_rank_multiset(num_cards, [&](const RankCounts& counts) {
      std::vector<Card> cards;
      for (int rank = 0; rank < kNumRanks; ++rank) {
        for (int copy = 0; copy < counts[static_cast<std::size_t>(rank)]; ++copy) {
          cards.push_back(make_card(rank, static_cast<int>(cards.size() % kNumSuits)));
        }
      }
      const std::uint32_t index = multiset_index(pack_counts(counts), num_cards);
      // Each index must be in range and used once: the hash really is perfect.
      check(index < table_size && table[index] == kInvalidHandRank,
            "multiset index is a bijection");
      table[index] = reference_evaluate_any(cards);
      ++num_visited;
    });
    // Enumeration agrees with the dp count, so every slot was filled exactly once.
    check(num_visited == table_size, "enumerated multisets == dp count");
    if (num_cards == 7) check(num_visited == kNumSevenCardRankMultisets, "49,205 7-card multisets");
  }
  return tables;
}

// The tables, built on first use. C++ guarantees that a function-local static is initialized
// exactly once, even if several threads call this at the same time.
const Tables& tables() {
  static const Tables instance = build_tables();
  return instance;
}

// =============================================================================================
// Part 4: the fast evaluator.
// =============================================================================================

// Bits 0, 4, 8, ..., 48: the lowest bit of each rank's 4-bit group, i.e. the clubs. Shifting a
// card mask right by `suit` and AND-ing with this keeps exactly the cards of that suit.
constexpr std::uint64_t kClubsMask = 0x0001'1111'1111'1111;

// Collects bits 0, 4, 8, ..., 48 of x into a 13-bit mask (bit r of the result = bit 4r of x).
inline unsigned gather_rank_bits(std::uint64_t x) {
  unsigned ranks = 0;
  for (int rank = 0; rank < kNumRanks; ++rank) {
    ranks |= static_cast<unsigned>((x >> (4 * rank)) & 1u) << rank;
  }
  return ranks;
}

// Replaces each 4-bit group of a card mask (the four suits of one rank) by its number of set
// bits, giving the rank counts packed 4 bits per rank. This is the first half of the classic
// "SWAR" popcount (SIMD within a register):
inline std::uint64_t packed_rank_counts(std::uint64_t mask) {
  // Every 2-bit field "ab" becomes a + b: as a number it is 2a + b, and subtracting a leaves
  // a + b (00 -> 00, 01 -> 01, 10 -> 01, 11 -> 10). No borrow crosses a field.
  std::uint64_t x = mask - ((mask >> 1) & 0x5555'5555'5555'5555);
  // Add the two 2-bit fields of every 4-bit group. The sum is at most 4, so it fits.
  x = (x & 0x3333'3333'3333'3333) + ((x >> 2) & 0x3333'3333'3333'3333);
  return x;
}

// mask: a valid card mask with exactly num_cards (5..7) cards.
inline HandRank evaluate_valid_mask(std::uint64_t mask, int num_cards) {
  const Tables& t = tables();

  // Step 1: flushes. With 5 or more cards of one suit (and at most 7 cards in total, only one
  // suit can have that many), the best hand is a flush or a straight flush made from that suit:
  //   * The only hands above a flush are full house, four of a kind and straight flush.
  //   * The suited cards all have different ranks, so the extra copies needed for a full house
  //     or four of a kind must come from the at most 2 other cards. A full house needs at least
  //     3 of them (2 more for the three of a kind, 1 more for the pair) and four of a kind needs
  //     3. So neither is possible, and the flush table already includes straight flushes.
  for (int suit = 0; suit < kNumSuits; ++suit) {
    const std::uint64_t suited = (mask >> suit) & kClubsMask;
    if (std::popcount(suited) >= 5) return t.flush[gather_rank_bits(suited)];
  }

  // Step 2: no flush, so only the rank counts matter; look up their perfect-hash index.
  const std::uint64_t counts = packed_rank_counts(mask);
  return t.nonflush[static_cast<std::size_t>(num_cards)][multiset_index(counts, num_cards)];
}

template <std::size_t N>
HandRank evaluate_cards(std::span<const Card, N> cards) {
  std::uint64_t mask = 0;
  for (const Card card : cards) mask |= card_bit(card);  // an invalid id adds no bit
  // Fewer than N bits means a repeated card or an invalid id.
  if (std::popcount(mask) != static_cast<int>(N)) return kInvalidHandRank;
  return evaluate_valid_mask(mask, static_cast<int>(N));
}

}  // namespace

HandRank evaluate5(std::span<const Card, 5> cards) noexcept { return evaluate_cards(cards); }
HandRank evaluate6(std::span<const Card, 6> cards) noexcept { return evaluate_cards(cards); }
HandRank evaluate7(std::span<const Card, 7> cards) noexcept { return evaluate_cards(cards); }

HandRank evaluate(std::uint64_t mask) noexcept {
  const int num_cards = std::popcount(mask);
  if (num_cards < 5 || num_cards > kMaxCards || (mask & ~kFullDeckMask) != 0) {
    return kInvalidHandRank;
  }
  return evaluate_valid_mask(mask, num_cards);
}

namespace reference {

HandRank evaluate5(std::span<const Card, 5> cards) noexcept {
  if (!are_valid_distinct_cards(cards)) return kInvalidHandRank;
  return rank_from_score(reference_score5(cards));
}
HandRank evaluate6(std::span<const Card, 6> cards) noexcept {
  return reference_best_of_subsets(cards);
}
HandRank evaluate7(std::span<const Card, 7> cards) noexcept {
  return reference_best_of_subsets(cards);
}

}  // namespace reference

namespace detail {

std::uint32_t num_rank_multisets(int num_cards) noexcept {
  if (num_cards < 0 || num_cards > kMaxCards) return 0;
  return kNumRankMultisets[kNumRanks][static_cast<std::size_t>(num_cards)];
}

std::uint32_t rank_multiset_index(const RankCounts& counts) noexcept {
  int total = 0;
  for (const std::uint8_t count : counts) {
    if (count > 4) return UINT32_MAX;  // not a valid rank multiset
    total += count;
  }
  if (total > kMaxCards) return UINT32_MAX;
  return multiset_index(pack_counts(counts), total);
}

}  // namespace detail

}  // namespace nuts
