// Smoke test for the WebAssembly build: loads nuts.mjs in Node.js and checks known answers.
//
// Usage (from the repository root, after building; see wasm/README.md):
//     node wasm/smoke.mjs [BUILD_DIR]        (default BUILD_DIR: build-wasm)
//
// Exits with status 1 if any check fails. Also prints how long the slower calls took.
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const buildDir = path.resolve(process.argv[2] ?? path.join(repoRoot, "build-wasm"));
const { default: createNuts } = await import(pathToFileURL(path.join(buildDir, "nuts.mjs")).href);
const nuts = await createNuts();

let failures = 0;
function check(condition, what) {
  console.log(`${condition ? "ok  " : "FAIL"}  ${what}`);
  if (!condition) failures += 1;
}
function near(actual, expected, tolerance) {
  return Math.abs(actual - expected) <= tolerance;
}
function timed(call) {
  const start = performance.now();
  const result = call();
  return [result, performance.now() - start];
}

// ---- evaluate / category ----------------------------------------------------------------------
check(nuts.evaluate("As Ks Qs Js Ts") === 7462, "royal flush is 7462");
check(nuts.evaluate("7c 5d 4h 3s 2c") === 1, "7-5-4-3-2 offsuit is 1");
check(nuts.evaluate("5s 4s 3s 2s As") === 7453, "the wheel straight flush is 7453");
check(nuts.evaluate("2c 3d As Ks Qs Js Ts") === 7462, "7 cards: the best five count");
check(nuts.evaluate("As Ks Qs Js") === 0 && nuts.evaluate("As As Ks Qs Js") === 0,
      "invalid hands give 0");
check(nuts.category(nuts.evaluate("As Ad Kc Kd 7h")) === "Two Pair", "category: Two Pair");
check(nuts.category(7462) === "Straight Flush" && nuts.category(0) === "",
      "category: Straight Flush, and '' for 0");

// ---- parse_range ------------------------------------------------------------------------------
const rangeCounts = { "22+": 78, AK: 16, AKs: 4, AKo: 12, random: 1326, "A2s+": 48 };
for (const [text, count] of Object.entries(rangeCounts)) {
  const range = nuts.parse_range(text);
  check(range.error === undefined && range.combos.length === count,
        `parse_range("${text}") has ${count} combos`);
}
check(typeof nuts.parse_range("AKx").error === "string", "parse_range reports a bad range");

// ---- equity -----------------------------------------------------------------------------------
const [aaKk, exactMs] = timed(() => nuts.equity(["AhAs", "KdKc"], "", 0, 0));
check(aaKk.exact && aaKk.deals === 1712304, "exact preflop equity enumerates 1,712,304 boards");
check(near(aaKk.players[0].equity, 0.81255489679402726, 1e-12) &&
      near(aaKk.players[0].win, 0.81064577318046327, 1e-12) &&
      near(aaKk.players[1].equity, 0.18744510320597277, 1e-12),
      `AhAs vs KdKc: ${aaKk.players[0].equity.toFixed(6)} (${exactMs.toFixed(0)} ms)`);

const flop = nuts.equity(["AsKs", "QhQd", "7c6c"], "Qs Jh 2c", 0, 0);
check(flop.deals === 903 && near(flop.players[0].equity, 0.1583610188261351, 1e-12) &&
      near(flop.players[1].equity, 0.81063122923588038, 1e-12),
      "three players on a flop");

// Monte Carlo (ranges): AA against KK, compared with the exact class equity in the committed
// matrix (data/equity169.bin: 16-byte header, then 169 x 169 weights, then 169 x 169 half pots,
// all little-endian uint32; AA is class 0 and KK class 14).
const matrix = readFileSync(path.join(repoRoot, "data", "equity169.bin"));
const cell = 0 * 169 + 14;
const weight = matrix.readUInt32LE(16 + 4 * cell);
const halfPots = matrix.readUInt32LE(16 + 4 * (169 * 169 + cell));
const aaVsKk = halfPots / (2 * 1712304 * weight);
const [mc, mcMs] = timed(() => nuts.equity(["AA", "KK"], "", 200000, 1));
check(!mc.exact && mc.deals === 200000 &&
      Math.abs(mc.players[0].equity - aaVsKk) < 4 * mc.players[0].std_error,
      `Monte Carlo AA vs KK ${mc.players[0].equity.toFixed(4)} +- ` +
      `${mc.players[0].std_error.toFixed(4)}, exact ${aaVsKk.toFixed(4)} ` +
      `(200,000 trials, ${mcMs.toFixed(0)} ms)`);
const again = nuts.equity(["AA", "KK"], "", 200000, 1);
const otherSeed = nuts.equity(["AA", "KK"], "", 200000, 2);
check(again.players[0].equity === mc.players[0].equity &&
      otherSeed.players[0].equity !== mc.players[0].equity,
      "Monte Carlo: same seed, same result; another seed, another result");
const spaced = nuts.equity(["As Ks", "QQ"], "", 20000, 3);
const unspaced = nuts.equity(["AsKs", "QQ"], "", 20000, 3);
check(spaced.error === undefined && spaced.players[0].equity === unspaced.players[0].equity,
      "Monte Carlo: \"As Ks\" is the same hand as \"AsKs\"");
const table = nuts.equity(Array(23).fill("random"), "", 2000, 1);
check(table.error === undefined && table.players.length === 23 &&
      near(table.players.reduce((sum, p) => sum + p.equity, 0), 1, 1e-9),
      "Monte Carlo: 23 players with random hands");
check(typeof nuts.equity(["AsKs", "AsQs"], "", 0, 0).error === "string" &&
      typeof nuts.equity(["AA", "KK"], "Xx", 1000, 1).error === "string" &&
      typeof nuts.equity(["AA", "KK"], "", 0, 1).error === "string",
      "equity reports errors (card used twice, bad board, 0 trials)");

// ---- push/fold --------------------------------------------------------------------------------
const committed = JSON.parse(readFileSync(path.join(repoRoot, "data", "pushfold.json"), "utf8"));
const [solution, solveMs] = timed(() => nuts.pushfold(10));
const expected = committed.solutions.find((s) => s.stack_bb === 10);
let largestDifference = 0;
for (const grid of ["shove", "call"]) {
  for (let row = 0; row < 13; row += 1) {
    for (let column = 0; column < 13; column += 1) {
      const difference = Math.abs(solution[grid][row][column] - expected[grid][row][column]);
      largestDifference = Math.max(largestDifference, difference);
    }
  }
}
check(solution.hands[0][0] === "AA" && solution.hands[0][1] === "AKs" &&
      solution.hands[1][0] === "AKo", "push/fold chart layout");
check(solution.exploitability < 1e-3 && solution.iterations === expected.iterations &&
      largestDifference <= 1e-6,
      `pushfold(10) matches data/pushfold.json (largest difference ` +
      `${largestDifference.toExponential(1)}, ${solveMs.toFixed(0)} ms)`);
check(typeof nuts.pushfold(0.5).error === "string" && typeof nuts.pushfold(100.5).error === "string",
      "pushfold reports a stack below 1 bb or above 100 bb");

// ---- all 133,784,560 seven-card hands ---------------------------------------------------------
const all = nuts.count_all_seven_card_hands();
const expectedCounts = {
  "Straight Flush": 41584,  // 4,324 royal + 37,260 other
  "Four of a Kind": 224848,
  "Full House": 3473184,
  "Flush": 4047644,
  "Straight": 6180020,
  "Three of a Kind": 6461620,
  "Two Pair": 31433400,
  "One Pair": 58627800,
  "High Card": 23294460,
};
const countsMatch = Object.entries(expectedCounts).every(([name, n]) => all.categories[name] === n);
check(all.hands === 133784560 && all.distinct_values === 4824 && all.royal_flushes === 4324 &&
      countsMatch,
      `all 133,784,560 seven-card hands: category counts match, 4,824 distinct values ` +
      `(${(all.elapsed_ms / 1000).toFixed(2)} s, ` +
      `${(all.hands / all.elapsed_ms / 1000).toFixed(1)} M hands/s)`);

console.log(failures === 0 ? "\nall checks passed" : `\n${failures} check(s) failed`);
process.exit(failures === 0 ? 0 : 1);
