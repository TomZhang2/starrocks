# StarRocks ES Catalog native_query 实现设计文档

> 基于 JDBC native_query 设计文档扩展：`native_query_design.md`
> 对应提交：`16f5fb71870` [Feature] support jdbc native query (#72005)
> 目标版本：v4.2+
> 文档：`docs/en/sql-reference/sql-functions/table-functions/native_query.md`（需扩展）

---

## 1. 概述

### 1.1 功能定义

将 `native_query` 表函数从 **JDBC Catalog 专属**扩展到 **ES Catalog**，允许用户将一段 Elasticsearch SQL（ES|QL）语句以 pass-through 方式下推到 ES 集群执行，并将结果集作为 StarRocks 的一个关系（relation）暴露出来。

### 1.2 为什么选择 ES SQL 而非 Query DSL

| 维度 | ES SQL（`_sql` 端点） | Query DSL（`_search` 端点） |
|------|----------------------|---------------------------|
| **Schema 推断** | ✅ 响应直接包含 `columns: [{name, type}]` | ❌ 需从 `_source` + mapping 推断 |
| **与 JDBC native_query 一致性** | ✅ SQL pass-through，用户体验一致 | ❌ JSON pass-through，范式不同 |
| **分页** | ✅ cursor 机制，自动清理 | ⚠️ scroll/PIT，需手动管理 |
| **成熟度** | ✅ GA since ES 6.3 (2018)，API 稳定 | ✅ 核心 API |
| **DSL 过滤** | ✅ 通过 `filter` 参数混合使用 | ✅ 原生 |
| **复杂聚合** | ⚠️ 有限（composite agg） | ✅ 完整 agg DSL |
| **响应解析** | ✅ 简单的列式 rows | ❌ 文档导向，_source 可变 |

**决策**：采用 ES SQL 作为 primary 格式。理由：
1. Schema 推断简洁——`POST /_sql` with `fetch_size:1` 即可获取 `columns` 元数据
2. 与 JDBC native_query UX 一致——均为 SQL pass-through
3. `normalizePassThroughQuery` 校验（必须以 SELECT 开头）可直接复用
4. ES SQL 内部编译为 DSL 执行，无需额外执行路径
5. DSL 模式可作为未来扩展（见 §9.2）

### 1.3 适用场景

- ES 索引的复杂过滤查询，难以通过单一 ES 外表表达
- 使用 ES 特有的全文检索函数（`MATCH()`、`SCORE()`）
- ES 索引的多索引模式匹配查询（`FROM "log*"`）
- 将 ES 查询结果直接 `INSERT INTO` 加载到 StarRocks 内表

### 1.4 核心价值

无需为每个查询需求创建持久的 ES 外表，即可在 SQL 中动态发起 ES 原生 SQL 查询，实现"即查即用"的联邦查询能力。与 JDBC native_query 形成统一的 pass-through 查询体验。

---

## 2. 语法与使用方式

### 2.1 语法

```sql
SELECT ...
FROM TABLE(<es_catalog>.native_query('<select_sql>')) [AS] <alias>
[WHERE ...];
```

### 2.2 参数说明

| 参数           | 说明                                                          |
| ------------ | ----------------------------------------------------------- |
| `es_catalog`  | 已创建的 ES Catalog 名称                                           |
| `select_sql`  | 字符串字面量，包含 ES SQL 语句。去掉前导注释和尾部分号后必须以 `SELECT` 开头 |
| `alias`       | 可选的表别名                                                      |

### 2.3 使用示例

```sql
-- 示例1：ES 全文检索 + StarRocks 外层过滤
SELECT id, name, _score
FROM TABLE(es0.native_query(
    'SELECT id, name, SCORE() AS _score
     FROM app_logs
     WHERE MATCH(content, ''error'')
     ORDER BY _score DESC
     LIMIT 100'
)) q
WHERE _score > 5.0;

-- 示例2：ES 聚合查询 + StarRocks 二次聚合
SELECT category, SUM(cnt) AS total_count
FROM TABLE(es0.native_query(
    'SELECT category, COUNT(*) AS cnt
     FROM products
     GROUP BY category
     ORDER BY cnt DESC'
)) q
GROUP BY category;

-- 示例3：多索引模式匹配
SELECT * FROM TABLE(es0.native_query(
    'SELECT timestamp, level, message
     FROM "logstash-*"
     WHERE level = ''ERROR''
     LIMIT 1000'
)) q;

-- 示例4：将 native_query 结果加载到 StarRocks 内表
INSERT INTO error_summary
SELECT level, COUNT(*) AS error_count
FROM TABLE(es0.native_query(
    'SELECT level FROM "logstash-*" WHERE level = ''ERROR'''
)) q
GROUP BY level;
```

### 2.4 约束与不支持的形式

```sql
-- 不支持命名参数（与 JDBC 一致）
SELECT * FROM TABLE(es0.native_query(query => 'SELECT id FROM logs'));

-- 不支持 WITH 开头的查询（ES SQL 本身不支持 CTE）
SELECT * FROM TABLE(es0.native_query('WITH q AS (...) SELECT * FROM q'));

-- 不支持表别名后跟列别名（与 JDBC 一致）
SELECT * FROM TABLE(es0.native_query('SELECT id FROM logs')) q(id_alias);

-- 不支持非 SELECT 语句
SELECT * FROM TABLE(es0.native_query('DELETE FROM logs WHERE id = 1'));
```

### 2.5 ES SQL 自身的限制（pass-through 不做额外校验，由 ES 报错）

| 限制 | 说明 |
|------|------|
| 不支持 JOIN | 仅支持 nested document |
| 子查询受限 | `SELECT FROM (SELECT ...)` 仅在可展平时支持 |
| GROUP BY 排序 | 依赖 composite aggregation，客户端排序上限 65535 行 |
| TIME 类型 | 不支持作为 GROUP BY 或 HISTOGRAM 的键 |

---

## 3. 设计目标与约束

| 设计目标              | 实现方式                                                          |
|-------------------|---------------------------------------------------------------|
| **最小侵入**          | 复用现有 ES 扫描链路（`EsScanNode` → `ESDataSource` → `ESScanReader`） |
| **通用化改造**         | 将 JDBC 专属的 `queryTable` 机制泛化为多 Catalog 通用                  |
| **源库执行**          | ES SQL pass-through，StarRocks 不做语法解析                          |
| **动态 Schema 推断** | 通过 `POST /_sql` 响应的 `columns` 元数据推断 Schema               |
| **锁优化**           | Schema 推断在 PlannerMetaLock 之外完成（复用 JDBC 的两阶段解析）        |
| **权限控制**          | 校验用户对 ES Catalog 的 USAGE 权限（复用现有机制）                    |
| **安全性**           | 仅允许 SELECT 语句，拒绝 INSERT/UPDATE/DELETE 等                    |
| **无 Thrift 变更**    | 通过 `TEsScanNode.properties` map 传递 native query，不新增 Thrift 字段 |

---

## 4. 整体架构

### 4.1 核心设计思想

ES native_query 与 JDBC native_query 共享相同的**适配器模式**：将一段 pass-through SQL 转换为一个合成的 Table，之后复用现有的扫描链路。

关键差异在于：
- **JDBC**：将 SQL 包装为子查询 `(query) starrocks_query`，通过 JDBC 驱动执行
- **ES**：将 SQL 直接发送到 `POST /_sql` 端点，通过 REST API 执行

```
用户 SQL: TABLE(es0.native_query('SELECT ... FROM index WHERE ...'))
         │
         ▼
    ┌─────────────────────────────────────────┐
    │  SQL Parser                               │  生成 TableFunctionRelation
    │  (functionName = "es0.native_query")      │
    └────────────┬─────────────────────────────┘
                 │
                 ▼
    ┌─────────────────────────────────────────┐
    │  QueryAnalyzer (FE)                       │  识别 catalog.native_query 模式
    │  ├─ 参数校验（单字符串字面量）              │  校验 SELECT-only（复用 normalizePassThroughQuery）
    │  ├─ resolveQueryTable()                   │  调用 ConnectorMetadata.getTableFromQuery()
    │  └─ buildQueryTableScope()                │  构建 Field/Scope
    └────────────┬─────────────────────────────┘
                 │
                 ▼
    ┌─────────────────────────────────────────┐
    │  ElasticsearchMetadata.getTableFromQuery()│  Schema 探断
    │  POST /_sql  { "query": "...",            │  执行 ES SQL 探测查询
    │    "fetch_size": 1 }                      │  读取 columns 元数据
    │  → 构建合成 EsTable (queryTable=true)      │  esTable 存储 passThroughQuery
    └────────────┬─────────────────────────────┘
                 │
                 ▼
    ┌─────────────────────────────────────────┐
    │  RelationTransformer (FE)                 │  构建逻辑计划
    │  buildQueryTablePlan()                    │  → LogicalEsScanOperator
    │  (复用现有 ES 扫描算子)                    │  (与普通 ES 外表完全相同)
    └────────────┬─────────────────────────────┘
                 │
                 ▼
    ┌─────────────────────────────────────────┐
    │  EsScanNode (FE)                          │  物理扫描节点
    │  isQueryTable() → 跳过分片扫描范围          │  创建单扫描范围（使用 ES 集群节点）
    │  properties.put("native_query", sql)      │  通过 properties 传递 SQL 到 BE
    └────────────┬─────────────────────────────┘
                 │
                 ▼
    ┌─────────────────────────────────────────┐
    │  BE ESDataSource (C++)                    │  检测 native_query property
    │  ├─ 若存在 native_query:                  │
    │  │   创建 ESSqlReader (新)                 │  → POST /_sql with cursor
    │  │   解析列式 JSON 响应                     │  → rows → Chunk
    │  └─ 否则:                                 │
    │      走现有 ESScrollQueryBuilder 路径       │  → POST /{index}/_search?scroll
    └─────────────────────────────────────────┘
```

### 4.2 与 JDBC native_query 的对比

| 维度       | JDBC native_query                          | ES native_query                          |
| ---------- | ------------------------------------------ | ---------------------------------------- |
| 查询语言    | 源数据库 SQL                                | ES SQL（通过 `_sql` 端点）                 |
| Schema 来源 | `ResultSet.getMetaData()` of `WHERE 1=0`  | `columns` 字段 of `_sql` 响应             |
| FE 执行查询？| 是（`SELECT * FROM (query) WHERE 1=0`）   | 是（`POST /_sql` with `fetch_size:1`）   |
| BE 执行方式 | JDBC 驱动执行 SQL                           | HTTP POST 到 `/_sql` 端点                |
| 扫描范围    | 单一范围（整个查询）                         | 单一范围（非分片）                          |
| Pass-through 标记 | `JDBCTable.isQueryTable()`          | `EsTable.isQueryTable()`（新增）          |
| 查询包装    | `(query) starrocks_query` 子查询           | 原样传递到 `/_sql`                       |
| 分页方式    | JDBC ResultSet                             | ES SQL cursor                           |

### 4.3 与现有 ES 扫描链路的关系

| 维度       | 普通 ES 外表扫描                        | ES native_query                      |
| ---------- | -------------------------------------- | ------------------------------------ |
| 扫描范围    | 每分片一个 `TEsScanRange`              | 单一扫描范围（集群级 `_sql` 端点）       |
| 查询构造    | BE `ESScrollQueryBuilder` 从谓词构建 DSL | 直接使用用户 SQL，发送到 `/_sql`       |
| 分页方式    | scroll API (`/_search?scroll=`)        | cursor (`POST /_sql` with cursor)    |
| 响应格式    | 文档导向 (`hits.hits[]._source`)       | 列式 (`columns` + `rows`)            |
| Schema 来源 | ES index `_mapping`                    | ES SQL `columns` 元数据              |
| 谓词下推    | `QueryConverter` 将 conjuncts → DSL    | 无（SQL 已包含全部过滤条件）            |

---

## 5. 核心实现

### 5.1 通用化改造（FE 框架层）

ES native_query 的实现需要先对 JDBC 专属的框架代码进行**通用化改造**，使其能够处理任意 Catalog 类型的 pass-through 查询表。

#### 5.1.1 引入 PassThroughQueryTable 接口

**新增文件**：`fe/fe-core/src/main/java/com/starrocks/catalog/PassThroughQueryTable.java`

```java
package com.starrocks.catalog;

/**
 * Marker interface for tables created by native_query table function.
 * Both JDBCTable and EsTable implement this to indicate they represent
 * a pass-through query rather than a real table.
 */
public interface PassThroughQueryTable {
    boolean isQueryTable();
    String getPassThroughQuery();
    void setPassThroughQuery(String query);
}
```

**设计理由**：
- 避免在 `QueryAnalyzer` 和 `RelationTransformer` 中使用 `instanceof` 多重判断
- `JDBCTable` 和 `EsTable` 各自实现此接口，保持类型安全
- `TableFunctionRelation.queryTable` 的类型从 `JDBCTable` 放宽为 `Table`，通过此接口做标记

#### 5.1.2 JDBCTable 实现接口

**文件**：`fe/fe-core/src/main/java/com/starrocks/catalog/JDBCTable.java`

```java
public class JDBCTable extends Table implements GsonPostProcessable, PassThroughQueryTable {
    // 已有字段和方法保持不变
    // isQueryTable() 和 setPassThroughQuery() 已存在，只需加 implements 声明

    @Override
    public String getPassThroughQuery() {
        // 返回已存储的 pass-through 查询（即 jdbcTable 字段值）
        return queryTable ? jdbcTable : null;
    }
}
```

#### 5.1.3 TableFunctionRelation 类型放宽

**文件**：`fe/fe-core/src/main/java/com/starrocks/sql/ast/TableFunctionRelation.java`

**当前代码（L18, L44, L91-97）**：
```java
import com.starrocks.catalog.JDBCTable;     // L18
...
private JDBCTable queryTable;                // L44 - 硬编码 JDBC 类型

public JDBCTable getQueryTable() {           // L91
    return queryTable;
}
public void setQueryTable(JDBCTable queryTable) {  // L95
    this.queryTable = queryTable;
}
```

**改为**：
```java
import com.starrocks.catalog.Table;          // 改为 Table 基类
// 移除 JDBCTable import
...
private Table queryTable;                    // L44 - 放宽为 Table

public Table getQueryTable() {               // L91
    return queryTable;
}
public void setQueryTable(Table queryTable) {  // L95
    this.queryTable = queryTable;
}
```

**影响范围**：这是**关键节点改动**。放宽后：
- `AuthorizerStmtVisitor.NativeQueryCatalogCollector` — **零改动**（只调用 `getCatalogName()`，该方法在 `Table` 基类上）
- `QueryAnalyzer` — 需要调整 `resolveJdbcQueryTable` 返回类型（见 5.1.4）
- `RelationTransformer` — 需要按 Table 类型分派计划构建（见 5.1.5）

> **注意**：`TableFunctionRelation` 已经有 `setQueryTable()` 方法，违反了 FE AGENTS.md 中的 AST 不可变原则。此改动不引入新的可变性问题，仅放宽类型。

#### 5.1.4 QueryAnalyzer 通用化

**文件**：`fe/fe-core/src/main/java/com/starrocks/sql/analyzer/QueryAnalyzer.java`

**改动1：泛化 resolveJdbcQueryTable → resolveQueryTable**

当前代码（L229-252）返回 `JDBCTable`，并在 L247 检查 `instanceof JDBCTable`：

```java
// 当前（JDBC 专属）
private JDBCTable resolveJdbcQueryTable(JdbcQueryTableFunctionName functionName, String passThroughQuery) {
    ...
    table = metadata.get().getTableFromQuery(session, currentDb, passThroughQuery);  // L242 - 通用 SPI 调用
    ...
    if (!(table instanceof JDBCTable jdbcTable) || !jdbcTable.isQueryTable()) {  // L247 - JDBC 硬编码
        throw new SemanticException("Catalog '%s' does not support JDBC query table function", ...);
    }
    return jdbcTable;
}
```

改为：

```java
// 通用化
private Table resolveQueryTable(QueryTableFunctionName functionName, String passThroughQuery) {
    Optional<ConnectorMetadata> metadata = metadataMgr.getOptionalMetadata(functionName.catalogName);
    if (metadata.isEmpty()) {
        throw new SemanticException("Unknown catalog '%s'", functionName.catalogName);
    }

    String currentDb = null;
    if (functionName.catalogName.equalsIgnoreCase(session.getCurrentCatalog())) {
        currentDb = session.getDatabase();
    }

    Table table;
    try {
        table = metadata.get().getTableFromQuery(session, currentDb, passThroughQuery);
    } catch (RuntimeException e) {
        throw new SemanticException("Failed to resolve query table function: %s", e.getMessage());
    }

    // 通用化检查：使用 PassThroughQueryTable 接口替代 instanceof JDBCTable
    if (!(table instanceof PassThroughQueryTable queryTable) || !queryTable.isQueryTable()) {
        throw new SemanticException("Catalog '%s' does not support native query table function",
                functionName.catalogName);
    }
    return table;
}
```

**改动2：泛化 buildJdbcQueryTableScope → buildQueryTableScope**

当前代码（L254-271）参数类型为 `JDBCTable`：

```java
// 当前
private Scope buildJdbcQueryTableScope(TableFunctionRelation node, JDBCTable jdbcTable) {
    node.setQueryTable(jdbcTable);
    ...
    for (Column column : jdbcTable.getFullSchema()) {  // getFullSchema() 在 Table 基类上
        ...
    }
    ...
}
```

改为：

```java
// 通用化
private Scope buildQueryTableScope(TableFunctionRelation node, Table queryTable) {
    node.setQueryTable(queryTable);
    TableName relationName = node.getResolveTableName();
    ImmutableList.Builder<Field> fields = ImmutableList.builder();
    for (Column column : queryTable.getFullSchema()) {  // getFullSchema() 在 Table 基类上
        String columnName = column.getName();
        fields.add(new Field(columnName, column.getType(), relationName,
                new SlotRef(relationName, columnName, columnName), true, column.isAllowNull()));
    }
    Scope outputScope = new Scope(RelationId.of(node), new RelationFields(fields.build()));
    node.setScope(outputScope);
    return outputScope;
}
```

**改动3：泛化类名和常量**

```java
// 重命名（可选但推荐）
private static class QueryTableFunctionName {  // 原 JdbcQueryTableFunctionName
    private final String catalogName;
    ...
}

// 错误消息通用化
private static final String QUERY_TABLE_FUNCTION_USAGE =
        "native query table function only supports TABLE(<catalog>.native_query('<sql>'))";
// 原: "JDBC query table function only supports ..."
```

**改动4：normalizePassThroughQuery 提取为公共工具**

当前 `JDBCTable.normalizePassThroughQuery()` 是 JDBCTable 的静态方法。ES 也需要相同的校验逻辑（trim + 去分号 + 去注释 + SELECT 校验），提取到公共工具类：

**新增文件**：`fe/fe-core/src/main/java/com/starrocks/catalog/PassThroughQueryValidator.java`

```java
package com.starrocks.catalog;

import org.apache.commons.lang3.StringUtils;

public class PassThroughQueryValidator {
    /**
     * Normalize a pass-through query: trim, strip trailing semicolons,
     * strip leading comments, and validate it starts with SELECT.
     */
    public static String normalize(String query) {
        String normalizedQuery = StringUtils.trimToEmpty(query);
        while (normalizedQuery.endsWith(";")) {
            normalizedQuery = StringUtils.stripEnd(
                normalizedQuery.substring(0, normalizedQuery.length() - 1), null);
        }
        if (normalizedQuery.isEmpty()) {
            throw new IllegalArgumentException("pass-through query cannot be empty");
        }
        validateSelectOnly(normalizedQuery);
        return normalizedQuery;
    }

    private static void validateSelectOnly(String query) {
        String leadingSql = stripLeadingComments(query);
        if (!startsWithSqlKeyword(leadingSql, "select")) {
            throw new IllegalArgumentException(
                "native query table function only supports SELECT queries");
        }
    }

    // stripLeadingComments 和 startsWithSqlKeyword 从 JDBCTable 迁移
    // ...
}
```

`JDBCTable.normalizePassThroughQuery()` 改为委托调用：

```java
public static String normalizePassThroughQuery(String query) {
    return PassThroughQueryValidator.normalize(query);
}
```

#### 5.1.5 RelationTransformer 通用化

**文件**：`fe/fe-core/src/main/java/com/starrocks/sql/optimizer/transformer/RelationTransformer.java`

当前代码（L1166-1246）在 `visitTableFunction` 中检测 `getQueryTable() != null`，然后调用 `buildJdbcQueryTablePlan`，后者硬编码使用 `LogicalJDBCScanOperator`。

改为按 Table 类型分派：

```java
@Override
public LogicalPlan visitTableFunction(TableFunctionRelation node, ExpressionMapping context) {
    if (node.getQueryTable() != null) {
        return buildQueryTablePlan(node);  // 通用入口
    }
    // ... 常规 builtin 表函数逻辑 ...
}

private LogicalPlan buildQueryTablePlan(TableFunctionRelation node) {
    Table table = node.getQueryTable();

    if (table instanceof JDBCTable) {
        return buildJdbcQueryTablePlan(node, (JDBCTable) table);  // 原有 JDBC 逻辑
    } else if (table instanceof EsTable) {
        return buildEsQueryTablePlan(node, (EsTable) table);     // 新增 ES 逻辑
    } else {
        throw new StarRocksException("Unsupported query table type: " + table.getClass().getSimpleName());
    }
}

// 原有 JDBC 方法（签名改为接收 JDBCTable 参数）
private LogicalPlan buildJdbcQueryTablePlan(TableFunctionRelation node, JDBCTable table) {
    // 原有逻辑不变，只是从参数获取 table 而非 node.getQueryTable()
    ...
    LogicalScanOperator scanOperator = new LogicalJDBCScanOperator(table, ...);
    ...
}

// 新增 ES 方法
private LogicalPlan buildEsQueryTablePlan(TableFunctionRelation node, EsTable table) {
    List<Field> relationFields = node.getRelationFields().getAllFields();
    List<Column> fullSchema = table.getFullSchema();

    int relationId = columnRefFactory.getNextRelationId();
    Map<ColumnRefOperator, Column> colRefToColumnMetaMap = Maps.newHashMap();
    Map<Column, ColumnRefOperator> columnMetaToColRefMap = Maps.newHashMap();

    for (int i = 0; i < fullSchema.size(); i++) {
        Column column = fullSchema.get(i);
        Field field = relationFields.get(i);
        ColumnRefOperator columnRef = columnRefFactory.create(
                field.getName(), field.getType(), column.isAllowNull());
        columnRefFactory.updateColumnToRelationIds(columnRef.getId(), relationId);
        columnRefFactory.updateColumnRefToColumns(columnRef, column, table);
        colRefToColumnMetaMap.put(columnRef, column);
        columnMetaToColRefMap.put(column, columnRef);
    }

    // 复用 LogicalEsScanOperator（与普通 ES 外表完全相同的算子）
    LogicalScanOperator scanOperator = new LogicalEsScanOperator(table,
            colRefToColumnMetaMap, columnMetaToColRefMap,
            Operator.DEFAULT_LIMIT, null, null);
    return new LogicalPlan(new OptExprBuilder(scanOperator, ...), outputVariables, List.of());
}
```

同样，`visitNormalizedTableFunction`（L1248-1260）的分派逻辑已经是通用的（只检查 `getQueryTable() != null`），无需改动。

#### 5.1.6 权限校验（无需改动）

**文件**：`fe/fe-core/src/main/java/com/starrocks/sql/analyzer/AuthorizerStmtVisitor.java`

`NativeQueryCatalogCollector`（L507-529）的逻辑完全通用：
- 调用 `node.getQueryTable().getCatalogName()` — `getCatalogName()` 在 `Table` 基类上
- 校验 `USAGE` 权限 — 与 Catalog 类型无关

**一旦 `TableFunctionRelation.queryTable` 放宽为 `Table` 类型，此文件零改动即可支持 ES。**

### 5.2 ES Schema 推断

**文件**：`fe/fe-core/src/main/java/com/starrocks/connector/elasticsearch/ElasticsearchMetadata.java`

新增 `getTableFromQuery()` 方法，通过执行 ES SQL 探测查询来推断结果集 Schema：

```java
@Override
public Table getTableFromQuery(ConnectContext context, String dbName, String query) {
    // 1. 规范化查询（复用公共校验逻辑）
    String normalizedQuery = PassThroughQueryValidator.normalize(query);

    // 2. 执行 ES SQL 探测查询，获取 columns 元数据
    //    POST /_sql  {"query": "<normalized_query>", "fetch_size": 1}
    //    响应中包含 "columns": [{"name": "xxx", "type": "yyy"}, ...]
    List<EsSqlColumn> esColumns;
    try {
        esColumns = esRestClient.probeEsSqlSchema(normalizedQuery);
    } catch (Exception e) {
        throw new StarRocksConnectorException(
            "Failed to infer schema for ES native query: " + e.getMessage(), e);
    }

    if (esColumns.isEmpty()) {
        throw new StarRocksConnectorException("ES native query returned no columns");
    }

    // 3. 将 ES SQL 类型映射为 StarRocks Column
    List<Column> fullSchema = Lists.newArrayList();
    for (EsSqlColumn esCol : esColumns) {
        Type srType = EsUtil.convertEsSqlType(esCol.getType());
        fullSchema.add(new Column(esCol.getName(), srType, true));  // native query 列默认可空
    }

    // 4. 构建合成的 EsTable
    // ⚠️ EsTable 构造函数会调用 validate(properties)，要求 properties 中包含非空的
    // "hosts"（以 http:// 或 https:// 开头）和 "index"。
    // native_query 没有用户指定的索引，但必须传入 catalog 级别的 hosts 配置
    // 和一个占位 index（BE 在 native_query 模式下忽略 index，见 §5.5.1）
    Map<String, String> tableProps = new HashMap<>();
    tableProps.put(EsTable.KEY_HOSTS, esConfig.getHosts());       // catalog 的 ES 集群地址
    tableProps.put(EsTable.KEY_INDEX, "_native_query_placeholder"); // 占位索引（BE 忽略）
    tableProps.put(EsTable.KEY_USER, esConfig.getUser());
    tableProps.put(EsTable.KEY_PASSWORD, esConfig.getPassword());
    tableProps.put(EsTable.KEY_ES_NET_SSL, String.valueOf(esConfig.isSslEnabled()));
    tableProps.put(EsTable.KEY_TRANSPORT, "http");                 // native_query 强制 HTTP 传输
    if (esConfig.getTimeZone() != null) {
        tableProps.put(EsTable.KEY_TIME_ZONE, esConfig.getTimeZone());
    }

    int tableId = ConnectorTableId.CONNECTOR_ID_GENERATOR.getNextId().asInt();
    EsTable queryTable = new EsTable(tableId, "_query_" + tableId,
            fullSchema, dbName, catalogName, tableProps);
    queryTable.setPassThroughQuery(normalizedQuery);  // 设置 passThroughQuery + queryTable=true
    return queryTable;
}
```

#### 5.2.1 EsRestClient 新增 ES SQL 探测方法

**文件**：`fe/fe-core/src/main/java/com/starrocks/connector/elasticsearch/EsRestClient.java`

```java
/**
 * Probe ES SQL schema by executing the query with fetch_size=1.
 * Returns column metadata from the _sql response.
 * The cursor is immediately closed after reading columns.
 */
public List<EsSqlColumn> probeEsSqlSchema(String sqlQuery) {
    // 构造请求体
    JsonObject requestBody = new JsonObject();
    requestBody.addProperty("query", sqlQuery);
    requestBody.addProperty("fetch_size", 1);  // 只取 1 行用于获取 columns
    // 注入 time_zone：确保 ES 返回的 datetime 列值与 session 时区一致
    // 与 ESScanReader 在 BE 侧处理 KEY_TIME_ZONE 的语义对齐
    if (timeZone != null) {
        requestBody.addProperty("time_zone", timeZone);
    }

    // POST /_sql
    String response = executePost("/_sql", requestBody.toString());

    JsonObject json = JsonParser.parseString(response).getAsJsonObject();

    // 解析 columns
    List<EsSqlColumn> columns = new ArrayList<>();
    if (json.has("columns")) {
        for (JsonElement colElem : json.getAsJsonArray("columns")) {
            JsonObject colObj = colElem.getAsJsonObject();
            columns.add(new EsSqlColumn(
                colObj.get("name").getAsString(),
                colObj.get("type").getAsString()
            ));
        }
    }

    // 关闭 cursor（如果存在）
    if (json.has("cursor")) {
        String cursor = json.get("cursor").getAsString();
        try {
            closeEsSqlCursor(cursor);
        } catch (Exception e) {
            LOG.warn("Failed to close ES SQL cursor during schema probe", e);
        }
    }

    return columns;
}

/**
 * Execute POST request to ES.
 * Reuses the same SSL client selection as execute() to ensure HTTPS clusters work.
 * Unlike existing GET-only methods, this sends a JSON body.
 */
private String executePost(String path, String jsonBody) {
    // ⚠️ 必须复用 execute() 的 SSL 客户端选择逻辑，否则 HTTPS ES 集群会连接失败
    OkHttpClient client = sslEnabled ? getOrCreateSSLClient() : NETWORK_CLIENT;

    // 遍历所有节点尝试请求，带 failover（与 execute() 的节点遍历逻辑一致）
    for (String node : nodes) {
        try {
            RequestBody body = RequestBody.create(jsonBody, MediaType.parse("application/json"));
            Request request = new Request.Builder()
                    .url(node + path)
                    .post(body)
                    .build();
            if (!Strings.isNullOrEmpty(userName)) {
                request = request.newBuilder()
                        .header("Authorization", Credentials.basic(userName, passwd))
                        .build();
            }
            try (Response response = client.newCall(request).execute()) {
                if (!response.isSuccessful()) {
                    LOG.warn("ES POST {}{} returned HTTP {}", node, path, response.code());
                    continue;  // try next node
                }
                return response.body().string();
            }
        } catch (Exception e) {
            LOG.warn("Failed to execute POST to {}{}", node, path, e);
        }
    }
    throw new StarRocksConnectorException("All ES nodes failed for POST " + path);
}

/**
 * Close an ES SQL cursor to free server state.
 * POST /_sql/close  {"cursor": "..."}
 */
private void closeEsSqlCursor(String cursor) {
    JsonObject body = new JsonObject();
    body.addProperty("cursor", cursor);
    executePost("/_sql/close", body.toString());
}
```

> **注意**：现有 `EsRestClient` 只有 GET 方法（`execute(path)`）。ES SQL 需要 POST 请求发送 JSON body，因此需要新增 `executePost(path, jsonBody)` 方法。
>
> **⚠️ SSL 关键**：`executePost` **必须**复用 `execute()` 的 SSL 客户端选择逻辑（`sslEnabled ? getOrCreateSSLClient() : NETWORK_CLIENT`）。现有 `execute()` 在 L231 通过 `sslEnabled` 切换 SSL 客户端。如果 `executePost` 不做同样处理，HTTPS ES 集群（生产环境常见）上 schema 探测会连接失败，导致整个 native_query 特性不可用。

#### 5.2.2 ES SQL 类型映射

**文件**：`fe/fe-core/src/main/java/com/starrocks/connector/elasticsearch/EsUtil.java`

新增 `convertEsSqlType()` 方法，将 ES SQL 返回的类型名映射为 StarRocks 类型：

```java
/**
 * Convert ES SQL type name (from _sql response columns) to StarRocks Type.
 * ES SQL type names differ slightly from ES mapping type names.
 */
public static Type convertEsSqlType(String esSqlType) {
    if (esSqlType == null) {
        return Type.NULL;
    }
    switch (esSqlType.toLowerCase()) {
        case "boolean":
            return Type.BOOLEAN;
        case "byte":
            return Type.TINYINT;
        case "short":
            return Type.SMALLINT;
        case "integer":
        case "int":
            return Type.INT;
        case "long":
            return Type.BIGINT;
        case "unsigned_long":
            return Type.LARGEINT;
        case "float":
        case "half_float":
            return Type.FLOAT;
        case "double":
        case "scaled_float":
            return Type.DOUBLE;
        case "datetime":
        case "date":
            return Type.DATETIME;
        case "time":
            return Type.TIME;
        case "keyword":
        case "text":
        case "ip":
        case "version":
            return Type.VARCHAR;
        case "binary":
            return Type.VARBINARY;
        case "object":
        case "nested":
            return Type.JSON;
        case "null":
            return Type.NULL;
        default:
            // 未知类型默认为 VARCHAR，避免查询失败
            return Type.VARCHAR;
    }
}
```

**ES SQL 类型与 ES Mapping 类型的差异**：

| ES Mapping 类型 | ES SQL 类型 | StarRocks 类型 |
|----------------|-------------|----------------|
| `text` | `text` | VARCHAR |
| `keyword` | `keyword` | VARCHAR |
| `date` | `datetime` | DATETIME |
| `integer` | `integer` | INT |
| `long` | `long` | BIGINT |
| `float` | `float` | FLOAT |
| `double` | `double` | DOUBLE |
| `boolean` | `boolean` | BOOLEAN |
| `ip` | `ip` | VARCHAR |
| `nested` | `nested` | JSON |

### 5.3 EsTable 扩展

**文件**：`fe/fe-core/src/main/java/com/starrocks/catalog/EsTable.java`

#### 5.3.1 新增 queryTable 标志和 passThroughQuery 字段

```java
public class EsTable extends Table implements GsonPostProcessable, PassThroughQueryTable {

    // ... 现有字段 ...

    // 新增：native_query 标志和查询存储
    @SerializedName(value = "qt")
    private boolean queryTable;

    private String passThroughQuery;  // 不序列化，运行时使用

    @Override
    public boolean isQueryTable() {
        return queryTable;
    }

    @Override
    public String getPassThroughQuery() {
        return passThroughQuery;
    }

    @Override
    public void setPassThroughQuery(String query) {
        this.passThroughQuery = query;
        this.queryTable = true;
        // native query 不需要分片信息，跳过 EsMetaStateTracker
    }
}
```

#### 5.3.2 toThrift 扩展

**文件**：`fe/fe-core/src/main/java/com/starrocks/catalog/EsTable.java`

`TEsTable` 目前是空 struct。native query 的 SQL 通过 `TEsScanNode.properties` 传递，**不需要修改 `TEsTable`**。

但 `EsTable.toThrift()` 需要确保 query table 不依赖 `esTablePartitions`：

```java
@Override
public TTableDescriptor toThrift() {
    TTableDescriptor tTableDescriptor = new TTableDescriptor(id, TTableType.ES_TABLE,
            fullSchema.size(), 0, getName(), "");
    tTableDescriptor.setEsTable(new TEsTable());
    return tTableDescriptor;
}
// 无需改动——TEsTable 是空 struct，所有信息通过 TEsScanNode 传递
```

### 5.4 EsScanNode 扫描范围

**文件**：`fe/fe-core/src/main/java/com/starrocks/planner/EsScanNode.java`

#### 5.4.1 跳过分片扫描范围

普通 ES 外表通过 `computeShardLocations()` 为每个分片创建 `TEsScanRange`。native query 不需要分片级扫描——ES SQL 是集群级 API。

新增方法：

```java
/**
 * Create a single scan range for native query (non-shard).
 * Uses the ES cluster's configured hosts directly.
 */
public List<TScanRangeLocations> computeQueryTableScanRanges() {
    // 获取 ES 集群节点地址
    List<TNetworkAddress> esHosts = Lists.newArrayList();
    for (String seed : table.getSeeds()) {
        // 解析 host:port
        String[] parts = seed.split(":");
        esHosts.add(new TNetworkAddress(parts[0], Integer.parseInt(parts[1])));
    }

    // 获取可用的 compute nodes
    assignNodes();
    if (nodeList.isEmpty()) {
        throw new StarRocksException("No alive backends or compute nodes");
    }

    // 创建单一扫描范围
    TScanRangeLocations locations = new TScanRangeLocations();

    // 分配到 1-3 个 compute node
    int numNode = Math.min(3, nodeList.size());
    for (int i = 0; i < numNode; i++) {
        TScanRangeLocation location = new TScanRangeLocation();
        ComputeNode be = nodeList.get(i);
        location.setBackend_id(be.getId());
        location.setServer(new TNetworkAddress(be.getHost(), be.getBePort()));
        locations.addToLocations(location);
    }

    // 创建 TEsScanRange（使用哨兵值）
    TEsScanRange esScanRange = new TEsScanRange();
    esScanRange.setEs_hosts(esHosts);
    esScanRange.setIndex("");        // 空索引——SQL 查询不绑定特定索引
    esScanRange.setShard_id(-1);     // 哨兵值——表示非分片扫描

    TScanRange scanRange = new TScanRange();
    scanRange.setEs_scan_range(esScanRange);
    locations.setScan_range(scanRange);

    return Collections.singletonList(locations);
}
```

#### 5.4.2 toThrift 传递 native query

在 `toThrift()` 中，当 `isQueryTable()` 为 true 时，将 SQL 查询放入 `properties`：

```java
@Override
protected void toThrift(TPlanNode msg) {
    if (EsTable.KEY_TRANSPORT_HTTP.equals(table.getTransport())) {
        msg.node_type = TPlanNodeType.ES_HTTP_SCAN_NODE;
    } else {
        msg.node_type = TPlanNodeType.ES_SCAN_NODE;
    }

    Map<String, String> properties = Maps.newHashMap();
    properties.put(EsTable.KEY_USER, table.getUserName());
    properties.put(EsTable.KEY_PASSWORD, table.getPasswd());
    properties.put(EsTable.KEY_ES_NET_SSL, String.valueOf(table.sslEnabled()));

    String time_zone = table.getTimeZone();
    if (time_zone != null) {
        properties.put(EsTable.KEY_TIME_ZONE, time_zone);
    }

    // 新增：native query 传递
    if (table.isQueryTable()) {
        properties.put(EsTable.KEY_NATIVE_QUERY, table.getPassThroughQuery());
    }

    TEsScanNode esScanNode = new TEsScanNode(desc.getId().asInt());
    esScanNode.setProperties(properties);

    // native query 不需要 docvalue_context / fields_context（Schema 来自 SQL 响应）
    if (!table.isQueryTable()) {
        if (table.isDocValueScanEnable()) {
            esScanNode.setDocvalue_context(table.docValueContext());
            properties.put(EsTable.KEY_DOC_VALUES_MODE,
                    String.valueOf(useDocValueScan(desc, table.docValueContext())));
        }
        if (table.isKeywordSniffEnable() && table.fieldsContext().size() > 0) {
            esScanNode.setFields_context(table.fieldsContext());
        }
    }

    msg.es_scan_node = esScanNode;
    setConnectorCatalogType(msg);
}
```

**EsTable 新增常量**：

```java
public static final String KEY_NATIVE_QUERY = "native_query";
```

#### 5.4.3 物理计划构建集成

在 `PhysicalEsScanOperator` 或 `EsScanNode` 的创建逻辑中，需要根据 `isQueryTable()` 选择扫描范围创建方式：

```java
// 在 EsScanOperator physical plan builder 中
if (esTable.isQueryTable()) {
    esScanNode.setShardScanRanges(esScanNode.computeQueryTableScanRanges());
} else {
    // 原有逻辑：使用 esTablePartitions 的分片信息
    esScanNode.setShardScanRanges(esScanNode.computeShardLocations(selectedIndex));
}
```

### 5.5 BE 执行

#### 5.5.1 ESDataSource 分支

**文件**：`be/src/connector/elasticsearch/es_connector.cpp`

在 `_create_scanner()` 方法中检测 `native_query` property，选择不同的 reader。

> **⚠️ 分支位置关键**：现有 `_create_scanner()` 在 L199-206 **无条件**设置 `KEY_INDEX`/`KEY_SHARD`/`KEY_HOST_PORT` 等 property，然后在 L214 调用 `ESScrollQueryBuilder::build()`（需要非空 index/shard 构造 scroll URL）。native_query 分支**必须插入在 `ESScrollQueryBuilder::build()` 调用之前**（理想位置：在 `_properties` 从 `es_scan_node` 初始化之后、scroll query 构建之前），并提前 return。如果分支放在 `build()` 之后，空 index 会生成畸形的 `/{index}/_search` URL，导致 ES 返回 404。

```cpp
Status ESDataSource::_create_scanner() {
    // === native_query 分支：必须在 ESScrollQueryBuilder::build() 之前 ===
    auto it = _properties.find("native_query");
    if (it != _properties.end()) {
        // native query 模式：使用 ESSqlReader
        // ESSqlReader 直接从 TEsScanRange.es_hosts 获取 ES 节点地址，
        // 不依赖 KEY_INDEX/KEY_SHARD（这些对 SQL 模式无意义）
        _native_query = it->second;
        _scanner = std::make_unique<ESSqlReader>(
            _es_hosts,           // 从 TEsScanRange 获取
            _properties,         // user/pass/ssl/timezone
            _native_query,       // SQL 查询字符串
            _chunk_size,         // batch size（已做上限保护，见 §5.5.5）
            _runtime_state       // 用于超时控制
        );
        return Status::OK();
        // ⚠️ 提前返回，跳过下方 ESScrollQueryBuilder::build() 路径
    }

    // === 原有逻辑：使用 ESScrollQueryBuilder + ESScanReader ===
    // L199: _properties[KEY_INDEX] = es_scan_range.index;
    // L203: _properties[KEY_SHARD] = std::to_string(es_scan_range.shard_id);
    // L206: _properties[KEY_HOST_PORT] = ...;
    // L214: ESScrollQueryBuilder query_builder(...);  // ← native_query 分支必须在此行之前返回
    ESScrollQueryBuilder query_builder(...);
    ...
}
```

#### 5.5.2 ESSqlReader 实现

**新增文件**：`be/src/connector/elasticsearch/es_sql_reader.h`

```cpp
#pragma once

#include "common/status.h"
#include "connector/elasticsearch/es_scroll_parser.h"

namespace starrocks {

// ES SQL response column metadata
struct EsSqlColumn {
    std::string name;
    std::string type;
};

// Reader for ES SQL (_sql endpoint)
// Unlike ESScanReader (which uses scroll API with per-shard _search),
// ESSqlReader uses the cluster-level _sql endpoint with cursor pagination.
class ESSqlReader {
public:
    ESSqlReader(const std::vector<TNetworkAddress>& es_hosts,
                const std::map<std::string, std::string>& properties,
                const std::string& sql_query,
                int batch_size,
                RuntimeState* state);

    ~ESSqlReader();

    // Open the reader: send first _sql request, parse columns
    Status open();

    // Get next batch of rows
    Status get_next(Chunk* chunk);

    // Close: release cursor if exists
    void close();

private:
    // Send POST /_sql with query (first request)
    Status _execute_initial_query();

    // Send POST /_sql with cursor (subsequent requests)
    Status _execute_cursor_fetch();

    // Send POST /_sql/close to release cursor
    Status _close_cursor();

    // Parse _sql JSON response into chunk
    Status _parse_response(const std::string& response, Chunk* chunk);

    // HTTP POST helper
    Status _http_post(const std::string& path, const std::string& body,
                      std::string* response);

    std::vector<TNetworkAddress> _es_hosts;
    std::map<std::string, std::string> _properties;
    std::string _sql_query;
    int _batch_size;
    RuntimeState* _state;

    std::string _cursor;           // ES SQL cursor for pagination
    std::vector<EsSqlColumn> _columns;  // Column metadata (first response only)
    bool _first_request_done = false;
    bool _eos = false;
    int _current_host_index = 0;   // failover index
};

} // namespace starrocks
```

**新增文件**：`be/src/connector/elasticsearch/es_sql_reader.cpp`

```cpp
#include "connector/elasticsearch/es_sql_reader.h"

namespace starrocks {

ESSqlReader::ESSqlReader(...) : ... {}

Status ESSqlReader::open() {
    return _execute_initial_query();
}

Status ESSqlReader::_execute_initial_query() {
    // Build request body
    // {"query": "<sql>", "fetch_size": <batch_size>, "time_zone": "<tz>"}
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("query", _sql_query, alloc);
    doc.AddMember("fetch_size", _batch_size, alloc);
    // 注入 time_zone：确保 datetime 列值以 session 时区返回
    auto it_tz = _properties.find("time_zone");
    if (it_tz != _properties.end() && !it_tz->second.empty()) {
        doc.AddMember("time_zone", it_tz->second, alloc);
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::string response;
    RETURN_IF_ERROR(_http_post("/_sql", buffer.GetString(), &response));

    // Parse response
    // {"columns": [{"name":"x","type":"long"}, ...], "rows": [[1],[2],...], "cursor": "..."}
    rapidjson::Document resp;
    resp.Parse(response.c_str());

    if (resp.HasParseError()) {
        return Status::InternalError("Failed to parse ES SQL response");
    }

    // Check for error
    if (resp.HasMember("error")) {
        return Status::InternalError("ES SQL error: " +
            resp["error"]["type"].GetString() + " - " +
            resp["error"]["reason"].GetString());
    }

    // Parse columns (only present in first response)
    if (resp.HasMember("columns")) {
        const auto& cols = resp["columns"].GetArray();
        for (const auto& col : cols) {
            _columns.push_back({
                col["name"].GetString(),
                col["type"].GetString()
            });
        }
    }

    // Parse rows
    if (resp.HasMember("rows")) {
        const auto& rows = resp["rows"].GetArray();
        // TODO: convert rows to chunk
        // Each row is a JSON array of values, positional match with _columns
    }

    // Save cursor for next fetch
    if (resp.HasMember("cursor")) {
        _cursor = resp["cursor"].GetString();
    } else {
        _eos = true;
    }

    _first_request_done = true;
    return Status::OK();
}

Status ESSqlReader::get_next(Chunk* chunk) {
    if (_eos) {
        return Status::EndOfFile("EOS");
    }

    if (!_first_request_done) {
        RETURN_IF_ERROR(_execute_initial_query());
        // First request already returned rows
        return Status::OK();
    }

    // Subsequent fetches use cursor
    RETURN_IF_ERROR(_execute_cursor_fetch());
    return Status::OK();
}

Status ESSqlReader::_execute_cursor_fetch() {
    // POST /_sql  {"cursor": "..."}
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("cursor", _cursor, alloc);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::string response;
    RETURN_IF_ERROR(_http_post("/_sql", buffer.GetString(), &response));

    rapidjson::Document resp;
    resp.Parse(response.c_str());

    if (resp.HasParseError()) {
        return Status::InternalError("Failed to parse ES SQL cursor response");
    }

    // ⚠️ 必须检查 error：cursor 可能被 ES 集群中途失效（集群重启、cursor 超时、资源压力）
    // 不检查会导致：无 "rows" 被当作空批次，无 "cursor" 被当作 EOS → 静默截断结果集
    if (resp.HasMember("error")) {
        // cursor 已失效，清理本地状态并报错（不静默吞没）
        _cursor.clear();
        _eos = true;
        return Status::InternalError("ES SQL cursor invalidated: " +
            std::string(resp["error"]["type"].GetString()) + " - " +
            std::string(resp["error"]["reason"].GetString()));
    }

    // Parse rows (columns NOT present in cursor responses)
    if (resp.HasMember("rows")) {
        const auto& rows = resp["rows"].GetArray();
        // TODO: convert rows to chunk using _columns metadata
    }

    if (resp.HasMember("cursor")) {
        _cursor = resp["cursor"].GetString();
    } else {
        _eos = true;
        _cursor.clear();
    }

    return Status::OK();
}

Status ESSqlReader::_close_cursor() {
    if (_cursor.empty()) return Status::OK();

    // POST /_sql/close  {"cursor": "..."}
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    doc.AddMember("cursor", _cursor, alloc);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::string response;
    return _http_post("/_sql/close", buffer.GetString(), &response);
}

// ⚠️ _http_post 必须复用 ESScanReader 的 SSL/认证/超时/故障转移模式
// 现有 ESScanReader（es_scan_reader.cpp:129-135, 164-171）在每次请求时调用：
//   set_basic_auth / set_content_type / trust_all_ssl / set_timeout_ms
// ESSqlReader 必须做同样处理，否则 HTTPS 集群、认证集群、超时、节点故障转移均不工作
Status ESSqlReader::_http_post(const std::string& path, const std::string& body,
                                std::string* response) {
    HttpClient client;
    // 遍历所有 ES 节点，带故障转移（与 EsRestClient.execute 的节点遍历逻辑一致）
    for (int i = 0; i < _es_hosts.size(); i++) {
        int idx = (_current_host_index + i) % _es_hosts.size();
        const auto& host = _es_hosts[idx];
        std::string url = fmt::format("{}:{}{}", host.hostname, host.port, path);

        client.reset();
        // SSL：与 ESScanReader 一致，信任所有 SSL 证书（ES 自签名证书场景）
        client.trust_all_ssl();
        // 认证：Basic Auth
        auto it_user = _properties.find("user");
        auto it_pass = _properties.find("password");
        if (it_user != _properties.end() && !it_user->second.empty()) {
            client.set_basic_auth(it_user->second, it_pass->second);
        }
        // Content-Type
        client.set_content_type("application/json");
        // 超时：复用现有 ES HTTP 超时配置
        client.set_timeout_ms(config::es_http_timeout_ms);

        auto status = client.execute_post_request(url, body, response);
        if (status.ok()) {
            _current_host_index = idx;  // 记住成功的节点
            return Status::OK();
        }
        LOG(WARNING) << "ES SQL POST failed for " << url << ": " << status.message();
    }
    return Status::InternalError("All ES nodes failed for POST " + path);
}

ESSqlReader::~ESSqlReader() {
    // 析构时 best-effort 关闭 cursor，防止 ES 侧状态泄漏
    // ES SQL cursor 有 keepalive 自动过期机制，即使关闭失败也不会永久泄漏
    if (!_cursor.empty()) {
        auto st = _close_cursor();
        if (!st.ok()) {
            LOG(WARNING) << "Failed to close ES SQL cursor in destructor: " << st.message();
        }
    }
}

} // namespace starrocks
```

#### 5.5.3 ES SQL 响应解析与类型转换

**新增文件**：`be/src/connector/elasticsearch/es_sql_parser.h/cpp`

ES SQL 响应格式与现有 scroll 响应格式完全不同：

| 维度 | Scroll 响应 (`_search`) | SQL 响应 (`_sql`) |
|------|------------------------|-------------------|
| 数据结构 | `hits.hits[]._source` (文档导向) | `rows: [[v1, v2, ...], ...]` (列式) |
| Schema | 无（需从 mapping 推断） | `columns: [{name, type}]` |
| 分页 | `_scroll_id` | `cursor` |

需要新的解析器将 ES SQL 的列式 rows 转换为 StarRocks Chunk：

```cpp
// es_sql_parser.cpp
Status EsSqlResponseParser::parse(const rapidjson::Value& rows,
                                   const std::vector<EsSqlColumn>& columns,
                                   Chunk* chunk) {
    // columns 定义了每列的名称和类型
    // rows 是二维数组：rows[i][j] 是第 i 行第 j 列的值
    //
    // 类型映射（BE 侧，与 FE EsUtil.convertEsSqlType 对应）：
    //   ES SQL "long"     → TYPE_BIGINT
    //   ES SQL "integer"  → TYPE_INT
    //   ES SQL "text"     → TYPE_VARCHAR
    //   ES SQL "datetime" → TYPE_DATETIME
    //   ...

    for (const auto& row : rows.GetArray()) {
        for (size_t col_idx = 0; col_idx < columns.size(); col_idx++) {
            const auto& val = row.GetArray()[col_idx];
            // 根据 columns[col_idx].type 将 val 转换并追加到对应列
            append_value(columns[col_idx].type, val, chunk, col_idx);
        }
    }
    return Status::OK();
}

// ⚠️ datetime 类型解析：ES SQL 返回 ISO8601 格式字符串，必须正确处理
// ES SQL datetime 列返回格式示例：
//   "2024-01-01T12:00:00.123Z"           (UTC，带毫秒)
//   "2024-01-01T12:00:00.123+08:00"      (带时区偏移)
//   "2024-01-01T12:00:00"                (无毫秒，无时区)
//
// 注意：如果请求体中注入了 time_zone（见 _execute_initial_query），
// ES 返回的 datetime 字符串已是 session 时区的本地时间，无 Z 后缀。
//
// 解析方式：使用 DateTimeValue::from_string 解析，需处理：
// 1. 'T' 分隔符（ISO8601）vs 空格分隔符（StarRocks 默认）
// 2. 毫秒部分（.123）——StarRocks DATETIME 支持微秒精度
// 3. 时区后缀（Z / +08:00）——若存在，转换为 UTC 再存为本地时间，
//    或直接忽略（若 time_zone 已在请求中注入，ES 返回的已是本地时间）
Status append_datetime_value(const rapidjson::Value& val, Column* col) {
    if (val.IsNull()) {
        col->append_null();
        return Status::OK();
    }
    const char* str = val.GetString();
    DateTimeValue dtv;
    // from_string 需要支持 ISO8601 格式（含 'T' 和毫秒）
    // 若现有 DateTimeValue::from_string 不支持，需预处理：
    //   将 'T' 替换为空格，截断毫秒和时区后缀
    std::string normalized = normalize_iso8601_datetime(str);
    if (!dtv.from_string(normalized.c_str(), normalized.length())) {
        return Status::InternalError("Failed to parse ES SQL datetime: " + std::string(str));
    }
    col->append(&dtv);
    return Status::OK();
}

// 将 ISO8601 格式归一化为 StarRocks DateTimeValue::from_string 可解析的格式
// "2024-01-01T12:00:00.123Z"      → "2024-01-01 12:00:00.123"
// "2024-01-01T12:00:00.123+08:00" → "2024-01-01 12:00:00.123"
// "2024-01-01T12:00:00"           → "2024-01-01 12:00:00"
std::string normalize_iso8601_datetime(const std::string& iso8601) {
    std::string result = iso8601;
    // 替换 T 为空格
    size_t t_pos = result.find('T');
    if (t_pos != std::string::npos) {
        result[t_pos] = ' ';
    }
    // 截断时区后缀（Z 或 +hh:mm 或 -hh:mm）
    // 注意：若 time_zone 已在请求体中注入，ES 返回本地时间无 Z 后缀，此步为 no-op
    size_t tz_pos = result.find_first_of("Z+-", 19);  // 从日期时间部分之后开始查找
    if (tz_pos != std::string::npos) {
        result = result.substr(0, tz_pos);
    }
    // 去除尾部空格
    result.erase(result.find_last_not_of(" \t") + 1);
    return result;
}
```

#### 5.5.4 BE 模块边界合规

根据 `be/AGENTS.md` 的模块边界规则：
- `ESSqlReader` 和 `EsSqlResponseParser` 放在 `be/src/connector/elasticsearch/` 目录下
- 属于 `ConnectorElasticsearch` 模块目标
- 依赖关系与现有 `ESScanReader`、`es_scroll_parser` 一致

#### 5.5.5 fetch_size 上限保护（OOM 安全）

ES SQL 的 `fetch_size` 控制 cursor 每次返回的行数，对应单次 HTTP 响应的大小。与 `_search` API 不同，ES SQL **没有** `index.max_result_window` 的上限保护。如果不做限制，宽行（多个 `text` 列）+ 大 `fetch_size` 可能导致 BE 解析整个 JSON 响应时 OOM。

**保护策略**：在 `ESDataSource._create_scanner()` 中，构造 `ESSqlReader` 之前对 `_chunk_size` 做上限保护：

```cpp
// be/src/connector/elasticsearch/es_connector.cpp
// 在创建 ESSqlReader 之前
int sql_fetch_size = _chunk_size;
// 复用现有 ES 配置作为上限（与 ESScanReader 的 batch_size 计算一致）
int max_fetch_size = config::es_index_max_result_window;  // 默认 10000
if (sql_fetch_size > max_fetch_size) {
    sql_fetch_size = max_fetch_size;
    LOG(INFO) << "ES SQL fetch_size capped to " << max_fetch_size
              << " (original chunk_size=" << _chunk_size << ")";
}

_scanner = std::make_unique<ESSqlReader>(
    _es_hosts, _properties, _native_query, sql_fetch_size, _runtime_state);
```

> **注意**：`es_index_max_result_window` 是现有 BE 配置项（`be/src/common/config.h`），默认值 10000。ES SQL 的 `fetch_size` 语义与 `_search` 的 `size` 不同（fetch_size 是 cursor 批大小，不是总结果上限），但作为行数上限保护仍然适用。如果未来需要更精细的控制，可以引入独立的 `es_sql_max_fetch_size` 配置项。

### 5.6 锁优化——无锁预解析

**复用 JDBC native_query 的两阶段解析策略**，但需要补充异常处理。

ES native_query 的 Schema 推断（`getTableFromQuery` → `POST /_sql`）涉及远程 HTTP 调用。为避免在分析期间长时间持有 `PlannerMetaLock`，使用 `ExternalTablesOnlyVisitor` 在无锁阶段完成 Schema 推断。

`ExternalTablesOnlyVisitor.visitTableFunction()` 已有的宽松版解析器 `tryParseCanonicalJdbcQueryTableFunctionName()` 只检查 `parts.size() == 2 && parts.get(1).equalsIgnoreCase("native_query")`——**与 Catalog 类型无关**，天然支持 ES。

#### ⚠️ 异常处理修复（必须）

当前 `ExternalTablesOnlyVisitor.visitTableFunction()` 在调用 `resolveJdbcQueryTable()`（通用化后为 `resolveQueryTable()`）时，只 catch 了 `normalizePassThroughQuery` 抛出的 `IllegalArgumentException`（L2201）。但 `resolveQueryTable()` 在 ES 不可达或 SQL 语法错误时会抛 `SemanticException`（`RuntimeException`），该异常**不会被**现有 catch 捕获，导致查询在无锁预解析阶段直接崩溃。

**修复方式**：在 `resolveQueryTable()` 调用处增加 `RuntimeException` catch，静默返回 null，让持锁阶段重新解析并报出规范错误：

```java
// ExternalTablesOnlyVisitor.visitTableFunction() 中的修复
Table queryTable = null;
try (Timer ignored = Tracers.watchScope("AnalyzeTable")) {
    queryTable = resolveQueryTable(functionName, passThroughQuery);
} catch (RuntimeException e) {
    // 预解析阶段静默失败：ES 不可达、SQL 语法错误等
    // 让持锁阶段（tryResolveQueryTableFunction）重新解析并报出规范错误
    LOG.debug("Pre-resolution failed for catalog {}, will retry in locked phase: {}",
            functionName.catalogName, e.getMessage());
    return null;  // 不设置 queryTable，持锁阶段会重新解析
}
node.setQueryTable(queryTable);
return null;
```

> **注意**：此修复也惠及 JDBC native_query——JDBC 的 `getTableFromQuery` 在数据库不可达时同样会抛 `RuntimeException`，现有代码存在同样的预解析崩溃问题。

### 5.7 逻辑计划到物理计划的转换

`LogicalEsScanOperator` 在物理计划构建阶段需要正确处理 query table：

**文件**：`fe/fe-core/src/main/java/com/starrocks/sql/optimizer/rule/implementation/EsScanImplementationRule.java`（或等效的物理计划构建器）

当 `EsTable.isQueryTable()` 为 true 时：
1. 跳过 `EsTablePartitions` / `EsShardPartitions` 相关的分片选择逻辑
2. 调用 `EsScanNode.computeQueryTableScanRanges()` 而非 `computeShardLocations()`
3. 跳过 `QueryConverter` 谓词下推（native query 的过滤已在 SQL 内部定义）

```java
// 物理计划构建
EsScanNode esScanNode = new EsScanNode(...);
EsTable esTable = (EsTable) scanOperator.getTable();

if (esTable.isQueryTable()) {
    // native query：单一扫描范围，无分片
    esScanNode.assignNodes();
    esScanNode.setShardScanRanges(esScanNode.computeQueryTableScanRanges());
} else {
    // 原有逻辑：分片扫描范围
    esScanNode.assignNodes();
    List<EsShardPartitions> selectedIndex = ...;  // 分片选择
    esScanNode.setShardScanRanges(esScanNode.computeShardLocations(selectedIndex));
}
```

---

## 6. 关键设计决策

### 6.1 为何选择 ES SQL 而非 Query DSL

| 方案 | 优点 | 缺点 |
|------|------|------|
| **ES SQL**（选择） | Schema 推断简洁（columns 元数据）；与 JDBC native_query UX 一致；cursor 分页自动清理 | ES SQL 有功能限制（无 JOIN、子查询受限）；需要 ES 6.3+ |
| **Query DSL** | 功能完整；可复用现有 scroll 管道 | Schema 推断困难（需 probe query + mapping）；与 JDBC native_query UX 不一致 |
| **混合** | 两种格式都支持 | 实现复杂度高；参数校验逻辑分叉 |

ES SQL 的 `columns` 元数据是决定性因素——它让 Schema 推断变得简单可靠，与 JDBC 的 `ResultSet.getMetaData()` 模式高度一致。

### 6.2 为何不复用 JDBC 扫描链路

JDBC native_query 复用 JDBC 扫描链路（`LogicalJDBCScanOperator` → `JDBCScanNode` → BE JDBC Scanner → JDBC Bridge）。ES 不走 JDBC 驱动，而是通过 HTTP REST API 与 ES 集群通信。两者的执行协议完全不同：

| 维度 | JDBC | ES |
|------|------|-----|
| 协议 | JDBC 驱动（Java） | HTTP REST |
| 连接 | JDBC Connection | HTTP Client（OkHttp / libcurl） |
| 执行 | `Statement.executeQuery()` | `POST /_sql` |
| 结果集 | `ResultSet` 迭代器 | JSON 响应 + cursor |
| Bridge | 需要 JDBC Bridge（Java 进程） | 直接 HTTP 调用 |

因此 ES 必须使用自己的扫描链路（`LogicalEsScanOperator` → `EsScanNode` → BE ESDataSource）。

### 6.3 为何需要通用化改造而非新增 ES 专属分支

在 `QueryAnalyzer` 和 `RelationTransformer` 中有两个选择：

**方案 A（选择）：通用化改造**
- 将 `TableFunctionRelation.queryTable` 从 `JDBCTable` 放宽为 `Table`
- 引入 `PassThroughQueryTable` 接口
- 在 `resolveQueryTable` 中使用接口检查替代 `instanceof JDBCTable`
- 优点：未来支持其他 Catalog（如 Hive SQL pass-through）零改动
- 缺点：需要修改 JDBC 现有代码（但改动是类型安全的，不改变行为）

**方案 B：ES 专属分支**
- 在 `QueryAnalyzer` 中新增 `tryResolveEsQueryTableFunction()` 方法
- 在 `TableFunctionRelation` 中新增 `esQueryTable` 字段
- 优点：不触碰 JDBC 代码
- 缺点：代码重复；每增加一个 Catalog 类型就要重复一遍

选择方案 A 因为 native_query 的设计目标是**通用 pass-through 查询框架**，而非 JDBC/ES 特定功能。

### 6.4 为何使用 properties 传递查询而非 Thrift 新字段

`TEsScanNode.properties` 是 `map<string, string>`，已经用于传递 user/password/ssl/timezone 等配置。通过新增一个 key `"native_query"` 传递 SQL 查询：

- **零 Thrift 变更**：不修改 `TEsScanNode` / `TEsScanRange` 结构体
- **向后兼容**：不传递该 key 时行为完全不变
- **符合现有模式**：与 user/password 等配置的传递方式一致

替代方案（新增 Thrift 字段 `optional string native_query`）虽然更类型安全，但需要修改 `gensrc/thrift/PlanNodes.thrift` 并重新生成代码，收益有限。

### 6.5 为何跳过分片扫描范围

普通 ES 外表为每个 ES 分片创建独立的 `TEsScanRange`，BE 并行扫描各分片。ES SQL（`_sql` 端点）是集群级 API，不针对特定分片：

- ES SQL 内部自动路由到相关分片
- cursor 是集群级状态，不绑定分片
- 多分片并行扫描会导致重复结果

因此 native query 创建单一扫描范围，使用 ES 集群的配置节点地址。

### 6.6 为何使用哨兵值而非新 Thrift 结构

`TEsScanRange` 的 `index` 和 `shard_id` 是 `required` 字段（不能改为 optional，因为 Thrift 不允许将 required 改为 optional）。对于 native query，这两个字段无意义：

- `index = ""`：空字符串，BE 检测到空索引时走 SQL 路径
- `shard_id = -1`：哨兵值，表示非分片扫描

BE 在 `ESDataSource._create_scanner()` 中通过 `properties["native_query"]` 是否存在来决定走 SQL 路径还是 scroll 路径，不依赖 `index` / `shard_id` 的值。

---

## 7. 数据流总结

```
┌──────────────────────────────────────────────────────────────────┐
│ 1. 用户 SQL                                                       │
│    TABLE(es0.native_query('SELECT id, name FROM logs WHERE ...')) │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 2. SQL Parser → TableFunctionRelation                             │
│    functionName = "es0.native_query"                              │
│    args = [StringLiteral("SELECT id, name FROM logs WHERE ...")]  │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 3. ExternalTablesOnlyVisitor（无锁预解析）                         │
│    tryParseCanonicalQueryTableFunctionName → catalog="es0"       │
│    resolveQueryTable → ElasticsearchMetadata.getTableFromQuery()  │
│    ┌─────────────────────────────────────────────────────────┐   │
│    │ 探断请求: POST /_sql                                     │   │
│    │   {"query": "SELECT id, name FROM logs WHERE ...",      │   │
│    │    "fetch_size": 1}                                      │   │
│    │ → 响应: {"columns": [{"name":"id","type":"long"},        │   │
│    │          {"name":"name","type":"text"}],                 │   │
│    │         "rows": [["1","test"]],                          │   │
│    │         "cursor": "..."}                                 │   │
│    │ → 关闭 cursor: POST /_sql/close                          │   │
│    │ → Column[id BIGINT, name VARCHAR]                        │   │
│    │ → EsTable("_query_42", queryTable=true)                  │   │
│    │   passThroughQuery = "SELECT id, name FROM logs WHERE..."│   │
│    └─────────────────────────────────────────────────────────┘   │
│    TableFunctionRelation.queryTable = EsTable                     │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 4. QueryAnalyzer.Visitor（持锁完整分析）                           │
│    tryResolveQueryTableFunction → 复用预解析结果                   │
│    buildQueryTableScope → 构建 Field/Scope                        │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 5. AuthorizerStmtVisitor（权限校验）                               │
│    NativeQueryCatalogCollector → catalogs = {"es0"}               │
│    Authorizer.checkCatalogAction(es0, USAGE)                      │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 6. RelationTransformer → LogicalEsScanOperator                    │
│    buildEsQueryTablePlan → 创建 ColumnRefOperator +               │
│    LogicalEsScanOperator(EsTable)                                 │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 7. EsScanNode（物理计划）                                          │
│    isQueryTable()=true → computeQueryTableScanRanges()            │
│    单一 TEsScanRange (es_hosts=集群节点, index="", shard_id=-1)    │
│    properties.put("native_query", "SELECT id, name FROM ...")     │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 8. BE ESDataSource → ESSqlReader                                  │
│    检测 properties["native_query"] 存在 → 创建 ESSqlReader        │
│    ┌─────────────────────────────────────────────────────────┐   │
│    │ 首次请求: POST /_sql                                     │   │
│    │   {"query": "SELECT id, name FROM logs WHERE ...",      │   │
│    │    "fetch_size": 4096}                                   │   │
│    │ → 解析 columns + rows → Chunk                            │   │
│    │ → 保存 cursor                                            │   │
│    ├─────────────────────────────────────────────────────────┤   │
│    │ 后续请求: POST /_sql                                     │   │
│    │   {"cursor": "..."}                                      │   │
│    │ → 解析 rows → Chunk                                      │   │
│    ├─────────────────────────────────────────────────────────┤   │
│    │ 无 cursor → EOS                                          │   │
│    │ 关闭: POST /_sql/close (如有 cursor)                     │   │
│    └─────────────────────────────────────────────────────────┘   │
│    ES 集群执行 SQL，返回结果集                                     │
└──────────────────────────────────────────────────────────────────┘
```

---

## 8. 涉及文件清单

### 8.1 通用化改造文件（JDBC + ES 共享）

| 文件 | 改动类型 | 职责 |
|------|---------|------|
| `fe/.../catalog/PassThroughQueryTable.java` | **新增** | 标记接口 |
| `fe/.../catalog/PassThroughQueryValidator.java` | **新增** | 查询规范化公共工具 |
| `fe/.../catalog/JDBCTable.java` | 修改 | 实现 PassThroughQueryTable 接口；normalizePassThroughQuery 委托 |
| `fe/.../catalog/EsTable.java` | 修改 | 实现 PassThroughQueryTable 接口；新增 queryTable 字段 |
| `fe/.../sql/ast/TableFunctionRelation.java` | 修改 | queryTable 类型 JDBCTable → Table |
| `fe/.../sql/analyzer/QueryAnalyzer.java` | 修改 | resolveJdbcQueryTable → resolveQueryTable（通用化 instanceof 检查） |
| `fe/.../sql/optimizer/transformer/RelationTransformer.java` | 修改 | buildJdbcQueryTablePlan → buildQueryTablePlan（按类型分派） |

### 8.2 ES 专属文件

| 文件 | 改动类型 | 职责 |
|------|---------|------|
| `fe/.../connector/elasticsearch/ElasticsearchMetadata.java` | 修改 | 新增 getTableFromQuery() |
| `fe/.../connector/elasticsearch/EsRestClient.java` | 修改 | 新增 probeEsSqlSchema()、executePost()、closeEsSqlCursor() |
| `fe/.../connector/elasticsearch/EsUtil.java` | 修改 | 新增 convertEsSqlType() |
| `fe/.../catalog/EsTable.java` | 修改 | 新增 queryTable 标志、passThroughQuery 字段、KEY_NATIVE_QUERY 常量 |
| `fe/.../planner/EsScanNode.java` | 修改 | 新增 computeQueryTableScanRanges()；toThrift() 传递 native_query |
| `fe/.../sql/optimizer/operator/logical/LogicalEsScanOperator.java` | 无改动 | 复用现有算子（已支持 EsTable） |
| `be/src/connector/elasticsearch/es_connector.cpp` | 修改 | _create_scanner() 分支：native_query → ESSqlReader |
| `be/src/connector/elasticsearch/es_connector.h` | 修改 | 新增 _native_query 成员 |
| `be/src/connector/elasticsearch/es_sql_reader.h` | **新增** | ESSqlReader 声明 |
| `be/src/connector/elasticsearch/es_sql_reader.cpp` | **新增** | ESSqlReader 实现（POST /_sql + cursor 分页） |
| `be/src/connector/elasticsearch/es_sql_parser.h` | **新增** | ES SQL 响应解析器声明 |
| `be/src/connector/elasticsearch/es_sql_parser.cpp` | **新增** | ES SQL JSON 响应 → Chunk 转换 |

### 8.3 权限校验文件

| 文件 | 改动类型 | 职责 |
|------|---------|------|
| `fe/.../sql/analyzer/AuthorizerStmtVisitor.java` | **无改动** | NativeQueryCatalogCollector 已是通用逻辑 |

### 8.4 测试文件

| 文件 | 说明 |
|------|------|
| `test/sql/test_es_catalog/T/test_native_query` | SQL 回归测试（正向用例） |
| `test/sql/test_es_catalog/R/test_native_query` | SQL 回归测试（期望结果 + 错误用例） |
| `fe/.../catalog/EsTableTest.java` | EsTable queryTable 单元测试 |
| `fe/.../connector/elasticsearch/ElasticsearchMetadataTest.java` | getTableFromQuery 单元测试 |
| `fe/.../connector/elasticsearch/MockedElasticsearchMetadata.java` | ES 元数据 Mock |
| `fe/.../sql/plan/StatementPlannerExternalTablesLockTest.java` | 扩展无锁预解析测试覆盖 ES |
| `be/test/connector/elasticsearch/es_sql_reader_test.cpp` | ESSqlReader 单元测试 |

---

## 9. 扩展性

### 9.1 通用化改造的收益

本次通用化改造（`PassThroughQueryTable` 接口 + `TableFunctionRelation.queryTable` 放宽为 `Table`）使得未来支持其他 Catalog 类型的 native_query 变得简单：

1. 新 Catalog 实现 `ConnectorMetadata.getTableFromQuery()`
2. 新 Table 类实现 `PassThroughQueryTable` 接口
3. 在 `RelationTransformer.buildQueryTablePlan()` 中添加新类型分派
4. 权限校验、SQL 解析、锁优化——**零改动**

### 9.2 未来支持 ES DSL 模式

如果未来需要支持 ES Query DSL pass-through（`TABLE(es0.native_query('{"query":{"match":{...}}}'))`），扩展方式：

1. **自动检测**：在 `PassThroughQueryValidator.normalize()` 中，如果查询以 `{` 开头则跳过 SELECT 校验，标记为 DSL 模式
2. **Schema 推断**：DSL 模式下，从 ES index mapping 推断 Schema（或执行 `POST /index/_search?size=0` 探测）
3. **BE 执行**：DSL 模式复用现有 `ESScanReader`（scroll 管道），将用户 DSL 直接作为 `KEY_QUERY` 注入，跳过 `ESScrollQueryBuilder`
4. **扫描范围**：DSL 模式仍需分片扫描范围（因为 `_search` 是索引级 API）

### 9.3 ES SQL filter 参数混合 DSL

ES SQL 支持 `filter` 参数在 SQL 之上叠加 DSL 过滤。未来可扩展 native_query 语法支持此特性：

```sql
-- 未来扩展（非本期）
SELECT * FROM TABLE(es0.native_query(
    'SELECT * FROM logs WHERE level = ''ERROR''',
    '{"range":{"timestamp":{"gte":"2024-01-01"}}}'
))
```

---

## 10. 风险与注意事项

### 10.1 ES SQL 版本兼容性

- ES SQL 从 ES 6.3 开始 GA（2018 年）
- 不同版本的 ES SQL 功能有差异（如 `PIVOT` 从 7.x 开始支持）
- 实现应检测 ES 版本（通过 `EsMajorVersion`），在不支持 SQL 的版本上给出明确错误

### 10.2 ES SQL 许可证

- ES SQL 在开源版（Basic）中可用（从 7.11+ 开始随 Open Search 发布）
- 但部分高级 SQL 功能可能需要 Platinum/Enterprise 许可证
- 实现不应对许可证做假设，让 ES 自行处理许可证校验

### 10.3 AST 不可变性

`TableFunctionRelation.queryTable` 已有 setter（JDBC native_query 引入），违反了 FE AGENTS.md 中的 AST 不可变原则。本次改动仅放宽类型，不引入新的可变性问题。

### 10.4 Thrift 兼容性

- `TEsScanNode.properties` 是 `map<string, string>`，新增 key 不影响现有序列化
- `TEsScanRange` 使用哨兵值（`index=""`, `shard_id=-1`），不修改 Thrift 定义
- 符合 `gensrc/AGENTS.md`：不新增 required 字段，不复用 ordinal

### 10.5 BE 模块边界

- `ESSqlReader` 和 `EsSqlResponseParser` 放在 `be/src/connector/elasticsearch/`
- 属于 `ConnectorElasticsearch` 模块目标
- 依赖关系与现有 `ESScanReader` 一致
- 需要更新 `be/src/connector/elasticsearch/CMakeLists.txt` 添加新源文件

### 10.6 性能考量

- ES SQL 的 `fetch_size` 控制 BE 请求的批大小，建议默认与 `chunk_size` 一致
- ES SQL cursor 在 ES 集群上维护状态，需要确保在查询结束/取消时调用 `/_sql/close` 释放资源
- 单扫描范围意味着只有一个 BE 实例执行查询（无并行扫描），对于大结果集可能有性能瓶颈。未来可考虑通过 `slice` 参数实现并行

---

## 11. 实现优先级

### Phase 1：通用化改造（前置条件）
1. 新增 `PassThroughQueryTable` 接口和 `PassThroughQueryValidator` 工具类
2. `JDBCTable` 实现接口
3. `TableFunctionRelation.queryTable` 放宽为 `Table`
4. `QueryAnalyzer` 通用化（resolveQueryTable、buildQueryTableScope）
5. `RelationTransformer` 通用化（buildQueryTablePlan 分派）
6. 确保 JDBC native_query 回归测试全部通过

### Phase 2：ES Schema 推断（FE）
1. `EsTable` 实现 `PassThroughQueryTable` 接口
2. `EsRestClient` 新增 `executePost()`、`probeEsSqlSchema()`
3. `EsUtil` 新增 `convertEsSqlType()`
4. `ElasticsearchMetadata` 实现 `getTableFromQuery()`

### Phase 3：ES 物理执行（FE + BE）
1. `EsScanNode` 新增 `computeQueryTableScanRanges()` 和 `toThrift()` 改造
2. 物理计划构建器集成 query table 分支
3. BE `ESDataSource` 新增 native_query 分支
4. BE 新增 `ESSqlReader` 和 `EsSqlResponseParser`

### Phase 4：测试与文档
1. FE 单元测试
2. BE 单元测试
3. SQL 集成测试
4. 文档更新（`docs/en/sql-reference/sql-functions/table-functions/native_query.md`）
