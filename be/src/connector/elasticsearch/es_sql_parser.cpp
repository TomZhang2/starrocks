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
#include "column/column_helper.h"
#include "column/nullable_column.h"
#include "common/compiler_util.h"
#include "common/status.h"
#include "types/logical_type.h"
#include "types/timestamp_value.h"
#include "rapidjson/document.h"

namespace starrocks {

// Parse ES SQL _sql response into a StarRocks Chunk.
//
// Key difference from ScrollParser:
// - _search response: hits.hits[]._source (document-oriented, field-by-field)
// - _sql response:    rows: [[v1, v2, ...], ...] (columnar, positional)
//
// We dispatch on the chunk column's actual LogicalType (set by FE type mapping),
// NOT on the ES SQL type string. This avoids type-mismatch crashes.
Status EsSqlResponseParser::parse(const std::string& response, const std::vector<EsSqlColumn>& columns,
                                   Chunk* chunk) {
    rapidjson::Document doc;
    doc.Parse(response.c_str());
    if (doc.HasParseError()) {
        return Status::InternalError("Failed to parse ES SQL response JSON");
    }

    if (doc.HasMember("error")) {
        return Status::InternalError("ES SQL response contains error");
    }

    if (!doc.HasMember("rows")) {
        return Status::OK();
    }

    const auto& rows = doc["rows"].GetArray();
    size_t num_columns = chunk->num_columns();

    for (const auto& row : rows) {
        const auto& row_array = row.GetArray();
        for (size_t col_idx = 0; col_idx < columns.size() && col_idx < num_columns; col_idx++) {
            const auto& val = row_array[col_idx];
            Column* col = chunk->get_column_raw_ptr_by_index(col_idx);
            RETURN_IF_ERROR(append_value(val, col));
        }
    }

    return Status::OK();
}

// Append a single JSON value to a StarRocks column.
// Dispatches on the column's LogicalType (from TypeDescriptor), not the ES SQL type,
// so the down_cast always matches the actual runtime column type.
Status EsSqlResponseParser::append_value(const rapidjson::Value& val, Column* col) {
    if (val.IsNull()) {
        col->append_default();
        return Status::OK();
    }

    // Unwrap NullableColumn to get the underlying data column + type
    Column* data_col = col;
    if (col->is_nullable()) {
        auto* nullable = down_cast<NullableColumn*>(col);
        nullable->null_column_data().push_back(0);
        data_col = nullable->data_column_raw_ptr();
    }

    LogicalType lt = data_col->type_info()->type();

    switch (lt) {
    case TYPE_BOOLEAN: {
        down_cast<BooleanColumn*>(data_col)->append(val.GetBool());
        break;
    }
    case TYPE_TINYINT: {
        down_cast<Int8Column*>(data_col)->append(static_cast<int8_t>(val.GetInt64()));
        break;
    }
    case TYPE_SMALLINT: {
        down_cast<Int16Column*>(data_col)->append(static_cast<int16_t>(val.GetInt64()));
        break;
    }
    case TYPE_INT: {
        down_cast<Int32Column*>(data_col)->append(static_cast<int32_t>(val.GetInt64()));
        break;
    }
    case TYPE_BIGINT: {
        down_cast<Int64Column*>(data_col)->append(val.GetInt64());
        break;
    }
    case TYPE_LARGEINT: {
        // ES SQL "unsigned_long" returns uint64; store as int128
        down_cast<Int128Column*>(data_col)->append(static_cast<int128_t>(val.GetUint64()));
        break;
    }
    case TYPE_FLOAT: {
        down_cast<FloatColumn*>(data_col)->append(static_cast<float>(val.GetDouble()));
        break;
    }
    case TYPE_DOUBLE: {
        down_cast<DoubleColumn*>(data_col)->append(val.GetDouble());
        break;
    }
    case TYPE_DATETIME: {
        std::string str = val.GetString();
        std::string normalized = normalize_iso8601_datetime(str);
        int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0, usec = 0;
        if (sscanf(normalized.c_str(), "%d-%d-%d %d:%d:%d.%d", &year, &month, &day, &hour, &min, &sec, &usec) >= 6) {
            down_cast<TimestampColumn*>(data_col)->append(
                    TimestampValue::create(year, month, day, hour, min, sec, usec));
        } else if (sscanf(normalized.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &min, &sec) >= 6) {
            down_cast<TimestampColumn*>(data_col)->append(
                    TimestampValue::create(year, month, day, hour, min, sec, 0));
        } else if (sscanf(normalized.c_str(), "%d-%d-%d", &year, &month, &day) >= 3) {
            down_cast<TimestampColumn*>(data_col)->append(
                    TimestampValue::create(year, month, day, 0, 0, 0, 0));
        } else {
            col->append_default();
        }
        break;
    }
    case TYPE_TIME: {
        // ES SQL "time" type returns string like "12:00:00.000"
        std::string str = val.GetString();
        int hour = 0, min = 0, sec = 0;
        if (sscanf(str.c_str(), "%d:%d:%d", &hour, &min, &sec) >= 3) {
            down_cast<DoubleColumn*>(data_col)->append(
                    static_cast<double>(hour * 3600 + min * 60 + sec));
        } else {
            col->append_default();
        }
        break;
    }
    case TYPE_VARCHAR:
    case TYPE_CHAR: {
        // ES SQL returns text/keyword/ip/version as JSON strings
        if (val.IsString()) {
            Slice slice(val.GetString(), val.GetStringLength());
            down_cast<BinaryColumn*>(data_col)->append(slice);
        } else {
            // Fallback: convert non-string JSON value to string representation
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            val.Accept(writer);
            std::string str(buffer.GetString(), buffer.GetSize());
            down_cast<BinaryColumn*>(data_col)->append(Slice(str));
        }
        break;
    }
    case TYPE_JSON: {
        // ES SQL returns nested/object as JSON strings
        std::string str;
        if (val.IsString()) {
            str = val.GetString();
        } else {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            val.Accept(writer);
            str = std::string(buffer.GetString(), buffer.GetSize());
        }
        down_cast<BinaryColumn*>(data_col)->append(Slice(str));
        break;
    }
    case TYPE_NULL: {
        col->append_default();
        break;
    }
    default: {
        // Unknown type: best-effort as string
        if (val.IsString()) {
            Slice slice(val.GetString(), val.GetStringLength());
            down_cast<BinaryColumn*>(data_col)->append(slice);
        } else {
            col->append_default();
        }
        break;
    }
    }

    return Status::OK();
}

// Normalize ISO8601 datetime string for sscanf parsing.
//   "2024-01-01T12:00:00.123Z"      → "2024-01-01 12:00:00.123"
//   "2024-01-01T12:00:00.123+08:00" → "2024-01-01 12:00:00.123"
//   "2024-01-01T12:00:00"           → "2024-01-01 12:00:00"
std::string EsSqlResponseParser::normalize_iso8601_datetime(const std::string& iso8601) {
    std::string result = iso8601;
    size_t t_pos = result.find('T');
    if (t_pos != std::string::npos) {
        result[t_pos] = ' ';
    }
    // Truncate timezone suffix starting from position 19 (after "YYYY-MM-DD HH:MM:SS")
    // Only look for Z/+/- after the time portion to avoid matching the date's dashes
    if (result.size() > 19) {
        size_t tz_pos = result.find_first_of("Z+-", 19);
        if (tz_pos != std::string::npos) {
            result = result.substr(0, tz_pos);
        }
    }
    result.erase(result.find_last_not_of(" \t") + 1);
    return result;
}

} // namespace starrocks
