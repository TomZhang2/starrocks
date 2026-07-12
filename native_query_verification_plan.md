# native_query 性能验证方案

> **版本**: v1.0  
> **日期**: 2026-07-12  
> **范围**: JDBC native_query + ES native_query vs 外部 Catalog/外表查询  
> **分支**: branch-3.4.5

---

## 目录

- [1. 概述](#1-概述)
- [2. 术语与架构对比](#2-术语与架构对比)
- [3. JDBC native_query 实现补全](#3-jdbc-native_query-实现补全)
- [4. 测试环境设计](#4-测试环境设计)
- [5. 测试数据集设计](#5-测试数据集设计)
- [6. 测试用例设计](#6-测试用例设计)
  - [6.1 场景1：简单聚合下推](#61-场景1简单聚合下推)
  - [6.2 场景2：JOIN 下推](#62-场景2join-下推)
  - [6.3 场景3：三表 JOIN + 聚合](#63-场景3三表-join--聚合)
  - [6.4 场景4：UNION 查询](#64-场景4union-查询)
  - [6.5 场景5：函数过滤](#65-场景5函数过滤)
  - [6.6 场景6：源库视图 / CTE / 子查询](#66-场景6源库视图--cte--子查询)
  - [6.7 场景7：native_query 结果与 StarRocks 内表 JOIN](#67-场景7native_query-结果与-starrocks-内表-join)
  - [6.8 场景8：ES 全文检索 + 聚合](#68-场景8es-全文检索--聚合)
  - [6.9 场景9：UNION + JOIN 组合](#69-场景9union--join-组合)
  - [6.10 场景10：并行度劣势验证](#610-场景10并行度劣势验证)
- [7. 场景分析：native_query 何时优于外表查询](#7-场景分析native_query-何时优于外表查询)
- [8. 测量方法论](#8-测量方法论)
- [9. 预期结果矩阵](#9-预期结果矩阵)
- [10. 参考基准数据](#10-参考基准数据)

---

## 1. 概述

本文档定义了 StarRocks `native_query` 功能的性能验证方案，覆盖 JDBC 和 Elasticsearch (ES) 两种 native_query 路径，与直接查询外部 Catalog 或外表的性能进行对比。

**验证目标**：

1. 量化 native_query 相比外表查询的性能优势
2. 识别 native_query 优于外表查询的具体场景
3. 识别 native_query 劣于外表查询的场景（边界条件）
4. 为用户提供 native_query 使用指南

---

## 2. 术语与架构对比

### 2.1 术语定义

| 术语 | 定义 |
|---|---|
| **native_query** | StarRocks 表函数，将用户 SQL 原样传递给源系统执行（ES `/_sql` 端点 / JDBC 源数据库） |
| **外表查询** | 通过外部 Catalog 直接查询外部表，StarRocks 拉取原始数据到本地计算 |
| **JDBC native_query** | `TABLE(jdbc_catalog.native_query('SELECT...'))` — SQL 传递给 MySQL/PostgreSQL/Oracle 等源数据库执行 |
| **ES native_query** | `TABLE(es_catalog.native_query('SELECT...'))` — SQL 传递给 ES/OpenSearch `/_sql` 端点执行 |

### 2.2 架构对比

#### JDBC native_query vs JDBC 外表查询

```
JDBC 外表查询:
  StarRocks → JDBC driver → SELECT cols FROM table WHERE <pushed_predicates>
  → 拉取全部匹配行到 StarRocks → 本地聚合/JOIN/过滤
  → 不支持聚合下推、JOIN 下推、函数下推
  → 无 Data Cache

JDBC native_query:
  StarRocks → JDBC driver → SELECT <完整用户SQL> FROM ...
  → 源数据库执行完整 SQL（利用索引、优化器、JOIN 策略）
  → 仅结果集返回 StarRocks
  → StarRocks 可对结果追加额外过滤
```

#### ES native_query vs ES 外表查询

```
ES 外表查询:
  StarRocks 构建 ES Query DSL → scroll API 拉取原始文档 → 本地计算
  → 每个分片一个 scan range（多分片并行）
  → 谓词下推受限于 DSL 表达能力
  → docvalue_fields / stored_fields 优化

ES native_query:
  StarRocks → POST /_sql → ES SQL 引擎执行 → cursor 分页返回结果
  → 单个集群级 scan range（无分片并行）
  → SQL 完整交给 ES，ES 自有优化器
  → 无 docvalue/stored_fields 优化
```

### 2.3 核心维度对比

| 维度 | native_query | 外表查询 |
|---|---|---|
| **计算位置** | 源系统（MySQL/ES SQL 引擎） | StarRocks BE 本地 |
| **网络传输** | 仅结果集（少量行） | 全部原始行 |
| **源端索引利用** | 完整利用（主键、二级索引、执行计划优化） | 仅简单谓词下推，函数不可下推 |
| **聚合执行** | 源端执行，只返回聚合结果 | JDBC: 拉全表本地聚合；ES: scroll 拉全文档本地聚合 |
| **JOIN 执行** | 源端索引 JOIN | 拉两表数据到 StarRocks 本地 JOIN |
| **并行度** | 单 scan range（单流） | ES: 多分片并行；JDBC: 单连接 |
| **Schema 探测开销** | 额外一次探测请求 | 无（元数据预加载） |

---

## 3. JDBC native_query 实现补全

### 3.1 实现状态

在 branch-3.4.5 分支上，ES native_query 已完全实现，JDBC native_query 基础设施已就绪但缺失 `getTableFromQuery()` 方法。以下改动补全了 JDBC native_query。

### 3.2 改动文件清单

| 文件 | 改动类型 | 说明 |
|---|---|---|
| `fe/fe-core/src/main/java/com/starrocks/connector/jdbc/JDBCMetadata.java` | 新增方法 | `getTableFromQuery()` + `probeSchemaFromQuery()` + `getTableStatistics()` override |
| `fe/fe-core/src/main/java/com/starrocks/planner/JDBCScanNode.java` | 修改构造函数 | `isQueryTable()` 时跳过 identifier 符号包裹 |

### 3.3 数据流（JDBC native_query 完整路径）

```
用户: TABLE(jdbc_catalog.native_query('SELECT category, COUNT(*) FROM orders GROUP BY category'))
  │
  ▼ QueryAnalyzer.tryParseJdbcQueryTableFunctionName()
  │  解析 catalog.native_query('sql') 语法
  │
  ▼ QueryAnalyzer.resolveQueryTable()
  │  调用 metadata.getTableFromQuery()
  │
  ▼ JDBCMetadata.getTableFromQuery()
  │  1. PassThroughQueryValidator.normalize(query)  — 验证 SELECT-only，去分号
  │  2. probeSchemaFromQuery(normalizedQuery)
  │     → 执行 SELECT * FROM (normalizedQuery) AS starrocks_native_query WHERE 1=0
  │     → 从 ResultSetMetaData 推断列名和类型
  │     → schemaResolver.convertColumnType() 复用现有类型转换逻辑
  │  3. 创建 JDBCTable，调用 setPassThroughQuery(normalizedQuery)
  │     → jdbcTable = "(normalizedQuery) starrocks_query"
  │     → queryTable = true
  │
  ▼ PlanFragmentBuilder.visitPhysicalJDBCScan()
  │  创建 JDBCScanNode，传入 JDBCTable
  │  → 构造函数检测 isQueryTable()，跳过 backtick 包裹
  │  → tableName = "(SELECT category, COUNT(*) FROM orders GROUP BY category) starrocks_query"
  │
  ▼ JDBCScanNode.toThrift()
  │  发送 table_name, columns, filters, limit 到 BE
  │
  ▼ BE: jdbc_connector.cpp get_jdbc_sql()
  │  纯字符串拼接: SELECT cols FROM (user_sql) starrocks_query WHERE filters
  │
  ▼ BE: jdbc_scanner.cpp → JNI → JDBCScanner.java
  │  connection.prepareStatement(sql).executeQuery()
  │
  ▼ 源数据库执行 SQL，返回结果集
```

### 3.4 代码变更详情

#### 3.4.1 JDBCMetadata.java

**新增 import**:

```java
import com.starrocks.catalog.PassThroughQueryValidator;
import com.starrocks.catalog.PartitionKey;
import com.starrocks.connector.TableVersionRange;
import com.starrocks.sql.analyzer.ConnectContext;
import com.starrocks.sql.optimizer.OptimizerContext;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.sql.optimizer.statistics.ColumnStatistic;
import com.starrocks.sql.optimizer.statistics.Statistics;

import java.sql.ResultSetMetaData;
import java.sql.Statement;
```

**新增方法 `getTableFromQuery()`**:

```java
@Override
public Table getTableFromQuery(ConnectContext context, String dbName, String query) {
    String normalizedQuery = PassThroughQueryValidator.normalize(query);

    List<Column> fullSchema = probeSchemaFromQuery(normalizedQuery);
    if (fullSchema.isEmpty()) {
        throw new StarRocksConnectorException("JDBC native query returned no columns");
    }

    long tableId = ConnectorTableId.CONNECTOR_ID_GENERATOR.getNextId().asInt();
    String tableName = "_native_query_" + tableId;
    try {
        JDBCTable queryTable = (JDBCTable) schemaResolver.getTable(
                tableId, tableName, fullSchema, dbName, catalogName, properties);
        queryTable.setPassThroughQuery(normalizedQuery);
        return queryTable;
    } catch (DdlException e) {
        throw new StarRocksConnectorException(
                "Failed to create JDBC query table: " + e.getMessage(), e);
    }
}
```

**新增方法 `probeSchemaFromQuery()`**:

```java
private List<Column> probeSchemaFromQuery(String normalizedQuery) {
    String probeSql = "SELECT * FROM (" + normalizedQuery + ") AS starrocks_native_query WHERE 1=0";
    List<Column> schema = Lists.newArrayList();
    try (Connection connection = getConnection();
         Statement stmt = connection.createStatement()) {
        try (ResultSet rs = stmt.executeQuery(probeSql)) {
            ResultSetMetaData metaData = rs.getMetaData();
            int columnCount = metaData.getColumnCount();
            for (int i = 1; i <= columnCount; i++) {
                String columnName = metaData.getColumnLabel(i);
                int dataType = metaData.getColumnType(i);
                String typeName = metaData.getColumnTypeName(i);
                int columnSize = metaData.getPrecision(i);
                int digits = metaData.getScale(i);
                boolean nullable = metaData.isNullable(i) != ResultSetMetaData.columnNoNulls;

                Type type = schemaResolver.convertColumnType(dataType, typeName, columnSize, digits);
                schema.add(new Column(columnName, type, nullable));
            }
        }
    } catch (SQLException e) {
        throw new StarRocksConnectorException(
                "Failed to infer schema for JDBC native query: " + e.getMessage(), e);
    }
    return schema;
}
```

**新增方法 `getTableStatistics()` override**:

```java
private static final long DEFAULT_QUERY_TABLE_ROW_COUNT = 1L;

@Override
public Statistics getTableStatistics(OptimizerContext session,
                                     Table table,
                                     Map<ColumnRefOperator, Column> columns,
                                     List<PartitionKey> partitionKeys,
                                     ScalarOperator predicate,
                                     long limit,
                                     TableVersionRange tableVersionRange) {
    Statistics.Builder builder = Statistics.builder();
    JDBCTable jdbcTable = (JDBCTable) table;
    if (jdbcTable.isQueryTable()) {
        builder.setOutputRowCount(DEFAULT_QUERY_TABLE_ROW_COUNT);
    }
    for (Map.Entry<ColumnRefOperator, Column> entry : columns.entrySet()) {
        builder.addColumnStatistic(entry.getKey(), ColumnStatistic.builder()
                .setAverageRowSize(entry.getValue().getType().getTypeSize())
                .setNullsFraction(0)
                .setType(ColumnStatistic.StatisticType.ESTIMATE)
                .build());
    }
    return builder.build();
}
```

#### 3.4.2 JDBCScanNode.java

**修改构造函数**:

```java
public JDBCScanNode(PlanNodeId id, TupleDescriptor desc, JDBCTable tbl) {
    super(id, desc, "SCAN JDBC");
    table = tbl;
    if (tbl.isQueryTable()) {
        tableName = tbl.getCatalogTableName();
    } else {
        String objectIdentifier = getIdentifierSymbol();
        tableName = objectIdentifier + tbl.getCatalogTableName() + objectIdentifier;
    }
}
```

**变更原因**: 当 `isQueryTable()` 为 true 时，`jdbcTable` 的值是 `(user_sql) starrocks_query` 子查询形式。如果用 backtick 包裹（MySQL 模式），MySQL 会将整个子查询当作字面量表名而非子查询处理，导致 SQL 语法错误。跳过 identifier 符号包裹确保子查询正确传递给源数据库。

---

## 4. 测试环境设计

### 4.1 集群拓扑

| 组件 | 规格 | 数量 | 说明 |
|---|---|---|---|
| FE | 8C 16G | 1 | 元数据管理、查询规划 |
| BE/CN | 16C 64G + 950GB NVMe | 3+ | 查询执行、Data Cache 存储 |
| ES 集群 | 8C 32G | 3 节点 | ES 7.x+ / OpenSearch 2.x，用于 ES native_query 对比 |
| MySQL | 8C 16G | 1 | MySQL 8.0，用于 JDBC native_query 对比 |
| HDFS/S3 | — | — | 数据湖存储，用于 Hive/Iceberg 对比 |

**关键要求**: BE 与外部数据源网络延迟可控且可测量（同机房或同 VPC），排除网络抖动干扰。

### 4.2 关键 Session Variable

每次测试前显式设置：

```sql
-- 缓存控制
SET enable_profile = true;
SET enable_datacache_io_adaptor = false;          -- 防止 warm 测试时路由到远端
SET enable_datacache_sharing = false;              -- 隔离单节点缓存行为
SET enable_query_cache = false;                    -- 公平对比（外表不支持 Query Cache）
SET populate_datacache_mode = 'auto';              -- 或 'never'(冷测) / 'always'(强制缓存)
SET enable_datacache_async_populate_mode = false;  -- 同步模式，确定性 warmup

-- Iceberg 专用
SET plan_mode = 'local';                           -- 或 'distributed' 分别测试

-- 统计信息
SET enable_cbo = true;
```

### 4.3 冷热缓存控制协议

| 场景 | 操作 | 测量目标 |
|---|---|---|
| **全冷** | 清 OS page cache + 删 BE 的 datacache 目录 + 重启 BE + `populate_datacache_mode='never'` | 纯远端存储读取延迟 |
| **冷缓存+populate** | 清缓存 + `populate_datacache_mode='auto'` | 首次查询延迟（含缓存写入开销） |
| **全热（同步）** | warmup 1 次（同步模式）→ 测量第 2 次+ | 完全 warm 的缓存性能 |
| **全热（异步）** | warmup 3-5 次 → 测量 | 生产级 warm 缓存性能 |
| **元数据冷+数据热** | `REFRESH EXTERNAL TABLE` 刷元数据 + 保留数据缓存 | 隔离元数据获取成本 |
| **元数据热+数据冷** | 预热元数据 + 清数据缓存 | 隔离数据获取成本 |

---

## 5. 测试数据集设计

### 5.1 数据规模分层

| 规模 | 行数 | 说明 | 用途 |
|---|---|---|---|
| **S** | 10 万行 | 小数据集 | 验证固定开销（schema 探测、连接建立等） |
| **M** | 1000 万行 | 中等数据集 | 聚合下推 vs 本地聚合的分水岭 |
| **L** | 1 亿行 | 大数据集 | 网络传输瓶颈凸显 |
| **XL** | 10 亿行 | 超大数据集 | scroll/JDBC 全量拉取的极限场景 |

### 5.2 数据表结构

在 ES/MySQL/Hive 中分别建相同结构和数据量的表，确保对比公平：

```
table: benchmark_events
├── id          BIGINT       -- 主键（ES有索引，MySQL有索引）
├── user_id     VARCHAR(64)  -- 高基数维度（100万唯一值）
├── event_type  VARCHAR(32)  -- 低基数维度（50种）
├── category    VARCHAR(32)  -- 低基数维度（20种）
├── amount      DOUBLE       -- 数值度量
├── status      INT          -- 低基数（5种）
├── timestamp   DATETIME     -- 时间维度
├── payload     TEXT/JSON    -- 大字段（验证大列传输开销）
└── geo_point   GEO_POINT    -- ES特有（验证ES原生查询优势）
```

**数据分布要求**：

- `timestamp`：均匀分布跨越 1 年，支持分区裁剪测试
- `amount`：正态分布，支持范围过滤
- `user_id`：高基数，支持聚合去重测试
- `payload`：平均 2KB/行，验证大列网络传输开销

---

## 6. 测试用例设计

以下所有 SQL 假设：

- `mysql_catalog` 指向 MySQL 数据库（含 `orders`、`users`、`products`、`refunds` 表）
- `es_catalog` 指向 ES/OpenSearch 集群（含 `events`、`click_events`、`view_events`、`product_logs` 索引）
- 每个场景给出 **native_query 版本** 和 **外表版本** 对比

---

### 6.1 场景1：简单聚合下推

**原理**: native_query 将聚合推给源系统执行，只返回聚合结果（少量行）；外表路径拉取全部原始行到 StarRocks 本地聚合。

#### JDBC (MySQL)

```sql
-- native_query: 聚合在 MySQL 执行，只返回 20 行
SELECT * FROM TABLE(mysql_catalog.native_query('
    SELECT category, COUNT(*) AS cnt, SUM(amount) AS total
    FROM orders
    WHERE create_time >= ''2025-01-01''
    GROUP BY category
    ORDER BY total DESC
'));

-- 外表: StarRocks 拉取所有匹配行到本地聚合
SELECT category, COUNT(*) AS cnt, SUM(amount) AS total
FROM mysql_catalog.db.orders
WHERE create_time >= '2025-01-01'
GROUP BY category
ORDER BY total DESC;
```

#### ES

```sql
-- native_query: 聚合在 ES SQL 引擎执行
SELECT * FROM TABLE(es_catalog.native_query('
    SELECT event_type, COUNT(*) AS cnt
    FROM events
    WHERE timestamp >= ''2025-01-01''
    GROUP BY event_type
'));

-- 外表: StarRocks scroll 拉全文档本地聚合
SELECT event_type, COUNT(*) AS cnt
FROM es_catalog.db.events
WHERE timestamp >= '2025-01-01'
GROUP BY event_type;
```

**关键指标**: `RowsRead`（StarRocks 侧读取行数）、`QueryTime`、`BytesRead`

**参考证据**: GitHub Issue #69762 明确指出 JDBC 外表不支持下推聚合，`COUNT(*)` 会拉取全部行。

---

### 6.2 场景2：JOIN 下推

**原理**: native_query 可以在源系统内执行 JOIN（利用源端索引和优化器）；外表路径 StarRocks 将两个外表数据都拉到本地再 JOIN。

```sql
-- === JDBC (MySQL) ===

-- native_query: JOIN 在 MySQL 执行，利用索引
SELECT * FROM TABLE(mysql_catalog.native_query('
    SELECT o.id, o.amount, u.user_name, u.email
    FROM orders o
    INNER JOIN users u ON o.user_id = u.id
    WHERE o.status = 1 AND o.create_time >= ''2025-06-01''
    ORDER BY o.amount DESC
    LIMIT 100
'));

-- 外表: 两个外表数据拉到 StarRocks 本地 JOIN
SELECT o.id, o.amount, u.user_name, u.email
FROM mysql_catalog.db.orders o
INNER JOIN mysql_catalog.db.users u ON o.user_id = u.id
WHERE o.status = 1 AND o.create_time >= '2025-06-01'
ORDER BY o.amount DESC
LIMIT 100;

-- === ES ===

-- native_query: ES SQL 引擎内部 JOIN（ES 7.6+ 支持）
SELECT * FROM TABLE(es_catalog.native_query('
    SELECT e.user_id, e.event_type, u.user_name
    FROM events e
    JOIN users u ON e.user_id = u.id
    WHERE e.timestamp >= ''2025-06-01''
'));
```

**关键指标**: `RowsRead`、`QueryTime`、JOIN 执行计划中的 `RowsReturned`

**参考证据**: GitHub Issue #70813 指出 JDBC Catalog 不支持 JOIN 下推到外部数据库。

---

### 6.3 场景3：三表 JOIN + 聚合

**原理**: 多表 JOIN + 聚合是 native_query 优势最显著的场景：全部计算在源端完成，StarRocks 只接收最终结果。

```sql
-- === JDBC (MySQL) ===

-- native_query: 三表 JOIN + 聚合全部在 MySQL 执行
SELECT * FROM TABLE(mysql_catalog.native_query('
    SELECT p.product_name, COUNT(o.id) AS order_count, SUM(o.amount) AS total_revenue
    FROM orders o
    INNER JOIN users u ON o.user_id = u.id
    INNER JOIN products p ON o.product_id = p.id
    WHERE o.status = 2
      AND o.create_time >= ''2025-01-01''
      AND u.region = ''CN''
    GROUP BY p.product_name
    HAVING total_revenue > 10000
    ORDER BY total_revenue DESC
'));

-- 外表: 三表数据全部拉到 StarRocks 本地 JOIN + 聚合
SELECT p.product_name, COUNT(o.id) AS order_count, SUM(o.amount) AS total_revenue
FROM mysql_catalog.db.orders o
INNER JOIN mysql_catalog.db.users u ON o.user_id = u.id
INNER JOIN mysql_catalog.db.products p ON o.product_id = p.id
WHERE o.status = 2
  AND o.create_time >= '2025-01-01'
  AND u.region = 'CN'
GROUP BY p.product_name
HAVING total_revenue > 10000
ORDER BY total_revenue DESC;
```

---

### 6.4 场景4：UNION 查询

**原理**: native_query 将 UNION 查询完整传递给源系统，源系统在本地合并结果后返回；外表路径需要分别拉取两个表的数据到 StarRocks 再合并。

#### JDBC (MySQL)

```sql
-- native_query: UNION 在 MySQL 执行
SELECT * FROM TABLE(mysql_catalog.native_query('
    SELECT ''order'' AS source, id, amount, create_time
    FROM orders
    WHERE create_time >= ''2025-07-01''
    UNION ALL
    SELECT ''refund'' AS source, id, -amount AS amount, create_time
    FROM refunds
    WHERE create_time >= ''2025-07-01''
    ORDER BY create_time DESC
    LIMIT 1000
'));

-- 外表: 两个外表分别查询后在 StarRocks UNION
SELECT 'order' AS source, id, amount, create_time
FROM mysql_catalog.db.orders
WHERE create_time >= '2025-07-01'
UNION ALL
SELECT 'refund' AS source, id, -amount AS amount, create_time
FROM mysql_catalog.db.refunds
WHERE create_time >= '2025-07-01'
ORDER BY create_time DESC
LIMIT 1000;
```

#### ES

```sql
-- native_query: UNION 多索引查询在 ES 执行
SELECT * FROM TABLE(es_catalog.native_query('
    SELECT ''click'' AS event_type, user_id, timestamp
    FROM click_events
    WHERE timestamp >= ''2025-07-01''
    UNION ALL
    SELECT ''view'' AS event_type, user_id, timestamp
    FROM view_events
    WHERE timestamp >= ''2025-07-01''
    LIMIT 500
'));
```

---

### 6.5 场景5：函数过滤

**原理**: native_query 将完整 SQL 交给源系统，源系统可执行任意函数；外表路径受限于 StarRocks 的谓词下推能力（JDBC 不支持函数下推）。

```sql
-- === JDBC (MySQL) ===

-- native_query: 函数在 MySQL 执行，利用索引
SELECT * FROM TABLE(mysql_catalog.native_query('
    SELECT id, user_id, amount, UPPER(status) AS status_upper
    FROM orders
    WHERE UPPER(status) = ''COMPLETED''
      AND DATE_FORMAT(create_time, ''%Y-%m'') = ''2025-06''
'));

-- 外表: UPPER() 和 DATE_FORMAT() 无法下推到 MySQL，StarRocks 拉全表后本地过滤
SELECT id, user_id, amount, UPPER(status) AS status_upper
FROM mysql_catalog.db.orders
WHERE UPPER(status) = 'COMPLETED'
  AND DATE_FORMAT(create_time, '%Y-%m') = '2025-06';
```

**关键指标**: `RowsRead` — native_query 应远小于外表（源端过滤 vs 全表拉取）

**参考证据**: StarRocks 官方文档明确指出 JDBC 外表不支持函数下推（`PushDownPredicateToExternalTableScanRule` 中 `CanFullyPushDownVisitor` 对任何函数调用返回 false）。

---

### 6.6 场景6：源库视图 / CTE / 子查询

**原理**: native_query 可以访问源数据库中定义的视图、CTE 递归查询等，这些是外表路径完全无法访问的。

#### 源库视图

```sql
-- native_query: 直接查询 MySQL 视图
SELECT * FROM TABLE(mysql_catalog.native_query('
    SELECT * FROM v_monthly_sales_report WHERE month = ''2025-06''
'));

-- 外表: 无法访问 MySQL 视图（只能访问物理表）
-- ❌ 不支持: SELECT * FROM mysql_catalog.db.v_monthly_sales_report
```

#### CTE 递归查询

```sql
-- native_query: CTE 递归查询
SELECT * FROM TABLE(mysql_catalog.native_query('
    WITH RECURSIVE category_path AS (
        SELECT id, name, parent_id, CAST(name AS CHAR(1000)) AS path
        FROM categories
        WHERE parent_id IS NULL
        UNION ALL
        SELECT c.id, c.name, c.parent_id, CONCAT(cp.path, '' > '', c.name)
        FROM categories c
        INNER JOIN category_path cp ON c.parent_id = cp.id
    )
    SELECT * FROM category_path WHERE path LIKE ''Electronics%''
'));

-- 外表: ❌ 不支持 CTE 下推
```

#### 源库特有语法

```sql
-- native_query: MySQL FORCE INDEX hint
SELECT * FROM TABLE(mysql_catalog.native_query('
    SELECT * FROM orders FORCE INDEX(idx_status) WHERE status = 1
'));

-- 外表: ❌ 不支持源库 hint 语法
```

---

### 6.7 场景7：native_query 结果与 StarRocks 内表 JOIN

**原理**: native_query 返回的结果可以与 StarRocks 内部表 JOIN，实现"源端计算 + StarRocks 本地分析"的混合查询。

```sql
-- native_query 结果作为子查询与 StarRocks 内表 JOIN
SELECT t.category, t.total, s.warehouse_name
FROM TABLE(mysql_catalog.native_query('
    SELECT category, SUM(amount) AS total
    FROM orders
    WHERE create_time >= ''2025-06-01''
    GROUP BY category
')) t
INNER JOIN starrocks_warehouse.stock_summary s ON t.category = s.category
WHERE s.warehouse_name = 'BJ-01';

-- 对比: 先在外表上聚合再 JOIN 内表
SELECT t.category, t.total, s.warehouse_name
FROM (
    SELECT category, SUM(amount) AS total
    FROM mysql_catalog.db.orders
    WHERE create_time >= '2025-06-01'
    GROUP BY category
) t
INNER JOIN starrocks_warehouse.stock_summary s ON t.category = s.category
WHERE s.warehouse_name = 'BJ-01';
```

---

### 6.8 场景8：ES 全文检索 + 聚合

**原理**: ES SQL 支持全文检索、地理查询等 ES 原生能力，通过 native_query 可以在源端执行这些操作并聚合结果。

```sql
-- === ES ===

-- native_query: ES SQL 全文检索 + 聚合
SELECT * FROM TABLE(es_catalog.native_query('
    SELECT category, COUNT(*) AS cnt
    FROM product_logs
    WHERE MATCH(content, ''starrocks performance'')
    AND timestamp >= ''2025-06-01''
    GROUP BY category
    ORDER BY cnt DESC
    LIMIT 20
'));

-- 外表: 使用 esquery() 下推 MATCH
SELECT category, COUNT(*) AS cnt
FROM es_catalog.db.product_logs
WHERE esquery('content', '{"match": {"content": "starrocks performance"}}')
  AND timestamp >= '2025-06-01'
GROUP BY category
ORDER BY cnt DESC
LIMIT 20;
```

#### ES 地理查询

```sql
-- native_query: ES SQL 地理距离查询
SELECT * FROM TABLE(es_catalog.native_query('
    SELECT id, user_id, geo_point
    FROM user_locations
    WHERE GEO_DISTANCE(geo_point, 39.9, 116.4) < 1000
    LIMIT 100
'));

-- 外表: 使用 esquery() 下推 geo_distance
SELECT id, user_id, geo_point
FROM es_catalog.db.user_locations
WHERE esquery('geo_point', '{"geo_distance": {"distance": "1km", "geo_point": [116.4, 39.9]}}')
LIMIT 100;
```

#### ES HISTOGRAM 聚合

```sql
-- native_query: ES SQL HISTOGRAM 聚合（ES 独有能力）
SELECT * FROM TABLE(es_catalog.native_query('
    SELECT HISTOGRAM(timestamp, INTERVAL 1 DAY) AS day, COUNT(*) AS cnt
    FROM events
    WHERE timestamp >= ''2025-06-01''
    GROUP BY day
    ORDER BY day
'));

-- 外表: ❌ StarRocks 无法将 HISTOGRAM 下推到 ES scroll 路径
```

---

### 6.9 场景9：UNION + JOIN 组合

**原理**: 复杂 SQL（UNION + 子查询 + JOIN + 聚合）全部在源端执行，是 native_query 优势最极致的体现。

```sql
-- === JDBC (MySQL) ===

-- native_query: UNION + JOIN + 聚合 全部在 MySQL 执行
SELECT * FROM TABLE(mysql_catalog.native_query('
    SELECT u.user_name, t.total_amount
    FROM (
        SELECT user_id, SUM(amount) AS total_amount
        FROM (
            SELECT user_id, amount FROM orders WHERE status = 2 AND create_time >= ''2025-06-01''
            UNION ALL
            SELECT user_id, amount FROM orders WHERE status = 3 AND create_time >= ''2025-06-01''
        ) combined
        GROUP BY user_id
    ) t
    INNER JOIN users u ON t.user_id = u.id
    WHERE t.total_amount > 1000
    ORDER BY t.total_amount DESC
'));

-- 外表: 多个外表拉数据到 StarRocks 本地执行
SELECT u.user_name, t.total_amount
FROM (
    SELECT user_id, SUM(amount) AS total_amount
    FROM (
        SELECT user_id, amount FROM mysql_catalog.db.orders WHERE status = 2 AND create_time >= '2025-06-01'
        UNION ALL
        SELECT user_id, amount FROM mysql_catalog.db.orders WHERE status = 3 AND create_time >= '2025-06-01'
    ) combined
    GROUP BY user_id
) t
INNER JOIN mysql_catalog.db.users u ON t.user_id = u.id
WHERE t.total_amount > 1000
ORDER BY t.total_amount DESC;
```

---

### 6.10 场景10：并行度劣势验证

**原理**: native_query 是单 scan range（集群级），无法利用 StarRocks 多 BE 并行；外表路径按分片/文件分多个 scan range，多 BE 并行执行。在大结果集无过滤场景下，外表可能更快。

```sql
-- === ES ===

-- native_query: 单 scan range，cursor 分页（可能慢于多分片并行）
SELECT * FROM TABLE(es_catalog.native_query('
    SELECT user_id, event_type, timestamp, payload
    FROM events
    WHERE timestamp >= ''2025-06-01''
    LIMIT 100000
'));

-- 外表: 多分片并行 scroll（可能更快）
SELECT user_id, event_type, timestamp, payload
FROM es_catalog.db.events
WHERE timestamp >= '2025-06-01'
LIMIT 100000;
```

**关键指标**: `InstanceNum`（并行度）、每个 Instance 的 `RowsRead`、总扫描时间 vs 单 scan range cursor 分页时间

---

## 7. 场景分析：native_query 何时优于外表查询

### 7.1 native_query 优势场景

| 场景 | 优势倍数 | 原因 |
|---|---|---|
| **聚合查询**（大表 GROUP BY） | 10x-100x | 源端聚合返回 20 行 vs 拉百万行本地聚合 |
| **JOIN 查询**（源端索引 JOIN） | 5x-50x | 源端索引 JOIN vs 拉两表数据本地 JOIN |
| **函数过滤**（UPPER/DATE_FORMAT 等） | 10x-100x | JDBC 外表不支持函数下推，拉全表后本地过滤 |
| **高选择性等值过滤**（有索引） | 5x-20x | 源端索引毫秒级 vs 全表扫描 |
| **源库视图/CTE/UDF** | ∞（独有能力） | 外表无法访问源库视图/CTE/UDF |
| **ES 全文检索/地理查询** | 2x-10x | ES SQL 原生能力，对比 scroll DSL |
| **UNION + JOIN 组合** | 10x-50x | 全部源端执行 vs 多表数据拉取本地计算 |
| **三表 JOIN + 聚合** | 20x-100x | 全部源端执行，仅返回聚合结果 |

### 7.2 外表查询优势场景

| 场景 | 优势倍数 | 原因 |
|---|---|---|
| **全表扫描大数据集** | 2x-5x | 多分片并行 vs 单 scan range cursor 分页 |
| **小表简单查询** | 1.5x-3x | 无 schema 探测开销 |
| **StarRocks 内表 JOIN 外表** | 视情况 | native_query 结果需先物化再 JOIN |

### 7.3 决策矩阵

```
查询是否包含聚合？           → 是 → native_query 大优（10x-100x）
查询是否包含多表 JOIN？      → 是 → native_query 大优（5x-50x）
查询是否使用函数过滤？       → 是 → native_query 大优（10x-100x）
查询是否访问源库视图/CTE？   → 是 → native_query 独有（外表不支持）
查询是否需要 ES 全文检索？   → 是 → native_query 优（2x-10x）
查询是否大结果集无过滤返回？ → 是 → 外表可能优（2x-5x，并行优势）
查询是否小表简单点查？       → 是 → 外表可能优（1.5x-3x，无探测开销）
```

---

## 8. 测量方法论

### 8.1 测量协议（每个用例）

每个用例按以下流程执行：

```
1. 清理环境（冷测时）：
   - 清除 Data Cache（删 datacache 目录 + 重启 BE）
   - 清除元数据缓存（REFRESH EXTERNAL TABLE）
   - SET populate_datacache_mode = 'never'

2. 执行 ANALYZE TABLE（确保 CBO 统计信息可用）

3. Warmup：执行 1 次查询（不计时，仅 warmup）
   - 冷测跳过此步

4. 正式测量：执行 5 次查询
   - 记录每次响应时间
   - 去掉最大值和最小值
   - 取剩余 3 次平均值

5. 收集 Query Profile：
   - SET enable_profile = true
   - 从 profile 中提取关键指标

6. 记录结果
```

### 8.2 关键指标采集

| 指标 | 来源 | 意义 |
|---|---|---|
| `QueryTime` | Profile | 端到端响应时间 |
| `RowsRead` | Profile | StarRocks 侧读取的行数 |
| `BytesRead` | Profile | 网络传输字节 |
| `DataCacheReadBytes` | Profile | 缓存命中读取字节 |
| `DataCacheWriteBytes` | Profile | 缓存写入字节（冷测时非零） |
| `TotalRawReadTime` | Profile | 原始数据读取时间 |
| `MaterializeTupleTime` | Profile | 元组物化时间 |
| `InstanceNum` | Profile | 并行度 |
| FE `probeSchemaFromQuery` 耗时 | FE 日志 | native_query 的 schema 探测开销 |
| ES 侧 `_search` 请求次数 | ES 慢日志 | scroll 分页次数 |

### 8.3 结果记录模板

每个用例需填写：

| 字段 | 说明 |
|---|---|
| 用例 ID | 如 AGG-1, JOIN-1 等 |
| 数据规模 | S/M/L/XL |
| 缓存状态 | 全冷/冷+populate/全热/元数据冷+数据热/元数据热+数据冷 |
| native_query 耗时 | 5 次测量去极值后的平均（ms） |
| 外表耗时 | 5 次测量去极值后的平均（ms） |
| 倍数 | native_query / 外表 |
| RowsRead (native) | StarRocks 侧读取行数 |
| RowsRead (外表) | StarRocks 侧读取行数 |
| BytesRead (native) | 网络传输字节 |
| BytesRead (外表) | 网络传输字节 |
| 分析 | 差异原因说明 |

---

## 9. 预期结果矩阵

| 场景 | native_query 优势? | 预期倍数 | 关键指标差异 |
|---|---|---|---|
| 简单聚合 (场景1) | ✅ 大优 | 10x-100x | RowsRead: 20 vs 百万 |
| JOIN 下推 (场景2) | ✅ 大优 | 5x-50x | RowsRead: 100 vs 百万 |
| 三表 JOIN+聚合 (场景3) | ✅ 极大优 | 20x-100x | RowsRead: 20 vs 百万 |
| UNION 查询 (场景4) | ✅ 优 | 3x-10x | 源端合并 vs 两端分别拉取 |
| 函数过滤 (场景5) | ✅ 大优 | 10x-100x | RowsRead: 匹配行 vs 全表 |
| 源库视图/CTE (场景6) | ✅ 独有 | ∞ | 外表无法执行 |
| 内表 JOIN native 结果 (场景7) | ✅ 优 | 3x-10x | 源端聚合 + 内表 JOIN |
| ES 全文检索 (场景8) | ✅ 优 | 2x-10x | ES SQL 优化 vs scroll DSL |
| UNION+JOIN 组合 (场景9) | ✅ 大优 | 10x-50x | 全部源端执行 |
| 大结果集返回 (场景10) | ❌ 可能慢 | 0.3x-0.8x | 单流 vs 并行 |
| 小表简单查询 | ❌ 可能慢 | 0.5x-0.9x | 探测开销占比大 |

### 9.1 测试执行优先级

| 优先级 | 用例场景 | 理由 |
|---|---|---|
| **P0** | 场景1（聚合下推） | native_query 核心价值，差距最显著 |
| **P0** | 场景3（三表 JOIN+聚合） | 优势极致场景 |
| **P0** | 场景5（函数过滤） | JDBC 外表已知缺陷验证 |
| **P1** | 场景2（JOIN 下推） | 第二核心价值 |
| **P1** | 场景9（UNION+JOIN 组合） | 复杂 SQL 综合验证 |
| **P1** | 场景10（并行度劣势） | 验证 native_query 的边界 |
| **P2** | 场景4（UNION 查询） | 量化 UNION 下推价值 |
| **P2** | 场景6（视图/CTE） | 独有能力验证 |
| **P2** | 场景7（内表 JOIN） | 混合查询场景 |
| **P2** | 场景8（ES 全文检索） | ES 特有能力验证 |

---

## 10. 参考基准数据

已有官方基准数据可作为参照：

| 基准 | 对比 | 结果 | 来源 |
|---|---|---|---|
| TPC-DS 1TB | StarRocks 原生表 vs Iceberg Catalog | 314s vs 368s（1.17x） | [TPC-DS Benchmark](https://docs.starrocks.io/docs/benchmarking/TPC_DS_Benchmark/) |
| TPC-H 100GB | StarRocks 原生 vs Hive 外表 | 16.6s vs 91.8s（5.5x，无 Data Cache） | [TPC-H Benchmark](https://docs.starrocks.io/docs/benchmarking/TPC-H_Benchmarking/) |
| Alibaba EMR | StarRocks 本地 vs Hive 外表 vs Trino | 21s vs 92s vs 307s | [Alibaba Blog](https://www.alibabacloud.com/blog/598919) |
| Shopee | StarRocks 外表 vs Presto 外表 | 3x-10x | [Shopee Blog](https://medium.com/starrocks-engineering/how-we-3xed-our-query-performance-with-starrocks-at-shopee-8a6a1c737567) |
| GitHub Issue #69762 | JDBC 外表 COUNT(*) | 拉全表超时 300s | [Issue #69762](https://github.com/StarRocks/starrocks/issues/69762) |
| GitHub Issue #70813 | JDBC Catalog JOIN 下推 | 不支持 JOIN 下推 | [Issue #70813](https://github.com/StarRocks/starrocks/issues/70813) |
| GitHub PR #72005 | native_query 功能 PR | JDBC pass-through SQL | [PR #72005](https://github.com/StarRocks/starrocks/pull/72005) |

### 官方文档参考

| 文档 | URL |
|---|---|
| native_query 文档 | https://docs.starrocks.io/docs/sql-reference/sql-functions/table-functions/native_query/ |
| Data Cache 文档 | https://docs.starrocks.io/docs/data_source/data_cache/ |
| Data Cache FAQ | https://docs.starrocks.io/docs/data_source/data_cache_troubleshooting/ |
| JDBC Catalog 文档 | https://docs.starrocks.io/docs/data_source/catalog/jdbc_catalog/ |
| ES Catalog 文档 | https://docs.starrocks.io/docs/data_source/catalog/elasticsearch_catalog/ |
| JDBC Catalog Roadmap | https://github.com/StarRocks/starrocks/issues/70852 |
