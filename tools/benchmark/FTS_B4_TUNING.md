# B4 full-text index tuning

Hot-path changes against `2026-competition`, aimed at the three equal-weight
categories in `fts_large_bench_score.py`: index build, `TOKENIZE`, and `MATCH`
aggregation (`query_cn`, `query_beng`, `query_mixed`, `query_limit`).

The cloud agent cannot boot seekdb or run `fts_large_bench.sh`. The commands
below are the measurement procedure. Score the report with the checked-in
baseline; do not edit `fts_large_bench_baseline.json`.

## What changed

Parser and index build (build_* and tokenize_*):

- One IK parser and one basic-English parser are kept per thread. Later
  documents reuse the dictionary pins, segmenters, and analyzer. A nested
  call on the same thread still allocates a private parser. A pooled IK
  parser is only reused while the dictionary hub can still load the ranges it
  is pinned to; otherwise it is dropped and the next document does a normal
  fresh init.
- Parser slots and the stopword conversion arena are thread-local through
  `ObDITls`, which deletes them at thread exit and will not build a new one
  while the thread is leaving.
- IK token lists and trie-hit lists use `ObFastSegmentArray`, a ring buffer
  whose `reset()` keeps its storage. Hits are copied with the copy
  constructor, since `ObDATrieHit::operator=` does not copy `dict_`.
- The IK arbitrator keeps its hash buckets and arena across batches instead
  of creating them for every 1000-character window.
- Dictionary range lookup is a binary search. Ranges are still produced in
  first-character order.
- `casedown` skips the heap copy when folding does not change the bytes.
  Unchanged tokens still point at the source document, which lives until the
  caller finishes inserting the index rows (the same window as the per-row
  arena).
- Stopword checks for ASCII-compatible charsets fold `A-Z` on the stack and
  probe the existing singleton set. Other charsets reuse one convert arena
  per thread instead of allocating one per token.
- Thread-local parsers are heap allocated on first use and are not destroyed
  by a later thread's static destructor, so dictionary and analyzer teardown
  never races with runtime shutdown.

Query aggregation (`query_*`):

- BM25 is still evaluated through the expression's own `eval` / `eval_batch`,
  which is the path the plan's vectorized format and skip vector expect.
- The per-token row-count estimate reuses its arena.

Token text, stopword decisions, and BM25 weights are unchanged, so
`query_*_hits` should stay on the baseline values (8001 / 11000 / 7332 / 20
for the default 20k-row run).

## How to measure

Use the same machine, the same `seekdb` build flags, and a quiet box. Repeat
each label at least three times and compare medians. `select1_avg_ms` is the
SQL-loop floor; tokenize time cannot go below it.

```bash
# Baseline binary, checked out at 2026-competition.
LABEL=before OUTPUT=/tmp/fts_before.txt ./tools/benchmark/fts_large_bench.sh

# This branch, same ROWS/BATCH/ROUNDS/QUERY_ROUNDS/SAMPLES/WARMUP.
LABEL=after OUTPUT=/tmp/fts_after.txt ./tools/benchmark/fts_large_bench.sh

python3 tools/benchmark/fts_large_bench_score.py /tmp/fts_after.txt
```

The scorer requires the default config (`rows=20000`, `batch=500`,
`rounds=3000`, `query_rounds=200`, `samples=3`, `warmup=30`, `skip_load=0`).
A smaller smoke run is fine while iterating; pass `--allow-config-mismatch`
and do not treat that score as the contest number.

```bash
LABEL=smoke ROWS=2000 ROUNDS=200 QUERY_ROUNDS=20 SAMPLES=1 WARMUP=5 \
  ./tools/benchmark/fts_large_bench.sh
```

`fts_large_bench_score.py` scores three categories with equal weight:

- build: `build_ik_all_sec`, `build_ik_content_sec`, `build_beng_en_sec`
- tokenize: `tokenize_ik_avg_ms`, `tokenize_beng_avg_ms`
- query: `query_cn_avg_ms`, `query_beng_avg_ms`, `query_mixed_avg_ms`,
  `query_limit_avg_ms`

Lower is better. Full marks inside the scorer is a 50% mean category
improvement versus `fts_large_bench_baseline.json`. The contest points are
separate from that 0-100 script score.

## Correctness checks

On a seekdb that can run SQL, compare token sets before and after. They
should be identical for both parsers:

```sql
SELECT tokenize('OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化', 'ik');
SELECT tokenize('The quick brown fox jumps over the lazy dog.', 'beng');
```

After the bench load, the report's `query_*_hits` lines should match the
baseline file. A hit-count change means tokenization or scoring drifted.

## What this environment did not run

No `observer` / `seekdb` binary was started here, so there is no before/after
timing in this change. The expected direction, once the bench is run on a
real server:

- `build_ik_*` and `tokenize_ik_avg_ms` move the most (parser, dictionary,
  and casedown copies are per document / per `TOKENIZE`).
- `build_beng_en_sec` and `tokenize_beng_avg_ms` move from the parser reuse
  and the stopword check.
- `query_*_avg_ms` moves only from the estimate arena reuse; `query_limit`
  stays close to the other query times if the `LIMIT` is applied above the
  text-retrieval scan rather than inside it; this change does not rewrite
  that plan.
