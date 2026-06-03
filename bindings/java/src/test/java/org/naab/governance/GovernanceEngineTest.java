package org.naab.governance;

/**
 * Tests for NAAb governance JNI bindings.
 *
 * Runs standalone (no JUnit dependency) — each test method prints PASS/FAIL.
 * Exit code 0 = all pass, 1 = any fail.
 */
public class GovernanceEngineTest {

    private static final String TEST_CONFIG =
        "{\"version\":\"3.0\",\"mode\":\"enforce\"," +
        "\"restrictions\":{\"dangerous_calls\":{\"level\":\"hard\"}," +
        "\"code_injection\":{\"level\":\"hard\"}}," +
        "\"code_quality\":{\"no_secrets\":{\"level\":\"hard\"}}}";

    private static int passed = 0;
    private static int failed = 0;

    private static void check(String name, boolean condition) {
        if (condition) {
            System.out.println("  PASS: " + name);
            passed++;
        } else {
            System.out.println("  FAIL: " + name);
            failed++;
        }
    }

    public static void main(String[] args) {
        System.out.println();
        System.out.println("--- Java JNI Binding Tests ---");
        System.out.println();

        testVersion();
        testLifecycle();
        testScanSafeCode();
        testScanDangerousCode();
        testReset();
        testIsActive();
        testResultCount();

        System.out.println();
        System.out.println("Results: " + passed + "/" + (passed + failed) + " passed");
        System.out.println();
        System.exit(failed > 0 ? 1 : 0);
    }

    private static void testVersion() {
        String v = GovernanceEngine.version();
        check("version not null", v != null);
        check("version not empty", v != null && !v.isEmpty());
    }

    private static void testLifecycle() {
        try (GovernanceEngine engine = new GovernanceEngine()) {
            check("create engine", engine != null);
        }
        // double close via try-with-resources then explicit — should not crash
        boolean doubleCloseOk = false;
        try {
            GovernanceEngine engine = new GovernanceEngine();
            engine.close();
            engine.close();
            doubleCloseOk = true;
        } catch (Exception e) {
            // native crash that propagates as Java exception
        }
        check("double close safe", doubleCloseOk);
    }

    private static void testScanSafeCode() {
        try (GovernanceEngine engine = new GovernanceEngine()) {
            engine.loadConfigString(TEST_CONFIG);
            check("isActive after config", engine.isActive());

            String result = engine.scan("python", "x = 42\nprint(x)", "test.py", 1);
            check("scan returns JSON", result != null && result.contains("blocked"));
            check("safe code not blocked", !engine.wasBlocked());
        }
    }

    private static void testScanDangerousCode() {
        try (GovernanceEngine engine = new GovernanceEngine()) {
            engine.loadConfigString(TEST_CONFIG);
            engine.scan("python", "import os; os.system('rm -rf /')", "test.py", 1);
            check("dangerous code blocked", engine.wasBlocked());
        }
    }

    private static void testReset() {
        try (GovernanceEngine engine = new GovernanceEngine()) {
            engine.loadConfigString(TEST_CONFIG);
            engine.scan("python", "eval(input())", "test.py", 1);
            check("has results after scan", engine.resultCount() > 0);

            engine.reset();
            check("no results after reset", engine.resultCount() == 0);
        }
    }

    private static void testIsActive() {
        try (GovernanceEngine engine = new GovernanceEngine()) {
            check("not active before config", !engine.isActive());
            engine.loadConfigString(TEST_CONFIG);
            check("active after config", engine.isActive());
        }
    }

    private static void testResultCount() {
        try (GovernanceEngine engine = new GovernanceEngine()) {
            engine.loadConfigString(TEST_CONFIG);
            check("zero results before scan", engine.resultCount() == 0);
            engine.scan("python", "eval(input())", "test.py", 1);
            check("has results after scan", engine.resultCount() > 0);
        }
    }
}
