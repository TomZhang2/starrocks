# ES native_query 适配 OpenSearch 完整分析

> 日期：2026-07-09
> 分支：branch-3.4.5
> 范围：将 ES native_query 能力适配到 OpenSearch（通过 ES catalog 查询 OS）

---

## 目录

- [1. 当前状态概要](#1-当前状态概要)
- [2. 第一层：BE 侧 — 端点与响应格式](#2-第一层be-侧--端点与响应格式)
- [3. 第二层：FE 侧 — ES Catalog 连接修复](#3-第二层fe-侧--es-catalog-连接修复)
- [4. 第三层：FE 侧 — native_query 管道](#4-第三层fe-侧--native_query-管道)
- [5. 统一 vs 独立 Connector 的架构选择](#5-统一-vs-独立-connector-的架构选择)
- [6. 完整改动清单](#6-完整改动清单)
- [7. 附录：ES vs OpenSearch SQL API 逐项对比](#7-附录es-vs-opensearch-sql-api-逐项对比)

---

## 1. 当前状态概要

### 关键发现

> **native_query 的 FE 侧在 branch-3.4.5 上完全不存在。**

BE 侧有 `ESSqlReader` + `EsSqlResponseParser` + `es_connector.cpp` 中的 native_query 分支，但 FE 从未将 `native_query` 属性放入 `TEsScanNode.properties`。以下概念在当前代码库中均不存在：

- `PassThroughQueryTable` 接口
- `PassThroughQueryValidator` 类
- `KEY_NATIVE_QUERY` 常量
- `isQueryTable()` / `getPassThroughQuery()` 方法
- `computeQueryTableScanRanges()` 方法

字符串 `native_query` 在整个仓库中仅出现在一个文件：`be/src/connector/es_connector.cpp`。

### 适配工作分三层

```
┌─────────────────────────────────────────────────────────────┐
│  第一层：BE 侧 — 端点与响应格式适配（4 处关键改动）          │
│  es_sql_reader.cpp / es_sql_parser.cpp                     │
├─────────────────────────────────────────────────────────────┤
│  第二层：FE 侧 — ES Catalog 连接修复（3 处关键改动）         │
│  EsRestClient / EsMajorVersion / EsTable                  │
├─────────────────────────────────────────────────────────────┤
│  第三层：FE 侧 — native_query 管道搭建（4 处新增）           │
│  EsTable / EsScanNode                                      │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. 第一层：BE 侧 — 端点与响应格式

### 2.1 端点路径差异

| 用途 | Elasticsearch | OpenSearch |
|------|--------------|------------|
| 查询 | `POST /_sql` | `POST /_plugins/_sql` |
| 游标分页 | `POST /_sql` (body 含 cursor) | `POST /_plugins/_sql` (body 含 cursor) |
| 关闭游标 | `POST /_sql/close` | `POST /_plugins/_sql/close` |
| Explain | `POST /_sql/_explain` | `POST /_plugins/_sql/_explain` |

### 2.2 响应格式差异

**ES SQL `json` 格式响应：**

```json
{
  "columns": [
    {"name": "author",       "type": "text"},
    {"name": "page_count",   "type": "short"},
    {"name": "release_date", "type": "datetime"}
  ],
  "rows": [
    ["Peter F. Hamilton", 768, "2004-03-02T00:00:00.000Z"]
  ],
  "cursor": "sDXF1ZXJ5QW5kRmV0Y2gB..."
}
```

**OpenSearch SQL `jdbc` 格式响应（默认）：**

```json
{
  "schema": [
    {"name": "firstname", "type": "text"},
    {"name": "lastname",  "type": "text"}
  ],
  "cursor": "d:eyJhIjp7fSwi...",
  "total": 956,
  "datarows": [
    ["Cherry", "Carey"],
    ["Lindsey", "Hawkins"]
  ],
  "size": 5,
  "status": 200
}
```

**字段名映射：**

| 用途 | ES SQL | OpenSearch SQL (jdbc) |
|------|--------|----------------------|
| 列元数据 | `columns` | `schema` |
| 数据行 | `rows` | `datarows` |
| 分页游标 | `cursor` | `cursor` ✅ (相同) |
| 总行数 | *(不存在)* | `total` |
| 本页行数 | *(不存在)* | `size` |
| HTTP 状态 | *(不存在)* | `status` |

### 2.3 需要修改的 BE 代码

| # | 文件 | 行号 | 当前（ES） | 需改为（OS） | 严重程度 |
|---|---|---|---|---|---|
| 1 | `es_sql_reader.cpp` | 75, 152 | `http://{}:{}/_sql` | `http://{}:{}/_plugins/_sql` | **Critical** |
| 2 | `es_sql_reader.cpp` | 213 | `http://{}:{}/_sql/close` | `http://{}:{}/_plugins/_sql/close` | **Critical** |
| 3 | `es_sql_reader.cpp` | 102 | `resp["columns"]` | `resp["schema"]` | **Critical** |
| 4 | `es_sql_parser.cpp` | 47, 51 | `doc["rows"]` | `doc["datarows"]` | **Critical** |

**当前代码：**

```cpp
// es_sql_reader.cpp:75 — open()
std::string url = fmt::format("http://{}:{}/_sql", host.hostname, host.port);

// es_sql_reader.cpp:152 — get_next() cursor fetch
std::string url = fmt::format("http://{}:{}/_sql", host.hostname, host.port);

// es_sql_reader.cpp:213 — close()
std::string url = fmt::format("http://{}:{}/_sql/close", host.hostname, host.port);

// es_sql_reader.cpp:102 — 列元数据解析
if (resp.HasMember("columns")) {
    const auto& cols = resp["columns"].GetArray();
    for (const auto& col : cols) {
        _columns.push_back({col["name"].GetString(), col["type"].GetString()});
    }
}

// es_sql_parser.cpp:47 — 数据行解析
if (!doc.HasMember("rows")) {
    return Status::OK();
}
const auto& rows = doc["rows"].GetArray();
```

### 2.4 额外问题 — 硬编码 `http://` scheme（BE 潜在 Bug）

`es_sql_reader.cpp` 第 75/152/213 行硬编码 `http://`，即使 `es.net.ssl=true` 也不会使用 HTTPS。

对比：scroll 路径的 `es_scan_reader.cpp` 通过 `_target` 参数（包含 scheme）规避了这个问题。

OpenSearch 集群常用 HTTPS，这是必须修复的。

```cpp
// 当前（有 Bug）：
std::string url = fmt::format("http://{}:{}/_sql", host.hostname, host.port);

// 应改为：
std::string scheme = (_properties.count("es.net.ssl") && _properties.at("es.net.ssl") == "true")
                         ? "https" : "http";
std::string url = fmt::format("{}://{}:{}/_sql", scheme, host.hostname, host.port);
```

### 2.5 兼容的部分（无需改动）

| 方面 | 说明 |
|------|------|
| **请求体格式** | `{"query":"...", "fetch_size":N, "cursor":"..."}` — ES 和 OS 完全一致 |
| **游标分页协议** | 初始请求 → cursor → cursor fetch → close — 完全一致 |
| **Basic auth** | `user`/`password` 属性 → `set_basic_auth()` — 完全一致 |
| **错误响应** | ES: `{"error":{"type":...,"reason":...}}`；OS 额外有 `details` 字段，但 `type`/`reason` 位置一致 |
| **`unsigned_long` 字符串** | OS 没有 `unsigned_long` 类型，但现有代码的 fallback 分支（`IsUint64`/`IsInt64`/`IsDouble`）已覆盖 OS 的数字返回 |
| **Boolean 解析** | 两端均返回 JSON `true`/`false`，`val.GetBool()` 兼容 |
| **Datetime ISO 8601 解析** | 两端均返回 ISO 8601 字符串，`normalize_iso8601_datetime` 兼容 |

### 2.6 `time_zone` 请求体字段差异

- **ES SQL**：支持 `time_zone` 作为请求体字段（ISO-8601 时区 ID）
- **OpenSearch SQL**：文档**未列出** `time_zone` 请求体字段，通过 cluster setting `plugins.sql.query.time_zone` 配置
- **当前代码**：`es_sql_reader.cpp:60-64` 发送 `time_zone`，OS 会忽略此字段（不报错但不生效）

---

## 3. 第二层：FE 侧 — ES Catalog 连接修复

### 3.1 问题 1：`EsMajorVersion.parse()` 不识别 OpenSearch

`EsRestClient.version()` 调用 `GET /`，只读 `version.number`，**完全忽略 `version.distribution` 字段**：

```java
// EsRestClient.java:145-152
public EsMajorVersion version() throws StarRocksConnectorException {
    Map<String, Object> result = get("/", null);          // GET /
    Map<String, String> versionBody = (Map<String, String>) result.get("version");
    return EsMajorVersion.parse(versionBody.get("number"));  // 只读 number
}
```

**OpenSearch 的 `GET /` 响应：**

```json
{
  "version": {
    "distribution": "opensearch",
    "number": "2.11.1",
    "build_type": "tar",
    "build_hash": "..."
  },
  "tagline": "The OpenSearch Project: https://opensearch.org/"
}
```

`EsMajorVersion.parse("2.11.1")` 返回 `V_2_X`，导致：

- `EsNodeInfo` 中 `version.before(V_5_X)` 为 true → 走 pre-5.x 废弃分支读 `attributes.data` 而非 `roles` 数组
- `EsTable.validate()` 中 `majorVersion.before(V_5_X)` 会**直接拒绝**显式设置 version 的 OpenSearch 集群

### 3.2 问题 2：`EsTable.validate()` 的版本门槛

```java
// EsTable.java:233
if (majorVersion.before(EsMajorVersion.V_5_X)) {
    throw new StarRocksConnectorException("Unsupported/Unknown ES Cluster version...");
}
```

OpenSearch 2.x 被解析为 `V_2_X`，`before(V_5_X)` 为 true → 被拒绝。

### 3.3 问题 3：`EsNodeInfo` 的版本依赖分支

```java
// EsNodeInfo.java:65
EsMajorVersion version = EsMajorVersion.parse((String) map.get("version"));
if (version.before(EsMajorVersion.V_5_X)) {
    // 废弃路径：读 attributes.data
} else {
    // 正确路径：读 roles 数组
    List<String> roles = (List<String>) map.get("roles");
    this.isClient = roles.contains("data") == false;
    this.isData = roles.contains("data");
    this.isIngest = roles.contains("ingest");
}
```

OpenSearch 节点返回 `roles: ["data","ingest","cluster_manager"]`，应走 else 分支，但因版本误判走了 if 分支。

### 3.4 FE 侧修复方案

| 优先级 | 文件 | 改动 |
|---|---|---|
| P0 | `EsRestClient.java:145-152` | 读取 `version.distribution`，若为 `"opensearch"` 则识别为 OpenSearch |
| P0 | `EsMajorVersion.java` | OS 版本映射：1.x/2.x/3.x → `V_7_X` 行为（OS 1.x ≈ ES 7.10.2） |
| P1 | `EsTable.java:233` | 修复 `before(V_5_X)` 拒绝逻辑，不拒绝 OpenSearch |

**推荐检测模式（来自 OpenSearch Data Prepper）：**

```java
private void validateDistribution(final String distribution) {
    if (!distribution.equals("opensearch")
            && !distribution.startsWith("elasticsearch")) {
        throw new IllegalArgumentException("Unsupported distribution: " + distribution);
    }
}
```

### 3.5 其他 ES Catalog 端点兼容性

ES Catalog 使用的所有 REST 端点在 OpenSearch 中均存在，且响应格式兼容：

| 端点 | 用途 | OS 兼容性 |
|------|------|----------|
| `GET /` | 集群信息 + 版本 | ✅ 格式一致，额外有 `distribution` 字段 |
| `GET _nodes/http` | 发现数据节点 | ✅ 格式一致 |
| `GET {index}/_mapping` | 索引 schema | ✅ 格式一致（OS 2.x 移除了 mapping types） |
| `GET {index}/_search_shards` | 分片路由 | ✅ 格式一致 |
| `GET _cat/indices` | 列出索引 | ✅ 格式一致 |
| `GET _aliases` | 列出别名 | ✅ 格式一致 |

**注意**：OpenSearch 2.x 移除了 mapping types，`{index}/{type}/_search` 形式不再支持。只要用户不设置 type，`{index}/_search` 形式可在 OS 2.x+ 上工作。

---

## 4. 第三层：FE 侧 — native_query 管道

当前 FE 完全没有 native_query 的管道。要让 `native_query` 属性从 FE 传递到 BE，需要以下工作：

### 4.1 BE 侧消费链（已就绪）

```cpp
// es_connector.cpp:198 — 已实现
auto it_native = _properties.find("native_query");
if (it_native != _properties.end()) {
    // native query 模式：使用 ESSqlReader
    const TEsScanRange& es_scan_range = _scan_range;
    int sql_fetch_size = std::min(config::es_index_max_result_window,
                                  _runtime_state->chunk_size());
    _es_sql_reader = std::make_unique<ESSqlReader>(
        es_scan_range.es_hosts, _properties, it_native->second,  // value = SQL 字符串
        sql_fetch_size, _runtime_state);
    return _es_sql_reader->open();
}
```

`_properties` 来自 Thrift `TEsScanNode.properties` (field 2, `map<string,string>`)。BE 已经能正确消费。

### 4.2 FE 侧需要搭建的管道

| 组件 | 文件 | 需要做的事 |
|---|---|---|
| 属性定义 | `EsTable.java` | 添加 `KEY_NATIVE_QUERY` 常量，`validate()` 中接受并存储该属性 |
| 属性传递 | `EsScanNode.toThrift()` | 将 `native_query` SQL 字符串放入 `TEsScanNode.properties` map |
| Scan Range | `EsScanNode` | 添加 `computeQueryTableScanRanges()` — native_query 是集群级查询（非分片），创建单一 scan range |
| 属性跳过 | `EsScanNode.toThrift()` | native_query 模式下跳过 docvalue_context / fields_context（schema 来自 SQL 响应） |

### 4.3 FE 侧 native_query 流程设计

```
用户创建 ES Catalog 表时指定 native_query 属性
    │
    ▼
EsTable.validate()  ← 接受并存储 native_query 属性
    │
    ▼
EsScanNode.toThrift()  ← 将 native_query 放入 TEsScanNode.properties
    │                     跳过 docvalue_context / fields_context
    ▼
EsScanNode.computeQueryTableScanRanges()  ← 集群级单 scan range
    │                                        (非分片，使用 ES 集群 hosts)
    ▼
TEsScanNode.properties  →  BE ESDataSource._properties
    │
    ▼
es_connector.cpp:_create_scanner()  ← 检测 native_query 属性
    │
    ▼
ESSqlReader.open()  ← POST /_plugins/_sql (OS) 或 /_sql (ES)
    │
    ▼
EsSqlResponseParser::parse()  ← 解析 schema/datarows (OS) 或 columns/rows (ES)
```

---

## 5. 统一 vs 独立 Connector 的架构选择

### 5.1 行业实践

| 方案 | 代表项目 | 原因 | StarRocks 适用度 |
|---|---|---|---|
| **统一 connector + distribution 检测** | Zipkin, Graylog | 使用原始 HTTP 客户端（无 ES Java SDK 依赖） | ✅ **推荐** |
| **独立 connector** | Trino, Spark | 使用 ES Java SDK（Elastic 在 SDK 中加了版本检查拒绝 OS） | ❌ 不需要 |

**Trino 拆分原因**（[PR #20257](https://github.com/trinodb/trino/pull/20257)）：

> "Elastic made its latest libraries not work with OpenSearch."

ES Java 客户端添加了显式版本检查，主动拒绝 OpenSearch。Trino 被迫拆分为 `trino-elasticsearch` 和 `trino-opensearch`。

**StarRocks 不受此影响**：StarRocks 使用原始 OkHttp（FE）/ libcurl（BE），不依赖 ES Java SDK，没有版本检查冲突问题。

### 5.2 推荐方案：统一 connector + 运行时 distribution 分支

```
EsRestClient
    │
    ├── GET / → 检测 version.distribution
    │
    ├── distribution = "opensearch"
    │   ├── 版本映射：1.x/2.x/3.x → V_7_X 行为
    │   ├── BE: endpoint = /_plugins/_sql
    │   ├── BE: 响应字段 = schema / datarows
    │   └── 跳过 mapping type 相关逻辑
    │
    └── distribution = "elasticsearch" (或不存在)
        ├── 版本解析：保持现有逻辑
        ├── BE: endpoint = /_sql
        └── BE: 响应字段 = columns / rows
```

---

## 6. 完整改动清单

### 6.1 BE 侧（4 处关键 + 1 Bug 修复）

| 优先级 | 文件 | 行号 | 改动 |
|---|---|---|---|
| P0 | `es_sql_reader.cpp` | 75, 152 | endpoint `/_sql` → `/_plugins/_sql`（OS 模式下） |
| P0 | `es_sql_reader.cpp` | 213 | endpoint `/_sql/close` → `/_plugins/_sql/close`（OS 模式下） |
| P0 | `es_sql_reader.cpp` | 102 | `resp["columns"]` → `resp["schema"]`（OS 模式下，或同时检查两者） |
| P0 | `es_sql_parser.cpp` | 47, 51 | `doc["rows"]` → `doc["datarows"]`（OS 模式下，或同时检查两者） |
| P1 | `es_sql_reader.cpp` | 75, 152, 213 | 硬编码 `http://` → 根据 `es.net.ssl` 选择 `http://`/`https://` |

### 6.2 FE 侧 — Catalog 连接（3 处修复）

| 优先级 | 文件 | 改动 |
|---|---|---|
| P0 | `EsRestClient.java:145-152` | 读取 `version.distribution`，识别 OpenSearch |
| P0 | `EsMajorVersion.java` | OS 版本映射：1.x/2.x → V_7_X 行为 |
| P1 | `EsTable.java:233` | 修复 `before(V_5_X)` 拒绝逻辑，不拒绝 OpenSearch |

### 6.3 FE 侧 — native_query 管道（4 处新增）

| 优先级 | 文件 | 改动 |
|---|---|---|
| P0 | `EsTable.java` | 添加 `KEY_NATIVE_QUERY`，`validate()` 接受属性 |
| P0 | `EsScanNode.toThrift()` | 将 `native_query` 放入 `TEsScanNode.properties` |
| P0 | `EsScanNode` | 添加 `computeQueryTableScanRanges()`（集群级单 scan range） |
| P1 | `EsScanNode.toThrift()` | native_query 模式跳过 docvalue/fields context |

### 6.4 可选增强

| 优先级 | 改动 | 原因 |
|---|---|---|
| P2 | `EsRestClient` 添加 AWS SigV4 签名 | Amazon OpenSearch Service 默认 IAM 认证 |
| P2 | `es_sql_reader.cpp` 添加 API Key 认证 | OS 支持 `Authorization: ApiKey os_<token>` |
| P3 | `EsConfig` 添加 `distribution` 属性 | 允许用户显式指定 ES/OS 模式，跳过自动检测 |

---

## 7. 附录：ES vs OpenSearch SQL API 逐项对比

### 7.1 REST 端点

| 功能 | Elasticsearch | OpenSearch |
|------|--------------|------------|
| 查询 | `POST /_sql` | `POST /_plugins/_sql` |
| 游标分页 | `POST /_sql` (body: `{"cursor":"..."}`) | `POST /_plugins/_sql` (body: `{"cursor":"..."}`) |
| 关闭游标 | `POST /_sql/close` | `POST /_plugins/_sql/close` |
| Explain | `POST /_sql/_explain` | `POST /_plugins/_sql/_explain` |

### 7.2 请求体

| 字段 | ES SQL | OpenSearch SQL |
|------|--------|----------------|
| `query` (string, required) | ✅ | ✅ |
| `fetch_size` (integer) | ✅ | ✅ |
| `time_zone` (string) | ✅ | ❌ 不支持（通过 cluster setting 配置） |
| `filter` (JSON object) | ✅ | ✅ |
| `cursor` (string) | ✅ | ✅ |
| `format` (URL param) | `json`, `csv`, `tsv`, `txt`, `yaml`, `cbor`, `smile` | `jdbc`, `csv`, `raw`, `json` |

**重要**：OpenSearch 的 `format=json` 返回原始搜索响应（含 `hits`/`_source`），**不是**表格格式。表格格式是 `format=jdbc`（默认）。StarRocks 依赖表格格式，OpenSearch 默认 `jdbc` 格式正好兼容。

### 7.3 响应字段

| 用途 | ES SQL | OpenSearch SQL (jdbc) |
|------|--------|----------------------|
| 列元数据 | `columns` (数组，每项含 `name`/`type`) | `schema` (数组，每项含 `name`/`type`) |
| 数据行 | `rows` (数组的数组) | `datarows` (数组的数组) |
| 分页游标 | `cursor` (string) | `cursor` (string) ✅ |
| 总行数 | *(不存在)* | `total` |
| 本页行数 | *(不存在)* | `size` |
| HTTP 状态 | *(不存在)* | `status` |

### 7.4 列类型名称

| 概念 | ES SQL type | OpenSearch SQL type |
|------|------------|-------------------|
| Boolean | `boolean` | `boolean` ✅ |
| Byte | `byte` | `byte` ✅ |
| Short | `short` | `short` ✅ |
| Integer | `integer` | `integer` ✅ |
| Long | `long` | `long` ✅ |
| Float | `float` | `float` ✅ |
| Double | `double` | `double` ✅ |
| Keyword | `keyword` | `keyword` ✅ |
| Text | `text` | `text` ✅ |
| 日期时间 | `datetime` | **`timestamp`** ⚠️ |
| 仅日期 | `date` | `date` ✅ |
| 仅时间 | `time` | `time` ✅ |
| 无符号 64 位 | **`unsigned_long`** | **不存在** ⚠️ |
| IP | `ip` | `ip` ✅ |
| Binary | `binary` | `binary` ✅ |

**注意**：`es_sql_parser.cpp` 的类型分发基于 StarRocks tuple descriptor 的 `LogicalType`，**不依赖** ES/OS 报告的 `type` 字符串。因此类型名称差异不影响值解析。

### 7.5 `unsigned_long` 处理

**ES SQL**：`unsigned_long` 在值超过 2^53 时作为 JSON **字符串**返回（保持精度）。

StarRocks 代码已处理（`es_sql_parser.cpp:114-138`）：

```cpp
case TYPE_LARGEINT: {
    int128_t value = 0;
    if (val.IsString()) {
        // ES SQL returns unsigned_long as a JSON string...
        StringParser::ParseResult result;
        value = StringParser::string_to_int<int128_t>(...);
    } else if (val.IsUint64()) {
        value = static_cast<int128_t>(val.GetUint64());
    } else if (val.IsInt64()) {
        value = static_cast<int128_t>(val.GetInt64());
    } else if (val.IsDouble()) {
        value = static_cast<int128_t>(val.GetDouble());
    }
    // ...
}
```

**OpenSearch SQL**：没有 `unsigned_long` 类型，最大整数类型是 `long`（有符号 64 位），作为 JSON 数字返回。

现有代码的 fallback 分支（`IsUint64`/`IsInt64`/`IsDouble`）已覆盖 OpenSearch 的数字返回，**前向兼容**。

### 7.6 认证

| 机制 | Elasticsearch | OpenSearch | StarRocks 当前 |
|------|--------------|------------|--------------|
| HTTP Basic Auth | ✅ | ✅ | ✅ (仅此) |
| API Key | ✅ (`ApiKey <token>`) | ✅ (`ApiKey os_<token>`) | ❌ |
| AWS SigV4 / IAM | (via proxy) | ✅ 原生 (Amazon OS Service 默认) | ❌ |
| SAML / OIDC | ✅ | ✅ | ❌ |
| Client TLS Certificate | ✅ | ✅ | ❌ |

### 7.7 SQL 插件安装

- **Elasticsearch**：SQL 功能内置（7.x+）
- **OpenSearch**：SQL 插件 (`opensearch-sql`) 是**捆绑插件** — 包含在所有 OpenSearch 发行版中（最小发行版除外），自 1.0.0 起可用，默认启用

### 7.8 版本兼容性

| OpenSearch 版本 | 对应 ES 版本 | REST 兼容性 | `override_main_response_version` |
|----------------|------------|-----------|--------------------------------|
| 1.x | ES 7.10.2 | 完全兼容 | ✅ 可用 |
| 2.x | (有 breaking changes) | 部分兼容（移除 mapping types 等） | ⚠️ 部分支持 |
| 3.x | (完全不兼容) | 不兼容 | ❌ 已移除 |

---

## 参考资料

### OpenSearch 官方文档
- [OpenSearch SQL API](https://docs.opensearch.org/latest/sql-and-ppl/sql-and-ppl-api/index/)
- [OpenSearch SQL endpoint](https://github.com/opensearch-project/sql/blob/main/docs/user/interfaces/endpoint.rst)
- [OpenSearch SQL pagination](https://github.com/opensearch-project/sql/blob/main/docs/dev/Pagination-v2.md)
- [OpenSearch SQL data types](https://github.com/opensearch-project/sql/blob/main/docs/user/general/datatypes.rst)
- [OpenSearch plugins](https://docs.opensearch.org/latest/install-and-configure/plugins/)
- [OpenSearch security configuration](https://docs.opensearch.org/latest/security/configuration/configuration/)

### OpenSearch 版本检测
- [Issue #693: version.distribution field](https://github.com/opensearch-project/OpenSearch/issues/693)
- [PR #847: override_main_response_version](https://github.com/opensearch-project/OpenSearch/pull/847)
- [Issue #3023: 2.0 breaking changes](https://github.com/opensearch-project/OpenSearch/issues/3023)
- [Data Prepper detection pattern](https://github.com/opensearch-project/data-prepper/blob/main/data-prepper-plugins/opensearch/src/main/java/org/opensearch/dataprepper/plugins/source/opensearch/worker/client/SearchAccessorStrategy.java)
- [Zipkin OpensearchVersionTest](https://github.com/openzipkin/zipkin/blob/master/zipkin-storage/elasticsearch/src/test/java/zipkin2/elasticsearch/OpensearchVersionTest.java)
- [Graylog VersionProbeImplTest](https://github.com/Graylog2/graylog2-server/blob/master/graylog2-server/src/test/java/org/graylog2/storage/versionprobe/VersionProbeImplTest.java)

### 行业实践
- [Trino PR #20257: Add trino-opensearch plugin](https://github.com/trinodb/trino/pull/20257)
- [Trino issue #20258: Upgrade Elasticsearch client](https://github.com/trinodb/trino/issues/20258)
- [opensearch-hadoop COMPATIBILITY.md](https://github.com/opensearch-project/opensearch-hadoop/blob/main/COMPATIBILITY.md)

### Elasticsearch SQL
- [ES SQL REST format](https://www.elastic.co/docs/reference/query-languages/sql/sql-rest-format)
- [ES SQL API](https://www.elastic.co/docs/api/doc/elasticsearch/operation/operation-sql-query)
