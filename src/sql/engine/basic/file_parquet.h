/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sql/engine/basic/file_scan_reader.h"

namespace oceanbase {
namespace sql {

// Decode a Parquet file held in memory into flat columns and rows.
// Supported: flat schemas, UNCOMPRESSED/SNAPPY, DATA_PAGE(_V2),
// PLAIN and dictionary encodings for BOOLEAN/INT32/INT64/FLOAT/DOUBLE/BYTE_ARRAY.
bool decode_parquet(const std::string &bytes,
                    std::vector<FileColumn> &columns,
                    std::vector<std::vector<FileCell>> &rows,
                    std::string &error);

}  // namespace sql
}  // namespace oceanbase
