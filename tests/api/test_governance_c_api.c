/*
 * NAAb Governance C API — Integration Tests
 * Tests the public C API surface (naab_governance.h).
 */

#include "naab_governance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    if (test_##name()) { \
        tests_passed++; \
        printf("PASS\n"); \
    } else { \
        printf("FAIL\n"); \
    } \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("ASSERT failed: %s (line %d) ", #cond, __LINE__); \
        return 0; \
    } \
} while(0)

/* Inline governance config for testing */
static const char* TEST_CONFIG =
    "{"
    "  \"version\": \"3.0\","
    "  \"mode\": \"enforce\","
    "  \"restrictions\": {"
    "    \"dangerous_calls\": { \"level\": \"hard\" },"
    "    \"shell_injection\": { \"level\": \"hard\" },"
    "    \"code_injection\": { \"level\": \"hard\" }"
    "  },"
    "  \"code_quality\": {"
    "    \"semantic_checks\": {"
    "      \"level\": \"hard\","
    "      \"check_imports\": true,"
    "      \"check_dangerous_eval\": true"
    "    },"
    "    \"no_secrets\": { \"level\": \"hard\" },"
    "    \"no_unsafe_deserialization\": { \"level\": \"hard\" },"
    "    \"no_sql_injection\": { \"level\": \"hard\" }"
    "  },"
    "  \"security\": { \"sandbox_level\": \"unrestricted\" }"
    "}";

/* --- Test: create/destroy lifecycle --- */
static int test_lifecycle(void) {
    naab_gov_engine_t eng = naab_gov_create();
    ASSERT(eng != NULL);
    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: destroy NULL is safe --- */
static int test_destroy_null(void) {
    naab_gov_destroy(NULL);  /* should not crash */
    return 1;
}

/* --- Test: version string --- */
static int test_version(void) {
    const char* ver = naab_gov_version_string();
    ASSERT(ver != NULL);
    ASSERT(strlen(ver) > 0);
    return 1;
}

/* --- Test: load config from JSON string --- */
static int test_load_config_string(void) {
    naab_gov_engine_t eng = naab_gov_create();
    ASSERT(eng != NULL);

    naab_gov_error_t rc = naab_gov_load_config_string(eng, TEST_CONFIG);
    ASSERT(rc == NAAB_GOV_OK);
    ASSERT(naab_gov_is_active(eng) == 1);

    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: load invalid config --- */
static int test_load_invalid_config(void) {
    naab_gov_engine_t eng = naab_gov_create();
    ASSERT(eng != NULL);

    naab_gov_error_t rc = naab_gov_load_config_string(eng, "not valid json{{{");
    ASSERT(rc == NAAB_GOV_ERR_CONFIG);

    const char* err = naab_gov_last_error(eng);
    ASSERT(err != NULL);
    ASSERT(strlen(err) > 0);

    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: NULL arg handling --- */
static int test_null_args(void) {
    ASSERT(naab_gov_load_config(NULL, "test") == NAAB_GOV_ERR_NULL_ARG);
    ASSERT(naab_gov_load_config_string(NULL, "{}") == NAAB_GOV_ERR_NULL_ARG);
    ASSERT(naab_gov_discover_config(NULL, ".") == NAAB_GOV_ERR_NULL_ARG);
    ASSERT(naab_gov_is_active(NULL) == 0);
    ASSERT(naab_gov_scan(NULL, "python", "x=1", "", 1) == NULL);
    ASSERT(naab_gov_was_blocked(NULL) == 0);
    ASSERT(naab_gov_result_count(NULL) == 0);

    const char* err = naab_gov_last_error(NULL);
    ASSERT(err != NULL);  /* returns "" not NULL */

    naab_gov_free_string(NULL);  /* should not crash */
    return 1;
}

/* --- Test: scan clean code --- */
static int test_scan_clean(void) {
    naab_gov_engine_t eng = naab_gov_create();
    naab_gov_load_config_string(eng, TEST_CONFIG);

    char* result = naab_gov_scan(eng, "python", "x = 42\nprint(x)", "test.py", 1);
    ASSERT(result != NULL);

    /* Parse result — should not be blocked */
    ASSERT(strstr(result, "\"blocked\"") != NULL);
    /* Clean code should have "blocked":false */
    ASSERT(strstr(result, "\"blocked\":true") == NULL);

    naab_gov_free_string(result);
    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: scan dangerous code (eval) → blocked --- */
static int test_scan_eval_blocked(void) {
    naab_gov_engine_t eng = naab_gov_create();
    naab_gov_load_config_string(eng, TEST_CONFIG);

    char* result = naab_gov_scan(eng, "python",
        "user_input = input()\nresult = eval(user_input)", "test.py", 1);
    ASSERT(result != NULL);
    ASSERT(strstr(result, "\"blocked\":true") != NULL);
    ASSERT(naab_gov_was_blocked(eng) == 1);

    naab_gov_free_string(result);
    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: scan os.system → blocked --- */
static int test_scan_os_system_blocked(void) {
    naab_gov_engine_t eng = naab_gov_create();
    naab_gov_load_config_string(eng, TEST_CONFIG);

    char* result = naab_gov_scan(eng, "python",
        "import os\nos.system('rm -rf /')", "test.py", 1);
    ASSERT(result != NULL);
    ASSERT(strstr(result, "\"blocked\":true") != NULL);

    naab_gov_free_string(result);
    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: JSON report structure --- */
static int test_json_report(void) {
    naab_gov_engine_t eng = naab_gov_create();
    naab_gov_load_config_string(eng, TEST_CONFIG);

    char* result = naab_gov_scan(eng, "python", "x = 1", "test.py", 1);
    naab_gov_free_string(result);

    char* report = naab_gov_json_report(eng);
    ASSERT(report != NULL);
    ASSERT(strlen(report) > 2);  /* at least "{}" */

    naab_gov_free_string(report);
    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: SARIF report non-empty --- */
static int test_sarif_report(void) {
    naab_gov_engine_t eng = naab_gov_create();
    naab_gov_load_config_string(eng, TEST_CONFIG);

    char* result = naab_gov_scan(eng, "python", "x = 1", "test.py", 1);
    naab_gov_free_string(result);

    char* sarif = naab_gov_sarif_report(eng);
    ASSERT(sarif != NULL);
    ASSERT(strlen(sarif) > 10);

    naab_gov_free_string(sarif);
    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: single check (secrets) --- */
static int test_check_secrets(void) {
    naab_gov_engine_t eng = naab_gov_create();
    naab_gov_load_config_string(eng, TEST_CONFIG);

    char* result = naab_gov_check(eng, "secrets", "python",
        "api_key = \"AKIAIOSFODNN7EXAMPLE\"", 1);
    ASSERT(result != NULL);
    ASSERT(strstr(result, "\"blocked\":true") != NULL);

    naab_gov_free_string(result);
    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: unknown check name --- */
static int test_check_unknown(void) {
    naab_gov_engine_t eng = naab_gov_create();
    naab_gov_load_config_string(eng, TEST_CONFIG);

    char* result = naab_gov_check(eng, "nonexistent_check", "python", "x = 1", 1);
    ASSERT(result == NULL);

    const char* err = naab_gov_last_error(eng);
    ASSERT(strstr(err, "Unknown check") != NULL);

    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: reset clears results --- */
static int test_reset(void) {
    naab_gov_engine_t eng = naab_gov_create();
    naab_gov_load_config_string(eng, TEST_CONFIG);

    char* result = naab_gov_scan(eng, "python",
        "eval('dangerous')", "test.py", 1);
    naab_gov_free_string(result);
    ASSERT(naab_gov_result_count(eng) > 0);

    naab_gov_reset(eng);
    ASSERT(naab_gov_result_count(eng) == 0);

    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: summary string --- */
static int test_summary(void) {
    naab_gov_engine_t eng = naab_gov_create();
    naab_gov_load_config_string(eng, TEST_CONFIG);

    char* result = naab_gov_scan(eng, "python", "x = 1", "test.py", 1);
    naab_gov_free_string(result);

    char* summary = naab_gov_summary(eng);
    ASSERT(summary != NULL);

    naab_gov_free_string(summary);
    naab_gov_destroy(eng);
    return 1;
}

/* --- Test: multiple independent engines --- */
static int test_multiple_engines(void) {
    naab_gov_engine_t eng1 = naab_gov_create();
    naab_gov_engine_t eng2 = naab_gov_create();
    ASSERT(eng1 != NULL);
    ASSERT(eng2 != NULL);
    ASSERT(eng1 != eng2);

    naab_gov_load_config_string(eng1, TEST_CONFIG);
    ASSERT(naab_gov_is_active(eng1) == 1);
    ASSERT(naab_gov_is_active(eng2) == 0);  /* eng2 has no config */

    naab_gov_destroy(eng1);
    naab_gov_destroy(eng2);
    return 1;
}

/* --- Single checks read what the block path reads ---
 *
 * naab_gov_check() runs one named check. Until it shared the block path's
 * dispatch, it handed every check the code with string literals removed, so a
 * check that looks INSIDE strings (secrets, SQL) or at a module name (imports)
 * passed code naab_gov_scan() blocks. Each arm runs the same input through
 * naab_gov_scan() first: that is the control that the config and input really
 * are a violation. Each check also gets a safe input that must pass both ways,
 * so a check that refuses everything fails here. */

/* Each config enables only the check under test, so the scan control can only
 * have blocked for that check. */
#define CFG(body) "{\"version\":\"3.0\",\"mode\":\"enforce\"," body "}"

static int blocked_in(const char* json) {
    return json != NULL && strstr(json, "\"blocked\":true") != NULL;
}

/* 1 when scan and check agree with `expect_blocked`, else 0 (printing which). */
static int scan_and_check(const char* cfg, const char* check, const char* lang,
                          const char* code, int expect_blocked) {
    naab_gov_engine_t eng = naab_gov_create();
    if (naab_gov_load_config_string(eng, cfg) != NAAB_GOV_OK) {
        printf("(config did not load: %s) ", naab_gov_last_error(eng));
        naab_gov_destroy(eng);
        return 0;
    }
    char* s = naab_gov_scan(eng, lang, code, "t", 1);
    int scan_blocked = blocked_in(s);
    naab_gov_free_string(s);
    naab_gov_destroy(eng);

    eng = naab_gov_create();
    naab_gov_load_config_string(eng, cfg);
    char* c = naab_gov_check(eng, check, lang, code, 1);
    int check_ran = c != NULL;
    int check_blocked = blocked_in(c);
    naab_gov_free_string(c);
    naab_gov_destroy(eng);

    if (scan_blocked != expect_blocked) {
        printf("(scan control: blocked=%d, expected %d) ", scan_blocked, expect_blocked);
        return 0;
    }
    if (!check_ran) {
        printf("(check '%s' returned NULL) ", check);
        return 0;
    }
    if (check_blocked != expect_blocked) {
        printf("(check '%s': blocked=%d, scan blocked=%d) ", check, check_blocked, scan_blocked);
        return 0;
    }
    return 1;
}

static const char* SQL_CFG =
    CFG("\"code_quality\":{\"no_sql_injection\":{\"level\":\"hard\"}}");
static const char* IMPORTS_CFG =
    CFG("\"restrictions\":{\"imports\":{\"level\":\"hard\","
        "\"blocked\":{\"javascript\":[\"child_process\"]}}}");
static const char* PRIV_CFG =
    CFG("\"restrictions\":{\"privilege_escalation\":{\"level\":\"hard\"}}");
static const char* CRYPTO_CFG =
    CFG("\"restrictions\":{\"crypto\":{\"level\":\"hard\"}}");
static const char* INJECTION_CFG =
    CFG("\"restrictions\":{\"code_injection\":{\"level\":\"hard\"}}");
static const char* DANGEROUS_CFG =
    CFG("\"restrictions\":{\"dangerous_calls\":{\"level\":\"hard\"}}");

/* SQL lives in a string literal: the check reads the raw code. */
static int test_check_reads_raw_sql(void) {
    ASSERT(scan_and_check(SQL_CFG, "sql_injection", "python",
        "q = \"SELECT * FROM t WHERE id=\" + uid\n", 1));
    ASSERT(scan_and_check(SQL_CFG, "sql_injection", "python",
        "q = \"SELECT 1\"\n", 0));
    return 1;
}

/* A module name is a string: require("child_process"). */
static int test_check_reads_module_names(void) {
    ASSERT(scan_and_check(IMPORTS_CFG, "imports", "javascript",
        "const cp = require(\"child_process\");\n", 1));
    ASSERT(scan_and_check(IMPORTS_CFG, "imports", "javascript",
        "const fs = require(\"fs\");\n", 0));
    return 1;
}

/* The language name is normalized as in a block: js is javascript. */
static int test_check_normalizes_language(void) {
    ASSERT(scan_and_check(IMPORTS_CFG, "imports", "js",
        "const cp = require(\"child_process\");\n", 1));
    return 1;
}

/* sudo inside the string an exec function runs: read on the normalized text. */
static int test_check_privilege_in_string(void) {
    ASSERT(scan_and_check(PRIV_CFG, "privilege_escalation", "python",
        "import os\nos.system(\"sudo id\")\n", 1));
    ASSERT(scan_and_check(PRIV_CFG, "privilege_escalation", "python",
        "import os\nos.system(\"id\")\n", 0));
    return 1;
}

/* A key assigned a literal: the quotes are what the pattern needs. */
static int test_check_crypto_hardcoded_key(void) {
    ASSERT(scan_and_check(CRYPTO_CFG, "crypto_weakness", "python",
        "signing_key = \"abc123\"\n", 1));
    ASSERT(scan_and_check(CRYPTO_CFG, "crypto_weakness", "python",
        "signing_key = load()\n", 0));
    return 1;
}

/* code_injection's SQL patterns read the normalized text. */
static int test_check_injection_sql_in_string(void) {
    ASSERT(scan_and_check(INJECTION_CFG, "code_injection", "python",
        "q = \"SELECT * FROM t WHERE id=\" + uid\n", 1));
    ASSERT(scan_and_check(INJECTION_CFG, "code_injection", "python",
        "q = \"SELECT 1\"\n", 0));
    return 1;
}

/* preg_replace's /e modifier is inside the pattern string. */
static int test_check_dangerous_in_string(void) {
    ASSERT(scan_and_check(DANGEROUS_CFG, "dangerous_calls", "php",
        "<?php preg_replace('/x/e', $r, $s); ?>\n", 1));
    ASSERT(scan_and_check(DANGEROUS_CFG, "dangerous_calls", "php",
        "<?php preg_replace('/x/', $r, $s); ?>\n", 0));
    return 1;
}

int main(void) {
    printf("=== NAAb Governance C API Tests ===\n\n");

    TEST(lifecycle);
    TEST(destroy_null);
    TEST(version);
    TEST(load_config_string);
    TEST(load_invalid_config);
    TEST(null_args);
    TEST(scan_clean);
    TEST(scan_eval_blocked);
    TEST(scan_os_system_blocked);
    TEST(json_report);
    TEST(sarif_report);
    TEST(check_secrets);
    TEST(check_unknown);
    TEST(reset);
    TEST(summary);
    TEST(multiple_engines);
    TEST(check_reads_raw_sql);
    TEST(check_reads_module_names);
    TEST(check_normalizes_language);
    TEST(check_privilege_in_string);
    TEST(check_crypto_hardcoded_key);
    TEST(check_injection_sql_in_string);
    TEST(check_dangerous_in_string);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
