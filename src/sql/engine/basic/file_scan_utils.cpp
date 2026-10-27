#include "file_scan_utils.h"
#include "file_parquet_reader.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sys/stat.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace oceanbase {
namespace sql {
namespace file_sql {
namespace {
bool digits(const std::string &s, size_t pos, size_t count) {
  if (pos + count > s.size())
    return false;
  for (size_t i = pos; i < pos + count; ++i)
    if (s[i] < '0' || s[i] > '9')
      return false;
  return true;
}
unsigned number(const std::string &s, size_t pos, size_t count) {
  unsigned n = 0;
  for (size_t i = pos; i < pos + count; ++i)
    n = n * 10 + s[i] - '0';
  return n;
}
bool date(const std::string &s) {
  if (s.size() < 10 || s[4] != '-' || s[7] != '-' || !digits(s, 0, 4) || !digits(s, 5, 2) ||
      !digits(s, 8, 2))
    return false;
  unsigned y = number(s, 0, 4), m = number(s, 5, 2), d = number(s, 8, 2);
  const unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return y >= 1 && m >= 1 && m <= 12 && d >= 1 &&
         d <= days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}
bool numeric(const std::string &s, bool &integer) {
  size_t p = 0;
  integer = true;
  if (p < s.size() && (s[p] == '-' || s[p] == '+'))
    ++p;
  const size_t start = p;
  while (p < s.size() && s[p] >= '0' && s[p] <= '9')
    ++p;
  if (p == start)
    return false;
  if (p < s.size() && s[p] == '.') {
    integer = false;
    const size_t fraction = ++p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9')
      ++p;
    if (p == fraction)
      return false;
  }
  if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
    integer = false;
    ++p;
    if (p < s.size() && (s[p] == '-' || s[p] == '+'))
      ++p;
    const size_t exponent = p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9')
      ++p;
    if (p == exponent)
      return false;
  }
  return p == s.size();
}
std::string folded(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return s;
}
bool inside(const std::string &path, std::string root) {
  while (root.size() > 1 && (root.back() == '/' || root.back() == '\\'))
    root.pop_back();
  if (root == "/")
    return !path.empty() && path.front() == '/';
  return path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
         (path[root.size()] == '/' || path[root.size()] == '\\');
}
// Linear wildcard matching with a single backtracking position. Directory
// components are literal: discovery does not traverse an unbounded tree.
bool wildcard(const std::string &pattern, const std::string &name) {
  size_t p = 0, n = 0, star = std::string::npos, retry = 0;
  while (n < name.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n])) {
      ++p;
      ++n;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      retry = n;
    } else if (star != std::string::npos) {
      p = star + 1;
      n = ++retry;
    } else
      return false;
  }
  while (p < pattern.size() && pattern[p] == '*')
    ++p;
  return p == pattern.size();
}
#ifdef _WIN32
bool handle_path(HANDLE h, std::string &s) {
  char buf[32768];
  DWORD n = GetFinalPathNameByHandleA(h, buf, sizeof(buf), FILE_NAME_NORMALIZED);
  if (!n || n >= sizeof(buf))
    return false;
  s.assign(buf, n);
  s = folded(s);
  return true;
}
#endif

// A strict flat JSON object parser. Nested containers are rejected before any
// recursion or allocation. Strings are decoded once, including surrogate pairs.
class Json {
public:
  explicit Json(const std::string &s) : s_(s) {}
  bool parse(std::vector<Cell> &cells, std::vector<std::string> &names) {
    ws();
    if (!take('{'))
      return false;
    ws();
    if (take('}')) {
      ws();
      return p_ == s_.size();
    }
    for (;;) {
      std::string key;
      if (cells.size() >= Reader::MAX_COLUMNS || !string(key) || key.empty() || key.size() > 256)
        return false;
      for (const auto &name : names)
        if (folded(name) == folded(key))
          return false;
      ws();
      if (!take(':'))
        return false;
      ws();
      Cell c;
      if (p_ < s_.size() && s_[p_] == '"') {
        if (!string(c.text))
          return false;
        // Recognize ISO dates in JSON strings, but preserve quoted numbers.
        c.type = infer_type(c.text);
        if (c.type != Type::Date && c.type != Type::Datetime)
          c.type = Type::Varchar;
      } else {
        size_t start = p_;
        while (p_ < s_.size() && s_[p_] != ',' && s_[p_] != '}' && s_[p_] != ' ' &&
               s_[p_] != '\t' && s_[p_] != '\r')
          ++p_;
        c.text = s_.substr(start, p_ - start);
        if (c.text == "null") {
          c.type = Type::Null;
          c.text.clear();
        } else if (c.text == "true" || c.text == "false")
          c.type = Type::Boolean;
        else {
          bool integer = false;
          size_t first = !c.text.empty() && c.text[0] == '-' ? 1 : 0;
          if (c.text.empty() || c.text[0] == '+' || !numeric(c.text, integer) ||
              (first + 1 < c.text.size() && c.text[first] == '0' && c.text[first + 1] >= '0' &&
               c.text[first + 1] <= '9'))
            return false;
          c.type = infer_type(c.text);
        }
      }
      names.push_back(std::move(key));
      cells.push_back(std::move(c));
      ws();
      if (take('}')) {
        ws();
        return p_ == s_.size();
      }
      if (!take(','))
        return false;
      ws();
    }
  }

private:
  void ws() {
    while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r'))
      ++p_;
  }
  bool take(char c) {
    if (p_ < s_.size() && s_[p_] == c) {
      ++p_;
      return true;
    }
    return false;
  }
  bool hex(unsigned &n) {
    n = 0;
    for (unsigned i = 0; i < 4; ++i) {
      if (p_ == s_.size())
        return false;
      char c = s_[p_++];
      unsigned d = c >= '0' && c <= '9'   ? c - '0'
                   : c >= 'a' && c <= 'f' ? c - 'a' + 10
                   : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                          : 16;
      if (d == 16)
        return false;
      n = n * 16 + d;
    }
    return true;
  }
  bool string(std::string &out) {
    if (!take('"'))
      return false;
    while (p_ < s_.size()) {
      unsigned char c = s_[p_++];
      if (c == '"')
        return valid_utf8(out);
      if (c < 32)
        return false;
      if (c != '\\') {
        out += static_cast<char>(c);
        continue;
      }
      if (p_ == s_.size())
        return false;
      c = s_[p_++];
      switch (c) {
      case '"':
      case '\\':
      case '/':
        out += static_cast<char>(c);
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        unsigned u = 0;
        if (!hex(u))
          return false;
        if (u >= 0xd800 && u <= 0xdbff) {
          unsigned low = 0;
          if (!take('\\') || !take('u') || !hex(low) || low < 0xdc00 || low > 0xdfff)
            return false;
          u = 0x10000 + ((u - 0xd800) << 10) + low - 0xdc00;
        } else if (u >= 0xdc00 && u <= 0xdfff)
          return false;
        if (u < 0x80)
          out += static_cast<char>(u);
        else if (u < 0x800) {
          out += static_cast<char>(0xc0 | (u >> 6));
          out += static_cast<char>(0x80 | (u & 63));
        } else if (u < 0x10000) {
          out += static_cast<char>(0xe0 | (u >> 12));
          out += static_cast<char>(0x80 | ((u >> 6) & 63));
          out += static_cast<char>(0x80 | (u & 63));
        } else {
          out += static_cast<char>(0xf0 | (u >> 18));
          out += static_cast<char>(0x80 | ((u >> 12) & 63));
          out += static_cast<char>(0x80 | ((u >> 6) & 63));
          out += static_cast<char>(0x80 | (u & 63));
        }
        break;
      }
      default:
        return false;
      }
    }
    return false;
  }
  const std::string &s_;
  size_t p_ = 0;
};
} // namespace

Type infer_type(const std::string &s) {
  if (s == "true" || s == "false" || s == "TRUE" || s == "FALSE")
    return Type::Boolean;
  if (date(s)) {
    if (s.size() == 10)
      return Type::Date;
    if (s.size() >= 19 && (s[10] == ' ' || s[10] == 'T') && s[13] == ':' && s[16] == ':' &&
        digits(s, 11, 2) && digits(s, 14, 2) && digits(s, 17, 2) && number(s, 11, 2) < 24 &&
        number(s, 14, 2) < 60 && number(s, 17, 2) < 60 &&
        (s.size() == 19 ||
         (s.size() >= 21 && s.size() <= 26 && s[19] == '.' && digits(s, 20, s.size() - 20))))
      return Type::Datetime;
  }
  bool integer = false;
  if (numeric(s, integer)) {
    const char *begin = s.data() + (s[0] == '+' ? 1 : 0);
    if (integer) {
      int64_t value = 0;
      auto r = std::from_chars(begin, s.data() + s.size(), value);
      // Overflowing integers remain exact text, never an imprecise double.
      if (r.ec == std::errc() && r.ptr == s.data() + s.size())
        return Type::Bigint;
    } else {
      double value = 0;
      auto r = std::from_chars(begin, s.data() + s.size(), value);
      if (r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(value))
        return Type::Double;
    }
  }
  return Type::Varchar;
}
Type widen(Type a, Type b) {
  if (a == Type::Null)
    return b;
  if (b == Type::Null || a == b)
    return a;
  if ((a == Type::Bigint && b == Type::Double) || (b == Type::Bigint && a == Type::Double))
    return Type::Double;
  if ((a == Type::Date && b == Type::Datetime) || (b == Type::Date && a == Type::Datetime))
    return Type::Datetime;
  return Type::Varchar;
}
bool valid_utf8(const std::string &s) {
  for (size_t i = 0; i < s.size();) {
    unsigned c = static_cast<unsigned char>(s[i++]);
    if (c < 128)
      continue;
    unsigned n = c >= 0xc2 && c <= 0xdf   ? 1
                 : c >= 0xe0 && c <= 0xef ? 2
                 : c >= 0xf0 && c <= 0xf4 ? 3
                                          : 0;
    if (!n || s.size() - i < n)
      return false;
    unsigned u = c & ((1u << (6 - n)) - 1);
    for (unsigned j = 0; j < n; ++j) {
      c = static_cast<unsigned char>(s[i++]);
      if ((c & 0xc0) != 0x80)
        return false;
      u = (u << 6) | (c & 63);
    }
    if ((n == 1 && u < 0x80) || (n == 2 && u < 0x800) || (n == 3 && u < 0x10000) ||
        (u >= 0xd800 && u <= 0xdfff) || u > 0x10ffff)
      return false;
  }
  return true;
}
std::string json_string(const std::string &s) {
  const char hex[] = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (c < 32) {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    } else
      out += c;
  }
  out += '"';
  return out;
}

File::~File() {
  if (stream_)
    fclose(stream_);
}
bool File::snapshot(std::string &out) const {
#ifdef _WIN32
  BY_HANDLE_FILE_INFORMATION info;
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream_)));
  if (!GetFileInformationByHandle(h, &info))
    return false;
  out = std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) +
        ":" + std::to_string(info.nFileIndexLow) + ":" + std::to_string(info.nFileSizeHigh) + ":" +
        std::to_string(info.nFileSizeLow) + ":" +
        std::to_string(info.ftLastWriteTime.dwHighDateTime) + ":" +
        std::to_string(info.ftLastWriteTime.dwLowDateTime);
#else
  struct stat st;
  if (fstat(fileno(stream_), &st) != 0 || !S_ISREG(st.st_mode))
    return false;
  out = std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":" +
        std::to_string(st.st_size) + ":" + std::to_string(st.st_mtim.tv_sec) + ":" +
        std::to_string(st.st_mtim.tv_nsec) + ":" + std::to_string(st.st_ctim.tv_sec) + ":" +
        std::to_string(st.st_ctim.tv_nsec);
#endif
  return true;
}
bool File::open(const std::string &path, const std::string &allowed, Error &error) {
  if (stream_) {
    error.message = "file already open";
    return false;
  }
  if (path.empty() || path.size() > 32760 || path.find('\0') != std::string::npos ||
      allowed.empty() || allowed.size() > 32760 || folded(allowed) == "null" ||
      allowed.find('\0') != std::string::npos) {
    error.message = "file access denied by secure_file_priv";
    return false;
  }
  std::string actual, root;
#ifdef _WIN32
  HANDLE directory =
      CreateFileA(allowed.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (directory == INVALID_HANDLE_VALUE) {
    error.message = "cannot open secure_file_priv directory";
    return false;
  }
  bool root_ok = handle_path(directory, root);
  CloseHandle(directory);
  // Deny writes/deletion while the query owns the input handle.
  HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  BY_HANDLE_FILE_INFORMATION info;
  if (h == INVALID_HANDLE_VALUE || !root_ok || !GetFileInformationByHandle(h, &info) ||
      (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || GetFileType(h) != FILE_TYPE_DISK ||
      !handle_path(h, actual) || !inside(actual, root)) {
    if (h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
    error.message = "file access denied by secure_file_priv";
    return false;
  }
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY | _O_BINARY);
  if (fd < 0)
    CloseHandle(h);
  else {
    stream_ = _fdopen(fd, "rb");
    if (!stream_)
      _close(fd);
  }
#else
  char *canonical = realpath(allowed.c_str(), nullptr);
  if (!canonical) {
    error.message = "cannot resolve secure_file_priv directory";
    return false;
  }
  root = canonical;
  free(canonical);
  // O_NONBLOCK prevents a malicious FIFO from hanging the SQL worker.
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  struct stat st;
  char target[32768];
  std::string proc = "/proc/self/fd/" + std::to_string(fd);
  ssize_t n = fd < 0 ? -1 : readlink(proc.c_str(), target, sizeof(target));
  if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || n <= 0 ||
      n >= static_cast<ssize_t>(sizeof(target)) || !inside(std::string(target, n), root)) {
    if (fd >= 0)
      close(fd);
    error.message = "file access denied by secure_file_priv";
    return false;
  }
  actual.assign(target, n);
  stream_ = fdopen(fd, "rb");
  if (!stream_)
    close(fd);
#endif
  if (!stream_ || !snapshot(identity_)) {
    error.message = "cannot open regular input file";
    return false;
  }
  path_ = actual;
  return true;
}
bool File::unchanged(Error &error) const {
  std::string now;
  if (!stream_ || !snapshot(now) || now != identity_) {
    error.message = "input file changed during query";
    return false;
  }
#ifndef _WIN32
  struct stat st, opened;
  if (stat(path_.c_str(), &st) || fstat(fileno(stream_), &opened) || st.st_dev != opened.st_dev ||
      st.st_ino != opened.st_ino) {
    error.message = "input file replaced during query";
    return false;
  }
#endif
  return true;
}
bool File::rewind(Error &error) {
  if (!unchanged(error))
    return false;
  if (fseek(stream_, 0, SEEK_SET)) {
    error.message = "cannot rewind input file";
    return false;
  }
  clearerr(stream_);
  return true;
}
bool Reader::fail(const char *message, size_t column) {
  error_.message = message;
  error_.row = row_number_;
  error_.column = column;
  return false;
}
bool Reader::open(const std::string &path, const std::string &allowed) {
  if (options_.sample_size != -1)
    return fail("sample_size must be -1");
  if (format_ == Format::Glob) {
    namespace fs = std::filesystem;
    if (allowed.empty() || folded(allowed) == "null" || allowed.find('\0') != std::string::npos ||
        path.empty() || path.find('\0') != std::string::npos || path.size() > 32760)
      return fail("file access denied by secure_file_priv");
    std::error_code ec;
    fs::path pattern(path);
    auto root = fs::canonical(allowed, ec);
    if (ec)
      return fail("cannot resolve secure_file_priv directory");
    auto parent =
        fs::canonical(pattern.has_parent_path() ? pattern.parent_path() : fs::path("."), ec);
    if (ec)
      return fail("cannot resolve glob directory");
    std::string parent_name = parent.generic_string(), root_name = root.generic_string();
#ifdef _WIN32
    parent_name = folded(parent_name);
    root_name = folded(root_name);
#endif
    if (parent_name != root_name && !inside(parent_name, root_name))
      return fail("file access denied by secure_file_priv");
    fs::directory_iterator it(parent, ec), end;
    if (ec)
      return fail("cannot enumerate glob directory");
    size_t bytes = 0;
    for (; it != end; it.increment(ec)) {
      if (ec)
        return fail("cannot enumerate glob directory");
      if (!wildcard(pattern.filename().string(), it->path().filename().string()))
        continue;
      if (!it->is_regular_file(ec)) {
        if (ec)
          return fail("cannot inspect glob entry");
        continue;
      }
      File check;
      Error error;
      if (!check.open(it->path().string(), allowed, error))
        return fail("glob entry denied by secure_file_priv");
      auto name = it->path().string();
      bytes += name.size();
      if (discovered_.size() == 10000 || bytes > MAX_SCHEMA_BYTES)
        return fail("glob exceeds discovery limit");
      discovered_.push_back(std::move(name));
    }
    if (ec)
      return fail("cannot enumerate glob directory");
    std::sort(discovered_.begin(), discovered_.end());
    uint64_t hash = 14695981039346656037ULL;
    for (const auto &name : discovered_) {
      for (unsigned char c : name) {
        hash ^= c;
        hash *= 1099511628211ULL;
      }
      hash ^= 0;
      hash *= 1099511628211ULL;
    }
    discovery_identity_ = std::to_string(hash);
    return true;
  }
  if (!options_.delimiter || !options_.quote || !options_.escape ||
      options_.delimiter == options_.quote || options_.delimiter == '\r' ||
      options_.delimiter == '\n' || options_.quote == '\r' || options_.quote == '\n' ||
      options_.escape == '\r' || options_.escape == '\n' ||
      static_cast<unsigned char>(options_.delimiter) >= 128 ||
      static_cast<unsigned char>(options_.quote) >= 128 ||
      static_cast<unsigned char>(options_.escape) >= 128 ||
      options_.null_text.size() > MAX_ROW_BYTES)
    return fail("invalid CSV options");
  return file_.open(path, allowed, error_);
}
Reader::Reader(Format format, const Options &options) : format_(format), options_(options) {}
Reader::~Reader() = default;
void Reader::project(const std::vector<size_t> &indices) {
  if (parquet_)
    parquet_->project(indices);
}

bool Reader::csv(std::vector<Cell> &row) {
  enum State { Start, Plain, Quoted, Closed } state = Start;
  Cell cell;
  bool was_quoted = false, any = false;
  size_t bytes = 0;
  auto finish = [&]() {
    if (!valid_utf8(cell.text))
      return fail("invalid UTF-8", row.size() + 1);
    cell.type = !was_quoted && cell.text == options_.null_text ? Type::Null : infer_type(cell.text);
    row.push_back(std::move(cell));
    cell = Cell();
    was_quoted = false;
    state = Start;
    return row.size() <= MAX_COLUMNS || fail("too many columns");
  };
  for (;;) {
    int c = fgetc(file_.stream());
    if (c == EOF) {
      if (ferror(file_.stream()))
        return fail("input read failed");
      if (state == Quoted)
        return fail("unterminated quoted CSV field", row.size() + 1);
      if (!any)
        return false;
      return finish();
    }
    any = true;
    if (++bytes > MAX_ROW_BYTES)
      return fail("row exceeds byte limit");
    if (state == Quoted) {
      if (c == options_.escape && options_.escape != options_.quote) {
        int escaped = fgetc(file_.stream());
        if (escaped != options_.quote && escaped != options_.escape)
          return fail("invalid CSV escape", row.size() + 1);
        if (++bytes > MAX_ROW_BYTES)
          return fail("row exceeds byte limit");
        cell.text += static_cast<char>(escaped);
      } else if (c == options_.quote)
        state = Closed;
      else
        cell.text += static_cast<char>(c);
    } else if (state == Closed && c == options_.quote && options_.escape == options_.quote) {
      cell.text += static_cast<char>(c);
      state = Quoted;
    } else if (c == options_.delimiter) {
      if (!finish())
        return false;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r') {
        int lf = fgetc(file_.stream());
        if (lf != '\n')
          return fail("expected LF after CR");
      }
      return finish();
    } else if (state == Start && c == options_.quote) {
      was_quoted = true;
      state = Quoted;
    } else if (state == Closed || c == options_.quote) {
      return fail("unexpected character in CSV field", row.size() + 1);
    } else {
      state = Plain;
      cell.text += static_cast<char>(c);
    }
  }
}
bool Reader::json(std::vector<Cell> &row, std::vector<std::string> &names) {
  std::string line;
  for (;;) {
    int c = fgetc(file_.stream());
    if (c == EOF) {
      if (ferror(file_.stream()))
        return fail("input read failed");
      if (line.empty())
        return false;
      break;
    }
    if (c == '\n')
      break;
    if (line.size() == MAX_ROW_BYTES)
      return fail("row exceeds byte limit");
    line += static_cast<char>(c);
  }
  if (!valid_utf8(line) || !Json(line).parse(row, names))
    return fail("expected a UTF-8 flat JSON object with unique fields");
  return true;
}
bool Reader::raw(std::vector<Cell> &row, std::vector<std::string> &names) {
  row.clear();
  names.clear();
  ++row_number_;
  return format_ == Format::Csv ? csv(row) : json(row, names);
}
bool Reader::infer() {
  if (inferred_)
    return fail("schema already inferred");
  if (format_ == Format::Glob) {
    columns_.push_back({"file", Type::Varchar});
    inferred_ = true;
    return true;
  }
  if (format_ == Format::Parquet) {
    parquet_.reset(new ParquetReader);
    inferred_ = parquet_->infer(file_, columns_, error_);
    return inferred_;
  }
  if (!file_.rewind(error_))
    return false;
  row_number_ = 0;
  // UTF-8 BOM is accepted only at the start of a file.
  int a = fgetc(file_.stream());
  if (a == 0xef) {
    if (fgetc(file_.stream()) != 0xbb || fgetc(file_.stream()) != 0xbf)
      return fail("invalid UTF-8 BOM");
  } else if (a != EOF)
    ungetc(a, file_.stream());
  std::vector<Cell> row;
  std::vector<std::string> names;
  size_t schema_bytes = 0;
  while (raw(row, names)) {
    if (cancelled_ && cancelled_()) return fail("file SQL query cancelled or timed out");
    if (format_ == Format::Csv) {
      if (columns_.empty()) {
        for (size_t i = 0; i < row.size(); ++i) {
          std::string name = options_.header ? row[i].text : "column" + std::to_string(i + 1);
          if (name.empty() || name.size() > 256 || name.find('\0') != std::string::npos)
            return fail("invalid column name", i + 1);
          for (const auto &col : columns_)
            if (folded(col.name) == folded(name))
              return fail("duplicate column name", i + 1);
          schema_bytes += name.size();
          columns_.push_back({std::move(name), Type::Null});
        }
        if (options_.header)
          continue;
      }
      if (row.size() != columns_.size())
        return fail("CSV field count differs from schema");
      for (size_t i = 0; i < row.size(); ++i)
        columns_[i].type = widen(columns_[i].type, row[i].type);
    } else {
      for (size_t i = 0; i < names.size(); ++i) {
        auto col = std::find_if(columns_.begin(), columns_.end(),
                                [&](const Column &c) { return c.name == names[i]; });
        if (col == columns_.end()) {
          for (const auto &c : columns_)
            if (folded(c.name) == folded(names[i]))
              return fail("ambiguous JSON field names");
          if (names[i].find('\0') != std::string::npos)
            return fail("invalid column name");
          schema_bytes += names[i].size();
          if (columns_.size() == MAX_COLUMNS || schema_bytes > MAX_SCHEMA_BYTES)
            return fail("schema exceeds resource limit");
          columns_.push_back({names[i], row[i].type});
        } else
          col->type = widen(col->type, row[i].type);
      }
    }
  }
  if (!error_.message.empty() || !file_.unchanged(error_))
    return false;
  if (columns_.empty())
    return fail("cannot infer columns from empty input");
  inferred_ = true;
  return rescan();
}
bool Reader::rescan() {
  if (format_ == Format::Parquet)
    return parquet_ && parquet_->rescan(error_);
  if (format_ == Format::Glob) {
    discovery_position_ = 0;
    return error_.message.empty() && inferred_;
  }
  if (!error_.message.empty() || !inferred_ || !file_.rewind(error_))
    return false;
  row_number_ = 0;
  int a = fgetc(file_.stream());
  if (a == 0xef) {
    if (fgetc(file_.stream()) != 0xbb || fgetc(file_.stream()) != 0xbf)
      return fail("invalid UTF-8 BOM");
  } else if (a != EOF)
    ungetc(a, file_.stream());
  if (format_ == Format::Csv && options_.header) {
    std::vector<Cell> header;
    std::vector<std::string> names;
    if (!raw(header, names))
      return fail("missing CSV header");
  }
  return true;
}
bool Reader::next_batch(size_t max_rows, std::vector<std::vector<Cell>> &rows) {
  rows.clear();
  if (!max_rows || max_rows > 1024)
    return fail("invalid batch row limit");
  size_t bytes = 0;
  while (rows.size() < max_rows && bytes < MAX_ROW_BYTES) {
    std::vector<Cell> row;
    if (!next(row))
      break;
    bytes += row.size() * sizeof(Cell);
    for (const auto &cell : row)
      bytes += cell.text.size();
    rows.push_back(std::move(row));
  }
  if (!error_.message.empty()) {
    rows.clear();
    return false;
  }
  return !rows.empty();
}
bool Reader::next(std::vector<Cell> &row) {
  if (cancelled_ && cancelled_()) return fail("file SQL query cancelled or timed out");
  if (!error_.message.empty())
    return false;
  if (!inferred_)
    return fail("schema not inferred");
  if (format_ == Format::Parquet)
    return parquet_->next(row, error_);
  if (format_ == Format::Glob) {
    if (discovery_position_ == discovered_.size())
      return false;
    row = {{discovered_[discovery_position_++], Type::Varchar}};
    return true;
  }
  if (!file_.unchanged(error_))
    return false;
  std::vector<Cell> values;
  std::vector<std::string> names;
  if (!raw(values, names)) {
    file_.unchanged(error_);
    return false;
  }
  if (format_ == Format::Csv) {
    if (values.size() != columns_.size())
      return fail("CSV field count differs from schema");
    row = std::move(values);
  } else {
    row.assign(columns_.size(), Cell());
    for (size_t i = 0; i < names.size(); ++i) {
      auto col = std::find_if(columns_.begin(), columns_.end(),
                              [&](const Column &c) { return c.name == names[i]; });
      if (col == columns_.end())
        return fail("input schema changed during query");
      row[col - columns_.begin()] = std::move(values[i]);
    }
  }
  return file_.unchanged(error_);
}

struct CsvWriter::State {
  Options options;
  std::string temporary, target;
  bool committed = false;
#ifdef _WIN32
  HANDLE directory = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE;
#else
  int directory = -1, output = -1;
#endif
  ~State() {
#ifdef _WIN32
    if (output != INVALID_HANDLE_VALUE) {
      if (!committed) {
        FILE_DISPOSITION_INFO disposition = {TRUE};
        SetFileInformationByHandle(output, FileDispositionInfo, &disposition, sizeof(disposition));
      }
      CloseHandle(output);
    }
    if (directory != INVALID_HANDLE_VALUE)
      CloseHandle(directory);
#else
    if (output >= 0)
      close(output);
    if (directory >= 0) {
      if (!committed && !temporary.empty())
        unlinkat(directory, temporary.c_str(), 0);
      close(directory);
    }
#endif
  }
};
CsvWriter::CsvWriter(const Options &options) : state_(new State) { state_->options = options; }
CsvWriter::~CsvWriter() = default;
bool CsvWriter::open(const std::string &path, const std::string &allowed, Error &error) {
  auto fail = [&](const char *s) {
    error.message = s;
    return false;
  };
  if (path.empty() || path.size() > 32000 || path.find('\0') != std::string::npos ||
      allowed.empty() || folded(allowed) == "null" || allowed.find('\0') != std::string::npos)
    return fail("COPY denied by secure_file_priv");
  const auto &o = state_->options;
  if (!o.delimiter || !o.quote || !o.escape || o.delimiter == o.quote || o.delimiter == '\n' ||
      o.delimiter == '\r' || o.quote == '\n' || o.quote == '\r' || o.escape == '\n' ||
      o.escape == '\r' || static_cast<unsigned char>(o.delimiter) >= 128 ||
      static_cast<unsigned char>(o.quote) >= 128 || static_cast<unsigned char>(o.escape) >= 128 ||
      o.null_text.find_first_of(std::string() + o.delimiter + o.quote + "\r\n") !=
          std::string::npos ||
      o.null_text.size() > Reader::MAX_ROW_BYTES || !valid_utf8(o.null_text))
    return fail("invalid COPY CSV options");
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::path target = fs::absolute(fs::path(path), ec);
  if (ec || target.filename().empty() || target.filename() == "." || target.filename() == "..")
    return fail("invalid COPY path");
  std::string actual, root;
#ifdef _WIN32
  if (target.filename().string().find(':') != std::string::npos)
    return fail("COPY alternate streams are not allowed");
  HANDLE allowed_handle =
      CreateFileA(allowed.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (allowed_handle == INVALID_HANDLE_VALUE)
    return fail("cannot open secure_file_priv");
  bool ok = handle_path(allowed_handle, root);
  CloseHandle(allowed_handle);
  state_->directory =
      CreateFileA(target.parent_path().string().c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (!ok || state_->directory == INVALID_HANDLE_VALUE || !handle_path(state_->directory, actual) ||
      (actual != root && !inside(actual, root)))
    return fail("COPY denied by secure_file_priv");
  state_->target = actual + "\\" + target.filename().string();
  if (GetFileAttributesA(state_->target.c_str()) != INVALID_FILE_ATTRIBUTES)
    return fail("COPY target already exists");
  for (unsigned i = 0; i < 100; ++i) {
    state_->temporary = state_->target + ".seekdb-tmp-" + std::to_string(GetCurrentProcessId()) +
                        "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(i);
    state_->output = CreateFileA(state_->temporary.c_str(), GENERIC_WRITE | 0x00010000L, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (state_->output != INVALID_HANDLE_VALUE)
      return true;
    if (GetLastError() != ERROR_FILE_EXISTS)
      break;
  }
#else
  char *resolved = realpath(allowed.c_str(), nullptr);
  if (!resolved)
    return fail("cannot resolve secure_file_priv");
  root = resolved;
  free(resolved);
  state_->directory = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  char buf[32768];
  std::string proc = "/proc/self/fd/" + std::to_string(state_->directory);
  ssize_t n = readlink(proc.c_str(), buf, sizeof(buf));
  if (state_->directory < 0 || n <= 0 || n >= static_cast<ssize_t>(sizeof(buf)))
    return fail("cannot open COPY directory");
  actual.assign(buf, n);
  if (actual != root && !inside(actual, root))
    return fail("COPY denied by secure_file_priv");
  state_->target = target.filename().string();
  struct stat st;
  if (!fstatat(state_->directory, state_->target.c_str(), &st, AT_SYMLINK_NOFOLLOW))
    return fail("COPY target already exists");
  for (unsigned i = 0; i < 100; ++i) {
    state_->temporary =
        state_->target + ".seekdb-tmp-" + std::to_string(getpid()) + "-" + std::to_string(i);
    state_->output = openat(state_->directory, state_->temporary.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (state_->output >= 0)
      return true;
    state_->temporary.clear();
    if (errno != EEXIST)
      break;
  }
#endif
  return fail("cannot create private COPY temporary file");
}
bool CsvWriter::write(const std::vector<Cell> &row, Error &error) {
  std::string line;
  if (row.size() > Reader::MAX_COLUMNS) {
    error.message = "COPY column limit exceeded";
    return false;
  }
  const auto &o = state_->options;
  for (size_t i = 0; i < row.size(); ++i) {
    if (i)
      line += o.delimiter;
    if (row[i].type == Type::Null)
      line += o.null_text;
    else {
      if (!valid_utf8(row[i].text)) {
        error.message = "COPY requires UTF-8 text";
        return false;
      }
      // Quote every non-NULL field, including strings equal to the NULL marker.
      line += o.quote;
      for (char c : row[i].text) {
        if (c == o.quote || (c == o.escape && o.escape != o.quote))
          line += o.escape;
        line += c;
        if (line.size() > Reader::MAX_ROW_BYTES * 2) {
          error.message = "COPY row exceeds limit";
          return false;
        }
      }
      line += o.quote;
    }
    if (line.size() > Reader::MAX_ROW_BYTES * 2) {
      error.message = "COPY row exceeds limit";
      return false;
    }
  }
  line += '\n';
#ifdef _WIN32
  DWORD n = 0;
  if (!WriteFile(state_->output, line.data(), static_cast<DWORD>(line.size()), &n, nullptr) ||
      n != line.size()) {
    error.message = "COPY write failed";
    return false;
  }
#else
  size_t p = 0;
  while (p < line.size()) {
    ssize_t n = ::write(state_->output, line.data() + p, line.size() - p);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      error.message = "COPY write failed";
      return false;
    }
    p += n;
  }
#endif
  return true;
}
bool CsvWriter::commit(Error &error) {
  if (state_->committed) {
    error.message = "COPY already committed";
    return false;
  }
#ifdef _WIN32
  int size = MultiByteToWideChar(CP_ACP, 0, state_->target.c_str(), -1, nullptr, 0);
  if (size <= 0 || !FlushFileBuffers(state_->output)) {
    error.message = "COPY flush failed";
    return false;
  }
  std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + size * sizeof(wchar_t));
  auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(buffer.data());
  rename->ReplaceIfExists = FALSE;
  rename->RootDirectory = nullptr;
  rename->FileNameLength = (size - 1) * sizeof(wchar_t);
  MultiByteToWideChar(CP_ACP, 0, state_->target.c_str(), -1, rename->FileName, size);
  if (!SetFileInformationByHandle(state_->output, FileRenameInfo, rename,
                                  static_cast<DWORD>(buffer.size()))) {
    error.message = "COPY publish failed or target exists";
    return false;
  }
#else
  if (fsync(state_->output) || linkat(state_->directory, state_->temporary.c_str(),
                                      state_->directory, state_->target.c_str(), 0)) {
    error.message = "COPY publish failed or target exists";
    return false;
  }
  unlinkat(state_->directory, state_->temporary.c_str(), 0);
#endif
  state_->committed = true;
  return true;
}
} // namespace file_sql
} // namespace sql
} // namespace oceanbase
