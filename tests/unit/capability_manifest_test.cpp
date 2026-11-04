// The capability manifest's file format (include/naab/capability_manifest.h,
// Bundle 11 PR 2). A manifest a person approved must mean exactly what it says,
// so the parser is strict: a false accept turns a typo into a grant or a
// silent omission. Each refusal below is beside the valid manifest it edits.
#include "naab/capability_manifest.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using naab::governance::CapabilityManifest;
using naab::governance::capabilityManifestPathFor;
using naab::governance::parseCapabilityManifest;
using naab::governance::serializeCapabilityManifest;

namespace {

const std::vector<std::string> kActions = {
    "FS_READ", "FS_WRITE", "NET_CONNECT", "SHELL_EXEC", "ENV_READ", "ENV_WRITE", "AGENT_SEND"};

const std::string kHash(64, 'a');

std::string valid(const std::string& replace_from = "", const std::string& replace_to = "") {
    std::string s =
        "{\"naab_manifest\": 1,"
        " \"program\": {\"file\": \"p.naab\", \"sha256\": \"" + kHash + "\"},"
        " \"modules\": {\"lib.naab\": \"" + kHash + "\"},"
        " \"actions\": [\"FS_READ\", \"FS_WRITE\"],"
        " \"languages\": [\"python\"],"
        " \"hosts\": [\"api.example.com\"],"
        " \"files\": {\"read\": [\"data\"], \"write\": [\"out/a.txt\"]},"
        " \"agents\": [\"reviewer\"],"
        " \"_limits\": \"a comment\"}";
    if (!replace_from.empty()) {
        auto at = s.find(replace_from);
        if (at != std::string::npos) s.replace(at, replace_from.size(), replace_to);
    }
    return s;
}

std::string refusal(const std::string& text) {
    CapabilityManifest m;
    std::string err;
    if (parseCapabilityManifest(text, kActions, m, err)) return "";
    return err.empty() ? "(refused, no reason)" : err;
}

}  // namespace

TEST(CapabilityManifest, ValidManifestParses) {
    CapabilityManifest m;
    std::string err;
    ASSERT_TRUE(parseCapabilityManifest(valid(), kActions, m, err)) << err;
    EXPECT_EQ(m.program_file, "p.naab");
    EXPECT_EQ(m.program_sha256, kHash);
    EXPECT_EQ(m.modules.at("lib.naab"), kHash);
    EXPECT_EQ(m.actions, (std::vector<std::string>{"FS_READ", "FS_WRITE"}));
    EXPECT_EQ(m.files_read, (std::vector<std::string>{"data"}));
    EXPECT_EQ(m.files_write, (std::vector<std::string>{"out/a.txt"}));
    EXPECT_EQ(m.agents, (std::vector<std::string>{"reviewer"}));
}

TEST(CapabilityManifest, UnknownAndMissingKeysAreRefused) {
    EXPECT_NE(refusal(valid("\"agents\"", "\"agent\"")).find("unknown key \"agent\""), std::string::npos);
    EXPECT_NE(refusal(valid("\"hosts\": [\"api.example.com\"],", "")).find("missing key \"hosts\""), std::string::npos);
    EXPECT_FALSE(refusal(valid("\"naab_manifest\": 1", "\"naab_manifest\": 2")).empty());
    EXPECT_FALSE(refusal("[]").empty());
    EXPECT_FALSE(refusal("not json").empty());
}

TEST(CapabilityManifest, ValuesHaveOneType) {
    EXPECT_FALSE(refusal(valid("\"actions\": [\"FS_READ\", \"FS_WRITE\"]", "\"actions\": \"FS_READ\"")).empty());
    EXPECT_FALSE(refusal(valid("\"languages\": [\"python\"]", "\"languages\": [1]")).empty());
    EXPECT_FALSE(refusal(valid("\"agents\": [\"reviewer\"]", "\"agents\": [\"\"]")).empty());
    EXPECT_FALSE(refusal(valid("{\"read\": [\"data\"], \"write\": [\"out/a.txt\"]}",
                               "{\"read\": [\"data\"]}")).empty());
    EXPECT_FALSE(refusal(valid("{\"read\": [\"data\"], \"write\": [\"out/a.txt\"]}",
                               "{\"read\": [\"data\"], \"write\": [], \"delete\": []}")).empty());
}

TEST(CapabilityManifest, ActionsComeFromTheLeaseVocabulary) {
    EXPECT_NE(refusal(valid("\"FS_WRITE\"]", "\"FS_DELETE\"]")).find("unknown action"), std::string::npos);
}

TEST(CapabilityManifest, ProgramAndModuleHashesAreExact) {
    EXPECT_FALSE(refusal(valid("\"sha256\": \"" + kHash, "\"sha256\": \"" + std::string(64, 'A'))).empty());
    EXPECT_FALSE(refusal(valid("\"sha256\": \"" + kHash, "\"sha256\": \"" + std::string(63, 'a'))).empty());
    EXPECT_FALSE(refusal(valid("\"lib.naab\": \"" + kHash, "\"lib.naab\": \"" + std::string(64, 'z'))).empty());
    EXPECT_FALSE(refusal(valid("\"file\": \"p.naab\"", "\"file\": \"dir/p.naab\"")).empty());
}

TEST(CapabilityManifest, HostsAreExactNames) {
    EXPECT_FALSE(refusal(valid("\"api.example.com\"", "\"*.example.com\"")).empty());
    EXPECT_FALSE(refusal(valid("\"api.example.com\"", "\"https://api.example.com\"")).empty());
    EXPECT_FALSE(refusal(valid("\"api.example.com\"", "\"user@api.example.com\"")).empty());
    CapabilityManifest m;
    std::string err;
    ASSERT_TRUE(parseCapabilityManifest(valid("\"api.example.com\"", "\"API.Example.COM\""), kActions, m, err)) << err;
    EXPECT_EQ(m.hosts, (std::vector<std::string>{"api.example.com"}));
}

TEST(CapabilityManifest, SerializedDraftParsesBack) {
    CapabilityManifest m;
    m.program_file = "p.naab";
    m.program_sha256 = kHash;
    m.modules["lib.naab"] = kHash;
    m.actions = {"FS_WRITE", "FS_READ", "FS_READ"};
    m.languages = {"python"};
    m.files_write = {"out/a.txt"};
    const std::string text = serializeCapabilityManifest(m, {"a note"});
    CapabilityManifest back;
    std::string err;
    ASSERT_TRUE(parseCapabilityManifest(text, kActions, back, err)) << err << "\n" << text;
    EXPECT_EQ(back.actions, (std::vector<std::string>{"FS_READ", "FS_WRITE"}));  // sorted, de-duplicated
    EXPECT_EQ(back.modules, m.modules);
    EXPECT_NE(text.find("CONTRA-013"), std::string::npos);  // the limits note is in every draft
}

TEST(CapabilityManifest, SidecarNameIsTheStem) {
    EXPECT_EQ(capabilityManifestPathFor("dir/prog.naab"), "dir/prog.manifest.json");
    EXPECT_EQ(capabilityManifestPathFor("prog.naab"), "prog.manifest.json");
}
