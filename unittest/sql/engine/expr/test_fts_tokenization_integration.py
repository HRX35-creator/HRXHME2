"""Semantic references for the IK dictionary-range hot path.

Run against an isolated native instance using SEEKDB_TEST_SOCKET or
SEEKDB_TEST_HOST/SEEKDB_TEST_PORT. References were captured before optimization;
the engine's existing TOKENIZE mysqltest supplies additional fixed expectations.
"""
import json
import os
from pathlib import Path
import unittest

import pymysql

REFERENCE = Path(__file__).with_name('fts_tokenization_reference.json')
TEXTS = [
    None, '', ' ', 'hello world', 'HELLO hello HeLLo WORLD world',
    'the and of database search data', 'a ab abc abcd',
    'one,two;three:four-five_six@example.com / seven\\eight',
    'zero\x00one\tone\ntwo\rthree',
    'OceanBase是一款非常稳定的数据库，支持全文索引和向量检索。',
    '中华人民共和国人民大会堂', '一二三四五 六七八 九十百千万亿',
    '123 456 1.2亿 3,456.78 2026-10-27', 'hello-world all-in-one test@example.com',
    'ＡＢＣ１２３ａｂｃ，全文索引；数据库。', '日本語のテスト カタカナ ひらがな 한국어',
    '数据库😀𠀀向量🚀 café CAFÉ naïve Straße İSTANBUL',
    '\u00a0attached\u00a0 data\u2003search\u3000全文\u200b索引',
    'database database 数据库 数据库 ' * 40,
    # Words adjacent to all six main-dictionary range boundaries (50,000-word
    # target ranges extend to the end of their first-character group).
    '一一列举 一一对应 单鹄寡凫 单齿额定升降能力 卖不出去 卖不掉',
    '应领 应验 底下 底下人',
    '榫子 榫眼 榱崩栋折 榱栋崩折',
    '粘附分子 粘骨膜分离器 粜风卖雨 粝食粗衣',
    '醉马草 醉鬼 醋劲 醋劲儿',
    '龟龙片甲 龟龙麟凤 龠龡',
    '榫榫眼 榫榬榭榮榯榰榱榲 醉醊醋 abc AZaz09',
]


def cases():
    for index, text in enumerate(TEXTS):
        for parser, mode in [('beng', None), ('ik', 'smart'), ('ik', 'max_word')]:
            yield {'id': f'utf8-{index}-{parser}-{mode or "default"}', 'text': text,
                   'parser': parser, 'mode': mode, 'charset': 'utf8mb4'}
    for charset in ['utf16', 'utf16le']:
        for index in [3, 4, 9, 10, 11, 12, 14, 15, 16, 17]:
            yield {'id': f'{charset}-{index}-ik', 'text': TEXTS[index], 'parser': 'ik',
                   'mode': 'smart', 'charset': charset}


def connect():
    arguments = dict(user=os.getenv('SEEKDB_TEST_USER', 'root'),
                     password=os.getenv('SEEKDB_TEST_PASSWORD', ''), charset='utf8mb4',
                     connect_timeout=5, read_timeout=60, autocommit=True)
    if os.getenv('SEEKDB_TEST_SOCKET'):
        arguments['unix_socket'] = os.environ['SEEKDB_TEST_SOCKET']
    else:
        arguments.update(host=os.getenv('SEEKDB_TEST_HOST', '127.0.0.1'),
                         port=int(os.getenv('SEEKDB_TEST_PORT', '2881')))
    return pymysql.connect(**arguments)


def canonical(value):
    if isinstance(value, list):
        return sorted([canonical(item) for item in value], key=lambda item: json.dumps(item, sort_keys=True))
    if isinstance(value, dict):
        return {key: canonical(item) for key, item in value.items()}
    return value


def evaluate(cursor, case):
    # Only this fixed, internal charset list is interpolated; document inputs
    # and parser properties are parameters, including embedded NUL bytes.
    assert case['charset'] in ('utf8mb4', 'utf16', 'utf16le')
    properties = [{'output': 'all'}]
    if case['mode']:
        properties.append({'additional_args': [{'ik_mode': case['mode']}]})
    try:
        cursor.execute('SELECT TOKENIZE(CONVERT(%s USING ' + case['charset'] + '),%s,%s)',
                       (case['text'], case['parser'], json.dumps(properties)))
        value = cursor.fetchone()[0]
        return {'value': canonical(json.loads(value)) if value is not None else None}
    except pymysql.MySQLError as error:
        return {'error_code': error.args[0]}


class FTSTokenizationIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.connection = connect()
        cls.reference = json.loads(REFERENCE.read_text())

    @classmethod
    def tearDownClass(cls):
        cls.connection.close()

    def test_ik_and_beng_preserve_tokens_frequencies_and_encoding_errors(self):
        with self.connection.cursor() as cursor:
            for case in cases():
                with self.subTest(case=case['id']):
                    self.assertEqual(self.reference['results'][case['id']], evaluate(cursor, case))

    def test_beng_casefold_and_duplicate_frequency(self):
        with self.connection.cursor() as cursor:
            actual = evaluate(cursor, {'text': 'HELLO hello HeLLo WORLD world',
                              'parser': 'beng', 'mode': None, 'charset': 'utf8mb4'})
            self.assertEqual({'value': {'doc_len': 5, 'tokens': [{'hello': 3}, {'world': 2}]}}, actual)


if __name__ == '__main__':
    unittest.main()
