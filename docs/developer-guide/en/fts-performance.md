# Full-text dictionary and normalization overhead

IK looked up each input character by linearly checking dictionary ranges with
the UTF8MB4_BIN collation comparator. The built-in main dictionary has 275,908
words in six ranges. Letters outside these ranges also paid for the entire scan.

The reader now decodes and validates single-character range boundaries once.
For ordered, disjoint ranges, it decodes the probe once and searches integer
code-point bounds. Unordered, overlapping, malformed or multicharacter bounds
retain the original comparator path; malformed or multicharacter probes also
use that path. The dictionary contents, partition boundaries and token rules
are unchanged.

The English normalizer also avoids case conversion and a temporary allocation
when a trimmed UTF-8 token already consists of lowercase ASCII. Uppercase,
non-ASCII and other character sets retain the existing conversion. The fast
path borrows the current input document; the existing consumers still copy
tokens that they retain. A regression covers duplicate tokens and successive
documents to check ownership.

## Correctness

Code commit: `2327ff5b8f0d03aa064d8d3a5e790a8fab943fa8`.

- LLVM 19 RelWithDebInfo build completed on macOS arm64.
- 130 SQL integration tests passed: 102 AI-function tests, 25 File SQL tests,
  and three FTS tests. The FTS references cover 98 cases, including every
  built-in dictionary range boundary, gaps, duplicate frequencies, mixed text,
  fullwidth characters and existing unsupported-encoding errors.
- The original `fts_index.tokenize_function` and `fts_index.simple_query`
  mysqltests passed against their unchanged expected results.
- All complete benchmark runs retained query hit counts 8,001 / 11,000 / 7,332 /
  20, and the five benchmark/data/scoring files retained identical SHA-256s.

The token references were captured from baseline commit
`154bac9c8857bfa37517e17175ab68a0379f2c1d`, binary SHA-256
`422178c29d0147de119a838a5371a09042487c7afb8a5ed9d30cf5dbc8d2a88b`.
The verified candidate binary SHA-256 is
`33b0cb24099520ebd34d6a497846a7bd0296e3fa0aa3f0d78f0402d83a622e70`.

```sh
SEEKDB_TEST_SOCKET=/absolute/path/to/instance/run/sql.sock \
  python3 unittest/sql/engine/expr/test_fts_tokenization_integration.py
```

## Fixed-workload measurements

Both binaries ran on the same macOS arm64 host, using the same compiler,
4 GiB server memory budget, two network threads and isolated database directory.
No compilation ran alongside measured workloads. Each complete run recreated
the dataset and indexes. Parameters were fixed to ROWS=20000, BATCH=500,
ROUNDS=3000, QUERY_ROUNDS=200, SAMPLES=3, WARMUP=30 and SKIP_LOAD=0.

The table compares the median metric across five baseline runs and three
candidate runs. Each per-run tokenize/query metric remains the original
script's arithmetic mean of its three samples. Positive change means less time;
negative values are retained when calculating the category averages.

| Metric | Baseline | Candidate | Time reduction |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 4.0740 | 3.6580 | 10.21% |
| build_ik_content_sec | 3.0740 | 2.7480 | 10.61% |
| build_beng_en_sec | 1.6400 | 1.6370 | 0.18% |
| tokenize_ik_avg_ms | 0.0552 | 0.0520 | 5.80% |
| tokenize_beng_avg_ms | 0.0312 | 0.0311 | 0.32% |
| query_cn_avg_ms | 0.7150 | 0.7010 | 1.96% |
| query_beng_avg_ms | 1.0534 | 1.0557 | -0.22% |
| query_mixed_avg_ms | 0.8480 | 0.8016 | 5.47% |
| query_limit_avg_ms | 0.9560 | 0.9463 | 1.01% |

Equal weighting of the build, tokenize and query category averages gives a
4.04% local time reduction. Short query/tokenize samples show scheduling noise
on this development machine; small differences should not be treated as
independent improvements. These measurements are not official competition
scores or a prediction of the uniform CI result.

Run the unchanged workload with a fresh output file for each run:

```sh
ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 \
SKIP_LOAD=0 LABEL=candidate OUTPUT=/absolute/path/to/fresh-report.txt \
  bash tools/benchmark/fts_large_bench.sh
```

On macOS, use Bash 4+ and GNU `date` in PATH. The script itself is unchanged.
Local raw reports and binary/source bindings are retained in the coordinating
workspace's `artifacts/fts-combined-comparison-final.json`.
