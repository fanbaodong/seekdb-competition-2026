# seekdb File SQL 速查

## 表函数

| 函数 | 格式 | 说明 |
| --- | --- | --- |
| `read_csv(path [, options])` | CSV | 默认首行为表头 |
| `read_json(path, format = 'newline_delimited')` | JSONL/NDJSON | 每行一个 JSON 对象，自动并集 schema |
| `read_ndjson(path, ...)` | 同上 | `read_json` 的别名 |
| `read_parquet(path)` | Parquet | 支持 PLAIN / 字典编码、SNAPPY、DATA_PAGE(_V2) |
| `glob(pattern)` | 目录列表 | 只匹配单层文件名，返回一列 `file` |

## 选项（CSV）

| 选项 | 取值 | 默认 |
| --- | --- | --- |
| `header` | `true` / `false` / `1` / `0` | `true` |
| `delim` / `delimiter` | 单字符 | `,` |
| `quote` | 单字符 | `"` |
| `escape` | 单字符 | `\` |
| `nullstr` | 字符串 | 空（空字段即 NULL） |
| `sample_size` | `-1` 表示全量推断 | 抽样 |

JSONL 只接受 `format = 'newline_delimited'`；Parquet 不接受选项。

## 类型映射

| 文件类型 | SQL 类型 |
| --- | --- |
| BOOLEAN（`true/false`） | `BOOLEAN` |
| INT32 / INT64 / 整数文本 | `BIGINT` |
| FLOAT / DOUBLE | `DOUBLE` |
| DATE（`YYYY-MM-DD`） | `DATE` |
| DATETIME（`YYYY-MM-DD HH:MM:SS`，也接受 `T` 分隔） | `DATETIME` |
| BYTE_ARRAY / 其他文本 | `VARCHAR` |

列全为非空时 `DESCRIBE` 的 `null` 列为 `NO`，否则 `YES`。

## DESCRIBE 输出

```
column_name  column_type  null  key  default  extra
id           BIGINT       NO    NULL NULL     NULL
...
```

## COPY 导出

```sql
COPY (SELECT ...) TO '/path/out.csv'
  (format csv, header, nullstr 'NULL');
```

- 目标文件必须**不存在**（存在会报错，避免误覆盖）。
- 输出使用逗号分隔、按需双引号包裹、`\n` 换行。
- 导出后可用 `read_csv('/path/out.csv')` 立即回读校验。

## 性能建议

- 只选需要的列（投影下推），避免 `SELECT *`。
- 大文件优先 Parquet；JSONL 字段越稳定越快。
- 聚合优先在 SQL 里做（`COUNT/SUM/MIN/MAX`），不要拉全量到客户端。
- 需要多次查询同一文件时，考虑物化到表里再建索引。
