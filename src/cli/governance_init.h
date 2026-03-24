#pragma once
// governance_init.h — Interactive governance config generator for `naab-lang init`

#include <string>
#include <vector>

namespace naab { namespace cli {

enum class StrictnessPreset { RELAXED, STANDARD, STRICT, PARANOID };
enum class ProjectType { APPLICATION, LIBRARY, SCRIPT, TEST_SUITE };
enum class CapabilityLevel { MINIMAL, STANDARD, FULL };

struct GovernanceInitConfig {
    std::vector<std::string> languages = {"python", "shell"};
    StrictnessPreset strictness = StrictnessPreset::STANDARD;
    bool taint_enabled = true;
    std::vector<std::string> taint_sources;
    std::vector<std::string> taint_sinks;
    ProjectType project_type = ProjectType::APPLICATION;
    CapabilityLevel capabilities = CapabilityLevel::STANDARD;
    bool scanner_enabled = true;
    bool contracts_enabled = false;
};

// Interactive flow (reads from stdin). If !is_tty, uses all defaults silently.
GovernanceInitConfig runInteractiveSetup(bool is_tty);

// Build config from a named preset (relaxed/standard/strict/paranoid)
GovernanceInitConfig presetToConfig(const std::string& preset,
                                     const std::string& languages_override,
                                     bool taint_flag);

// Generate complete govern.json content from config (all 83 sections)
std::string generateGovernJson(const GovernanceInitConfig& config);

// Write govern.json to disk. Returns true on success.
bool writeGovernJson(const std::string& path,
                     const GovernanceInitConfig& config,
                     bool force);

}} // namespace naab::cli
