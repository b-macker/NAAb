using System;
using System.Runtime.InteropServices;

namespace NaabGovernance
{
    /// <summary>
    /// C# bindings for the NAAb governance engine.
    ///
    /// Provides static analysis of code to detect security violations,
    /// dangerous patterns, and policy breaches via P/Invoke to libnaab-governance.
    ///
    /// <example>
    /// <code>
    /// using var engine = new GovernanceEngine();
    /// engine.LoadConfigString(@"{""version"":""3.0"",""mode"":""enforce"",...}");
    /// string result = engine.Scan("python", code, "source.py", 1);
    /// if (engine.WasBlocked)
    ///     Console.WriteLine("Code blocked by governance");
    /// </code>
    /// </example>
    /// </summary>
    public class GovernanceEngine : IDisposable
    {
        private IntPtr _handle;

        /// <summary>Creates a new governance engine instance.</summary>
        public GovernanceEngine()
        {
            _handle = NativeMethods.naab_gov_create();
            if (_handle == IntPtr.Zero)
                throw new OutOfMemoryException("Failed to create NAAb governance engine");
        }

        /// <summary>Loads governance rules from a govern.json file.</summary>
        public void LoadConfig(string path)
        {
            int rc = NativeMethods.naab_gov_load_config(_handle, path);
            if (rc != 0)
                throw new InvalidOperationException($"Failed to load config: {GetLastError()}");
        }

        /// <summary>Loads governance rules from a JSON string.</summary>
        public void LoadConfigString(string json)
        {
            int rc = NativeMethods.naab_gov_load_config_string(_handle, json);
            if (rc != 0)
                throw new InvalidOperationException($"Failed to load config: {GetLastError()}");
        }

        /// <summary>Walks up from dir to find and load govern.json.</summary>
        public void DiscoverConfig(string dir)
        {
            int rc = NativeMethods.naab_gov_discover_config(_handle, dir);
            if (rc != 0)
                throw new InvalidOperationException($"Failed to discover config: {GetLastError()}");
        }

        /// <summary>Returns true if governance rules have been loaded.</summary>
        public bool IsActive => NativeMethods.naab_gov_is_active(_handle) == 1;

        /// <summary>Runs all governance checks on the given code.</summary>
        /// <returns>JSON string with scan results.</returns>
        public string Scan(string language, string code, string sourceFile = "", int startLine = 1)
        {
            IntPtr raw = NativeMethods.naab_gov_scan(_handle, language, code, sourceFile, startLine);
            if (raw == IntPtr.Zero)
                throw new InvalidOperationException($"Scan failed: {GetLastError()}");
            string result = Marshal.PtrToStringUTF8(raw)!;
            NativeMethods.naab_gov_free_string(raw);
            return result;
        }

        /// <summary>Returns true if the last scan had a HARD governance block.</summary>
        public bool WasBlocked => NativeMethods.naab_gov_was_blocked(_handle) == 1;

        /// <summary>Clears check results for the next scan.</summary>
        public void Reset() => NativeMethods.naab_gov_reset(_handle);

        /// <summary>Number of check results from the last scan.</summary>
        public int ResultCount => NativeMethods.naab_gov_result_count(_handle);

        /// <summary>Returns the library version string.</summary>
        public static string Version
        {
            get
            {
                IntPtr p = NativeMethods.naab_gov_version_string();
                return Marshal.PtrToStringUTF8(p) ?? "";
            }
        }

        private string GetLastError()
        {
            IntPtr p = NativeMethods.naab_gov_last_error(_handle);
            return p != IntPtr.Zero ? Marshal.PtrToStringUTF8(p) ?? "" : "";
        }

        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                NativeMethods.naab_gov_destroy(_handle);
                _handle = IntPtr.Zero;
            }
        }
    }
}
