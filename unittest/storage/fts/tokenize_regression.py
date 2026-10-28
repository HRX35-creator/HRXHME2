#!/usr/bin/env python3
"""Compare complete TOKENIZE/MATCH results with an unmodified seekdb baseline.

Requires PyMySQL and a dedicated running test instance. The bundled fixture was
recorded on b521f88d6. To create a separate reference, use --record on the original
build, then compare on the candidate with the same --reference file. The only
schema created/dropped by this test is fts_tokenize_regression.
"""

import argparse
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import random

import pymysql


def corpus():
    texts = [
        "", " ", "\t\r\n.,!?", "a A the THE with www",
        "Hello HELLO hello world world", "ab abc " + "x" * 84 + " " + "x" * 85,
        "OceanBase是一款非常稳定的数据库，全文索引分词器热路径优化。",
        "中华人民共和国人民大会堂", "南京市长江大桥 研究生命起源",
        "一百二十三公斤 123公斤 3.14 1,234 2026-10-28",
        "foo-bar foo_bar C++ a@b.com #tag 123abc",
        "ＡＢＣ１２３ 全角，标点。 café naïve İSTANBUL Straße",
        "日本語ひらがなカタカナ 한국어 😀𠀀\U0010ffff",
        "\0alpha\0beta\0", "中文 English 123\n数据库 database",
        "数据库 alpha 123公斤。" * 300,
        "alpha " + "A" * 4096 + " 中 " + "z" * 4097 + " beta alpha",
    ]
    rng = random.Random(20261028)
    alphabet = "abcXYZ019 -_.,\t\n中文数据库公斤Ａ１２éß😀あ한"
    texts.extend("".join(rng.choices(alphabet, k=rng.randrange(1, 160)))
                 for _ in range(160))
    # Sweep CJK characters across every built-in dictionary range, including
    # gaps and rare characters. Spaces make each character a separate lookup.
    for start in range(0x3400, 0xA000, 256):
        texts.append(" ".join(chr(code) for code in range(start, start + 256)))
    return texts


def canonical(value):
    # TOKENIZE returns JSON maps/arrays. Preserve array ordering and frequencies.
    if isinstance(value, str):
        try:
            return json.loads(value)
        except ValueError:
            return value
    return value


def capture(connection):
    results = {}
    with connection.cursor() as cur:
        cur.execute("SET ob_query_timeout = 100000000000")
        for parser in ("ik", "beng"):
            for i, text in enumerate(corpus()):
                for mode in (("smart", "max_word") if parser == "ik" else ("default",)):
                    for output in ("default", "all"):
                        properties = [{"output": output}]
                        if parser == "ik":
                            properties.append({"additional_args": [{"ik_mode": mode}]})
                        cur.execute("SELECT tokenize(%s, %s, %s) AS tokens",
                                    (text, parser, json.dumps(properties)))
                        results[f"{parser}/{mode}/{i}/{output}"] = canonical(cur.fetchone()[0])

        # Exercise the general charset fallback and error behavior as well as
        # the UTF-8 ASCII path. Some parser/charset pairs are unsupported.
        for parser in ("ik", "beng"):
            for charset in ("utf8mb4", "utf16", "utf16le", "latin1", "gbk"):
                for text in ("Alpha BETA 123", "中文１２３ 😀 café", "a\0b\tC"):
                    key = f"charset/{parser}/{charset}/{text}"
                    try:
                        cur.execute(f"SELECT tokenize(CONVERT(%s USING {charset}), %s) AS tokens",
                                    (text, parser))
                        results[key] = canonical(cur.fetchone()[0])
                    except pymysql.Error as error:
                        results[key] = {"error": error.args[0]}
            for raw in ("ff", "e4b8", "f0808080", "616c70686120ff2062657461"):
                key = f"invalid/{parser}/{raw}"
                try:
                    cur.execute("SELECT tokenize(CAST(_binary X'" + raw +
                                "' AS CHAR CHARACTER SET utf8mb4), %s) AS tokens", (parser,))
                    results[key] = canonical(cur.fetchone()[0])
                except pymysql.Error as error:
                    results[key] = {"error": error.args[0]}

        # Repeated evaluation in one statement catches accidental buffer reuse.
        cur.execute("SELECT tokenize('alpha beta', 'beng'), "
                    "tokenize('gamma delta', 'beng'), tokenize('alpha beta', 'beng')")
        values = [canonical(v) for v in cur.fetchone()]
        assert values[0] == values[2] and values[0] != values[1], values
        results["expression_lifetime"] = values

        cur.execute("DROP DATABASE IF EXISTS fts_tokenize_regression")
        cur.execute("CREATE DATABASE fts_tokenize_regression")
        cur.execute("USE fts_tokenize_regression")
        try:
            for parser in ("ik", "beng"):
                cur.execute(f"CREATE TABLE docs_{parser} (id INT PRIMARY KEY, body TEXT)")
                rows = [(i, text) for i, text in enumerate(corpus()[:17])]
                cur.executemany(f"INSERT INTO docs_{parser} VALUES (%s, %s)", rows)
                cur.execute(f"ALTER TABLE docs_{parser} ADD FULLTEXT INDEX ft(body) "
                            f"WITH PARSER {parser}")
                for phase in ("built", "updated"):
                    if phase == "updated":
                        cur.execute(f"UPDATE docs_{parser} SET body='database 数据库' WHERE id=4")
                        cur.execute(f"DELETE FROM docs_{parser} WHERE id=5")
                        cur.execute(f"INSERT INTO docs_{parser} VALUES (100, 'alpha 数据库 数据库')")
                    for query in ("数据库", "alpha", "database", "OceanBase 数据库", "missingword"):
                        for mode in ("", " IN BOOLEAN MODE"):
                            sql = (f"SELECT id, MATCH(body) AGAINST(%s{mode}) "
                                   f"FROM docs_{parser} WHERE MATCH(body) AGAINST(%s{mode}) "
                                   "ORDER BY id")
                            cur.execute(sql, (query, query))
                            results[f"match/{parser}/{phase}/{query}/{mode}"] = cur.fetchall()
                            cur.execute(sql + " LIMIT 2", (query, query))
                            results[f"limit/{parser}/{phase}/{query}/{mode}"] = cur.fetchall()
        finally:
            cur.execute("DROP DATABASE fts_tokenize_regression")
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--reference", type=Path,
                        default=Path(__file__).with_name("tokenize_reference.json.gz"))
    parser.add_argument("--record", action="store_true")
    args = parser.parse_args()
    with pymysql.connect(host=args.host, port=args.port, user=args.user,
                         password=os.getenv("MYSQL_PWD", ""), charset="utf8mb4",
                         autocommit=True) as connection:
        actual = capture(connection)
    # Round-trip tuples/numeric values through the same serialization as fixture.
    serialized = json.dumps(actual, ensure_ascii=False, sort_keys=True, default=str)
    actual = json.loads(serialized)
    if args.record:
        if args.reference.suffix == ".gz":
            args.reference.write_bytes(gzip.compress(serialized.encode(), mtime=0))
        else:
            args.reference.write_text(serialized + "\n", encoding="utf-8")
    else:
        reference = args.reference.read_bytes()
        if args.reference.suffix == ".gz":
            reference = gzip.decompress(reference)
        expected = json.loads(reference)
        assert actual.keys() == expected.keys(), "test case set changed"
        mismatches = []
        for key in actual:
            if key.startswith(("match/", "limit/")):
                # Allow libm's last-bit variation across CPU architectures;
                # document IDs and all token frequencies still compare exactly.
                lhs, rhs = actual[key], expected[key]
                equal = len(lhs) == len(rhs) and all(
                    a[0] == b[0] and math.isclose(float(a[1]), float(b[1]),
                                               rel_tol=1e-12, abs_tol=1e-12)
                    for a, b in zip(lhs, rhs))
            else:
                equal = actual[key] == expected[key]
            if not equal:
                mismatches.append(key)
        assert not mismatches, f"changed full results: {mismatches[:20]}"
    print(f"{'Recorded' if args.record else 'Passed'} {len(actual)} full-result checks; "
          f"sha256={hashlib.sha256(serialized.encode()).hexdigest()}")


if __name__ == "__main__":
    main()
