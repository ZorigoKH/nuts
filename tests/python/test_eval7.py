"""Cross-checks against eval7, an independent poker library (https://github.com/julianandrews/pyeval7).

eval7's hand values are different numbers from ours (and not 1..7462), so the evaluator check
compares orderings: for the same pair of hands, both must agree on which one is better, or that
they tie. Equity is checked against a brute force that uses eval7 to evaluate every board.
"""

from __future__ import annotations

import itertools
import random
from fractions import Fraction

import numpy as np
import pytest

import nuts

eval7 = pytest.importorskip("eval7")

RANKS = "23456789TJQKA"
SUITS = "cdhs"
DECK = [rank + suit for rank in RANKS for suit in SUITS]  # card id = 4 * rank + suit, as in nuts

# eval7's category names, translated to ours.
EVAL7_CATEGORY = {
    "High Card": "High Card",
    "Pair": "One Pair",
    "Two Pair": "Two Pair",
    "Trips": "Three of a Kind",
    "Straight": "Straight",
    "Flush": "Flush",
    "Full House": "Full House",
    "Quads": "Four of a Kind",
    "Straight Flush": "Straight Flush",
}


def random_showdowns(count: int, seed: int) -> list[tuple[list[str], list[str]]]:
    """`count` showdowns: two 7-card hands sharing a 5-card board (9 different cards each)."""
    rng = random.Random(seed)
    showdowns = []
    for _ in range(count):
        cards = rng.sample(DECK, 9)
        board, first, second = cards[:5], cards[5:7], cards[7:9]
        showdowns.append((board + first, board + second))
    return showdowns


def test_ordering_agrees_on_100k_random_showdowns() -> None:
    eval7_card = {name: eval7.Card(name) for name in DECK}
    ours_first, ours_second, theirs_first, theirs_second = [], [], [], []
    categories_agree = 0
    for first, second in random_showdowns(100_000, seed=2026):
        for hand, ours, theirs in (
            (first, ours_first, theirs_first),
            (second, ours_second, theirs_second),
        ):
            rank = nuts.evaluate(hand)
            value = eval7.evaluate([eval7_card[c] for c in hand])
            ours.append(rank)
            theirs.append(value)
            categories_agree += nuts.category(rank) == EVAL7_CATEGORY[eval7.handtype(value)]

    our_order = np.sign(np.array(ours_first) - np.array(ours_second))
    their_order = np.sign(np.array(theirs_first, dtype=np.int64) - np.array(theirs_second))
    assert (our_order == their_order).all()
    assert (our_order == 0).sum() > 1000  # plenty of ties (both play the board, same pair, ...)
    assert categories_agree == 200_000


def test_categories_agree_on_rare_hands() -> None:
    # Random hands rarely hold quads or straight flushes, so check a few directly.
    for hand in [
        "As Ks Qs Js Ts 2c 3d",
        "5d 4d 3d 2d Ad Kc Kh",
        "9h 9d 9c 9s 2c 3d 4h",
        "Kh Kd Kc 2s 2c 7d 8h",
        "Kh Kd Kc 2s 2c 2d 8h",  # two sets of trips make a full house
        "Ah 2c 3d 4h 5s 9c Kd",  # the wheel
    ]:
        value = eval7.evaluate([eval7.Card(c) for c in hand.split()])
        assert nuts.category(nuts.evaluate(hand)) == EVAL7_CATEGORY[eval7.handtype(value)]


def eval7_exact_equity(hands: list[str], board: str) -> list[tuple[float, float, float]]:
    """(win, tie, equity) per player by brute force: eval7 evaluates every completion of the
    board, and the shares are added up as exact fractions."""
    hole_cards = [[eval7.Card(hand[:2]), eval7.Card(hand[2:])] for hand in hands]
    known = [eval7.Card(card) for card in board.split()]
    used = set(board.split()) | {hand[:2] for hand in hands} | {hand[2:] for hand in hands}
    unseen = [eval7.Card(card) for card in DECK if card not in used]

    wins = [0] * len(hands)
    ties = [0] * len(hands)
    shares = [Fraction(0)] * len(hands)
    boards = 0
    for rest in itertools.combinations(unseen, 5 - len(known)):
        full_board = known + list(rest)
        values = [eval7.evaluate(full_board + cards) for cards in hole_cards]
        best = max(values)
        winners = [p for p, value in enumerate(values) if value == best]
        for p in winners:
            shares[p] += Fraction(1, len(winners))
            if len(winners) == 1:
                wins[p] += 1
            else:
                ties[p] += 1
        boards += 1
    return [
        (wins[p] / boards, ties[p] / boards, float(shares[p] / boards)) for p in range(len(hands))
    ]


@pytest.mark.parametrize(
    ("hands", "board"),
    [
        (["AhAs", "KdKc"], "Kh 7c 2d"),  # set against an overpair
        (["AsKs", "2h2d"], "Ts 9s 3d"),  # flush draw and overcards against a small pair
        (["8c8d", "AhKh"], "Kc 7h 2h"),  # a pair against top pair and a flush draw
        (["AsKs", "QhQd", "7c6c"], "Qs Jh 2c"),  # three players
        (["AcKd", "AhKc", "7s7d", "Js9s"], "Ts 8s 2d"),  # four players, lots of ties
        (["AcKd", "JhTh"], "9s 8c 2d 3h"),  # the turn
        (["2c3d", "4h5c"], "As Ks Qs Js"),  # the turn, a likely split
        (["AhAs", "KdKc"], "Kh 7c 2d 9s 3h"),  # the river
    ],
)
def test_exact_equity_equals_eval7_brute_force(hands: list[str], board: str) -> None:
    ours = nuts.equity(hands, board, exact=True)
    theirs = eval7_exact_equity(hands, board)
    for player, (win, tie, share) in zip(ours, theirs, strict=True):
        assert player["win"] == pytest.approx(win, abs=1e-12)
        assert player["tie"] == pytest.approx(tie, abs=1e-12)
        assert player["equity"] == pytest.approx(share, abs=1e-12)


def test_monte_carlo_range_equity_agrees_with_eval7() -> None:
    # AsKs against QQ+ before the flop. Our exact answer: the average of the exact equities
    # against each QQ+ combo that does not use the As or the Ks (all equally likely).
    villain_combos = [c for c in nuts.parse_range("QQ+") if "As" not in c and "Ks" not in c]
    exact = np.mean([nuts.equity(["AsKs", combo])[0]["equity"] for combo in villain_combos])

    ours = nuts.equity(["AsKs", "QQ+"], trials=200_000, seed=3, threads=2)[0]
    assert abs(ours["equity"] - exact) < 4 * ours["std_error"]

    trials = 200_000
    theirs = eval7.py_hand_vs_range_monte_carlo(
        [eval7.Card("As"), eval7.Card("Ks")], eval7.HandRange("QQ+"), [], trials
    )
    # eval7 does not report an error estimate; bound it by that of a win/lose coin flip.
    assert abs(theirs - exact) < 5 * np.sqrt(exact * (1 - exact) / trials)
