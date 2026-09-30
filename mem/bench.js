#!/usr/bin/env node
//
// bench.js -- time table and sequence operations on a plain JS object,
// a Map, and an Array. (JavaScript has no built-in deque.)
//
// Same workload as bench.cpp and bench.py, so the numbers line up.
// The first five ops use the structure as a table from key to value:
//
//   keys   = a shuffled permutation of 0..n-1 (same order in every language)
//   insert : table[key] = 2*key            for each key
//   lookup : sum += table[key]             for each key   (all hits)
//   miss   : count += (n+key in table)     for each key   (all misses)
//   iterate: sum += value                  for every entry
//   erase  : remove key                    for each key
//
// The last two start from empty and add n entries, each one at an end:
//
//   append-last : Array: push.    object/Map: keys 0, 1, ..., n-1
//   append-first: Array: unshift. object/Map: keys n-1, ..., 1, 0
//
// JavaScript cannot see when V8 reallocates a backing store or calls the
// allocator, so the resizes/allocs/frees columns print as "-".
//
// usage: ./bench.js [--reps R] [n ...]
//

"use strict";

const OPS = ["insert", "lookup", "miss", "iterate", "erase", "append-last", "append-first"];

// Ops that cost O(n) per step (so n^2 in all) are skipped above this size.
// Here that is Array append-first: every unshift shifts the whole array over.
const QUADRATIC_MAX = 100_000;

// Fisher-Yates shuffle of 0..n-1 driven by xorshift32 (same as bench.cpp).
function shuffled(n) {
  const keys = new Array(n);
  for (let i = 0; i < n; ++i) keys[i] = i;
  let x = 2026;
  for (let i = n - 1; i > 0; --i) {
    x ^= x << 13;
    x ^= x >>> 17;
    x ^= x << 5;
    const j = (x >>> 0) % (i + 1);
    [keys[i], keys[j]] = [keys[j], keys[i]];
  }
  return keys;
}

function seconds(f) {
  const t0 = process.hrtime.bigint();
  f();
  return Number(process.hrtime.bigint() - t0) / 1e9;
}

function check(ok, what) {
  if (!ok) {
    console.error(`check failed: ${what}`);
    process.exit(1);
  }
}

// Start from make(), then add(table, k) for k = 0..n-1 (or n-1..0 when
// descending, which puts each new key or value first). size(table) checks it.
function appending(n, make, add, descending, size) {
  let table;
  const sec = seconds(() => {
    table = make();
    if (descending) for (let k = n - 1; k >= 0; --k) add(table, k);
    else for (let k = 0; k < n; ++k) add(table, k);
  });
  check(size(table) === n, "append size");
  return sec;
}

// A plain object used as a dictionary. Note V8 stores integer-like keys as
// "elements" (array-ish storage), not as named properties.
function objectPass(keys) {
  const n = keys.length;
  const table = {};
  let sum = 0, count = 0;
  const t = [];

  t.push(seconds(() => { for (const k of keys) table[k] = 2 * k; }));
  t.push(seconds(() => { for (const k of keys) sum += table[k]; }));
  check(sum === n * (n - 1), "lookup sum");
  t.push(seconds(() => { for (const k of keys) count += (n + k) in table; }));
  check(count === 0, "miss count");
  sum = 0;
  t.push(seconds(() => { for (const k in table) sum += table[k]; }));
  check(sum === n * (n - 1), "iterate sum");
  t.push(seconds(() => { for (const k of keys) delete table[k]; }));
  for (const k in table) check(false, "erase empty");
  t.push(appending(n, () => ({}), (o, k) => { o[k] = 2 * k; }, false, (o) => Object.keys(o).length));
  t.push(appending(n, () => ({}), (o, k) => { o[k] = 2 * k; }, true, (o) => Object.keys(o).length));
  return t;
}

function mapPass(keys) {
  const n = keys.length;
  const table = new Map();
  let sum = 0, count = 0;
  const t = [];

  t.push(seconds(() => { for (const k of keys) table.set(k, 2 * k); }));
  t.push(seconds(() => { for (const k of keys) sum += table.get(k); }));
  check(sum === n * (n - 1), "lookup sum");
  t.push(seconds(() => { for (const k of keys) count += table.has(n + k); }));
  check(count === 0, "miss count");
  sum = 0;
  t.push(seconds(() => { for (const v of table.values()) sum += v; }));
  check(sum === n * (n - 1), "iterate sum");
  t.push(seconds(() => { for (const k of keys) table.delete(k); }));
  check(table.size === 0, "erase empty");
  t.push(appending(n, () => new Map(), (m, k) => { m.set(k, 2 * k); }, false, (m) => m.size));
  t.push(appending(n, () => new Map(), (m, k) => { m.set(k, 2 * k); }, true, (m) => m.size));
  return t;
}

// An Array is a "table" whose keys are exactly 0..n-1: the key *is* the index.
// It grows on demand (push) as keys arrive, and a slot holding EMPTY has no key.
// Erase empties the slot and pops empty slots off the end; V8 trims the
// backing store on its own as the array shrinks.
const EMPTY = -1;

function arrayPass(keys) {
  const n = keys.length;
  let table;
  let sum = 0, count = 0;
  const t = [];

  t.push(seconds(() => {
    table = [];
    for (const k of keys) {
      while (table.length <= k) table.push(EMPTY);
      table[k] = 2 * k;
    }
  }));
  t.push(seconds(() => { for (const k of keys) sum += table[k]; }));
  check(sum === n * (n - 1), "lookup sum");
  t.push(seconds(() => {
    const size = table.length;
    for (const k of keys) {
      const q = n + k;
      count += q < size && table[q] !== EMPTY;
    }
  }));
  check(count === 0, "miss count");
  sum = 0;
  t.push(seconds(() => { for (const v of table) if (v !== EMPTY) sum += v; }));
  check(sum === n * (n - 1), "iterate sum");
  t.push(seconds(() => {
    for (const k of keys) {
      table[k] = EMPTY;
      while (table.length > 0 && table[table.length - 1] === EMPTY) table.pop();
    }
  }));
  check(table.length === 0, "erase empty");
  t.push(appending(n, () => [], (a, k) => { a.push(2 * k); }, false, (a) => a.length));
  t.push(n > QUADRATIC_MAX ? null :
         appending(n, () => [], (a, k) => { a.unshift(2 * k); }, true, (a) => a.length));
  return t;
}

function report(name, n, reps, run) {
  const best = OPS.map(() => null);
  for (let r = 0; r < reps; ++r) {
    run().forEach((t, i) => {
      if (t !== null && (best[i] === null || t < best[i])) best[i] = t;
    });
  }
  OPS.forEach((op, i) => {
    const ns = best[i] === null ? "-" : (1e9 * best[i] / n).toFixed(1);
    console.log(`${"node".padEnd(8)} ${name.padEnd(22)} ${String(n).padStart(10)} ${op.padEnd(12)} ${ns.padStart(10)} ${"-".padStart(8)} ${"-".padStart(8)} ${"-".padStart(8)}`);
  });
}

function main(argv) {
  let reps = 3;
  const sizes = [];
  for (let i = 2; i < argv.length; ++i) {
    if (argv[i] === "--reps") reps = parseInt(argv[++i]);
    else sizes.push(parseInt(argv[i]));
  }
  if (sizes.length === 0) sizes.push(1_000, 10_000, 100_000, 1_000_000);

  console.log(`${"lang".padEnd(8)} ${"structure".padEnd(22)} ${"n".padStart(10)} ${"op".padEnd(12)} ${"ns/op".padStart(10)} ${"resizes".padStart(8)} ${"allocs".padStart(8)} ${"frees".padStart(8)}`);
  for (const n of sizes) {
    const keys = shuffled(n);
    report("object", n, reps, () => objectPass(keys));
    report("Map", n, reps, () => mapPass(keys));
    report("Array", n, reps, () => arrayPass(keys));
  }
}

main(process.argv);
