# Spec: ES Catalog native_query

## ADDED Requirements

### REQ-ES-NQ-001: native_query 表函数支持 ES Catalog

`native_query` 表函数 SHALL 接受 ES Catalog 名称作为函数名前缀，将 ES SQL 语句 pass-through 下推到 ES 集群执行。

#### Scenario: 基本 ES SQL pass-through 查询

```
GIVEN 已创建 ES Catalog "es0"，ES 集群版本 >= 6.3
WHEN 用户执行 SELECT * FROM TABLE(es0.native_query('SELECT id, name FROM logs LIMIT 10'))
THEN 结果集包含 logs 索引中的 id 和 name 列
AND ES 集群收到 POST /_sql 请求，请求体包含 {"query": "SELECT id, name FROM logs LIMIT 10"}
```

#### Scenario: ES SQL 聚合查询

```
GIVEN 已创建 ES Catalog "es0"
WHEN 用户执行 SELECT category, SUM(cnt) FROM TABLE(es0.native_query('SELECT category, COUNT(*) AS cnt FROM products GROUP BY category')) q GROUP BY category
THEN ES 集群执行聚合 SQL
AND StarRocks 对 ES 返回的聚合结果做二次聚合
```

#### Scenario: ES SQL 结果加载到内表

```
GIVEN 已创建 ES Catalog "es0" 和 StarRocks 内表 error_summary
WHEN 用户执行 INSERT INTO error_summary SELECT level, COUNT(*) FROM TABLE(es0.native_query('SELECT level FROM "logstash-*" WHERE level = ''ERROR''')) q GROUP BY level
THEN ES SQL 查询结果被加载到 error_summary 表
```

### REQ-ES-NQ-002: ES SQL Schema 推断

`ElasticsearchMetadata.getTableFromQuery()` SHALL 通过执行 `POST /_sql` with `fetch_size:1` 探测结果集列元数据，从响应的 `columns` 数组推断 Schema。

#### Scenario: Schema 推断从 columns 元数据获取列名和类型

```
GIVEN ES SQL 查询 'SELECT id, name, score FROM logs'
WHEN getTableFromQuery 执行探测查询
THEN POST /_sql 请求体为 {"query": "SELECT id, name, score FROM logs", "fetch_size": 1, "time_zone": "<session_tz>"}
AND 返回的 EsTable 的 fullSchema 包含 3 列
AND 每列的名称和类型从 ES SQL columns 响应映射
AND 探测后立即关闭 cursor（POST /_sql/close）
```

#### Scenario: ES SQL 返回空列时报错

```
GIVEN ES SQL 查询返回 0 列
WHEN getTableFromQuery 执行探测查询
THEN 抛出 StarRocksConnectorException("ES native query returned no columns")
```

### REQ-ES-NQ-003: ES SQL 类型映射

`EsUtil.convertEsSqlType()` SHALL 将 ES SQL 类型名映射为 StarRocks 类型。

#### Scenario: 基本类型映射

```
GIVEN ES SQL 响应包含 columns: [{"name": "id", "type": "long"}, {"name": "name", "type": "text"}, {"name": "ts", "type": "datetime"}]
WHEN convertEsSqlType 处理每列
THEN id 映射为 BIGINT，name 映射为 VARCHAR，ts 映射为 DATETIME
```

#### Scenario: 未知类型回退为 VARCHAR

```
GIVEN ES SQL 返回一个未识别的类型 "unknown_type"
WHEN convertEsSqlType 处理该类型
THEN 映射为 VARCHAR（不报错）
```

### REQ-ES-NQ-004: EsTable pass-through 标记

`EsTable` SHALL 实现 `PassThroughQueryTable` 接口，通过 `isQueryTable()` 标记为 native query 合成表。

#### Scenario: setPassThroughQuery 设置标记

```
GIVEN 一个 EsTable 实例
WHEN 调用 setPassThroughQuery("SELECT * FROM logs")
THEN isQueryTable() 返回 true
AND getPassThroughQuery() 返回 "SELECT * FROM logs"
```

### REQ-ES-NQ-005: EsScanNode 单一扫描范围

当 `EsTable.isQueryTable()` 为 true 时，`EsScanNode` SHALL 创建单一扫描范围（非分片），使用 ES 集群配置的节点地址。

#### Scenario: native query 创建单一扫描范围

```
GIVEN EsTable.isQueryTable() == true
WHEN EsScanNode 构建扫描范围
THEN 创建恰好 1 个 TScanRangeLocations
AND TEsScanRange.es_hosts 包含 ES 集群节点地址
AND TEsScanRange.index 为空字符串（哨兵值）
AND TEsScanRange.shard_id 为 -1（哨兵值）
AND properties 中包含 "native_query" key，值为 ES SQL 字符串
```

### REQ-ES-NQ-006: BE ESSqlReader cursor 分页

`ESSqlReader` SHALL 通过 ES SQL cursor 实现分页：首次请求 `POST /_sql` with query + fetch_size，后续请求 `POST /_sql` with cursor。

#### Scenario: 首次请求获取 columns 和 rows

```
GIVEN ESSqlReader 已初始化，SQL = "SELECT id FROM logs"
WHEN 调用 open()
THEN 发送 POST /_sql 请求体 {"query": "SELECT id FROM logs", "fetch_size": <batch_size>, "time_zone": "<tz>"}
AND 从响应解析 columns 元数据
AND 从响应解析 rows 并转换为 Chunk
AND 保存 cursor 用于后续分页
```

#### Scenario: cursor 分页获取后续数据

```
GIVEN 首次请求已完成，cursor 已保存
WHEN 调用 get_next(chunk)
THEN 发送 POST /_sql 请求体 {"cursor": "<saved_cursor>"}
AND 从响应解析 rows（columns 不在 cursor 响应中）
AND 更新 cursor
```

#### Scenario: cursor 失效时报错（不静默截断）

```
GIVEN cursor 分页请求发送后
WHEN ES 返回 {"error": {"type": "...", "reason": "cursor expired"}}
THEN ESSqlReader 返回 Status::InternalError
AND 不静默返回空结果或 EOS
```

#### Scenario: 无 cursor 时标记 EOS

```
GIVEN cursor 分页请求发送后
WHEN ES 响应不包含 "cursor" 字段
THEN ESSqlReader 标记 _eos = true
AND 下次 get_next 返回 Status::EndOfFile
```

### REQ-ES-NQ-007: ESSqlReader SSL/认证/超时/故障转移

`ESSqlReader._http_post()` SHALL 对每个 HTTP 请求设置 SSL 信任、Basic Auth 认证、超时、节点故障转移。

#### Scenario: HTTPS ES 集群连接

```
GIVEN ES 集群启用 HTTPS
WHEN ESSqlReader 发送 POST /_sql 请求
THEN HttpClient 调用 trust_all_ssl()
AND 请求成功发送到 HTTPS 端点
```

#### Scenario: 认证集群连接

```
GIVEN ES 集群启用 Basic Auth
WHEN ESSqlReader 发送 POST /_sql 请求
THEN HttpClient 调用 set_basic_auth(user, password)
AND 请求包含 Authorization 头
```

#### Scenario: 节点故障转移

```
GIVEN ES 集群有 3 个节点，第一个节点不可达
WHEN ESSqlReader 发送 POST /_sql 请求
THEN 尝试第一个节点失败后自动切换到第二个节点
AND 请求在第二个节点成功
```

### REQ-ES-NQ-008: cursor 清理

`ESSqlReader` SHALL 在查询结束或析构时 best-effort 关闭 cursor。

#### Scenario: 正常结束时关闭 cursor

```
GIVEN ESSqlReader 查询完成（EOS）
WHEN 调用 close()
THEN 发送 POST /_sql/close {"cursor": "<cursor>"}
AND cursor 在 ES 侧被释放
```

#### Scenario: 析构时 best-effort 关闭

```
GIVEN ESSqlReader 有未关闭的 cursor
WHEN 析构函数执行
THEN best-effort 调用 _close_cursor()
AND 若关闭失败仅记录 WARNING 日志（cursor 依赖 ES keepalive 自动过期）
```

### REQ-ES-NQ-009: fetch_size 上限保护

`ESSqlReader` 的 `fetch_size` SHALL 不超过 `config::es_index_max_result_window`（默认 10000），防止宽行 OOM。

#### Scenario: chunk_size 超过上限时截断

```
GIVEN chunk_size = 50000，es_index_max_result_window = 10000
WHEN 创建 ESSqlReader
THEN ESSqlReader 的 batch_size = 10000
AND 记录 INFO 日志 "ES SQL fetch_size capped to 10000"
```

### REQ-ES-NQ-010: datetime ISO8601 解析

BE 侧 SHALL 正确解析 ES SQL 返回的 ISO8601 格式 datetime 字符串，包括 'T' 分隔符、毫秒和时区后缀。

#### Scenario: 解析带毫秒和时区的 datetime

```
GIVEN ES SQL 响应 rows 包含 datetime 值 "2024-01-01T12:00:00.123Z"
WHEN EsSqlResponseParser 解析该值
THEN 归一化为 "2024-01-01 12:00:00.123"
AND 成功转换为 StarRocks DateTimeValue
AND 追加到 DATETIME 列
```

#### Scenario: time_zone 注入到 _sql 请求

```
GIVEN ES Catalog 配置了 time_zone = "Asia/Shanghai"
WHEN ESSqlReader 发送首次 _sql 请求
THEN 请求体包含 "time_zone": "Asia/Shanghai"
AND ES 返回的 datetime 值为上海时区本地时间
```

## MODIFIED Requirements

### REQ-GEN-001: native_query 框架通用化

`native_query` 表函数的框架代码 SHALL 支持任意实现了 `PassThroughQueryTable` 接口的 Catalog 类型，不再硬编码为 JDBC。

#### Scenario: ES Catalog 通过 PassThroughQueryTable 接口被接受

```
GIVEN ElasticsearchMetadata.getTableFromQuery() 返回 EsTable 且 EsTable.isQueryTable() == true
WHEN QueryAnalyzer.resolveQueryTable() 检查返回值
THEN 检查 table instanceof PassThroughQueryTable 且 isQueryTable() == true
AND 不抛出 "does not support native query table function" 异常
```

#### Scenario: JDBC native_query 行为不变

```
GIVEN JDBC Catalog 的 native_query 查询
WHEN 执行 TABLE(jdbc0.native_query('SELECT * FROM t'))
THEN 行为与通用化改造前完全一致
AND LogicalJDBCScanOperator 被创建（非 LogicalEsScanOperator）
```

### REQ-GEN-002: 无锁预解析异常处理

`ExternalTablesOnlyVisitor.visitTableFunction()` SHALL 在 `resolveQueryTable()` 抛出 `RuntimeException` 时静默返回 null，让持锁阶段重新解析。

#### Scenario: ES 不可达时预解析不崩溃

```
GIVEN ES 集群不可达
WHEN ExternalTablesOnlyVisitor 预解析 native_query
THEN resolveQueryTable() 抛出 RuntimeException
AND 预解析 catch 该异常并返回 null（不传播）
AND 持锁阶段重新解析并报出规范错误
```

### REQ-GEN-003: normalizePassThroughQuery 公共化

`PassThroughQueryValidator.normalize()` SHALL 提供 `JDBCTable.normalizePassThroughQuery()` 的等价校验逻辑，供 ES 和 JDBC 共用。

#### Scenario: ES SQL 校验 SELECT-only

```
GIVEN 用户传入 "DELETE FROM logs"
WHEN PassThroughQueryValidator.normalize() 处理
THEN 抛出 IllegalArgumentException("native query table function only supports SELECT queries")
```

#### Scenario: 去除尾部分号

```
GIVEN 用户传入 "SELECT * FROM logs;"
WHEN PassThroughQueryValidator.normalize() 处理
THEN 返回 "SELECT * FROM logs"（无尾部分号）
```
