/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_scan_reader.h"
#include <cstdio>
#include <string>
#include <filesystem>
using namespace oceanbase::sql;
// Standalone reader driver used by test_file_parquet_reader.py. No SQL engine
// other than seekdb is used in production; Arrow only generates test fixtures.
int run(const std::string &path)
{
  FileScanReader reader;
  FileReadOptions options;
  if (!reader.open(path, "parquet", options) || !reader.infer()) {
    fprintf(stderr, "%s\n", reader.error().c_str()); return 1;
  }
  for (int pass = 0; pass < 2; ++pass) {
    if (!reader.rewind()) return 1;
    std::vector<FileCell> row;
    bool eof = false;
    int64_t count = 0;
    while (true) {
      if (!reader.next(row, eof)) { fprintf(stderr, "%s\n", reader.error().c_str()); return 1; }
      if (eof) break;
      if (pass == 0) {
        for (size_t i = 0; i < row.size(); ++i) {
          if (i) putchar('\t');
          const std::string value = row[i].is_null ? "\\N" : row[i].text;
          fwrite(value.data(), 1, value.size(), stdout);
        }
        putchar('\n');
      }
      ++count;
    }
    if (count != reader.row_count()) return 3;
  }
  return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
  if (argc != 2) return 2;
  const auto path = std::filesystem::path(argv[1]).u8string();
  return run(std::string(reinterpret_cast<const char *>(path.data()), path.size()));
}
#else
int main(int argc, char **argv) { return argc == 2 ? run(argv[1]) : 2; }
#endif
