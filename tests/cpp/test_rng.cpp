#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <vector>

#include "nuts/rng.hpp"

namespace {

using nuts::Xoshiro256StarStar;

}  // namespace

// The expected values below come from tools/verify/rng_reference.py, a separate Python
// transcription of the reference C code (https://prng.di.unimi.it/), which recomputes and checks
// them all. The first SplitMix64 output for seed 0 (0xe220a8397b1dcdaf) and the first
// xoshiro256** output for the state {1, 2, 3, 4} (11520) are also widely published.

TEST_CASE("SplitMix64 matches the reference code", "[rng]") {
  nuts::SplitMix64 zero(0);
  CHECK(zero.next() == 0xe220a8397b1dcdaf);
  CHECK(zero.next() == 0x6e789e6aa1b965f4);
  CHECK(zero.next() == 0x06c45d188009454f);

  nuts::SplitMix64 other(1234567);
  CHECK(other.next() == 0x599ed017fb08fc85);
  CHECK(other.next() == 0x2c73f08458540fa5);
  CHECK(other.next() == 0x883ebce5a3f27c77);
}

TEST_CASE("xoshiro256** matches the reference code", "[rng]") {
  auto rng = Xoshiro256StarStar::from_state({1, 2, 3, 4});
  const std::array<std::uint64_t, 6> expected = {
      0x2d00, 0x0, 0x5a007080, 0x10e0000000009d80, 0x10e0b61ce1009d80, 0x0870021ce143ad00};
  for (const std::uint64_t value : expected) CHECK(rng.next() == value);

  // Seeding: the state is four SplitMix64 outputs.
  Xoshiro256StarStar seeded(42);
  CHECK(seeded.state() == Xoshiro256StarStar::State{0xbdd732262feb6e95, 0x28efe333b266f103,
                                                    0x47526757130f9f52, 0x581ce1ff0e4ae394});
  CHECK(seeded.next() == 0x15780b2e0c2ec716);
  CHECK(seeded.next() == 0x6104d9866d113a7e);
  CHECK(seeded.next() == 0xae17533239e499a1);
}

// tools/verify/rng_reference.py also checks, independently of the jump constants, that the jump
// equals 2^128 single steps: it builds the 256 x 256 bit matrix of one step and squares it 128
// times.
TEST_CASE("jump() matches the reference code", "[rng]") {
  auto rng = Xoshiro256StarStar::from_state({1, 2, 3, 4});
  rng.jump();
  CHECK(rng.state() == Xoshiro256StarStar::State{0x8c7a153956b5f3d1, 0x701f1a713401d85e,
                                                 0x6527f66a65469085, 0x8386b786c4408050});
  CHECK(rng.next() == 0xbbd2f312298443d8);
  CHECK(rng.next() == 0x62e57db2d5706577);
}

TEST_CASE("same seed, same numbers; different seeds, different numbers", "[rng]") {
  Xoshiro256StarStar a(7);
  Xoshiro256StarStar b(7);
  Xoshiro256StarStar c(8);
  int same_as_c = 0;
  for (int i = 0; i < 1000; ++i) {
    const std::uint64_t x = a.next();
    CHECK(x == b.next());
    if (x == c.next()) ++same_as_c;
  }
  CHECK(same_as_c == 0);
}

TEST_CASE("below() is in range and uniform", "[rng]") {
  Xoshiro256StarStar rng(2026);
  for (const std::uint32_t bound : {1u, 2u, 3u, 7u, 52u, 1326u, 4'000'000'000u}) {
    for (int i = 0; i < 10'000; ++i) REQUIRE(rng.below(bound) < bound);
  }

  // 600,000 draws in [0, 6): each count should be 100,000 give or take a few hundred (the
  // standard deviation is sqrt(600000 * 1/6 * 5/6) ~ 289). 6 standard deviations = 1,732.
  constexpr int kDraws = 600'000;
  std::array<int, 6> counts{};
  for (int i = 0; i < kDraws; ++i) ++counts[rng.below(6)];
  for (const int count : counts) {
    CHECK(count > kDraws / 6 - 1'732);
    CHECK(count < kDraws / 6 + 1'732);
  }
}

TEST_CASE("uniform01() is in [0, 1) with mean 1/2", "[rng]") {
  Xoshiro256StarStar rng(99);
  constexpr int kDraws = 1'000'000;
  double sum = 0;
  for (int i = 0; i < kDraws; ++i) {
    const double x = rng.uniform01();
    REQUIRE(x >= 0.0);
    REQUIRE(x < 1.0);
    sum += x;
  }
  // The mean of n uniform draws has standard deviation sqrt(1/12 / n) ~ 0.00029.
  CHECK(sum / kDraws > 0.5 - 0.002);
  CHECK(sum / kDraws < 0.5 + 0.002);
}

TEST_CASE("jumped streams differ from the original", "[rng]") {
  Xoshiro256StarStar base(5);
  Xoshiro256StarStar jumped = base;
  jumped.jump();
  std::vector<std::uint64_t> first;
  for (int i = 0; i < 100; ++i) first.push_back(base.next());
  int equal = 0;
  for (int i = 0; i < 100; ++i) {
    if (jumped.next() == first[static_cast<std::size_t>(i)]) ++equal;
  }
  CHECK(equal == 0);
}
