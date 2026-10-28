/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#include "sql/file_sql/ob_file_sql_scan_utils.h"

#include <gtest/gtest.h>
#include <fstream>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{
namespace
{

class FileSqlScanUtilsTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    root_ = std::filesystem::temp_directory_path() / "seekdb-file-sql-test";
    std::filesystem::create_directories(root_);
  }

  void TearDown() override
  {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  std::filesystem::path root_;
};

TEST_F(FileSqlScanUtilsTest, CsvHandlesQuotedNewlinesAndFullSchemaInference)
{
  const auto path = root_ / "rows.csv";
  std::ofstream output(path, std::ios::binary);
  output << "id,name,score,active,day,created,note\n"
         << "1,alice,1.5,true,2026-01-01,2026-01-01 10:20:30,\"hello\nworld\"\n"
         << "2,bob,NULL,false,2026-01-02,2026-01-02 10:20:30,\"a \"\"quoted\"\" note\"\n";
  output.close();

  CsvReader reader;
  FileSqlError error;
  ASSERT_TRUE(reader.open(path, true, ',', "NULL", error)) << error.message;
  std::vector<FileSqlColumn> schema;
  ASSERT_TRUE(reader.infer_schema(schema, error)) << error.message;
  ASSERT_EQ(7U, schema.size());
  EXPECT_EQ(FileSqlType::BIGINT, schema[0].type);
  EXPECT_EQ(FileSqlType::VARCHAR, schema[1].type);
  EXPECT_EQ(FileSqlType::DOUBLE, schema[2].type);
  EXPECT_EQ(FileSqlType::BOOLEAN, schema[3].type);
  EXPECT_EQ(FileSqlType::DATE, schema[4].type);
  EXPECT_EQ(FileSqlType::DATETIME, schema[5].type);

  std::vector<FileSqlValue> row;
  ASSERT_TRUE(reader.next(row, error));
  ASSERT_EQ(7U, row.size());
  EXPECT_EQ("hello\nworld", row[6].text);
  ASSERT_TRUE(reader.next(row, error));
  EXPECT_TRUE(row[2].is_null);
  EXPECT_EQ("a \"quoted\" note", row[6].text);
}

TEST_F(FileSqlScanUtilsTest, JsonlUnionsFieldsAndRejectsNestedValues)
{
  const auto path = root_ / "rows.jsonl";
  std::ofstream output(path, std::ios::binary);
  output << R"({"id":1,"name":"alice","active":true})" << '\n'
         << R"({"id":2,"score":2.5,"active":false})" << '\n';
  output.close();

  JsonlReader reader;
  FileSqlError error;
  ASSERT_TRUE(reader.open(path, error)) << error.message;
  std::vector<FileSqlColumn> schema;
  ASSERT_TRUE(reader.infer_schema(schema, error)) << error.message;
  ASSERT_EQ(4U, schema.size());
  EXPECT_EQ("id", schema[0].name);
  EXPECT_EQ("score", schema[2].name);
  EXPECT_EQ(FileSqlType::DOUBLE, schema[2].type);

  std::vector<FileSqlValue> row;
  ASSERT_TRUE(reader.next(row, error));
  ASSERT_EQ(4U, row.size());
  EXPECT_TRUE(row[2].is_null);
  ASSERT_TRUE(reader.next(row, error));
  EXPECT_EQ("2.5", row[2].text);

  const auto nested = root_ / "nested.jsonl";
  std::ofstream nested_output(nested, std::ios::binary);
  nested_output << R"({"id":1,"payload":{"x":2}})" << '\n';
  nested_output.close();
  JsonlReader nested_reader;
  ASSERT_TRUE(nested_reader.open(nested, error));
  EXPECT_FALSE(nested_reader.infer_schema(schema, error));
  EXPECT_NE(std::string::npos, error.message.find("nested"));
}

TEST_F(FileSqlScanUtilsTest, PathPolicyRejectsEscape)
{
  std::filesystem::path resolved;
  EXPECT_TRUE(is_path_allowed(root_, root_ / "inside.csv", &resolved));
  EXPECT_FALSE(is_path_allowed(root_, root_ / ".." / "outside.csv", &resolved));
  EXPECT_EQ(FileSqlType::DOUBLE, widen_type(FileSqlType::BIGINT, FileSqlType::DOUBLE));
  EXPECT_EQ(FileSqlType::VARCHAR, widen_type(FileSqlType::BOOLEAN, FileSqlType::BIGINT));
}

} // namespace
} // namespace file_sql
} // namespace sql
} // namespace oceanbase
