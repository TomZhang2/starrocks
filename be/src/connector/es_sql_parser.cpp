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

#include "connector/es_sql_parser.h"

#include <string>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

#include "util/string_parser.hpp"
#include "column/binary_column.h"
#include "column/chunk.h"
#include "column/column.h"
#include "column/nullable_column.h"
#include "common/compiler_util.h"
#include "common/status.h"
#include "types/logical_type.h"
#include "types/timestamp_value.h"

namespace starrocks {

Status EsSqlResponseParser::parse(const std::string& response, const std::vector<EsSqlColumn>& columns,
                                   const TupleDescriptor* tuple_desc, Chunk* chunk) {
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
    const auto& slots = tuple_desc->slots();
    size_t num_columns = chunk->num_columns();

    for (const auto& row : rows) {
        const auto& row_array = row.GetArray();
        const size_t row_size = row_array.Size();
        for (size_t col_idx = 0; col_idx < columns.size() && col_idx < num_columns; col_idx++) {
            Column* col = chunk->get_column_by_index(col_idx).get();
            LogicalType lt = TYPE_UNKNOWN;
            if (col_idx < slots.size()) {
                lt = slots[col_idx]->type().type;
            }
            if (col_idx < row_size) {
                const auto& val = row_array[col_idx];
                RETURN_IF_ERROR(append_value(val, col, lt));
            } else {
                // Row has fewer values than the column count (e.g. ES-side
                // schema drift between probe and execution). Keep the chunk
                // aligned by appending a null for the missing column instead
                // of reading out of bounds.
                col->append_default();
            }
        }
    }

    return Status::OK();
}

Status EsSqlResponseParser::append_value(const rapidjson::Value& val, Column* col, LogicalType lt) {
    if (val.IsNull()) {
        col->append_default();
        return Status::OK();
    }

    Column* data_col = col;
    if (col->is_nullable()) {
        auto* nullable = down_cast<NullableColumn*>(col);
        nullable->null_column_data().push_back(0);
        data_col = nullable->mutable_data_column();
    }

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
        int128_t value = 0;
        if (val.IsString()) {
            // ES SQL returns unsigned_long as a JSON string (not a number) to
            // preserve precision for values exceeding 2^53. Parse the decimal
            // string into int128; GetUint64() would assert/UB on kStringType.
            StringParser::ParseResult result;
            value = StringParser::string_to_int<int128_t>(val.GetString(), val.GetStringLength(), &result);
            if (UNLIKELY(result != StringParser::PARSE_SUCCESS)) {
                data_col->append_default();
                break;
            }
        } else if (val.IsUint64()) {
            value = static_cast<int128_t>(val.GetUint64());
        } else if (val.IsInt64()) {
            value = static_cast<int128_t>(val.GetInt64());
        } else if (val.IsDouble()) {
            value = static_cast<int128_t>(val.GetDouble());
        } else {
            data_col->append_default();
            break;
        }
        down_cast<Int128Column*>(data_col)->append(value);
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
        int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
        if (sscanf(normalized.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &min, &sec) >= 6) {
            // ES SQL DATETIME has millisecond precision; convert the fractional
            // seconds to microseconds (e.g. ".123" -> 123000us, not 123us).
            int usec = fraction_to_microseconds(normalized);
            down_cast<TimestampColumn*>(data_col)->append(
                    TimestampValue::create(year, month, day, hour, min, sec, usec));
        } else if (sscanf(normalized.c_str(), "%d-%d-%d", &year, &month, &day) >= 3) {
            down_cast<TimestampColumn*>(data_col)->append(
                    TimestampValue::create(year, month, day, 0, 0, 0, 0));
        } else {
            data_col->append_default();
        }
        break;
    }
    case TYPE_TIME: {
        std::string str = val.GetString();
        int hour = 0, min = 0, sec = 0;
        if (sscanf(str.c_str(), "%d:%d:%d", &hour, &min, &sec) >= 3) {
            down_cast<DoubleColumn*>(data_col)->append(
                    static_cast<double>(hour * 3600 + min * 60 + sec));
        } else {
            data_col->append_default();
        }
        break;
    }
    case TYPE_VARCHAR:
    case TYPE_CHAR: {
        if (val.IsString()) {
            Slice slice(val.GetString(), val.GetStringLength());
            down_cast<BinaryColumn*>(data_col)->append(slice);
        } else {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            val.Accept(writer);
            std::string str(buffer.GetString(), buffer.GetSize());
            down_cast<BinaryColumn*>(data_col)->append(Slice(str));
        }
        break;
    }
    case TYPE_JSON: {
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
        data_col->append_default();
        break;
    }
    default: {
        if (val.IsString()) {
            Slice slice(val.GetString(), val.GetStringLength());
            down_cast<BinaryColumn*>(data_col)->append(slice);
        } else {
            data_col->append_default();
        }
        break;
    }
    }

    return Status::OK();
}

std::string EsSqlResponseParser::normalize_iso8601_datetime(const std::string& iso8601) {
    std::string result = iso8601;
    size_t t_pos = result.find('T');
    if (t_pos != std::string::npos) {
        result[t_pos] = ' ';
    }
    if (result.size() > 19) {
        size_t tz_pos = result.find_first_of("Z+-", 19);
        if (tz_pos != std::string::npos) {
            result = result.substr(0, tz_pos);
        }
    }
    result.erase(result.find_last_not_of(" \t") + 1);
    return result;
}

int EsSqlResponseParser::fraction_to_microseconds(const std::string& datetime) {
    // Convert the fractional-seconds portion (digits following the first '.') of
    // a normalized datetime string into microseconds. ES SQL DATETIME uses
    // millisecond precision, so ".123" must yield 123000us rather than 123us,
    // and ".1" must yield 100000us rather than 1us.
    size_t dot = datetime.find('.');
    if (dot == std::string::npos) {
        return 0;
    }
    size_t i = dot + 1;
    int usec = 0;
    int digits = 0;
    while (i < datetime.size() && datetime[i] >= '0' && datetime[i] <= '9' && digits < 6) {
        usec = usec * 10 + (datetime[i] - '0');
        ++i;
        ++digits;
    }
    // Scale up to a full 6-digit microsecond value, padding missing digits.
    for (int j = digits; j < 6; ++j) {
        usec *= 10;
    }
    return usec;
}

} // namespace starrocks
