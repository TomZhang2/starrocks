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

#include <gtest/gtest.h>

#include "column/chunk.h"
#include "column/column_helper.h"
#include "common/config.h"
#include "runtime/descriptor_helper.h"
#include "runtime/descriptors.h"
#include "runtime/runtime_state.h"
#include "types/timestamp_value.h"

#ifndef __clang__
DIAGNOSTIC_PUSH
DIAGNOSTIC_IGNORE("-Wclass-memaccess")
#endif
#include <rapidjson/document.h>
#ifndef __clang__
DIAGNOSTIC_POP
#endif

namespace starrocks {

struct SlotDesc {
    std::string name;
    TypeDescriptor type;
};

class EsSqlParserTest : public ::testing::Test {
public:
    void SetUp() override { _create_runtime_state(""); }
    void TearDown() override {}

protected:
    void _create_runtime_state(const std::string& timezone);
    TupleDescriptor* _create_tuple_desc(SlotDesc* descs);
    std::unique_ptr<Chunk> _create_chunk(SlotDesc* descs);
    std::vector<EsSqlColumn> _make_columns(SlotDesc* descs);

    ObjectPool _pool;
    RuntimeState* _runtime_state = nullptr;
};

void EsSqlParserTest::_create_runtime_state(const std::string& timezone) {
    TUniqueId fragment_id;
    TQueryOptions query_options;
    TQueryGlobals query_globals;
    if (timezone != "") {
        query_globals.__set_time_zone(timezone);
    }
    _runtime_state =
            _pool.add(new RuntimeState(fragment_id, query_options, query_globals, static_cast<ExecEnv*>(nullptr)));
    _runtime_state->init_instance_mem_tracker();
}

TupleDescriptor* EsSqlParserTest::_create_tuple_desc(SlotDesc* descs) {
    TDescriptorTableBuilder table_desc_builder;
    TSlotDescriptorBuilder slot_desc_builder;
    TTupleDescriptorBuilder tuple_desc_builder;
    int slot_id = 0;
    while (descs->name != "") {
        slot_desc_builder.column_name(descs->name).type(descs->type).id(slot_id).nullable(true);
        tuple_desc_builder.add_slot(slot_desc_builder.build());
        descs += 1;
        slot_id += 1;
    }
    tuple_desc_builder.build(&table_desc_builder);
    std::vector<TTupleId> row_tuples = std::vector<TTupleId>{0};
    DescriptorTbl* tbl = nullptr;
    CHECK(DescriptorTbl::create(_runtime_state, &_pool, table_desc_builder.desc_tbl(), &tbl, config::vector_chunk_size)
                  .ok());
    auto* row_desc = _pool.add(new RowDescriptor(*tbl, row_tuples));
    auto* tuple_desc = row_desc->tuple_descriptors()[0];
    return tuple_desc;
}

std::unique_ptr<Chunk> EsSqlParserTest::_create_chunk(SlotDesc* descs) {
    Columns columns;
    SlotDesc* p = descs;
    while (p->name != "") {
        columns.emplace_back(ColumnHelper::create_column(p->type, true));
        p += 1;
    }
    return std::make_unique<Chunk>(std::move(columns), Chunk::SlotHashMap{});
}

std::vector<EsSqlColumn> EsSqlParserTest::_make_columns(SlotDesc* descs) {
    std::vector<EsSqlColumn> cols;
    SlotDesc* p = descs;
    while (p->name != "") {
        cols.push_back({p->name, ""});
        p += 1;
    }
    return cols;
}

// [P1] unsigned_long values arrive as JSON strings and must not crash
// GetUint64(); they must parse into int128 preserving full precision.
TEST_F(EsSqlParserTest, UnsignedLongAsString) {
    SlotDesc descs[] = {
            {"ul", TypeDescriptor(TYPE_LARGEINT)},
            {""},
    };
    auto* tuple_desc = _create_tuple_desc(descs);
    auto columns = _make_columns(descs);

    std::string response = R"({"columns":[{"name":"ul","type":"unsigned_long"}],)"
                           R"("rows":[["18446744073709551615"],["9007199254740993"]]})";

    auto chunk = _create_chunk(descs);
    ASSERT_TRUE(EsSqlResponseParser::parse(response, columns, tuple_desc, chunk.get()).ok());

    ASSERT_EQ(2, chunk->num_rows());
    EXPECT_EQ((int128_t)18446744073709551615ULL, chunk->get_column_by_index(0)->get(0).get_int128());
    EXPECT_EQ((int128_t)9007199254740993ULL, chunk->get_column_by_index(0)->get(1).get_int128());
}

// [P1] Datetime fractional seconds are millisecond precision and must be scaled
// to microseconds; ".123" -> 123000us, not 123us.
TEST_F(EsSqlParserTest, DatetimeFractionalSecondsAreMilliseconds) {
    SlotDesc descs[] = {
            {"dt", TypeDescriptor(TYPE_DATETIME)},
            {""},
    };
    auto* tuple_desc = _create_tuple_desc(descs);
    auto columns = _make_columns(descs);

    std::string response = R"({"rows":[)"
                           R"(["2023-01-15T10:30:45.123Z"],)"
                           R"(["2023-06-01T00:00:00.1Z"],)"
                           R"(["2023-06-01T00:00:00.12Z"],)"
                           R"(["2023-06-01T00:00:00.123456Z"],)"
                           R"(["2023-06-01T00:00:00Z"],)"
                           R"(["2023-06-01"])"
                           R"(]})";

    auto chunk = _create_chunk(descs);
    ASSERT_TRUE(EsSqlResponseParser::parse(response, columns, tuple_desc, chunk.get()).ok());
    ASSERT_EQ(6, chunk->num_rows());

    auto col = chunk->get_column_by_index(0);
    auto sub_usec = [&](size_t r) { return col->get(r).get_timestamp().to_unixtime() % 1000000; };

    EXPECT_EQ(123000, sub_usec(0));
    EXPECT_EQ(100000, sub_usec(1));
    EXPECT_EQ(120000, sub_usec(2));
    EXPECT_EQ(123456, sub_usec(3));
    EXPECT_EQ(0, sub_usec(4));
    EXPECT_EQ(0, sub_usec(5));

    auto expected = TimestampValue::create(2023, 1, 15, 10, 30, 45, 123000);
    EXPECT_EQ(expected.to_unixtime(), col->get(0).get_timestamp().to_unixtime());
}

// [P2] A row with fewer values than the column count must not read out of
// bounds; missing columns become null so the chunk stays aligned.
TEST_F(EsSqlParserTest, ShortRowAppendsNullForMissingColumns) {
    SlotDesc descs[] = {
            {"a", TypeDescriptor(TYPE_LARGEINT)},
            {"b", TypeDescriptor(TYPE_DATETIME)},
            {""},
    };
    auto* tuple_desc = _create_tuple_desc(descs);
    auto columns = _make_columns(descs);

    std::string response = R"({"rows":[["42","2023-01-01T00:00:00.000Z"],["7"]]})";

    auto chunk = _create_chunk(descs);
    ASSERT_TRUE(EsSqlResponseParser::parse(response, columns, tuple_desc, chunk.get()).ok());
    ASSERT_EQ(2, chunk->num_rows());

    EXPECT_EQ((int128_t)42, chunk->get_column_by_index(0)->get(0).get_int128());
    EXPECT_EQ((int128_t)7, chunk->get_column_by_index(0)->get(1).get_int128());
    EXPECT_FALSE(chunk->get_column_by_index(1)->is_null(0));
    EXPECT_TRUE(chunk->get_column_by_index(1)->is_null(1));
}

} // namespace starrocks
