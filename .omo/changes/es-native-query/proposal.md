# Proposal: ES Catalog native_query

## Why

StarRocks 的 `native_query` 表函数目前仅支持 JDBC Catalog，允许用户将源数据库原生 SQL 以 pass-through 方式下推执行。Elasticsearch Catalog 不支持此功能，用户无法在 SQL 中动态发起 ES 原生查询，必须预先创建 ES 外表。

ES SQL（`_sql` 端点）自 ES 6.3 起 GA，返回结构化列元数据（`columns: [{name, type}]`），适合 Schema 推断。将 `native_query` 扩展到 ES Catalog 可以为用户提供统一的 pass-through 联邦查询体验，无需为每个查询需求创建持久外表。

## What Changes

1. **通用化改造**：将 JDBC 专属的 `native_query` 框架代码泛化为多 Catalog 通用——引入 `PassThroughQueryTable` 接口，将 `TableFunctionRelation.queryTable` 从 `JDBCTable` 放宽为 `Table`，将 `resolveJdbcQueryTable` 中的 `instanceof JDBCTable` 检查改为接口检查
2. **ES Schema 推断**：在 `ElasticsearchMetadata` 中实现 `getTableFromQuery()`，通过 `POST /_sql` with `fetch_size:1` 探测列元数据
3. **EsTable 扩展**：新增 `queryTable` 标志和 `passThroughQuery` 字段，实现 `PassThroughQueryTable` 接口
4. **EsScanNode 扩展**：新增 `computeQueryTableScanRanges()` 创建单一扫描范围，`toThrift()` 通过 properties 传递 SQL
5. **BE ESSqlReader**：新增 `ESSqlReader` 和 `EsSqlResponseParser`，通过 `POST /_sql` + cursor 分页执行 ES SQL
6. **BE ESDataSource 分支**：在 `_create_scanner()` 中检测 `native_query` property，选择 `ESSqlReader` 路径

## Scope

### In Scope

- 通用化 `native_query` 框架代码（PassThroughQueryTable 接口、TableFunctionRelation 类型放宽）
- ES Catalog 的 `getTableFromQuery()` 实现
- ES SQL Schema 探测（FE 侧 `EsRestClient.executePost` + `probeEsSqlSchema`）
- ES SQL 类型映射（`EsUtil.convertEsSqlType`）
- BE 侧 `ESSqlReader` + `EsSqlResponseParser` 实现
- BE 侧 `_create_scanner()` native_query 分支
- 无锁预解析异常处理修复
- fetch_size 上限保护
- cursor 错误处理（防止静默数据截断）
- SSL/认证/超时/故障转移处理

### Out of Scope

- ES Query DSL pass-through 模式（未来扩展，见设计文档 §9.2）
- ES SQL `filter` 参数混合 DSL（未来扩展）
- LIMIT 下推到 ES SQL（文档化为已知限制）
- 多 BE 并行扫描（v1 单一扫描范围，文档化为已知限制）
- 列裁剪（pass-through 固有特性，文档化）

## Impact

### 代码影响区域

- **FE 分析层**：`QueryAnalyzer`（通用化 resolveJdbcQueryTable）、`RelationTransformer`（按类型分派计划构建）、`AuthorizerStmtVisitor`（零改动，已是通用逻辑）
- **FE AST**：`TableFunctionRelation`（queryTable 类型放宽）
- **FE Catalog**：`JDBCTable`（实现接口）、`EsTable`（新增 queryTable 字段）、新增 `PassThroughQueryTable` 接口、新增 `PassThroughQueryValidator` 工具类
- **FE ES Connector**：`ElasticsearchMetadata`（新增 getTableFromQuery）、`EsRestClient`（新增 executePost/probeEsSqlSchema）、`EsUtil`（新增 convertEsSqlType）
- **FE Planner**：`EsScanNode`（新增 computeQueryTableScanRanges、toThrift 扩展）
- **BE ES Connector**：`es_connector.cpp`（_create_scanner 分支）、新增 `es_sql_reader.h/cpp`、新增 `es_sql_parser.h/cpp`

### API 影响

- 新增 SQL 语法：`TABLE(<es_catalog>.native_query('<select_sql>'))`（与 JDBC native_query 语法一致，仅 Catalog 类型不同）

### 依赖关系

- 依赖现有 JDBC native_query 实现（提交 `16f5fb71870`）
- 依赖 ES SQL `_sql` 端点（ES 6.3+）
- 不引入新外部依赖

## Capabilities

### New Capabilities

- **ES native_query**：用户可通过 `TABLE(es_catalog.native_query('SELECT ...'))` 将 ES SQL 下推到 ES 集群执行，结果集作为 StarRocks 关系暴露

### Modified Capabilities

- **JDBC native_query 框架通用化**：`native_query` 的框架代码从 JDBC 专属改为多 Catalog 通用，JDBC 行为不变
