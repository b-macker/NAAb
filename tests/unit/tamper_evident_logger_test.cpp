// Unit tests for TamperEvidenceLogger
// Phase 1 Item 8: Tamper-Evident Logging

#include <gtest/gtest.h>
#include "naab/tamper_evident_logger.h"
#include "naab/audit_logger.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

using namespace naab::security;

class TamperEvidenceLoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use current directory instead of /tmp (read-only in Termux)
        test_log_path_ = "./test_tamper_evident_" +
                        std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
                        ".log";
    }

    void TearDown() override {
        // Clean up test log file
        if (std::filesystem::exists(test_log_path_)) {
            std::filesystem::remove(test_log_path_);
        }
    }

    std::string test_log_path_;
};

// ============================================================================
// Basic Functionality Tests
// ============================================================================

TEST_F(TamperEvidenceLoggerTest, InitializationCreatesGenesisBlock) {
    TamperEvidenceLogger logger(test_log_path_);

    // Verify file was created
    ASSERT_TRUE(std::filesystem::exists(test_log_path_));

    // Verify genesis block
    std::ifstream file(test_log_path_);
    std::string first_line;
    std::getline(file, first_line);

    EXPECT_FALSE(first_line.empty());
    EXPECT_NE(first_line.find("\"sequence\":0"), std::string::npos);
    EXPECT_NE(first_line.find("LOG_INIT"), std::string::npos);
}

TEST_F(TamperEvidenceLoggerTest, LogEventIncreasesSequence) {
    TamperEvidenceLogger logger(test_log_path_);

    EXPECT_EQ(logger.getSequence(), 0);  // Genesis block

    logger.logEvent(AuditEvent::BLOCK_LOAD, "Test event");
    EXPECT_EQ(logger.getSequence(), 1);

    logger.logEvent(AuditEvent::BLOCK_EXECUTE, "Another event");
    EXPECT_EQ(logger.getSequence(), 2);
}

TEST_F(TamperEvidenceLoggerTest, HashChainLinking) {
    TamperEvidenceLogger logger(test_log_path_);

    // Log multiple events
    logger.logEvent(AuditEvent::BLOCK_LOAD, "Event 1");
    logger.logEvent(AuditEvent::BLOCK_EXECUTE, "Event 2");
    logger.logEvent(AuditEvent::SECURITY_VIOLATION, "Event 3");
    logger.flush();

    // Read log and verify chain
    std::ifstream file(test_log_path_);
    std::string line;
    std::string prev_hash;

    // Genesis block
    std::getline(file, line);
    size_t hash_pos = line.find("\"hash\":\"");
    ASSERT_NE(hash_pos, std::string::npos);
    size_t hash_start = hash_pos + 8;
    size_t hash_end = line.find("\"", hash_start);
    prev_hash = line.substr(hash_start, hash_end - hash_start);

    // Verify subsequent entries link correctly
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        // Check prev_hash matches
        size_t prev_hash_pos = line.find("\"prev_hash\":\"");
        ASSERT_NE(prev_hash_pos, std::string::npos);
        size_t prev_hash_start = prev_hash_pos + 13;
        size_t prev_hash_end = line.find("\"", prev_hash_start);
        std::string current_prev_hash = line.substr(prev_hash_start, prev_hash_end - prev_hash_start);

        EXPECT_EQ(current_prev_hash, prev_hash) << "Hash chain broken!";

        // Get current hash for next iteration
        hash_pos = line.find("\"hash\":\"");
        ASSERT_NE(hash_pos, std::string::npos);
        hash_start = hash_pos + 8;
        hash_end = line.find("\"", hash_start);
        prev_hash = line.substr(hash_start, hash_end - hash_start);
    }
}

// ============================================================================
// HMAC Signature Tests
// ============================================================================

TEST_F(TamperEvidenceLoggerTest, HMACSigningEnabled) {
    TamperEvidenceLogger logger(test_log_path_);
    logger.enableHMAC("test-secret-key");

    logger.logEvent(AuditEvent::BLOCK_LOAD, "Test with HMAC");
    logger.flush();

    // Verify signature field exists
    std::ifstream file(test_log_path_);
    std::string line;
    bool found_signature = false;

    while (std::getline(file, line)) {
        if (line.find("\"signature\":\"hmac-sha256:") != std::string::npos) {
            found_signature = true;
            break;
        }
    }

    EXPECT_TRUE(found_signature) << "HMAC signature not found in log";
}

TEST_F(TamperEvidenceLoggerTest, HMACDisabling) {
    TamperEvidenceLogger logger(test_log_path_);

    logger.enableHMAC("test-secret-key");
    logger.logEvent(AuditEvent::BLOCK_LOAD, "With HMAC");

    logger.disableHMAC();
    logger.logEvent(AuditEvent::BLOCK_EXECUTE, "Without HMAC");
    logger.flush();

    // Count entries with and without signatures
    std::ifstream file(test_log_path_);
    std::string line;
    int with_sig = 0;
    int without_sig = 0;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (line.find("\"signature\":\"hmac-sha256:") != std::string::npos) {
            with_sig++;
        } else if (line.find("\"signature\":\"\"") != std::string::npos) {
            without_sig++;
        }
    }

    EXPECT_GT(with_sig, 0) << "Should have entries with HMAC";
    EXPECT_GT(without_sig, 0) << "Should have entries without HMAC";
}

// ============================================================================
// Verification Tests
// ============================================================================

TEST_F(TamperEvidenceLoggerTest, VerifyIntactLog) {
    TamperEvidenceLogger logger(test_log_path_);

    logger.logEvent(AuditEvent::BLOCK_LOAD, "Event 1");
    logger.logEvent(AuditEvent::BLOCK_EXECUTE, "Event 2");
    logger.logEvent(AuditEvent::SECURITY_VIOLATION, "Event 3");
    logger.flush();

    // Verify integrity
    auto result = logger.verifyIntegrity();

    EXPECT_TRUE(result.is_valid) << "Intact log should verify successfully";
    EXPECT_EQ(result.total_entries, 4);  // Genesis + 3 events
    EXPECT_EQ(result.verified_entries, 4);
    EXPECT_TRUE(result.errors.empty());
    EXPECT_TRUE(result.tampered_sequences.empty());
}

TEST_F(TamperEvidenceLoggerTest, DetectTamperedEntry) {
    {
        TamperEvidenceLogger logger(test_log_path_);
        logger.logEvent(AuditEvent::BLOCK_LOAD, "Original event");
        logger.logEvent(AuditEvent::BLOCK_EXECUTE, "Another event");
        logger.flush();
    }

    // Tamper with the log (modify an entry)
    std::ifstream in_file(test_log_path_);
    std::string content;
    std::string line;
    int line_num = 0;

    while (std::getline(in_file, line)) {
        if (line_num == 1) {  // Modify second line (first real event)
            // Change the details field
            size_t pos = line.find("Original event");
            if (pos != std::string::npos) {
                line.replace(pos, 14, "TAMPERED EVENT");
            }
        }
        content += line + "\n";
        line_num++;
    }
    in_file.close();

    std::ofstream out_file(test_log_path_);
    out_file << content;
    out_file.close();

    // Verify - should detect tampering
    TamperEvidenceLogger logger(test_log_path_);
    auto result = logger.verifyIntegrity();

    EXPECT_FALSE(result.is_valid) << "Tampered log should fail verification";
    EXPECT_FALSE(result.tampered_sequences.empty());
}

// ============================================================================
// Concurrent Access Tests
// ============================================================================

TEST_F(TamperEvidenceLoggerTest, ConcurrentLogging) {
    TamperEvidenceLogger logger(test_log_path_);

    // Log from multiple threads
    std::vector<std::thread> threads;
    const int num_threads = 5;
    const int events_per_thread = 10;

    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back([&logger, i]() {
            for (int j = 0; j < events_per_thread; j++) {
                logger.logEvent(AuditEvent::BLOCK_LOAD,
                               "Thread " + std::to_string(i) + " Event " + std::to_string(j));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }

    logger.flush();

    // Verify all events logged
    EXPECT_EQ(logger.getSequence(), num_threads * events_per_thread);

    // Verify integrity
    auto result = logger.verifyIntegrity();
    EXPECT_TRUE(result.is_valid);
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(TamperEvidenceLoggerTest, EmptyMetadata) {
    TamperEvidenceLogger logger(test_log_path_);

    std::map<std::string, std::string> empty_metadata;
    logger.logEvent(AuditEvent::BLOCK_LOAD, "No metadata", empty_metadata);
    logger.flush();

    auto result = logger.verifyIntegrity();
    EXPECT_TRUE(result.is_valid);
}

TEST_F(TamperEvidenceLoggerTest, LargeMetadata) {
    TamperEvidenceLogger logger(test_log_path_);

    std::map<std::string, std::string> large_metadata;
    for (int i = 0; i < 100; i++) {
        large_metadata["key_" + std::to_string(i)] = "value_" + std::to_string(i);
    }

    logger.logEvent(AuditEvent::BLOCK_LOAD, "Lots of metadata", large_metadata);
    logger.flush();

    auto result = logger.verifyIntegrity();
    EXPECT_TRUE(result.is_valid);
}

TEST_F(TamperEvidenceLoggerTest, SpecialCharactersInDetails) {
    TamperEvidenceLogger logger(test_log_path_);

    logger.logEvent(AuditEvent::SECURITY_VIOLATION,
                   "Special chars: \n\t\"\\{}\u00A9\u00AE");
    logger.flush();

    auto result = logger.verifyIntegrity();
    EXPECT_TRUE(result.is_valid);
}

// ============================================================================
// AuditLogger Integration Tests
// ============================================================================

TEST_F(TamperEvidenceLoggerTest, AuditLoggerIntegration) {
    // Test through AuditLogger interface
    AuditLogger::setLogFile(test_log_path_);
    AuditLogger::setTamperEvidence(true);

    EXPECT_TRUE(AuditLogger::isTamperEvidenceEnabled());

    AuditLogger::logBlockLoad("TEST-BLOCK", "sha256:hash");
    AuditLogger::logSecurityViolation("Test violation");

    AuditLogger::flush();

    // Verify file exists and has content
    EXPECT_TRUE(std::filesystem::exists(test_log_path_ + ".tamper_evident"));

    AuditLogger::setTamperEvidence(false);
    EXPECT_FALSE(AuditLogger::isTamperEvidenceEnabled());
}

// ============================================================================
// Performance Tests (Optional - commented out for regular test runs)
// ============================================================================

/*
TEST_F(TamperEvidenceLoggerTest, DISABLED_PerformanceBenchmark) {
    TamperEvidenceLogger logger(test_log_path_);

    const int num_events = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_events; i++) {
        logger.logEvent(AuditEvent::BLOCK_LOAD, "Performance test event");
    }
    logger.flush();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    std::cout << "Logged " << num_events << " events in " << duration.count() << " ms\n";
    std::cout << "Average: " << (duration.count() / static_cast<double>(num_events)) << " ms/event\n";

    // Performance target: < 1ms per event
    EXPECT_LT(duration.count() / static_cast<double>(num_events), 1.0);
}
*/
