"""Independent check of the random number generator constants in tests/cpp/test_rng.cpp.

A Python transcription of the reference C code for SplitMix64 and xoshiro256**
(https://prng.di.unimi.it/). It recomputes every expected value in the C++ tests, and checks,
without using the jump constants, that jump() advances the generator by 2^128 steps.

Usage (from the repository root; no dependencies, takes a few seconds):

    python tools/verify/rng_reference.py

Prints each check and exits with status 1 if any fails.
"""

from __future__ import annotations

import sys

MASK = (1 << 64) - 1  # Python integers are unbounded; keep the low 64 bits after each step


def rotl(x: int, k: int) -> int:
    return ((x << k) | (x >> (64 - k))) & MASK


def splitmix64(seed: int, count: int) -> list[int]:
    """The first `count` outputs of SplitMix64 started from `seed`."""
    state = seed & MASK
    out = []
    for _ in range(count):
        state = (state + 0x9E3779B97F4A7C15) & MASK
        z = state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
        out.append(z ^ (z >> 31))
    return out


def xoshiro_step(s: list[int]) -> None:
    """Advances the xoshiro256 state (a list of four 64-bit words) by one step, in place."""
    t = (s[1] << 17) & MASK
    s[2] ^= s[0]
    s[3] ^= s[1]
    s[1] ^= s[2]
    s[0] ^= s[3]
    s[2] ^= t
    s[3] = rotl(s[3], 45)


def xoshiro_next(s: list[int]) -> int:
    """The next xoshiro256** output (the "**" scrambler), advancing the state."""
    result = (rotl((s[1] * 5) & MASK, 7) * 9) & MASK
    xoshiro_step(s)
    return result


def xoshiro_jump(s: list[int]) -> list[int]:
    """The state 2^128 steps after `s`, with the jump polynomial of the reference code."""
    jump = [0x180EC6D33CFD0ABA, 0xD5A61266F0C9392C, 0xA9582618E03FC9AA, 0x39ABDC4529B1661C]
    s = list(s)
    total = [0, 0, 0, 0]
    for word in jump:
        for bit in range(64):
            if (word >> bit) & 1:
                total = [a ^ b for a, b in zip(total, s, strict=True)]
            xoshiro_step(s)
    return total


def jump_by_matrix(s: list[int]) -> list[int]:
    """The state 2^128 steps after `s`, computed without the jump constants.

    One step is linear over GF(2) (only XOR, shifts and rotations), so it is a 256 x 256 bit
    matrix M. Squaring M 128 times gives M^(2^128). The 256-bit state is one Python int, word i
    in bits 64i..64i+63; a matrix is a list of its 256 columns (column j = M applied to bit j).
    """

    def to_int(words: list[int]) -> int:
        return sum(w << (64 * i) for i, w in enumerate(words))

    def to_words(v: int) -> list[int]:
        return [(v >> (64 * i)) & MASK for i in range(4)]

    def step_int(v: int) -> int:
        words = to_words(v)
        xoshiro_step(words)
        return to_int(words)

    def apply(columns: list[int], v: int) -> int:
        result = 0
        j = 0
        while v:
            if v & 1:
                result ^= columns[j]
            v >>= 1
            j += 1
        return result

    columns = [step_int(1 << j) for j in range(256)]
    for _ in range(128):
        columns = [apply(columns, c) for c in columns]  # M := M * M
    return to_words(apply(columns, to_int(s)))


def main() -> int:
    failures = 0

    def check(name: str, actual: list[int], expected: list[int]) -> None:
        nonlocal failures
        ok = actual == expected
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'}  {name}: {[hex(v) for v in actual]}")

    # The expected values are the ones in tests/cpp/test_rng.cpp.
    check(
        "SplitMix64, seed 0",
        splitmix64(0, 3),
        [0xE220A8397B1DCDAF, 0x6E789E6AA1B965F4, 0x06C45D188009454F],
    )
    check(
        "SplitMix64, seed 1234567",
        splitmix64(1234567, 3),
        [0x599ED017FB08FC85, 0x2C73F08458540FA5, 0x883EBCE5A3F27C77],
    )
    state = [1, 2, 3, 4]
    check(
        "xoshiro256**, state {1, 2, 3, 4}",
        [xoshiro_next(state) for _ in range(6)],
        [0x2D00, 0x0, 0x5A007080, 0x10E0000000009D80, 0x10E0B61CE1009D80, 0x0870021CE143AD00],
    )
    seeded = splitmix64(42, 4)  # seeding: the state is four SplitMix64 outputs
    check(
        "xoshiro256**, seed 42: state",
        seeded,
        [0xBDD732262FEB6E95, 0x28EFE333B266F103, 0x47526757130F9F52, 0x581CE1FF0E4AE394],
    )
    check(
        "xoshiro256**, seed 42: outputs",
        [xoshiro_next(seeded) for _ in range(3)],
        [0x15780B2E0C2EC716, 0x6104D9866D113A7E, 0xAE17533239E499A1],
    )
    jumped = xoshiro_jump([1, 2, 3, 4])
    check(
        "jump() of {1, 2, 3, 4}",
        jumped,
        [0x8C7A153956B5F3D1, 0x701F1A713401D85E, 0x6527F66A65469085, 0x8386B786C4408050],
    )
    check("2^128 steps by matrix squaring", jump_by_matrix([1, 2, 3, 4]), jumped)
    check(
        "outputs after the jump",
        [xoshiro_next(jumped) for _ in range(2)],
        [0xBBD2F312298443D8, 0x62E57DB2D5706577],
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
