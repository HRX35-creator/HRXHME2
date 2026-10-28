/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Construct encoded fixed-length blocks and test the real vectorized decoder.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "lib/allocator/page_arena.h"
#include "storage/blocksstable/encoding/ob_string_diff_decoder.h"

using namespace oceanbase::common;
using namespace oceanbase::blocksstable;

struct Segment { bool diff; int size; };

int run_case(const std::vector<Segment> &segments, bool hex, bool with_null, int rows)
{
  constexpr char alphabet[] = "0123456789abcdef";
  int full_len = 0;
  int common_len = 0;
  int diff_len = 0;
  for (const auto &segment : segments) {
    full_len += segment.size;
    (segment.diff ? diff_len : common_len) += segment.size;
  }
  const int hexmap_len = hex ? 16 : 0;
  const int bitmap_len = with_null ? (rows + 7) / 8 : 0;
  const int cell_len = hex ? (diff_len + 1) / 2 : diff_len;
  const int metadata_len = sizeof(ObStringDiffHeader) + segments.size() + hexmap_len + common_len;
  std::vector<char> block(metadata_len + bitmap_len + rows * cell_len + 16, 0);
  auto *header = new (block.data()) ObStringDiffHeader();
  header->string_size_ = full_len;
  header->length_ = cell_len;
  header->hex_char_array_size_ = hexmap_len;
  header->diff_desc_cnt_ = segments.size();
  for (size_t i = 0; i < segments.size(); ++i) {
    header->diff_descs_[i].diff_ = segments[i].diff;
    header->diff_descs_[i].count_ = segments[i].size;
  }
  if (hex) std::memcpy(header->hex_char_array(), alphabet, hexmap_len);
  char *common = header->common_data();
  for (int i = 0; i < common_len; ++i) common[i] = static_cast<char>('A' + i % 26);

  char *col_data = block.data() + metadata_len;
  if (with_null) {
    for (int row = 0; row < rows; ++row) {
      if (row % 5 == 1) ObBitStream::memory_safe_set(
          reinterpret_cast<unsigned char *>(col_data), static_cast<int64_t>(row), int64_t{1}, uint64_t{1});
    }
  }
  std::vector<std::string> expected(rows);
  for (int row = 0; row < rows; ++row) {
    int common_pos = 0;
    int diff_pos = 0;
    unsigned char *cell = reinterpret_cast<unsigned char *>(col_data + bitmap_len + row * cell_len);
    for (const auto &segment : segments) {
      for (int j = 0; j < segment.size; ++j) {
        if (segment.diff) {
          const int value = (row * 7 + diff_pos * 3) % (hex ? 16 : 26);
          expected[row].push_back(hex ? alphabet[value] : static_cast<char>('a' + value));
          if (hex) {
            cell[diff_pos / 2] |= static_cast<unsigned char>(value << ((diff_pos % 2) ? 0 : 4));
          } else {
            cell[diff_pos] = static_cast<unsigned char>('a' + value);
          }
          ++diff_pos;
        } else {
          expected[row].push_back(common[common_pos++]);
        }
      }
    }
  }

  ObMicroBlockHeader micro;
  micro.row_count_ = rows;
  micro.extend_value_bit_ = with_null ? 1 : 0;
  ObColumnHeader column;
  column.offset_ = 0;
  column.length_ = metadata_len;
  column.set_fix_lenght_attr();
  if (with_null) column.set_has_extend_value_attr();
  ObArenaAllocator allocator(ObMemAttr("StringDiffTest"));
  ObColumnDecoderCtx ctx;
  ctx.reset();
  ctx.micro_block_header_ = &micro;
  ctx.col_header_ = &column;
  ctx.allocator_ = &allocator;
  ctx.cache_attributes_[ObColumnHeader::FIX_LENGTH] = true;
  ctx.cache_attributes_[ObColumnHeader::HAS_EXTEND_VALUE] = with_null;
  ObStringDiffDecoder decoder;
  if (decoder.init(micro, column, block.data()) != OB_SUCCESS) return 1;
  std::vector<int32_t> row_ids(rows);
  std::vector<ObDatum> actual(rows);
  for (int i = 0; i < rows; ++i) row_ids[i] = rows - i - 1;
  if (decoder.batch_decode(ctx, nullptr, row_ids.data(), nullptr, rows, actual.data()) != OB_SUCCESS) return 2;
  for (int i = 0; i < rows; ++i) {
    const int source_row = row_ids[i];
    if (with_null && source_row % 5 == 1) {
      if (!actual[i].is_null()) return 3;
    } else if (actual[i].is_null() || actual[i].len_ != static_cast<uint32_t>(full_len)
               || (full_len > 0 && std::memcmp(actual[i].ptr_, expected[source_row].data(), full_len) != 0)) {
      return 4;
    }
  }
  return 0;
}

int main()
{
  const std::vector<std::vector<Segment>> cases = {
      {}, {{false, 12}}, {{true, 12}}, {{false, 3}, {true, 2}, {false, 4}, {true, 3}, {false, 1}},
      {{true, 1}, {false, 7}, {true, 7}, {false, 1}},
      {{false, 100}, {true, 60}, {false, 100}},
  };
  int total = 0;
  for (const auto &segments : cases) {
    for (bool hex : {false, true}) {
      for (bool with_null : {false, true}) {
        for (int rows : {1, 16, 33}) {
          const int ret = run_case(segments, hex, with_null, rows);
          if (ret != 0) {
            std::fprintf(stderr, "decoder mismatch: case=%zu hex=%d null=%d rows=%d ret=%d\n",
                         static_cast<size_t>(&segments - &cases[0]), hex, with_null, rows, ret);
            return ret;
          }
          ++total;
        }
      }
    }
  }
  std::printf("string diff batch decoder: PASS (%d cases)\n", total);
  return 0;
}
