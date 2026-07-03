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

#include "connector/elasticsearch/es_sql_parser.h"

#include <string>

#include "column/binary_column.h"
#include "column/chunk.h"
#include "column/column.h"
#include "column/nullable_column.h"
#include "common/compiler_util.h"
#include "common/status.h"
#include "types/logical_type.h"
#include "types/timestamp_value.h"
#include "rapidjson/document.h"

namespace starrocks {

Status EsSqlResponseParser::parse(const std::string& response, const std::vector<EsSqlColumn>& columns,
                                   Chunk* chunk) {
    rapidjson::Document doc;
    doc.Parse(response.c_str());
    if (doc.HasParseError()) {
        return Status::InternalError("Failed to parse ES SQL response JSON");
    }

    // Check for error
    if (doc.HasMember("error")) {
        return Status::InternalError("ES SQL response contains error");
    }

    if (!doc.HasMember("rows")) {
        return Status::OK(); // empty response
    }

    const auto& rows = doc["rows"].GetArray();
    size_t num_columns = chunk->num_columns();

    for (const auto& row : rows) {
        const auto& row_array = row.GetArray();
        for (size_t col_idx = 0; col_idx < columns.size() && col_idx < num_columns; col_idx++) {
            const auto& val = row_array[col_idx];
            const auto& es_type = columns[col_idx].type;
            RETURN_IF_ERROR(append_value(es_type, val, chunk->get_column_raw_ptr_by_index(col_idx), col_idx));
        }
    }

    return Status::OK();
}

Status EsSqlResponseParser::append_value(const std::string& es_type, const rapidjson::Value& val, Column* col,
                                          size_t col_idx) {
    if (val.IsNull()) {
        col->append_default();
        return Status::OK();
    }

    const std::string& type = es_type;
    // Handle NullableColumn wrapper
    Column* data_col = col;
    if (col->is_nullable()) {
        auto* nullable = down_cast<NullableColumn*>(col);
        nullable->null_column_data().push_back(0); // not null
        data_col = nullable->data_column_raw_ptr();
    }

    if (type == "long" || type == "integer" || type == "short" || type == "byte") {
        down_cast<Int64Column*>(data_col)->append(val.GetInt64());
    } else if (type == "unsigned_long") {
        down_cast<Int128Column*>(data_col)->append(static_cast<int128_t>(val.GetUint64()));
    } else if (type == "double" || type == "float" || type == "scaled_float" || type == "half_float") {
        down_cast<DoubleColumn*>(data_col)->append(val.GetDouble());
    } else if (type == "boolean") {
        down_cast<BooleanColumn*>(data_col)->append(val.GetBool());
    } else if (type == "datetime" || type == "date") {
        std::string str = val.GetString();
        std::string normalized = normalize_iso8601_datetime(str);
        // Parse to timestamp and append
        // Simple parsing: "2024-01-01 12:00:00.123"
        int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0, usec = 0;
        if (sscanf(normalized.c_str(), "%d-%d-%d %d:%d:%d.%d", &year, &month, &day, &hour, &min, &sec, &usec) >= 6) {
            down_cast<TimestampColumn*>(data_col)->append(
                    TimestampValue::create(year, month, day, hour, min, sec, usec));
        } else if (sscanf(normalized.c_str(), "%d-%d-%d", &year, &month, &day) >= 3) {
            // Date-only value
            down_cast<TimestampColumn*>(data_col)->append(TimestampValue::create(year, month, day, 0, 0, 0, 0));
        } else {
            col->append_default();
        }
    } else {
        // Default: treat as string (text, keyword, ip, version, etc.)
        std::string str = val.GetString();
        down_cast<BinaryColumn*>(data_col)->append(str);
    }

    return Status::OK();
}

std::string EsSqlResponseParser::normalize_iso8601_datetime(const std::string& iso8601) {
    std::string result = iso8601;
    // Replace 'T' with space
    size_t t_pos = result.find('T');
    if (t_pos != std::string::npos) {
        result[t_pos] = ' ';
    }
    // Truncate timezone suffix (Z or +hh:mm or -hh:mm)
    // If time_zone was injected in the request, ES returns local time without Z
    size_t tz_pos = result.find_first_of("Z+-", 19);
    if (tz_pos != std::string::npos) {
        result = result.substr(0, tz_pos);
    }
    // Trim trailing whitespace
    result.erase(result.find_last_not_of(" \t") + 1);
    return result;
}

} // namespace starrocks
