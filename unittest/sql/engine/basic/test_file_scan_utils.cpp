#include "sql/engine/basic/file_scan_utils.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace oceanbase::sql::file_sql;
namespace fs = std::filesystem;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      std::cerr << __LINE__ << ": " << #x << '\n';                                                 \
      std::abort();                                                                                \
    }                                                                                              \
  } while (0)

int main() {
  const fs::path root =
      fs::temp_directory_path() /
      ("seekdb_file_scan_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  CHECK(fs::create_directory(root));
  auto write = [&](const char *name, const std::string &text) {
    fs::path path = root / name;
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), text.size());
    CHECK(out.good());
    return path.string();
  };
  CHECK(infer_type("9223372036854775807") == Type::Bigint);
  CHECK(infer_type("-9223372036854775808") == Type::Bigint);
  CHECK(infer_type("9223372036854775808") == Type::Varchar);
  CHECK(infer_type("-9223372036854775809") == Type::Varchar);
  CHECK(infer_type("1.25e2") == Type::Double);
  CHECK(infer_type("1e999") == Type::Varchar);
  CHECK(infer_type("NaN") == Type::Varchar);
  CHECK(infer_type("2024-02-29") == Type::Date);
  CHECK(infer_type("1900-02-29") == Type::Varchar);
  CHECK(infer_type("2000-02-29 23:59:59.123456") == Type::Datetime);
  CHECK(infer_type("2000-02-29 24:00:00") == Type::Varchar);
  CHECK(infer_type("true") == Type::Boolean);
  for (int a = 0; a <= 6; ++a)
    for (int b = 0; b <= 6; ++b) {
      CHECK(widen(static_cast<Type>(a), static_cast<Type>(b)) ==
            widen(static_cast<Type>(b), static_cast<Type>(a)));
    }
  CHECK(widen(Type::Null, Type::Date) == Type::Date);
  CHECK(widen(Type::Bigint, Type::Double) == Type::Double);
  CHECK(widen(Type::Boolean, Type::Bigint) == Type::Varchar);
  CHECK(valid_utf8("\xf0\x9f\x98\x80"));
  CHECK(!valid_utf8("\xc0\x80"));
  CHECK(!valid_utf8("\xed\xa0\x80"));
  CHECK(!valid_utf8("\xf4\x90\x80\x80"));
  CHECK(!valid_utf8("\xe2\x82"));
  {
    auto path = write("quoted.csv",
                      "\xef\xbb\xbfid,name,nothing\r\n1,\"hello,\r\nworld\",\r\n2,\"a\"\"b\",\r\n");
    Reader r(Reader::Format::Csv, {});
    CHECK(r.open(path, root.string()));
    CHECK(r.infer());
    CHECK(r.columns().size() == 3);
    CHECK(r.columns()[0].type == Type::Bigint);
    CHECK(r.columns()[2].type == Type::Null);
    std::vector<Cell> row;
    CHECK(r.next(row));
    CHECK(row[1].text == "hello,\r\nworld");
    CHECK(row[2].type == Type::Null);
    CHECK(r.next(row));
    CHECK(row[1].text == "a\"b");
    CHECK(!r.next(row));
    CHECK(r.error().message.empty());
    CHECK(r.rescan());
    CHECK(r.next(row));
    CHECK(row[0].text == "1");
  }
  {
    Options o;
    o.header = false;
    o.delimiter = '|';
    o.escape = '\\';
    o.null_text = "NULL";
    Reader r(Reader::Format::Csv, o);
    CHECK(r.open(write("options.csv", "1|\"a\\\"b\"|NULL|\"NULL\"\n"), root.string()));
    CHECK(r.infer());
    std::vector<Cell> row;
    CHECK(r.next(row));
    CHECK(row.size() == 4);
    CHECK(row[1].text == "a\"b");
    CHECK(row[2].type == Type::Null);
    CHECK(row[3].type == Type::Varchar);
  }
  for (const auto &bad : {"a,b\n1\n", "a,A\n1,2\n", "a\n\"unterminated\n", "a\nx\"y\n",
                          "a\n\"x\"y\n", "a\n\xc0\x80\n"}) {
    Reader r(Reader::Format::Csv, {});
    CHECK(r.open(write("bad.csv", bad), root.string()));
    CHECK(!r.infer());
    CHECK(!r.error().message.empty());
  }
  {
    Reader r(Reader::Format::Jsonl, {});
    CHECK(r.open(
        write("union.jsonl", "{\"a\":1,\"b\":null}\n{\"a\":2.5,\"c\":\"\\ud83d\\ude00\"}\n{}\n"),
        root.string()));
    CHECK(r.infer());
    CHECK(r.columns().size() == 3);
    CHECK(r.columns()[0].type == Type::Double);
    std::vector<Cell> row;
    CHECK(r.next(row));
    CHECK(row[2].type == Type::Null);
    CHECK(r.next(row));
    CHECK(row[1].type == Type::Null);
    CHECK(row[2].text == "\xf0\x9f\x98\x80");
    CHECK(r.next(row));
    CHECK(row[0].type == Type::Null);
    CHECK(r.rescan());
    CHECK(r.next(row));
  }
  {
    Reader r(Reader::Format::Jsonl, {});
    CHECK(r.open(write("conflict.jsonl", "{\"value\":1}\n{\"value\":true}\n{\"value\":\"text\"}\n"),
                 root.string()));
    CHECK(r.infer());
    CHECK(r.columns()[0].type == Type::Varchar);
  }
  for (const auto &bad : {"[]", "{\"a\":{}}", "{\"a\":[]}", "{\"a\":01}", "{\"a\":true,}",
                          "{\"a\":1,\"a\":2}", "{\"a\":\"\\ud800\"}", "{\"a\":false}junk"}) {
    Reader r(Reader::Format::Jsonl, {});
    CHECK(r.open(write("bad.jsonl", bad), root.string()));
    CHECK(!r.infer());
    CHECK(!r.error().message.empty());
  }
  {
    Reader r(Reader::Format::Csv, {});
    CHECK(r.open(write("limit.csv", "a\n" + std::string(Reader::MAX_ROW_BYTES + 1, 'x')),
                 root.string()));
    CHECK(!r.infer());
    CHECK(r.error().row == 2);
  }
  {
    auto path = write("secure.csv", "a\n1\n");
    for (const auto &allowed : {std::string(), std::string("NULL"), (root / "missing").string()}) {
      Reader r(Reader::Format::Csv, {});
      CHECK(!r.open(path, allowed));
    }
    fs::create_directory(root / "sub");
    Reader r(Reader::Format::Csv, {});
    CHECK(!r.open((root / "sub/../secure.csv").string(), (root / "sub").string()));
    Options o;
    o.sample_size = 10;
    Reader sample(Reader::Format::Csv, o);
    CHECK(!sample.open(path, root.string()));
  }
  {
    write("discover_2.csv", "x\n2\n");
    write("discover_1.csv", "x\n1\n");
    Reader r(Reader::Format::Glob, {});
    CHECK(r.open((root / "discover_?.csv").string(), root.string()));
    CHECK(r.infer());
    CHECK(r.columns().size() == 1);
    CHECK(r.columns()[0].name == "file");
    std::vector<Cell> row;
    CHECK(r.next(row));
    CHECK(fs::path(row[0].text).filename() == "discover_1.csv");
    CHECK(r.next(row));
    CHECK(fs::path(row[0].text).filename() == "discover_2.csv");
    CHECK(!r.next(row));
    CHECK(r.error().message.empty());
    CHECK(r.rescan());
    CHECK(r.next(row));
    Reader denied(Reader::Format::Glob, {});
    CHECK(!denied.open((root / "sub/../*.csv").string(), (root / "sub").string()));
    Reader empty(Reader::Format::Glob, {});
    CHECK(empty.open((root / "no_match_*.csv").string(), root.string()));
    CHECK(empty.infer());
    CHECK(!empty.next(row));
    CHECK(empty.error().message.empty());
  }
  {
    auto path = write("stream.csv", "n\n");
    {
      std::ofstream out(path, std::ios::app);
      for (int i = 0; i < 50000; ++i)
        out << i << '\n';
      CHECK(out.good());
    }
    Reader r(Reader::Format::Csv, {});
    CHECK(r.open(path, root.string()));
    CHECK(r.infer());
    std::vector<Cell> row;
    for (int pass = 0; pass < 2; ++pass) {
      int64_t sum = 0, count = 0;
      while (r.next(row)) {
        sum += std::strtoll(row[0].text.c_str(), nullptr, 10);
        ++count;
      }
      CHECK(r.error().message.empty());
      CHECK(count == 50000);
      CHECK(sum == 1249975000);
      CHECK(r.rescan());
    }
    std::vector<std::vector<Cell>> batch;
    int64_t count = 0;
    while (r.next_batch(256, batch)) { CHECK(batch.size() <= 256); count += batch.size(); }
    CHECK(r.error().message.empty()); CHECK(count == 50000);
  }
#ifdef _WIN32
  {
    auto path = write("locked.csv", "a\n1\n");
    Reader r(Reader::Format::Csv, {});
    CHECK(r.open(path, root.string()));
    CHECK(r.infer());
    std::ofstream writer(path, std::ios::app);
    CHECK(!writer.is_open());
    std::error_code ec;
    fs::remove(path, ec);
    CHECK(ec);
  }
#endif
#ifndef _WIN32
  {
    auto path = write("change.csv", "a\n1\n");
    Reader r(Reader::Format::Csv, {});
    CHECK(r.open(path, root.string()));
    CHECK(r.infer());
    write("change.csv", "a\n2\n");
    std::vector<Cell> row;
    CHECK(!r.next(row));
    CHECK(!r.error().message.empty());
  }
  {
    fs::create_symlink(root / "secure.csv", root / "sub/escape.csv");
    Reader r(Reader::Format::Csv, {});
    CHECK(!r.open((root / "sub/escape.csv").string(), (root / "sub").string()));
  }
#endif
  for (const char *fixture : {"flat_1.0_NONE.parquet", "flat_1.0_snappy.parquet",
                              "flat_2.0_NONE.parquet", "flat_2.0_snappy.parquet"}) {
    auto source = fs::path("unittest/sql/engine/basic/data") / fixture;
    auto path = root / fixture;
    fs::copy_file(source, path);
    Reader r(Reader::Format::Parquet, {});
    CHECK(r.open(path.string(), root.string()));
    if (!r.infer())
      std::cerr << r.error().message << "\n";
    CHECK(r.error().message.empty());
    CHECK(r.columns().size() == 6);
    CHECK(r.columns()[4].type == Type::Date);
    CHECK(r.columns()[5].type == Type::Datetime);
    for (int pass = 0; pass < 2; ++pass) {
      std::vector<Cell> row;
      int count = 0;
      while (r.next(row)) {
        ++count;
        CHECK(row[0].text == std::to_string(count));
        if (count == 2) {
          CHECK(row[1].type == Type::Null);
          CHECK(row[5].type == Type::Null);
        }
        if (count == 4)
          CHECK(row[5].text == "1969-12-31 23:59:59.999999");
      }
      if (!r.error().message.empty())
        std::cerr << r.error().message << "\n";
      CHECK(r.error().message.empty());
      CHECK(count == 5);
      CHECK(r.rescan());
    }
  }
  {
    std::ifstream input("unittest/sql/engine/basic/data/flat_1.0_snappy.parquet", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    CHECK(bytes.size() > 100);
    for (size_t length : {size_t(0), size_t(3), size_t(8), bytes.size()/2, bytes.size()-1}) {
      Reader bad(Reader::Format::Parquet, {});
      CHECK(bad.open(write("truncated.parquet", bytes.substr(0, length)), root.string()));
      CHECK(!bad.infer()); CHECK(!bad.error().message.empty());
    }
    Reader projected(Reader::Format::Parquet, {});
    CHECK(projected.open((root / "flat_1.0_snappy.parquet").string(), root.string()));
    CHECK(projected.infer()); projected.project({0, 2});
    std::vector<Cell> row; CHECK(projected.next(row));
    CHECK(row[0].text == "1"); CHECK(row[1].type == Type::Null); CHECK(row[2].text == "1.5");
  }
  {
    auto path = (root / "copy.csv").string();
    Options options;
    options.null_text = "NULL";
    Error error;
    {
      CsvWriter writer(options);
      CHECK(writer.open(path, root.string(), error));
      CHECK(writer.write({{"a", Type::Varchar}, {"b", Type::Varchar}}, error));
      CHECK(writer.write({{"quote \" and\nnewline", Type::Varchar}, {"", Type::Null}}, error));
      CHECK(writer.write({{"NULL", Type::Varchar}, {"", Type::Varchar}}, error));
      CHECK(writer.commit(error));
    }
    Reader reader(Reader::Format::Csv, options);
    CHECK(reader.open(path, root.string()));
    CHECK(reader.infer());
    std::vector<Cell> row;
    CHECK(reader.next(row));
    CHECK(row[0].text == "quote \" and\nnewline");
    CHECK(row[1].type == Type::Null);
    CHECK(reader.next(row));
    CHECK(row[0].text == "NULL");
    CHECK(row[0].type != Type::Null);
    CHECK(row[1].type != Type::Null);
    CsvWriter exists(options);
    CHECK(!exists.open(path, root.string(), error));
    auto failed = (root / "failed.csv").string();
    {
      CsvWriter cancelled(options);
      CHECK(cancelled.open(failed, root.string(), error));
      CHECK(cancelled.write({{"partial", Type::Varchar}}, error));
    }
    CHECK(!fs::exists(failed));
    for (const auto &entry : fs::directory_iterator(root))
      CHECK(entry.path().string().find("seekdb-tmp-") == std::string::npos);
    CsvWriter outside(options);
    CHECK(!outside.open((root / "sub/../escape.csv").string(), (root / "sub").string(), error));
  }
  {
    Error error; auto path = (root / "race.csv").string();
    {
      CsvWriter writer({}); CHECK(writer.open(path, root.string(), error));
      CHECK(writer.write({{"new", Type::Varchar}}, error));
      write("race.csv", "existing\n"); CHECK(!writer.commit(error));
    }
    std::ifstream existing(path); std::string value; std::getline(existing, value); CHECK(value == "existing");
    for (const auto &entry : fs::directory_iterator(root)) CHECK(entry.path().string().find("seekdb-tmp-") == std::string::npos);
  }
  CHECK(fs::remove_all(root) > 0);
  std::cout << "file_scan_utils: all tests passed\n";
}
