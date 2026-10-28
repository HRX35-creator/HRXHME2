#!/usr/bin/env bash
# Regression check for the shared IK dictionary cache.
#
# The IK tokenizer reuses the dictionaries owned by ObFTDictHub instead of
# rebuilding a range dict for every document. This script verifies that
# tokenization and fulltext matching still behave identically when several
# IK parsers share the dictionaries, including after the dict cache is flushed.
#
# Requirements: a reachable seekdb instance and a mysql client.
# Usage:
#   MYSQL=/path/to/mysql fts_shared_dict_regression.sh
set -u

MYSQL="${MYSQL:-mysql}"
DB="${DB:-fts_shared_dict_reg}"
FAILED=0

fail() {
  echo "FAIL: $*" >&2
  FAILED=1
}

sql() {
  ${MYSQL} -N -s -e "SET NAMES utf8mb4; $1"
}

expect_eq() {
  local what="$1"
  local expected="$2"
  local actual="$3"
  if [[ "${actual}" != "${expected}" ]]; then
    fail "${what}: expected '${expected}', got '${actual}'"
  else
    echo "ok: ${what}"
  fi
}

# TOKENIZE returns a JSON array; the element order is produced by a hash map and
# is not part of the contract, so compare the token set instead of the raw text.
norm_tokens() {
  local s="${1%$'\r'}"
  if [[ "${s}" == \[*\] ]]; then
    printf '%s' "${s}" | tr -d '[]\r' | tr ',' '\n' \
      | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e '/^$/d' \
      | LC_ALL=C sort | paste -sd',' -
  else
    printf '%s' "${s}"
  fi
}

expect_tokens_eq() {
  local what="$1"
  local expected="$2"
  local actual="$3"
  if [[ "$(norm_tokens "${actual}")" != "$(norm_tokens "${expected}")" ]]; then
    fail "${what}: token set changed (expected '${expected}', got '${actual}')"
  else
    echo "ok: ${what}"
  fi
}

TEXT1="数据库分词器性能优化测试文本内容检索延迟度量指标"
TEXT2="OceanBase是一款非常稳定的数据库，全文索引的分词器热路径优化包括解析器实例复用、内存池化数据结构。"
TEXT3="中华人民共和国人民大会堂是著名地标，用于测试分词与全文索引构建。"

${MYSQL} -e "DROP DATABASE IF EXISTS ${DB}; CREATE DATABASE ${DB};" >/dev/null

sql "CREATE TABLE ${DB}.docs(id INT PRIMARY KEY, title VARCHAR(256), content TEXT) DEFAULT CHARSET=utf8mb4;
     CREATE FULLTEXT INDEX ft_title ON ${DB}.docs(title) WITH PARSER ik;
     CREATE FULLTEXT INDEX ft_content ON ${DB}.docs(content) WITH PARSER ik;" >/dev/null

sql "INSERT INTO ${DB}.docs VALUES
  (1, '${TEXT1}', '${TEXT1}'),
  (2, '${TEXT2}', '${TEXT2}'),
  (3, '${TEXT3}', '${TEXT3}');" >/dev/null

# 1. Repeated tokenization must be identical (the dictionary is shared and must
#    not change between calls).
tok_a=$(sql "SELECT TOKENIZE('${TEXT1}', 'ik');")
tok_b=$(sql "SELECT TOKENIZE('${TEXT1}', 'ik');")
tok_c=$(sql "SELECT TOKENIZE('${TEXT1}', 'ik');")
expect_tokens_eq "tokenize output is stable" "${tok_a}" "${tok_b}"
expect_tokens_eq "tokenize output is stable (3rd call)" "${tok_a}" "${tok_c}"

# 2. Tokenization must not depend on how many parsers already use the dict:
#    build more indexes and tokenize again.
sql "CREATE TABLE ${DB}.docs2(id INT PRIMARY KEY, content TEXT) DEFAULT CHARSET=utf8mb4;
     CREATE FULLTEXT INDEX ft2 ON ${DB}.docs2(content) WITH PARSER ik;" >/dev/null
sql "INSERT INTO ${DB}.docs2 VALUES (1, '${TEXT1}'), (2, '${TEXT2}');" >/dev/null
tok_d=$(sql "SELECT TOKENIZE('${TEXT1}', 'ik');")
expect_tokens_eq "tokenize output is stable across parsers" "${tok_a}" "${tok_d}"

# 3. A second IK index with a different mode must keep its own segmentation
#    settings while sharing the dictionary.
sql "CREATE TABLE ${DB}.docs_maxword(id INT PRIMARY KEY, content TEXT) DEFAULT CHARSET=utf8mb4;
     CREATE FULLTEXT INDEX ft3 ON ${DB}.docs_maxword(content) WITH PARSER ik PARSER_PROPERTIES=(ik_mode='max_word');" >/dev/null
sql "INSERT INTO ${DB}.docs_maxword VALUES (1, '${TEXT1}');" >/dev/null
tok_smart=$(sql "SELECT TOKENIZE('${TEXT2}', 'ik');")
# TOKENIZE takes a JSON array whose elements are single-key objects; the parser
# properties (ik_mode) live under additional_args.
tok_maxword=$(sql "SELECT TOKENIZE('${TEXT2}', 'ik', '[{\"additional_args\":[{\"ik_mode\":\"max_word\"}]}]');")
if [[ -z "${tok_maxword}" || "${tok_maxword}" != \[*\] ]]; then
  fail "max_word tokenize returned no JSON array: ${tok_maxword}"
elif [[ "$(norm_tokens "${tok_smart}")" == "$(norm_tokens "${tok_maxword}")" ]]; then
  fail "smart and max_word produced the same tokens, parser properties were ignored"
else
  echo "ok: smart and max_word keep different segmentation"
fi
# the plain smart call must be unaffected by the max_word table above
tok_e=$(sql "SELECT TOKENIZE('${TEXT1}', 'ik');")
expect_tokens_eq "smart tokenization unaffected by max_word index" "${tok_a}" "${tok_e}"

# 4. Every token of a document must be findable through MATCH, for both
#    columns, which proves index build and query use the same dictionary.
for word in 数据库 全文索引 分词器 人民大会堂; do
  cnt=$(sql "SELECT COUNT(*) FROM ${DB}.docs WHERE MATCH(content) AGAINST('${word}');")
  if [[ "${cnt}" -lt 1 ]]; then
    fail "MATCH(content) AGAINST('${word}') returned ${cnt} rows"
  else
    echo "ok: MATCH(content) AGAINST('${word}') -> ${cnt}"
  fi
done

total=$(sql "SELECT COUNT(*) FROM ${DB}.docs WHERE id = 1 AND MATCH(title) AGAINST('数据库');")
expect_eq "MATCH on title column" "1" "${total}"

# 5. A word that is not in the documents must not match.
cnt=$(sql "SELECT COUNT(*) FROM ${DB}.docs WHERE MATCH(content) AGAINST('zzqqxxnotexist');")
expect_eq "unmatched word returns no row" "0" "${cnt}"

# 6. Concurrent sessions share the dictionary without interfering.
CONC_DIR="${TMPDIR:-/tmp}/fts_shared_dict_conc.$$"
rm -rf "${CONC_DIR}"
mkdir -p "${CONC_DIR}"
for idx in 1 2 3 4; do
  ( for i in $(seq 1 20); do sql "SELECT TOKENIZE('${TEXT2}', 'ik');"; done ) \
    > "${CONC_DIR}/out.${idx}" 2> "${CONC_DIR}/err.${idx}" &
done
wait
conc_ref=$(sql "SELECT TOKENIZE('${TEXT2}', 'ik');")
conc_ref_norm=$(norm_tokens "${conc_ref}")
conc_bad=0
for idx in 1 2 3 4; do
  seen=0
  session_ok=1
  while IFS= read -r line; do
    line="${line%$'\r'}"
    # only JSON arrays are TOKENIZE results; client warnings go to err.${idx}
    [[ "${line}" == \[*\] ]] || continue
    seen=$((seen + 1))
    if [[ "$(norm_tokens "${line}")" != "${conc_ref_norm}" ]]; then
      fail "concurrent tokenize output differs in session ${idx}"
      session_ok=0
      conc_bad=1
      break
    fi
  done < "${CONC_DIR}/out.${idx}"
  if [[ "${session_ok}" -eq 1 && "${seen}" -ne 20 ]]; then
    fail "concurrent session ${idx} returned ${seen}/20 tokenize results"
    conc_bad=1
  fi
done
if [[ "${conc_bad}" -eq 0 ]]; then
  echo "ok: 4 concurrent sessions x 20 calls produce identical tokens"
fi
rm -rf "${CONC_DIR}"

# 7. Flush the dict cache: the shared dict is pinned by the parser cache and
#    must keep working, including for newly created tables.
sql "ALTER SYSTEM FLUSH KVCACHE cache='dict_cache';" >/dev/null
tok_f=$(sql "SELECT TOKENIZE('${TEXT1}', 'ik');")
expect_tokens_eq "tokenize output is stable after dict cache flush" "${tok_a}" "${tok_f}"

sql "CREATE TABLE ${DB}.docs3(id INT PRIMARY KEY, content TEXT) DEFAULT CHARSET=utf8mb4;
     CREATE FULLTEXT INDEX ft4 ON ${DB}.docs3(content) WITH PARSER ik;" >/dev/null
sql "INSERT INTO ${DB}.docs3 VALUES (1, '${TEXT2}');" >/dev/null
cnt=$(sql "SELECT COUNT(*) FROM ${DB}.docs3 WHERE MATCH(content) AGAINST('数据库');")
expect_eq "MATCH works on a table created after the flush" "1" "${cnt}"

# 8. A larger document set keeps per-document results consistent with MATCH.
doc_rows=""
for i in $(seq 1 50); do
  doc_rows="${doc_rows}${doc_rows:+,}(${i}, '${TEXT1}第${i}条记录')"
done
sql "CREATE TABLE ${DB}.docs_many(id INT PRIMARY KEY, content TEXT) DEFAULT CHARSET=utf8mb4;
     CREATE FULLTEXT INDEX ft5 ON ${DB}.docs_many(content) WITH PARSER ik;" >/dev/null
sql "INSERT INTO ${DB}.docs_many VALUES ${doc_rows};" >/dev/null
cnt=$(sql "SELECT COUNT(*) FROM ${DB}.docs_many WHERE MATCH(content) AGAINST('数据库');")
expect_eq "all 50 documents are matched" "50" "${cnt}"
tok_g=$(sql "SELECT TOKENIZE('${TEXT1}第7条记录', 'ik');")
if [[ "${tok_g}" != *"数据库"* ]]; then
  fail "tokenize lost the CJK word 数据库: ${tok_g}"
else
  echo "ok: tokenize keeps CJK words in a 50-document table"
fi

if [[ "${FAILED}" -ne 0 ]]; then
  echo "regression: FAILED" >&2
  exit 1
fi
echo "regression: PASSED"
