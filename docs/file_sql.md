# File SQL

File SQL is disabled by default. Configure `secure_file_priv` to a local directory
at server startup, then enable it for the session:

```sql
SET enable_file_sql = 1;
SELECT name, SUM(amount) AS total
FROM read_csv('/allowed/orders.csv')
WHERE amount > 0 GROUP BY name HAVING SUM(amount) > 10 ORDER BY total DESC;

SELECT a.id, b.city
FROM read_csv('/allowed/people.csv') a
LEFT JOIN read_ndjson('/allowed/cities.jsonl') b ON a.id = b.id;

SELECT id, stamp FROM read_parquet('/allowed/events.parquet');
DESCRIBE SELECT * FROM read_csv('/allowed/orders.csv');
SELECT file FROM glob('/allowed/*.csv');

COPY (SELECT id, name FROM read_csv('/allowed/people.csv') ORDER BY id)
TO '/allowed/export.csv' (FORMAT CSV, HEADER TRUE, DELIMITER '|', NULL 'NULL');
SELECT * FROM read_csv('/allowed/export.csv', delimiter='|', nullstr='NULL');
COPY existing_table TO '/allowed/table.csv';
```

Paths and options must be literals. `read_json` and `read_ndjson` are synonyms
for newline-delimited flat JSON objects. Each function accepts one file, not a
list or pattern. `glob` only discovers files: it returns one `file` column,
sorted by path, and supports `*` and `?` in the filename component. Directory
components must be literal. There is no remote access or automatic file merging.

CSV options use `name=value`: `header` (default true), `delimiter`/`delim`
(default comma), `quote` (default double quote), `escape` (default double quote),
`nullstr`/`null` (default empty text), and `sample_size` (only -1). Characters
must be single-byte non-newline ASCII. With equal quote/escape characters,
doubled quotes escape a quote; with separate characters, escape quotes and the
escape character. Unquoted NULL text produces SQL NULL; quoted NULL text is a
value. UTF-8 BOM, LF/CRLF, and embedded quoted newlines are supported. Headerless
columns are named `column1`, `column2`, etc. Duplicate or empty column names,
invalid UTF-8, malformed quoting, and inconsistent field counts are errors.

CSV and JSONL inference reads the entire input without retaining its rows.
Column order is header order or JSON first-appearance order. Missing JSON fields
are NULL; nested objects/arrays and duplicate keys are rejected. Types are NULL,
BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE and DATETIME. NULL widens to the other
type, integer plus floating point widens to DOUBLE, and DATE plus DATETIME
widens to DATETIME. Other conflicts become VARCHAR. Out-of-range integers remain
exact VARCHAR values; non-finite floating-point text does not become DOUBLE.
Dates use the Gregorian calendar and timestamps allow up to six fractional
seconds. JSON quoted numbers remain strings; ISO date/timestamp strings are
recognized. All-NULL columns retain the SQL NULL type.

Parquet reads the footer schema and streams pages across row groups. It supports
flat required/optional BOOLEAN, INT32/INT64, FLOAT/DOUBLE, UTF-8 BYTE_ARRAY,
DATE and integer TIMESTAMP columns, uncompressed/Snappy pages, page versions
1/2, PLAIN and dictionary encodings, and RLE/bit-packed definition levels and
Boolean values. Unused columns are not decoded. Nested/repeated schemas,
external column files, unsigned integers, decimals, INT96, other codecs and
other value encodings are rejected explicitly. No query engine or Arrow runtime
is linked; the fixture generator optionally uses PyArrow only to write tests.

The native parser/resolver binds inferred columns into a local JSON_TABLE
operator. It converts one bounded row at a time; native expressions, filters,
aggregates, sorts and joins execute the query. File plans are excluded from the
plan cache. The execution operator rechecks the feature switch, path permission
and file identity after binding. Re-scans reopen the logical stream at the
beginning. EXPLAIN shows the native JSON_TABLE scan and relational operators.

Input handles are read-only. Handle-derived canonical paths must lie beneath
`secure_file_priv`; an empty or NULL setting fails closed. Windows input handles
exclude writers/deletion while scanning. Linux checks inode, size, modification
and change times, including path replacement. Resource bounds include 1024
columns, 8 MiB input rows, bounded Parquet metadata/pages and decoded dictionary
values, and at most 10,000 glob results/1 MiB of path text. Resource-limit failures
are errors, never silently truncated output. A batch holds at most 1024 rows
and stops at the bounded byte budget.

COPY accepts table names or parenthesized SELECT queries. Options accept either
`name=value` or conventional `HEADER TRUE, DELIMITER '|', QUOTE '"', ESCAPE '"',
NULL 'NULL', FORMAT CSV`. It uses the same feature switch and path restriction.
The destination must not exist. Output is written to an exclusively created
private temporary file in the destination directory, then published without
replacement only after query success. Normal error/cancellation cleanup removes
the temporary file. A process crash can leave a temporary file, but never
publishes a partial destination. All non-NULL fields are quoted, so NULL and its
literal text round-trip distinctly.

## Tests

- `//unittest/sql/engine/basic:test_file_scan_utils`: standalone readers, types,
  malformed input, limits, mutation/locking, projection, rescan, batch and export.
- `//unittest/sql/parser:test_file_sql_parser`: native parser syntax regression
  (Linux Bazel test linked to the existing in-process server library).
- `unittest/sql/engine/basic/test_file_sql_integration.py`: executable SQL
  assertions against a local server, including native plans, joins, security and
  export failure cleanup. Requires `pymysql`; set `FILE_SQL_TEST_DIR` to the exact
  configured `secure_file_priv`, and optionally `FILE_SQL_PORT` (default 2881)
  and `FILE_SQL_PASSWORD`. The test creates/removes its own directory and database.
- `tools/deploy/mysql_test/test_suite/file_sql/`: mysqltest fixtures and basic
  suite. The server must allow `MYSQLTEST_VARDIR` in `secure_file_priv`.

Windows compilation uses the two toolsets described in AGENTS.md. A compiled
unit is not a substitute for executing the reader and SQL tests.

Validation in this checkout: reader/writer tests and 13 native parser cases were
executed on Windows; seven integration groups passed against a locally linked
seekdb server. The affected Bazel units compile with VC 14.38, and the basic
execution unity unit also compiles directly with VC 14.44. The server links with
VC 14.44 (VC 14.38 cannot link the newer prebuilt dependency STL symbols).
The mysqltest SQL was executed through PyMySQL to produce its result fixture;
the mysqltest runner itself and Linux-specific tests were not run here.
