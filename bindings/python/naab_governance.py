"""
NAAb Governance — Python Bindings

ctypes wrapper for libnaab-governance, the C API shared library.
Falls back to subprocess mode (naab-gov CLI) when no shared library
is available (e.g., on ARM64 Android/bionic where only static builds work).

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
import shutil
import subprocess
import sys
from pathlib import Path


class GovernanceViolation(Exception):
    """Raised when a governance scan blocks code execution."""

    def __init__(self, result: dict):
        self.result = result
        super().__init__(result.get("error", "Governance violation"))


_SHARED_LIB_NAMES = ("libnaab-governance.so", "libnaab-governance.dylib",
                      "naab-governance.dll")


def _find_shared_library():
    """Locate libnaab-governance shared library. Returns path or None."""
    # 1. Environment variable override
    env_path = os.environ.get("NAAB_GOV_LIB")
    if env_path and os.path.isfile(env_path):
        return env_path

    # 2. Alongside this Python file
    this_dir = Path(__file__).parent
    for name in _SHARED_LIB_NAMES:
        candidate = this_dir / name
        if candidate.is_file():
            return str(candidate)

    # 3. Build directory (development workflow)
    for build_dir in [
        this_dir.parent.parent / "build",
        Path.home() / ".naab" / "language" / "build",
    ]:
        for name in _SHARED_LIB_NAMES:
            candidate = build_dir / name
            if candidate.is_file():
                return str(candidate)

    # 4. System library path
    found = ctypes.util.find_library("naab-governance")
    if found:
        return found

    return None


def _find_cli_binary():
    """Locate naab-gov CLI binary for subprocess fallback. Returns path or None."""
    # 1. Environment variable override
    env_path = os.environ.get("NAAB_GOV_BIN")
    if env_path and os.path.isfile(env_path):
        return env_path

    # 2. Build directory
    this_dir = Path(__file__).parent
    for build_dir in [
        this_dir.parent.parent / "build",
        Path.home() / ".naab" / "language" / "build",
    ]:
        candidate = build_dir / "naab-gov"
        if candidate.is_file():
            return str(candidate)

    # 3. PATH
    found = shutil.which("naab-gov")
    if found:
        return found

    return None


class GovernanceEngine:
    """Python wrapper for the NAAb governance scanner C API.

    Each instance is independent — create multiple engines for
    concurrent scanning with different policies.

    Falls back to subprocess mode (naab-gov CLI) when the shared
    library is not available (e.g., static-only builds on ARM64 Android).
    """

    def __init__(self, lib_path: str = None):
        """Create a new governance engine.

        Args:
            lib_path: Optional explicit path to libnaab-governance.
                      If None, searches standard locations.
                      Falls back to subprocess mode if no shared library found.
        """
        self._subprocess_mode = False
        self._subprocess_bin = None
        self._subprocess_config = None
        self._last_result = {}

        path = lib_path or _find_shared_library()
        if path:
            try:
                self._lib = ctypes.CDLL(path)
                self._setup_functions()
                self._handle = self._lib.naab_gov_create()
                if not self._handle:
                    raise MemoryError("Failed to create governance engine")
                # Save destroy function reference for safe __del__ (Fix #5:
                # prevents segfault if _lib is GC'd before this object)
                self._destroy_fn = self._lib.naab_gov_destroy
                return
            except OSError:
                pass  # Fall through to subprocess mode

        # Subprocess fallback: use naab-gov CLI binary
        cli = _find_cli_binary()
        if not cli:
            raise FileNotFoundError(
                "Cannot find libnaab-governance shared library or naab-gov CLI. "
                "Set NAAB_GOV_LIB or NAAB_GOV_BIN environment variable, or build with: "
                "cd build && cmake .. && make naab_governance naab-gov"
            )
        self._subprocess_mode = True
        self._subprocess_bin = cli
        self._lib = None
        self._handle = None
        self._destroy_fn = None

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

        # Scanning — C8 fix: use c_void_p instead of c_char_p for owned strings
        # to prevent ctypes from auto-converting and leaking the malloc'd pointer.
        # We manually copy and free via naab_gov_free_string.
        lib.naab_gov_scan.restype = ctypes.c_void_p
        lib.naab_gov_scan.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                       ctypes.c_char_p, ctypes.c_char_p,
                                       ctypes.c_int]

        lib.naab_gov_check.restype = ctypes.c_void_p
        lib.naab_gov_check.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                        ctypes.c_char_p, ctypes.c_char_p,
                                        ctypes.c_int]

        # Results
        lib.naab_gov_was_blocked.restype = ctypes.c_int
        lib.naab_gov_was_blocked.argtypes = [ctypes.c_void_p]

        lib.naab_gov_json_report.restype = ctypes.c_void_p
        lib.naab_gov_json_report.argtypes = [ctypes.c_void_p]

        lib.naab_gov_sarif_report.restype = ctypes.c_void_p
        lib.naab_gov_sarif_report.argtypes = [ctypes.c_void_p]

        lib.naab_gov_summary.restype = ctypes.c_void_p
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
        if hasattr(self, "_destroy_fn") and self._destroy_fn and self._handle:
            self._destroy_fn(self._handle)
            self._handle = None

    def _check_error(self, rc: int, operation: str):
        """Check return code and raise on failure."""
        if rc != 0:
            err = self._lib.naab_gov_last_error(self._handle)
            msg = err.decode("utf-8", errors="replace") if err else "Unknown error"
            raise RuntimeError(f"{operation} failed: {msg}")

    def _owned_string(self, ptr):
        """C8 fix: safely extract a malloc'd C string and free it.
        Returns the string as Python str, or None if ptr is NULL/0."""
        if not ptr:
            return None
        try:
            raw = ctypes.string_at(ptr)
            return raw.decode("utf-8", errors="replace")
        finally:
            self._lib.naab_gov_free_string(ctypes.cast(ptr, ctypes.c_char_p))

    def _encode(self, s: str) -> bytes:
        return s.encode("utf-8") if isinstance(s, str) else s

    def _subprocess_scan(self, language: str, code: str, config=None) -> dict:
        """Run governance scan via naab-gov CLI subprocess."""
        cmd = [self._subprocess_bin, "check", "--language", language]
        if config:
            # Pass config inline via --config-string to avoid signature
            # verification issues with temp files (Ed25519 .sig required
            # when trusted keys are installed)
            cfg_json = json.dumps(
                config if isinstance(config, dict) else json.loads(config)
            )
            cmd.extend(["--config-string", cfg_json])
        proc = subprocess.run(
            cmd, input=code, capture_output=True, text=True, timeout=30
        )
        if proc.returncode == 1:
            raise RuntimeError(f"naab-gov check failed: {proc.stderr.strip()}")
        if proc.returncode == 4:
            raise RuntimeError(f"naab-gov config error: {proc.stderr.strip()}")
        try:
            result = json.loads(proc.stdout)
        except json.JSONDecodeError:
            raise RuntimeError(f"naab-gov returned invalid JSON: {proc.stdout[:200]}")
        self._last_result = result
        return result

    # --- Configuration ---

    def load_config(self, path: str):
        """Load governance policy from a govern.json file."""
        if self._subprocess_mode:
            with open(path) as f:
                self._subprocess_config = json.load(f)
            return
        rc = self._lib.naab_gov_load_config(self._handle, self._encode(path))
        self._check_error(rc, "load_config")

    def discover_config(self, directory: str = "."):
        """Walk up from directory to find and load govern.json."""
        if self._subprocess_mode:
            # Walk up to find govern.json, same as C++ discoverAndLoad
            d = Path(directory).resolve()
            while True:
                candidate = d / "govern.json"
                if candidate.is_file():
                    with open(candidate) as f:
                        self._subprocess_config = json.load(f)
                    return
                parent = d.parent
                if parent == d:
                    break
                d = parent
            raise RuntimeError("discover_config failed: govern.json not found")
        rc = self._lib.naab_gov_discover_config(self._handle, self._encode(directory))
        self._check_error(rc, "discover_config")

    def load_config_dict(self, config: dict):
        """Load governance policy from a Python dict."""
        if self._subprocess_mode:
            self._subprocess_config = config
            return
        json_str = json.dumps(config)
        rc = self._lib.naab_gov_load_config_string(self._handle,
                                                     self._encode(json_str))
        self._check_error(rc, "load_config_dict")

    def load_config_string(self, json_config: str):
        """Load governance policy from a JSON string."""
        if self._subprocess_mode:
            self._subprocess_config = json.loads(json_config)
            return
        rc = self._lib.naab_gov_load_config_string(self._handle,
                                                     self._encode(json_config))
        self._check_error(rc, "load_config_string")

    @property
    def is_active(self) -> bool:
        """True if a config is loaded and active."""
        if self._subprocess_mode:
            return self._subprocess_config is not None
        return self._lib.naab_gov_is_active(self._handle) == 1

    # --- Scanning ---

    def scan(self, language: str, code: str,
             source_file: str = "", start_line: int = 1) -> dict:
        """Scan a code block for governance violations.

        Returns a dict with keys: blocked (bool), error (str), report (dict).
        """
        if self._subprocess_mode:
            return self._subprocess_scan(language, code, self._subprocess_config)
        raw_ptr = self._lib.naab_gov_scan(
            self._handle,
            self._encode(language),
            self._encode(code),
            self._encode(source_file),
            start_line
        )
        raw_str = self._owned_string(raw_ptr)
        if raw_str is None:
            err = self._lib.naab_gov_last_error(self._handle)
            raise RuntimeError(
                f"scan failed: {err.decode('utf-8', errors='replace') if err else 'NULL result'}"
            )
        return json.loads(raw_str)

    def check(self, check_name: str, language: str, code: str,
              start_line: int = 1) -> dict:
        """Run a single named check against code.

        Valid check names: secrets, code_injection, sql_injection,
        dangerous_calls, shell_injection, imports, obfuscation,
        deserialization, privilege_escalation, oversimplification,
        incomplete_logic, encoding, path_traversal, data_exfiltration,
        crypto_weakness.
        """
        if self._subprocess_mode:
            # Subprocess mode doesn't support single-check; run full scan
            return self._subprocess_scan(language, code, self._subprocess_config)
        raw_ptr = self._lib.naab_gov_check(
            self._handle,
            self._encode(check_name),
            self._encode(language),
            self._encode(code),
            start_line
        )
        raw_str = self._owned_string(raw_ptr)
        if raw_str is None:
            err = self._lib.naab_gov_last_error(self._handle)
            raise RuntimeError(
                f"check failed: {err.decode('utf-8', errors='replace') if err else 'NULL result'}"
            )
        return json.loads(raw_str)

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
        if self._subprocess_mode:
            return self._last_result.get("blocked", False)
        return self._lib.naab_gov_was_blocked(self._handle) == 1

    @property
    def json_report(self) -> dict:
        """Full JSON report from the last scan."""
        if self._subprocess_mode:
            return self._last_result
        raw_str = self._owned_string(self._lib.naab_gov_json_report(self._handle))
        if raw_str is None:
            return {}
        return json.loads(raw_str)

    @property
    def sarif_report(self) -> dict:
        """SARIF report from the last scan."""
        if self._subprocess_mode:
            return {}  # SARIF not available in subprocess mode
        raw_str = self._owned_string(self._lib.naab_gov_sarif_report(self._handle))
        if raw_str is None:
            return {}
        return json.loads(raw_str)

    @property
    def summary(self) -> str:
        """Human-readable summary from the last scan."""
        if self._subprocess_mode:
            r = self._last_result
            vc = r.get("violation_count", 0)
            return f"{'BLOCKED' if r.get('blocked') else 'CLEAN'}: {vc} violation(s)"
        raw_str = self._owned_string(self._lib.naab_gov_summary(self._handle))
        return raw_str if raw_str else ""

    @property
    def result_count(self) -> int:
        """Number of check results from the last scan."""
        if self._subprocess_mode:
            return self._last_result.get("violation_count", 0)
        return self._lib.naab_gov_result_count(self._handle)

    def reset(self):
        """Clear results for next scan."""
        if self._subprocess_mode:
            self._last_result = {}
            return
        self._lib.naab_gov_reset(self._handle)

    # --- Info ---

    @property
    def last_error(self) -> str:
        """Last error message (empty if none)."""
        if self._subprocess_mode:
            return ""
        raw = self._lib.naab_gov_last_error(self._handle)
        return raw.decode("utf-8", errors="replace") if raw else ""

    @property
    def version(self) -> str:
        """Library version string."""
        if self._subprocess_mode:
            try:
                proc = subprocess.run(
                    [self._subprocess_bin, "--version"],
                    capture_output=True, text=True, timeout=5
                )
                return proc.stdout.strip().replace("naab-gov ", "")
            except (subprocess.SubprocessError, OSError):
                return "unknown"
        raw = self._lib.naab_gov_version_string()
        return raw.decode("utf-8") if raw else ""
