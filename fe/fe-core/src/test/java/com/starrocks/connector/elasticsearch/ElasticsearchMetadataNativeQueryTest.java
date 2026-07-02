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

package com.starrocks.connector.elasticsearch;

import com.starrocks.catalog.EsTable;
import com.starrocks.catalog.PassThroughQueryTable;
import com.starrocks.catalog.Table;
import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;
import org.mockito.Mockito;

import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class ElasticsearchMetadataNativeQueryTest {

    @Test
    public void testGetTableFromQueryReturnsEsTableWithCorrectSchema() throws Exception {
        // Mock EsRestClient
        EsRestClient mockClient = Mockito.mock(EsRestClient.class);

        // Mock probeEsSqlSchema to return test columns
        List<EsRestClient.EsSqlColumn> mockColumns = Arrays.asList(
                new EsRestClient.EsSqlColumn("id", "long"),
                new EsRestClient.EsSqlColumn("name", "text"),
                new EsRestClient.EsSqlColumn("score", "double"),
                new EsRestClient.EsSqlColumn("ts", "datetime")
        );
        Mockito.when(mockClient.probeEsSqlSchema(Mockito.anyString())).thenReturn(mockColumns);

        // Create ElasticsearchMetadata with mock client
        Map<String, String> properties = new HashMap<>();
        properties.put(EsTable.KEY_HOSTS, "http://localhost:9200");
        properties.put(EsTable.KEY_USER, "user");
        properties.put(EsTable.KEY_PASSWORD, "pass");
        ElasticsearchMetadata metadata = new ElasticsearchMetadata(mockClient, properties, "es0");

        // Call getTableFromQuery
        Table result = metadata.getTableFromQuery(null, "default_db", "SELECT id, name, score, ts FROM logs");

        // Verify
        Assertions.assertNotNull(result);
        Assertions.assertTrue(result instanceof EsTable);
        Assertions.assertTrue(result instanceof PassThroughQueryTable);
        Assertions.assertTrue(((PassThroughQueryTable) result).isQueryTable());
        Assertions.assertEquals("SELECT id, name, score, ts FROM logs",
                ((PassThroughQueryTable) result).getPassThroughQuery());

        // Verify schema
        Assertions.assertEquals(4, result.getFullSchema().size());
        Assertions.assertEquals("id", result.getFullSchema().get(0).getName());
        Assertions.assertEquals("name", result.getFullSchema().get(1).getName());
        Assertions.assertEquals("score", result.getFullSchema().get(2).getName());
        Assertions.assertEquals("ts", result.getFullSchema().get(3).getName());

        // Verify the probe was called
        Mockito.verify(mockClient).probeEsSqlSchema("SELECT id, name, score, ts FROM logs");
    }

    @Test
    public void testGetTableFromQueryRejectsNonSelect() {
        EsRestClient mockClient = Mockito.mock(EsRestClient.class);
        Map<String, String> properties = new HashMap<>();
        properties.put(EsTable.KEY_HOSTS, "http://localhost:9200");
        ElasticsearchMetadata metadata = new ElasticsearchMetadata(mockClient, properties, "es0");

        Assertions.assertThrows(RuntimeException.class, () -> {
            metadata.getTableFromQuery(null, "default_db", "DELETE FROM logs");
        });

        // Verify probe was NOT called (validation happens before probe)
        Mockito.verify(mockClient, Mockito.never()).probeEsSqlSchema(Mockito.anyString());
    }

    @Test
    public void testGetTableFromQueryRejectsEmptyQuery() {
        EsRestClient mockClient = Mockito.mock(EsRestClient.class);
        Map<String, String> properties = new HashMap<>();
        properties.put(EsTable.KEY_HOSTS, "http://localhost:9200");
        ElasticsearchMetadata metadata = new ElasticsearchMetadata(mockClient, properties, "es0");

        Assertions.assertThrows(RuntimeException.class, () -> {
            metadata.getTableFromQuery(null, "default_db", "");
        });

        Mockito.verify(mockClient, Mockito.never()).probeEsSqlSchema(Mockito.anyString());
    }

    @Test
    public void testGetTableFromQueryStripsTrailingSemicolon() throws Exception {
        EsRestClient mockClient = Mockito.mock(EsRestClient.class);
        Mockito.when(mockClient.probeEsSqlSchema(Mockito.anyString())).thenReturn(
                Collections.singletonList(new EsRestClient.EsSqlColumn("id", "long"))
        );
        Map<String, String> properties = new HashMap<>();
        properties.put(EsTable.KEY_HOSTS, "http://localhost:9200");
        ElasticsearchMetadata metadata = new ElasticsearchMetadata(mockClient, properties, "es0");

        Table result = metadata.getTableFromQuery(null, "default_db", "SELECT id FROM logs;");

        // Verify the query passed to probe has no semicolon
        Mockito.verify(mockClient).probeEsSqlSchema("SELECT id FROM logs");

        // Verify the stored pass-through query also has no semicolon
        Assertions.assertEquals("SELECT id FROM logs", ((PassThroughQueryTable) result).getPassThroughQuery());
    }
}
