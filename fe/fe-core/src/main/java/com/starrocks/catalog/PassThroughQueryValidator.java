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

import org.apache.commons.lang3.StringUtils;

/**
 * Query normalization and validation utility for pass-through (native_query)
 * table functions. Shared by JDBCTable and EsTable so both enforce the same
 * SELECT-only constraint with identical comment-stripping semantics.
 */
public class PassThroughQueryValidator {

    /**
     * Trim the query, strip trailing semicolons, reject empty input, and validate
     * that the leading SQL statement is a SELECT.
     */
    public static String normalize(String query) {
        String normalizedQuery = StringUtils.trimToEmpty(query);
        while (normalizedQuery.endsWith(";")) {
            normalizedQuery = StringUtils.stripEnd(normalizedQuery.substring(0, normalizedQuery.length() - 1), null);
        }
        if (normalizedQuery.isEmpty()) {
            throw new IllegalArgumentException("pass-through query cannot be empty");
        }
        validateSelectOnly(normalizedQuery);
        return normalizedQuery;
    }

    private static void validateSelectOnly(String query) {
        String leadingSql = stripLeadingComments(query);
        if (!startsWithSqlKeyword(leadingSql, "select")) {
            throw new IllegalArgumentException("native query table function only supports SELECT queries");
        }
    }

    private static String stripLeadingComments(String query) {
        int offset = 0;
        while (offset < query.length()) {
            char ch = query.charAt(offset);
            if (Character.isWhitespace(ch)) {
                offset++;
                continue;
            }
            if (ch == '-' && offset + 1 < query.length() && query.charAt(offset + 1) == '-') {
                offset += 2;
                while (offset < query.length() && query.charAt(offset) != '\n' && query.charAt(offset) != '\r') {
                    offset++;
                }
                continue;
            }
            if (ch == '/' && offset + 1 < query.length() && query.charAt(offset + 1) == '*') {
                int commentEnd = query.indexOf("*/", offset + 2);
                if (commentEnd < 0) {
                    throw new IllegalArgumentException("native query table function only supports SELECT queries");
                }
                offset = commentEnd + 2;
                continue;
            }
            break;
        }
        return query.substring(offset);
    }

    private static boolean startsWithSqlKeyword(String query, String keyword) {
        if (!query.regionMatches(true, 0, keyword, 0, keyword.length())) {
            return false;
        }
        if (query.length() == keyword.length()) {
            return true;
        }
        char next = query.charAt(keyword.length());
        return !Character.isLetterOrDigit(next) && next != '_';
    }
}
