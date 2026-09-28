#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

#include "nuts/card.hpp"
#include "nuts/evaluator.hpp"

namespace {

using nuts::Card;
using nuts::Category;
using nuts::HandRank;

// ---- Helpers ----------------------------------------------------------------------------------

// Parses exactly N cards, e.g. hand<5>("As Ks Qs Js Ts").
template <std::size_t N>
std::array<Card, N> hand(std::string_view text) {
  const auto cards = nuts::parse_cards(text);
  REQUIRE(cards.has_value());
  REQUIRE(cards->size() == N);
  std::array<Card, N> result{};
  std::copy(cards->begin(), cards->end(), result.begin());
  return result;
}

HandRank rank5(std::string_view text) { return nuts::evaluate5(hand<5>(text)); }
HandRank rank7(std::string_view text) { return nuts::evaluate7(hand<7>(text)); }

HandRank first_rank_of(Category category) {
  return nuts::kCategoryFirstRank[static_cast<std::size_t>(category)];
}

// Calls visit(cards) for every N-card subset of the deck, in increasing lexicographic order:
// {0,1,2,3,4}, {0,1,2,3,5}, ..., {47,48,49,50,51}.
template <std::size_t N, typename Visit>
void for_each_hand(Visit&& visit) {
  std::array<Card, N> cards{};
  for (std::size_t i = 0; i < N; ++i) cards[i] = static_cast<Card>(i);
  while (true) {
    visit(std::as_const(cards));
    // Move to the next subset: find the rightmost card that can still go up, increase it, and
    // put the cards after it right behind it.
    constexpr std::size_t kDeckSize = nuts::kNumCards;
    std::size_t i = N;
    while (i > 0 && cards[i - 1] == kDeckSize - N + (i - 1)) --i;
    if (i == 0) return;  // that was the last subset
    ++cards[i - 1];
    for (std::size_t j = i; j < N; ++j) cards[j] = static_cast<Card>(cards[j - 1] + 1);
  }
}

// N distinct random cards: the first N steps of a Fisher-Yates shuffle of the deck. (The modulo
// bias of `rng() % k` is about 1e-17 here and does not matter for a test.)
template <std::size_t N>
std::array<Card, N> random_hand(std::mt19937_64& rng) {
  std::array<Card, nuts::kNumCards> deck{};
  std::iota(deck.begin(), deck.end(), Card{0});
  for (std::size_t i = 0; i < N; ++i) {
    const std::size_t j = i + rng() % (deck.size() - i);
    std::swap(deck[i], deck[j]);
  }
  std::array<Card, N> result{};
  std::copy_n(deck.begin(), N, result.begin());
  return result;
}

// Tallies of a set of hands: how many hands per category (royal flushes counted separately
// from other straight flushes) and which HandRank values were seen.
struct Tally {
  std::uint64_t hands = 0;
  std::uint64_t royal_flushes = 0;
  std::array<std::uint64_t, nuts::kNumCategories> per_category{};  // royals are in StraightFlush
  std::vector<bool> seen = std::vector<bool>(nuts::kBestHandRank + 1, false);

  void add(HandRank rank) {
    ++hands;
    ++per_category[static_cast<std::size_t>(nuts::category(rank))];
    if (rank == nuts::kBestHandRank) ++royal_flushes;
    seen[rank] = true;
  }
  std::uint64_t count(Category c) const { return per_category[static_cast<std::size_t>(c)]; }
  int distinct_values() const { return static_cast<int>(std::ranges::count(seen, true)); }
};

// Enumerates every 13-rank count vector (each count 0..4) with the given total, in lexicographic
// order, independently of the evaluator's own enumeration.
void enumerate_counts(int total, std::array<std::uint8_t, nuts::kNumRanks>& counts, int rank,
                      std::vector<std::array<std::uint8_t, nuts::kNumRanks>>& out) {
  if (rank == nuts::kNumRanks) {
    if (total == 0) out.push_back(counts);
    return;
  }
  for (int c = 0; c <= 4 && c <= total; ++c) {
    counts[static_cast<std::size_t>(rank)] = static_cast<std::uint8_t>(c);
    enumerate_counts(total - c, counts, rank + 1, out);
  }
  counts[static_cast<std::size_t>(rank)] = 0;
}

}  // namespace

// ---- Categories and single hands ------------------------------------------------------------

TEST_CASE("category boundaries", "[evaluator]") {
  CHECK(nuts::kCategoryFirstRank == std::array<HandRank, nuts::kNumCategories>{
                                        1, 1278, 4138, 4996, 5854, 5864, 7141, 7297, 7453});
  CHECK(nuts::category(1) == Category::HighCard);
  CHECK(nuts::category(1277) == Category::HighCard);
  CHECK(nuts::category(1278) == Category::OnePair);
  CHECK(nuts::category(5853) == Category::ThreeOfAKind);
  CHECK(nuts::category(5854) == Category::Straight);
  CHECK(nuts::category(7452) == Category::FourOfAKind);
  CHECK(nuts::category(7453) == Category::StraightFlush);
  CHECK(nuts::category(7462) == Category::StraightFlush);

  CHECK(nuts::category_name(Category::HighCard) == "High Card");
  CHECK(nuts::category_name(Category::ThreeOfAKind) == "Three of a Kind");
  CHECK(nuts::category_name(Category::StraightFlush) == "Straight Flush");
}

TEST_CASE("known 5-card hands", "[evaluator]") {
  CHECK(rank5("7c 5d 4h 3s 2c") == nuts::kWorstHandRank);
  CHECK(rank5("As Ks Qs Js Ts") == nuts::kBestHandRank);
  CHECK(rank5("Ts Js Qs Ks As") == nuts::kBestHandRank);  // card order does not matter

  // The wheel A-2-3-4-5 is the lowest straight; 5-4-3-2-A suited is the lowest straight flush.
  CHECK(rank5("5c 4d 3h 2s Ac") == first_rank_of(Category::Straight));
  CHECK(rank5("6c 5d 4h 3s 2c") == first_rank_of(Category::Straight) + 1);
  CHECK(rank5("Ac Kd Qh Js Tc") == first_rank_of(Category::Flush) - 1);  // the highest straight
  CHECK(rank5("5s 4s 3s 2s As") == first_rank_of(Category::StraightFlush));
  // Q-K-A-2-3 does not wrap around: it is ace high.
  CHECK(nuts::category(rank5("Qc Kd Ah 2s 3c")) == Category::HighCard);

  CHECK(nuts::category(rank5("Kc Kd Kh Ks 2c")) == Category::FourOfAKind);
  CHECK(nuts::category(rank5("Kc Kd Kh 2s 2c")) == Category::FullHouse);
  CHECK(nuts::category(rank5("Ah Jh 8h 4h 2h")) == Category::Flush);
  CHECK(nuts::category(rank5("9c 9d 9h As 2c")) == Category::ThreeOfAKind);
  CHECK(nuts::category(rank5("9c 9d 4h 4s Ac")) == Category::TwoPair);
  CHECK(nuts::category(rank5("9c 9d 4h 3s Ac")) == Category::OnePair);
  CHECK(nuts::category(rank5("Ac Qd 9h 5s 3c")) == Category::HighCard);

  // Tie-breaks inside a category.
  CHECK(rank5("Ac Ad 2h 3s 4c") > rank5("Kc Kd Ah Qs Jc"));  // pair rank first
  CHECK(rank5("Ac Ad Kh 3s 2c") > rank5("Ah As Qd Jc Tc"));  // then kickers
  CHECK(rank5("Kc Kd 2h 2s 3c") > rank5("Qc Qd Jh Js Ac"));  // top pair of two pair decides
  CHECK(rank5("Kc Kd 3h 3s 2c") > rank5("Kh Ks 2d 2c Ac"));  // then the second pair
  CHECK(rank5("Kc Kd Kh 2s 2c") > rank5("Qc Qd Qh As Ac"));  // trips decide a full house
  CHECK(rank5("3c 3d 3h 2s 2c") > rank5("2c 2d 2h As Ac"));
  CHECK(rank5("Ah Kh Qh Jh 9h") > rank5("Ac Kc Qc Jc 8c"));   // flush: compare all five ranks
  CHECK(rank5("Ah Kh Qh Jh 9h") == rank5("Ac Kc Qc Jc 9c"));  // suits never break ties
  CHECK(rank5("2c 2d 2h 2s 3c") > rank5("Ac Ad Ah Ks Kc"));   // any quads beat any full house
}

TEST_CASE("known 7-card hands", "[evaluator]") {
  CHECK(rank7("As Ks Qs Js Ts 2c 3d") == nuts::kBestHandRank);
  // A flush beats the straight that is also on board.
  CHECK(nuts::category(rank7("2h 3h 4h 5h 7h 6c 8d")) == Category::Flush);
  // Six hearts: the best five hearts count.
  CHECK(rank7("Ah Kh 9h 7h 5h 3h 2c") == rank5("Ah Kh 9h 7h 5h"));
  // A straight flush hidden in a flush.
  CHECK(nuts::category(rank7("9d 8d 7d 6d 5d Ad Ac")) == Category::StraightFlush);
  // Two trips make a full house (the higher trips plus a pair from the lower ones).
  CHECK(rank7("Kc Kd Kh 7s 7c 7d 2h") == rank5("Kc Kd Kh 7s 7c"));
  // Three pairs: the best two pairs plus the best kicker (here the third pair's rank).
  CHECK(rank7("Ac Ad Kh Ks Qc Qd 2h") == rank5("Ac Ad Kh Ks Qc"));
  // Four of a kind plus a pair: the kicker is simply the best other card.
  CHECK(rank7("9c 9d 9h 9s Ac Ad 2h") == rank5("9c 9d 9h 9s Ac"));
}

TEST_CASE("invalid input gives kInvalidHandRank", "[evaluator]") {
  const std::array<Card, 5> repeated = {0, 0, 1, 2, 3};
  const std::array<Card, 7> bad_id = {0, 1, 2, 3, 4, 5, 52};
  CHECK(nuts::evaluate5(repeated) == nuts::kInvalidHandRank);
  CHECK(nuts::evaluate7(bad_id) == nuts::kInvalidHandRank);
  CHECK(nuts::reference::evaluate5(repeated) == nuts::kInvalidHandRank);
  CHECK(nuts::reference::evaluate7(bad_id) == nuts::kInvalidHandRank);

  CHECK(nuts::evaluate(0) == nuts::kInvalidHandRank);
  CHECK(nuts::evaluate(0b1111) == nuts::kInvalidHandRank);       // 4 cards
  CHECK(nuts::evaluate(0b1111'1111) == nuts::kInvalidHandRank);  // 8 cards
  CHECK(nuts::evaluate(0b11111) != nuts::kInvalidHandRank);      // 2c 2d 2h 2s 3c: valid
  // Five cards, but one of them is bit 52, which is not a card.
  CHECK(nuts::evaluate(0b1111 | std::uint64_t{1} << 52) == nuts::kInvalidHandRank);
}

// ---- The rank-multiset perfect hash ---------------------------------------------------------

TEST_CASE("rank multiset index is a perfect hash", "[evaluator]") {
  for (int total = 0; total <= 7; ++total) {
    std::vector<std::array<std::uint8_t, nuts::kNumRanks>> all;
    std::array<std::uint8_t, nuts::kNumRanks> counts{};
    enumerate_counts(total, counts, 0, all);
    INFO("total = " << total);
    REQUIRE(all.size() == nuts::detail::num_rank_multisets(total));
    // In lexicographic order the index counts up 0, 1, 2, ...: no gaps, no collisions.
    for (std::size_t i = 0; i < all.size(); ++i) {
      REQUIRE(nuts::detail::rank_multiset_index(all[i]) == i);
    }
  }
  // The table sizes used by the evaluator, confirmed by enumeration above.
  CHECK(nuts::detail::num_rank_multisets(5) == 6175);
  CHECK(nuts::detail::num_rank_multisets(6) == 18395);
  CHECK(nuts::detail::num_rank_multisets(7) == 49205);

  std::array<std::uint8_t, nuts::kNumRanks> five_aces{};
  five_aces[nuts::kAce] = 5;
  CHECK(nuts::detail::rank_multiset_index(five_aces) == UINT32_MAX);
}

// ---- Exhaustive and random comparisons ------------------------------------------------------

TEST_CASE("all 2,598,960 five-card hands", "[evaluator]") {
  Tally tally;
  std::array<int, nuts::kNumCategories> distinct_per_category{};
  std::uint64_t mismatches = 0;
  for_each_hand<5>([&](const std::array<Card, 5>& cards) {
    const HandRank fast = nuts::evaluate5(cards);
    const HandRank mask_based = nuts::evaluate(nuts::mask_of(cards));
    const HandRank slow = nuts::reference::evaluate5(cards);
    if (fast != slow || mask_based != slow) ++mismatches;
    tally.add(fast);
  });

  CHECK(mismatches == 0);
  CHECK(tally.hands == 2'598'960);
  CHECK(tally.royal_flushes == 4);
  CHECK(tally.count(Category::StraightFlush) == 40);
  CHECK(tally.count(Category::FourOfAKind) == 624);
  CHECK(tally.count(Category::FullHouse) == 3'744);
  CHECK(tally.count(Category::Flush) == 5'108);
  CHECK(tally.count(Category::Straight) == 10'200);
  CHECK(tally.count(Category::ThreeOfAKind) == 54'912);
  CHECK(tally.count(Category::TwoPair) == 123'552);
  CHECK(tally.count(Category::OnePair) == 1'098'240);
  CHECK(tally.count(Category::HighCard) == 1'302'540);
  CHECK(tally.distinct_values() == 7'462);

  // The distinct values seen in each category fill that category's block of HandRanks exactly.
  for (HandRank rank = nuts::kWorstHandRank; rank <= nuts::kBestHandRank; ++rank) {
    const auto category_index = static_cast<std::size_t>(nuts::category(rank));
    if (tally.seen[rank]) ++distinct_per_category[category_index];
  }
  CHECK(distinct_per_category == nuts::kCategorySize);
}

TEST_CASE("random 7-card hands: fast == reference", "[evaluator]") {
  std::mt19937_64 rng(20260927);
  constexpr int kSamples = 1'000'000;
  int mismatches = 0;
  for (int i = 0; i < kSamples; ++i) {
    const std::array<Card, 7> cards = random_hand<7>(rng);
    const HandRank fast = nuts::evaluate7(cards);
    const HandRank slow = nuts::reference::evaluate7(cards);
    if (fast != slow || nuts::evaluate(nuts::mask_of(cards)) != fast) {
      ++mismatches;
      if (mismatches <= 5) {  // report the first few only
        FAIL_CHECK(nuts::to_string(cards) << ": fast " << fast << ", reference " << slow);
      }
    }
  }
  CHECK(mismatches == 0);
}

TEST_CASE("random 6-card hands: fast == reference", "[evaluator]") {
  std::mt19937_64 rng(6);
  constexpr int kSamples = 200'000;
  int mismatches = 0;
  for (int i = 0; i < kSamples; ++i) {
    const std::array<Card, 6> cards = random_hand<6>(rng);
    const HandRank fast = nuts::evaluate6(cards);
    const HandRank slow = nuts::reference::evaluate6(cards);
    if (fast != slow || nuts::evaluate(nuts::mask_of(cards)) != fast) {
      ++mismatches;
      if (mismatches <= 5) {  // report the first few only
        FAIL_CHECK(nuts::to_string(cards) << ": fast " << fast << ", reference " << slow);
      }
    }
  }
  CHECK(mismatches == 0);
}

// Hidden ([.]) so it only runs when asked for: nuts_tests "[exhaustive]". Meant for Release
// builds (about a second there; much slower unoptimized or with sanitizers).
TEST_CASE("all 133,784,560 seven-card hands", "[.][exhaustive]") {
  Tally tally;
  for_each_hand<7>([&](const std::array<Card, 7>& cards) { tally.add(nuts::evaluate7(cards)); });

  const std::uint64_t other_straight_flushes =
      tally.count(Category::StraightFlush) - tally.royal_flushes;
  const std::pair<const char*, std::uint64_t> rows[] = {
      {"Royal flush", tally.royal_flushes},
      {"Straight flush (other)", other_straight_flushes},
      {"Four of a kind", tally.count(Category::FourOfAKind)},
      {"Full house", tally.count(Category::FullHouse)},
      {"Flush", tally.count(Category::Flush)},
      {"Straight", tally.count(Category::Straight)},
      {"Three of a kind", tally.count(Category::ThreeOfAKind)},
      {"Two pair", tally.count(Category::TwoPair)},
      {"One pair", tally.count(Category::OnePair)},
      {"High card", tally.count(Category::HighCard)},
  };
  std::printf("All 7-card hands: %llu hands, %d distinct values\n",
              static_cast<unsigned long long>(tally.hands), tally.distinct_values());
  for (const auto& [name, count] : rows) {
    std::printf("  %-24s %12llu\n", name, static_cast<unsigned long long>(count));
  }

  CHECK(tally.hands == 133'784'560);
  CHECK(tally.royal_flushes == 4'324);
  CHECK(other_straight_flushes == 37'260);
  CHECK(tally.count(Category::FourOfAKind) == 224'848);
  CHECK(tally.count(Category::FullHouse) == 3'473'184);
  CHECK(tally.count(Category::Flush) == 4'047'644);
  CHECK(tally.count(Category::Straight) == 6'180'020);
  CHECK(tally.count(Category::ThreeOfAKind) == 6'461'620);
  CHECK(tally.count(Category::TwoPair) == 31'433'400);
  CHECK(tally.count(Category::OnePair) == 58'627'800);
  CHECK(tally.count(Category::HighCard) == 23'294'460);
  CHECK(tally.distinct_values() == 4'824);
}
