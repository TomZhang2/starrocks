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

public class PassThroughQueryValidatorTest {

    @Test
    public void testNormalSelect() {
        String result = PassThroughQueryValidator.normalize("SELECT id FROM logs");
        Assertions.assertEquals("SELECT id FROM logs", result);
    }

    @Test
    public void testTrimWhitespace() {
        String result = PassThroughQueryValidator.normalize("  SELECT id FROM logs  ");
        Assertions.assertEquals("SELECT id FROM logs", result);
    }

    @Test
    public void testStripTrailingSemicolon() {
        String result = PassThroughQueryValidator.normalize("SELECT id FROM logs;");
        Assertions.assertEquals("SELECT id FROM logs", result);
    }

    @Test
    public void testStripMultipleTrailingSemicolons() {
        String result = PassThroughQueryValidator.normalize("SELECT id FROM logs;;;");
        Assertions.assertEquals("SELECT id FROM logs", result);
    }

    @Test
    public void testRejectNonSelect() {
        Assertions.assertThrows(IllegalArgumentException.class, () -> {
            PassThroughQueryValidator.normalize("DELETE FROM logs WHERE id = 1");
        });
    }

    @Test
    public void testRejectInsert() {
        Assertions.assertThrows(IllegalArgumentException.class, () -> {
            PassThroughQueryValidator.normalize("INSERT INTO logs VALUES (1)");
        });
    }

    @Test
    public void testRejectEmpty() {
        Assertions.assertThrows(IllegalArgumentException.class, () -> {
            PassThroughQueryValidator.normalize("");
        });
    }

    @Test
    public void testRejectOnlySemicolons() {
        Assertions.assertThrows(IllegalArgumentException.class, () -> {
            PassThroughQueryValidator.normalize(";;;");
        });
    }

    @Test
    public void testSkipLineComment() {
        // normalize() strips comments internally for SELECT validation, but returns the original string
        String result = PassThroughQueryValidator.normalize("-- comment\nSELECT id FROM logs");
        Assertions.assertEquals("-- comment\nSELECT id FROM logs", result);
    }

    @Test
    public void testSkipBlockComment() {
        // normalize() strips comments internally for SELECT validation, but returns the original string
        String result = PassThroughQueryValidator.normalize("/* comment */SELECT id FROM logs");
        Assertions.assertEquals("/* comment */SELECT id FROM logs", result);
    }

    @Test
    public void testCaseInsensitiveSelect() {
        String result = PassThroughQueryValidator.normalize("select id from logs");
        Assertions.assertEquals("select id from logs", result);
    }

    @Test
    public void testRejectSelectionKeyword() {
        // "selection" should not match "select"
        Assertions.assertThrows(IllegalArgumentException.class, () -> {
            PassThroughQueryValidator.normalize("selection FROM logs");
        });
    }
}
