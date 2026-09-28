"""Cross-checks against phevaluator (the Python binding of PokerHandEvaluator) and treys.

Both number the 7,462 hand values like we do, but the other way round: 1 is the royal flush and
7462 the worst hand. So their rank must be exactly 7463 minus ours, for every hand.
"""

from __future__ import annotations

import random

import pytest

import nuts

RANKS = "23456789TJQKA"
SUITS = "cdhs"
DECK = [rank + suit for rank in RANKS for suit in SUITS]


def random_hands(count: int, seed: int) -> list[list[str]]:
    """`count` random hands of 5, 6 and 7 cards (a third of each)."""
    rng = random.Random(seed)
    return [rng.sample(DECK, 5 + i % 3) for i in range(count)]


def test_phevaluator_ranks_match_exactly() -> None:
    phevaluator = pytest.importorskip("phevaluator")
    for hand in random_hands(100_000, seed=11):
        assert nuts.evaluate(hand) == 7463 - phevaluator.evaluate_cards(*hand), hand


def test_treys_ranks_match_exactly() -> None:
    treys = pytest.importorskip("treys")
    evaluator = treys.Evaluator()
    treys_card = {name: treys.Card.new(name) for name in DECK}
    for hand in random_hands(20_000, seed=12):  # treys is pure Python, so fewer hands
        cards = [treys_card[c] for c in hand]
        # treys takes the board and the hole cards separately; only their union matters.
        assert nuts.evaluate(hand) == 7463 - evaluator.evaluate(cards[2:], cards[:2]), hand
