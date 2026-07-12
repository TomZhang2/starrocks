# Apache Druid 电商用户行为 Demo

1000 万条电商用户行为日志的建表 + 导入 + 查询完整方案。

## 文件清单

| 文件 | 说明 |
|---|---|
| `user_events_ingest.json` | Druid ingestion spec（建表 + 导入规范） |
| `gen_user_events.py` | 数据生成脚本（Python 标准库，无需安装依赖） |

## 数据模型

**dataSource**: `user_events`

| 字段 | 类型 | Druid 角色 | 说明 |
|---|---|---|---|
| `event_time` | timestamp | `__time` | 事件发生时间 (ISO8601) |
| `user_id` | string | 维度 | 用户ID, 如 `U0421783` |
| `device` | string | 维度 | `mobile` / `pc` / `tablet` |
| `os` | string | 维度 | `iOS` / `Android` / `Windows` / `macOS` |
| `channel` | string | 维度 | `organic` / `ad` / `push` / `email` / `social` |
| `event_type` | string | 维度 | `view` / `click` / `cart` / `order` / `pay` |
| `product_id` | string | 维度 | 商品ID, 如 `P03421` |
| `product_cat` | string | 维度 | `electronics` / `clothing` / `food` / `books` / `home` |
| `country` | string | 维度 | `CN` / `US` / `JP` / `KR` / `DE` / `UK` / `FR` |
| `province` | string | 维度 | 中国省份, 非中国留空 |
| `amount` | double | 度量 (doubleSum) | 金额 |
| `quantity` | long | 度量 (longSum) | 数量 |
| - | - | 度量 (count) | `events` 计数 |
| - | - | 度量 (thetaSketch) | `uv` 去重用户数 |

**建模要点**:
- `rollup: true` — 相同维度+分钟桶的行合并, 1000 万可压到 300-500 万
- `queryGranularity: minute` — 分钟级查询粒度
- `segmentGranularity: DAY` — 按天分 segment
- `thetaSketch` for UV — 独立访客估算, 高效可合并

---

## 使用步骤

### 1. 生成数据

```bash
# 默认生成 1000 万行到 /data/user_events/
python3 gen_user_events.py

# 或自定义输出目录和行数
python3 gen_user_events.py --output ./data --rows 1000000
```

输出 10 个文件, 每个约 100 万行:
```
events_part_01.json
events_part_02.json
...
events_part_10.json
```

### 2. 修改 spec 中的路径

编辑 `user_events_ingest.json`, 把 `baseDir` 改为你的实际数据目录:

```json
"inputSource": {
  "type": "local",
  "baseDir": "/data/user_events/",
  "filter": "*.json"
}
```

> 如果数据在 S3/HDFS, 把 `inputSource.type` 改为 `s3` / `hdfs`, 并调整对应配置。

### 3. 提交导入任务

```bash
# 提交
curl -X POST "http://localhost:8888/druid/indexer/v1/task" \
  -H "Content-Type: application/json" \
  -d @user_events_ingest.json

# 返回 task_id, 查看状态
curl "http://localhost:8888/druid/indexer/v1/task/<task_id>/status" | jq
```

或在 Druid Web Console (`http://localhost:8888/unified-console.html`) → Load data 手动提交。

预计导入 3-8 分钟。

### 4. 验证查询

在 Druid Web Console 的 Query 页面执行 SQL:

#### 总行数
```sql
SELECT COUNT(*) AS rows, SUM("events") AS total_events
FROM user_events
WHERE __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08';
```

#### 每小时 PV / UV
```sql
SELECT
  FLOOR(__time TO HOUR) AS hour,
  SUM("events") AS pv,
  THETA_SKETCH_ESTIMATE(DS_THETA("uv")) AS uv
FROM user_events
WHERE __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-02'
GROUP BY 1
ORDER BY 1;
```

#### 漏斗分析
```sql
SELECT
  event_type,
  SUM("events") AS cnt,
  SUM("sum_amount") AS total_amount
FROM user_events
WHERE __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
GROUP BY 1
ORDER BY 1;
```

#### 商品热度 TOP 10
```sql
SELECT product_id, product_cat, SUM("events") AS views
FROM user_events
WHERE event_type = 'view'
  AND __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
GROUP BY 1, 2
ORDER BY 3 DESC
LIMIT 10;
```

#### 地域分布
```sql
SELECT country, province, SUM("events") AS events,
       THETA_SKETCH_ESTIMATE(DS_THETA("uv")) AS uv
FROM user_events
WHERE country = 'CN'
  AND __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
GROUP BY 1, 2
ORDER BY 3 DESC;
```

---

## 调优参数

| 参数 | 值 | 说明 |
|---|---|---|
| `maxRowsPerSegment` | 5,000,000 | 每 segment 约 500MB |
| `maxRowsInMemory` | 100,000 | 内存缓冲行数 |
| `maxNumConcurrentSubTasks` | 4 | 并行子任务数, 按 MiddleManager 容量调 |
| `segmentGranularity` | DAY | 按天分 segment |
| `queryGranularity` | minute | 分钟级查询粒度 |
| `rollup` | true | 预聚合, 压缩数据 |
