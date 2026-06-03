use naab_governance::{GovernanceEngine, version};

const TEST_CONFIG: &str = r#"{
    "version": "3.0",
    "mode": "enforce",
    "restrictions": {
        "dangerous_calls": {"level": "hard"},
        "code_injection": {"level": "hard"}
    },
    "code_quality": {
        "no_secrets": {"level": "hard"}
    }
}"#;

#[test]
fn test_version() {
    let v = version();
    assert!(!v.is_empty(), "version string should not be empty");
}

#[test]
fn test_lifecycle() {
    let engine = GovernanceEngine::new().expect("create engine");
    drop(engine); // explicit drop should not panic
}

#[test]
fn test_scan_safe_code() {
    let engine = GovernanceEngine::new().unwrap();
    engine.load_config_string(TEST_CONFIG).unwrap();
    assert!(engine.is_active(), "engine should be active after loading config");

    let result = engine.scan("python", "x = 42\nprint(x)", "test.py", 1).unwrap();
    assert!(!result.blocked, "safe code should not be blocked");
}

#[test]
fn test_scan_dangerous_code() {
    let engine = GovernanceEngine::new().unwrap();
    engine.load_config_string(TEST_CONFIG).unwrap();

    let result = engine.scan("python", "import os; os.system('rm -rf /')", "test.py", 1).unwrap();
    assert!(result.blocked, "dangerous code should be blocked");
}

#[test]
fn test_reset() {
    let engine = GovernanceEngine::new().unwrap();
    engine.load_config_string(TEST_CONFIG).unwrap();

    engine.scan("python", "eval(input())", "test.py", 1).unwrap();
    assert!(engine.result_count() > 0, "should have results after scan");

    engine.reset();
    assert_eq!(engine.result_count(), 0, "should have no results after reset");
}

#[test]
fn test_is_active() {
    let engine = GovernanceEngine::new().unwrap();
    assert!(!engine.is_active(), "should not be active before loading config");

    engine.load_config_string(TEST_CONFIG).unwrap();
    assert!(engine.is_active(), "should be active after loading config");
}

#[test]
fn test_result_count() {
    let engine = GovernanceEngine::new().unwrap();
    engine.load_config_string(TEST_CONFIG).unwrap();

    assert_eq!(engine.result_count(), 0, "no results before scan");

    engine.scan("python", "eval(input())", "test.py", 1).unwrap();
    assert!(engine.result_count() > 0, "should have results after scanning dangerous code");
}
