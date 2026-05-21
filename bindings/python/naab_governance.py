"""
NAAb Governance — Python Bindings

ctypes wrapper for libnaab-governance, the C API shared library.
Provides in-process governance scanning for AI agent frameworks.

Usage:
    from naab_governance import GovernanceEngine

    engine = GovernanceEngine()
    engine.load_config("govern.json")
    result = engine.scan("python", "import os; os.system('rm -rf /')", "agent.py")
    if result["blocked"]:
        print("Blocked:", result["error"])
"""

import ctypes
import ctypes.util
import json
import os
import sys
from pathlib import Path


class GovernanceViolation(Exception):
    """Raised when a governance scan blocks code execution."""

    def __init__(self, result: dict):
        self.result = result
        super().__init__(result.get("error", "Governance violation"))


def _find_library() -> str:
    """Locate libnaab-governance shared/static library."""
    # 1. Environment variable override
    env_path = os.environ.get("NAAB_GOV_LIB")
    if env_path and os.path.isfile(env_path):
        return env_path

    # 2. Alongside this Python file
    this_dir = Path(__file__).parent
    for name in ("libnaab-governance.so", "libnaab-governance.dylib",
                 "naab-governance.dll"):
        candidate = this_dir / name
        if candidate.is_file():
            return str(candidate)

    # 3. Build directory (development workflow)
    for build_dir in [
        this_dir.parent.parent / "build",
        Path.home() / ".naab" / "language" / "build",
    ]:
        for name in ("libnaab-governance.so", "libnaab-governance.dylib",
                     "naab-governance.dll"):
            candidate = build_dir / name
            if candidate.is_file():
                return str(candidate)

    # 4. System library path
    found = ctypes.util.find_library("naab-governance")
    if found:
        return found

    raise FileNotFoundError(
        "Cannot find libnaab-governance. Set NAAB_GOV_LIB environment variable "
        "or place the library alongside this file. Build with: "
        "cd build && cmake .. && make naab_governance"
    )


class GovernanceEngine:
    """Python wrapper for the NAAb governance scanner C API.

    Each instance is independent — create multiple engines for
    concurrent scanning with different policies.
    """

    def __init__(self, lib_path: str = None):
        """Create a new governance engine.

        Args:
            lib_path: Optional explicit path to libnaab-governance.
                      If None, searches standard locations.
        """
        path = lib_path or _find_library()
        self._lib = ctypes.CDLL(path)
        self._setup_functions()

        self._handle = self._lib.naab_gov_create()
        if not self._handle:
            raise MemoryError("Failed to create governance engine")

    def _setup_functions(self):
        """Configure ctypes function signatures."""
        lib = self._lib

        # Lifecycle
        lib.naab_gov_create.restype = ctypes.c_void_p
        lib.naab_gov_create.argtypes = []

        lib.naab_gov_destroy.restype = None
        lib.naab_gov_destroy.argtypes = [ctypes.c_void_p]

        # Config
        lib.naab_gov_load_config.restype = ctypes.c_int
        lib.naab_gov_load_config.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

        lib.naab_gov_discover_config.restype = ctypes.c_int
        lib.naab_gov_discover_config.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

        lib.naab_gov_load_config_string.restype = ctypes.c_int
        lib.naab_gov_load_config_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

        lib.naab_gov_is_active.restype = ctypes.c_int
        lib.naab_gov_is_active.argtypes = [ctypes.c_void_p]

        # Scanning
        lib.naab_gov_scan.restype = ctypes.c_char_p
        lib.naab_gov_scan.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                       ctypes.c_char_p, ctypes.c_char_p,
                                       ctypes.c_int]

        lib.naab_gov_check.restype = ctypes.c_char_p
        lib.naab_gov_check.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                        ctypes.c_char_p, ctypes.c_char_p,
                                        ctypes.c_int]

        # Results
        lib.naab_gov_was_blocked.restype = ctypes.c_int
        lib.naab_gov_was_blocked.argtypes = [ctypes.c_void_p]

        lib.naab_gov_json_report.restype = ctypes.c_char_p
        lib.naab_gov_json_report.argtypes = [ctypes.c_void_p]

        lib.naab_gov_sarif_report.restype = ctypes.c_char_p
        lib.naab_gov_sarif_report.argtypes = [ctypes.c_void_p]

        lib.naab_gov_summary.restype = ctypes.c_char_p
        lib.naab_gov_summary.argtypes = [ctypes.c_void_p]

        lib.naab_gov_result_count.restype = ctypes.c_int
        lib.naab_gov_result_count.argtypes = [ctypes.c_void_p]

        lib.naab_gov_reset.restype = None
        lib.naab_gov_reset.argtypes = [ctypes.c_void_p]

        # Error
        lib.naab_gov_last_error.restype = ctypes.c_char_p
        lib.naab_gov_last_error.argtypes = [ctypes.c_void_p]

        # Memory
        lib.naab_gov_free_string.restype = None
        lib.naab_gov_free_string.argtypes = [ctypes.c_char_p]

        # Version
        lib.naab_gov_version_string.restype = ctypes.c_char_p
        lib.naab_gov_version_string.argtypes = []

    def __del__(self):
        if hasattr(self, "_handle") and self._handle:
            self._lib.naab_gov_destroy(self._handle)
            self._handle = None

    def _check_error(self, rc: int, operation: str):
        """Check return code and raise on failure."""
        if rc != 0:
            err = self._lib.naab_gov_last_error(self._handle)
            msg = err.decode("utf-8", errors="replace") if err else "Unknown error"
            raise RuntimeError(f"{operation} failed: {msg}")

    def _encode(self, s: str) -> bytes:
        return s.encode("utf-8") if isinstance(s, str) else s

    # --- Configuration ---

    def load_config(self, path: str):
        """Load governance policy from a govern.json file."""
        rc = self._lib.naab_gov_load_config(self._handle, self._encode(path))
        self._check_error(rc, "load_config")

    def discover_config(self, directory: str = "."):
        """Walk up from directory to find and load govern.json."""
        rc = self._lib.naab_gov_discover_config(self._handle, self._encode(directory))
        self._check_error(rc, "discover_config")

    def load_config_dict(self, config: dict):
        """Load governance policy from a Python dict."""
        json_str = json.dumps(config)
        rc = self._lib.naab_gov_load_config_string(self._handle,
                                                     self._encode(json_str))
        self._check_error(rc, "load_config_dict")

    def load_config_string(self, json_config: str):
        """Load governance policy from a JSON string."""
        rc = self._lib.naab_gov_load_config_string(self._handle,
                                                     self._encode(json_config))
        self._check_error(rc, "load_config_string")

    @property
    def is_active(self) -> bool:
        """True if a config is loaded and active."""
        return self._lib.naab_gov_is_active(self._handle) == 1

    # --- Scanning ---

    def scan(self, language: str, code: str,
             source_file: str = "", start_line: int = 1) -> dict:
        """Scan a code block for governance violations.

        Returns a dict with keys: blocked (bool), error (str), report (dict).
        """
        raw = self._lib.naab_gov_scan(
            self._handle,
            self._encode(language),
            self._encode(code),
            self._encode(source_file),
            start_line
        )
        if not raw:
            err = self._lib.naab_gov_last_error(self._handle)
            raise RuntimeError(
                f"scan failed: {err.decode('utf-8', errors='replace') if err else 'NULL result'}"
            )
        result = json.loads(raw.decode("utf-8"))
        return result

    def check(self, check_name: str, language: str, code: str,
              start_line: int = 1) -> dict:
        """Run a single named check against code.

        Valid check names: secrets, code_injection, sql_injection,
        dangerous_calls, shell_injection, imports, obfuscation,
        deserialization, privilege_escalation, oversimplification,
        incomplete_logic, encoding, path_traversal, data_exfiltration,
        crypto_weakness.
        """
        raw = self._lib.naab_gov_check(
            self._handle,
            self._encode(check_name),
            self._encode(language),
            self._encode(code),
            start_line
        )
        if not raw:
            err = self._lib.naab_gov_last_error(self._handle)
            raise RuntimeError(
                f"check failed: {err.decode('utf-8', errors='replace') if err else 'NULL result'}"
            )
        return json.loads(raw.decode("utf-8"))

    def scan_or_raise(self, language: str, code: str,
                      source_file: str = "", start_line: int = 1) -> dict:
        """Scan code and raise GovernanceViolation if blocked."""
        result = self.scan(language, code, source_file, start_line)
        if result.get("blocked"):
            raise GovernanceViolation(result)
        return result

    # --- Results ---

    @property
    def was_blocked(self) -> bool:
        """True if the last scan had a HARD block."""
        return self._lib.naab_gov_was_blocked(self._handle) == 1

    @property
    def json_report(self) -> dict:
        """Full JSON report from the last scan."""
        raw = self._lib.naab_gov_json_report(self._handle)
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))

    @property
    def sarif_report(self) -> dict:
        """SARIF report from the last scan."""
        raw = self._lib.naab_gov_sarif_report(self._handle)
        if not raw:
            return {}
        return json.loads(raw.decode("utf-8"))

    @property
    def summary(self) -> str:
        """Human-readable summary from the last scan."""
        raw = self._lib.naab_gov_summary(self._handle)
        return raw.decode("utf-8") if raw else ""

    @property
    def result_count(self) -> int:
        """Number of check results from the last scan."""
        return self._lib.naab_gov_result_count(self._handle)

    def reset(self):
        """Clear results for next scan."""
        self._lib.naab_gov_reset(self._handle)

    # --- Info ---

    @property
    def last_error(self) -> str:
        """Last error message (empty if none)."""
        raw = self._lib.naab_gov_last_error(self._handle)
        return raw.decode("utf-8", errors="replace") if raw else ""

    @property
    def version(self) -> str:
        """Library version string."""
        raw = self._lib.naab_gov_version_string()
        return raw.decode("utf-8") if raw else ""
