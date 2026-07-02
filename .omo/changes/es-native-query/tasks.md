# Tasks: ES Catalog native_query

## File Structure

### Create

- `fe/fe-core/src/main/java/com/starrocks/catalog/PassThroughQueryTable.java` — 标记接口，标识 native_query 合成表
- `fe/fe-core/src/main/java/com/starrocks/catalog/PassThroughQueryValidator.java` — pass-through 查询规范化公共工具（trim/去分号/去注释/SELECT 校验）
- `be/src/connector/elasticsearch/es_sql_reader.h` — ESSqlReader 声明，通过 _sql 端点 + cursor 分页执行 ES SQL
- `be/src/connector/elasticsearch/es_sql_reader.cpp` — ESSqlReader 实现（HTTP POST、cursor 分页、SSL/认证/超时/故障转移）
- `be/src/connector/elasticsearch/es_sql_parser.h` — EsSqlResponseParser 声明，解析 ES SQL JSON 响应为 Chunk
- `be/src/connector/elasticsearch/es_sql_parser.cpp` — EsSqlResponseParser 实现（列式 rows → Chunk，ISO8601 datetime 解析）

### Modify

- `fe/fe-core/src/main/java/com/starrocks/catalog/JDBCTable.java` — 添加 `implements PassThroughQueryTable`，normalizePassThroughQuery 委托给 PassThroughQueryValidator
- `fe/fe-core/src/main/java/com/starrocks/catalog/EsTable.java` — 添加 `implements PassThroughQueryTable`，新增 queryTable 字段、passThroughQuery 字段、KEY_NATIVE_QUERY 常量
- `fe/fe-core/src/main/java/com/starrocks/sql/ast/TableFunctionRelation.java:44,91-97` — queryTable 类型从 JDBCTable 放宽为 Table
- `fe/fe-core/src/main/java/com/starrocks/sql/analyzer/QueryAnalyzer.java:186-271,1956-1993,2172-2216` — 通用化 resolveJdbcQueryTable→resolveQueryTable、buildJdbcQueryTableScope→buildQueryTableScope、无锁预解析异常处理
- `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/transformer/RelationTransformer.java:1166-1260` — buildJdbcQueryTablePlan→buildQueryTablePlan 按 Table 类型分派
- `fe/fe-core/src/main/java/com/starrocks/connector/elasticsearch/ElasticsearchMetadata.java` — 新增 getTableFromQuery() 方法
- `fe/fe-core/src/main/java/com/starrocks/connector/elasticsearch/EsRestClient.java` — 新增 executePost()、probeEsSqlSchema()、closeEsSqlCursor() 方法
- `fe/fe-core/src/main/java/com/starrocks/connector/elasticsearch/EsUtil.java` — 新增 convertEsSqlType() 方法
- `fe/fe-core/src/main/java/com/starrocks/planner/EsScanNode.java:132-159` — toThrift() 传递 native_query property，新增 computeQueryTableScanRanges()
- `be/src/connector/elasticsearch/es_connector.cpp:196-223` — _create_scanner() 新增 native_query 分支（在 ESScrollQueryBuilder::build 之前）
- `be/src/connector/elasticsearch/es_connector.h` — 新增 _native_query 成员
- `be/src/connector/elasticsearch/CMakeLists.txt` — 添加 es_sql_reader.cpp 和 es_sql_parser.cpp 源文件

## Interfaces

### Batch 1 → Batch 2
- **Produces**: `PassThroughQueryTable` 接口 — Batch 2 的 JDBCTable 和 Batch 3 的 EsTable 实现此接口
- **Produces**: `PassThroughQueryValidator.normalize()` — Batch 2 的 JDBCTable.normalizePassThroughQuery 委托调用，Batch 3 的 ElasticsearchMetadata.getTableFromQuery 调用

### Batch 2 → Batch 3
- **Produces**: `TableFunctionRelation.queryTable` 类型放宽为 `Table` — Batch 3 的 resolveQueryTable 返回 `Table`（非 JDBCTable）
- **Produces**: `resolveQueryTable()` 通用化方法 — Batch 3 的 ElasticsearchMetadata.getTableFromQuery 被 resolveQueryTable 调用

### Batch 3 → Batch 4
- **Produces**: `EsTable.isQueryTable()` / `getPassThroughQuery()` — Batch 4 的 RelationTransformer 和 EsScanNode 通过此接口判断
- **Produces**: `ElasticsearchMetadata.getTableFromQuery()` — 被 resolveQueryTable 调用

### Batch 4 → Batch 5
- **Produces**: `EsScanNode.toThrift()` 传递 `native_query` property — Batch 5 的 BE ESDataSource 检测此 property
- **Produces**: `EsScanNode.computeQueryTableScanRanges()` 创建单一扫描范围 — Batch 5 的 ESSqlReader 从 TEsScanRange.es_hosts 获取 ES 节点

### Batch 5 → Batch 6
- **Produces**: `ESSqlReader` 类 — Batch 6 的 EsSqlResponseParser 被 ESSqlReader 调用解析响应

---

## Batch 1: 通用化基础设施

**Depends on**: 无

### 1.1 Create PassThroughQueryTable 接口

**Create**: `fe/fe-core/src/main/java/com/starrocks/catalog/PassThroughQueryTable.java`

```java
package com.starrocks.catalog;

public interface PassThroughQueryTable {
    boolean isQueryTable();
    String getPassThroughQuery();
    void setPassThroughQuery(String query);
}
```

### 1.2 Create PassThroughQueryValidator 工具类

**Create**: `fe/fe-core/src/main/java/com/starrocks/catalog/PassThroughQueryValidator.java`

将 `JDBCTable` 中的 `normalizePassThroughQuery`、`validatePassThroughQuery`、`stripLeadingComments`、`startsWithSqlKeyword` 逻辑迁移到此公共类。

```java
package com.starrocks.catalog;

import org.apache.commons.lang3.StringUtils;

public class PassThroughQueryValidator {
    public static String normalize(String query) {
        // trim + 去尾部分号 + validateSelectOnly（stripLeadingComments + startsWithSqlKeyword "select"）
    }
}
```

### 1.3 JDBCTable 实现接口 + 委托

**Modify**: `fe/fe-core/src/main/java/com/starrocks/catalog/JDBCTable.java`

- 类声明添加 `implements PassThroughQueryTable`
- `normalizePassThroughQuery` 改为委托 `PassThroughQueryValidator.normalize()`
- 新增 `getPassThroughQuery()` 方法返回 `queryTable ? jdbcTable : null`

### 1.4 验证 JDBC native_query 回归测试

运行现有 JDBC native_query 测试确认无回归：
```bash
cd fe && mvn test -Dtest=JDBCTableTest
```

### 1.5 Commit

```
[Refactor] Extract PassThroughQueryTable interface and PassThroughQueryValidator
```

---

## Batch 2: FE 框架通用化

**Depends on**: Batch 1

### 2.1 TableFunctionRelation 类型放宽

**Modify**: `fe/fe-core/src/main/java/com/starrocks/sql/ast/TableFunctionRelation.java:18,44,91-97`

- L18: `import com.starrocks.catalog.JDBCTable` → `import com.starrocks.catalog.Table`
- L44: `private JDBCTable queryTable` → `private Table queryTable`
- L91: `public JDBCTable getQueryTable()` → `public Table getQueryTable()`
- L95: `public void setQueryTable(JDBCTable queryTable)` → `public void setQueryTable(Table queryTable)`

### 2.2 QueryAnalyzer resolveJdbcQueryTable → resolveQueryTable

**Modify**: `fe/fe-core/src/main/java/com/starrocks/sql/analyzer/QueryAnalyzer.java:186-271`

- `JdbcQueryTableFunctionName` 重命名为 `QueryTableFunctionName`（或保留原名，仅改方法）
- `resolveJdbcQueryTable` → `resolveQueryTable`：返回 `Table`，L247 检查改为 `instanceof PassThroughQueryTable`
- `buildJdbcQueryTableScope` → `buildQueryTableScope`：参数类型 `JDBCTable` → `Table`
- 错误消息从 "JDBC query table function" 改为 "native query table function"

### 2.3 QueryAnalyzer tryResolveJdbcQueryTableFunction 通用化

**Modify**: `fe/fe-core/src/main/java/com/starrocks/sql/analyzer/QueryAnalyzer.java:1956-1993`

- L1988: `JDBCTable jdbcTable = node.getQueryTable()` → `Table queryTable = node.getQueryTable()`
- L1990: `resolveJdbcQueryTable` → `resolveQueryTable`
- L1992: `buildJdbcQueryTableScope` → `buildQueryTableScope`

### 2.4 QueryAnalyzer ExternalTablesOnlyVisitor 通用化 + 异常修复

**Modify**: `fe/fe-core/src/main/java/com/starrocks/sql/analyzer/QueryAnalyzer.java:2172-2216`

- L2210: `JDBCTable jdbcTable` → `Table queryTable`
- L2212: `resolveJdbcQueryTable` → `resolveQueryTable`
- L2214: `node.setQueryTable(jdbcTable)` → `node.setQueryTable(queryTable)`
- **异常修复**：在 resolveQueryTable 调用处添加 `catch (RuntimeException e) { return null; }`

### 2.5 RelationTransformer 通用化

**Modify**: `fe/fe-core/src/main/java/com/starrocks/sql/optimizer/transformer/RelationTransformer.java:1166-1260`

- `buildJdbcQueryTablePlan` → `buildQueryTablePlan`：按 `instanceof JDBCTable` / `instanceof EsTable` 分派
- 原有 JDBC 逻辑提取为 `buildJdbcQueryTablePlan(node, JDBCTable table)`
- 新增 `buildEsQueryTablePlan(node, EsTable table)`：创建 `LogicalEsScanOperator`

### 2.6 验证 JDBC native_query 回归测试

```bash
cd fe && mvn test -Dtest=StatementPlannerExternalTablesLockTest
```

### 2.7 Commit

```
[Refactor] Generalize native_query framework from JDBC-specific to multi-catalog
```

---

## Batch 3: ES Schema 推断（FE）

**Depends on**: Batch 2

### 3.1 EsTable 实现 PassThroughQueryTable 接口

**Modify**: `fe/fe-core/src/main/java/com/starrocks/catalog/EsTable.java`

- 类声明添加 `implements PassThroughQueryTable`
- 新增字段：`@SerializedName("qt") private boolean queryTable`、`private String passThroughQuery`
- 新增常量：`public static final String KEY_NATIVE_QUERY = "native_query"`
- 实现 `isQueryTable()`、`getPassThroughQuery()`、`setPassThroughQuery(String)`

### 3.2 EsRestClient 新增 executePost 方法

**Modify**: `fe/fe-core/src/main/java/com/starrocks/connector/elasticsearch/EsRestClient.java`

新增 `executePost(String path, String jsonBody)` 方法：
- 复用 `sslEnabled ? getOrCreateSSLClient() : NETWORK_CLIENT` SSL 客户端选择
- 遍历所有节点带故障转移
- 设置 Basic Auth header
- 检查 HTTP 状态码

### 3.3 EsRestClient 新增 probeEsSqlSchema 方法

**Modify**: `fe/fe-core/src/main/java/com/starrocks/connector/elasticsearch/EsRestClient.java`

新增 `probeEsSqlSchema(String sqlQuery)` 方法：
- 构造 `{"query": sql, "fetch_size": 1, "time_zone": tz}` 请求体
- 调用 `executePost("/_sql", body)`
- 解析 `columns` 数组为 `List<EsSqlColumn>`
- best-effort 关闭 cursor

### 3.4 EsRestClient 新增 closeEsSqlCursor 方法

**Modify**: `fe/fe-core/src/main/java/com/starrocks/connector/elasticsearch/EsRestClient.java`

新增 `closeEsSqlCursor(String cursor)` 方法：调用 `executePost("/_sql/close", {"cursor": cursor})`

### 3.5 EsUtil 新增 convertEsSqlType 方法

**Modify**: `fe/fe-core/src/main/java/com/starrocks/connector/elasticsearch/EsUtil.java`

新增 `convertEsSqlType(String esSqlType)` 方法：switch-case 映射 ES SQL 类型到 StarRocks Type，default → VARCHAR

### 3.6 ElasticsearchMetadata 新增 getTableFromQuery 方法

**Modify**: `fe/fe-core/src/main/java/com/starrocks/connector/elasticsearch/ElasticsearchMetadata.java`

新增 `getTableFromQuery(ConnectContext, String dbName, String query)` 方法：
- 调用 `PassThroughQueryValidator.normalize(query)`
- 调用 `esRestClient.probeEsSqlSchema(normalizedQuery)` 获取列元数据
- 调用 `EsUtil.convertEsSqlType()` 映射每列类型
- 构造 properties map（hosts/user/password/ssl/transport/time_zone + 占位 index）
- 构造 EsTable 并调用 `setPassThroughQuery(normalizedQuery)`
- 返回 EsTable

### 3.7 Commit

```
[Feature] Add ES SQL schema inference for native_query table function
```

---

## Batch 4: ES 物理计划（FE）

**Depends on**: Batch 3

### 4.1 EsScanNode 新增 computeQueryTableScanRanges

**Modify**: `fe/fe-core/src/main/java/com/starrocks/planner/EsScanNode.java`

新增 `computeQueryTableScanRanges()` 方法：
- 从 `table.getSeeds()` 获取 ES 集群节点地址
- 调用 `assignNodes()` 获取 compute nodes
- 创建单一 `TScanRangeLocations`，分配 1-3 个 compute node
- 创建 `TEsScanRange`：es_hosts=集群节点，index=""，shard_id=-1

### 4.2 EsScanNode toThrift 传递 native_query

**Modify**: `fe/fe-core/src/main/java/com/starrocks/planner/EsScanNode.java:132-159`

在 `toThrift()` 中：
- 当 `table.isQueryTable()` 时，`properties.put(EsTable.KEY_NATIVE_QUERY, table.getPassThroughQuery())`
- 当 `table.isQueryTable()` 时，跳过 docvalue_context / fields_context 设置

### 4.3 物理计划构建器集成

**Modify**: ES scan 物理计划构建逻辑

当 `esTable.isQueryTable()` 时：
- 调用 `esScanNode.computeQueryTableScanRanges()` 而非 `computeShardLocations(selectedIndex)`
- 跳过 `EsTablePartitions` / `EsShardPartitions` 分片选择

### 4.4 Commit

```
[Feature] Add EsScanNode native_query scan range and thrift serialization
```

---

## Batch 5: BE ESSqlReader 实现

**Depends on**: Batch 4

### 5.1 Create es_sql_reader.h

**Create**: `be/src/connector/elasticsearch/es_sql_reader.h`

声明 `EsSqlColumn` 结构体和 `ESSqlReader` 类：
- `open()` / `get_next(Chunk*)` / `close()`
- `_execute_initial_query()` / `_execute_cursor_fetch()` / `_close_cursor()` / `_http_post()`
- 成员：`_es_hosts`、`_properties`、`_sql_query`、`_batch_size`、`_cursor`、`_columns`、`_eos`

### 5.2 Create es_sql_reader.cpp — 初始查询

**Create**: `be/src/connector/elasticsearch/es_sql_reader.cpp`

实现 `_execute_initial_query()`：
- 构造 `{"query": sql, "fetch_size": batch_size, "time_zone": tz}` 请求体
- 调用 `_http_post("/_sql", body, &response)`
- 解析响应：检查 error、解析 columns、解析 rows、保存 cursor

### 5.3 Create es_sql_reader.cpp — cursor 分页（含错误检查）

实现 `_execute_cursor_fetch()`：
- 构造 `{"cursor": cursor}` 请求体
- 调用 `_http_post`
- **必须检查** `resp.HasMember("error")` — cursor 失效时报错，不静默截断
- 解析 rows、更新 cursor

### 5.4 Create es_sql_reader.cpp — _http_post（SSL/认证/超时/故障转移）

实现 `_http_post()`：
- 遍历 `_es_hosts` 带故障转移
- 每个请求调用 `trust_all_ssl()`、`set_basic_auth()`、`set_content_type("application/json")`、`set_timeout_ms(config::es_http_timeout_ms)`
- 返回成功的响应

### 5.5 Create es_sql_reader.cpp — 析构函数 + close

实现 `~ESSqlReader()` 和 `close()`：best-effort 调用 `_close_cursor()`

### 5.6 Create es_sql_parser.h/cpp

**Create**: `be/src/connector/elasticsearch/es_sql_parser.h` 和 `es_sql_parser.cpp`

实现 `EsSqlResponseParser::parse()`：
- 遍历 rows 二维数组，按 columns 类型转换为 Chunk 列
- 实现 `append_datetime_value()`：ISO8601 归一化 + DateTimeValue::from_string
- 实现 `normalize_iso8601_datetime()`：T→空格、截断时区后缀

### 5.7 es_connector.cpp 新增 native_query 分支

**Modify**: `be/src/connector/elasticsearch/es_connector.cpp:196-223`

在 `_create_scanner()` 中，**在 `ESScrollQueryBuilder::build()` 之前**插入：
- 检查 `_properties.find("native_query")`
- 若存在：创建 `ESSqlReader`，fetch_size 上限保护（`min(chunk_size, config::es_index_max_result_window)`），return OK
- 若不存在：走原有 scroll 路径

### 5.8 es_connector.h 新增成员

**Modify**: `be/src/connector/elasticsearch/es_connector.h`

新增 `std::string _native_query` 成员变量

### 5.9 CMakeLists.txt 添加源文件

**Modify**: `be/src/connector/elasticsearch/CMakeLists.txt`

添加 `es_sql_reader.cpp` 和 `es_sql_parser.cpp`

### 5.10 Commit

```
[Feature] Add BE ESSqlReader for ES SQL _sql endpoint execution
```

---

## Batch 6: 测试

**Depends on**: Batch 5

### 6.1 FE 单元测试 — PassThroughQueryValidator

**Create**: `fe/fe-core/src/test/java/com/starrocks/catalog/PassThroughQueryValidatorTest.java`

测试用例：
- 正常 SELECT 查询通过校验
- 尾部分号被去除
- 非 SELECT 语句被拒绝
- 空查询被拒绝
- 前导注释被正确跳过

### 6.2 FE 单元测试 — EsTable queryTable

**Create/Modify**: `fe/fe-core/src/test/java/com/starrocks/catalog/EsTableTest.java`

测试用例：
- `setPassThroughQuery()` 后 `isQueryTable()` 返回 true
- `getPassThroughQuery()` 返回设置的 SQL

### 6.3 FE 单元测试 — ElasticsearchMetadata getTableFromQuery

**Create/Modify**: `fe/fe-core/src/test/java/com/starrocks/connector/elasticsearch/ElasticsearchMetadataTest.java`

Mock `EsRestClient.probeEsSqlSchema()` 返回列元数据，验证：
- 返回的 EsTable 的 fullSchema 列数和类型正确
- `isQueryTable()` 返回 true

### 6.4 FE 单元测试 — 通用化回归

**Modify**: `fe/fe-core/src/test/java/com/starrocks/sql/plan/StatementPlannerExternalTablesLockTest.java`

添加 ES native_query 无锁预解析测试用例

### 6.5 BE 单元测试 — ESSqlReader

**Create**: `be/test/connector/elasticsearch/es_sql_reader_test.cpp`

测试用例：
- 首次请求解析 columns 和 rows
- cursor 分页获取后续数据
- cursor 失效时报错（不静默截断）
- 无 cursor 时标记 EOS
- cursor 清理

### 6.6 SQL 集成测试

**Create**: `test/sql/test_es_catalog/T/test_native_query` 和 `test/sql/test_es_catalog/R/test_native_query`

测试用例：
- 基本 ES SQL pass-through 查询
- ES SQL 聚合查询 + StarRocks 二次聚合
- 多索引模式匹配
- INSERT INTO 加载
- 错误用例（非 SELECT、空查询、列别名）

### 6.7 Commit

```
[Test] Add unit and integration tests for ES native_query
```
