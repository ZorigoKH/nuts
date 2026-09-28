#include <catch2/catch_test_macros.hpp>

#include "nuts/card.hpp"

namespace {

using nuts::Card;

}  // namespace

TEST_CASE("card encoding is 4 * rank + suit", "[card]") {
  CHECK(nuts::make_card(nuts::kTwo, nuts::kClubs) == 0);
  CHECK(nuts::make_card(nuts::kTwo, nuts::kSpades) == 3);
  CHECK(nuts::make_card(nuts::kThree, nuts::kClubs) == 4);
  CHECK(nuts::make_card(nuts::kAce, nuts::kSpades) == 51);

  for (int id = 0; id < nuts::kNumCards; ++id) {
    const auto card = static_cast<Card>(id);
    CHECK(nuts::make_card(nuts::rank_of(card), nuts::suit_of(card)) == card);
    CHECK(nuts::is_valid_card(card));
  }
  CHECK_FALSE(nuts::is_valid_card(52));
  CHECK_FALSE(nuts::is_valid_card(255));
}

TEST_CASE("every card formats and parses back to itself", "[card]") {
  for (int id = 0; id < nuts::kNumCards; ++id) {
    const auto card = static_cast<Card>(id);
    const std::string text = nuts::to_string(card);
    REQUIRE(text.size() == 2);
    CHECK(nuts::parse_card(text) == card);
  }
  CHECK(nuts::to_string(nuts::make_card(nuts::kAce, nuts::kSpades)) == "As");
  CHECK(nuts::to_string(nuts::make_card(nuts::kTen, nuts::kDiamonds)) == "Td");
  CHECK(nuts::to_string(nuts::make_card(nuts::kTwo, nuts::kClubs)) == "2c");
  CHECK(nuts::to_string(static_cast<Card>(52)) == "??");
}

TEST_CASE("parse_card accepts either case and \"10\" for ten", "[card]") {
  const Card ace_spades = nuts::make_card(nuts::kAce, nuts::kSpades);
  CHECK(nuts::parse_card("As") == ace_spades);
  CHECK(nuts::parse_card("as") == ace_spades);
  CHECK(nuts::parse_card("AS") == ace_spades);
  CHECK(nuts::parse_card("aS") == ace_spades);

  const Card ten_hearts = nuts::make_card(nuts::kTen, nuts::kHearts);
  CHECK(nuts::parse_card("Th") == ten_hearts);
  CHECK(nuts::parse_card("th") == ten_hearts);
  CHECK(nuts::parse_card("10h") == ten_hearts);
  CHECK(nuts::parse_card("10H") == ten_hearts);
}

TEST_CASE("parse_card rejects malformed input", "[card]") {
  for (const char* bad : {"", "A", "s", "Ax", "1s", "1", "11s", "10", "Ass", "A s", "Zs", "0s"}) {
    INFO("input: '" << bad << "'");
    CHECK_FALSE(nuts::parse_card(bad).has_value());
  }
}

TEST_CASE("parse_cards splits lists", "[card]") {
  const std::vector<Card> expected = {
      nuts::make_card(nuts::kAce, nuts::kSpades),
      nuts::make_card(nuts::kKing, nuts::kDiamonds),
      nuts::make_card(nuts::kSeven, nuts::kClubs),
  };
  CHECK(nuts::parse_cards("As Kd 7c") == expected);
  CHECK(nuts::parse_cards("As,Kd,7c") == expected);
  CHECK(nuts::parse_cards("  As, Kd\t7c ") == expected);
  CHECK(nuts::parse_cards("AsKd7c") == expected);
  CHECK(nuts::parse_cards("asKD7C") == expected);

  const std::vector<Card> with_ten = {nuts::make_card(nuts::kTen, nuts::kSpades),
                                      nuts::make_card(nuts::kNine, nuts::kSpades)};
  CHECK(nuts::parse_cards("10s9s") == with_ten);
  CHECK(nuts::parse_cards("10s 9s") == with_ten);

  CHECK(nuts::parse_cards("") == std::vector<Card>{});
  CHECK(nuts::parse_cards("   ") == std::vector<Card>{});
}

TEST_CASE("parse_cards rejects bad cards and duplicates", "[card]") {
  CHECK_FALSE(nuts::parse_cards("As Kx").has_value());
  CHECK_FALSE(nuts::parse_cards("As K").has_value());
  CHECK_FALSE(nuts::parse_cards("AsK").has_value());
  CHECK_FALSE(nuts::parse_cards("As As").has_value());
  CHECK_FALSE(nuts::parse_cards("As as").has_value());
  CHECK_FALSE(nuts::parse_cards("As; Kd").has_value());
}

TEST_CASE("to_string of a card list", "[card]") {
  const std::vector<Card> cards = *nuts::parse_cards("as kd 10c");
  CHECK(nuts::to_string(cards) == "As Kd Tc");
  CHECK(nuts::to_string(std::vector<Card>{}).empty());
}

TEST_CASE("card masks", "[card]") {
  CHECK(nuts::card_bit(0) == 1);
  CHECK(nuts::card_bit(51) == std::uint64_t{1} << 51);
  CHECK(nuts::card_bit(52) == 0);  // invalid ids contribute no bit
  CHECK(nuts::card_bit(200) == 0);

  const std::vector<Card> cards = *nuts::parse_cards("2c 2d As");
  const std::uint64_t mask = nuts::mask_of(cards);
  CHECK(mask == ((std::uint64_t{1} << 0) | (std::uint64_t{1} << 1) | (std::uint64_t{1} << 51)));
  CHECK(nuts::cards_of(mask) == cards);  // already in increasing id order

  CHECK(nuts::cards_of(nuts::kFullDeckMask).size() == 52);
  CHECK(nuts::cards_of(0).empty());
  CHECK(nuts::cards_of(~std::uint64_t{0}).size() == 52);  // bits 52..63 are ignored
}
