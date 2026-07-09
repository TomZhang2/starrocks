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

import com.github.benmanes.caffeine.cache.Cache;
import com.github.benmanes.caffeine.cache.Caffeine;
import com.starrocks.catalog.Column;
import com.starrocks.catalog.Database;
import com.starrocks.catalog.EsTable;
import com.starrocks.catalog.PartitionKey;
import com.starrocks.catalog.PassThroughQueryValidator;
import com.starrocks.catalog.SinglePartitionInfo;
import com.starrocks.catalog.Table;
import com.starrocks.catalog.Type;
import com.starrocks.common.DdlException;
import com.starrocks.connector.ConnectorMetadata;
import com.starrocks.connector.TableVersionRange;
import com.starrocks.connector.exception.StarRocksConnectorException;
import com.starrocks.qe.ConnectContext;
import com.starrocks.sql.optimizer.OptimizerContext;
import com.starrocks.sql.optimizer.operator.scalar.ColumnRefOperator;
import com.starrocks.sql.optimizer.operator.scalar.ScalarOperator;
import com.starrocks.sql.optimizer.statistics.ColumnStatistic;
import com.starrocks.sql.optimizer.statistics.Statistics;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.NoSuchElementException;
import java.util.concurrent.TimeUnit;

import static com.starrocks.connector.ConnectorTableId.CONNECTOR_ID_GENERATOR;

// TODO add meta cache
public class ElasticsearchMetadata
        implements ConnectorMetadata {
    private static final Logger LOG = LogManager.getLogger(EsTable.class);

    private static final long DEFAULT_OUTPUT_ROW_COUNT = 1L;

    private final Cache<String, Long> rowCountCache = Caffeine.newBuilder()
            .maximumSize(1000)
            .expireAfterWrite(1, TimeUnit.HOURS)
            .build();

    private final EsRestClient esRestClient;
    private final Map<String, String> properties;
    private final String catalogName;
    public static final String DEFAULT_DB = "default_db";
    public static final long DEFAULT_DB_ID = 1L;


    public ElasticsearchMetadata(EsRestClient esRestClient, Map<String, String> properties, String catalogName) {
        this.esRestClient = esRestClient;
        this.properties = properties;
        this.catalogName = catalogName;
    }

    @Override
    public Table.TableType getTableType() {
        return Table.TableType.ELASTICSEARCH;
    }

    @Override
    public List<String> listDbNames() {
        return Arrays.asList(DEFAULT_DB);
    }

    @Override
    public List<String> listTableNames(String dbName) {
        return esRestClient.listTables();
    }

    @Override
    public Database getDb(String dbName) {
        return new Database(DEFAULT_DB_ID, DEFAULT_DB);
    }

    @Override
    public Table getTable(String dbName, String tblName) {
        if (!DEFAULT_DB.equalsIgnoreCase(dbName)) {
            return null;
        }
        return toEsTable(esRestClient, properties, tblName, dbName, catalogName);
    }

    @Override
    public Table getTableFromQuery(ConnectContext context, String dbName, String query) {
        String normalizedQuery = PassThroughQueryValidator.normalize(query);

        String distribution = properties.get(EsTable.KEY_ES_DISTRIBUTION);
        if (distribution != null) {
            esRestClient.setDistribution(distribution);
        }

        List<EsRestClient.EsSqlColumn> esColumns;
        try {
            esColumns = esRestClient.probeEsSqlSchema(normalizedQuery);
        } catch (Exception e) {
            throw new StarRocksConnectorException(
                    "Failed to infer schema for ES native query: " + e.getMessage(), e);
        }

        if (esColumns.isEmpty()) {
            throw new StarRocksConnectorException("ES native query returned no columns");
        }

        List<Column> fullSchema = new ArrayList<>();
        for (EsRestClient.EsSqlColumn esCol : esColumns) {
            Type srType = EsUtil.convertEsSqlType(esCol.getType());
            fullSchema.add(new Column(esCol.getName(), srType, true));
        }

        Map<String, String> tableProps = new HashMap<>(properties);
        tableProps.putIfAbsent(EsTable.KEY_INDEX, "_native_query_placeholder");

        long tableId = CONNECTOR_ID_GENERATOR.getNextId().asInt();
        try {
            EsTable queryTable = new EsTable(tableId, catalogName, dbName,
                    "_query_" + tableId, fullSchema, tableProps, new SinglePartitionInfo());
            queryTable.setPassThroughQuery(normalizedQuery);
            return queryTable;
        } catch (DdlException e) {
            throw new StarRocksConnectorException("Failed to create ES query table: " + e.getMessage(), e);
        }
    }

    @Override
    public Statistics getTableStatistics(OptimizerContext session,
                                         Table table,
                                         Map<ColumnRefOperator, Column> columns,
                                         List<PartitionKey> partitionKeys,
                                         ScalarOperator predicate,
                                         long limit,
                                         TableVersionRange tableVersionRange) {
        Statistics.Builder builder = Statistics.builder();
        EsTable esTable = (EsTable) table;
        long rowCount;
        if (esTable.isQueryTable()) {
            rowCount = DEFAULT_OUTPUT_ROW_COUNT;
        } else {
            rowCount = rowCountCache.get(esTable.getIndexName(), key -> {
                long cnt = esRestClient.getRowCount(key);
                return cnt >= 0 ? cnt : DEFAULT_OUTPUT_ROW_COUNT;
            });
        }
        builder.setOutputRowCount(rowCount);
        for (Map.Entry<ColumnRefOperator, Column> entry : columns.entrySet()) {
            builder.addColumnStatistic(entry.getKey(), ColumnStatistic.builder()
                    .setAverageRowSize(entry.getValue().getType().getTypeSize())
                    .setNullsFraction(0)
                    .setType(ColumnStatistic.StatisticType.ESTIMATE)
                    .build());
        }
        return builder.build();
    }

    public static EsTable toEsTable(EsRestClient esRestClient,
                                    Map<String, String> properties,
                                    String tableName, String dbName, String catalogName) {
        try {
            List<Column> columns = EsUtil.convertColumnSchema(esRestClient, tableName);
            properties.put(EsTable.KEY_INDEX, tableName);
            EsTable esTable = new EsTable(CONNECTOR_ID_GENERATOR.getNextId().asInt(),
                    catalogName, dbName, tableName, columns, properties, new SinglePartitionInfo());
            esTable.setComment("created by external es catalog");
            esTable.syncTableMetaData(esRestClient);
            return esTable;
        } catch (NoSuchElementException e) {
            LOG.error(String.format("Unknown index {%s}", tableName), e);
            return null;
        } catch (Exception e) {
            LOG.error("transform to EsTable Error", e);
            return null;
        }

    }
}