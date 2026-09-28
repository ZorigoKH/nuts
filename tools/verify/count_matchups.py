"""Independent count of the distinct heads-up preflop matchups (47,008).

tests/cpp/test_pushfold.cpp checks that the 1,624,350 ordered pairs of two-card hands with no
card in common fall into 47,008 classes when two matchups count as the same if one becomes the
other by renaming the suits (e.g. hearts <-> spades everywhere) and possibly swapping the two
players. compute_equity169() enumerates the boards of one matchup per class. This script counts
the classes by brute force, without the C++ code.

Usage (from the repository root; no dependencies, takes about 10 seconds):

    python tools/verify/count_matchups.py
"""

from __future__ import annotations

import itertools
import sys

# Card id = 4 * rank + suit, as in the C++ code.
CARDS = range(52)
SUIT_PERMUTATIONS = list(itertools.permutations(range(4)))  # all 24 ways to rename the suits


def main() -> int:
    hands = list(itertools.combinations(CARDS, 2))  # 1,326 hands, each a sorted pair of ids
    # renamed[p][card] = the card after renaming the suits with permutation p.
    renamed = [[4 * (c // 4) + p[c % 4] for c in CARDS] for p in SUIT_PERMUTATIONS]

    classes = set()
    ordered_pairs = 0
    for a, b in itertools.combinations(hands, 2):  # each unordered pair of different hands once
        if set(a) & set(b):
            continue
        ordered_pairs += 2  # (a, b) and (b, a)
        # The class of {a, b}: its smallest form over all suit renamings. Each hand is sorted
        # and the two hands are sorted, so the player order does not matter.
        classes.add(
            min(
                tuple(sorted((tuple(sorted(r[x] for x in a)), tuple(sorted(r[x] for x in b)))))
                for r in renamed
            )
        )

    print(f"ordered pairs of hands without a common card: {ordered_pairs:,}")
    print(f"matchups up to suit renaming and player order: {len(classes):,}")
    ok = ordered_pairs == 1_624_350 and len(classes) == 47_008
    print("ok" if ok else "FAIL: expected 1,624,350 and 47,008")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
