---
name: seekdb-file-sql
description: 用 seekdb 的文件 SQL 直接查询本地 CSV、JSONL、Parquet 文件（read_csv、read_json、glob、read_parquet、DESCRIBE、COPY），无需建表导入。Use for local data-file exploration, aggregation and export with SQL.
license: Apache-2.0
version: 1.0.0
metadata:
  author: fanbaodong
  version: "1.0.0"
  tags: ["seekdb", "sql", "csv", "jsonl", "parquet", "data-analysis", "oceanbase"]
---

# seekdb File SQL

## 何时使用

当用户希望用 SQL 分析**本地文件**而不是数据库表时使用本 Skill，典型场景：

- “帮我看一下这个 CSV 有多少行、字段类型是什么”
- “按城市统计这份 JSONL 里的订单金额”
- “把这两份 Parquet 关联起来看看”
- “把查询结果导出成 CSV 发给同事”
- “这批数据文件都有哪些列、有没有空值”

适用文件：`.csv`、`.jsonl` / `.ndjson`、`.parquet`。单个文件即可，不需要建表、不需要导入。

## 前置条件

1. 有一个可连接的 seekdb（OceanBase seekdb）实例，并且当前会话可以执行 SQL。
2. 文件必须位于服务端 `secure_file_priv` 允许的目录下（默认 `/`，生产环境通常是某个数据目录）。
3. 会话开关默认关闭，查询前先打开：

```sql
SET SESSION enable_file_sql = ON;
```

如果返回 `1235`（Not supported feature）说明开关没开；返回 `1227` 说明路径不在 `secure_file_priv` 内。

## 工作流

1. **确认路径**：向用户确认文件的**服务端绝对路径**（不是本机客户端路径）。
2. **看结构**：先用 `DESCRIBE` 看推断出的列名和类型，避免猜字段。
3. **查询/聚合**：用普通 SQL（`WHERE`、`GROUP BY`、`ORDER BY`、窗口函数）直接查文件表。
4. **导出**：需要交付结果时用 `COPY (...) TO '...'` 写出 CSV。
5. **收尾**：把关键结论（行数、字段含义、异常值）用简短文字说明给用户。

## 常用语句

```sql
-- 打开开关
SET SESSION enable_file_sql = ON;

-- 看 schema（列名、类型、是否可空）
DESCRIBE SELECT * FROM read_csv('/data/orders.csv');

-- 直接查询，表函数可当普通表用
SELECT city, COUNT(*) AS orders, ROUND(SUM(amount), 2) AS revenue
FROM read_csv('/data/orders.csv', header = true)
GROUP BY city
ORDER BY revenue DESC
LIMIT 10;

-- JSONL（每行一个对象，自动取并集 schema）
SELECT id, user.name AS user_name, amount
FROM read_json('/data/events.jsonl', format = 'newline_delimited') f
WHERE amount > 100;

-- Parquet
SELECT COUNT(*) AS rows, MIN(ts) AS first_ts, MAX(ts) AS last_ts
FROM read_parquet('/data/events.parquet');

-- 列文件
SELECT substring_index(file, '/', -1) AS name
FROM glob('/data/*.csv')
ORDER BY name;

-- 导出结果
COPY (SELECT id, city, amount FROM read_csv('/data/orders.csv') ORDER BY id)
  TO '/data/orders_export.csv'
  (format csv, header, nullstr 'NULL');
```

## 类型推断与 NULL

- CSV 默认 `header = true`；重复列名会自动加后缀（如 `id__2`）。
- 类型按样本推断：整数 → `BIGINT`，小数 → `DOUBLE`，`true/false` → `BOOLEAN`，
  `YYYY-MM-DD` → `DATE`，带时间 → `DATETIME`，其余 → `VARCHAR`。
- 空字段为 `NULL`（可用 `nullstr` 指定自定义空值标记）。
- 需要全量推断时加 `sample_size = -1`（默认抽样）。
- JSONL 缺失字段补 `NULL`，不同类型按并集规则放宽。

## 错误处理

| 错误 | 含义 | 处理 |
| --- | --- | --- |
| `1235` | 文件 SQL 开关未打开 | `SET SESSION enable_file_sql = ON` |
| `1227` | 路径不在 `secure_file_priv` 内 | 换到允许目录，或让 DBA 调整 |
| `1064` | 语法错误（常见于选项写法） | 选项写成 `header = true`、`format = 'newline_delimited'` |
| `1231` | 选项值不合法 | 检查 `delim`/`quote`/`escape` 是否为单字符 |
| 文件不存在/无权限 | 路径或权限问题 | 用服务端路径并确认文件可读 |

## 安全约定

- 只读：不要用本 Skill 修改或删除用户原始文件。
- 路径来自用户输入时，先确认是**服务端**路径，并避免把未校验的字符串拼进 SQL。
- 导出文件写到用户指定且允许的目录，不要覆盖已有文件（`COPY` 目标已存在会报错，先确认）。

## 参考

- `references/file-sql-cheatsheet.md`：函数、选项、类型规则完整速查。
- `scripts/run_file_sql.py`：通过 MySQL 协议执行一条文件 SQL 的小工具，便于快速验证。

<!-- OB-S:0mY5n1SuGvQYIZhFUf3Zig -->
