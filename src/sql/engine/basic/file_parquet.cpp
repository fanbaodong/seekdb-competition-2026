/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_parquet.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace oceanbase {
namespace sql {
namespace {

enum ThriftType {
  TC_TRUE = 1, TC_FALSE = 2, TC_BYTE = 3, TC_I16 = 4, TC_I32 = 5, TC_I64 = 6,
  TC_DOUBLE = 7, TC_BINARY = 8, TC_LIST = 9, TC_SET = 10, TC_MAP = 11, TC_STRUCT = 12
};

enum ParquetType {
  PT_BOOLEAN = 0, PT_INT32 = 1, PT_INT64 = 2, PT_INT96 = 3, PT_FLOAT = 4,
  PT_DOUBLE = 5, PT_BYTE_ARRAY = 6, PT_FIXED = 7
};

struct TVal {
  int type = 0;
  int64_t i = 0;
  double d = 0;
  std::string s;
  std::vector<TVal> list;
  std::vector<std::pair<int, TVal>> fields;

  const TVal *field(int id) const
  {
    for (const auto &item : fields) if (item.first == id) return &item.second;
    return nullptr;
  }
  int64_t get_int(int id, int64_t fallback = 0) const
  {
    const TVal *f = field(id);
    return f ? f->i : fallback;
  }
  const std::string *get_str(int id) const
  {
    const TVal *f = field(id);
    return f ? &f->s : nullptr;
  }
};

class Compact {
public:
  Compact(const uint8_t *data, size_t size, size_t pos = 0)
      : data_(data), size_(size), pos_(pos) {}
  bool ok() const { return !bad_; }
  size_t pos() const { return pos_; }

  bool byte(uint8_t &out)
  {
    if (pos_ >= size_) { bad_ = true; return false; }
    out = data_[pos_++];
    return true;
  }
  bool varint(uint64_t &out)
  {
    out = 0;
    int shift = 0;
    while (true) {
      uint8_t b = 0;
      if (!byte(b)) return false;
      out |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (0 == (b & 0x80)) return true;
      shift += 7;
      if (shift > 63) { bad_ = true; return false; }
    }
  }
  bool zigzag(int64_t &out)
  {
    uint64_t v = 0;
    if (!varint(v)) return false;
    out = static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
    return true;
  }
  bool binary(std::string &out)
  {
    uint64_t n = 0;
    if (!varint(n) || n > size_ - pos_) { bad_ = true; return false; }
    out.assign(reinterpret_cast<const char *>(data_ + pos_), static_cast<size_t>(n));
    pos_ += static_cast<size_t>(n);
    return true;
  }
  bool parse_struct(TVal &out) { return value(TC_STRUCT, out); }
  bool value(int type, TVal &out);

private:
  const uint8_t *data_;
  size_t size_;
  size_t pos_;
  bool bad_ = false;
};

bool Compact::value(int type, TVal &out)
{
  out.type = type;
  switch (type) {
    case TC_TRUE:
      out.i = 1;
      return true;
    case TC_FALSE:
      out.i = 0;
      return true;
    case TC_BYTE: {
      uint8_t b = 0;
      if (!byte(b)) return false;
      out.i = b;
      return true;
    }
    case TC_I16:
    case TC_I32:
    case TC_I64:
      return zigzag(out.i);
    case TC_DOUBLE: {
      if (size_ - pos_ < 8) { bad_ = true; return false; }
      uint64_t bits = 0;
      for (int i = 0; i < 8; ++i) bits |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
      pos_ += 8;
      memcpy(&out.d, &bits, sizeof(out.d));
      return true;
    }
    case TC_BINARY:
      return binary(out.s);
    case TC_LIST:
    case TC_SET: {
      uint8_t header = 0;
      if (!byte(header)) return false;
      uint64_t n = header >> 4;
      const int elem = header & 0x0f;
      if (15 == n && !varint(n)) return false;
      for (uint64_t i = 0; i < n; ++i) {
        TVal item;
        if (!value(elem, item)) return false;
        out.list.push_back(std::move(item));
      }
      return true;
    }
    case TC_MAP: {
      uint64_t n = 0;
      if (!varint(n)) return false;
      if (0 == n) return true;
      uint8_t kv = 0;
      if (!byte(kv)) return false;
      for (uint64_t i = 0; i < n; ++i) {
        TVal key;
        TVal item;
        if (!value(kv >> 4, key) || !value(kv & 0x0f, item)) return false;
      }
      return true;
    }
    case TC_STRUCT: {
      out.type = TC_STRUCT;
      int64_t last = 0;
      while (true) {
        uint8_t header = 0;
        if (!byte(header)) return false;
        if (0 == header) return true;
        const int kind = header & 0x0f;
        const int delta = header >> 4;
        int64_t id = 0;
        if (delta) id = last + delta;
        else if (!zigzag(id)) return false;
        last = id;
        TVal item;
        if (!value(kind, item)) return false;
        out.fields.emplace_back(static_cast<int>(id), std::move(item));
      }
    }
    default:
      bad_ = true;
      return false;
  }
}

uint32_t read_le32(const uint8_t *data)
{
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8)
      | (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t read_le64(const uint8_t *data)
{
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (8 * i);
  return value;
}

bool snappy_decompress(const uint8_t *data, size_t size, std::vector<uint8_t> &out)
{
  size_t pos = 0;
  uint64_t expected = 0;
  int shift = 0;
  while (pos < size) {
    const uint8_t b = data[pos++];
    expected |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (0 == (b & 0x80)) break;
    shift += 7;
    if (shift > 35) return false;
  }
  out.clear();
  if (expected > (1ull << 31)) return false;
  out.reserve(static_cast<size_t>(expected));
  while (pos < size) {
    const uint8_t tag = data[pos++];
    const int kind = tag & 3;
    if (0 == kind) {
      size_t len = tag >> 2;
      if (len < 60) {
        ++len;
      } else {
        const size_t extra = len - 59;
        if (pos + extra > size) return false;
        len = 0;
        for (size_t i = 0; i < extra; ++i) len |= static_cast<size_t>(data[pos + i]) << (8 * i);
        ++len;
        pos += extra;
      }
      if (pos + len > size) return false;
      out.insert(out.end(), data + pos, data + pos + len);
      pos += len;
    } else {
      size_t len = 0;
      size_t offset = 0;
      if (1 == kind) {
        len = ((tag >> 2) & 7) + 4;
        if (pos >= size) return false;
        offset = (static_cast<size_t>(tag >> 5) << 8) | data[pos++];
      } else if (2 == kind) {
        len = (tag >> 2) + 1;
        if (pos + 2 > size) return false;
        offset = static_cast<size_t>(data[pos]) | (static_cast<size_t>(data[pos + 1]) << 8);
        pos += 2;
      } else {
        len = (tag >> 2) + 1;
        if (pos + 4 > size) return false;
        offset = static_cast<size_t>(read_le32(data + pos));
        pos += 4;
      }
      if (0 == offset || offset > out.size()) return false;
      const size_t start = out.size() - offset;
      for (size_t i = 0; i < len; ++i) out.push_back(out[start + i]);
    }
  }
  return out.size() == static_cast<size_t>(expected);
}

bool decode_hybrid(const uint8_t *data, size_t size, int width, size_t count,
                   std::vector<uint32_t> &out, std::string &error)
{
  out.clear();
  if (width <= 0) {
    out.assign(count, 0);
    return true;
  }
  size_t pos = 0;
  while (out.size() < count && pos < size) {
    uint64_t header = 0;
    int shift = 0;
    while (true) {
      if (pos >= size) { error = "truncated parquet level header"; return false; }
      const uint8_t b = data[pos++];
      header |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (0 == (b & 0x80)) break;
      shift += 7;
    }
    if (header & 1) {
      const size_t groups = static_cast<size_t>(header >> 1);
      const size_t bytes = groups * static_cast<size_t>(width);
      if (pos + bytes > size) { error = "truncated bit-packed level run"; return false; }
      const uint8_t *packed = data + pos;
      const size_t total = groups * 8;
      for (size_t i = 0; i < total && out.size() < count; ++i) {
        uint32_t value = 0;
        for (int bit = 0; bit < width; ++bit) {
          const size_t index = i * static_cast<size_t>(width) + static_cast<size_t>(bit);
          value |= static_cast<uint32_t>((packed[index / 8] >> (index % 8)) & 1) << bit;
        }
        out.push_back(value);
      }
      pos += bytes;
    } else {
      const size_t run = static_cast<size_t>(header >> 1);
      const size_t bytes = static_cast<size_t>((width + 7) / 8);
      if (pos + bytes > size) { error = "truncated RLE level run"; return false; }
      uint32_t value = 0;
      for (size_t i = 0; i < bytes; ++i) value |= static_cast<uint32_t>(data[pos + i]) << (8 * i);
      pos += bytes;
      for (size_t i = 0; i < run && out.size() < count; ++i) out.push_back(value);
    }
  }
  if (out.size() < count) out.resize(count, 0);
  return true;
}

std::string format_double(double value)
{
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%.17g", value);
  return std::string(buffer);
}

std::string format_float(float value)
{
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%.9g", value);
  return std::string(buffer);
}

bool decode_plain(const uint8_t *data, size_t size, int type, int64_t count,
                  int64_t fixed_length, std::vector<std::string> &out, std::string &error)
{
  out.clear();
  out.reserve(static_cast<size_t>(std::max<int64_t>(0, count)));
  size_t pos = 0;
  for (int64_t i = 0; i < count; ++i) {
    switch (type) {
      case PT_BOOLEAN: {
        if (0 == (i % 8)) { if (pos >= size) { error = "truncated parquet boolean"; return false; } }
        const uint8_t byte = data[i / 8];
        out.push_back(((byte >> (i % 8)) & 1) ? "1" : "0");
        pos = static_cast<size_t>(i / 8 + 1);
        break;
      }
      case PT_INT32: {
        if (pos + 4 > size) { error = "truncated parquet int32"; return false; }
        const int32_t value = static_cast<int32_t>(read_le32(data + pos));
        pos += 4;
        out.push_back(std::to_string(value));
        break;
      }
      case PT_INT64: {
        if (pos + 8 > size) { error = "truncated parquet int64"; return false; }
        const int64_t value = static_cast<int64_t>(read_le64(data + pos));
        pos += 8;
        out.push_back(std::to_string(value));
        break;
      }
      case PT_FLOAT: {
        if (pos + 4 > size) { error = "truncated parquet float"; return false; }
        const uint32_t bits = read_le32(data + pos);
        pos += 4;
        float value = 0;
        memcpy(&value, &bits, sizeof(value));
        out.push_back(format_float(value));
        break;
      }
      case PT_DOUBLE: {
        if (pos + 8 > size) { error = "truncated parquet double"; return false; }
        const uint64_t bits = read_le64(data + pos);
        pos += 8;
        double value = 0;
        memcpy(&value, &bits, sizeof(value));
        out.push_back(format_double(value));
        break;
      }
      case PT_BYTE_ARRAY: {
        if (pos + 4 > size) { error = "truncated parquet byte array"; return false; }
        const uint32_t length = read_le32(data + pos);
        pos += 4;
        if (pos + length > size) { error = "truncated parquet byte array"; return false; }
        out.emplace_back(reinterpret_cast<const char *>(data + pos), static_cast<size_t>(length));
        pos += length;
        break;
      }
      case PT_FIXED: {
        if (fixed_length <= 0 || pos + static_cast<size_t>(fixed_length) > size) {
          error = "unsupported parquet fixed length array";
          return false;
        }
        out.emplace_back(reinterpret_cast<const char *>(data + pos), static_cast<size_t>(fixed_length));
        pos += static_cast<size_t>(fixed_length);
        break;
      }
      default:
        error = "unsupported parquet physical type";
        return false;
    }
  }
  return true;
}

bool decode_values(const uint8_t *data, size_t size, int type, int encoding, int64_t count,
                   int64_t fixed_length, const std::vector<std::string> &dictionary,
                   std::vector<std::string> &out, std::string &error)
{
  if (0 == encoding) {
    return decode_plain(data, size, type, count, fixed_length, out, error);
  }
  if (2 == encoding || 8 == encoding) {
    if (dictionary.empty() && count > 0) { error = "parquet dictionary missing"; return false; }
    if (count == 0) { out.clear(); return true; }
    if (size < 1) { error = "truncated parquet dictionary page"; return false; }
    const int width = data[0];
    std::vector<uint32_t> indices;
    if (!decode_hybrid(data + 1, size - 1, width, static_cast<size_t>(count), indices, error)) return false;
    out.clear();
    out.reserve(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
      if (indices[i] >= dictionary.size()) { error = "parquet dictionary index out of range"; return false; }
      out.push_back(dictionary[indices[i]]);
    }
    return true;
  }
  if (3 == encoding && PT_BOOLEAN == type) {
    std::vector<uint32_t> values;
    if (!decode_hybrid(data, size, 1, static_cast<size_t>(count), values, error)) return false;
    out.clear();
    for (int64_t i = 0; i < count; ++i) out.push_back(values[i] ? "1" : "0");
    return true;
  }
  error = "unsupported parquet value encoding";
  return false;
}

FileColumnType map_type(int parquet_type)
{
  switch (parquet_type) {
    case PT_BOOLEAN: return FileColumnType::BOOLEAN;
    case PT_INT32: return FileColumnType::BIGINT;
    case PT_INT64: return FileColumnType::BIGINT;
    case PT_FLOAT: return FileColumnType::DOUBLE;
    case PT_DOUBLE: return FileColumnType::DOUBLE;
    default: return FileColumnType::VARCHAR;
  }
}

bool decode_column_chunk(const std::string &bytes, size_t metadata_start, size_t chunk_start,
                         int type, int codec, int64_t num_values, int64_t max_def,
                         std::vector<FileCell> &cells, std::string &error)
{
  cells.clear();
  cells.reserve(static_cast<size_t>(std::max<int64_t>(0, num_values)));
  size_t pos = chunk_start;
  int64_t produced = 0;
  std::vector<std::string> dictionary;
  while (produced < num_values) {
    if (pos >= metadata_start) { error = "parquet page runs past metadata"; return false; }
    Compact reader(reinterpret_cast<const uint8_t *>(bytes.data()), metadata_start, pos);
    TVal header;
    if (!reader.parse_struct(header) || !reader.ok()) { error = "invalid parquet page header"; return false; }
    const size_t body_start = reader.pos();
    const int64_t compressed = header.get_int(3, -1);
    const int64_t page_type = header.get_int(1, -1);
    if (compressed < 0 || body_start + static_cast<size_t>(compressed) > metadata_start) {
      error = "parquet page body out of range";
      return false;
    }
    const uint8_t *body = reinterpret_cast<const uint8_t *>(bytes.data()) + body_start;
    const size_t body_size = static_cast<size_t>(compressed);
    if (2 == page_type) {
      const TVal *dict_header = header.field(7);
      if (!dict_header) { error = "missing parquet dictionary header"; return false; }
      const int64_t count = dict_header->get_int(1, 0);
      std::vector<uint8_t> plain;
      const uint8_t *payload = body;
      size_t payload_size = body_size;
      if (1 == codec) {
        if (!snappy_decompress(body, body_size, plain)) { error = "invalid parquet snappy page"; return false; }
        payload = plain.data();
        payload_size = plain.size();
      } else if (0 != codec) {
        error = "unsupported parquet compression codec";
        return false;
      }
      if (!decode_plain(payload, payload_size, type, count, 0, dictionary, error)) return false;
    } else if (0 == page_type || 3 == page_type) {
      const TVal *page = 0 == page_type ? header.field(5) : header.field(8);
      std::vector<uint8_t> uncompressed;
      const uint8_t *payload = body;
      size_t payload_size = body_size;
      int64_t count = 0;
      int encoding = 0;
      int32_t def_length = 0;
      int32_t rep_length = 0;
      if (0 == page_type) {
        if (!page) { error = "missing parquet data page header"; return false; }
        count = page->get_int(1, 0);
        encoding = static_cast<int>(page->get_int(2, 0));
        if (1 == codec) {
          if (!snappy_decompress(body, body_size, uncompressed)) { error = "invalid parquet snappy page"; return false; }
          payload = uncompressed.data();
          payload_size = uncompressed.size();
        } else if (0 != codec) {
          error = "unsupported parquet compression codec";
          return false;
        }
        if (max_def > 0) {
          if (payload_size < 4) { error = "truncated parquet definition levels"; return false; }
          def_length = static_cast<int32_t>(read_le32(payload));
          if (static_cast<size_t>(def_length) + 4 > payload_size) { error = "truncated parquet definition levels"; return false; }
          payload += 4;
          payload_size -= 4;
        }
      } else {
        (void)page;
        if (!header.field(8)) { error = "missing parquet data page v2 header"; return false; }
        const TVal *v2 = header.field(8);
        count = v2->get_int(1, 0);
        encoding = static_cast<int>(v2->get_int(4, 0));
        def_length = static_cast<int32_t>(v2->get_int(5, 0));
        rep_length = static_cast<int32_t>(v2->get_int(6, 0));
        if (static_cast<size_t>(def_length) + static_cast<size_t>(rep_length) > payload_size) {
          error = "truncated parquet page levels";
          return false;
        }
        const bool is_compressed = v2->field(7) ? v2->get_int(7) != 0 : true;
        const uint8_t *values = body + rep_length + def_length;
        const size_t values_size = body_size - static_cast<size_t>(rep_length) - static_cast<size_t>(def_length);
        if (is_compressed && 1 == codec) {
          if (!snappy_decompress(values, values_size, uncompressed)) { error = "invalid parquet snappy page"; return false; }
          payload = uncompressed.data();
          payload_size = uncompressed.size();
        } else if (0 != codec && is_compressed) {
          error = "unsupported parquet compression codec";
          return false;
        } else {
          payload = values;
          payload_size = values_size;
        }
      }
      std::vector<uint32_t> defs;
      if (max_def > 0) {
        const uint8_t *levels = 0 == page_type ? payload : body + rep_length;
        const size_t levels_size = 0 == page_type ? static_cast<size_t>(def_length) : static_cast<size_t>(def_length);
        if (!decode_hybrid(levels, levels_size, 1, static_cast<size_t>(count), defs, error)) return false;

      }
      int64_t present = count;
      if (max_def > 0) {
        present = 0;
        for (int64_t i = 0; i < count; ++i) if (defs[i] == static_cast<uint32_t>(max_def)) ++present;
      }
      std::vector<std::string> values;
      if (!decode_values(payload, payload_size, type, encoding, present, 0, dictionary, values, error)) return false;
      int64_t used = 0;
      for (int64_t i = 0; i < count; ++i) {
        FileCell cell;
        if (max_def > 0 && defs[i] != static_cast<uint32_t>(max_def)) {
          cell.is_null = true;
        } else if (used < static_cast<int64_t>(values.size())) {
          cell.is_null = false;
          cell.text = values[used++];
        } else {
          error = "parquet page has fewer values than levels";
          return false;
        }
        cells.push_back(cell);
      }
      produced += count;
    } else {
      error = "unsupported parquet page type";
      return false;
    }
    pos = body_start + body_size;
  }
  return true;
}

}  // namespace

bool decode_parquet(const std::string &bytes, std::vector<FileColumn> &columns,
                    std::vector<std::vector<FileCell>> &rows, std::string &error)
{
  columns.clear();
  rows.clear();
  const size_t size = bytes.size();
  if (size < 12 || 0 != memcmp(bytes.data(), "PAR1", 4)
      || 0 != memcmp(bytes.data() + size - 4, "PAR1", 4)) {
    error = "not a parquet file";
    return false;
  }
  const uint32_t metadata_length = read_le32(reinterpret_cast<const uint8_t *>(bytes.data()) + size - 8);
  if (metadata_length > size - 12) {
    error = "invalid parquet metadata length";
    return false;
  }
  const size_t metadata_start = size - 8 - metadata_length;
  Compact reader(reinterpret_cast<const uint8_t *>(bytes.data()), size, metadata_start);
  TVal metadata;
  if (!reader.parse_struct(metadata) || !reader.ok()) {
    error = "invalid parquet metadata";
    return false;
  }
  const TVal *schema = metadata.field(2);
  const TVal *row_groups = metadata.field(4);
  if (!schema || !row_groups) {
    error = "parquet metadata without schema or row groups";
    return false;
  }
  struct SchemaColumn {
    std::string name;
    int type = PT_INT32;
    int64_t max_def = 1;
  };
  std::vector<SchemaColumn> schema_columns;
  bool root = true;
  for (const auto &element : schema->list) {
    if (root) {
      root = false;
      continue;
    }
    const TVal *name = element.field(4);
    const TVal *type = element.field(1);
    const TVal *repetition = element.field(3);
    const TVal *children = element.field(5);
    if (!name || !type) {
      error = "invalid parquet schema element";
      return false;
    }
    if (children && children->i > 0) {
      error = "nested parquet schemas are not supported";
      return false;
    }
    SchemaColumn column;
    column.name = name->s;
    column.type = static_cast<int>(type->i);
    column.max_def = (repetition && 1 == repetition->i) ? 1 : 0;
    schema_columns.push_back(column);
  }
  if (schema_columns.empty()) {
    error = "parquet schema has no columns";
    return false;
  }
  for (const auto &column : schema_columns) {
    FileColumn info;
    info.name = column.name;
    info.type = map_type(column.type);
    info.non_null_count = 0;
    columns.push_back(info);
  }
  int64_t row_count = 0;
  for (const auto &group : row_groups->list) {
    const TVal *chunks = group.field(1);
    const TVal *group_rows = group.field(3);
    if (!chunks || !group_rows) {
      error = "invalid parquet row group";
      return false;
    }
    const int64_t group_row_count = group_rows->i;
    std::vector<std::vector<FileCell>> group_columns;
    group_columns.reserve(chunks->list.size());
    for (size_t index = 0; index < chunks->list.size() && index < schema_columns.size(); ++index) {
      const TVal &chunk = chunks->list[index];
      const TVal *column_meta = chunk.field(3);
      if (!column_meta) {
        error = "parquet column without metadata";
        return false;
      }
      const int type = static_cast<int>(column_meta->get_int(1, schema_columns[index].type));
      const int codec = static_cast<int>(column_meta->get_int(4, 0));
      const int64_t num_values = column_meta->get_int(5, group_row_count);
      int64_t offset = column_meta->get_int(9, -1);
      const int64_t dictionary_offset = column_meta->get_int(11, -1);
      if (dictionary_offset > 0 && (offset < 0 || dictionary_offset < offset)) offset = dictionary_offset;
      if (offset < 0) {
        error = "parquet column without data page offset";
        return false;
      }
      std::vector<FileCell> cells;
      if (!decode_column_chunk(bytes, metadata_start, static_cast<size_t>(offset), type, codec,
                               num_values, schema_columns[index].max_def, cells, error)) {
        return false;
      }
      if (static_cast<int64_t>(cells.size()) < group_row_count) {
        error = "parquet column has fewer values than rows";
        return false;
      }
      for (int64_t i = 0; i < group_row_count; ++i) {
        if (!cells[static_cast<size_t>(i)].is_null) {
          ++columns[index].non_null_count;
        }
      }
      group_columns.push_back(std::move(cells));
    }
    for (int64_t i = 0; i < group_row_count; ++i) {
      std::vector<FileCell> row;
      row.reserve(group_columns.size());
      for (auto &column : group_columns) row.push_back(column[static_cast<size_t>(i)]);
      rows.push_back(std::move(row));
    }
    row_count += group_row_count;
  }
  (void)row_count;

  return true;
}

}  // namespace sql
}  // namespace oceanbase
