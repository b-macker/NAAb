// Package naabgov provides Go bindings for the NAAb governance engine.
//
// The governance engine performs static analysis on code to detect security
// violations, dangerous patterns, and policy violations. It wraps the
// libnaab-governance C shared library via CGO.
//
// Usage:
//
//	engine, err := naabgov.NewEngine()
//	if err != nil { log.Fatal(err) }
//	defer engine.Close()
//
//	err = engine.LoadConfigString(`{"version":"3.0","mode":"enforce",...}`)
//	if err != nil { log.Fatal(err) }
//
//	result, err := engine.Scan("python", code, "source.py", 1)
//	if result.Blocked { fmt.Println("Code blocked by governance") }
package naabgov

/*
#cgo LDFLAGS: -lnaab-governance
#cgo CFLAGS: -I${SRCDIR}/../../../include/naab/public
#include "naab_governance.h"
#include <stdlib.h>
*/
import "C"
import (
	"encoding/json"
	"errors"
	"fmt"
	"runtime"
	"unsafe"
)

// Engine wraps a NAAb governance engine instance.
type Engine struct {
	handle C.naab_gov_engine_t
}

// ScanResult contains the result of a governance scan.
type ScanResult struct {
	Blocked bool            `json:"blocked"`
	Error   string          `json:"error"`
	Report  json.RawMessage `json:"report"`
}

// NewEngine creates a new governance engine instance.
func NewEngine() (*Engine, error) {
	h := C.naab_gov_create()
	if h == nil {
		return nil, errors.New("naabgov: failed to create governance engine")
	}
	e := &Engine{handle: h}
	runtime.SetFinalizer(e, (*Engine).Close)
	return e, nil
}

// Close destroys the engine and frees resources. Safe to call multiple times.
func (e *Engine) Close() {
	if e.handle != nil {
		C.naab_gov_destroy(e.handle)
		e.handle = nil
	}
}

// LoadConfig loads governance rules from a govern.json file.
func (e *Engine) LoadConfig(path string) error {
	cs := C.CString(path)
	defer C.free(unsafe.Pointer(cs))
	rc := C.naab_gov_load_config(e.handle, cs)
	if rc != C.NAAB_GOV_OK {
		return fmt.Errorf("naabgov: load config failed: %s", e.LastError())
	}
	return nil
}

// DiscoverConfig walks up from dir to find and load govern.json.
func (e *Engine) DiscoverConfig(dir string) error {
	cs := C.CString(dir)
	defer C.free(unsafe.Pointer(cs))
	rc := C.naab_gov_discover_config(e.handle, cs)
	if rc != C.NAAB_GOV_OK {
		return fmt.Errorf("naabgov: discover config failed: %s", e.LastError())
	}
	return nil
}

// LoadConfigString loads governance rules from a JSON string.
func (e *Engine) LoadConfigString(jsonConfig string) error {
	cs := C.CString(jsonConfig)
	defer C.free(unsafe.Pointer(cs))
	rc := C.naab_gov_load_config_string(e.handle, cs)
	if rc != C.NAAB_GOV_OK {
		return fmt.Errorf("naabgov: load config string failed: %s", e.LastError())
	}
	return nil
}

// IsActive returns true if governance rules have been loaded.
func (e *Engine) IsActive() bool {
	return C.naab_gov_is_active(e.handle) == 1
}

// Scan runs all governance checks on the given code.
// Returns a ScanResult with blocked status and full report.
func (e *Engine) Scan(language, code, sourceFile string, startLine int) (*ScanResult, error) {
	cLang := C.CString(language)
	cCode := C.CString(code)
	cFile := C.CString(sourceFile)
	defer C.free(unsafe.Pointer(cLang))
	defer C.free(unsafe.Pointer(cCode))
	defer C.free(unsafe.Pointer(cFile))

	raw := C.naab_gov_scan(e.handle, cLang, cCode, cFile, C.int(startLine))
	if raw == nil {
		return nil, fmt.Errorf("naabgov: scan failed: %s", e.LastError())
	}
	defer C.naab_gov_free_string(raw)

	var result ScanResult
	if err := json.Unmarshal([]byte(C.GoString(raw)), &result); err != nil {
		return nil, fmt.Errorf("naabgov: failed to parse scan result: %w", err)
	}
	return &result, nil
}

// WasBlocked returns true if the last scan had a HARD governance block.
func (e *Engine) WasBlocked() bool {
	return C.naab_gov_was_blocked(e.handle) == 1
}

// Reset clears check results for the next scan.
func (e *Engine) Reset() {
	C.naab_gov_reset(e.handle)
}

// ResultCount returns the number of check results from the last scan.
func (e *Engine) ResultCount() int {
	return int(C.naab_gov_result_count(e.handle))
}

// LastError returns the last error message (internal pointer, do not free).
func (e *Engine) LastError() string {
	p := C.naab_gov_last_error(e.handle)
	if p == nil {
		return ""
	}
	return C.GoString(p)
}

// Version returns the library version string.
func Version() string {
	return C.GoString(C.naab_gov_version_string())
}
