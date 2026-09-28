# Push/fold thresholds compared with HoldemResources

This checks our heads-up push/fold solution against a published one.

## The reference

HoldemResources.net, "HeadsUp Push/Fold Nash Equilibrium",
<https://www.holdemresources.net/hune>, the **No Ante** charts (pusher and caller). The values
were read from the page's HTML tables on 2026-09-27.

Each cell of those charts is "the highest effective stack size in big blinds where a hand can be
pushed or called". The stack is counted before the blinds are posted, and "20+" means the hand is
played at every stack up to 20 bb. It is the same game as ours: the SB posts 0.5 and can only
shove or fold, the BB posts 1 and can only call or fold, there is no ante, and it is chip EV
(heads-up, so there is no ICM).

The page says its chart is a simplified version of the exact mixed-strategy solution. Most hands
are mixed at some stacks, and many small gaps are left out. The chart shows one decimal.

## Our numbers

```sh
./build/nuts_gen_pushfold --thresholds data/equity169.bin
```

This solves the game at S = 1.00, 1.05, ..., 20.00 bb (381 stacks, each with CFR+ until the
exploitability is at most 1e-6 bb per hand; 2.7 to 3.6 s over our runs on an Apple M3 with 8
threads). For every hand it
prints the **largest stack at which the hand is shoved (or called) with probability at least 1/2**.
Our grid is 0.05 bb, so our thresholds are multiples of 0.05.

## Summary

Over the whole No Ante chart (all 169 hands, both roles):

| | numeric cells | within ±0.25 bb | within 0.05 bb | "20+" cells that agree |
|---|---|---|---|---|
| call (BB) | 127 | **127** | 127 | 42 of 42 |
| shove (SB) | 85 | **84** | 82 | 81 of 81 |

For the three shove cells that the chart marks with footnoted gaps (63s, 53s, 43s), the top
thresholds agree, but the lower intervals of 63s and 43s do not: we fold them at stacks where the
chart shoves (see "Footnoted gaps" and "Disagreements"). Only one numeric cell differs by more
than 0.25 bb: **Q4o (shove)**, also explained below.

These thresholds come from solves to 1e-6 bb. Solved to 1e-8 bb, a few of them move by one
0.05 bb step, and then 84 of the 85 numeric shove cells are within 0.05 bb (see "Disagreements").

Nearly all differences are 0 or +0.05 bb, and we are never lower by more than 0.05. That fits the
chart cutting a 0.05 bb value down to one decimal. We have not checked this, because we did not
download the raw 0.05 bb data that the page links to.

## Selected hands

These hands are spread over the chart. They include hands the chart pushes at every stack ("20+"),
hands with numeric thresholds, and the three footnoted gap hands. "Played in" lists the stack
intervals where we play the hand, when there is more than one.

| hand | role | HoldemResources (bb) | ours (bb) | ours − theirs | within ±0.25 | notes |
|---|---|---|---|---|---|---|
| 72o | shove | 1.6 | 1.60 | +0.00 | yes | |
| 32o | shove | 1.4 | 1.40 | +0.00 | yes | |
| K2o | shove | 11.6 | 11.65 | +0.05 | yes | |
| Q2o | shove | 7.0 | 7.05 | +0.05 | yes | |
| J5s | shove | 14.7 | 14.75 | +0.05 | yes | |
| T8o | shove | 17.5 | 17.50 | +0.00 | yes | |
| 96s | shove | 20+ | 20+ | – | yes | |
| A2o | shove | 20+ | 20+ | – | yes | |
| 22 | shove | 20+ | 20+ | – | yes | |
| K9o | shove | 20+ | 20+ | – | yes | |
| 54s | shove | 20+ | 20+ | – | yes | |
| Q7o | shove | 10.3 | 10.35 | +0.05 | yes | played in 1.00–10.05, 10.20–10.35 |
| T3s | shove | 7.7 | 7.70 | +0.00 | yes | played in 1.00–7.20, 7.35–7.50, 7.70 |
| K3s | shove | 19.9 | 19.90 | +0.00 | yes | |
| K2s | shove | 19.3 | 19.30 | +0.00 | yes | |
| Q2s | shove | 12.7 | 12.70 | +0.00 | yes | |
| J2s | shove | 8.5 | 8.60 | +0.10 | yes | 8.55 when solved to 1e-8 bb |
| T2s | shove | 6.5 | 6.50 | +0.00 | yes | played in 1.00–6.10, 6.45–6.50 |
| 92s | shove | 3.7 | 3.75 | +0.05 | yes | played in 1.00–3.45, 3.70–3.75 |
| J2o | shove | 4.6 | 4.60 | +0.00 | yes | |
| T2o | shove | 2.9 | 3.00 | +0.10 | yes | 2.95 when solved to 1e-8 bb |
| 92o | shove | 2.2 | 2.25 | +0.05 | yes | |
| K8o | shove | 18.0 | 18.00 | +0.00 | yes | |
| J7o | shove | 8.5 | 8.50 | +0.00 | yes | |
| 87o | shove | 14.7 | 14.70 | +0.00 | yes | |
| 76o | shove | 10.7 | 10.75 | +0.05 | yes | |
| 95s | shove | 14.4 | 14.45 | +0.05 | yes | |
| 84s | shove | 10.1 | 10.05 | −0.05 | yes | played in 1.00–7.90, 8.40–8.90, 9.65–10.05 |
| 63s | shove | 7.1 (gap) | 7.15 | +0.05 | top only | see "Footnoted gaps" |
| 53s | shove | 12.9 (gap) | 12.90 | +0.00 | yes | see "Footnoted gaps" |
| 43s | shove | 10.0 (gap) | 10.00 | +0.00 | top only | see "Footnoted gaps" |
| **Q4o** | shove | 7.9 | 8.30 | +0.40 | **no** | played in 1.00–7.90, 8.25–8.30; see "Disagreements" |
| 72o | call | 2.6 | 2.65 | +0.05 | yes | |
| 32o | call | 2.6 | 2.65 | +0.05 | yes | |
| K2o | call | 8.1 | 8.15 | +0.05 | yes | |
| Q2o | call | 5.6 | 5.60 | +0.00 | yes | |
| J5s | call | 6.9 | 6.90 | +0.00 | yes | |
| T8o | call | 6.6 | 6.65 | +0.05 | yes | |
| 96s | call | 5.8 | 5.85 | +0.05 | yes | |
| A2o | call | 15.8 | 15.85 | +0.05 | yes | |
| 22 | call | 15.0 | 15.05 | +0.05 | yes | |
| K9o | call | 17.1 | 17.15 | +0.05 | yes | |
| 54s | call | 4.6 | 4.60 | +0.00 | yes | |
| Q7o | call | 8.0 | 8.00 | +0.00 | yes | |
| T3s | call | 4.8 | 4.85 | +0.05 | yes | |
| K2s | call | 10.7 | 10.75 | +0.05 | yes | |
| Q2s | call | 7.2 | 7.25 | +0.05 | yes | |
| A4o | call | 18.3 | 18.35 | +0.05 | yes | |
| J2o | call | 4.2 | 4.20 | +0.00 | yes | |
| T9o | call | 8.4 | 8.40 | +0.00 | yes | |

## Footnoted gaps

The chart's footnotes give these three hands two separate intervals. The top ends agree with
ours, and so do the short-stack intervals. Inside the main interval, 53s nearly agrees (we fold it
at 3.85 and 4.00 bb only), but 63s and 43s do not: we fold them over whole stretches where the
chart shoves.

| hand | HoldemResources: pushed at | ours: shoved (probability at least 1/2) at |
|---|---|---|
| 63s | 5.1–7.1 bb and below 2.3 bb | 6.05–7.15 (plus 5.10–5.20, 5.65–5.75, 5.95) and 1.00–2.35 |
| 53s | 3.8–12.9 bb and below 2.4 bb | 4.05–12.90 (plus 3.75–3.80, 3.90–3.95) and 1.00–2.40 |
| 43s | 4.9–10.0 bb and below 2.2 bb | 4.85–7.85, 8.55–8.60, 8.75–9.00, 9.15–9.35, 9.55–9.60, 9.75–9.90, 10.00 and 1.00–2.20 |

So we fold 63s at 5.25–5.60, 5.80–5.90 and 6.00 bb, and 43s at 7.90–8.50 bb and at several
shorter stretches up to 9.95 bb: from 7.90 to 10.00 bb it is shoved at only 20 of the 43 stacks
of our grid.

## Disagreements

- **Q4o shove: 7.9 there, 8.30 here.** We shove Q4o at every stack up to 7.90, which matches
  their 7.9. We then fold it from 7.95 to 8.20 and shove it again at 8.25 and 8.30. The chart
  says it leaves out small gaps, and this 0.1 bb island above the main interval looks like one
  of them. If only the main interval counts, the two agree exactly.
- **63s and 43s inside the chart's intervals.** We fold 63s at 5.25–5.60 bb (shove
  probability 0.0007, 0.0005 and 0.0017 at 5.3, 5.4 and 5.5 bb), at 5.80–5.90 and at 6.00 bb,
  where the chart shoves it (5.1–7.1 bb). We fold 43s at 7.90–8.50 bb (probability 0.0011 at
  8.0 bb in `data/pushfold.json`, and at most 0.02 from 7.9 to 8.4 bb) and at several shorter
  stretches up to 9.95 bb, where the chart shoves it (4.9–10.0 bb). These are holes of 0.35 and
  0.65 bb, not small gaps. The chart says it is simplified and leaves out small gaps; it may
  have filled these in too, but we cannot tell from the chart alone.
- **Short extra intervals** also show up for Q7o, T3s, T2s, 92s, 84s and 43s. Near its
  threshold, a hand is close to indifferent between its two actions, so the equilibrium can
  switch it on and off as the stack changes. A threshold that uses "probability at least 1/2"
  then depends on where that happens, and some of it depends on how precisely the game is
  solved. We solved all 381 stacks again to exploitability 1e-8 bb (instead of 1e-6): 8 of the
  338 thresholds moved by one 0.05 bb step (shove: J2s 8.60 → 8.55, J4s 13.50 → 13.55,
  J6o 6.55 → 6.50, T2o 3.00 → 2.95, 84s 10.05 → 10.10; call: J2s 5.60 → 5.65, J7s 8.85 → 8.80,
  95o 3.70 → 3.75), and one inner interval end moved by 0.05 bb for 94s, J8o, 74s, 86o, 53s and
  43s. With the 1e-8 thresholds, 84 of the 85 numeric shove cells are within 0.05 bb (instead of
  82); the ±0.25 bb result does not change (84 of 85, only Q4o outside). Mixed probabilities
  move too, in the second or third decimal: 43s is shoved with probability 0.4515 at 9.5 bb in
  `data/pushfold.json` (1e-6), and 0.4586, 0.4585, 0.4585 when solved to 1e-7, 1e-8, 1e-9 bb;
  at 10.0 bb, 0.7123 in the file and 0.7094, 0.7108, 0.7107.

## Not compared

- The raw data at 0.05 bb resolution that the page links to
  (`holdemresources_hu_equilibrium.zip`). We did not download it. The page says it was computed
  again with a later version and may differ very slightly from the chart.
- The "Ante 10%" charts, because our model has no ante.
