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

package com.starrocks.catalog;

import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.util.HashMap;
import java.util.Map;

public class EsTableNativeQueryTest {

    @Test
    public void testPassThroughQueryMarker() {
        // Create an EsTable with minimal properties
        Map<String, String> props = new HashMap<>();
        props.put(EsTable.KEY_HOSTS, "http://localhost:9200");
        props.put(EsTable.KEY_INDEX, "test_index");
        props.put(EsTable.KEY_TRANSPORT, "http");

        EsTable esTable = new EsTable();
        // Before setPassThroughQuery
        Assertions.assertFalse(esTable.isQueryTable());
        Assertions.assertNull(esTable.getPassThroughQuery());

        // After setPassThroughQuery
        esTable.setPassThroughQuery("SELECT * FROM logs");
        Assertions.assertTrue(esTable.isQueryTable());
        Assertions.assertEquals("SELECT * FROM logs", esTable.getPassThroughQuery());
    }

    @Test
    public void testImplementsPassThroughQueryTable() {
        EsTable esTable = new EsTable();
        Assertions.assertTrue(esTable instanceof PassThroughQueryTable);
    }

    @Test
    public void testKeyNativeQueryConstant() {
        Assertions.assertEquals("native_query", EsTable.KEY_NATIVE_QUERY);
    }
}
