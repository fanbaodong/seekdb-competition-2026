/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_scan_reader.h"
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace oceanbase { namespace sql {
namespace {
std::string lower_ascii(std::string value)
{
  for (char &c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return value;
}
bool valid_utf8(const std::string &value)
{
  size_t i = 0;
  while (i < value.size()) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (c == 0) return false;
    if (c < 0x80) { ++i; continue; }
    size_t extra = 0;
    unsigned cp = 0;
    if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1Fu; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; }
    else return false;
    for (size_t k = 1; k <= extra; ++k) {
      if (i + k >= value.size()) return false;
      const unsigned char cc = static_cast<unsigned char>(value[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if ((1 == extra && cp < 0x80) || (2 == extra && cp < 0x800)
        || (3 == extra && cp < 0x10000) || cp > 0x10FFFF
        || (cp >= 0xD800 && cp <= 0xDFFF) || 0 == cp) return false;
    i += extra + 1;
  }
  return true;
}

void json_skip_ws(const std::string &text, size_t &pos)
{
  while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'
                               || text[pos] == '\n' || text[pos] == '\r')) ++pos;
}

void append_utf8(unsigned cp, std::string &out)
{
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool json_parse_string(const std::string &text, size_t &pos, std::string &out)
{
  if (pos >= text.size() || text[pos] != '"') return false;
  ++pos;
  out.clear();
  while (pos < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[pos++]);
    if (c == '"') return true;
    if (c == '\\') {
      if (pos >= text.size()) return false;
      const char esc = text[pos++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            if (pos >= text.size()) return false;
            const char h = text[pos++];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
            else return false;
          }
          if (code >= 0xD800 && code <= 0xDBFF) {
            if (pos + 6 > text.size() || text[pos] != '\\' || text[pos + 1] != 'u') return false;
            pos += 2;
            unsigned low = 0;
            for (int i = 0; i < 4; ++i) {
              const char h = text[pos++];
              low <<= 4;
              if (h >= '0' && h <= '9') low |= static_cast<unsigned>(h - '0');
              else if (h >= 'a' && h <= 'f') low |= static_cast<unsigned>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') low |= static_cast<unsigned>(h - 'A' + 10);
              else return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) return false;
            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return false;
          }
          if (0 == code) return false;
          append_utf8(code, out);
          break;
        }
        default:
          return false;
      }
    } else if (c < 0x20) {
      return false;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return false;
}

bool json_parse_number(const std::string &text, size_t &pos, FileCell &cell)
{
  const size_t start = pos;
  if (pos < text.size() && text[pos] == '-') ++pos;
  const size_t int_start = pos;
  while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
  if (pos == int_start) return false;
  bool is_double = false;
  if (pos < text.size() && text[pos] == '.') {
    is_double = true;
    ++pos;
    const size_t frac = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    if (pos == frac) return false;
  }
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    is_double = true;
    ++pos;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
    const size_t exp = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    if (pos == exp) return false;
  }
  const std::string number = text.substr(start, pos - start);
  cell.is_null = false;
  if (!is_double) {
    int64_t value = 0;
    const char *begin = number.data();
    const char *end = begin + number.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec == std::errc() && parsed.ptr == end) {
      cell.text = std::to_string(value);
      return true;
    }
  }
  char *stop = nullptr;
  errno = 0;
  const double value = strtod(number.c_str(), &stop);
  if (stop != number.c_str() + number.size() || !std::isfinite(value)) return false;
  char buf[64] = {0};
  snprintf(buf, sizeof(buf), "%.17g", value);
  cell.text = buf;
  return true;
}

bool json_parse_value(const std::string &text, size_t &pos, FileCell &cell)
{
  json_skip_ws(text, pos);
  if (pos >= text.size()) return false;
  const char c = text[pos];
  if (c == '"') {
    cell.is_null = false;
    return json_parse_string(text, pos, cell.text);
  }
  if (c == 't' && text.compare(pos, 4, "true") == 0) {
    pos += 4; cell.is_null = false; cell.text = "true"; return true;
  }
  if (c == 'f' && text.compare(pos, 5, "false") == 0) {
    pos += 5; cell.is_null = false; cell.text = "false"; return true;
  }
  if (c == 'n' && text.compare(pos, 4, "null") == 0) {
    pos += 4; cell.is_null = true; cell.text.clear(); return true;
  }
  if (c == '{' || c == '[') return false;
  return json_parse_number(text, pos, cell);
}

bool json_parse_object(const std::string &text,
                       std::vector<std::pair<std::string, FileCell>> &fields,
                       std::string &error)
{
  fields.clear();
  size_t pos = 0;
  json_skip_ws(text, pos);
  if (pos >= text.size() || text[pos] != '{') {
    error = "JSONL rows must be flat objects";
    return false;
  }
  ++pos;
  json_skip_ws(text, pos);
  if (pos < text.size() && text[pos] == '}') {
    ++pos;
  } else {
    while (true) {
      json_skip_ws(text, pos);
      std::string name;
      if (!json_parse_string(text, pos, name)) {
        error = "invalid JSONL field name";
        return false;
      }
      json_skip_ws(text, pos);
      if (pos >= text.size() || text[pos] != ':') {
        error = "invalid JSONL object";
        return false;
      }
      ++pos;
      FileCell cell;
      if (!json_parse_value(text, pos, cell)) {
        error = "nested JSON objects and arrays are unsupported";
        return false;
      }
      fields.emplace_back(std::move(name), std::move(cell));
      if (fields.size() > FileScanReader::MAX_COLUMNS) {
        error = "too many JSONL fields";
        return false;
      }
      json_skip_ws(text, pos);
      if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
      if (pos < text.size() && text[pos] == '}') { ++pos; break; }
      error = "invalid JSONL object";
      return false;
    }
  }
  json_skip_ws(text, pos);
  if (pos != text.size()) {
    error = "trailing data after JSONL object";
    return false;
  }
  return true;
}
bool digits(const std::string &s, size_t begin, size_t end)
{
  for (size_t i = begin; i < end; ++i) if (s[i] < '0' || s[i] > '9') return false;
  return true;
}
int decimal(const std::string &s, size_t begin, size_t end)
{
  int value = 0;
  for (size_t i = begin; i < end; ++i) value = value * 10 + s[i] - '0';
  return value;
}
bool date_prefix(const std::string &s)
{
  if (s.size() < 10 || s[4] != '-' || s[7] != '-' || !digits(s, 0, 4)
      || !digits(s, 5, 7) || !digits(s, 8, 10)) return false;
  const int y = decimal(s, 0, 4), m = decimal(s, 5, 7), d = decimal(s, 8, 10);
  if (y < 1 || m < 1 || m > 12) return false;
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  const bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  return d >= 1 && d <= days[m - 1] + ((m == 2 && leap) ? 1 : 0);
}
}

bool FileScanReader::fail(const std::string &message)
{
  error_ = path_ + ": line " + std::to_string(line_) + ": " + message;
  return false;
}

bool FileScanReader::canonical_path(const std::string &input, std::string &output, std::string &error)
{
  if (input.empty() || input.find('\0') != std::string::npos) {
    error = "empty path or embedded NUL";
    return false;
  }
  std::error_code ec;
  const auto resolved = std::filesystem::canonical(std::filesystem::u8path(input), ec);
  if (ec) { error = "cannot resolve file: " + ec.message(); return false; }
  if (!std::filesystem::is_regular_file(resolved, ec) || ec) {
    error = "only regular local files are supported";
    return false;
  }
  const std::u8string encoded = resolved.u8string();
  output.assign(reinterpret_cast<const char *>(encoded.data()), encoded.size());
  return true;
}

bool FileScanReader::fingerprint(std::string &value)
{
#ifdef _WIN32
  struct _stat64 st;
  if (_fstat64(_fileno(file_), &st) != 0) return fail("cannot stat open file");
#else
  struct stat st;
  if (fstat(fileno(file_), &st) != 0) return fail("cannot stat open file");
#endif
  std::error_code ec;
  const auto path = std::filesystem::u8path(path_);
  const auto modified = std::filesystem::last_write_time(path, ec);
  if (ec) return fail("file disappeared while scanning");
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size != static_cast<uintmax_t>(st.st_size)) return fail("file changed while scanning");
  value = std::to_string(st.st_dev) + ":" + std::to_string(st.st_ino) + ":"
        + std::to_string(st.st_size) + ":" + std::to_string(st.st_mtime) + ":"
        + std::to_string(modified.time_since_epoch().count());
  return true;
}

bool FileScanReader::open(const std::string &path, const std::string &format, const FileReadOptions &options)
{
  close();
  path_ = path;
  format_ = format;
  options_ = options;
  error_.clear();
  columns_.clear();
  row_count_ = 0;
  line_ = 1;
  if (format != "csv" && format != "jsonl") return fail("unsupported file format");
  if (!options.delimiter || !options.quote || options.delimiter == options.quote
      || options.delimiter == '\n' || options.delimiter == '\r'
      || options.quote == '\n' || options.quote == '\r') return fail("invalid CSV delimiter or quote");
#ifdef _WIN32
  file_ = _wfopen(std::filesystem::u8path(path).c_str(), L"rb");
#else
  file_ = fopen(path.c_str(), "rb");
#endif
  if (!file_) return fail("cannot open file");
  if (!fingerprint(signature_)) { close(); return false; }
  return true;
}

void FileScanReader::close()
{
  if (file_) { fclose(file_); file_ = nullptr; }
}

bool FileScanReader::check_unchanged()
{
  std::string current;
  return file_ && fingerprint(current) && (current == signature_ || fail("file changed while scanning"));
}

bool FileScanReader::csv_record(std::vector<FileCell> &row, bool &eof)
{
  row.clear();
  eof = false;
  FileCell cell;
  bool quoted = false, in_quote = false, closed = false, any = false;
  size_t bytes = 0;
  const auto finish = [&]() -> bool {
    if (!valid_utf8(cell.text)) return fail("invalid UTF-8 or embedded NUL");
    cell.is_null = !quoted && cell.text == options_.null_string;
    row.push_back(std::move(cell));
    cell = FileCell();
    quoted = in_quote = closed = false;
    return row.size() <= MAX_COLUMNS || fail("too many columns");
  };
  while (true) {
    int c = fgetc(file_);
    if (c == EOF) {
      if (ferror(file_)) return fail("file read error");
      if (in_quote) return fail("unterminated quoted CSV field");
      if (!any && row.empty() && cell.text.empty()) { eof = true; return true; }
      return finish();
    }
    any = true;
    if (++bytes > MAX_RECORD_BYTES) return fail("record exceeds 16 MiB");
    if (in_quote) {
      if (c == options_.escape && options_.escape != options_.quote) {
        const int escaped = fgetc(file_);
        if (escaped == EOF) return fail("unterminated escape");
        cell.text.push_back(static_cast<char>(escaped));
      } else if (c == options_.quote) {
        const int following = fgetc(file_);
        if (following == options_.quote && options_.escape == options_.quote) {
          cell.text.push_back(options_.quote);
        } else {
          if (following != EOF) ungetc(following, file_);
          in_quote = false;
          closed = true;
        }
      } else {
        if (c == '\r') {
          const int following = fgetc(file_);
          if (following != '\n' && following != EOF) ungetc(following, file_);
          c = '\n';
        }
        if (c == '\n') ++line_;
        cell.text.push_back(static_cast<char>(c));
      }
    } else if (c == options_.delimiter) {
      if (!finish()) return false;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r') {
        const int following = fgetc(file_);
        if (following != '\n' && following != EOF) ungetc(following, file_);
      }
      ++line_;
      return finish();
    } else if (c == options_.quote && cell.text.empty() && !closed) {
      quoted = in_quote = true;
    } else if (closed || c == options_.quote) {
      return fail("unexpected character around quoted CSV field");
    } else {
      cell.text.push_back(static_cast<char>(c));
    }
    if (cell.text.size() > MAX_FIELD_BYTES) return fail("field exceeds 1 MiB");
  }
}

bool FileScanReader::json_record(std::vector<std::string> &names, std::vector<FileCell> &row, bool &eof)
{
  names.clear(); row.clear(); eof = false;
  std::string text;
  while (true) {
    const int c = fgetc(file_);
    if (c == EOF) {
      if (ferror(file_)) return fail("file read error");
      if (text.empty()) { eof = true; return true; }
      break;
    }
    if (c == '\n') { ++line_; if (text.empty() || text == "\r") { text.clear(); continue; } break; }
    text.push_back(static_cast<char>(c));
    if (text.size() > MAX_RECORD_BYTES) return fail("JSONL record exceeds 16 MiB");
  }
  std::vector<std::pair<std::string, FileCell>> fields;
  std::string error;
  if (!json_parse_object(text, fields, error)) return fail(error);
  if (fields.size() > MAX_COLUMNS) return fail("too many JSONL fields");
  std::set<std::string> seen;
  for (auto &field : fields) {
    const std::string &name = field.first;
    if (name.empty() || !valid_utf8(name) || !seen.insert(lower_ascii(name)).second) {
      return fail("empty or duplicate JSONL field name");
    }
    FileCell &cell = field.second;
    if (cell.text.size() > MAX_FIELD_BYTES || !valid_utf8(cell.text)) {
      return fail("invalid or oversized JSONL field");
    }
    names.push_back(name);
    row.push_back(std::move(cell));
  }
  return true;
}

bool FileScanReader::read_record(std::vector<std::string> &names, std::vector<FileCell> &row, bool &eof)
{
  return format_ == "csv" ? csv_record(row, eof) : json_record(names, row, eof);
}

bool FileScanReader::skip_header()
{
  // A UTF-8 BOM belongs to the file, not the first field.
  unsigned char bom[3];
  const size_t count = fread(bom, 1, sizeof(bom), file_);
  if (!(count == 3 && bom[0] == 0xef && bom[1] == 0xbb && bom[2] == 0xbf)) {
    if (fseek(file_, 0, SEEK_SET) != 0) return fail("cannot rewind file");
  }
  if (format_ == "csv" && options_.header) {
    std::vector<FileCell> row; bool eof = false;
    if (!csv_record(row, eof)) return false;
    if (eof) return fail("CSV header is missing");
  }
  return true;
}

bool FileScanReader::rewind()
{
  if (!check_unchanged() || fseek(file_, 0, SEEK_SET) != 0) return fail("cannot rewind unchanged file");
  clearerr(file_); line_ = 1;
  return skip_header();
}

FileColumnType FileScanReader::infer_type(const std::string &text)
{
  const std::string folded = lower_ascii(text);
  if (folded == "true" || folded == "false") return FileColumnType::BOOLEAN;
  int64_t integer = 0;
  const char *begin = text.data(), *end = begin + text.size();
  if (begin != end && *begin == '+') ++begin;
  const auto parsed = std::from_chars(begin, end, integer);
  if (begin != end && parsed.ec == std::errc() && parsed.ptr == end) return FileColumnType::BIGINT;
  if (!text.empty() && text.find_first_of(".eE") != std::string::npos
      && text.find_first_not_of("0123456789+-.eE") == std::string::npos) {
    char *stop = nullptr;
    errno = 0;
    const double number = strtod(text.c_str(), &stop);
    if (stop == end && errno != ERANGE && std::isfinite(number)) return FileColumnType::DOUBLE;
  }
  if (date_prefix(text)) {
    if (text.size() == 10) return FileColumnType::DATE;
    if ((text.size() == 19 || (text.size() >= 21 && text.size() <= 26 && text[19] == '.' && digits(text, 20, text.size())))
        && (text[10] == ' ' || text[10] == 'T') && text[13] == ':' && text[16] == ':'
        && digits(text, 11, 13) && digits(text, 14, 16) && digits(text, 17, 19)
        && decimal(text, 11, 13) < 24 && decimal(text, 14, 16) < 60 && decimal(text, 17, 19) < 60) return FileColumnType::DATETIME;
  }
  return FileColumnType::VARCHAR;
}

FileColumnType FileScanReader::widen(FileColumnType left, FileColumnType right)
{
  if (left == FileColumnType::NIL) return right;
  if (right == FileColumnType::NIL || left == right) return left;
  const auto numeric = [](FileColumnType t) { return t == FileColumnType::BOOLEAN || t == FileColumnType::BIGINT || t == FileColumnType::DOUBLE; };
  if (numeric(left) && numeric(right)) return (left == FileColumnType::DOUBLE || right == FileColumnType::DOUBLE) ? FileColumnType::DOUBLE : FileColumnType::BIGINT;
  if ((left == FileColumnType::DATE && right == FileColumnType::DATETIME)
      || (right == FileColumnType::DATE && left == FileColumnType::DATETIME)) return FileColumnType::DATETIME;
  return FileColumnType::VARCHAR;
}

bool FileScanReader::infer()
{
  columns_.clear(); row_count_ = 0;
  if (!file_ || fseek(file_, 0, SEEK_SET) != 0) return fail("file is not open");
  unsigned char bom[3];
  const size_t count = fread(bom, 1, sizeof(bom), file_);
  if (!(count == 3 && bom[0] == 0xef && bom[1] == 0xbb && bom[2] == 0xbf)) fseek(file_, 0, SEEK_SET);
  std::vector<FileCell> row;
  std::vector<std::string> names;
  std::map<std::string, size_t> name_to_index;
  bool eof = false;
  if (format_ == "csv" && options_.header) {
    if (!csv_record(row, eof) || eof) return fail("CSV header is missing");
    for (size_t i = 0; i < row.size(); ++i) {
      std::string name = row[i].text.empty() ? "column_" + std::to_string(i) : row[i].text;
      const std::string base = name;
      for (size_t suffix = 2; name_to_index.count(lower_ascii(name)); ++suffix) name = base + "__" + std::to_string(suffix);
      name_to_index[lower_ascii(name)] = columns_.size();
      columns_.push_back({name, FileColumnType::NIL, 0});
    }
  }
  while (true) {
    if (!read_record(names, row, eof)) return false;
    if (eof) break;
    if (format_ == "csv") {
      if (columns_.empty() && !options_.header) {
        for (size_t i = 0; i < row.size(); ++i) columns_.push_back({"column_" + std::to_string(i), FileColumnType::NIL, 0});
      }
      if (row.size() != columns_.size()) return fail("CSV field count differs from schema");
      for (size_t i = 0; i < row.size(); ++i) if (!row[i].is_null) {
        columns_[i].type = widen(columns_[i].type, infer_type(row[i].text));
        ++columns_[i].non_null_count;
      }
    } else {
      for (size_t i = 0; i < names.size(); ++i) {
        const std::string key = lower_ascii(names[i]);
        if (!name_to_index.count(key)) {
          if (columns_.size() >= MAX_COLUMNS) return fail("too many union schema columns");
          name_to_index[key] = columns_.size();
          columns_.push_back({names[i], FileColumnType::NIL, 0});
        }
        FileColumn &column = columns_[name_to_index[key]];
        if (!row[i].is_null) { column.type = widen(column.type, infer_type(row[i].text)); ++column.non_null_count; }
      }
    }
    ++row_count_;
  }
  if (columns_.empty()) return fail("cannot infer an empty schema");
  for (auto &column : columns_) if (column.type == FileColumnType::NIL) column.type = FileColumnType::VARCHAR;
  return rewind();
}

bool FileScanReader::next(std::vector<FileCell> &row, bool &eof)
{
  if (!check_unchanged()) return false;
  std::vector<std::string> names;
  std::vector<FileCell> values;
  if (!read_record(names, values, eof)) return false;
  if (eof) return check_unchanged();
  if (format_ == "csv") {
    if (values.size() != columns_.size()) return fail("CSV field count differs from schema");
    row = std::move(values);
  } else {
    row.assign(columns_.size(), FileCell());
    for (size_t i = 0; i < names.size(); ++i) {
      size_t j = 0;
      while (j < columns_.size() && lower_ascii(columns_[j].name) != lower_ascii(names[i])) ++j;
      if (j == columns_.size()) return fail("JSONL schema changed after inference");
      row[j] = std::move(values[i]);
    }
  }
  return true;
}

} }
