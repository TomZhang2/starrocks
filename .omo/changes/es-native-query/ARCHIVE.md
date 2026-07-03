# Archive: es-native-query

**Change**: es-native-query
**Archived**: 2026-07-03
**DP-7**: Confirmed

## Change Summary

将 `native_query` 表函数从 JDBC Catalog 扩展到 ES Catalog，通过 ES SQL `_sql` 端点实现 pass-through 查询。同时将框架代码通用化（PassThroughQueryTable 接口），使未来支持其他 Catalog 零改动。

## Commits (6 implementation + 4 doc/spec)

| Commit | Type | Description |
|--------|------|-------------|
| `04334a48a0f` | Doc | ES native_query 设计文档 |
| `40a607ec7ea` | Doc | spec-superflow 工件 |
| `6026e5cea3c` | Doc | 执行契约 |
| `4c54142b62d` | Refactor | PassThroughQueryTable 接口 + PassThroughQueryValidator |
| `67063fd39ee` | Refactor | native_query 框架通用化 |
| `c6c0f383bd6` | Feature | ES SQL Schema 推断（FE） |
| `c992a008927` | Feature | EsScanNode native_query 扫描范围 + Thrift |
| `7fa3542c771` | Feature | BE ESSqlReader + EsSqlResponseParser |
| `a07e841845f` | Test | FE 单元测试 |

## Verification Status

- ✅ 文件完整性：所有 22 个文件存在且非空
- ✅ 任务覆盖：6/6 Batch 完成，所有任务有对应代码变更
- ✅ Spec 覆盖：13/13 SHALL 要求映射到实现
- ✅ BE 模块边界：0 违规
- ✅ 无范围外变更
- ⚠️ FE/BE 编译未验证（AGENTS.md 约束，需用户按需运行）

## Deliverables

- 设计文档：`es_native_query_design.md`（1784 行）
- Spec-superflow 工件：proposal, specs, design, tasks, execution-contract
- FE 新增：PassThroughQueryTable, PassThroughQueryValidator
- FE 修改：JDBCTable, EsTable, TableFunctionRelation, QueryAnalyzer, RelationTransformer, ElasticsearchMetadata, EsRestClient, EsUtil, EsScanNode, PlanFragmentBuilder
- BE 新增：es_sql_reader.h/cpp, es_sql_parser.h/cpp
- BE 修改：es_connector.h/cpp, CMakeLists.txt
- 测试：PassThroughQueryValidatorTest, EsTableNativeQueryTest, ElasticsearchMetadataNativeQueryTest

## Known Limitations

- 单 BE 执行（v1 无并行扫描）
- 无 LIMIT 下推
- 无列裁剪（pass-through 固有特性）
- ES SQL 要求 ES 6.3+

## Delta Specs

无。`specs/native-query-es.md` 是新创建的 spec，不是 delta，无需 spec-merger 同步。
