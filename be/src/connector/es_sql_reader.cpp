// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "connector/es_sql_reader.h"

#include <map>
#include <string>

#include "common/config.h"
#include "common/logging.h"
#include "common/status.h"
#include "fmt/compile.h"
#include "runtime/runtime_state.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace starrocks {

ESSqlReader::ESSqlReader(const std::vector<TNetworkAddress>& es_hosts,
                         const std::map<std::string, std::string>& properties,
                         const std::string& sql_query,
                         int batch_size,
                         RuntimeState* state)
        : _es_hosts(es_hosts),
          _properties(properties),
          _sql_query(sql_query),
          _batch_size(batch_size),
          _state(state) {}

ESSqlReader::~ESSqlReader() {
    if (!_cursor.empty()) {
        auto st = close();
        if (!st.ok()) {
            LOG(WARNING) << "Failed to close ES SQL cursor in destructor: " << st.message();
        }
    }
}

Status ESSqlReader::open() {
    // Build request body: {"query": "<sql>", "fetch_size": <batch_size>, "time_zone": "<tz>"}
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    rapidjson::Value sql_value(_sql_query.c_str(), alloc);
    doc.AddMember("query", sql_value, alloc);
    doc.AddMember("fetch_size", _batch_size, alloc);

    // Inject time_zone if present
    auto it_tz = _properties.find("time_zone");
    if (it_tz != _properties.end() && !it_tz->second.empty()) {
        rapidjson::Value tz_value(it_tz->second.c_str(), alloc);
        doc.AddMember("time_zone", tz_value, alloc);
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::string response;
    // Try each host with failover
    for (int i = 0; i < _es_hosts.size(); i++) {
        int idx = (_current_host_index + i) % _es_hosts.size();
        const auto& host = _es_hosts[idx];
        std::string url = fmt::format("http://{}:{}/_sql", host.hostname, host.port);
        Status st = _http_post(url, buffer.GetString(), &response);
        if (st.ok()) {
            _current_host_index = idx;
            break;
        }
        if (i == _es_hosts.size() - 1) {
            return st;
        }
    }

    // Parse response
    rapidjson::Document resp;
    resp.Parse(response.c_str());
    if (resp.HasParseError()) {
        return Status::InternalError("Failed to parse ES SQL response");
    }

    // Check for error
    if (resp.HasMember("error")) {
        std::string err_type = resp["error"].HasMember("type") ? resp["error"]["type"].GetString() : "unknown";
        std::string err_reason =
                resp["error"].HasMember("reason") ? resp["error"]["reason"].GetString() : "unknown";
        return Status::InternalError(fmt::format("ES SQL error: {} - {}", err_type, err_reason));
    }

    // Parse columns (only present in first response)
    if (resp.HasMember("columns")) {
        const auto& cols = resp["columns"].GetArray();
        for (const auto& col : cols) {
            _columns.push_back({col["name"].GetString(), col["type"].GetString()});
        }
    }

    // Save cursor
    if (resp.HasMember("cursor")) {
        _cursor = resp["cursor"].GetString();
    } else {
        _eos = true;
    }

    _first_request_done = true;
    _cached_response = response;
    _has_cached_response = true;
    return Status::OK();
}

Status ESSqlReader::get_next(std::string* response, bool* eos) {
    *eos = false;

    if (_has_cached_response) {
        *response = _cached_response;
        _cached_response.clear();
        _has_cached_response = false;
        return Status::OK();
    }

    if (_eos) {
        *eos = true;
        return Status::OK();
    }

    // Cursor fetch: POST /_sql {"cursor": "..."}
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    rapidjson::Value cursor_value(_cursor.c_str(), alloc);
    doc.AddMember("cursor", cursor_value, alloc);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::string resp_str;
    for (int i = 0; i < _es_hosts.size(); i++) {
        int idx = (_current_host_index + i) % _es_hosts.size();
        const auto& host = _es_hosts[idx];
        std::string url = fmt::format("http://{}:{}/_sql", host.hostname, host.port);
        Status st = _http_post(url, buffer.GetString(), &resp_str);
        if (st.ok()) {
            _current_host_index = idx;
            break;
        }
        if (i == _es_hosts.size() - 1) {
            return st;
        }
    }

    // Parse response — MUST check for error (cursor invalidation)
    rapidjson::Document resp;
    resp.Parse(resp_str.c_str());
    if (resp.HasParseError()) {
        return Status::InternalError("Failed to parse ES SQL cursor response");
    }

    if (resp.HasMember("error")) {
        // Cursor invalidated — do NOT silently truncate
        _cursor.clear();
        _eos = true;
        std::string err_type = resp["error"].HasMember("type") ? resp["error"]["type"].GetString() : "unknown";
        std::string err_reason =
                resp["error"].HasMember("reason") ? resp["error"]["reason"].GetString() : "unknown";
        return Status::InternalError(fmt::format("ES SQL cursor invalidated: {} - {}", err_type, err_reason));
    }

    *response = resp_str;

    if (resp.HasMember("cursor")) {
        _cursor = resp["cursor"].GetString();
    } else {
        _eos = true;
        _cursor.clear();
    }

    return Status::OK();
}

Status ESSqlReader::close() {
    if (_cursor.empty()) {
        return Status::OK();
    }

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();
    rapidjson::Value cursor_value(_cursor.c_str(), alloc);
    doc.AddMember("cursor", cursor_value, alloc);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    std::string response;
    _cursor.clear(); // clear before request to avoid recursive close in destructor

    for (int i = 0; i < _es_hosts.size(); i++) {
        int idx = (_current_host_index + i) % _es_hosts.size();
        const auto& host = _es_hosts[idx];
        std::string url = fmt::format("http://{}:{}/_sql/close", host.hostname, host.port);
        Status st = _http_post(url, buffer.GetString(), &response);
        if (st.ok()) {
            return Status::OK();
        }
    }
    return Status::OK(); // best-effort
}

Status ESSqlReader::_http_post(const std::string& url, const std::string& body, std::string* response) {
    HttpClient client;
    RETURN_IF_ERROR(client.init(url));
    // Auth
    auto it_user = _properties.find("user");
    auto it_pass = _properties.find("password");
    if (it_user != _properties.end() && !it_user->second.empty()) {
        client.set_basic_auth(it_user->second, it_pass->second);
    }
    // SSL
    auto it_ssl = _properties.find("es.net.ssl");
    if (it_ssl != _properties.end() && it_ssl->second == "true") {
        client.trust_all_ssl();
    }
    client.set_content_type("application/json");
    client.set_timeout_ms(config::es_http_timeout_ms);

    RETURN_IF_ERROR(client.execute_post_request(body, response));
    if (client.get_http_status() != 200) {
        return Status::InternalError(fmt::format("ES SQL request failed: HTTP {}", client.get_http_status()));
    }
    return Status::OK();
}

} // namespace starrocks
