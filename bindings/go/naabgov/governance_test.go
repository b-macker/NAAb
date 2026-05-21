package naabgov

import "testing"

const testConfig = `{
	"version": "3.0",
	"mode": "enforce",
	"restrictions": {
		"dangerous_calls": {"level": "hard"},
		"code_injection": {"level": "hard"}
	},
	"code_quality": {
		"no_secrets": {"level": "hard"}
	}
}`

func TestVersion(t *testing.T) {
	v := Version()
	if v == "" {
		t.Error("version string should not be empty")
	}
}

func TestLifecycle(t *testing.T) {
	eng, err := NewEngine()
	if err != nil {
		t.Fatal(err)
	}
	eng.Close()
	eng.Close() // double close should be safe
}

func TestScanSafeCode(t *testing.T) {
	eng, err := NewEngine()
	if err != nil {
		t.Fatal(err)
	}
	defer eng.Close()

	if err := eng.LoadConfigString(testConfig); err != nil {
		t.Fatal(err)
	}
	if !eng.IsActive() {
		t.Error("engine should be active after loading config")
	}

	result, err := eng.Scan("python", "x = 42\nprint(x)", "test.py", 1)
	if err != nil {
		t.Fatal(err)
	}
	if result.Blocked {
		t.Error("safe code should not be blocked")
	}
}

func TestScanDangerousCode(t *testing.T) {
	eng, err := NewEngine()
	if err != nil {
		t.Fatal(err)
	}
	defer eng.Close()

	if err := eng.LoadConfigString(testConfig); err != nil {
		t.Fatal(err)
	}

	result, err := eng.Scan("python", "import os; os.system('rm -rf /')", "test.py", 1)
	if err != nil {
		t.Fatal(err)
	}
	if !result.Blocked {
		t.Error("dangerous code should be blocked")
	}
}

func TestReset(t *testing.T) {
	eng, err := NewEngine()
	if err != nil {
		t.Fatal(err)
	}
	defer eng.Close()

	if err := eng.LoadConfigString(testConfig); err != nil {
		t.Fatal(err)
	}

	eng.Scan("python", "eval(input())", "test.py", 1)
	if eng.ResultCount() == 0 {
		t.Error("should have results after scan")
	}

	eng.Reset()
	if eng.ResultCount() != 0 {
		t.Error("should have no results after reset")
	}
}
