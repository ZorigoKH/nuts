"""nuts: a poker hand evaluator, equity calculator and game-theory solvers.

The work is done by a C++ library (the compiled module ``nuts._core``); this module is the
documented Python interface to it.

Cards are written as a rank and a suit, such as ``"As"`` (ace of spades), ``"Td"`` or ``"10d"``
(ten of diamonds) and ``"2c"``: ranks 2-9, T (or 10), J, Q, K, A and suits c, d, h, s, in
either case. A list of cards is one string (``"As Kd 7c"``, spaces or commas optional) or a list
of card strings.

Example::

    >>> import nuts
    >>> nuts.evaluate("As Ks Qs Js Ts")
    7462
    >>> nuts.category(nuts.evaluate("As Ad Kc Kd 7h"))
    'Two Pair'
    >>> [round(p["equity"], 4) for p in nuts.equity(["AhAs", "KdKc"])]
    [0.8126, 0.1874]
"""

from __future__ import annotations

import re
from importlib.metadata import PackageNotFoundError, version
from typing import TypedDict

from nuts import _core

try:
    __version__ = version("nuts-poker")
except PackageNotFoundError:  # running from a source tree that was not installed
    __version__ = "unknown"

# Limits of the C++ library, checked here so that a bad value raises ValueError with a clear
# message (the C++ functions take fixed-size integers, and nanobind would raise a TypeError for a
# Python int that does not fit).
_MAX_UINT64 = 2**64 - 1
_MAX_INT32 = 2**31 - 1
MAX_THREADS: int = _core.MAX_THREADS  # the most threads equity() accepts (256)
MAX_STACK_BB: float = _core.MAX_STACK_BB  # the largest stack pushfold() accepts (100)

# One card: a rank (2-9, T or 10, J, Q, K, A) and a suit (c, d, h, s), in either case.
_ONE_CARD = re.compile(r"(?:[2-9TJQKA]|10)[CDHS]", re.IGNORECASE)

__all__ = [
    "MAX_STACK_BB",
    "MAX_THREADS",
    "KuhnResult",
    "PlayerEquity",
    "PushFoldResult",
    "category",
    "equity",
    "evaluate",
    "kuhn_cfr",
    "parse_range",
    "pushfold",
]


class PlayerEquity(TypedDict):
    """One player's result from :func:`equity`."""

    hand: str  # the hand or range, as given
    equity: float  # expected share of the pot: win + the average share in ties
    win: float  # probability of winning the whole pot
    tie: float  # probability of sharing the pot
    std_error: float  # standard error of `equity` (Monte Carlo); 0.0 for exact results


class KuhnResult(TypedDict):
    """The result of :func:`kuhn_cfr`."""

    iterations: int
    game_value: float  # player 1's expected result per hand, in antes (equilibrium: -1/18)
    exploitability: float  # what best responses gain on average, in antes (0 at equilibrium)
    strategy: dict[str, float]  # information set ("J", "Qp", "Kpb", ...) -> P(bet or call)


class PushFoldResult(TypedDict):
    """The result of :func:`pushfold`. Grids are 13 x 13 lists of rows, laid out like ``hands``."""

    stack_bb: float
    iterations: int  # CFR+ iterations
    exploitability: float  # in big blinds per hand
    sb_ev: float  # the small blind's expected result, in big blinds per hand
    hands: list[list[str]]  # "AA" top left; suited hands above the diagonal, offsuit below
    shove: list[list[float]]  # probability that the small blind goes all-in
    call: list[list[float]]  # probability that the big blind calls the all-in


def evaluate(cards: str | list[str]) -> int:
    """Rank a 5-, 6- or 7-card poker hand.

    Returns a HandRank from 1 (7-5-4-3-2 of different suits, the worst hand) to 7462 (a royal
    flush): higher is better, equal ranks tie. With 6 or 7 cards, the hand is worth its best
    five cards.

    Args:
        cards: the cards, e.g. ``"As Ks Qs Js Ts 2c 3d"`` or ``["As", "Ks", "Qs", "Js", "Ts"]``.

    Raises:
        ValueError: a card cannot be read, a card is repeated, there are not 5 to 7 cards, or
            an item of the list is not exactly one card.
        TypeError: ``cards`` is neither a string nor a list of strings.
    """
    if isinstance(cards, str):
        return _core.evaluate(cards)
    for card in cards:
        if not isinstance(card, str):
            raise TypeError(f"cards must be a string or a list of strings, got an item {card!r}")
        if not _ONE_CARD.fullmatch(card.strip()):
            raise ValueError(f"{card!r} is not one card (such as 'As' or 'Td')")
    return _core.evaluate(" ".join(cards))


def category(rank: int) -> str:
    """The category of a HandRank from :func:`evaluate`.

    One of ``"High Card"``, ``"One Pair"``, ``"Two Pair"``, ``"Three of a Kind"``,
    ``"Straight"``, ``"Flush"``, ``"Full House"``, ``"Four of a Kind"``, ``"Straight Flush"``.

    Raises:
        ValueError: the rank is not in 1..7462.
    """
    if not 1 <= rank <= 7462:
        raise ValueError(f"a hand rank is 1..7462, got {rank}")
    return _core.category(rank)


def parse_range(text: str) -> list[str]:
    """The two-card combos in a range, such as ``"TT+, AKs, KQo"``.

    The range syntax: ``AA`` (a pair, 6 combos), ``AKs`` / ``AKo`` / ``AK`` (suited, offsuit,
    either: 4, 12, 16 combos), ``TT+`` (that pair and every higher one), ``A2s+`` (A2s up to
    AKs), ``22-55`` and ``K7o-KTo`` (spans), ``AsKs`` (one specific combo), ``random`` (all
    1,326 combos), separated by commas or spaces. An item can end in ``:weight`` (0 to 1); the
    weights are used by :func:`equity` but not returned here.

    Returns:
        The combos, each written as two cards (higher card first), e.g. ``["AsKs", "AhKh", ...]``.

    Raises:
        ValueError: an item cannot be read (the message says which).
    """
    return _core.parse_range(text)


def equity(
    hands: list[str],
    board: str = "",
    *,
    exact: bool | None = None,
    trials: int = 200_000,
    seed: int = 0,
    threads: int = 0,
) -> list[PlayerEquity]:
    """Each player's equity (expected share of the pot) if all the cards are dealt.

    Args:
        hands: one entry per player (2 to 23): two specific cards such as ``"AsKd"`` or
            ``"As Kd"``, or a range such as ``"TT+, AKs"`` (see :func:`parse_range`).
        board: 0 to 5 known board cards, e.g. ``"Qs Jh 2c"``.
        exact: ``True``: go through every way to complete the board and count (every player
            must have two specific cards). ``False``: Monte Carlo, random deals. ``None`` (the
            default): exact when every player has two specific cards, Monte Carlo otherwise.
        trials: Monte Carlo only: the number of random deals (1 to 2**64 - 1). The standard
            error shrinks like 1 / sqrt(trials).
        seed: Monte Carlo only: the random seed (0 to 2**64 - 1).
        threads: Monte Carlo only: threads to use, 0 to :data:`MAX_THREADS` (256); 0 means
            one per CPU core. The same (seed, threads) gives exactly the same result, so pass
            ``threads`` explicitly to get the same numbers on machines with different core
            counts.

    Monte Carlo deals a player whose range is every hand (``"random"``) from the cards left
    over after the others, so any number of such players works. Players with other ranges are
    dealt by trial and error (a deal in which two of them share a card is thrown away), which
    gets slow when many wide ranges overlap and fails after 1,000,000 such deals in a row.

    Returns:
        One :class:`PlayerEquity` dict per player, in order: ``hand``, ``equity``, ``win``,
        ``tie`` and ``std_error``.

    Raises:
        ValueError: bad cards or ranges, a card used twice, the wrong number of players, an
            argument out of range, or ranges that could not be dealt together (e.g. ``"AA"``
            for three players).
    """
    if isinstance(hands, str):
        raise TypeError('hands must be a list with one string per player, e.g. ["AsKs", "QQ"]')
    hands = list(hands)
    if exact is None:
        exact = all(_core.parse_hole_cards(hand) is not None for hand in hands)
    if exact:
        return _core.equity_exact(hands, board)
    if not 1 <= trials <= _MAX_UINT64:
        raise ValueError(f"trials must be from 1 to 2**64 - 1, got {trials}")
    if not 0 <= seed <= _MAX_UINT64:
        raise ValueError(f"seed must be from 0 to 2**64 - 1, got {seed}")
    if not 0 <= threads <= MAX_THREADS:
        raise ValueError(f"threads must be from 0 to {MAX_THREADS}, got {threads}")
    return _core.equity_monte_carlo(hands, board, trials, seed, threads)


def kuhn_cfr(iterations: int = 100_000) -> KuhnResult:
    """Solve Kuhn poker with vanilla counterfactual regret minimization (CFR).

    Kuhn poker: a 3-card deck (J, Q, K), an ante of 1, one card each, one bet of 1 allowed.
    Its equilibrium value for player 1 is -1/18 per hand, which makes it a test of the solver.

    Returns:
        A :class:`KuhnResult`: the CFR average strategy (probability of betting or calling at
        each of the 12 information sets, named card + history, e.g. ``"Kpb"``: holding the
        king after check, bet), its game value and its exploitability.

    Raises:
        ValueError: iterations is not from 1 to 2**31 - 1.
    """
    if not 1 <= iterations <= _MAX_INT32:
        raise ValueError(f"iterations must be from 1 to 2**31 - 1, got {iterations}")
    return _core.kuhn_cfr(iterations)


def pushfold(stack_bb: float) -> PushFoldResult:
    """The heads-up push/fold Nash equilibrium for an effective stack, solved with CFR+.

    The small blind (0.5 bb) either folds or goes all-in; the big blind (1 bb) calls or folds.
    No ante; results are chip expectations. The solver uses the exact preflop equities of the
    169 hand classes and runs until the strategies are within 1e-6 bb per hand of an
    equilibrium (a fraction of a second; longer for deeper stacks).

    Args:
        stack_bb: the effective stack in big blinds, before the blinds are posted: 1 to
            :data:`MAX_STACK_BB` (100). Push/fold is normally played up to about 20 bb.

    Returns:
        A :class:`PushFoldResult` with 13 x 13 grids ``shove`` and ``call`` laid out like
        ``hands`` (``hands[0][0] == "AA"``, ``hands[0][1] == "AKs"``, ``hands[1][0] == "AKo"``).

    Raises:
        ValueError: the stack is not a number from 1 to 100.
    """
    return _core.pushfold(stack_bb)
