#include "sql/parser/ob_parser.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
using namespace oceanbase::common;
using namespace oceanbase::sql;
int main() {
  const char *queries[] = {
      "SELECT 1",
      "EXPLAIN SELECT 1",
      "DESCRIBE SELECT 1",
      "SELECT * FROM read_csv('/tmp/a.csv')",
      "SELECT a.id FROM read_csv('/tmp/a.csv', header=true, sample_size=-1) AS a",
      "SELECT * FROM read_json('/tmp/a.jsonl') j LEFT JOIN read_ndjson('/tmp/b.jsonl') b ON "
      "j.id=b.id",
      "SELECT * FROM read_parquet('/tmp/a.parquet') p",
      "SELECT file FROM glob('/tmp/*.csv')",
      "DESCRIBE SELECT * FROM read_csv('/tmp/a.csv')",
      "COPY t TO '/tmp/a.csv'",
      "COPY (SELECT id FROM t ORDER BY id) TO '/tmp/a.csv' (header=true, delimiter='|')",
      "COPY t TO '/tmp/out.csv' (FORMAT CSV, HEADER TRUE, NULL 'NULL')",
      "SELECT read_csv FROM t"};
  for (const char *sql : queries) {
    ObArenaAllocator allocator;
    ObParser parser(allocator, 0);
    ParseResult result;
    int ret = parser.parse(ObString::make_string(sql), result);
    std::printf("%d %s %s\n", ret, sql, ret ? result.error_msg_ : "");
    if (ret)
      return 1;
    if (!result.result_tree_)
      return 2;
    parser.free_result(result);
  }
  return 0;
}
