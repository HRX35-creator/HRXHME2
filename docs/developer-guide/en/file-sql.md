# Native local file SQL

File SQL is disabled by default. On Linux and macOS, enable it for a session and
set `secure_file_priv` to a directory that contains the input and output files:

```sql
SET GLOBAL secure_file_priv = '/absolute/path/to/data';
SET SESSION enable_file_sql = ON;
SELECT id, name FROM read_csv('/absolute/path/to/data/input.csv') ORDER BY id;
SELECT * FROM read_json('/absolute/path/to/data/input.jsonl');
SELECT * FROM read_ndjson('/absolute/path/to/data/input.jsonl');
SELECT COUNT(*) FROM read_parquet('/absolute/path/to/data/input.parquet');
SELECT file FROM glob('/absolute/path/to/data/*.csv') ORDER BY file;
DESCRIBE SELECT * FROM read_csv('/absolute/path/to/data/input.csv');
COPY (SELECT id, name FROM read_csv('/absolute/path/to/data/input.csv'))
  TO '/absolute/path/to/data/output.csv' (FORMAT CSV, HEADER, NULLSTR 'NULL');
```

Filters, expressions, aggregates, sorting, limits and joins use ordinary seekdb
operators. Files are not imported into a temporary SQL table or sent to another
query engine. `DESCRIBE SELECT` returns the inferred output column names, types
and nullability as `column_name`, `column_type` and `null`, followed by nullable
`key`, `default` and `extra` columns. An ordinary `EXPLAIN` still shows the native
execution plan. CSV/JSONL DATETIME output uses the greatest fractional precision
present in that column; whole-second input has no fractional suffix. Parquet
timestamps retain microsecond precision.

CSV options are `header`, `delimiter` (alias `delim`), `quote`, `escape`, and
`nullstr`. Named reader options use `name=value`. Reader headers default to true;
the default NULL spelling is an unquoted empty field. COPY headers default to
false and its option list accepts, for example, `HEADER TRUE`, `DELIMITER ';'`,
`QUOTE '"'`, `ESCAPE '"'`, and `NULLSTR 'NULL'`. Quoted strings equal to the NULL
spelling remain strings on readback. COPY fails if the target exists and removes
unpublished temporary output on failure.

CSV and JSONL infer the schema from all records; only `sample_size=-1` is
supported. JSONL accepts flat objects, takes the union of field names and fills
missing fields with NULL. Nested arrays and objects are rejected. Types are
BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME and NULL. Conflicting types widen
deterministically to VARCHAR; integer text outside signed BIGINT is preserved.

Parquet decoding uses Apache's Rust `parquet` crate, pinned to 60.0.0. Footer
metadata supplies the schema. The operator requests only referenced columns and
streams bounded decode batches across row groups. Flat primitive columns,
nullable values, UTF-8 strings, dates and INT64 timestamps are supported. UINT64
columns use VARCHAR to preserve their complete range. Nested fields, decimals,
INT96 timestamps and non-finite floating point values are rejected.

Inputs must be regular files under the canonical allowed directory. Opened path
components reject symlink substitutions; size, identity and modification/change
timestamps are checked against the bound descriptor during scans and rescans.
NULL or empty `secure_file_priv` denies access. Glob traverses directories without
following links and returns regular files only. The POSIX implementation is
guarded on Windows, where File SQL reports unsupported.

Resource limits are 1,024 columns, 1,024 bytes per column name, 4 MiB per field,
16 MiB per CSV/JSONL record, and 8 MiB of copied strings per scan batch (plus its
last row). Parquet limits its footer and cached page headers to 8 MiB, each page
to 32 MiB, and each row group's decoded pages to 64 MiB and 1,048,576 rows.
Oversized or malformed inputs return an error rather than allocating a complete
file in memory.

Run the SQL regression suite against an isolated instance: it temporarily
changes global `secure_file_priv` and restores it during cleanup.

```sh
python3 -m pip install -r unittest/sql/engine/basic/requirements-file-sql.txt
SEEKDB_TEST_SOCKET=/absolute/path/to/instance/run/sql.sock \
  SEEKDB_TEST_DATABASE=test \
  python3 unittest/sql/engine/basic/test_file_sql_integration.py
cd rust
cargo test -p file-sql
```

PyArrow generates test fixtures only. It is not used by the engine.
