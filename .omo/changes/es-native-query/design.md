# Design: ES Catalog native_query

## Context

StarRocks 的 `native_query` 表函数（v4.1 起）目前仅支持 JDBC Catalog。其核心设计是一个 **JDBC 扫描链路的适配器**——将 pass-through SQL 包装为子查询 `(query) starrocks_query`，通过 JDBC 驱动执行。

ES Catalog 使用 HTTP REST API（非 JDBC 驱动）与 ES 集群通信。ES SQL（`_sql` 端点）自 ES 6.3 起 GA，返回结构化列元数据，适合 Schema 推断。本变更将 `native_query` 扩展到 ES Catalog，并在此过程中将框架代码通用化，使未来支持其他 Catalog 类型零改动。

**现有约束**：
- `TableFunctionRelation.queryTable` 硬编码为 `JDBCTable` 类型
- `QueryAnalyzer.resolveJdbcQueryTable` 硬编码 `instanceof JDBCTable` 检查
- `RelationTransformer.buildJdbcQueryTablePlan` 硬编码 `LogicalJDBCScanOperator`
- `EsRestClient` 仅有 GET 方法
- `TEsScanRange` 的 `index` 和 `shard_id` 是 `required` 字段
- `TEsTable` 是空 struct（所有信息通过 `TEsScanNode` 传递）

**详细设计文档**：`es_native_query_design.md`（1784 行，包含完整的代码级设计）

## Goals

1. **通用化**：将 JDBC 专属的 native_query 框架泛化为多 Catalog 通用，未来支持其他 Catalog 零改动
2. **ES SQL pass-through**：通过 `POST /_sql` 端点实现 ES SQL 下推执行
3. **Schema 推断**：从 ES SQL `columns` 响应元数据推断结果集 Schema
4. **零 Thrift 变更**：通过 `TEsScanNode.properties` map 传递 SQL，不修改 Thrift 定义
5. **JDBC 向后兼容**：通用化改造不改变 JDBC native_query 的任何行为
6. **安全集群支持**：正确处理 SSL、认证、超时、故障转移

## Decisions

### Decision 1: 选择 ES SQL 而非 Query DSL

**Choice**: 采用 ES SQL（`_sql` 端点）作为 native_query 的查询语言。

**Rationale**:
- ES SQL 响应直接包含 `columns: [{name, type}]` 元数据，Schema 推断简洁可靠
- 与 JDBC native_query 的 SQL pass-through UX 一致
- `normalizePassThroughQuery` 校验（SELECT-only）可直接复用
- cursor 分页自动清理，比 scroll/PIT 管理简单
- GA since ES 6.3 (2018)，API 稳定

**Alternatives**:
- Query DSL（`_search` 端点）：功能更完整，但 Schema 推断困难（需 probe + mapping），与 JDBC UX 不一致
- 混合模式：实现复杂度高，参数校验逻辑分叉

### Decision 2: 通用化改造（PassThroughQueryTable 接口）

**Choice**: 引入 `PassThroughQueryTable` 接口，将 `TableFunctionRelation.queryTable` 从 `JDBCTable` 放宽为 `Table`。

**Rationale**:
- 避免 `instanceof` 多重判断，通过接口实现多态
- `JDBCTable` 和 `EsTable` 各自实现接口，保持类型安全
- 未来支持其他 Catalog（如 Hive SQL pass-through）零改动
- 改动是类型安全的，不改变 JDBC 行为

**Alternatives**:
- ES 专属分支：在 QueryAnalyzer 中新增 `tryResolveEsQueryTableFunction()`，代码重复，每增加 Catalog 类型需重复

### Decision 3: 通过 properties map 传递 SQL（零 Thrift 变更）

**Choice**: 通过 `TEsScanNode.properties` map 新增 `"native_query"` key 传递 SQL 字符串。

**Rationale**:
- 零 Thrift 变更，不修改 `TEsScanNode` / `TEsScanRange` 结构体
- 向后兼容：不传递该 key 时行为完全不变
- 符合现有模式：user/password/ssl/timezone 已通过 properties 传递

**Alternatives**:
- 新增 Thrift 字段 `optional string native_query`：更类型安全，但需修改 `gensrc/thrift/PlanNodes.thrift` 并重新生成代码，收益有限

### Decision 4: 单一扫描范围（非分片）

**Choice**: native query 创建单一 `TEsScanRange`，使用哨兵值 `index=""`、`shard_id=-1`。

**Rationale**:
- ES SQL 是集群级 API，不针对特定分片
- cursor 是集群级状态，不绑定分片
- 多分片并行扫描会导致重复结果

**Trade-off**: 单一扫描范围 = 单 BE 执行，无并行扫描。v1 接受此限制，文档化为已知限制。

### Decision 5: BE native_query 分支位置

**Choice**: native_query 分支必须插入在 `ESScrollQueryBuilder::build()` 调用之前。

**Rationale**: 现有 `_create_scanner()` 无条件设置 `KEY_INDEX`/`KEY_SHARD` 后调用 `ESScrollQueryBuilder::build()`（需要非空 index）。如果分支在 `build()` 之后，空 index 会生成畸形 URL。

### Decision 6: cursor 错误处理

**Choice**: `_execute_cursor_fetch()` 必须检查 `resp.HasMember("error")`。

**Rationale**: ES SQL cursor 可能被中途失效（集群重启、超时、资源压力）。不检查会导致无 `rows` 被当作空批次，无 `cursor` 被当作 EOS → 静默截断结果集。

## Risks And Trade-Offs

### 风险：通用化改造破坏 JDBC native_query

**缓解**：Phase 1 完成后运行 JDBC native_query 回归测试套件。所有改动是类型安全的——`PassThroughQueryTable` 接口的 `isQueryTable()` / `getPassThroughQuery()` / `setPassThroughQuery()` 方法在 JDBCTable 上已存在，只需加 `implements` 声明。

### 风险：单 BE 执行的扩展性瓶颈

**缓解**：v1 接受此限制。未来可通过 ES SQL `slice` 参数实现并行（但 `slice` 要求单一索引，与多索引 `FROM "log*"` 不兼容）。文档化为已知限制。

### 风险：宽行 + 大 fetch_size 导致 OOM

**缓解**：fetch_size 上限保护，复用 `config::es_index_max_result_window`（默认 10000）作为上限。

### 风险：ES SQL 版本差异

**缓解**：类型映射使用 `default → VARCHAR` 回退，对未知类型不报错。不按版本做条件逻辑——让 ES 返回类型，防御性映射。

### 风险：无锁预解析异常崩溃

**缓解**：在 `ExternalTablesOnlyVisitor` 中 catch `RuntimeException`，静默返回 null，让持锁阶段重新解析。此修复也惠及 JDBC native_query。
