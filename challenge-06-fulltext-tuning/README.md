# 赛题六 全文索引调优

## 结论摘要

本目录记录对 seekdb 内置 IK 分词器热路径的优化：**IK 词典改为进程级共享**，不再为每个文档重新加载词典缓存并重建范围词典；同时把字符分类中的 Unicode 解码从 5 次降为 1 次。

- 正确性：10 条 `TOKENIZE` 语句输出与优化前**完全一致**（[evidence/tokenize-before.txt](evidence/tokenize-before.txt) vs [evidence/tokenize-after.txt](evidence/tokenize-after.txt)），`MATCH` 命中行数不变（8001 / 11000 / 7332 / 20）；新增回归脚本 [tools/benchmark/fts_shared_dict_regression.sh](../../tools/benchmark/fts_shared_dict_regression.sh) 通过。
- 性能：单文档词典初始化从 **17–20 µs 降到 4–7 µs**（同进程计数器实测），`build_ik_content` 两次对照分别 **−5.7% / −2.6%**；`build_ik_all`、`tokenize_ik` 的变化处于本机运行间噪声范围内（见下文）。
- 诚实说明：本机同一份评测脚本显示，**IK 分词本身只占索引构建耗时的约 14%**（≈71 µs/文档，而整行构建约 523 µs/文档），其余是每文档约 40 条倒排索引行的写入（约 11 µs/行）。因此即使把分词开销降到 0，该指标也无法达到题面「整体降低 50%」的目标；本提交只做了可验证、可回归、不改变分词规则的部分。

## 一、瓶颈诊断（实测）

测量条件：Windows x64 本机，`tools/benchmark/fts_large_bench.sh`（ROWS=20000、ROUNDS=3000、QUERY_ROUNDS=200），同一实例、同一数据目录，改动前后各自重新编译并重启。

### 1.1 索引构建耗时构成

用临时计数器（提交前已移除）分别统计 `ObFTParseHelper::segment`、`ObIKFTParser::init_dict`、逐字符循环与 `ObIKArbitrator`：

| 项目 | 每文档耗时 |
| --- | --- |
| 整行 FTS 索引构建（content 单列，benchmark 文本） | ≈ 523 µs |
| 其中 `ObFTParseHelper::segment`（全部分词工作） | ≈ 71 µs |
| ├ 词典初始化 `init_dict`（改动前） | 17–20 µs |
| ├ 逐字符处理（分类 + 词典匹配 + 4 个处理器） | ≈ 29 µs |
| └ `ObIKArbitrator`（链构建/择优） | ≈ 23 µs |
| 其余（生成并写入约 40 条倒排索引行） | ≈ 452 µs |

对照实验（同一张 2 万行表）：普通二级索引 `ADD INDEX idx_cat(category)` 0.461 s，`ADD INDEX idx_title(title)` 0.564 s，而 `ADD FULLTEXT INDEX ... WITH PARSER ik` 为 10.46 s。即全文索引构建的绝对大头是「每个词一条索引行」的 DML 写入，不是分词算法。

### 1.2 词典初始化为什么可以省掉

改动前 `ObIKFTParser::init_dict` 对每个文档、每个索引列都执行：

1. `ObFTDictHub::load_cache` × 3（main / quantifier / stopword）：加桶锁、拷贝 2 KB 的 `ObFTDictInfo`、按 range 逐个 `fetch_info_for_dict` 分配句柄并做 kvcache `get_dict`；
2. `build_dict_from_cache` × 3：为每个字典新建 `ObFTRangeDict`，并按 range 新建 `ObFTCacheDict`。

这些对象的生命周期原本就是「一个文档」，与文档内容无关。而词典内容在实例生命周期内不可变（`should_read_newest_table()` 恒为 false），因此可以安全共享。

### 1.3 TOKENIZE 链路

`SELECT TOKENIZE(text,'ik')` 稳态每语句约 0.53 ms，其中 `select1` 基线约 0.20 ms；表达式内部：`parse_param` ≈ 28 µs、`ObFTParseHelper::segment` ≈ 120 µs（词循环 ≈ 108 µs）、JSON 输出 ≈ 2 µs，其余为协议与表达式框架开销。词典初始化在这条链路上同样是每语句一次，已被本次改动消除。

## 二、改动内容

1. **`src/storage/fts/dict/ob_ft_dict_hub.{h,cpp}`：词典进程级共享**
   - 新增 `ObFTDictHub::get_shared_dict(desc, dict)`：首次使用时在桶写锁内构建（`load_cache` + 必要的 `build_cache` + `ObFTRangeDict::init`），之后所有分词器实例共享同一 `ObIFTDict`；
   - 共享词典的 kvcache range 句柄由 hub 持有并常驻 pin，`ObFTRangeDict` 只读访问（`match` 为 const），因此在多线程下安全；
   - 因维护顺序约束（`deinit_fts()` 在 `ObDictCache`/`ObKVGlobalCache` 销毁之后执行），`destroy()` 只析构词典对象、不释放 kvcache 句柄；
   - 原 `load_cache` / `build_cache` 行为保持不变，内部拆出不加锁的 `*_inner` 版本供共享构建复用。
2. **`src/storage/fts/ob_ik_ft_parser.{h,cpp}`**
   - `init_dict` 改为 3 次 `get_shared_dict`；删除每解析器私有的 `ObFTCacheRangeContainer`、`build_dict_from_cache`、`init_single_dict`；
   - `reset()` 不再析构词典（词典归 hub 所有）。
3. **`src/storage/fts/ik/ob_ik_char_util.h`**
   - `do_classify` 只解码一次 Unicode，再复用同一个码点做 alpha/arabic/chinese/other_cjk 判断，判断顺序与语义不变。
4. **`tools/benchmark/fts_shared_dict_regression.sh`（回归测试，16 项检查）**
   - 同一文本重复 `TOKENIZE` 结果必须一致（按 token 集合比较，哈希表顺序不计）；
   - 新建第二个 IK 全文索引后结果仍一致（验证多解析器共享同一份词典）；
   - `ik_mode='max_word'`（`TOKENIZE` 第三参 `[{"additional_args":[{"ik_mode":"max_word"}]}]`）仍产出与 smart 不同的切分，且不影响 smart 结果；
   - 文档的每个词都能被 `MATCH` 命中（验证建索引与查询使用同一份词典）；
   - 无关词不应命中；
   - 4 个并发会话各 20 次 `TOKENIZE` 全部一致（验证共享词典只读并发安全）；
   - `ALTER SYSTEM FLUSH KVCACHE cache='dict_cache'` 后行为不变，flush 后新建表仍可正常建索引并 `MATCH`；
   - 50 行文档全部命中，且长文本仍保留 CJK 词。

## 三、正确性验证

| 验证项 | 结果 | 证据 |
| --- | --- | --- |
| 10 条 `TOKENIZE` 语句（中英混排、数字、全角、空串、单字符） | 与优化前逐字一致 | [evidence/tokenize-before.txt](evidence/tokenize-before.txt) / [evidence/tokenize-after.txt](evidence/tokenize-after.txt) |
| `MATCH` 命中数（query_cn / query_beng / query_mixed / query_limit） | 8001 / 11000 / 7332 / 20，与优化前相同 | [evidence/bench-before.txt](evidence/bench-before.txt) / [evidence/bench-after.txt](evidence/bench-after.txt) |
| 新增回归脚本 | 连续两次 `regression: PASSED`（各 16 项 ok，exit=0） | [evidence/regression-test.txt](evidence/regression-test.txt) |

> `TOKENIZE` 返回的 JSON 数组顺序来自哈希表迭代，比较时对数组元素排序后做逐行 diff，其余字符逐字比较。

## 四、性能对比

同一实例、同一数据目录、`LABEL` 不同的两次完整评测（含重建 2 万行数据与三个全文索引）：

| 指标 | 优化前 | 优化后 | 变化 |
| --- | --- | --- | --- |
| build_ik_all_sec | 14.808 | 14.684 | −0.8% |
| build_ik_content_sec | 11.974 | 11.296 | **−5.7%** |
| build_beng_en_sec（未改动路径） | 6.124 | 6.163 | +0.6% |
| tokenize_ik_avg_ms | 0.5716 | 0.5723 | +0.1% |
| query_cn_avg_ms | 3.9200 | 3.9319 | +0.3% |

为排除机器状态漂移，随后又在同一机器状态下做了第二组背靠背对照（先跑基线二进制，再跑优化后二进制，其余条件相同）：

| 指标 | 基线 | 优化后 | 变化 |
| --- | --- | --- | --- |
| build_ik_all_sec | 14.293 | 14.520 | +1.6% |
| build_ik_content_sec | 11.484 | 11.180 | −2.6% |
| build_beng_en_sec（未改动路径） | 5.829 | 5.916 | +1.5% |
| tokenize_ik_avg_ms | 0.5337 | 0.4988 | −6.5% |
| tokenize_beng_avg_ms（未改动路径） | 0.3746 | 0.3525 | −5.9% |
| query_cn_avg_ms | 3.5524 | 3.6452 | +2.6% |
| query_limit_avg_ms | 5.2253 | 4.8242 | −7.7% |

结论必须如实陈述：**未改动路径（beng、query_limit）同样出现了 ±6% 的波动**，说明本机单次运行的噪声与本次改动的量级相当。可以稳定复现的收益是结构性的：单文档词典初始化 17–20 µs → 4–7 µs（同进程计数器，与机器噪声无关），对应 `build_ik_content` 上 2.6%–5.7% 的下降；`build_ik_all` / `tokenize_ik` 的单次差异不能作为确定收益。

未采用 `fts_large_bench_score.py` 打分：该脚本以 CI 基线（build_ik_all 35.28 s 等）为分母，而本机比 CI 快约 2.4 倍，直接套用会把机器差异算成「性能提升」。本目录只报告同机前后对照。

### 已试验但未纳入的改动

还试验了「词典范围查找改为二分 + 按字节比较」（`ObFTRangeDict::find_first_char_range`：range 已按起始字符有序，二分后比较一次上界；UTF8MB4_BIN 下按字节比较与 `ObCharset::strcmp` 等价，非有序或非 binary 排序规则时回退原逻辑）。改动后 `TOKENIZE` 输出与回归脚本均通过，但在本机无法证明收益：同一二进制在测量期间单条 `SELECT 1` 基线本身从 0.20 ms 漂移到 0.24 ms，整体吞吐波动 ±60%，小于该波动的差异不可信。为避免在 PR 中加入无法验证的改动，已回退，只保留可量化的词典共享与单次解码。

## 五、复现命令

```bash
# 1. 构建并启动 seekdb（Windows 本机示例）
cd seekdb/build_release && ../deps/3rd/tools/ninja/ninja.exe seekdb.exe
seekdb.exe --nodaemon --base-dir=<runtime-dir> --port=12881 \
  --parameter memory_budget=2G --parameter cpu_count=4 \
  --parameter datafile_size=1G --parameter datafile_maxsize=4G \
  --parameter log_disk_size=2G --parameter enable_rpc_service=false --log-level=WARN

# 2. 性能评测（MYSQL 指向 mysql 客户端，PATH 中需有 python3）
MYSQL=./tools/fts_mysql.sh MYSQL_VERBOSE=./tools/fts_mysql_verbose.sh \
LABEL=after ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 \
bash tools/benchmark/fts_large_bench.sh > challenge-06-fulltext-tuning/evidence/bench-after.txt

# 3. 回归测试
MYSQL=./tools/fts_mysql.sh bash tools/benchmark/fts_shared_dict_regression.sh

# 4. TOKENIZE 输出快照比对（对输出数组排序后 diff 两次运行结果）
```

## 六、提交与 PR

- 代码提交：本仓库 `2026-competition` 分支（改动见第二节文件清单）。
- Pull Request：[oceanbase/seekdb#2098](https://github.com/oceanbase/seekdb/pull/2098)，`666G-Creator:agent/fts-shared-dict` → `oceanbase:2026-competition`，15 个文件（元数据见 [evidence/pr.txt](evidence/pr.txt)，提交数与 diff 规模以 PR 页面为准）。
