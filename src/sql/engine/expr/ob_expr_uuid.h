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

#ifndef SRC_SQL_ENGINE_EXPR_OB_EXPR_UUID_H_
#define SRC_SQL_ENGINE_EXPR_OB_EXPR_UUID_H_

#include "sql/engine/expr/ob_expr_operator.h"

namespace oceanbase
{
namespace sql
{

class ObBigEndian
{
public:
  static int put_uint16(unsigned char *b, uint16_t v);
  static int put_uint32(unsigned char *b, uint32_t v);
private :
  //disallow copy
  DISALLOW_COPY_AND_ASSIGN(ObBigEndian);
};

struct ObUUIDNode
{
  static constexpr int64_t NODE_LENGTH = 6;
  // Fills a NODE_LENGTH-byte UUID node.
  typedef int (*NodeProvider)(unsigned char *node, const int64_t len);
  // Fills len bytes of cryptographically secure random data.
  typedef int (*EntropyProvider)(unsigned char *buf, const int64_t len);

  ObUUIDNode();
  // Uses the hardware MAC address, or a process-wide random multicast node
  // when no usable MAC address is available.
  int init();
  // Same as init() with injectable providers; the node is published and
  // is_inited_ is set only on success.
  int init(NodeProvider get_mac, NodeProvider get_random_node);

  static int get_mac_address(unsigned char *node, const int64_t len);
  static int get_process_random_node(unsigned char *node, const int64_t len);
  // Random node with the multicast bit set (RFC 9562 section 6.10).
  static int gen_random_node(EntropyProvider get_entropy, unsigned char *node, const int64_t len);
  static int get_os_entropy(unsigned char *buf, const int64_t len);
#if !defined(_WIN32)
  // Reads exactly len bytes, retrying on EINTR and partial reads.
  static int read_fully(const int fd, unsigned char *buf, const int64_t len);
#endif
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
  // getrandom(2)-compatible: returns the byte count, or -1 with errno set.
  typedef long (*GetRandomFunc)(void *buf, size_t len, unsigned int flags);
  // Reads len bytes with get_random, retrying on EINTR and partial results;
  // reads fallback_path instead when getrandom fails with ENOSYS or EPERM.
  static int get_linux_entropy(GetRandomFunc get_random, const char *fallback_path,
                               unsigned char *buf, const int64_t len);
#endif

  unsigned char mac_addr_[NODE_LENGTH];
  bool is_inited_;

private:
  static common::ObLatch random_node_lock_;
  static unsigned char random_node_[NODE_LENGTH];
  static bool has_random_node_;
};

class ObUUIDTime
{
public:
  static int get_time(uint64_t &time, uint16_t &seq);
private:
  static int time_now(uint64_t &now);
  static void reset_clock_seq();
  static const uint64_t LILLIAN = 2299160; // Julian day of 15 Oct 1582
  static const uint64_t UNIXX = 2440587; // Julian day of 1 Jan 1970
  static const uint64_t EPOCH = UNIXX - LILLIAN; // Days between EPOCHs
  static const uint64_t G1582 = EPOCH * 86400; // seconds between EPOCHs
  static const uint64_t G1582NS100 = G1582 * 10000000; // 100s of a nanoseconds between EPOCHs

  static uint64_t lasttime_;
  static uint16_t clock_seq_;
  static common::ObLatch lock_;
};

class UuidCommon {
public:
  static int uuid2bin(char *result, bool &is_valid, const char *src, int64_t len);
  static int bin2uuid(char *result, const uchar *src);
private:
  static int read_section(char *res, bool &is_valid, const char *src, int64_t len);
public:
  static constexpr common::ObLength bytes_per_section[] = {4, 2, 2, 2, 6};
  //aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee
  static const common::ObLength LENGTH_UUID = 36;
  static const common::ObLength BYTE_LENGTH = 16;
};

class ObExprUuid : public ObFuncExprOperator
{
public:
  explicit ObExprUuid(common::ObIAllocator &alloc);
  explicit ObExprUuid(
    common::ObIAllocator &alloc,
    ObExprOperatorType type,
    const char *name,
    int32_t param_num,
    int32_t dimension);
  virtual ~ObExprUuid();
  virtual int calc_result_type0(ObExprResType &type,
                                common::ObExprTypeCtx &type_ctx) const;
  static int init();
  static int eval_uuid(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum);
  //used to generate uuid for server_uuid system variable.
  static int gen_server_uuid(char *server_uuid, const int64_t uuid_len);
  virtual int cg_expr(ObExprCGCtx &op_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
protected:
  static int calc(unsigned char *scratch);
private:
  static ObUUIDNode *uuid_node;
  //disallow copy
  DISALLOW_COPY_AND_ASSIGN(ObExprUuid);
};

inline int ObExprUuid::calc_result_type0(ObExprResType &type,
                                         common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  type.set_varchar();
  type.set_collation_level(common::CS_LEVEL_IMPLICIT);
  type.set_collation_type(common::ObCharset::get_default_collation(common::ObCharset::get_default_charset()));
  type.set_length(UuidCommon::LENGTH_UUID);
  return common::OB_SUCCESS;
}

class ObExprUuid2bin : public ObFuncExprOperator
{
public:
  explicit ObExprUuid2bin(common::ObIAllocator &alloc);
  virtual ~ObExprUuid2bin();
  virtual int calc_result_typeN(ObExprResType &type,
                                ObExprResType *types,
                                int64_t type_num,
                                common::ObExprTypeCtx &type_ctx) const;
  virtual int cg_expr(ObExprCGCtx &op_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
  static int uuid2bin(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum);
  static int uuid2bin_batch(const ObExpr &expr, ObEvalCtx &ctx, const ObBitVector &skip, const int64_t batch_size);
private:
  DISALLOW_COPY_AND_ASSIGN(ObExprUuid2bin);
};

class ObExprIsUuid : public ObFuncExprOperator
{
public:
  explicit ObExprIsUuid(common::ObIAllocator &alloc);
  virtual ~ObExprIsUuid();
  virtual int calc_result_type1(ObExprResType &type,
                                ObExprResType &type1,
                                common::ObExprTypeCtx &type_ctx) const;
  virtual int cg_expr(ObExprCGCtx &op_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
  static int is_uuid(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum);
  static int is_uuid_batch(const ObExpr &expr, ObEvalCtx &ctx, const ObBitVector &skip, const int64_t batch_size);
private:
  DISALLOW_COPY_AND_ASSIGN(ObExprIsUuid);
};

inline int ObExprIsUuid::calc_result_type1(ObExprResType &type,
                                    ObExprResType &type1,
                                    common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;
  UNUSED(type_ctx);
  type.set_tinyint();
  type.set_precision(DEFAULT_PRECISION_FOR_BOOL);
  type.set_scale(DEFAULT_SCALE_FOR_INTEGER);
  type1.set_calc_type(common::ObVarcharType);
  type1.set_calc_collation_type(type1.get_collation_type());
  type1.set_calc_collation_level(type1.get_collation_level());
  return ret;
}

class ObExprBin2uuid : public ObFuncExprOperator
{
public:
  explicit ObExprBin2uuid(common::ObIAllocator &alloc);
  virtual ~ObExprBin2uuid();
  virtual int calc_result_typeN(ObExprResType &type,
                                ObExprResType *types,
                                int64_t type_num,
                                common::ObExprTypeCtx &type_ctx) const;
  virtual int cg_expr(ObExprCGCtx &op_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;
  static int bin2uuid(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum);
  static int bin2uuid_batch(const ObExpr &expr, ObEvalCtx &ctx, const ObBitVector &skip, const int64_t batch_size);
private:
  DISALLOW_COPY_AND_ASSIGN(ObExprBin2uuid);
};
}
}


#endif /* SRC_SQL_ENGINE_EXPR_OB_EXPR_UUID_H_ */
