# Asynchronous AI columns

AI columns declare model-generated results in a MySQL table. Business writes
invalidate affected results and enqueue durable work in the same transaction.
Workers call the existing model endpoint APIs after commit, then publish each
row's request group in a separate transaction.

## SQL

Register a completion model and endpoint with `DBMS_AI_SERVICE` first. Given a
model named `text_model`:

```sql
CREATE TABLE articles (
  id BIGINT PRIMARY KEY,
  body LONGTEXT,
  summary VARCHAR(1000) AI COLUMN(AI_COMPLETE('text_model', body))
    WITH(sync_mode='async', max_retries=2)
);

INSERT INTO articles(id, body) VALUES(1, 'Article text');
SHOW AI COLUMNS FROM articles;
SHOW CREATE TABLE articles;

ALTER TABLE articles ADD COLUMN tags JSON
  AI COLUMN(AI_COMPLETE('text_model', CONCAT('Extract tags: ', body)));
ALTER TABLE articles RETRY AI COLUMN tags WHERE status='PENDING';

ALTER TABLE articles RETRY AI COLUMN summary
  WHERE status='FAILED' AND id=1;
SET SESSION ai_column_strict_read=ON;
SELECT summary FROM articles WHERE id=1;
```

`AI_EMBED` definitions support `VECTOR(n)` results. A model dimension and an
explicit vector dimension must agree. Completion results receive a separate
system instruction appropriate to the SQL result type; the user input remains
unchanged. Conversion failures, invalid JSON and invalid vector values follow
the task's bounded retry policy.

`max_retries` counts additional attempts after the first call, defaults to two,
and accepts integers from zero to 100. Only asynchronous generation is
supported. Definitions reference deterministic ordinary input columns, not
other AI columns or generated columns. Business SQL cannot assign AI results.

New AI columns added to populated tables are initially unfinished. Use
`RETRY AI COLUMN` to enqueue generation for existing rows. The retry predicate
may select ordinary fields and the target column's `status` pseudo-column:
`PENDING`, `READY` or `FAILED`. A backtick-quoted `status` refers to an ordinary
column of that name. Retry changes only the specified AI column and rows, can
regenerate READY results, and follows the surrounding transaction's commit or
rollback.

Strict reads check every row of each AI column explicitly referenced by the
statement, using its transaction snapshot. A pending or failed row produces
error 11120 even when the statement's predicate selects another ready row.
SELECT, UPDATE and DELETE use this check; ordinary-column-only statements are
unaffected. Default reads return NULL for pending and failed results.

CREATE TABLE LIKE copies definitions with the new creator as their owner.
Dropping, renaming or changing the type of an AI result or a dependency is
currently rejected. MODIFY/CHANGE cannot convert an ordinary column into an
AI column. Unrelated column alterations, ADD AI COLUMN and DROP TABLE are
supported.

## Transactions and execution

Each definition owns hidden state, request and definition-version columns.
Dependencies and primary-key value changes clear results and replace requests;
unchanged inputs and unrelated updates preserve existing requests. Derived
assignments run after BEFORE trigger changes and before generated vector-index
assignments, so transaction rollback restores both values and indexes.

The task and group system tables hold committed work, leases and completed
HTTP outcomes. Two workers claim groups with a compare-and-set lease epoch.
They renew a 90-second lease before each call and validate the definition's
owner, table privileges and model access. Native HTTP has a 60-second default
timeout and supports shutdown cancellation. Every model attempt is recorded
by the durable task retry state; transport retries are disabled for these
calls. Retry backoff is one, two, four, eight, sixteen, then 32 seconds.

HTTP execution holds no business-row lock or SQL result. Saving an outcome and
publishing results use short transactions that lock the business row before
the group and task. Publication checks the lease owner/epoch, request ID and
definition version. All terminal siblings publish in one transaction: successful
columns become READY and exhausted failures become FAILED. Superseded requests
cannot overwrite new data, and deleted rows are not recreated.

Workers wait for schema recovery to finish before inspecting durable work.
Completed sibling outcomes survive restart without another model call. An
interrupted call may run again, including when an upstream service completed
but its response was lost; exactly-once external inference is not promised.
Internal result publication and manual retry do not execute user row triggers.
Ordinary business writes retain their trigger behavior.

This implementation bounds each materialized input and config expression at
1 MiB, and each HTTP response and output at 4 MiB. Oversized values fail without
truncation and follow bounded retries. Workers poll every 100 ms. These limits
and the fixed worker count apply to AI-column work, not ordinary AI-function SQL.

## Validation

Native code commit `7303c2cd7b9e9ad36d33df4eadbb9cb484851170` was built with
LLVM 19 on macOS arm64. Its binary SHA-256 is
`28f60f3adc2e171544b70f0d1797256ca5645098c9bc0db49d73be5cb76773aa`.
The cumulative SQL suite passed 166 cases, including 105 AI-function, 25 File SQL,
three FTS, three ANN and 30 AI-column cases. Test expectations from the original
competition mysqltests are unchanged.

The permanent integration suites live under `unittest/sql/engine/dml/` and use
the shared HTTP mock fixture in `unittest/sql/engine/expr/`. Run a focused suite
against a disposable native instance:

```sh
PYTHONPATH=unittest/sql/engine/expr \
SEEKDB_TEST_SOCKET=/absolute/path/to/instance/run/sql.sock \
SEEKDB_TEST_DATABASE=test \
  python3 unittest/sql/engine/dml/test_ai_column_worker_integration.py
```

The standalone recovery fixture owns a temporary database and native process.
It kills the process during a held HTTP request, restarts it, and checks
completed-outcome reuse, atomic publication and queue cleanup:

```sh
PYTHONPATH=unittest/sql/engine/expr \
SEEKDB_RECOVERY_BINARY=/absolute/path/to/seekdb \
  python3 unittest/sql/engine/dml/test_ai_column_recovery_integration.py
```

The separate upgrade fixture accepts `SEEKDB_PREVIOUS_BINARY` and
`SEEKDB_RECOVERY_BINARY` and checks missing variable defaults while preserving
existing global settings. These tests verify local behavior; they do not claim
official competition scores, gateway performance results or graceful shutdown.
