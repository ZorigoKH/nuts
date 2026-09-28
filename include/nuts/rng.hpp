// Random numbers: SplitMix64 and xoshiro256**.
//
// The Monte Carlo equity code needs a fast generator whose output is the same on every platform.
// The standard library's distributions do not promise that (std::uniform_int_distribution may
// give different numbers with different standard libraries), so the generators and the two
// conversions we need (an integer below a bound, a double in [0, 1)) are implemented here.
//
//   * xoshiro256** (Blackman and Vigna, 2018) is the generator used for sampling. Its state is
//     four 64-bit words. Each step mixes them with XORs, a shift and a rotation, and the output is
//     a scrambled copy of one word ("**": multiply, rotate, multiply). Period 2^256 - 1.
//   * SplitMix64 (Steele, Lea and Flood, 2014) turns one 64-bit seed into the four state words.
//     Nearby seeds (0, 1, 2, ...) give unrelated states.
//
// Both are transcriptions of the public-domain reference code at https://prng.di.unimi.it/.
#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace nuts {

class SplitMix64 {
 public:
  explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  // Adds a constant to the state and returns a well-mixed function of the new state. The mixing
  // function is a bijection, so outputs for consecutive states are all different.
  constexpr std::uint64_t next() noexcept {
    state_ += 0x9e37'79b9'7f4a'7c15;  // 2^64 divided by the golden ratio, rounded to odd
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xbf58'476d'1ce4'e5b9;
    z = (z ^ (z >> 27)) * 0x94d0'49bb'1331'11eb;
    return z ^ (z >> 31);
  }

 private:
  std::uint64_t state_;
};

class Xoshiro256StarStar {
 public:
  using State = std::array<std::uint64_t, 4>;

  // Seeds the state with four outputs of SplitMix64(seed). They cannot all be zero (the one
  // state xoshiro can never leave): the four outputs are different numbers.
  explicit constexpr Xoshiro256StarStar(std::uint64_t seed) noexcept {
    SplitMix64 seeder(seed);
    for (std::uint64_t& word : state_) word = seeder.next();
  }

  // Starts from an explicit state (used by the tests). It must not be all zero.
  static constexpr Xoshiro256StarStar from_state(const State& state) noexcept {
    Xoshiro256StarStar rng(0);
    rng.state_ = state;
    return rng;
  }

  constexpr const State& state() const noexcept { return state_; }

  // The next 64 random bits.
  constexpr std::uint64_t next() noexcept {
    State& s = state_;
    const std::uint64_t result = std::rotl(s[1] * 5, 7) * 9;
    const std::uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = std::rotl(s[3], 45);
    return result;
  }

  // A uniformly random integer in [0, bound), for 1 <= bound < 2^32, with no bias (Lemire's
  // multiply-and-reject method, https://arxiv.org/abs/1805.10941).
  //
  // Take 32 random bits x and form the 64-bit product x * bound. Its top 32 bits,
  // floor(x * bound / 2^32), are in [0, bound). Unless bound divides 2^32, some results would
  // come up once more often than others. Let t = 2^32 mod bound. For every result r, exactly
  // floor(2^32 / bound) values of x give top part r and a low part (the product's low 32 bits)
  // of at least t, so keeping only those products makes every r equally likely. Since t < bound,
  // the rejection test is only needed when the low part is below bound, which is rare.
  constexpr std::uint32_t below(std::uint32_t bound) noexcept {
    std::uint64_t product = (next() >> 32) * bound;
    auto low = static_cast<std::uint32_t>(product);
    if (low < bound) {
      // (2^32 - bound) mod bound == 2^32 mod bound, computed without 64-bit arithmetic.
      const std::uint32_t threshold = (std::uint32_t{0} - bound) % bound;
      while (low < threshold) {
        product = (next() >> 32) * bound;
        low = static_cast<std::uint32_t>(product);
      }
    }
    return static_cast<std::uint32_t>(product >> 32);
  }

  // A uniformly random double in [0, 1): the top 53 bits (a double's precision) times 2^-53.
  constexpr double uniform01() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

  // Moves the generator 2^128 steps ahead, as if next() had been called 2^128 times. Calling it
  // once per thread (thread t uses the generator jumped t times) splits one sequence into
  // streams that cannot overlap in any realistic run.
  //
  // How: every step is linear over bits (only XORs, shifts and rotations), so the state after
  // 2^128 steps is a fixed linear function of the current state. It can be written as the XOR of
  // the states at steps 0..255 selected by the bits of the constants below, which the reference
  // code precomputes (the coefficients of the polynomial x^(2^128) modulo the generator's
  // characteristic polynomial).
  constexpr void jump() noexcept {
    constexpr std::array<std::uint64_t, 4> kJump = {0x180e'c6d3'3cfd'0aba, 0xd5a6'1266'f0c9'392c,
                                                    0xa958'2618'e03f'c9aa, 0x39ab'dc45'29b1'661c};
    State sum{};
    for (const std::uint64_t word : kJump) {
      for (int bit = 0; bit < 64; ++bit) {
        if ((word >> bit) & 1) {
          for (std::size_t i = 0; i < sum.size(); ++i) sum[i] ^= state_[i];
        }
        next();
      }
    }
    state_ = sum;
  }

 private:
  State state_{};
};

}  // namespace nuts
