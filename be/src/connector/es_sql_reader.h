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

#pragma once

#include <map>
#include <string>
#include <vector>

#include "common/status.h"
#include "gen_cpp/Types_types.h" // TNetworkAddress
#include "http/http_client.h"
#include "rapidjson/document.h"

namespace starrocks {

class RuntimeState;

// ES SQL response column metadata
struct EsSqlColumn {
    std::string name;
    std::string type;
};

// Reader for ES/OpenSearch SQL endpoint
// Unlike ESScanReader (which uses scroll API with per-shard _search),
// ESSqlReader uses the cluster-level SQL endpoint with cursor pagination.
// Supports both Elasticsearch (/_sql) and OpenSearch (/_plugins/_sql).
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

    // Get next batch of rows as raw JSON response
    // Returns the response string for the caller (EsSqlResponseParser) to parse
    Status get_next(std::string* response, bool* eos);

    // Close: release cursor if exists
    Status close();

    // Get the columns from the first response
    const std::vector<EsSqlColumn>& columns() const { return _columns; }

private:
    Status _http_post(const std::string& url, const std::string& body, std::string* response);
    std::string _build_url(const TNetworkAddress& host, const std::string& path) const;

    std::vector<TNetworkAddress> _es_hosts;
    std::map<std::string, std::string> _properties;
    std::string _sql_query;
    int _batch_size;
    [[maybe_unused]] RuntimeState* _state;

    std::string _sql_base_path; // "/_sql" (ES) or "/_plugins/_sql" (OpenSearch)
    std::string _url_scheme;    // "http" or "https"

    std::string _cursor;
    std::vector<EsSqlColumn> _columns;
    bool _first_request_done = false;
    bool _eos = false;
    int _current_host_index = 0;
    std::string _cached_response; // first response cached for open()
    bool _has_cached_response = false;
};

} // namespace starrocks
