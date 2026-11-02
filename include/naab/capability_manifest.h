#pragma once

// The signed capability manifest of one program (Bundle 11 PR 2,
// docs/plan-bundles.md).
//
// `naab-gov manifest infer PROGRAM` drafts `<stem>.manifest.json` beside the
// program from what it can see statically: the actions its stdlib calls need,
// the languages of its polyglot blocks, the hosts and files named by literal
// arguments, the agents it creates, and the NAAb modules it imports. A person
// reads the draft, edits it, and signs it with a trust-store key
// (`naab-gov manifest sign`). With `manifest.required: true` in govern.json a
// run then needs that manifest -- present, signed by a trusted Ed25519 key,
// for exactly this program text -- and the manifest NARROWS the run like a
// lease: every action, language, host, file, agent and module it does not
// list is refused at runtime, wherever the program reaches it.
//
// A NAME CLASH TO KEEP APART: `src/manifest/` (ManifestLoader) is the
// package manifest, naab.toml. This is unrelated and is called the CAPABILITY
// manifest everywhere in code.
//
// What it cannot bound: a subprocess language (shell, node, ruby, php, go,
// ...) runs in its own process, where path and host policy do not reach
// (CONTRA-013). The manifest withholds SHELL_EXEC unless it lists it, and
// without SHELL_EXEC every separate-process language is refused; WITH it, such
// code is bounded by the sandbox only. Every draft says so (kLimitsNote).

#include <map>
#include <string>
#include <vector>

namespace naab {
namespace governance {

struct CapabilityManifest {
    int version = 1;
    // Set by the loader, never read from the file.
    std::string path;           // absolute path of the manifest file
    std::string base_dir;       // absolute directory the relative entries resolve against

    std::string program_file;   // the program's file name (no directory)
    std::string program_sha256; // SHA-256 of the program text, lower-case hex
    // NAAb modules the program may import: path relative to base_dir -> SHA-256.
    std::map<std::string, std::string> modules;
    std::vector<std::string> actions;    // lease vocabulary (GovernanceEngine::leaseActions())
    std::vector<std::string> languages;  // polyglot languages, as written in the blocks
    std::vector<std::string> hosts;      // exact host names or IP literals
    std::vector<std::string> files_read; // paths (files or directories) the program may read
    std::vector<std::string> files_write;// ... and may write
    std::vector<std::string> agents;     // agent configs the program may agent.create()
};

// `dir/prog.naab` -> `dir/prog.manifest.json`.
std::string capabilityManifestPathFor(const std::string& program_path);

// Parses and validates the manifest text. Strict: every key is required,
// every value has its one type, and a key the format does not define is an
// error, because a manifest a person approved must mean exactly what it says
// (a misspelt "agent" key would otherwise read as "no agents" or be dropped).
// Keys starting with '_' are comments and ignored. `valid_actions` is the
// lease vocabulary. Returns false with `error` naming the key.
bool parseCapabilityManifest(const std::string& text,
                             const std::vector<std::string>& valid_actions,
                             CapabilityManifest& out, std::string& error);

// The manifest as JSON text (two-space indent, sorted lists, trailing
// newline), with `_limits` and the given `_inference` notes as comments.
std::string serializeCapabilityManifest(const CapabilityManifest& m,
                                        const std::vector<std::string>& inference_notes);

// What a manifest cannot bound (CONTRA-013), written into every draft.
extern const char* const kCapabilityManifestLimitsNote;

}  // namespace governance
}  // namespace naab
