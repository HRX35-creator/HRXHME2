# 全文索引调优：词典范围索引与分词热路径

## 修改范围与瓶颈依据

本次优化面向 IK/BENG 的公共分词链路，不修改词典、分词规则、停用词集合、索引格式、测试数据、SQL、计时逻辑或评分基线。分词同时用于索引构建、`TOKENIZE` 和 MATCH 查询词解析。

对原版 IK TOKENIZE 单独执行 3 秒 `sample` 采样（不计入基准计时），在 `eval_tokenize` 的 653 个采样命中中，首字符范围定位子树命中约 110 次。调用栈显示大量时间消耗在 `ObCharset::strcmp` 的参数校验、字符集查找和间接比较中。原始记录见 [baseline-profile.txt](fts-results/baseline-profile.txt)。

据此优化实际热点，并清理以下重复工作：

| 路径 | 原有工作 | 修改 |
| --- | --- | --- |
| `ObFTRangeDict::find_first_char_range` | 每次查字典都线性扫描范围，对范围首尾反复调用 UTF8MB4_BIN 比较 | 从实际范围预计算无符号字节序比较键；验证范围有序且不重叠后使用二分查找，否则保持原扫描逻辑 |
| `ObFTCharUtil::do_classify` | 对同一字节序列依次执行字母、数字、中文、其他 CJK 等判断，每项重新解码 Unicode | 解码一次，仍按相同优先级判断相同 Unicode 区间 |
| `ObIKFTParser::process_one_char` | 批处理循环已取得字符、长度和类型，四个 processor 的 `process` 再次读取这些值 | 按原顺序调用 `do_process`，直接传入已验证的字符信息 |
| `ObTextWhitespaceTokenizer::get_next` | UTF-8 英文文本逐字符调用字符集的字符长度函数 | UTF-8 ASCII 字节直接返回长度 1；其他字节和其他字符集保留原解码路径 |
| `ObBasicEnglishNormalizer::get_next` | 每个词重置 arena，再申请小写转换空间 | 同一文档内复用 scratch buffer，容量不足时才重新申请；继续调用原字符集的 `casedn` |

字符解码、processor 分派和英文缓冲区等改动还经过静态调用链检查。单独清理这些开销时未观察到可靠的综合收益，因此性能结论以包含词典范围索引的最终版本为准，不能从减少的函数调用次数推算加速比例。

## 语义与资源生命周期

- 范围比较键按无符号字节的大端顺序编码，并保留长度低位，保持 UTF8MB4_BIN 的字节序与前缀/NUL 比较行为。新字段只属于运行时 `ObFTRange`，不改变 DAT 数据。无序、重叠或超长输入保留原路径。
- IK 保持分类优先级、processor 顺序、分隔符处理、末字符处理和 smart/max_word 仲裁逻辑。
- BENG 保持原有分隔符、首尾非字母数字裁剪、大小写转换、最短/最长词过滤及停用词过滤。
- 小写转换沿用字符集的 expansion multiplier 和转换函数，单倍长度的转换仍在复制后原地执行。
- scratch buffer 的有效期到下一次 `get_next`；原实现同样会在下一次调用时 reuse arena。`ObBEngFTParser` 和 grouping normalizer 仍在推进流前复制需要保存的词。
- `reuse` / `reset` 同时清空 buffer 指针和容量；扩容时不保留旧 buffer 的引用。没有新增全局可变状态。

## 正确性回归

`unittest/storage/fts/tokenize_regression.py` 在专用实例上比较原版和候选版的完整结果：

- IK smart/max_word、BENG，两种 JSON 输出形式；
- 中英文混合、全角字符、日韩文本、补充平面字符、空文本、NUL、标点、重复词、数字与量词；
- 超过 IK 分批阈值的文本、长词及 BENG buffer 扩容前后的词；
- 固定随机种子生成的 160 条混合字符输入，以及 U+3400–U+9FFF 共 27,648 个码位的分块扫描；
- UTF-8、UTF-16、UTF-16LE、latin1、GBK 和非法 UTF-8 字节的输出或错误码；
- 实际建立 IK/BENG 索引后的 MATCH 文档 ID、相关性分数、BOOLEAN MODE、LIMIT，以及 UPDATE/DELETE/INSERT 后的结果。

随代码提供的 `tokenize_reference.json.gz` 来自未修改的 `b521f88d6`，包含 1,829 项完整预期结果；默认直接用该 fixture 校验。MATCH 分数允许跨平台 libm 的 `1e-12` 末位误差，文档 ID、词和词频严格比较。本机前后结果的完整 SHA-256 完全一致：`a92f47b9c2a9cc6818e7ae09508f6a8df6ede7aceea0f74eca76197af7b33f4b`。

测试仅创建和删除 `fts_tokenize_regression` 库，需要使用专用测试实例。依赖 Python 3 和 PyMySQL。在候选版专用实例上可直接运行：

```bash
python3 -m pip install PyMySQL
python3 unittest/storage/fts/tokenize_regression.py --port=2881
```

如需生成同机参考，先在原版实例记录，再切换候选版实例比较，两个阶段使用相同参数：

```bash
python3 unittest/storage/fts/tokenize_regression.py --port=2881 \
  --reference=/tmp/fts-tokenize-reference.json --record
# 将专用实例切换为候选版本后执行：
python3 unittest/storage/fts/tokenize_regression.py --port=2881 \
  --reference=/tmp/fts-tokenize-reference.json
```

## 性能复现

基准使用仓库原有 `tools/benchmark/fts_large_bench.sh`、数据生成器和 SQL；两个版本分别完整建库、导入、构建索引。每轮写入新的报告文件。需要 Bash 4+、支持纳秒时间戳的 `date` 和 MySQL 兼容客户端。

```bash
export MYSQL='mysql -h127.0.0.1 -P2881 -uroot -N -s -A --default-character-set=utf8mb4'
export MYSQL_VERBOSE='mysql -h127.0.0.1 -P2881 -uroot -A --default-character-set=utf8mb4'
LABEL=before ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT=/tmp/fts-before-report.txt \
  bash tools/benchmark/fts_large_bench.sh
# 切换候选版本，保持硬件、资源、编译参数和负载相同，再运行：
LABEL=after ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
  SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT=/tmp/fts-after-report.txt \
  bash tools/benchmark/fts_large_bench.sh
python3 tools/benchmark/fts_large_bench_score.py /tmp/fts-after-report.txt
```

耗时下降率按 `(before-after)/before` 计算。先分别平均构建、分词、查询各类内部指标，再对三类等权平均。单项退化保留负值。不能将全部九项直接平均，也不能把本机对 CI 固定基线的参考分数视为正式比赛成绩。

## 本地验证环境与限制

工作区起点为 `a2f49ab23`。该合并提交存在本次改动之外的 Bazel 构建问题，包括新增 `share/schema/ai_column_definition.h` 未进入依赖清单。隔离验证使用父提交 `b521f88d6`；已核对该提交的 `src/storage/fts` 和 `src/share/text_analysis` 与工作区起点完全一致。

两个隔离版本使用相同的 macOS ARM64 工具链、O2 编译参数、2 GiB 实例内存、1 GiB 初始数据文件和 2 GiB 日志盘。隔离源码还对已有的 macOS `sched_getcpu` 声明问题应用了相同的临时编译补充（`get_cpu_id` 返回 0）。jemalloc 显式配置 Darwin/zone allocator、强制链接 zone 初始化对象，并通过 `JE_MALLOC_CONF=background_thread:false` 禁用本机不支持的后台线程。启动错误诊断和构建 scratch 保留也只用于隔离副本。完整差异见 [validation-platform.patch](fts-results/validation-platform.patch)，未应用到项目源码。实例参数为 `memory_budget=2G`、`datafile_size=1G`、`log_disk_size=2G`，日志级别为 ERROR。因此本地数据用于比较优化前后版本，不能替代当前合并提交在正式 Linux CI 上的构建和评分。


## 最终验证结果

- 隔离基线及最终候选版均完成 O2 构建；最终增量构建记录见 [build.log](fts-results/build.log)。
- 1,829 项完整结果差分通过，默认压缩 fixture 回归命令也通过；见 [correctness.txt](fts-results/correctness.txt)。
- 仓库现有 `fts_index/tokenize_function.test` 与原 `.result` 对照通过（mysqltest 退出码 0）；见 [运行记录](fts-results/tokenize-mysqltest.log)。其中 ERROR 行是测试预期的错误用例。
- 在正式生成的 20,000 文档上，四类 MATCH 的全部 ID/相关性分数摘要前后完全一致；分别核对 8,001、11,000、7,332、9,333 条完整结果，LIMIT 20 结果还验证了数量、唯一性与命中集合成员关系。见 [完整结果摘要](fts-results/large-query-checks.json)。
- 硬件为 macOS ARM64、Mac16,12、24 GiB 内存、10 个物理核；模型与二进制 SHA-256 见 [environment.json](fts-results/environment.json)。

以下是最终代码两轮独立的完整 A/B 测试，下降率保留负值。构建单位为秒，分词和查询单位为毫秒；同一轮中配置及参数完全相同。

| 指标 | 第 1 轮原版 | 第 1 轮优化版 | 下降率 | 第 2 轮原版 | 第 2 轮优化版 | 下降率 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `build_ik_all_sec` | 5.3660 | 4.1560 | 22.55% | 4.4680 | 4.2690 | 4.45% |
| `build_ik_content_sec` | 3.4510 | 3.2570 | 5.62% | 3.4500 | 3.2730 | 5.13% |
| `build_beng_en_sec` | 1.8400 | 1.8430 | -0.16% | 1.8350 | 1.8410 | -0.33% |
| `tokenize_ik_avg_ms` | 0.0790 | 0.0747 | 5.44% | 0.0789 | 0.0742 | 5.96% |
| `tokenize_beng_avg_ms` | 0.0468 | 0.0461 | 1.50% | 0.0455 | 0.0456 | -0.22% |
| `query_cn_avg_ms` | 0.9588 | 0.9730 | -1.48% | 0.9741 | 0.9774 | -0.34% |
| `query_beng_avg_ms` | 1.4228 | 1.4324 | -0.67% | 1.4321 | 1.4339 | -0.13% |
| `query_mixed_avg_ms` | 1.0843 | 1.0916 | -0.67% | 1.0899 | 1.0931 | -0.29% |
| `query_limit_avg_ms` | 1.2046 | 1.2125 | -0.66% | 1.2232 | 1.2084 | 1.21% |

| 等权类别 | 第 1 轮下降率 | 第 2 轮下降率 |
| --- | ---: | ---: |
| 构建 | 9.34% | 3.09% |
| 分词 | 3.47% | 2.87% |
| 查询 | -0.87% | 0.11% |
| 综合 | 3.98% | 2.02% |

IK 分词两轮分别下降 5.44% 和 5.96%，正文 IK 索引构建下降 5.62% 和 5.13%。第一轮双列 IK 构建原版耗时明显高于其他原版测试，因此不能把该轮的 22.55% 当成稳定收益。BENG 和查询没有观察到稳定提升；第二轮的类别等权综合下降为 2.02%。本地相对提升尚未达到耗时减半的满分目标。

原始报告：[第 1 轮原版](fts-results/before-round1.txt)、[第 1 轮优化版](fts-results/after-round1.txt)、[第 2 轮原版](fts-results/before-round2.txt)、[第 2 轮优化版](fts-results/after-round2.txt)；机器可读计算结果见 [comparison.json](fts-results/comparison.json)。未减去 SELECT 1 耗时，未剔除样本，未改变计分配置。

原有评分脚本对本机最终报告给出 [100/100 的 CI 固定基线参考分](fts-results/ci-reference-score.txt)，但对本机未优化原版也给出 100/100，因此这个数字不能归因于本次优化。正式比赛成绩仍需要在统一 Linux CI 中获得。原始报告的 `git_head` 是运行基准脚本的工作区提交；实际二进制基线来源为上述父提交，由环境记录中的哈希标识。
