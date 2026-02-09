// NAAb Polyglot Dependency Analyzer Implementation
// Implements dependency analysis for parallel polyglot execution

#include "naab/polyglot_dependency_analyzer.h"
#include "naab/ast.h"
#include <algorithm>
#include <unordered_map>

namespace naab {
namespace interpreter {

std::vector<PolyglotBlock> PolyglotDependencyAnalyzer::extractPolyglotBlocks(
    const std::vector<ast::Stmt*>& statements
) {
    std::vector<PolyglotBlock> blocks;

    for (size_t i = 0; i < statements.size(); ++i) {
        auto* stmt = statements[i];

        // Case 1: Variable declaration with polyglot block
        // Example: let result = <<python ...>>
        if (auto* var_decl = dynamic_cast<ast::VarDeclStmt*>(stmt)) {
            if (var_decl->getInit()) {
                if (auto* inline_code = dynamic_cast<ast::InlineCodeExpr*>(var_decl->getInit())) {
                    PolyglotBlock block;
                    block.statement = stmt;
                    block.node = inline_code;
                    block.assigned_var = var_decl->getName();
                    block.read_vars = inline_code->getBoundVariables();
                    block.write_vars = {var_decl->getName()};
                    block.statement_index = i;
                    blocks.push_back(block);
                }
            }
        }
        // Case 2: Expression statement with polyglot block
        // Example: <<python print("Hello") >>
        else if (auto* expr_stmt = dynamic_cast<ast::ExprStmt*>(stmt)) {
            if (auto* inline_code = dynamic_cast<ast::InlineCodeExpr*>(expr_stmt->getExpr())) {
                PolyglotBlock block;
                block.statement = stmt;
                block.node = inline_code;
                block.assigned_var = "";  // No variable assignment
                block.read_vars = inline_code->getBoundVariables();
                block.write_vars = {};    // Doesn't write to any variable
                block.statement_index = i;
                blocks.push_back(block);
            }
        }
        // Note: Assignment to existing variable (x = <<python ...>>) would be handled by
        // ast::AssignmentStmt, but we'll handle that when needed
    }

    return blocks;
}

bool PolyglotDependencyAnalyzer::hasDataDependency(
    const PolyglotBlock& a,
    const PolyglotBlock& b
) const {
    // RAW (Read-After-Write): Block 'b' reads a variable that 'a' writes
    // AND 'a' must come before 'b' in source order
    if (a.statement_index >= b.statement_index) {
        return false;  // 'b' comes before 'a', no RAW dependency
    }

    // Check if any variable written by 'a' is read by 'b'
    for (const auto& write_var : a.write_vars) {
        for (const auto& read_var : b.read_vars) {
            if (write_var == read_var) {
                return true;  // RAW dependency detected
            }
        }
    }

    return false;
}

bool PolyglotDependencyAnalyzer::hasOutputDependency(
    const PolyglotBlock& a,
    const PolyglotBlock& b
) const {
    // WAW (Write-After-Write): Both blocks write to the same variable
    // AND 'a' must come before 'b' in source order
    if (a.statement_index >= b.statement_index) {
        return false;  // 'b' comes before 'a', no WAW dependency
    }

    // Check if any variable written by both 'a' and 'b'
    for (const auto& write_a : a.write_vars) {
        for (const auto& write_b : b.write_vars) {
            if (write_a == write_b) {
                return true;  // WAW dependency detected
            }
        }
    }

    return false;
}

bool PolyglotDependencyAnalyzer::hasAntiDependency(
    const PolyglotBlock& a,
    const PolyglotBlock& b
) const {
    // WAR (Write-After-Read): Block 'b' writes a variable that 'a' reads
    // AND 'a' must come before 'b' in source order
    if (a.statement_index >= b.statement_index) {
        return false;  // 'b' comes before 'a', no WAR dependency
    }

    // Check if any variable read by 'a' is written by 'b'
    for (const auto& read_var : a.read_vars) {
        for (const auto& write_var : b.write_vars) {
            if (read_var == write_var) {
                return true;  // WAR dependency detected
            }
        }
    }

    return false;
}

bool PolyglotDependencyAnalyzer::hasDependency(
    const PolyglotBlock& a,
    const PolyglotBlock& b
) const {
    return hasDataDependency(a, b) ||
           hasOutputDependency(a, b) ||
           hasAntiDependency(a, b);
}

std::vector<DependencyGroup> PolyglotDependencyAnalyzer::buildDependencyGroups(
    const std::vector<PolyglotBlock>& blocks
) {
    if (blocks.empty()) {
        return {};
    }

    std::vector<DependencyGroup> groups;
    std::vector<bool> processed(blocks.size(), false);

    // Greedy algorithm: Build groups level by level
    // Each group contains blocks that can execute in parallel
    // Groups execute sequentially in order

    while (true) {
        DependencyGroup current_group;

        // Find all blocks that:
        // 1. Haven't been processed yet
        // 2. Don't depend on any unprocessed blocks
        // 3. Don't conflict with blocks already in current group

        for (size_t i = 0; i < blocks.size(); ++i) {
            if (processed[i]) {
                continue;  // Already in a previous group
            }

            // Check if this block depends on any unprocessed blocks that come before it
            bool has_unprocessed_dependency = false;
            for (size_t j = 0; j < i; ++j) {
                if (!processed[j] && hasDependency(blocks[j], blocks[i])) {
                    has_unprocessed_dependency = true;
                    break;
                }
            }

            if (has_unprocessed_dependency) {
                continue;  // Can't add to current group, needs to wait
            }

            // Check if this block conflicts with any block already in current group
            bool conflicts_with_group = false;
            for (const auto& block_in_group : current_group.parallel_blocks) {
                // Check both directions for dependencies
                if (hasDependency(blocks[i], block_in_group) ||
                    hasDependency(block_in_group, blocks[i])) {
                    conflicts_with_group = true;
                    break;
                }
            }

            if (!conflicts_with_group) {
                // This block can be added to the current group
                current_group.parallel_blocks.push_back(blocks[i]);
                processed[i] = true;
            }
        }

        // If we couldn't add any blocks to this group, we're done
        if (current_group.parallel_blocks.empty()) {
            break;
        }

        // Add the current group to the result
        groups.push_back(current_group);

        // Check if all blocks are processed
        bool all_processed = true;
        for (bool p : processed) {
            if (!p) {
                all_processed = false;
                break;
            }
        }

        if (all_processed) {
            break;
        }
    }

    // Set up group dependencies (each group depends on previous groups)
    for (size_t i = 1; i < groups.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
            groups[i].depends_on_groups.push_back(j);
        }
    }

    return groups;
}

std::vector<DependencyGroup> PolyglotDependencyAnalyzer::analyze(
    const std::vector<ast::Stmt*>& statements
) {
    // Step 1: Extract all polyglot blocks from statements
    auto blocks = extractPolyglotBlocks(statements);

    // If there are no polyglot blocks, return empty result
    if (blocks.empty()) {
        return {};
    }

    // If there's only one block, create a single group
    if (blocks.size() == 1) {
        DependencyGroup group;
        group.parallel_blocks.push_back(blocks[0]);
        return {group};
    }

    // Step 2: Split blocks into batches based on statement gaps
    // If there are 2+ non-polyglot statements between two blocks,
    // they should be in different batches (those statements might be variable declarations)
    std::vector<std::vector<PolyglotBlock>> batches;
    std::vector<PolyglotBlock> current_batch;
    current_batch.push_back(blocks[0]);

    for (size_t i = 1; i < blocks.size(); ++i) {
        size_t gap = blocks[i].statement_index - blocks[i-1].statement_index - 1;

        // If there's a gap of 2+ statements, start a new batch
        // (1 statement could just be a print, but 2+ likely includes variable declarations)
        if (gap >= 2) {
            batches.push_back(current_batch);
            current_batch.clear();
        }

        current_batch.push_back(blocks[i]);
    }

    if (!current_batch.empty()) {
        batches.push_back(current_batch);
    }

    // Step 3: Build dependency groups for each batch and combine
    std::vector<DependencyGroup> all_groups;
    for (const auto& batch : batches) {
        size_t prev_group_count = all_groups.size();
        auto batch_groups = buildDependencyGroups(batch);

        // Add cross-batch dependencies: each group in this batch depends on
        // all groups from previous batches (ensures execution order)
        for (auto& group : batch_groups) {
            for (size_t i = 0; i < prev_group_count; ++i) {
                group.depends_on_groups.push_back(i);
            }
        }

        all_groups.insert(all_groups.end(), batch_groups.begin(), batch_groups.end());
    }

    return all_groups;
}

} // namespace interpreter
} // namespace naab
