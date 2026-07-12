# App + Web 双表 Demo (验证 UNION / JOIN)

设计两个表（App 端 + Web 端），共享部分字段和用户池，用于验证 UNION ALL、JOIN、跨端对比等场景。

## 文件清单

| 文件 | 说明 |
|---|---|
| `app_events_ingest.json` | App 表 ingestion spec |
| `web_events_ingest.json` | Web 表 ingestion spec |
| `gen_union_events.py` | 双表数据生成脚本 |

---

## 表设计

### 字段对比

| 字段 | App 表 | Web 表 | UNION 可用 | 说明 |
|---|:---:|:---:|:---:|---|
| `event_time` | ✓ | ✓ | ✓ | 时间戳 → `__time` |
| `user_id` | ✓ | ✓ | ✓ | 用户ID |
| `event_type` | ✓ | ✓ | ✓ | view/click/cart/order/pay |
| `product_id` | ✓ | ✓ | ✓ | 商品ID |
| `product_cat` | ✓ | ✓ | ✓ | 商品类目 |
| `country` | ✓ | ✓ | ✓ | 国家 |
| `province` | ✓ | ✓ | ✓ | 省份 |
| `amount` | ✓ | ✓ | ✓ | 金额 (度量) |
| `quantity` | ✓ | ✓ | ✓ | 数量 (度量) |
| `device` | ✓ | - | - | App: mobile/tablet |
| `os` | ✓ | - | - | App: iOS/Android |
| `channel` | ✓ | - | - | App: organic/ad/push/... |
| `app_version` | ✓ | - | - | App 版本号 |
| `browser` | - | ✓ | - | Web: chrome/safari/... |
| `traffic_source` | - | ✓ | - | Web: organic/ad/social/... |
| `page_url` | - | ✓ | - | Web 页面 URL |
| `session_id` | - | ✓ | - | Web 会话ID |

**度量 (两表一致, UNION 后可相加)**:
- `events` (count)
- `sum_quantity` (longSum)
- `sum_amount` (doubleSum)
- `uv` (thetaSketch)

### 用户池设计 (验证 JOIN)

```
总用户池: 50 万
├── App 独占用户: U0000001 ~ U0350000  (35 万)  <- 只在 app_events
├── Web 独占用户: U0350001 ~ U0450000  (10 万)  <- 只在 web_events
└── 跨端用户:     U0450001 ~ U0500000  (5 万)   <- 两个表都有 ★ JOIN 可用
```

- App 用户池: 40 万 (35 万独占 + 5 万跨端)
- Web 用户池: 15 万 (10 万独占 + 5 万跨端)
- 跨端用户在两个表都有数据, 可通过 `user_id` 做 JOIN

### 数据量

| 表 | 行数 | 文件数 | 预计大小 |
|---|---|---|---|
| `app_events` | 600 万 | 6 个 | ~1.5 GB |
| `web_events` | 400 万 | 4 个 | ~1.2 GB |
| **合计** | **1000 万** | 10 个 | ~2.7 GB |

---

## 使用步骤

### 1. 生成数据

```bash
# 默认: App 600万 + Web 400万
python3 gen_union_events.py

# 自定义路径和行数 (小规模测试推荐)
python3 gen_union_events.py --output ./data --app-rows 100000 --web-rows 100000
```

输出:
```
<data>/
├── app/
│   ├── app_part_01.json   (100 万行)
│   ├── ...
│   └── app_part_06.json
└── web/
    ├── web_part_01.json   (100 万行)
    ├── ...
    └── web_part_04.json
```

### 2. 修改 spec 中的路径

编辑 `app_events_ingest.json` 和 `web_events_ingest.json`，把 `baseDir` 改为实际路径:

```json
// app_events_ingest.json
"inputSource": {
  "type": "local",
  "baseDir": "/data/union-demo/app/",
  "filter": "*.json"
}

// web_events_ingest.json
"inputSource": {
  "type": "local",
  "baseDir": "/data/union-demo/web/",
  "filter": "*.json"
}
```

### 3. 提交导入任务

```bash
# 导入 App 表
curl -X POST "http://localhost:8888/druid/indexer/v1/task" \
  -H "Content-Type: application/json" \
  -d @app_events_ingest.json

# 导入 Web 表
curl -X POST "http://localhost:8888/druid/indexer/v1/task" \
  -H "Content-Type: application/json" \
  -d @web_events_ingest.json
```

---

## 验证查询

以下 SQL 在 Druid Web Console 的 Query 页面执行 (`http://localhost:8888/unified-console.html`)。

### 场景 1: UNION ALL — 合并 App + Web 全量行为

选择两表公共字段, 合并为一个统一视图:

```sql
-- 全平台事件总量 (UNION ALL)
SELECT
  event_type,
  SUM(cnt) AS total_events,
  SUM(amt) AS total_amount
FROM (
  SELECT event_type, SUM("events") AS cnt, SUM("sum_amount") AS amt
  FROM app_events
  WHERE __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
  GROUP BY 1
  UNION ALL
  SELECT event_type, SUM("events") AS cnt, SUM("sum_amount") AS amt
  FROM web_events
  WHERE __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
  GROUP BY 1
)
GROUP BY 1
ORDER BY 1;
```

### 场景 2: UNION ALL — 全平台每小时 PV 趋势

```sql
SELECT
  FLOOR(__time TO HOUR) AS hour,
  SUM(cnt) AS pv
FROM (
  SELECT FLOOR(__time TO HOUR) AS hour, SUM("events") AS cnt
  FROM app_events
  WHERE __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-02'
  GROUP BY 1
  UNION ALL
  SELECT FLOOR(__time TO HOUR) AS hour, SUM("events") AS cnt
  FROM web_events
  WHERE __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-02'
  GROUP BY 1
)
GROUP BY 1
ORDER BY 1;
```

### 场景 3: 对比分析 — App vs Web 各事件占比

```sql
-- App 端漏斗
SELECT 'app' AS source, event_type, SUM("events") AS cnt
FROM app_events
WHERE __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
GROUP BY 1, 2

UNION ALL

-- Web 端漏斗
SELECT 'web' AS source, event_type, SUM("events") AS cnt
FROM web_events
WHERE __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
GROUP BY 1, 2
ORDER BY 1, 2;
```

### 场景 4: JOIN — 跨端用户行为 (5 万跨端用户)

找出同时在 App 和 Web 都有下单行为的用户:

```sql
SELECT
  a.user_id,
  a.app_orders,
  w.web_orders,
  a.app_amount,
  w.web_amount
FROM (
  SELECT user_id, SUM("events") AS app_orders, SUM("sum_amount") AS app_amount
  FROM app_events
  WHERE event_type = 'order'
    AND __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
  GROUP BY 1
) a
JOIN (
  SELECT user_id, SUM("events") AS web_orders, SUM("sum_amount") AS web_amount
  FROM web_events
  WHERE event_type = 'order'
    AND __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
  GROUP BY 1
) w
ON a.user_id = w.user_id
ORDER BY (a.app_amount + w.web_amount) DESC
LIMIT 20;
```

### 场景 5: JOIN — 跨端用户 App 浏览 → Web 下单路径

```sql
SELECT
  a.user_id,
  COUNT(*) AS app_views,
  w.web_orders
FROM app_events a
JOIN (
  SELECT user_id, SUM("events") AS web_orders
  FROM web_events
  WHERE event_type = 'order'
    AND __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
  GROUP BY 1
) w ON a.user_id = w.user_id
WHERE a.event_type = 'view'
  AND a.__time >= TIMESTAMP '2024-06-01' AND a.__time < TIMESTAMP '2024-06-08'
GROUP BY 1, 3
ORDER BY app_views DESC
LIMIT 20;
```

### 场景 6: App 独有字段分析 — 设备/版本分布

```sql
-- App 设备 + 系统分布
SELECT device, os, SUM("events") AS events,
       THETA_SKETCH_ESTIMATE(DS_THETA("uv")) AS uv
FROM app_events
WHERE __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
GROUP BY 1, 2
ORDER BY 3 DESC;
```

### 场景 7: Web 独有字段分析 — 浏览器/流量来源

```sql
-- Web 浏览器 + 流量来源
SELECT browser, traffic_source, SUM("events") AS events,
       THETA_SKETCH_ESTIMATE(DS_THETA("uv")) AS uv
FROM web_events
WHERE __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08'
GROUP BY 1, 2
ORDER BY 3 DESC;
```

### 场景 8: 验证跨端用户数

```sql
-- App 独占用户数
SELECT COUNT(DISTINCT user_id) AS app_only_users
FROM app_events
WHERE user_id NOT LIKE 'U045%'
  AND __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08';

-- 跨端用户数 (App 侧)
SELECT COUNT(DISTINCT user_id) AS cross_users_in_app
FROM app_events
WHERE user_id >= 'U0450001' AND user_id <= 'U0500000'
  AND __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08';

-- 跨端用户数 (Web 侧)
SELECT COUNT(DISTINCT user_id) AS cross_users_in_web
FROM web_events
WHERE user_id >= 'U0450001' AND user_id <= 'U0500000'
  AND __time >= TIMESTAMP '2024-06-01' AND __time < TIMESTAMP '2024-06-08';
```

---

## 验证场景总结

| 场景 | 查询方式 | 验证点 |
|---|---|---|
| 全平台总量 | UNION ALL | 两表公共字段合并, 度量相加 |
| 全平台趋势 | UNION ALL + GROUP BY | 合并后按时间分组 |
| App vs Web 对比 | UNION ALL + source 列 | 同结构数据带来源标记 |
| 跨端用户 | JOIN on user_id | 两表通过共享 user_id 关联 |
| 跨端路径 | JOIN + 过滤 | App 浏览 → Web 下单 |
| App 独有分析 | 单表查询 | device/os/channel/app_version |
| Web 独有分析 | 单表查询 | browser/traffic_source/page_url |
| 跨端用户验证 | COUNT DISTINCT | 确认 5 万跨端用户在两表都存在 |
