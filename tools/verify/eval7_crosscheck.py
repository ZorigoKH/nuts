"""Independent check of exact preflop equities with eval7, a different poker library.

Recomputes, with eval7's hand evaluator and plain Python loops over every board:

  * the preflop equities that tests/cpp/test_equity.cpp expects (AhAs vs KdKc, AsKs vs 2h2d,
    and Ac2d vs 7h7s vs KsQs), as exact fractions, and
  * three cells of the committed equity matrix data/equity169.bin (AA vs KK, AKs vs AKo and
    72o vs 32o), which tests/cpp/test_pushfold.cpp also recomputes with our own evaluator.

tests/python/test_eval7.py does the same brute force for boards with three or more cards on
every test run; the preflop cases here enumerate 1.7 million boards each, too slow for that.

Usage (from the repository root, with eval7 installed; takes about half a minute):

    python tools/verify/eval7_crosscheck.py
"""

from __future__ import annotations

import itertools
import struct
import sys
from fractions import Fraction
from pathlib import Path

import eval7

RANKS = "23456789TJQKA"
SUITS = "cdhs"
DECK = [rank + suit for rank in RANKS for suit in SUITS]
EQUITY_FILE = Path(__file__).resolve().parents[2] / "data" / "equity169.bin"


def exact_equity(hands: list[str]) -> tuple[list[Fraction], list[Fraction], list[Fraction]]:
    """(win, tie, equity) of each player before the flop, as exact fractions of all boards."""
    hole_cards = [[eval7.Card(hand[:2]), eval7.Card(hand[2:])] for hand in hands]
    used = {hand[:2] for hand in hands} | {hand[2:] for hand in hands}
    unseen = [eval7.Card(card) for card in DECK if card not in used]
    n = len(hands)
    wins = [0] * n
    ties = [0] * n
    shares = [0] * n  # in units of 1/12 pot (12 is divisible by 2 and 3, the possible splits)
    boards = 0
    for board in itertools.combinations(unseen, 5):
        cards = list(board)
        values = [eval7.evaluate(cards + hole) for hole in hole_cards]
        best = max(values)
        winners = [p for p, value in enumerate(values) if value == best]
        for p in winners:
            shares[p] += 12 // len(winners)
            if len(winners) == 1:
                wins[p] += 1
            else:
                ties[p] += 1
        boards += 1
    return (
        [Fraction(w, boards) for w in wins],
        [Fraction(t, boards) for t in ties],
        [Fraction(s, 12 * boards) for s in shares],
    )


def class_index(name: str) -> int:
    """Index of a hand class ("AA", "AKs", "72o") in the 13 x 13 chart, as in pushfold.hpp."""
    rows = RANKS[::-1]  # row 0 is the ace
    high, low = rows.index(name[0]), rows.index(name[1])
    if len(name) == 2:
        return 13 * high + high
    return 13 * high + low if name[2] == "s" else 13 * low + high


def class_combos(name: str) -> list[tuple[str, str]]:
    combos = []
    for s1, s2 in itertools.product(SUITS, repeat=2):
        if len(name) == 2:
            if s1 >= s2:
                continue  # each pair of suits once
        elif (name[2] == "s") != (s1 == s2):
            continue
        combos.append((name[0] + s1, name[1] + s2))
    return combos


def half_pots(a: tuple[str, str], b: tuple[str, str]) -> int:
    """Half pots won by hand a against hand b over all boards: 2 per win, 1 per tie."""
    hand_a = [eval7.Card(c) for c in a]
    hand_b = [eval7.Card(c) for c in b]
    unseen = [eval7.Card(c) for c in DECK if c not in a and c not in b]
    total = 0
    for board in itertools.combinations(unseen, 5):
        cards = list(board)
        value_a = eval7.evaluate(cards + hand_a)
        value_b = eval7.evaluate(cards + hand_b)
        total += 2 if value_a > value_b else 1 if value_a == value_b else 0
    return total


def matrix_cell(first: str, second: str) -> tuple[int, int]:
    """(weight, half pots) of a cell, recomputed with eval7.

    As in tests/cpp/test_pushfold.cpp: every hand of the first class is a suit renaming of any
    one of them, so the cell is num_combos(first) times the total of one fixed hand against
    all hands of the second class that share no card with it.
    """
    a = class_combos(first)[0]
    weight = 0
    total = 0
    for b in class_combos(second):
        if set(a) & set(b):
            continue
        weight += 1
        total += half_pots(a, b)
    return len(class_combos(first)) * weight, len(class_combos(first)) * total


def committed_cell(first: str, second: str) -> tuple[int, int]:
    data = EQUITY_FILE.read_bytes()
    classes = struct.unpack_from("<I", data, 8)[0]
    cell = class_index(first) * classes + class_index(second)
    weight = struct.unpack_from("<I", data, 16 + 4 * cell)[0]
    half = struct.unpack_from("<I", data, 16 + 4 * (classes * classes + cell))[0]
    return weight, half


def main() -> int:
    failures = 0

    def check(name: str, actual: object, expected: object) -> None:
        nonlocal failures
        ok = actual == expected
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'}  {name}: {actual}" + ("" if ok else f" != {expected}"))

    # The values expected by tests/cpp/test_equity.cpp; the fractions are converted to the
    # nearest double, which must be exactly the number written in the test.
    win, tie, equity = exact_equity(["AhAs", "KdKc"])
    check("AhAs vs KdKc: win", [float(w) for w in win], [0.81064577318046327, 0.18553597959240883])
    check("AhAs vs KdKc: tie", float(tie[0]), 0.0038182472271278932)
    check(
        "AhAs vs KdKc: equity",
        [float(e) for e in equity],
        [0.81255489679402726, 0.18744510320597277],
    )
    _, tie, equity = exact_equity(["AsKs", "2h2d"])
    check(
        "AsKs vs 2h2d: equity",
        [float(e) for e in equity],
        [0.50084243218493907, 0.49915756781506088],
    )
    check("AsKs vs 2h2d: tie", float(tie[0]), 0.0062926910174828767)
    _, _, equity = exact_equity(["Ac2d", "7h7s", "KsQs"])
    check(
        "Ac2d vs 7h7s vs KsQs: equity",
        [float(e) for e in equity],
        [0.24679142525451928, 0.37308031443521839, 0.38012826031026231],
    )

    for first, second in [("AA", "KK"), ("AKs", "AKo"), ("72o", "32o")]:
        check(
            f"data/equity169.bin, {first} vs {second} (weight, half pots)",
            matrix_cell(first, second),
            committed_cell(first, second),
        )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
