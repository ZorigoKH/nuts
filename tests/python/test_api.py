"""The Python API: results, argument handling and errors."""

from __future__ import annotations

import json
import locale
from collections.abc import Iterator
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import pytest

import nuts

REPO_ROOT = Path(__file__).resolve().parents[2]
CATEGORIES = [
    "High Card",
    "One Pair",
    "Two Pair",
    "Three of a Kind",
    "Straight",
    "Flush",
    "Full House",
    "Four of a Kind",
    "Straight Flush",
]


# ---- evaluate / category ----------------------------------------------------------------------


def test_evaluate_extremes() -> None:
    assert nuts.evaluate("As Ks Qs Js Ts") == 7462  # royal flush, the best hand
    assert nuts.evaluate("7c 5d 4h 3s 2c") == 1  # the worst hand
    assert nuts.evaluate("5s 4s 3s 2s As") == 7453  # the wheel straight flush, the lowest one


def test_evaluate_accepts_several_spellings() -> None:
    expected = nuts.evaluate("As Kd 7c 7h 2s")
    assert nuts.evaluate(["As", "Kd", "7c", "7h", "2s"]) == expected
    assert nuts.evaluate("as,kd,7C,7H,2S") == expected
    assert nuts.evaluate("AsKd7c7h2s") == expected
    assert nuts.evaluate("Ts 9s 8s 7s 6s") == nuts.evaluate("10s 9s 8s 7s 6s")


def test_evaluate_list_items_must_be_single_cards() -> None:
    with pytest.raises(ValueError):
        nuts.evaluate(["As Ks", "Qs", "Js", "Ts"])
    with pytest.raises(ValueError):
        nuts.evaluate(["AsKsQsJsTs"])
    with pytest.raises(TypeError):
        nuts.evaluate(b"As Ks Qs Js Ts")  # type: ignore[arg-type]
    with pytest.raises(TypeError):
        nuts.evaluate(["As", "Ks", "Qs", "Js", 10])  # type: ignore[list-item]
    assert nuts.evaluate(["10s", "js", "QS", "Ks", "As"]) == 7462


def test_evaluate_six_and_seven_cards_use_the_best_five() -> None:
    assert nuts.evaluate("As Ks Qs Js Ts 2c") == 7462
    assert nuts.evaluate("2c 3d As Ks Qs Js Ts") == 7462
    assert nuts.evaluate("Ah Ad Kc Kd Qs Qh 2c") == nuts.evaluate("Ah Ad Kc Kd Qs")


@pytest.mark.parametrize(
    "cards",
    ["As Ks Qs Js", "As Ks Qs Js Ts 9s 8s 7s", "As As Ks Qs Js", "As Ks Qs Js Xx", ""],
)
def test_evaluate_rejects_bad_hands(cards: str) -> None:
    with pytest.raises(ValueError):
        nuts.evaluate(cards)


@pytest.mark.parametrize(
    ("cards", "name"),
    [
        ("As Kd 9c 7h 2s", "High Card"),
        ("As Ad 9c 7h 2s", "One Pair"),
        ("As Ad 9c 9h 2s", "Two Pair"),
        ("As Ad Ac 7h 2s", "Three of a Kind"),
        ("As Kd Qc Jh Ts", "Straight"),
        ("As Ks 9s 7s 2s", "Flush"),
        ("As Ad Ac 7h 7s", "Full House"),
        ("As Ad Ac Ah 2s", "Four of a Kind"),
        ("9s 8s 7s 6s 5s", "Straight Flush"),
    ],
)
def test_category(cards: str, name: str) -> None:
    assert nuts.category(nuts.evaluate(cards)) == name


def test_category_blocks() -> None:
    # Ranks 1..7462 are assigned in blocks, one per category, weakest first.
    names = [nuts.category(rank) for rank in range(1, 7463)]
    assert [names.count(c) for c in CATEGORIES] == [1277, 2860, 858, 858, 10, 1277, 156, 156, 10]
    assert names == sorted(names, key=CATEGORIES.index)
    for bad in (0, 7463, -1, 2**40, -(2**40)):  # also beyond the C++ int
        with pytest.raises(ValueError):
            nuts.category(bad)


# ---- parse_range --------------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("text", "count"),
    [
        ("22+", 78),
        ("AK", 16),
        ("AKs", 4),
        ("AKo", 12),
        ("random", 1326),
        ("A2s+", 48),
        ("TT+, AKs", 34),
        ("K7o-KTo", 48),
        ("AsKs", 1),
        ("", 0),
    ],
)
def test_parse_range_combo_counts(text: str, count: int) -> None:
    assert len(nuts.parse_range(text)) == count


@pytest.fixture
def comma_decimal_locale() -> Iterator[str]:
    """Switches LC_NUMERIC to a locale that writes 0.5 as "0,5" for one test."""
    previous = locale.setlocale(locale.LC_NUMERIC)
    for name in ("de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR.utf8"):
        try:
            locale.setlocale(locale.LC_NUMERIC, name)
        except locale.Error:
            continue
        if locale.localeconv()["decimal_point"] == ",":
            break
    else:
        locale.setlocale(locale.LC_NUMERIC, previous)
        pytest.skip("no locale with a decimal comma is installed")
    try:
        yield name
    finally:
        locale.setlocale(locale.LC_NUMERIC, previous)


def test_range_weights_do_not_depend_on_the_locale(comma_decimal_locale: str) -> None:
    assert len(nuts.parse_range("AA:0.5, KK")) == 12
    with_weight = nuts.equity(["AA:0.5, KK", "QQ"], trials=1000, seed=1, threads=1)
    assert with_weight[0]["std_error"] > 0


def test_parse_range_combos() -> None:
    assert sorted(nuts.parse_range("AKs")) == ["AcKc", "AdKd", "AhKh", "AsKs"]
    assert nuts.parse_range("KsAs") == ["AsKs"]  # higher card first
    combos = nuts.parse_range("random")
    assert len(set(combos)) == 1326


@pytest.mark.parametrize("text", ["AKx", "AA+-KK", "XsKs", "AsAs"])
def test_parse_range_rejects_bad_ranges(text: str) -> None:
    with pytest.raises(ValueError):
        nuts.parse_range(text)


# ---- equity -------------------------------------------------------------------------------------


def test_exact_equity_known_matchup() -> None:
    # Values from the C++ tests, confirmed there by an independent brute force.
    aa, kk = nuts.equity(["AhAs", "KdKc"])
    assert aa["hand"] == "AhAs"
    assert aa["equity"] == pytest.approx(0.81255489679402726, abs=1e-12)
    assert aa["win"] == pytest.approx(0.81064577318046327, abs=1e-12)
    assert aa["tie"] == pytest.approx(0.0038182472271278932, abs=1e-12)
    assert kk["equity"] == pytest.approx(0.18744510320597277, abs=1e-12)
    assert aa["std_error"] == 0.0
    assert aa["equity"] + kk["equity"] == pytest.approx(1.0, abs=1e-12)


def test_exact_equity_with_a_board() -> None:
    players = nuts.equity(["AsKs", "QhQd", "7c6c"], "Qs Jh 2c")
    equities = [p["equity"] for p in players]
    assert equities == pytest.approx(
        [0.1583610188261351, 0.81063122923588038, 0.031007751937984496], abs=1e-12
    )
    # A board that makes a royal flush for everyone: a split pot.
    split = nuts.equity(["2c3d", "4h5c"], "As Ks Qs Js Ts")
    assert [p["tie"] for p in split] == [1.0, 1.0]
    assert [p["equity"] for p in split] == [0.5, 0.5]


def test_exact_is_chosen_automatically_for_specific_hands() -> None:
    assert nuts.equity(["AhAs", "KdKc"])[0]["std_error"] == 0.0  # exact
    assert nuts.equity(["AA", "KK"], trials=1000, threads=1)[0]["std_error"] > 0  # Monte Carlo
    monte_carlo = nuts.equity(["AhAs", "KdKc"], exact=False, trials=1000, threads=1)
    assert monte_carlo[0]["std_error"] > 0


def test_symmetric_matchup_is_symmetric() -> None:
    a, b = nuts.equity(["AsKd", "AhKc"])
    assert a["equity"] == pytest.approx(0.5, abs=1e-12)
    assert a["win"] == pytest.approx(b["win"], abs=1e-12)


def test_monte_carlo_agrees_with_exact() -> None:
    hands = ["AsKs", "8d8c"]
    exact = nuts.equity(hands, "Qs 7s 2d")
    estimate = nuts.equity(hands, "Qs 7s 2d", exact=False, trials=200_000, seed=7, threads=2)
    for e, m in zip(exact, estimate, strict=True):
        assert abs(e["equity"] - m["equity"]) < 4 * m["std_error"]


def test_monte_carlo_is_deterministic() -> None:
    def run(seed: int, threads: int) -> list[float]:
        players = nuts.equity(["QQ+, AKs", "random"], trials=20_000, seed=seed, threads=threads)
        return [p["equity"] for p in players]

    assert run(1, 3) == run(1, 3)
    assert run(1, 3) != run(2, 3)


def test_two_specific_cards_with_a_space() -> None:
    # "As Ks" is two specific cards everywhere, not only when every player has two cards.
    spaced = nuts.equity(["As Ks", "QQ"], trials=5000, seed=1, threads=1)
    plain = nuts.equity(["AsKs", "QQ"], trials=5000, seed=1, threads=1)
    assert [p["equity"] for p in spaced] == [p["equity"] for p in plain]
    assert spaced[0]["hand"] == "As Ks"
    exact = nuts.equity(["As Ks", "Qd Qc"])
    estimate = nuts.equity(["As Ks", "Qd Qc"], exact=False, trials=100_000, seed=2, threads=2)
    assert abs(estimate[0]["equity"] - exact[0]["equity"]) < 4 * estimate[0]["std_error"]


def test_many_random_players() -> None:
    # Up to 23 players holding "random": they are dealt from the cards left over, so this is
    # fast and never fails.
    players = nuts.equity(["random"] * 23, trials=2000, seed=1, threads=1)
    assert len(players) == 23
    assert sum(p["equity"] for p in players) == pytest.approx(1.0, abs=1e-9)
    mixed = nuts.equity(["AA"] + ["random"] * 15, trials=2000, seed=1, threads=1)
    assert mixed[0]["equity"] > mixed[1]["equity"]


def test_monte_carlo_range_weights() -> None:
    # KK:0 removes KK, leaving AA against QQ.
    with_weight = nuts.equity(["AA, KK:0", "QQ"], trials=20_000, seed=1, threads=1)
    plain = nuts.equity(["AA", "QQ"], trials=20_000, seed=1, threads=1)
    assert with_weight[0]["equity"] == plain[0]["equity"]


@pytest.mark.parametrize(
    ("hands", "board", "exact"),
    [
        (["AsKs"], "", None),  # one player
        (["AsKs", "AsQs"], "", None),  # a card twice
        (["AsKs", "QhQd"], "Ks 7c 2d", None),  # a hand card on the board
        (["AsKs", "QhQd"], "7c 2d 3h 4h 5h 6h", None),  # six board cards
        (["AsKs", "QhQd"], "7c 7c", None),  # a board card twice
        (["AA", "KK"], "", True),  # exact needs specific cards
        (["AKx", "KK"], "", False),  # bad range
        (["AA", "AA", "AA"], "", False),  # cannot deal three hands of aces
    ],
)
def test_equity_rejects_bad_input(hands: list[str], board: str, exact: bool | None) -> None:
    with pytest.raises(ValueError):
        nuts.equity(hands, board, exact=exact, trials=1000, threads=1)


def test_equity_rejects_bad_arguments() -> None:
    with pytest.raises(TypeError):
        nuts.equity("AsKs QhQd")  # type: ignore[arg-type]
    with pytest.raises(ValueError):
        nuts.equity(["AA", "KK"], trials=0)
    with pytest.raises(ValueError):
        nuts.equity(["AA", "KK"], seed=-1)
    with pytest.raises(ValueError):
        nuts.equity(["AA", "KK"], threads=-1)
    # Too large for the C++ integer types, or above the thread limit: ValueError, not a crash
    # (thousands of threads used to abort the interpreter) and not a TypeError.
    for bad in (
        {"trials": 2**64},
        {"seed": 2**64},
        {"threads": nuts.MAX_THREADS + 1},
        {"threads": 5000},
        {"threads": 2**31},
    ):
        with pytest.raises(ValueError):
            nuts.equity(["AA", "KK"], **bad)  # type: ignore[arg-type]
    assert nuts.MAX_THREADS == 256


def test_calls_from_several_threads_agree() -> None:
    # The long calls release the GIL, so they really run in parallel here; the results must not
    # change. (The push/fold equity matrix is also set up by whichever call comes first.)
    def work(stack: float) -> tuple[float, float]:
        solution = nuts.pushfold(stack)
        hands = nuts.equity(["AhAs", "KdKc"], "2c 7d")
        return solution["sb_ev"], hands[0]["equity"]

    stacks = [2.0, 5.0, 8.0, 12.0, 15.0, 19.5]
    with ThreadPoolExecutor(max_workers=4) as pool:
        parallel = list(pool.map(work, stacks))
    assert parallel == [work(stack) for stack in stacks]


# ---- solvers ------------------------------------------------------------------------------------


def test_kuhn_cfr() -> None:
    result = nuts.kuhn_cfr(100_000)
    assert result["iterations"] == 100_000
    assert abs(result["game_value"] + 1 / 18) < 1e-3
    assert result["exploitability"] < 1e-3
    strategy = result["strategy"]
    assert len(strategy) == 12
    assert strategy["Kb"] > 0.999  # the king always calls a bet
    assert strategy["Jb"] < 0.001  # the jack never calls one
    # Player 1 bets the king three times as often as it bluffs the jack.
    assert strategy["K"] == pytest.approx(3 * strategy["J"], abs=0.02)
    for bad in (0, 2**31):
        with pytest.raises(ValueError):
            nuts.kuhn_cfr(bad)


def test_pushfold_shape_and_sanity() -> None:
    result = nuts.pushfold(10)
    assert result["stack_bb"] == 10.0
    assert result["exploitability"] < 1e-3
    for key in ("hands", "shove", "call"):
        assert len(result[key]) == 13
        assert all(len(row) == 13 for row in result[key])
    hands = result["hands"]
    assert (hands[0][0], hands[0][1], hands[1][0], hands[12][12]) == ("AA", "AKs", "AKo", "22")
    shove = np.array(result["shove"])
    call = np.array(result["call"])
    assert ((shove >= 0) & (shove <= 1)).all()
    assert ((call >= 0) & (call <= 1)).all()
    assert shove[0][0] > 0.999  # AA always shoves ...
    assert call[0][0] > 0.999  # ... and always calls
    # 72o, the worst hand: offsuit hands are below the diagonal, in the row of the lower card
    # (the 2, row 12) and the column of the higher one (the 7, column 7). Never shoved at 20 bb
    # (up to the solver's tolerance: the average strategy keeps a trace of early iterations).
    assert hands[12][7] == "72o"
    assert nuts.pushfold(20)["shove"][12][7] < 1e-6


def test_pushfold_matches_committed_data() -> None:
    path = REPO_ROOT / "data" / "pushfold.json"
    if not path.exists():
        pytest.skip("data/pushfold.json is not available")
    solutions = {s["stack_bb"]: s for s in json.loads(path.read_text())["solutions"]}
    for stack in (1.0, 7.5, 20.0):
        result = nuts.pushfold(stack)
        committed = solutions[stack]
        # The file rounds the probabilities to 6 decimals.
        np.testing.assert_allclose(result["shove"], committed["shove"], rtol=0, atol=1e-6)
        np.testing.assert_allclose(result["call"], committed["call"], rtol=0, atol=1e-6)
        assert result["iterations"] == committed["iterations"]


@pytest.mark.parametrize("stack", [0.5, 0.0, -3.0, float("nan"), float("inf"), 100.5, 1e9])
def test_pushfold_rejects_bad_stacks(stack: float) -> None:
    with pytest.raises(ValueError):
        nuts.pushfold(stack)


def test_pushfold_deepest_stack() -> None:
    assert nuts.MAX_STACK_BB == 100
    result = nuts.pushfold(100)
    assert result["exploitability"] <= 1e-6
