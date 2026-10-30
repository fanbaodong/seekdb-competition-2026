# seekdb-file-sql

一个用于 **seekdb 文件 SQL** 的 Agent Skill：让智能体直接用 SQL 查询本地
CSV、JSONL、Parquet 文件，无需建表或导入。

## 能力

- `read_csv` / `read_json` / `read_ndjson` / `read_parquet`：把文件当表查
- `glob`：列出目录中的文件
- `DESCRIBE SELECT ...`：查看推断出的列名与类型
- `COPY (...) TO '...'`：把查询结果导出为 CSV

## 使用

1. 把本目录（含 `SKILL.md`）作为 Skill 加载到你的 Agent；
2. 在 seekdb 会话中打开开关：`SET SESSION enable_file_sql = ON;`
3. 直接让 Agent 分析文件，例如：
   “统计 /data/orders.csv 里每个城市的订单数和金额，前 10 名”。

## 目录

```
SKILL.md                        Skill 主文件（名称、适用场景、工作流、示例）
references/file-sql-cheatsheet.md  函数/选项/类型/错误速查
scripts/run_file_sql.py         执行单条文件 SQL 的小工具
```

## 说明

- 只读访问，不修改原始文件；
- 文件需位于 seekdb 服务端 `secure_file_priv` 允许的目录；
- 不需要在 Skill 中填写任何密钥。

## License

Apache-2.0
