/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_scan_reader.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace oceanbase::sql;
#define REQUIRE(condition) do { if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::abort(); } } while (0)

int main()
{
  const auto root = std::filesystem::temp_directory_path() /
      ("seekdb_file_reader_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directory(root));
  const auto utf8 = [](const std::filesystem::path &path) {
    const auto encoded = path.u8string();
    return std::string(reinterpret_cast<const char *>(encoded.data()), encoded.size());
  };
  const auto write = [&](const char *name, const std::string &data) {
    std::ofstream out(root / name, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    REQUIRE(out.good());
    return utf8(root / name);
  };
  FileScanReader reader;
  FileReadOptions options;
  std::vector<FileCell> row;
  bool eof = false;
  const auto csv = write("records.csv", "\xef\xbb\xbfid,name,id,day\r\n1,\"first\r\nsecond\",2,2024-02-29\r\n3,\"a\"\"b\",4,\r\n");
  REQUIRE(reader.open(csv, "csv", options));
  REQUIRE(reader.infer());
  REQUIRE(reader.row_count() == 2 && reader.columns().size() == 4);
  REQUIRE(reader.columns()[2].name == "id__2");
  REQUIRE(reader.columns()[3].type == FileColumnType::DATE);
  REQUIRE(reader.next(row, eof) && !eof && row[1].text == "first\nsecond");
  REQUIRE(reader.next(row, eof) && !eof && row[1].text == "a\"b" && row[3].is_null);
  REQUIRE(reader.next(row, eof) && eof);
  REQUIRE(reader.rewind() && reader.next(row, eof) && row[0].text == "1");
  const auto json = write("records.jsonl", "{\"id\":1,\"value\":null}\n{\"value\":2.5,\"flag\":true}\n");
  REQUIRE(reader.open(json, "jsonl", options) && reader.infer());
  REQUIRE(reader.columns().size() == 3 && reader.columns()[1].type == FileColumnType::DOUBLE);
  REQUIRE(reader.next(row, eof) && row[1].is_null && row[2].is_null);
  REQUIRE(reader.next(row, eof) && row[0].is_null && row[2].text == "true");
  REQUIRE(reader.open(write("bad.csv", "a,b\n1\n"), "csv", options));
  REQUIRE(!reader.infer());
  REQUIRE(reader.open(write("bad_quote.csv", "a\n\"unterminated"), "csv", options));
  REQUIRE(!reader.infer());
  REQUIRE(reader.open(write("bad.jsonl", "{\"x\":{\"nested\":1}}\n"), "jsonl", options));
  REQUIRE(!reader.infer());
  REQUIRE(reader.open(write("duplicate.jsonl", "{\"x\":1,\"X\":2}\n"), "jsonl", options));
  REQUIRE(!reader.infer());
  REQUIRE(FileScanReader::infer_type("9223372036854775807") == FileColumnType::BIGINT);
  REQUIRE(FileScanReader::infer_type("9223372036854775808") == FileColumnType::VARCHAR);
  REQUIRE(FileScanReader::infer_type("2023-02-29") == FileColumnType::VARCHAR);
  REQUIRE(FileScanReader::infer_type("2024-02-29T23:59:59.123456") == FileColumnType::DATETIME);
  REQUIRE(FileScanReader::infer_type("1e9999") == FileColumnType::VARCHAR);
  const auto changed = write("changed.csv", "x\n1\n");
  REQUIRE(reader.open(changed, "csv", options) && reader.infer());
  write("changed.csv", "x\n10000\n");
  REQUIRE(!reader.next(row, eof));
  reader.close();
  std::string canonical, error;
  REQUIRE(!FileScanReader::canonical_path(utf8(root), canonical, error));
  REQUIRE(FileScanReader::canonical_path(csv, canonical, error));
  REQUIRE(FileScanReader::canonical_glob(utf8(root / "*.csv"), canonical, error));
  REQUIRE(reader.open(canonical, "glob", options) && reader.infer());
  int64_t files = 0;
  while (reader.next(row, eof) && !eof) {
    REQUIRE(row.size() == 1 && !row[0].is_null && row[0].text.find(".csv") != std::string::npos);
    ++files;
  }
  REQUIRE(eof && files == 4);
  REQUIRE(reader.rewind() && reader.next(row, eof) && !eof);
  reader.close();
  // Root-directory patterns must retain the filesystem root when canonicalized.
  const auto root_pattern = root.root_path() / "__seekdb_no_such_file__*";
  REQUIRE(FileScanReader::canonical_glob(utf8(root_pattern), canonical, error));
  REQUIRE(std::filesystem::u8path(canonical).parent_path() == root.root_path());
  REQUIRE(std::filesystem::canonical(root).parent_path() == std::filesystem::canonical(std::filesystem::temp_directory_path()));
  std::filesystem::remove_all(root);
  puts("file_scan_reader: PASS");
  return 0;
}
