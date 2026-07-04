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

#include <string>
#include <vector>

#include "column/vectorized_fwd.h"
#include "common/status.h"
#include "connector/elasticsearch/es_sql_reader.h"
#include "rapidjson/document.h"
#include "runtime/descriptors.h"

namespace starrocks {

class Chunk;

class EsSqlResponseParser {
public:
    static Status parse(const std::string& response, const std::vector<EsSqlColumn>& columns,
                        const TupleDescriptor* tuple_desc, Chunk* chunk);

private:
    static Status append_value(const rapidjson::Value& val, Column* col, LogicalType lt);
    static std::string normalize_iso8601_datetime(const std::string& iso8601);
};

} // namespace starrocks
