using System;
using System.Runtime.InteropServices;

namespace NaabGovernance
{
    /// <summary>
    /// P/Invoke declarations for libnaab-governance C API.
    /// </summary>
    internal static class NativeMethods
    {
        private const string Lib = "naab-governance";

        [DllImport(Lib)]
        internal static extern IntPtr naab_gov_create();

        [DllImport(Lib)]
        internal static extern void naab_gov_destroy(IntPtr engine);

        [DllImport(Lib)]
        internal static extern int naab_gov_load_config(
            IntPtr engine,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string path);

        [DllImport(Lib)]
        internal static extern int naab_gov_discover_config(
            IntPtr engine,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string dir);

        [DllImport(Lib)]
        internal static extern int naab_gov_load_config_string(
            IntPtr engine,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string json);

        [DllImport(Lib)]
        internal static extern int naab_gov_is_active(IntPtr engine);

        [DllImport(Lib)]
        internal static extern IntPtr naab_gov_scan(
            IntPtr engine,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string language,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string code,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string sourceFile,
            int startLine);

        [DllImport(Lib)]
        internal static extern IntPtr naab_gov_check(
            IntPtr engine,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string checkName,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string language,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string code,
            int startLine);

        [DllImport(Lib)]
        internal static extern int naab_gov_was_blocked(IntPtr engine);

        [DllImport(Lib)]
        internal static extern IntPtr naab_gov_json_report(IntPtr engine);

        [DllImport(Lib)]
        internal static extern IntPtr naab_gov_sarif_report(IntPtr engine);

        [DllImport(Lib)]
        internal static extern IntPtr naab_gov_summary(IntPtr engine);

        [DllImport(Lib)]
        internal static extern int naab_gov_result_count(IntPtr engine);

        [DllImport(Lib)]
        internal static extern void naab_gov_reset(IntPtr engine);

        [DllImport(Lib)]
        internal static extern IntPtr naab_gov_last_error(IntPtr engine);

        [DllImport(Lib)]
        internal static extern void naab_gov_free_string(IntPtr str);

        [DllImport(Lib)]
        internal static extern IntPtr naab_gov_version_string();
    }
}
