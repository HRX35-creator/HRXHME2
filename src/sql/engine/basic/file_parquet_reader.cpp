#include "file_parquet_reader.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace oceanbase {
namespace sql {
namespace file_sql {
namespace parquet_detail {
constexpr size_t LIMIT = 16 * 1024 * 1024;
struct Bad : std::runtime_error {
  explicit Bad(const char *s) : std::runtime_error(s) {}
};
struct Buffer {
  std::vector<unsigned char> data;
  size_t pos = 0;
  unsigned byte() {
    if (pos == data.size())
      throw Bad("truncated Parquet data");
    return data[pos++];
  }
  uint64_t varint() {
    uint64_t n = 0;
    for (unsigned i = 0; i < 10; ++i) {
      unsigned b = byte();
      if (i == 9 && b > 1)
        throw Bad("Parquet integer overflow");
      n |= uint64_t(b & 127) << (7 * i);
      if (!(b & 128))
        return n;
    }
    throw Bad("invalid Parquet varint");
  }
  int64_t integer() {
    uint64_t n = varint();
    return static_cast<int64_t>((n >> 1) ^ (0 - (n & 1)));
  }
  uint64_t little(unsigned n) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i)
      v |= uint64_t(byte()) << (8 * i);
    return v;
  }
  std::string bytes(size_t n) {
    if (n > data.size() - pos)
      throw Bad("truncated Parquet string");
    std::string s(reinterpret_cast<const char *>(data.data() + pos), n);
    pos += n;
    return s;
  }
};
struct Node {
  int id = 0;
  int64_t number = 0;
  std::string text;
  std::vector<Node> children;
  const Node *find(int key) const {
    for (const auto &c : children)
      if (c.id == key)
        return &c;
    return nullptr;
  }
  int64_t n(int key, int64_t fallback = -1) const {
    const auto *p = find(key);
    return p ? p->number : fallback;
  }
};
Node value(Buffer &b, unsigned type, unsigned depth, size_t &budget);
Node structure(Buffer &b, unsigned depth, size_t &budget) {
  Node out;
  int previous = 0;
  for (;;) {
    unsigned h = b.byte();
    if (!h)
      return out;
    int id = h >> 4 ? previous + (h >> 4) : static_cast<int>(b.integer());
    previous = id;
    Node child = value(b, h & 15, depth + 1, budget);
    child.id = id;
    out.children.push_back(std::move(child));
  }
}
Node value(Buffer &b, unsigned type, unsigned depth, size_t &budget) {
  if (depth > 12 || !budget--)
    throw Bad("Parquet metadata exceeds limit");
  Node n;
  switch (type) {
  case 1:
    n.number = 1;
    break;
  case 2:
    n.number = 0;
    break;
  case 3:
    n.number = b.byte();
    break;
  case 4:
  case 5:
  case 6:
    n.number = b.integer();
    break;
  case 7:
    n.number = static_cast<int64_t>(b.little(8));
    break;
  case 8: {
    uint64_t len = b.varint();
    if (len > LIMIT)
      throw Bad("Parquet metadata string too long");
    n.text = b.bytes(static_cast<size_t>(len));
    break;
  }
  case 9:
  case 10: {
    unsigned h = b.byte();
    uint64_t count = h >> 4;
    if (count == 15)
      count = b.varint();
    if (count > 100000 || count > budget)
      throw Bad("Parquet metadata collection exceeds limit");
    for (uint64_t i = 0; i < count; ++i)
      n.children.push_back(value(b, h & 15, depth + 1, budget));
    break;
  }
  case 12:
    n = structure(b, depth, budget);
    break;
  default:
    throw Bad("unsupported Parquet metadata type");
  }
  return n;
}
uint64_t tell(FILE *f) {
#ifdef _WIN32
  auto n = _ftelli64(f);
#else
  auto n = ftello(f);
#endif
  if (n < 0)
    throw Bad("cannot seek Parquet input");
  return static_cast<uint64_t>(n);
}
void seek(FILE *f, uint64_t n) {
  if (n > INT64_MAX)
    throw Bad("Parquet offset overflow");
#ifdef _WIN32
  if (_fseeki64(f, static_cast<int64_t>(n), SEEK_SET))
    throw Bad("cannot seek Parquet input");
#else
  if (fseeko(f, static_cast<off_t>(n), SEEK_SET))
    throw Bad("cannot seek Parquet input");
#endif
}
Buffer read(FILE *f, size_t n) {
  if (n > LIMIT)
    throw Bad("Parquet page exceeds memory limit");
  Buffer b;
  b.data.resize(n);
  if (n && fread(b.data.data(), 1, n, f) != n)
    throw Bad("truncated Parquet file");
  return b;
}
// Raw Snappy blocks, with checked output lengths and overlapping copy support.
Buffer snappy(Buffer in, size_t expected) {
  if (expected > LIMIT || in.varint() != expected)
    throw Bad("invalid Snappy length");
  Buffer out;
  out.data.reserve(expected);
  while (in.pos < in.data.size()) {
    unsigned tag = in.byte(), kind = tag & 3;
    size_t len = 0, offset = 0;
    if (kind == 0) {
      len = tag >> 2;
      if (len >= 60)
        len = static_cast<size_t>(in.little(static_cast<unsigned>(len - 59)));
      ++len;
      if (len > expected - out.data.size() || len > in.data.size() - in.pos)
        throw Bad("invalid Snappy literal");
      out.data.insert(out.data.end(), in.data.begin() + in.pos, in.data.begin() + in.pos + len);
      in.pos += len;
    } else {
      if (kind == 1) {
        len = 4 + ((tag >> 2) & 7);
        offset = ((tag & 224) << 3) | in.byte();
      } else {
        len = 1 + (tag >> 2);
        offset = static_cast<size_t>(in.little(kind == 2 ? 2 : 4));
      }
      if (!offset || offset > out.data.size() || len > expected - out.data.size())
        throw Bad("invalid Snappy copy");
      for (size_t i = 0; i < len; ++i)
        out.data.push_back(out.data[out.data.size() - offset]);
    }
  }
  if (out.data.size() != expected)
    throw Bad("truncated Snappy output");
  return out;
}
std::vector<uint32_t> hybrid(Buffer &b, unsigned width, size_t count) {
  if (width > 32 || count > LIMIT / sizeof(uint32_t))
    throw Bad("Parquet level limit exceeded");
  std::vector<uint32_t> out;
  out.reserve(count);
  while (out.size() < count) {
    uint64_t h = b.varint();
    if (!h)
      throw Bad("invalid Parquet RLE run");
    uint64_t run = h >> 1;
    if (h & 1) {
      if (run > LIMIT / 8 || run * width > b.data.size() - b.pos)
        throw Bad("invalid Parquet packed run");
      uint64_t bits = 0;
      unsigned have = 0;
      for (uint64_t i = 0; i < run * 8; ++i) {
        while (have < width) {
          bits |= uint64_t(b.byte()) << have;
          have += 8;
        }
        uint32_t v = width ? static_cast<uint32_t>(bits & ((uint64_t(1) << width) - 1)) : 0;
        if (width) {
          bits >>= width;
          have -= width;
        }
        if (out.size() < count)
          out.push_back(v);
      }
    } else {
      uint32_t v = static_cast<uint32_t>(b.little((width + 7) / 8));
      if (run > count - out.size())
        throw Bad("Parquet RLE run exceeds value count");
      out.insert(out.end(), static_cast<size_t>(run), v);
    }
  }
  return out;
}
std::string civil(int64_t days) {
  days += 719468;
  int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  unsigned doe = static_cast<unsigned>(days - era * 146097),
           yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp + (mp < 10 ? 3 : -9);
  y += m <= 2;
  if (y < 1 || y > 9999)
    throw Bad("Parquet date outside SQL range");
  char s[32];
  snprintf(s, sizeof(s), "%04d-%02u-%02u", static_cast<int>(y), m, d);
  return s;
}
} // namespace parquet_detail
using namespace parquet_detail;
struct ParquetReader::State {
  struct Schema {
    Column column;
    int physical = 0;
    int converted = -1;
    int unit = 0;
    bool optional = false;
  };
  struct Chunk {
    uint64_t offset = 0, end = 0;
    int codec = 0;
    int64_t count = 0, seen = 0;
    std::vector<Cell> dictionary, page;
    size_t position = 0;
  };
  File *file = nullptr;
  uint64_t size = 0;
  Node footer;
  std::vector<Schema> schema;
  std::vector<Chunk> chunks;
  std::vector<bool> selected;
  size_t group = 0;
  int64_t row = 0, group_rows = 0;
  size_t page_limit() const {
    return std::min<size_t>(LIMIT, 16 * 1024 * 1024 / std::max<size_t>(1, schema.size()));
  }
  std::vector<Cell> plain(Buffer &b, const Schema &s, size_t count) {
    std::vector<Cell> values;
    if (count > page_limit() / sizeof(Cell))
      throw Bad("Parquet page has too many values");
    values.reserve(count);
    unsigned bools = 0;
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i) {
      Cell c;
      c.type = s.column.type;
      switch (s.physical) {
      case 0:
        if (i % 8 == 0)
          bools = b.byte();
        c.text = (bools & (1u << (i % 8))) ? "true" : "false";
        break;
      case 1:
      case 2: {
        int64_t n =
            s.physical == 1 ? static_cast<int32_t>(b.little(4)) : static_cast<int64_t>(b.little(8));
        if (c.type == Type::Date)
          c.text = civil(n);
        else if (c.type == Type::Datetime) {
          int64_t scale = s.unit == 1 ? 1000 : s.unit == 3 ? 1000000000 : 1000000;
          int64_t seconds = n / scale, fraction = n % scale;
          if (fraction < 0) {
            fraction += scale;
            --seconds;
          }
          int64_t day = seconds / 86400, t = seconds % 86400;
          if (t < 0) {
            t += 86400;
            --day;
          }
          char tail[40];
          snprintf(tail, sizeof(tail), " %02d:%02d:%02d.%06d", int(t / 3600), int(t / 60 % 60),
                   int(t % 60),
                   int(fraction * (1000000 / (scale <= 1000000 ? scale : 1000000)) /
                       (scale > 1000000 ? 1000 : 1)));
          c.text = civil(day) + tail;
        } else
          c.text = std::to_string(n);
        break;
      }
      case 4:
      case 5: {
        double d;
        if (s.physical == 4) {
          uint32_t bits = static_cast<uint32_t>(b.little(4));
          float f;
          memcpy(&f, &bits, 4);
          d = f;
        } else {
          uint64_t bits = b.little(8);
          memcpy(&d, &bits, 8);
        }
        if (!std::isfinite(d))
          throw Bad("nonfinite Parquet number");
        std::ostringstream out;
        out << std::setprecision(17) << d;
        c.text = out.str();
        break;
      }
      case 6: {
        uint64_t len = b.little(4);
        if (len > Reader::MAX_ROW_BYTES)
          throw Bad("Parquet string exceeds limit");
        c.text = b.bytes(static_cast<size_t>(len));
        if (!valid_utf8(c.text))
          throw Bad("invalid Parquet UTF-8");
        break;
      }
      default:
        throw Bad("unsupported Parquet physical type");
      }
      bytes += c.text.size();
      if (bytes > page_limit())
        throw Bad("Parquet decoded page exceeds memory limit");
      values.push_back(std::move(c));
    }
    return values;
  }
  void load_group() {
    const Node *groups = footer.find(4);
    if (!groups || group >= groups->children.size()) {
      group_rows = 0;
      return;
    }
    const auto &g = groups->children[group];
    const Node *cols = g.find(1);
    group_rows = g.n(3);
    row = 0;
    if (group_rows < 0 || !cols || cols->children.size() != schema.size())
      throw Bad("invalid Parquet row group");
    chunks.assign(schema.size(), Chunk());
    for (size_t i = 0; i < schema.size(); ++i) {
      if (cols->children[i].find(1) && !cols->children[i].find(1)->text.empty())
        throw Bad("external Parquet column files are unsupported");
      const Node *m = cols->children[i].find(3);
      if (!m)
        throw Bad("missing Parquet column metadata");
      const Node *path = m->find(3);
      if (!path || path->children.size() != 1 || path->children[0].text != schema[i].column.name)
        throw Bad("Parquet column path does not match schema");
      auto &c = chunks[i];
      int64_t off = m->n(11, m->n(9)), len = m->n(7);
      c.count = m->n(5);
      c.codec = static_cast<int>(m->n(4));
      if (m->n(1) != schema[i].physical || c.count != group_rows || off < 4 || len < 0 ||
          uint64_t(off) > size || uint64_t(len) > size - uint64_t(off))
        throw Bad("invalid Parquet column bounds");
      if (c.codec != 0 && c.codec != 1)
        throw Bad("only uncompressed and Snappy Parquet are supported");
      c.offset = off;
      c.end = off + len;
    }
  }
  void page(size_t index) {
    auto &c = chunks[index];
    const auto &s = schema[index];
    while (c.page.empty()) {
      if (c.offset >= c.end)
        throw Bad("missing Parquet data page");
      seek(file->stream(), c.offset);
      size_t look = static_cast<size_t>(std::min<uint64_t>(65536, c.end - c.offset));
      Buffer header = read(file->stream(), look);
      size_t budget = 1000;
      Node h = structure(header, 0, budget);
      int64_t compressed = h.n(3), uncompressed = h.n(2);
      if (compressed < 0 || uncompressed < 0 || uncompressed > static_cast<int64_t>(page_limit()) ||
          uint64_t(compressed) > c.end - c.offset - header.pos)
        throw Bad("invalid Parquet page bounds");
      c.offset += header.pos;
      seek(file->stream(), c.offset);
      Buffer payload = read(file->stream(), static_cast<size_t>(compressed));
      c.offset += compressed;
      int kind = static_cast<int>(h.n(1));
      bool v2 = kind == 3;
      const Node *d = h.find(v2 ? 8 : 5);
      if (kind == 2) {
        if (!h.find(7) || h.find(7)->n(2) != 0)
          throw Bad("unsupported Parquet dictionary");
        if (c.codec == 1)
          payload = snappy(std::move(payload), uncompressed);
        c.dictionary = plain(payload, s, static_cast<size_t>(h.find(7)->n(1)));
        continue;
      }
      if ((kind != 0 && !v2) || !d)
        throw Bad("unsupported Parquet page type");
      int64_t count = d->n(1);
      if (count < 0 || count > group_rows - c.seen || count > page_limit() / sizeof(Cell))
        throw Bad("invalid Parquet page value count");
      std::vector<uint32_t> levels;
      int encoding = static_cast<int>(d->n(v2 ? 4 : 2));
      if (v2) {
        int64_t def = d->n(5), rep = d->n(6);
        if (def < 0 || rep != 0 || uint64_t(def) > payload.data.size() || def > uncompressed)
          throw Bad("invalid Parquet V2 levels");
        Buffer defs;
        defs.data.assign(payload.data.begin(), payload.data.begin() + def);
        if (s.optional)
          levels = hybrid(defs, 1, static_cast<size_t>(count));
        Buffer data;
        data.data.assign(payload.data.begin() + def, payload.data.end());
        if (c.codec == 1 && d->n(7, 1))
          data = snappy(std::move(data), static_cast<size_t>(uncompressed - def));
        payload = std::move(data);
      } else {
        if (c.codec == 1)
          payload = snappy(std::move(payload), uncompressed);
        if (s.optional) {
          if (d->n(3) != 3)
            throw Bad("unsupported Parquet definition encoding");
          uint64_t len = payload.little(4);
          Buffer defs;
          std::string bytes = payload.bytes(static_cast<size_t>(len));
          defs.data.assign(bytes.begin(), bytes.end());
          levels = hybrid(defs, 1, static_cast<size_t>(count));
        }
      }
      if (!s.optional)
        levels.assign(static_cast<size_t>(count), 1);
      size_t present = 0;
      for (auto level : levels) {
        if (level > 1)
          throw Bad("invalid definition level");
        present += level;
      }
      if (s.column.type == Type::Null && present)
        throw Bad("non-NULL value in Parquet NULL column");
      std::vector<Cell> values;
      if (encoding == 0)
        values = plain(payload, s, present);
      else if (encoding == 2 || encoding == 8) {
        unsigned width = payload.byte();
        auto ids = hybrid(payload, width, present);
        size_t bytes = 0;
        for (auto id : ids) {
          if (id >= c.dictionary.size())
            throw Bad("invalid Parquet dictionary index");
          bytes += c.dictionary[id].text.size();
          if (bytes > page_limit())
            throw Bad("Parquet decoded dictionary page exceeds memory limit");
          values.push_back(c.dictionary[id]);
        }
      } else if (encoding == 3 && s.physical == 0) {
        uint64_t len = payload.little(4);
        Buffer bits;
        auto bytes = payload.bytes(static_cast<size_t>(len));
        bits.data.assign(bytes.begin(), bytes.end());
        auto vals = hybrid(bits, 1, present);
        for (auto v : vals) {
          if (v > 1)
            throw Bad("invalid Boolean value");
          values.push_back({v ? "true" : "false", Type::Boolean});
        }
      } else
        throw Bad("unsupported Parquet data encoding");
      size_t j = 0;
      c.page.reserve(static_cast<size_t>(count));
      for (auto level : levels)
        c.page.push_back(level ? std::move(values[j++]) : Cell());
      c.position = 0;
      c.seen += count;
    }
  }
};
ParquetReader::ParquetReader() : state_(new State) {}
ParquetReader::~ParquetReader() = default;
void ParquetReader::project(const std::vector<size_t> &indices) {
  state_->selected.assign(state_->schema.size(), false);
  for (size_t i : indices)
    if (i < state_->selected.size())
      state_->selected[i] = true;
}
bool ParquetReader::infer(File &file, std::vector<Column> &columns, Error &error) {
  try {
    auto &s = *state_;
    s.file = &file;
    FILE *f = file.stream();
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END))
      throw Bad("cannot seek Parquet file");
#else
    if (fseeko(f, 0, SEEK_END))
      throw Bad("cannot seek Parquet file");
#endif
    s.size = tell(f);
    if (s.size < 12)
      throw Bad("invalid Parquet file");
    seek(f, 0);
    if (read(f, 4).bytes(4) != "PAR1")
      throw Bad("invalid Parquet magic");
    seek(f, s.size - 8);
    Buffer tail = read(f, 8);
    uint64_t len = tail.little(4);
    if (tail.bytes(4) != "PAR1" || len > s.size - 12 || len > LIMIT)
      throw Bad("invalid Parquet footer");
    seek(f, s.size - 8 - len);
    Buffer footer = read(f, static_cast<size_t>(len));
    size_t budget = 100000;
    s.footer = structure(footer, 0, budget);
    const Node *schema = s.footer.find(2);
    if (!schema || schema->children.size() < 2 ||
        schema->children.size() > Reader::MAX_COLUMNS + 1 ||
        schema->children[0].n(5) != int64_t(schema->children.size() - 1))
      throw Bad("only flat Parquet schemas are supported");
    for (size_t i = 1; i < schema->children.size(); ++i) {
      const Node &n = schema->children[i];
      State::Schema c;
      const Node *name = n.find(4);
      c.physical = static_cast<int>(n.n(1));
      c.converted = static_cast<int>(n.n(6));
      c.optional = n.n(3) == 1;
      if (!name || name->text.empty() || name->text.size() > 256 ||
          name->text.find('\0') != std::string::npos || !valid_utf8(name->text) || n.n(5, 0) != 0 ||
          n.n(3) < 0 || n.n(3) > 1)
        throw Bad("invalid or nested Parquet schema");
      c.column.name = name->text;
      for (auto &old : columns) {
        std::string a = old.name, b = c.column.name;
        std::transform(a.begin(), a.end(), a.begin(),
                       [](unsigned char x) { return x >= 'A' && x <= 'Z' ? x + 32 : x; });
        std::transform(b.begin(), b.end(), b.begin(),
                       [](unsigned char x) { return x >= 'A' && x <= 'Z' ? x + 32 : x; });
        if (a == b)
          throw Bad("duplicate Parquet column name");
      }
      switch (c.physical) {
      case 0:
        c.column.type = Type::Boolean;
        break;
      case 1:
      case 2:
        c.column.type = Type::Bigint;
        break;
      case 4:
      case 5:
        c.column.type = Type::Double;
        break;
      case 6:
        c.column.type = Type::Varchar;
        break;
      default:
        throw Bad("unsupported Parquet type");
      }
      if (c.converted == 6) {
        if (c.physical != 1)
          throw Bad("invalid Parquet date");
        c.column.type = Type::Date;
      } else if (c.converted == 9 || c.converted == 10) {
        if (c.physical != 2)
          throw Bad("invalid Parquet timestamp");
        c.column.type = Type::Datetime;
        c.unit = c.converted == 9 ? 1 : 2;
      } else if (c.converted >= 0 && c.converted != 0 && !(c.converted >= 15 && c.converted <= 18))
        throw Bad("unsupported Parquet logical type");
      if (const Node *logical = n.find(10)) {
        if (logical->find(6)) {
          if (c.physical != 1)
            throw Bad("invalid Parquet date type");
          c.column.type = Type::Date;
        } else if (const Node *ts = logical->find(8)) {
          const Node *u = ts->find(2);
          if (!u || u->children.size() != 1 || u->children[0].id < 1 || u->children[0].id > 3 ||
              c.physical != 2)
            throw Bad("invalid timestamp unit");
          c.unit = u->children[0].id;
          c.column.type = Type::Datetime;
        } else if (logical->find(11))
          c.column.type = Type::Null;
        else if (logical->find(10) && logical->find(10)->n(2) != 1)
          throw Bad("unsigned Parquet integers are unsupported");
        else if (!logical->find(1) && !logical->find(10))
          throw Bad("unsupported Parquet logical type");
      }
      columns.push_back(c.column);
      s.schema.push_back(c);
    }
    if (!s.footer.find(4))
      throw Bad("missing Parquet row groups");
    int64_t total = 0;
    for (const auto &g : s.footer.find(4)->children) {
      int64_t n = g.n(3);
      if (n < 0 || n > INT64_MAX - total)
        throw Bad("invalid Parquet row count");
      total += n;
    }
    if (total != s.footer.n(3))
      throw Bad("Parquet footer row count mismatch");
    s.selected.assign(s.schema.size(), true);
    s.load_group();
    return file.unchanged(error);
  } catch (const std::exception &e) {
    error.message = e.what();
    return false;
  }
}
bool ParquetReader::rescan(Error &error) {
  try {
    state_->group = 0;
    state_->load_group();
    return state_->file->unchanged(error);
  } catch (const std::exception &e) {
    error.message = e.what();
    return false;
  }
}
bool ParquetReader::next(std::vector<Cell> &row, Error &error) {
  try {
    auto &s = *state_;
    if (!s.file->unchanged(error))
      return false;
    while (s.row == s.group_rows) {
      if (++s.group >= s.footer.find(4)->children.size())
        return false;
      s.load_group();
    }
    row.clear();
    size_t bytes = 0;
    for (size_t i = 0; i < s.schema.size(); ++i) {
      if (!s.selected[i]) {
        row.push_back(Cell());
        continue;
      }
      auto &c = s.chunks[i];
      if (c.position == c.page.size()) {
        c.page.clear();
        s.page(i);
      }
      Cell cell = c.page[c.position++];
      bytes += cell.text.size();
      if (bytes > Reader::MAX_ROW_BYTES)
        throw Bad("Parquet row exceeds limit");
      row.push_back(std::move(cell));
    }
    ++s.row;
    return s.file->unchanged(error);
  } catch (const std::exception &e) {
    error.message = e.what();
    return false;
  }
}
} // namespace file_sql
} // namespace sql
} // namespace oceanbase
