# Execution Contract: ES Catalog native_query

**Change**: `es-native-query`
**Created**: 2026-07-03
**DP-2**: Approved

---

## Intent Lock

将 `native_query` 表函数从 JDBC Catalog 扩展到 ES Catalog，通过 ES SQL `_sql` 端点实现 pass-through 查询，并将框架代码通用化以支持多 Catalog。

## Scope Fence

### In Scope

1. 通用化 `native_query` 框架代码（PassThroughQueryTable 接口、TableFunctionRelation 类型放宽、resolveJdbcQueryTable 通用化）
2. ES Catalog 的 `getTableFromQuery()` 实现（FE 侧 ES SQL 探测）
3. ES SQL 类型映射（`EsUtil.convertEsSqlType`）
4. BE 侧 `ESSqlReader` + `EsSqlResponseParser` 实现
5. BE 侧 `_create_scanner()` native_query 分支
6. 无锁预解析异常处理修复
7. fetch_size 上限保护
8. cursor 错误处理（防止静默数据截断）
9. SSL/认证/超时/故障转移处理
10. datetime ISO8601 解析 + time_zone 注入

### Out of Scope

- ES Query DSL pass-through 模式（未来扩展）
- ES SQL `filter` 参数混合 DSL（未来扩展）
- LIMIT 下推到 ES SQL（文档化为已知限制）
- 多 BE 并行扫描（v1 单一扫描范围，文档化为已知限制）
- 列裁剪（pass-through 固有特性）

### Non-Goals

- 不修改 Thrift 定义（通过 properties map 传递 SQL）
- 不改变 JDBC native_query 的任何行为
- 不引入新外部依赖

---

## Approved Behavior Summary

### ADDED Requirements (10)

| ID | Summary |
|----|---------|
| REQ-ES-NQ-001 | `native_query` 接受 ES Catalog 前缀，pass-through ES SQL 到 ES 集群执行 |
| REQ-ES-NQ-002 | `getTableFromQuery()` 通过 `POST /_sql` with `fetch_size:1` 探测列元数据 |
| REQ-ES-NQ-003 | `convertEsSqlType()` 将 ES SQL 类型映射为 StarRocks 类型，未知类型回退 VARCHAR |
| REQ-ES-NQ-004 | `EsTable` 实现 `PassThroughQueryTable` 接口，`setPassThroughQuery` 设置标记 |
| REQ-ES-NQ-005 | `EsScanNode` 为 query table 创建单一扫描范围（哨兵值 index=""/shard_id=-1） |
| REQ-ES-NQ-006 | `ESSqlReader` cursor 分页：首次 query+fetch_size，后续 cursor；cursor 失效时报错 |
| REQ-ES-NQ-007 | `_http_post()` 设置 SSL/Basic Auth/超时/节点故障转移 |
| REQ-ES-NQ-008 | 析构和 close 时 best-effort 关闭 cursor |
| REQ-ES-NQ-009 | `fetch_size` 不超过 `config::es_index_max_result_window`（默认 10000） |
| REQ-ES-NQ-010 | BE 正确解析 ISO8601 datetime（T 分隔符、毫秒、时区后缀）+ time_zone 注入请求体 |

### MODIFIED Requirements (3)

| ID | Summary |
|----|---------|
| REQ-GEN-001 | `native_query` 框架通过 `PassThroughQueryTable` 接口支持多 Catalog，JDBC 行为不变 |
| REQ-GEN-002 | `ExternalTablesOnlyVisitor` 在 `resolveQueryTable` 抛 RuntimeException 时静默返回 null |
| REQ-GEN-003 | `PassThroughQueryValidator.normalize()` 提供 JDBC/ES 共用的查询校验 |

---

## Build Rules

### Hard Constraints (不可漂移)

1. **零 Thrift 变更**：不修改 `TEsScanNode` / `TEsScanRange` / `TEsTable` 结构体。SQL 通过 `TEsScanNode.properties["native_query"]` 传递。
2. **JDBC 向后兼容**：通用化改造后 JDBC native_query 的所有现有测试必须通过。`JDBCTable` 只加 `implements PassThroughQueryTable`，不改行为。
3. **BE 分支位置**：`_create_scanner()` 中 native_query 分支必须在 `ESScrollQueryBuilder::build()` 之前返回，否则空 index 生成畸形 URL。
4. **cursor 错误检查**：`_execute_cursor_fetch()` 必须检查 `resp.HasMember("error")`。不检查会导致静默数据截断。
5. **FE SSL 处理**：`EsRestClient.executePost()` 必须复用 `sslEnabled ? getOrCreateSSLClient() : NETWORK_CLIENT`。不复用会导致 HTTPS 集群失败。
6. **EsTable 构造函数**：`getTableFromQuery()` 构造 EsTable 时 properties 必须包含非空 `hosts` 和 `index`（占位值），否则 `validate()` 抛 DdlException。
7. **无锁预解析异常吞没**：`ExternalTablesOnlyVisitor` 中 `resolveQueryTable()` 调用必须 catch `RuntimeException` 返回 null。

### Test-First Obligations

每个 ADDED/MODIFIED 要求必须先有失败的测试，再写实现：

| Requirement | Test | Batch |
|-------------|------|-------|
| REQ-GEN-003 | PassThroughQueryValidatorTest: SELECT-only 校验、去分号、空查询拒绝 | Batch 6 (1.1) |
| REQ-ES-NQ-004 | EsTableTest: setPassThroughQuery → isQueryTable/getPassThroughQuery | Batch 6 (1.2) |
| REQ-ES-NQ-002/003 | ElasticsearchMetadataTest: mock probeEsSqlSchema → EsTable schema 验证 | Batch 6 (1.3) |
| REQ-GEN-001/002 | StatementPlannerExternalTablesLockTest: JDBC 回归 + ES 预解析异常 | Batch 6 (1.4) |
| REQ-ES-NQ-006/008 | es_sql_reader_test: cursor 分页、错误处理、EOS、清理 | Batch 6 (1.5) |
| REQ-ES-NQ-001 | SQL 集成测试: pass-through、聚合、INSERT、错误用例 | Batch 6 (1.6) |

### Implementation Constraints

- **AST 不可变性**：`TableFunctionRelation.queryTable` 已有 setter（JDBC native_query 引入），本次仅放宽类型，不新增 setter。
- **BE 模块边界**：`ESSqlReader` 和 `EsSqlResponseParser` 放在 `be/src/connector/elasticsearch/`，属于 `ConnectorElasticsearch` 模块。
- **Thrift required 字段**：`TEsScanRange.index` 和 `shard_id` 是 `required`（遗留代码），使用哨兵值 `""` 和 `-1`。
- **FE Checkstyle**：4 空格缩进，130 字符行限，import 顺序：第三方 → Java 标准 → 静态导入。
- **BE clang-format**：遵循 `.clang-format`，`#pragma once`，`Status`/`StatusOr` 错误处理。

---

## Execution Batches

### Batch 1: 通用化基础设施

**Depends on**: 无
**Done when**:
- `PassThroughQueryTable` 接口和 `PassThroughQueryValidator` 工具类已创建
- `JDBCTable` 实现 `PassThroughQueryTable` 接口，`normalizePassThroughQuery` 委托
- JDBC native_query 单元测试通过（`JDBCTableTest`）
**Commit**: `[Refactor] Extract PassThroughQueryTable interface and PassThroughQueryValidator`

### Batch 2: FE 框架通用化

**Depends on**: Batch 1
**Done when**:
- `TableFunctionRelation.queryTable` 类型为 `Table`（非 `JDBCTable`）
- `resolveJdbcQueryTable` → `resolveQueryTable`，L247 检查改为 `instanceof PassThroughQueryTable`
- `RelationTransformer` 按 Table 类型分派（JDBC/ES）
- `ExternalTablesOnlyVisitor` 异常处理修复（catch RuntimeException → return null）
- JDBC 回归测试通过（`StatementPlannerExternalTablesLockTest`）
**Commit**: `[Refactor] Generalize native_query framework from JDBC-specific to multi-catalog`

### Batch 3: ES Schema 推断（FE）

**Depends on**: Batch 2
**Done when**:
- `EsTable` 实现 `PassThroughQueryTable` 接口（queryTable 字段 + setPassThroughQuery）
- `EsRestClient.executePost()` 复用 SSL 客户端选择
- `EsRestClient.probeEsSqlSchema()` 执行 `POST /_sql` 探测并关闭 cursor
- `EsUtil.convertEsSqlType()` 类型映射，default → VARCHAR
- `ElasticsearchMetadata.getTableFromQuery()` 返回合成 EsTable
**Commit**: `[Feature] Add ES SQL schema inference for native_query table function`

### Batch 4: ES 物理计划（FE）

**Depends on**: Batch 3
**Done when**:
- `EsScanNode.computeQueryTableScanRanges()` 创建单一扫描范围
- `EsScanNode.toThrift()` 传递 `native_query` property，跳过 docvalue/fields context
- 物理计划构建器在 `isQueryTable()` 时走 native_query 路径
**Commit**: `[Feature] Add EsScanNode native_query scan range and thrift serialization`

### Batch 5: BE ESSqlReader 实现

**Depends on**: Batch 4
**Done when**:
- `ESSqlReader` 实现初始查询 + cursor 分页 + 错误检查
- `_http_post()` 实现 SSL/认证/超时/故障转移
- `EsSqlResponseParser` 实现列式 rows → Chunk + ISO8601 datetime 解析
- `es_connector.cpp` `_create_scanner()` 在 `ESScrollQueryBuilder::build()` 前插入 native_query 分支
- fetch_size 上限保护生效
- CMakeLists.txt 包含新源文件
**Commit**: `[Feature] Add BE ESSqlReader for ES SQL _sql endpoint execution`

### Batch 6: 测试

**Depends on**: Batch 5
**Done when**:
- `PassThroughQueryValidatorTest`：5 个用例（正常/分号/非SELECT/空/注释）
- `EsTableTest`：setPassThroughQuery + isQueryTable/getPassThroughQuery
- `ElasticsearchMetadataTest`：mock probeEsSqlSchema → schema 验证
- `StatementPlannerExternalTablesLockTest`：JDBC 回归 + ES 预解析异常
- `es_sql_reader_test`：cursor 分页、错误处理、EOS、清理
- SQL 集成测试：pass-through、聚合、INSERT、错误用例
**Commit**: `[Test] Add unit and integration tests for ES native_query`

---

## Review Gates

| Gate | After Batch | Review Focus |
|------|------------|--------------|
| Gate 1 | Batch 1 | JDBC native_query 回归测试通过 |
| Gate 2 | Batch 2 | 通用化不破坏 JDBC；无锁预解析异常修复 |
| Gate 3 | Batch 3 | SSL 复用正确；cursor 探测后关闭；类型映射完整 |
| Gate 4 | Batch 4 | 哨兵值安全；properties 传递正确 |
| Gate 5 | Batch 5 | BE 分支位置正确；cursor 错误检查；SSL/认证/故障转移；fetch_size 上限 |
| Gate 6 | Batch 6 | 所有 spec 场景可测试；覆盖所有 13 个 SHALL 要求 |

---

## Rewind Triggers

以下情况必须停止实现，返回规划：

1. **JDBC 回归失败**：Batch 1 或 2 后 JDBC native_query 测试失败 → 通用化改造有误，返回 design.md 审查
2. **Thrift 变更需求**：实现中发现必须修改 Thrift 定义 → 返回 proposal.md 调整 scope
3. **ES SQL 兼容性问题**：ES SQL 行为与 spec 假设不符（如 cursor 语义差异）→ 返回 specs/ 修正
4. **BE 模块边界违规**：`ESSqlReader` 依赖了不允许的模块 → 返回 design.md 调整架构
5. **类型映射不完整**：发现 ES SQL 类型无法映射到 StarRocks 类型且 default VARCHAR 不可接受 → 返回 specs/ 补充

---

## Coverage Cross-Check

| Requirement | Batch | Test | Status |
|-------------|-------|------|--------|
| REQ-ES-NQ-001 | 3,4,5 | 6.6 SQL 集成测试 | ✅ |
| REQ-ES-NQ-002 | 3 | 6.3 ElasticsearchMetadataTest | ✅ |
| REQ-ES-NQ-003 | 3 | 6.3 ElasticsearchMetadataTest | ✅ |
| REQ-ES-NQ-004 | 3 | 6.2 EsTableTest | ✅ |
| REQ-ES-NQ-005 | 4 | 6.6 SQL 集成测试 | ✅ |
| REQ-ES-NQ-006 | 5 | 6.5 es_sql_reader_test | ✅ |
| REQ-ES-NQ-007 | 5 | 6.5 es_sql_reader_test | ✅ |
| REQ-ES-NQ-008 | 5 | 6.5 es_sql_reader_test | ✅ |
| REQ-ES-NQ-009 | 5 | 6.5 es_sql_reader_test | ✅ |
| REQ-ES-NQ-010 | 5 | 6.5 es_sql_reader_test | ✅ |
| REQ-GEN-001 | 2 | 6.4 StatementPlannerExternalTablesLockTest | ✅ |
| REQ-GEN-002 | 2 | 6.4 StatementPlannerExternalTablesLockTest | ✅ |
| REQ-GEN-003 | 1 | 6.1 PassThroughQueryValidatorTest | ✅ |

**覆盖率**: 13/13 要求全部映射到 Batch 和测试。**无遗漏**。

---

## Escalation Rules

- **未映射要求**：无。所有 13 个 SHALL 要求均映射到 Batch 和测试。
- **歧义决策**：如果实现中遇到 `EsTable` 构造函数 properties 的占位 index 值选择问题，默认使用 `"_native_query_placeholder"`（BE 在 native_query 模式下忽略 index）。
- **跨 Batch 依赖**：严格按 Batch 1→2→3→4→5→6 顺序执行。Batch 2 依赖 Batch 1 的接口；Batch 3 依赖 Batch 2 的类型放宽；Batch 4 依赖 Batch 3 的 EsTable 扩展；Batch 5 依赖 Batch 4 的 properties 传递；Batch 6 依赖全部实现完成。
