# Transaction TraceLog removal regression

`verify_tracelog_transactions.py` runs against a dedicated seekdb instance and
**drops/recreates the `tracelog_regression` database** during `prepare`.
It requires Python 3 and PyMySQL. The test account is `root`; use
`SEEKDB_TEST_PASSWORD` if the isolated instance has a password.

```sh
python3 verify_tracelog_transactions.py --phase prepare \
  --snapshot /tmp/tx-snapshot.json --output /tmp/tx-prepare.json \
  --revision YOUR_COMMIT_SHA
```

The nine checks cover commit, full rollback, savepoint rollback, a statement
error followed by commit, uncommitted visibility, repeatable read, read
committed, disconnect rollback, and 400 concurrent transactions (300 commits,
100 rollbacks). Assertions compare stored rows and balances, including the
transaction journal.

After gracefully stopping and restarting the same instance, run:

```sh
python3 verify_tracelog_transactions.py --phase verify \
  --snapshot /tmp/tx-snapshot.json --output /tmp/tx-restart.json \
  --revision YOUR_COMMIT_SHA
```

To check crash recovery, start `--phase hold` with the same snapshot and a
separate output file. Wait for that output file to appear: it confirms an
uncommitted update and insert are held. Kill **only the dedicated test server**,
restart it with the same data directory, then run `--phase verify` again. The
committed snapshot must match exactly, and the uncommitted rows must be absent.
The holder exits when its connection is lost; it does not kill the server.

Use `--host` and `--port` to select the dedicated instance. Run identical checks
on the unchanged baseline and candidate. This script checks transaction and
recovery behavior; source inspection and DWARF object sizes establish removal
of the recorder itself. The upstream `repeatable_read_transaction` and
`serializable_transaction` mysqltests and an alternating sysbench run provide
additional checks.
