#!/usr/bin/env python3
"""Compare long-ASCII and mixed-character TOKENIZE results across binaries."""
import argparse
import json
import os
import random
from pathlib import Path
import subprocess

CLIENT = os.environ.get('MYSQL_CLIENT', str(Path(__file__).resolve().parents[2] / 'deps/3rd/u01/obclient/bin/obclient'))


def canonical(value):
    if isinstance(value, dict):
        return {k: canonical(v) for k, v in sorted(value.items())}
    if isinstance(value, list):
        return sorted((canonical(v) for v in value), key=lambda x: json.dumps(x, sort_keys=True))
    return value


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['record', 'verify'])
    parser.add_argument('path', type=Path)
    args = parser.parse_args()
    rng = random.Random(20261028)
    words = ['a', 'about', 'abcdefg', 'abcdefgh', 'ABCDEFGHI', 'performance',
             'abcdefghijklmnopqrstuvwx', 'x' * 1000, 'the ', 'performance\x00',
             'performaNCE', 'perforMANCEé', 'Åbcdefgh', 'performance\tAND',
             'ＡＢＣＤＥＦＧＨ', '数据库abcdefghi', 'hello-world-long']
    alphabet = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'
    for _ in range(500):
        word = ''.join(rng.choice(alphabet) for _ in range(rng.randrange(8, 50)))
        words.append(word if rng.randrange(3) else word + rng.choice([' ', '\t', '\x00', 'é', '-']))
    labels, queries = [], []
    for name in ('ik', 'beng', 'space'):
        for word in words:
            labels.append([name, word])
            queries.append("SELECT TOKENIZE(CONVERT(UNHEX('" + word.encode().hex() +
                           "') USING utf8mb4), '" + name + "', '[{\"output\":\"all\"}]');")
    result = subprocess.run([CLIENT, '-h127.0.0.1', '-P' + os.environ.get('SEEKDB_TEST_PORT', '10105'), '-uroot', '-N', '-s', '--raw',
                             '--default-character-set=utf8mb4'],
                            input='SET NAMES utf8mb4;\n' + '\n'.join(queries),
                            capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise RuntimeError(result.stderr)
    rows = result.stdout.splitlines()
    if len(rows) != len(labels):
        raise AssertionError((len(rows), len(labels)))
    actual = [[label, canonical(json.loads(row))] for label, row in zip(labels, rows)]
    if args.mode == 'record':
        args.path.write_text(json.dumps(actual, ensure_ascii=False))
    else:
        expected = json.loads(args.path.read_text())
        assert len(expected) == len(actual)
        for prior, current in zip(expected, actual):
            if prior != current:
                raise AssertionError({'baseline':prior, 'candidate':current})
    print(f'{args.mode}: {len(actual)} long and mixed-character TOKENIZE results checked')


if __name__ == '__main__':
    main()
