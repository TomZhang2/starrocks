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

// This file is based on code available under the Apache license here:
//   https://github.com/apache/incubator-doris/blob/master/fe/fe-core/src/main/java/org/apache/doris/external/elasticsearch/EsRestClient.java

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package com.starrocks.connector.elasticsearch;

import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.MapperFeature;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.google.common.collect.ImmutableList;
import com.starrocks.connector.exception.StarRocksConnectorException;
import okhttp3.Credentials;
import okhttp3.MediaType;
import okhttp3.OkHttpClient;
import okhttp3.Request;
import okhttp3.RequestBody;
import okhttp3.Response;
import org.apache.http.HttpHeaders;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;
import org.apache.logging.log4j.util.Strings;

import java.io.IOException;
import java.security.SecureRandom;
import java.security.cert.CertificateException;
import java.security.cert.X509Certificate;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.stream.Collectors;
import javax.net.ssl.HostnameVerifier;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSession;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManager;
import javax.net.ssl.X509TrustManager;

import static com.starrocks.connector.elasticsearch.EsUtil.getFromJSONArray;
import static com.starrocks.connector.elasticsearch.EsUtil.readTree;

public class EsRestClient {

    private static final Logger LOG = LogManager.getLogger(EsRestClient.class);
    private final ObjectMapper mapper;

    {
        mapper = new ObjectMapper();
        mapper.configure(MapperFeature.USE_ANNOTATIONS, false);
    }

    private static final OkHttpClient NETWORK_CLIENT = new OkHttpClient.Builder()
            .readTimeout(10, TimeUnit.SECONDS)
            .build();

    private static OkHttpClient sslNetworkClient;

    private final String[] nodes;
    private final String authHeader;
    private final AtomicInteger currentNodeIndex = new AtomicInteger(0);

    private boolean sslEnabled;

    public EsRestClient(String[] nodes, String authUser, String authPassword, boolean sslEnabled) {
        this(nodes, authUser, authPassword);
        this.sslEnabled = sslEnabled;
    }

    public EsRestClient(String[] nodes, String authUser, String authPassword) {
        this.nodes = nodes;
        this.authHeader = (!Strings.isEmpty(authUser) && !Strings.isEmpty(authPassword))
                ? Credentials.basic(authUser, authPassword) : null;
    }

    /**
     * Build a fresh Request.Builder with the auth header (if any) pre-set.
     * Each call gets its own builder so concurrent requests never share
     * mutable Request.Builder state.
     */
    private Request.Builder newRequestBuilder() {
        Request.Builder b = new Request.Builder();
        if (authHeader != null) {
            b.addHeader(HttpHeaders.AUTHORIZATION, authHeader);
        }
        return b;
    }

    /**
     * Resolve the node at the given round-robin offset into a normalized URL
     * (with protocol prepended if missing). Uses a local computation so
     * concurrent calls never clobber shared node state.
     */
    private String nodeUrl(int offset) {
        int idx = offset % nodes.length;
        String node = nodes[idx].trim();
        if (!node.startsWith("http://") && !node.startsWith("https://")) {
            node = "http://" + node;
        }
        return node;
    }

    public Map<String, EsNodeInfo> getHttpNodes() throws StarRocksConnectorException {
        Map<String, Map<String, Object>> nodesData = get("_nodes/http", "nodes");
        if (nodesData == null) {
            return Collections.emptyMap();
        }
        Map<String, EsNodeInfo> nodesMap = new HashMap<>();
        for (Map.Entry<String, Map<String, Object>> entry : nodesData.entrySet()) {
            EsNodeInfo node = new EsNodeInfo(entry.getKey(), entry.getValue());
            if (node.hasHttp()) {
                nodesMap.put(node.getId(), node);
            }
        }
        return nodesMap;
    }

    /**
     * Get remote ES Cluster version
     *
     * @return
     * @throws Exception
     */
    public EsMajorVersion version() throws StarRocksConnectorException {
        Map<String, Object> result = get("/", null);
        if (result == null) {
            throw new StarRocksConnectorException("Unable to retrieve ES main cluster info.");
        }
        Map<String, String> versionBody = (Map<String, String>) result.get("version");
        return EsMajorVersion.parse(versionBody.get("number"));
    }

    /**
     * Get mapping for indexName
     *
     * @param indexName
     * @return
     * @throws Exception
     */
    public String getMapping(String indexName) throws StarRocksConnectorException {
        String path = indexName + "/_mapping";
        String indexMapping = execute(path);
        if (indexMapping == null) {
            throw new StarRocksConnectorException("index[" + indexName + "] not found");
        }
        return indexMapping;
    }

    /**
     * Estimate the number of documents in an index using _cat/indices metadata.
     * Reads from Lucene segment metadata — near-zero overhead, slightly stale, good enough for CBO.
     *
     * @param indexName
     * @return document count, or -1 on failure
     */
    public long getRowCount(String indexName) {
        String path = "_cat/indices/" + indexName + "?h=docs.count&format=json";
        try {
            String response = execute(path);
            if (response == null) {
                return -1L;
            }
            List list = mapper.readValue(response, List.class);
            long total = 0;
            boolean found = false;
            for (Object row : list) {
                if (row instanceof Map) {
                    Object val = ((Map) row).get("docs.count");
                    if (val != null) {
                        total += Long.parseLong(val.toString());
                        found = true;
                    }
                }
            }
            if (found) {
                return total;
            }
        } catch (Exception e) {
            LOG.warn("Failed to get docs.count for ES index {}: {}", indexName, e.getMessage());
        }
        return -1L;
    }

    /**
     * Get Shard location
     *
     * @param indexName
     * @return
     * @throws StarRocksConnectorException
     */
    public EsShardPartitions searchShards(String indexName) throws StarRocksConnectorException {
        String path = indexName + "/_search_shards";
        String searchShards = execute(path);
        if (searchShards == null) {
            throw new StarRocksConnectorException("request index [" + indexName + "] search_shards failure");
        }
        return EsShardPartitions.findShardPartitions(indexName, searchShards);
    }

    /**
     * execute request for specific path, it will try again nodes.length times if it fails
     *
     * @param path the path must not leading with '/'
     * @return response
     */
    String execute(String path) throws StarRocksConnectorException {
        int retrySize = nodes.length;
        int startIndex = currentNodeIndex.getAndIncrement();
        StarRocksConnectorException scratchExceptionForThrow = null;
        OkHttpClient client;
        if (sslEnabled) {
            client = getOrCreateSSLClient();
        } else {
            client = NETWORK_CLIENT;
        }
        for (int i = 0; i < retrySize; i++) {
            String currentNode = nodeUrl(startIndex + i);
            Request request = newRequestBuilder().get()
                    .url(currentNode + "/" + path)
                    .build();
            Response response = null;
            if (LOG.isTraceEnabled()) {
                LOG.trace("es rest client request URL: {}", currentNode + "/" + path);
            }
            try {
                response = client.newCall(request).execute();
                if (response.isSuccessful()) {
                    return response.body().string();
                }
            } catch (IOException e) {
                LOG.warn("request node [{}] [{}] failures {}, try next nodes", currentNode, path, e);
                scratchExceptionForThrow = new StarRocksConnectorException(e.getMessage());
            } finally {
                if (response != null) {
                    response.close();
                }
            }
        }
        LOG.warn("try all nodes [{}],no other nodes left", (Object) nodes);
        if (scratchExceptionForThrow != null) {
            throw scratchExceptionForThrow;
        }
        return null;
    }

    /**
     * Execute a POST request to ES. Reuses the same SSL client selection as execute().
     */
    String executePost(String path, String jsonBody) throws StarRocksConnectorException {
        int retrySize = nodes.length;
        int startIndex = currentNodeIndex.getAndIncrement();
        StarRocksConnectorException scratchExceptionForThrow = null;
        OkHttpClient client;
        if (sslEnabled) {
            client = getOrCreateSSLClient();
        } else {
            client = NETWORK_CLIENT;
        }
        MediaType jsonMediaType = MediaType.parse("application/json; charset=utf-8");
        for (int i = 0; i < retrySize; i++) {
            String currentNode = nodeUrl(startIndex + i);
            RequestBody body = RequestBody.create(jsonBody, jsonMediaType);
            Request request = newRequestBuilder().post(body)
                    .url(currentNode + "/" + path)
                    .build();
            Response response = null;
            try {
                response = client.newCall(request).execute();
                if (response.isSuccessful()) {
                    return response.body().string();
                }
                // For 4xx client errors (e.g. SQL syntax errors), read the body and throw
                // immediately without retrying other nodes — the error is deterministic.
                if (response.code() >= 400 && response.code() < 500) {
                    String errorBody = response.body() != null ? response.body().string() : "";
                    throw new StarRocksConnectorException(
                            "ES request to " + currentNode + "/" + path + " failed: HTTP " + response.code()
                                    + " — " + errorBody);
                }
            } catch (StarRocksConnectorException e) {
                throw e;
            } catch (IOException e) {
                LOG.warn("request node [{}] [{}] failures {}, try next nodes", currentNode, path, e);
                scratchExceptionForThrow = new StarRocksConnectorException(e.getMessage());
            } finally {
                if (response != null) {
                    response.close();
                }
            }
        }
        throw scratchExceptionForThrow != null
                ? scratchExceptionForThrow
                : new StarRocksConnectorException("All ES nodes failed for POST " + path);
    }

    /**
     * Probe ES SQL schema by executing the query with fetch_size=1.
     * Returns column metadata from the _sql response.
     * The cursor is immediately closed after reading columns.
     */
    public List<EsSqlColumn> probeEsSqlSchema(String sqlQuery) throws StarRocksConnectorException {
        try {
            com.fasterxml.jackson.databind.node.ObjectNode requestBody = mapper.createObjectNode();
            requestBody.put("query", sqlQuery);
            requestBody.put("fetch_size", 1);

            String response = executePost("_sql", requestBody.toString());

            JsonNode json = mapper.readTree(response);

            if (json.has("error")) {
                throw new StarRocksConnectorException("ES SQL error: " +
                        json.path("error").path("type").asText() + " - " +
                        json.path("error").path("reason").asText());
            }

            List<EsSqlColumn> columns = new ArrayList<>();
            if (json.has("columns")) {
                for (JsonNode colNode : json.get("columns")) {
                    columns.add(new EsSqlColumn(
                            colNode.get("name").asText(),
                            colNode.get("type").asText()
                    ));
                }
            }

            if (json.has("cursor")) {
                String cursor = json.get("cursor").asText();
                try {
                    closeEsSqlCursor(cursor);
                } catch (Exception e) {
                    LOG.warn("Failed to close ES SQL cursor during schema probe", e);
                }
            }

            return columns;
        } catch (StarRocksConnectorException e) {
            throw e;
        } catch (Exception e) {
            throw new StarRocksConnectorException("Failed to probe ES SQL schema: " + e.getMessage(), e);
        }
    }

    /**
     * Close an ES SQL cursor to free server state.
     */
    private void closeEsSqlCursor(String cursor) throws StarRocksConnectorException {
        try {
            com.fasterxml.jackson.databind.node.ObjectNode body = mapper.createObjectNode();
            body.put("cursor", cursor);
            executePost("_sql/close", body.toString());
        } catch (Exception e) {
            LOG.warn("Failed to close ES SQL cursor", e);
        }
    }

    public <T> T get(String q, String key) throws StarRocksConnectorException {
        return parseContent(execute(q), key);
    }

    @SuppressWarnings("unchecked")
    private <T> T parseContent(String response, String key) {
        Map<String, Object> map = Collections.emptyMap();
        try {
            JsonParser jsonParser = mapper.getJsonFactory().createJsonParser(response);
            map = mapper.readValue(jsonParser, Map.class);
        } catch (IOException ex) {
            LOG.error("parse es response failure: [{}]", response);
            throw new StarRocksConnectorException(ex.getMessage());
        }
        return (T) (key != null ? map.get(key) : map);
    }

    private synchronized OkHttpClient getOrCreateSSLClient() {
        if (sslNetworkClient == null) {
            sslNetworkClient = new OkHttpClient.Builder()
                    .readTimeout(10, TimeUnit.SECONDS)
                    .sslSocketFactory(createSSLSocketFactory(), new TrustAllCerts())
                    .hostnameVerifier(new TrustAllHostnameVerifier())
                    .build();
        }
        return sslNetworkClient;
    }

    private static class TrustAllCerts implements X509TrustManager {
        public void checkClientTrusted(X509Certificate[] chain, String authType) throws CertificateException {
        }

        public void checkServerTrusted(X509Certificate[] chain, String authType) throws CertificateException {
        }

        public X509Certificate[] getAcceptedIssuers() {
            return new X509Certificate[0];
        }
    }

    private static class TrustAllHostnameVerifier implements HostnameVerifier {
        public boolean verify(String hostname, SSLSession session) {
            return true;
        }
    }

    private static SSLSocketFactory createSSLSocketFactory() {
        SSLSocketFactory ssfFactory;
        try {
            SSLContext sc = SSLContext.getInstance("TLS");
            sc.init(null, new TrustManager[] {new TrustAllCerts()}, new SecureRandom());
            ssfFactory = sc.getSocketFactory();
        } catch (Exception e) {
            throw new StarRocksConnectorException("Errors happens when create ssl socket");
        }
        return ssfFactory;
    }

    public static class EsIndex {
        private String index;

        public String getIndex() {
            return index;
        }
    }

    /**
     * response
     [
        {
        "index": ".kibana_1"
        },
        {
        "index": ".opendistro_security"
        },
        {
        "index": "kibana_sample_data_ecommerce"
        },
        {
        "index": "kibana_sample_data_ecommerce_2"
        },
        {
        "index": "kibana_sample_data_flights"
        },
        {
        "index": "kibana_sample_data_logs"
        }
     ]
     * indices are same as table
     * @return
     */
    private Set<String> getIndices() {
        String response = execute("_cat/indices?h=index&format=json&s=index:asc");
        if (Objects.isNull(response)) {
            throw new StarRocksConnectorException("es indexes are null, maybe this is error");
        }

        EsIndex[] esIndices = getFromJSONArray(response, EsIndex[].class);
        return Arrays.asList(esIndices).stream()
                .filter(index -> !index.getIndex().startsWith("."))
                .map(index -> index.getIndex())
                .collect(Collectors.toSet());
    }
    /**
     {
        "kibana_sample_data_ecommerce": {
            "aliases": {}
        }
     }
     * Get all alias.
     **/
    private Map<String, List<String>> getAliases() {
        String response = execute("_aliases");
        // key: index, value: aliases
        Map<String, List<String>> aliases = new HashMap<>();
        JsonNode root = readTree(response);
        if (root == null) {
            return aliases;
        }
        Iterator<Map.Entry<String, JsonNode>> indexElements = root.fields();
        while (indexElements.hasNext()) {
            Map.Entry<String, JsonNode> indexAlias = indexElements.next();
            JsonNode aliasesNode = indexAlias.getValue().get("aliases");
            Iterator<String> aliasNames = aliasesNode.fieldNames();
            if (aliasNames.hasNext()) {
                aliases.put(indexAlias.getKey(), ImmutableList.copyOf(aliasNames));
            }
        }
        return aliases;
    }

    /**
     * get tables union between indices and aliases
     * @return
     */
    public List<String> listTables() {
        Set<String> indices = getIndices();
        Map<String, List<String>> aliases = getAliases();
        aliases.entrySet().stream()
                .filter(e -> (!e.getKey().startsWith(".") && !indices.contains(e.getKey())))
                .flatMap(e -> e.getValue().stream())
                .forEach(indices::add);
        return new ArrayList<>(indices);
    }

    /**
     * Column metadata from ES SQL _sql response.
     */
    public static class EsSqlColumn {
        private final String name;
        private final String type;

        public EsSqlColumn(String name, String type) {
            this.name = name;
            this.type = type;
        }

        public String getName() {
            return name;
        }

        public String getType() {
            return type;
        }
    }
}
