"""Regenerate tiny Parquet interoperability fixtures; requires pyarrow."""
import datetime as dt
from pathlib import Path
import pyarrow as pa
import pyarrow.parquet as pq

root = Path(__file__).resolve().parent
table = pa.table({
    "id": pa.array([1, 2, 3, 4, 5], type=pa.int64()),
    "name": ["one", None, "three", "one", "\u4e94"],
    "amount": [1.5, None, -2.0, 4.25, 0.0],
    "active": [True, False, None, True, False],
    "day": pa.array([dt.date(2024, 2, 29), None, dt.date(1970, 1, 1),
                     dt.date(1969, 12, 31), dt.date(2000, 1, 1)], type=pa.date32()),
    "stamp": pa.array([dt.datetime(2024, 2, 29, 12, 34, 56, 123456), None,
                       dt.datetime(1970, 1, 1), dt.datetime(1969, 12, 31, 23, 59, 59, 999999),
                       dt.datetime(2000, 1, 1)], type=pa.timestamp("us")),
})
for version in ["1.0", "2.0"]:
    for compression in ["NONE", "snappy"]:
        pq.write_table(table, root / f"flat_{version}_{compression}.parquet",
                       row_group_size=2, compression=compression,
                       data_page_version=version, use_dictionary=True)
