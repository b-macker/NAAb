package org.naab.governance;

/**
 * Java bindings for the NAAb governance engine via JNI.
 *
 * <p>Provides static analysis of code to detect security violations,
 * dangerous patterns, and policy breaches.
 *
 * <pre>
 * try (GovernanceEngine engine = new GovernanceEngine()) {
 *     engine.loadConfigString("{\"version\":\"3.0\",\"mode\":\"enforce\",...}");
 *     String result = engine.scan("python", code, "source.py", 1);
 *     if (engine.wasBlocked()) {
 *         System.out.println("Code blocked by governance");
 *     }
 * }
 * </pre>
 */
public class GovernanceEngine implements AutoCloseable {

    static {
        System.loadLibrary("naab-governance-jni");
    }

    private long handle;

    /**
     * Creates a new governance engine instance.
     * @throws RuntimeException if the engine cannot be created
     */
    public GovernanceEngine() {
        this.handle = nativeCreate();
        if (this.handle == 0) {
            throw new RuntimeException("Failed to create NAAb governance engine");
        }
    }

    /**
     * Loads governance rules from a govern.json file.
     * @param path path to govern.json
     * @throws RuntimeException on load failure
     */
    public void loadConfig(String path) {
        int rc = nativeLoadConfig(handle, path);
        if (rc != 0) {
            throw new RuntimeException("Failed to load config: " + nativeLastError(handle));
        }
    }

    /**
     * Loads governance rules from a JSON string.
     * @param json governance configuration as JSON
     * @throws RuntimeException on parse failure
     */
    public void loadConfigString(String json) {
        int rc = nativeLoadConfigString(handle, json);
        if (rc != 0) {
            throw new RuntimeException("Failed to load config: " + nativeLastError(handle));
        }
    }

    /**
     * Returns true if governance rules have been loaded.
     */
    public boolean isActive() {
        return nativeIsActive(handle);
    }

    /**
     * Runs all governance checks on the given code.
     * @param language target language (e.g., "python", "javascript")
     * @param code source code to scan
     * @param sourceFile source file name for reporting
     * @param startLine starting line number
     * @return JSON string with scan results
     */
    public String scan(String language, String code, String sourceFile, int startLine) {
        return nativeScan(handle, language, code, sourceFile, startLine);
    }

    /**
     * Returns true if the last scan had a HARD governance block.
     */
    public boolean wasBlocked() {
        return nativeWasBlocked(handle);
    }

    /**
     * Clears check results for the next scan.
     */
    public void reset() {
        nativeReset(handle);
    }

    /**
     * Returns the number of check results from the last scan.
     */
    public int resultCount() {
        return nativeResultCount(handle);
    }

    /**
     * Returns the library version string.
     */
    public static String version() {
        return nativeVersion();
    }

    @Override
    public void close() {
        if (handle != 0) {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    // Native method declarations
    private static native long nativeCreate();
    private static native void nativeDestroy(long handle);
    private static native int nativeLoadConfig(long handle, String path);
    private static native int nativeLoadConfigString(long handle, String json);
    private static native boolean nativeIsActive(long handle);
    private static native String nativeScan(long handle, String language, String code,
                                            String sourceFile, int startLine);
    private static native boolean nativeWasBlocked(long handle);
    private static native void nativeReset(long handle);
    private static native int nativeResultCount(long handle);
    private static native String nativeLastError(long handle);
    private static native String nativeVersion();
}
