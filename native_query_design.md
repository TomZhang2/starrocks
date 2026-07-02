# StarRocks native_query 表函数实现设计文档

> 对应提交：`16f5fb71870` [Feature] support jdbc native query (#72005)
> Bugfix：`4ee8f9ca560` [BugFix] Use Statement for JDBC native_query metadata probe (#72968)
> 文档：`docs/en/sql-reference/sql-functions/table-functions/native_query.md`
> 起始版本：v4.1

---

## 1. 概述

### 1.1 功能定义

`native_query` 是一个 **JDBC Catalog 专属的表函数**，允许用户将一段源数据库原生的 `SELECT` 语句以 pass-through 方式下推到 JDBC 外表所连接的源数据库执行，并将结果集作为 StarRocks 的一个关系（relation）暴露出来，供后续过滤、JOIN、聚合、投影或 `INSERT INTO` 加载使用。

### 1.2 适用场景

- 源库侧的预过滤子查询，难以通过单一 JDBC 外表表达
- 源库侧的多表 JOIN，希望在源库执行而非拉回 StarRocks
- 使用源库特定的 SQL 方言、函数或语法
- 将源库查询结果直接 `INSERT INTO` 加载到 StarRocks 内表

### 1.3 核心价值

无需为每个查询需求创建持久的 JDBC 外表，即可在 SQL 中动态发起源库原生查询，实现"即查即用"的联邦查询能力。

---

## 2. 语法与使用方式

### 2.1 语法

```sql
SELECT ...
FROM TABLE(<jdbc_catalog>.native_query('<select_sql>')) [AS] <alias>
[WHERE ...];
```

### 2.2 参数说明

| 参数             | 说明                                                 |
| -------------- | -------------------------------------------------- |
| `jdbc_catalog` | 已创建的 JDBC Catalog 名称，仅 JDBC Catalog 支持此函数          |
| `select_sql`   | 字符串字面量，包含源数据库原生 SQL 语句。去掉前导注释和尾部分号后必须以 `SELECT` 开头 |
| `alias`        | 可选的表别名                                             |

### 2.3 使用示例

```sql
-- 示例1：源库子查询 + StarRocks 外层过滤
SELECT id, name, doubled_score
FROM TABLE(jdbc0.native_query(
    'SELECT id, name, score * 2 AS doubled_score
     FROM app.orders
     WHERE score >= 20'
)) q
WHERE doubled_score < 70
ORDER BY id;

-- 示例2：对 pass-through 结果做聚合
SELECT category, SUM(score) AS total_score
FROM TABLE(jdbc0.native_query(
    'SELECT category, score
     FROM app.orders
     WHERE status = ''PAID'''
)) q
GROUP BY category;

-- 示例3：将 native_query 结果加载到 StarRocks 内表
INSERT INTO paid_order_summary
SELECT category, SUM(score) AS total_score
FROM TABLE(jdbc0.native_query(
    'SELECT category, score FROM app.orders WHERE status = ''PAID'''
)) q
GROUP BY category;
```

### 2.4 约束与不支持的形式

```sql
-- 不支持命名参数
SELECT * FROM TABLE(jdbc0.native_query(query => 'SELECT id FROM app.orders'));

-- 不支持旧版 system.query 别名
SELECT * FROM TABLE(jdbc0.system.query('SELECT id FROM app.orders'));

-- 不支持 WITH 开头的查询（必须以 SELECT 开头）
SELECT * FROM TABLE(jdbc0.native_query('WITH q AS (...) SELECT * FROM q'));

-- 不支持表别名后跟列别名
SELECT * FROM TABLE(jdbc0.native_query('SELECT id FROM app.orders')) q(id_alias);
```

---

## 3. 设计目标与约束

| 设计目标             | 实现方式                                                      |
| ---------------- | --------------------------------------------------------- |
| **最小侵入**         | 不新建 NativeQuery 类，复用现有 JDBC 扫描链路                          |
| **源库执行**         | pass-through SQL 原样下推，StarRocks 不做语法解析                    |
| **动态 Schema 推断** | 通过 `SELECT * FROM (query) WHERE 1=0` 探测 ResultSetMetaData |
| **锁优化**          | Schema 推断在 PlannerMetaLock 之外完成（耗时远程调用不持锁）                |
| **权限控制**         | 校验用户对 JDBC Catalog 的 USAGE 权限                             |
| **安全性**          | 仅允许 SELECT 语句，拒绝 INSERT/UPDATE/DELETE 等                   |

---

## 4. 整体架构

### 4.1 核心设计思想

`native_query` **不是**传统意义上的 builtin 表函数（如 `unnest`、`json_each`），它**不实现** BE 端的 `TableFunction` 抽象类，也**不在** `TableFunction.initBuiltins()` 中注册。

它的本质是一个 **JDBC 扫描链路的适配器（adapter）**：将一段 pass-through SQL 字符串转换为一个合成的 `JDBCTable`（以子查询作为表名），之后所有下游组件——逻辑算子、物理扫描节点、BE 执行器——都将其等同于普通的 JDBC 外表扫描处理。

```
用户 SQL: TABLE(jdbc0.native_query('SELECT ...'))
        │
        ▼
   ┌─────────────────────────────────┐
   │  SQL Parser                      │  生成 TableFunctionRelation
   │  (functionName = "jdbc0.native_query") │
   └────────────┬────────────────────┘
                │
                ▼
   ┌─────────────────────────────────┐
   │  QueryAnalyzer (FE)              │  识别 catalog.native_query 模式
   │  ├─ 参数校验（单字符串字面量）     │  校验 SELECT-only、无命名参数
   │  ├─ JDBCTable.normalizePassThroughQuery()
   │  └─ resolveJdbcQueryTable()      │  调用 ConnectorMetadata.getTableFromQuery()
   └────────────┬────────────────────┘
                │
                ▼
   ┌─────────────────────────────────┐
   │  JDBCMetadata.getTableFromQuery()│  Schema 探断
   │  SELECT * FROM (<query>)         │  执行 WHERE 1=0 探测查询
   │  starrocks_query WHERE 1 = 0     │  读取 ResultSetMetaData
   │  → 构建合成 JDBCTable            │  jdbcTable = "(<query>) starrocks_query"
   │    (queryTable=true)             │  queryTable = true
   └────────────┬────────────────────┘
                │
                ▼
   ┌─────────────────────────────────┐
   │  RelationTransformer (FE)        │  构建逻辑计划
   │  buildJdbcQueryTablePlan()       │  → LogicalJDBCScanOperator
   │  (复用现有 JDBC 扫描算子)         │  (与普通 JDBC 外表完全相同)
   └────────────┬────────────────────┘
                │
                ▼
   ┌─────────────────────────────────┐
   │  JDBCScanNode (FE)               │  物理扫描节点
   │  isQueryTable() → 跳过标识符引用  │  tableName 直接使用子查询表达式
   └────────────┬────────────────────┘
                │
                ▼
   ┌─────────────────────────────────┐
   │  BE JDBC Scanner (C++)           │  通过 JDBC Bridge 执行
   │  下推 SQL 到源数据库              │  源库执行 pass-through 查询
   └─────────────────────────────────┘
```

### 4.2 与 Builtin 表函数的对比

| 维度     | Builtin 表函数 (unnest, json_each...)                          | native_query                 |
| ------ | ----------------------------------------------------------- | ---------------------------- |
| 注册方式   | `TableFunction.initBuiltins()` + BE `TableFunctionResolver` | 无注册，按函数名点号解析动态分发             |
| 实现层    | FE `TableFunction` + BE `TableFunction` 抽象类                 | 纯 FE，复用 JDBC 扫描链路            |
| BE 参与  | 是（`TableFunctionNode` / pipeline operator）                  | 否（走 JDBC Scan 路径）            |
| Schema | 静态定义（参数类型 → 返回列类型）                                          | 动态推断（JDBC ResultSetMetaData） |
| 执行位置   | BE 本地计算                                                     | 源数据库远程执行                     |

---

## 5. 核心实现

### 5.1 SQL 解析与分发

**文件**：`fe/fe-core/src/main/java/com/starrocks/sql/analyzer/QueryAnalyzer.java`

用户书写 `TABLE(jdbc0.native_query('SELECT ...'))` 时，SQL Parser 生成 `TableFunctionRelation`，其 `functionName` 为 `"jdbc0.native_query"`（含点号）。

`QueryAnalyzer.Visitor.visitTableFunction()` 在处理常规 builtin 表函数之前，先调用 `tryResolveJdbcQueryTableFunction()` 进行拦截：

```java
// visitTableFunction() 入口
public Scope visitTableFunction(TableFunctionRelation node, Scope parent) {
    // ... 参数校验 ...
    List<String> names = node.getFunctionParams().getExprsNames();

    // 先尝试 native_query 分发
    Scope queryTableScope = tryResolveJdbcQueryTableFunction(node, names);
    if (queryTableScope != null) {
        return queryTableScope;
    }
    // ... 走常规 builtin 表函数逻辑 ...
}
```

**函数名解析**（两个版本）：

```java
// 严格版：用于完整分析阶段，遇到错误抛异常
private static JdbcQueryTableFunctionName tryParseJdbcQueryTableFunctionName(String functionName) {
    List<String> parts = Arrays.stream(functionName.split("\\."))
            .filter(part -> !part.isEmpty())
            .collect(Collectors.toList());
    if (parts.isEmpty()) return null;

    String lastPart = parts.get(parts.size() - 1);
    // 识别 catalog.native_query
    if (lastPart.equalsIgnoreCase("native_query")) {
        if (parts.size() == 2) {
            return new JdbcQueryTableFunctionName(parts.get(0));
        }
        throw new SemanticException(JDBC_QUERY_TABLE_FUNCTION_USAGE);
    }
    // 显式拒绝旧版 catalog.system.query
    if (lastPart.equalsIgnoreCase("query") && parts.size() == 3
            && parts.get(1).equalsIgnoreCase("system")) {
        throw new SemanticException(JDBC_QUERY_TABLE_FUNCTION_USAGE);
    }
    return null;
}

// 宽松版：用于无锁预解析阶段，遇到任何异常静默返回 null
private static JdbcQueryTableFunctionName tryParseCanonicalJdbcQueryTableFunctionName(String functionName) {
    // 仅在 parts.size()==2 且 parts.get(1).equalsIgnoreCase("native_query") 时返回
    // 不抛异常，失败返回 null
}
```

**参数校验**（`tryResolveJdbcQueryTableFunction`）：

```java
private Scope tryResolveJdbcQueryTableFunction(TableFunctionRelation node, List<String> argNames) {
    JdbcQueryTableFunctionName functionName = tryParseJdbcQueryTableFunctionName(...);
    if (functionName == null) return null;  // 非 native_query，走常规路径

    // 不允许表别名后跟列别名
    if (node.getColumnOutputNames() != null) {
        throw new SemanticException("column aliases are not supported...");
    }

    List<Expr> args = node.getFunctionParams().exprs();
    // 必须恰好一个参数
    if (args.size() != 1) {
        throw new SemanticException("requires exactly one query argument");
    }
    // 不允许命名参数
    if (argNames != null && !argNames.isEmpty()) {
        throw new SemanticException(JDBC_QUERY_TABLE_FUNCTION_USAGE);
    }
    // 参数必须是字符串字面量
    if (!(queryExpr instanceof StringLiteral)) {
        throw new SemanticException("argument must be a string literal");
    }

    // 规范化查询
    String passThroughQuery = JDBCTable.normalizePassThroughQuery(((StringLiteral) queryExpr).getStringValue());

    // 若预解析阶段已填充 queryTable，直接复用
    JDBCTable jdbcTable = node.getQueryTable();
    if (jdbcTable == null) {
        jdbcTable = resolveJdbcQueryTable(functionName, passThroughQuery);
    }
    return buildJdbcQueryTableScope(node, jdbcTable);
}
```

### 5.2 查询规范化与校验

**文件**：`fe/fe-core/src/main/java/com/starrocks/catalog/JDBCTable.java`

`JDBCTable` 新增 `queryTable` 布尔字段（Thrift 序列化名 `"qt"`），并提供查询规范化静态方法：

```java
@SerializedName(value = "qt")
private boolean queryTable;

// 将 pass-through 查询包装为子查询形式
public void setPassThroughQuery(String query) {
    jdbcTable = "(" + normalizePassThroughQuery(query) + ") starrocks_query";
    queryTable = true;
}

// 规范化：trim + 去尾部分号 + 校验
public static String normalizePassThroughQuery(String query) {
    String normalizedQuery = StringUtils.trimToEmpty(query);
    while (normalizedQuery.endsWith(";")) {
        normalizedQuery = StringUtils.stripEnd(
            normalizedQuery.substring(0, normalizedQuery.length() - 1), null);
    }
    if (normalizedQuery.isEmpty()) {
        throw new IllegalArgumentException("pass-through query cannot be empty");
    }
    validatePassThroughQuery(normalizedQuery);
    return normalizedQuery;
}

// 校验：去掉前导注释后必须以 SELECT 开头
private static void validatePassThroughQuery(String query) {
    String leadingSql = stripLeadingComments(query);
    if (!startsWithSqlKeyword(leadingSql, "select")) {
        throw new IllegalArgumentException(
            "JDBC query table function only supports SELECT queries");
    }
}
```

`stripLeadingComments` 处理两种注释格式：

- `--` 行注释：跳到行尾
- `/* */` 块注释：找到 `*/` 结束位置

`startsWithSqlKeyword` 使用大小写不敏感匹配，并检查关键字后的字符不是字母/数字/下划线（避免匹配到 `selection` 之类）。

### 5.3 Schema 推断

**文件**：`fe/fe-core/src/main/java/com/starrocks/connector/jdbc/JDBCMetadata.java`

`getTableFromQuery()` 是 native_query 的核心——通过执行零行探测查询来推断结果集 Schema：

```java
@Override
public Table getTableFromQuery(ConnectContext context, String dbName, String query) {
    String normalizedQuery = JDBCTable.normalizePassThroughQuery(query);
    // 构建探测查询：SELECT * FROM (<query>) starrocks_query WHERE 1 = 0
    String metadataQuery = "SELECT * FROM (" + normalizedQuery + ") starrocks_query WHERE 1 = 0";

    try (Connection connection = getConnection();
            Statement statement = connection.createStatement()) {  // Bugfix #72968: 用 Statement 替代 PreparedStatement
        int queryTimeoutSeconds = schemaResolver.getQueryTimeoutSeconds();
        if (queryTimeoutSeconds > 0) {
            statement.setQueryTimeout(queryTimeoutSeconds);
        }

        try (ResultSet resultSet = statement.executeQuery(metadataQuery)) {
            // 从 ResultSetMetaData 推断列类型
            Map<String, Integer> originalJdbcTypes = new HashMap<>();
            List<Column> fullSchema = schemaResolver.convertToSRTable(
                    resultSet.getMetaData(), originalJdbcTypes);
            if (fullSchema.isEmpty()) {
                throw new StarRocksConnectorException("pass-through query returned no columns");
            }

            // 构建合成的 JDBCTable
            int tableId = ConnectorTableId.CONNECTOR_ID_GENERATOR.getNextId().asInt();
            JDBCTable queryTable = new JDBCTable(tableId, "_query_" + tableId,
                    fullSchema, dbName, catalogName, properties);
            queryTable.setPassThroughQuery(normalizedQuery);  // 设置 jdbcTable + queryTable=true
            if (!originalJdbcTypes.isEmpty()) {
                queryTable.setOriginalJdbcColumnTypes(originalJdbcTypes);
            }
            return queryTable;
        }
    } catch (SQLException | DdlException e) {
        throw new StarRocksConnectorException("get query table for JDBC catalog fail!", e);
    }
}
```

**类型映射**由 `JDBCSchemaResolver.convertToSRTable(ResultSetMetaData, Map)` 完成，遍历 JDBC 结果集的每一列：

- 调用 `convertColumnType()` 将 JDBC 类型映射为 StarRocks `Type`
- 记录原始 JDBC 类型码到 `originalJdbcTypes`（供后续 BE 扫描使用）
- 调用 `normalizeColumnName()` 进行方言相关的列名规范化

**方言扩展**：`PostgresSchemaResolver` 重写 `normalizeColumnName()` 以实现 Postgres 的小写化语义。其他方言（MySQL、ClickHouse、Oracle、SQL Server）使用各自 SchemaResolver 的默认行为。

**接口声明**（`ConnectorMetadata.java`）：

```java
/**
 * Build a temporary table from a pass-through query when the connector can infer the result schema.
 */
default Table getTableFromQuery(ConnectContext context, String dbName, String query) {
    return null;  // 默认不支持，仅 JDBC 实现了此方法
}
```

`CatalogConnectorMetadata` 作为委托包装器，将调用转发到底层的 `JDBCMetadata`。

### 5.4 锁优化——无锁预解析

**文件**：`fe/fe-core/src/main/java/com/starrocks/sql/analyzer/QueryAnalyzer.java`（`ExternalTablesOnlyVisitor`）

JDBC Schema 推断涉及远程数据库调用，可能耗时较长。为避免在分析期间长时间持有 `PlannerMetaLock`，`native_query` 采用了**两阶段解析**策略：

```
阶段1（无锁）：ExternalTablesOnlyVisitor.visitTableFunction()
    └─ 调用 resolveJdbcQueryTable() 完成远程 Schema 推断
    └─ 将结果存入 TableFunctionRelation.queryTable

阶段2（持锁）：Visitor.visitTableFunction() → tryResolveJdbcQueryTableFunction()
    └─ 检查 node.getQueryTable() 是否已填充
    └─ 若已填充，直接复用，跳过远程调用
    └─ 若未填充（预解析失败），在锁内重新解析
```

`ExternalTablesOnlyVisitor.visitTableFunction()` 使用宽松版解析器 `tryParseCanonicalJdbcQueryTableFunctionName()`，遇到任何不符合条件的情况静默返回 null（不抛异常），确保预解析不会阻断正常流程：

```java
@Override
public Void visitTableFunction(TableFunctionRelation node, Void context) {
    if (node.getQueryTable() != null) return null;  // 已解析

    JdbcQueryTableFunctionName functionName =
            tryParseCanonicalJdbcQueryTableFunctionName(node.getFunctionName().getFunction());
    if (functionName == null) return null;  // 非 native_query

    // 快速校验参数格式，不通过则跳过
    // ...
    if (args.size() != 1 || (argNames != null && !argNames.isEmpty())) return null;
    if (!(queryExpr instanceof StringLiteral)) return null;

    // 无锁环境下执行远程 Schema 推断
    try (Timer ignored = Tracers.watchScope("AnalyzeTable")) {
        jdbcTable = resolveJdbcQueryTable(functionName, passThroughQuery);
    }
    node.setQueryTable(jdbcTable);  // 缓存结果
    return null;
}
```

### 5.5 权限校验

**文件**：`fe/fe-core/src/main/java/com/starrocks/sql/analyzer/AuthorizerStmtVisitor.java`

`native_query` 不涉及具体外表对象，因此不走常规的表级 `SELECT` 权限校验，而是校验用户对 JDBC Catalog 的 `USAGE` 权限：

```java
public void checkSelectTableAction(ConnectContext context, QueryStatement statement, List<TableName> excludeTables) {
    checkNativeQueryCatalogUsage(context, statement);  // 新增：native_query catalog 权限
    ColumnPrivilege.check(context, statement, excludeTables);
}

private void checkNativeQueryCatalogUsage(ConnectContext context, QueryStatement statement) {
    if (statement == null) return;

    Set<String> catalogs = new HashSet<>();
    // 遍历 AST，收集所有 native_query 涉及的 catalog
    new NativeQueryCatalogCollector(catalogs).visit(statement);

    for (String catalog : catalogs) {
        try {
            Authorizer.checkCatalogAction(context, catalog, PrivilegeType.USAGE);
        } catch (AccessDeniedException e) {
            AccessDeniedException.reportAccessDenied(
                    catalog, context.getCurrentUserIdentity(),
                    context.getCurrentRoleIds(),
                    PrivilegeType.USAGE.name(),
                    ObjectType.CATALOG.name(), catalog);
        }
    }
}

// AST 遍历器：收集 queryTable 不为空的 TableFunctionRelation 对应的 catalog 名
private static class NativeQueryCatalogCollector extends AstTraverser<Void, Void> {
    private final Set<String> catalogs;

    @Override
    public Void visitTableFunction(TableFunctionRelation node, Void context) {
        if (node.getQueryTable() != null) {
            catalogs.add(node.getQueryTable().getCatalogName());
        }
        return null;
    }

    @Override
    public Void visitNormalizedTableFunction(NormalizedTableFunctionRelation node, Void context) {
        if (node.getRight() != null) visit(node.getRight(), context);
        return null;
    }
}
```

### 5.6 逻辑计划构建

**文件**：`fe/fe-core/src/main/java/com/starrocks/sql/optimizer/transformer/RelationTransformer.java`

`visitTableFunction()` 在入口处检查 `queryTable`，若不为 null 则走 native_query 专用路径，复用现有 `LogicalJDBCScanOperator`：

```java
@Override
public LogicalPlan visitTableFunction(TableFunctionRelation node, ExpressionMapping context) {
    // native_query 快速路径
    if (node.getQueryTable() != null) {
        return buildJdbcQueryTablePlan(node);
    }
    // ... 常规 builtin 表函数逻辑 ...
}

private LogicalPlan buildJdbcQueryTablePlan(TableFunctionRelation node) {
    JDBCTable table = node.getQueryTable();
    List<Field> relationFields = node.getRelationFields().getAllFields();
    List<Column> fullSchema = table.getFullSchema();

    // 为每列创建 ColumnRefOperator
    int relationId = columnRefFactory.getNextRelationId();
    for (int i = 0; i < fullSchema.size(); i++) {
        Column column = fullSchema.get(i);
        Field field = relationFields.get(i);
        ColumnRefOperator columnRef = columnRefFactory.create(
                field.getName(), field.getType(), column.isAllowNull());
        columnRefFactory.updateColumnToRelationIds(columnRef.getId(), relationId);
        columnRefFactory.updateColumnRefToColumns(columnRef, column, table);
        // ... 建立 columnRef ↔ column 映射 ...
    }

    // 复用 LogicalJDBCScanOperator（与普通 JDBC 外表完全相同的算子）
    LogicalScanOperator scanOperator = new LogicalJDBCScanOperator(table,
            colRefToColumnMetaMap, columnMetaToColRefMap,
            Operator.DEFAULT_LIMIT, null, null);
    return new LogicalPlan(new OptExprBuilder(scanOperator, ...), outputVariables, List.of());
}
```

对于 LATERAL JOIN 形式（`NormalizedTableFunctionRelation`），同样做短路处理：

```java
@Override
public LogicalPlan visitNormalizedTableFunction(NormalizedTableFunctionRelation node, ExpressionMapping context) {
    if (node.getRight() instanceof TableFunctionRelation
            && ((TableFunctionRelation) node.getRight()).getQueryTable() != null) {
        return visit(node.getRight(), context);  // 直接当作普通 JDBC 扫描
    }
    // ... 常规 lateral join 逻辑 ...
}
```

### 5.7 物理执行

**文件**：`fe/fe-core/src/main/java/com/starrocks/planner/JDBCScanNode.java`

`JDBCScanNode` 构造函数检测 `isQueryTable()`，若是则跳过标识符引用（因为表名已经是子查询表达式 `(SELECT ...) starrocks_query`，不是合法的标识符）：

```java
public JDBCScanNode(PlanNodeId id, TupleDescriptor desc, JDBCTable tbl) {
    super(id, desc, "SCAN JDBC");
    table = tbl;
    if (tbl.isQueryTable()) {
        tableName = tbl.getCatalogTableName();  // 直接使用子查询表达式，不加引号
    } else {
        String objectIdentifier = getIdentifierSymbol();
        tableName = wrapWithIdentifier(tbl.getCatalogTableName(), objectIdentifier);  // 正常引用
    }
}
```

`JDBCTable.toThrift()` 中，当 `queryTable` 为 true 时，直接设置 `jdbc_url` 而非表名：

```java
// JDBCTable.toThrift()
if (connectInfo.get(JDBC_TABLENAME) != null || queryTable || Strings.isNullOrEmpty(dbName)) {
    tJDBCTable.setJdbc_url(uri);  // queryTable 走此分支
} else {
    // 正常 JDBC 外表：在 URL 中拼接表名
}
```

最终下推到源数据库的 SQL 形如：

```sql
SELECT `col1`, `col2` FROM (SELECT ... FROM app.orders WHERE ...) starrocks_query
```

源数据库执行该子查询并返回结果集，BE 通过 JDBC Bridge 拉取数据，后续流程与普通 JDBC 外表扫描完全一致。

---

## 6. 关键设计决策

### 6.1 为何复用 JDBC 扫描链路而非新建表函数

| 方案                                   | 优点                     | 缺点                                     |
| ------------------------------------ | ---------------------- | -------------------------------------- |
| **新建 BE TableFunction**（如 unnest 模式） | 架构统一                   | 需在 BE 端实现 JDBC 调用、结果集反序列化、类型映射等，大量重复代码 |
| **复用 JDBC 扫描链路**（实际选择）               | 零重复代码，仅 FE 层适配；BE 完全无感 | native_query 表函数与 JDBC 外表扫描共享执行路径      |

实际方案只需将 pass-through SQL 包装为 `(query) starrocks_query` 子查询并设为 `JDBCTable.jdbcTable`，整个 JDBC 扫描链路（`LogicalJDBCScanOperator` → `JDBCScanNode` → BE JDBC Scanner → JDBC Bridge）无需修改即可工作。

### 6.2 为何使用 Statement 而非 PreparedStatement

Bugfix `4ee8f9ca560`（#72968）将 Schema 探断从 `PreparedStatement` 改为 `Statement`：

- 部分 JDBC 驱动在 `PreparedStatement.getMetaData()` 时行为不一致
- `Statement.executeQuery(metadataQuery)` 更通用，兼容性更好
- 探断查询 `SELECT * FROM (query) WHERE 1=0` 无参数绑定需求

### 6.3 为何校验 SELECT-only

`normalizePassThroughQuery` → `validatePassThroughQuery` 强制要求去掉前导注释后以 `SELECT` 开头，拒绝 `WITH`/`INSERT`/`UPDATE`/`DELETE` 等：

- **安全性**：防止通过 native_query 执行 DML/DDL 修改源库数据
- **语义一致性**：native_query 的语义是"查询"，返回结果集关系
- **WITH 限制**：部分数据库的 WITH 语法行为复杂，限制为 SELECT 开头降低风险

### 6.4 为何拒绝列别名

不支持 `TABLE(catalog.native_query('...')) q(c1, c2)` 形式的列别名，要求在 `select_sql` 内部定义别名（如 `SELECT id AS id_alias`）：

- 列名来自源库 ResultSetMetaData，用户无法预知
- 内部别名由源库解析，语义更清晰
- 避免列名与数量不匹配的校验复杂度

---

## 7. 数据流总结

```
┌──────────────────────────────────────────────────────────────────┐
│ 1. 用户 SQL                                                       │
│    TABLE(jdbc0.native_query('SELECT id, score*2 FROM orders'))   │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 2. SQL Parser → TableFunctionRelation                             │
│    functionName = "jdbc0.native_query"                            │
│    args = [StringLiteral("SELECT id, score*2 FROM orders")]       │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 3. ExternalTablesOnlyVisitor（无锁预解析）                         │
│    tryParseCanonicalJdbcQueryTableFunctionName → catalog="jdbc0" │
│    resolveJdbcQueryTable → JDBCMetadata.getTableFromQuery()       │
│    ┌─────────────────────────────────────────────────────────┐   │
│    │ 探断查询: SELECT * FROM (SELECT id, score*2 FROM orders) │   │
│    │          starrocks_query WHERE 1 = 0                     │   │
│    │ → ResultSetMetaData → Column[id INT, score*2 DOUBLE]     │   │
│    │ → JDBCTable("_query_42", queryTable=true)                │   │
│    │   jdbcTable = "(SELECT id, score*2 FROM orders)          │   │
│    │               starrocks_query"                           │   │
│    └─────────────────────────────────────────────────────────┘   │
│    TableFunctionRelation.queryTable = JDBCTable                   │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 4. QueryAnalyzer.Visitor（持锁完整分析）                           │
│    tryResolveJdbcQueryTableFunction → 复用预解析结果               │
│    buildJdbcQueryTableScope → 构建 Field/Scope                    │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 5. AuthorizerStmtVisitor（权限校验）                               │
│    NativeQueryCatalogCollector → catalogs = {"jdbc0"}             │
│    Authorizer.checkCatalogAction(jdbc0, USAGE)                    │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 6. RelationTransformer → LogicalJDBCScanOperator                  │
│    buildJdbcQueryTablePlan → 创建 ColumnRefOperator +             │
│    LogicalJDBCScanOperator(JDBCTable)                             │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 7. JDBCScanNode（物理计划）                                        │
│    isQueryTable()=true → tableName = "(SELECT ...) starrocks_query"│
│    不加标识符引用                                                  │
└──────────────────────┬───────────────────────────────────────────┘
                       ▼
┌──────────────────────────────────────────────────────────────────┐
│ 8. BE JDBC Scanner → JDBC Bridge → 源数据库                       │
│    下推 SQL: SELECT col1, col2 FROM                              │
│              (SELECT id, score*2 FROM orders) starrocks_query     │
│    源库执行子查询，返回结果集                                      │
└──────────────────────────────────────────────────────────────────┘
```

---

## 8. 涉及文件清单

### 8.1 核心实现文件（均在 `origin/main`）

| 文件                                                                   | 改动行数 | 职责                             |
| -------------------------------------------------------------------- | ---- | ------------------------------ |
| `fe/.../sql/analyzer/QueryAnalyzer.java`                             | +201 | 入口分发、参数校验、Schema 解析、无锁预解析      |
| `fe/.../catalog/JDBCTable.java`                                      | +94  | queryTable 标志、查询规范化、Thrift 序列化 |
| `fe/.../connector/jdbc/JDBCMetadata.java`                            | +33  | Schema 探断、合成 JDBCTable 构建      |
| `fe/.../connector/ConnectorMetadata.java`                            | +7   | getTableFromQuery 接口声明         |
| `fe/.../connector/CatalogConnectorMetadata.java`                     | +5   | 委托转发                           |
| `fe/.../sql/optimizer/transformer/RelationTransformer.java`          | +47  | 逻辑计划构建                         |
| `fe/.../sql/analyzer/AuthorizerStmtVisitor.java`                     | +51  | 权限校验                           |
| `fe/.../sql/ast/TableFunctionRelation.java`                          | +12  | AST 节点扩展                       |
| `fe/.../planner/JDBCScanNode.java`                                   | +8   | 物理节点标识符处理                      |
| `fe/.../connector/jdbc/JDBCSchemaResolver.java`                      | +32  | 类型映射                           |
| `fe/.../connector/jdbc/PostgresSchemaResolver.java`                  | +12  | Postgres 列名规范化                 |
| `fe/.../sql/optimizer/operator/logical/LogicalJDBCScanOperator.java` | -1   | 复用，无修改                         |
| `fe/.../sql/formatter/AST2SQLVisitor.java`                           | +3   | SQL 格式化                        |
| `fe/.../sql/optimizer/dump/DesensitizedSQLBuilder.java`              | +8   | Dump 脱敏                        |

### 8.2 测试文件

| 文件                                                            | 说明                     |
| ------------------------------------------------------------- | ---------------------- |
| `test/sql/test_jdbc_catalog/T/test_native_query`              | SQL 回归测试（正向用例）         |
| `test/sql/test_jdbc_catalog/R/test_native_query`              | SQL 回归测试（期望结果 + 错误用例）  |
| `fe/.../catalog/JDBCTableTest.java`                           | JDBCTable 单元测试         |
| `fe/.../connector/jdbc/JDBCMetadataTest.java`                 | getTableFromQuery 单元测试 |
| `fe/.../connector/jdbc/MockedJDBCMetadata.java`               | JDBC 元数据 Mock          |
| `fe/.../planner/MySqlAndJDBCScanNodeTest.java`                | 扫描节点测试                 |
| `fe/.../sql/plan/StatementPlannerExternalTablesLockTest.java` | 无锁预解析测试                |
| `fe/.../sql/plan/JDBCIdentifierQuoteTest.java`                | 标识符引用测试                |

---

## 9. 扩展性

### 9.1 支持新的 Catalog 类型

`native_query` 的能力通过 `ConnectorMetadata.getTableFromQuery()` 接口暴露，默认返回 `null`（不支持）。任何 Connector 只需重写此方法即可支持 native_query，当前仅 `JDBCMetadata` 实现。

### 9.2 方言适配

通过 `JDBCSchemaResolver` 的子类实现方言适配：

- **类型映射**：`convertColumnType()` 将 JDBC 类型映射为 StarRocks 类型
- **列名规范化**：`normalizeColumnName()` 处理方言相关的命名规则（如 Postgres 小写化）
- **查询超时**：`getQueryTimeoutSeconds()` 由各方言配置

现有方言实现：MySQL、PostgreSQL、ClickHouse、Oracle、SQL Server。

---

## 10. 版本说明

- native_query 功能在 `origin/main` 分支上（提交 `16f5fb71870`，2026-04-27 合入）
- Bugfix `4ee8f9ca560`（2026-05-12）修复了 Schema 探断的 Statement 兼容性问题
- 官方文档标注为 v4.1 起支持
- 当前工作目录所在分支 `branch-4.1.2` **不包含**此功能的实现代码，仅包含文档文件
