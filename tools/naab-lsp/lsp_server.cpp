#include "lsp_server.h"
#include <iostream>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <cctype>

namespace naab {
namespace lsp {

// ============================================================================
// Log Level Configuration
// ============================================================================

static LogLevel g_log_level = LogLevel::INFO;  // Default log level

LogLevel getLogLevelFromEnv() {
    const char* env_level = std::getenv("NAAB_LSP_LOG_LEVEL");
    if (!env_level) {
        return LogLevel::INFO;  // Default
    }

    std::string level_str = env_level;
    // Convert to uppercase for case-insensitive comparison
    std::transform(level_str.begin(), level_str.end(), level_str.begin(), ::toupper);

    if (level_str == "DEBUG") return LogLevel::DEBUG;
    if (level_str == "INFO") return LogLevel::INFO;
    if (level_str == "WARN") return LogLevel::WARN;
    if (level_str == "ERROR") return LogLevel::ERROR;
    if (level_str == "NONE") return LogLevel::NONE;

    // Invalid value, default to INFO
    return LogLevel::INFO;
}

bool shouldLog(LogLevel level) {
    return static_cast<int>(level) >= static_cast<int>(g_log_level);
}


// ============================================================================
// ServerCapabilities
// ============================================================================

json ServerCapabilities::toJson() const {
    return {
        {"textDocumentSync", {
            {"openClose", true},
            {"change", 1},  // Full sync for now
            {"save", true}
        }},
        {"completionProvider", {
            {"triggerCharacters", {".", "::"}}
        }},
        {"hoverProvider", true},
        {"definitionProvider", true},
        {"documentSymbolProvider", true},
        {"codeActionProvider", true},
        {"workspaceSymbolProvider", true},
        {"renameProvider", true}
    };
}

// ============================================================================
// LSPServer
// ============================================================================

LSPServer::LSPServer()
    : state_(ServerState::Uninitialized) {
    // Initialize log level from environment
    g_log_level = getLogLevelFromEnv();

    // Start debounce thread
    debounce_thread_ = std::thread(&LSPServer::debounceThread, this);
}

LSPServer::~LSPServer() {
    // Stop debounce thread
    should_stop_debounce_ = true;
    debounce_cv_.notify_all();
    if (debounce_thread_.joinable()) {
        debounce_thread_.join();
    }
}

// V-LSP-006: guard against stack overflow via deeply nested JSON-RPC input.
// nlohmann::json::parse has no built-in depth limit — a deeply nested payload
// exhausts the C++ call stack and crashes the LSP server.
static bool checkJsonDepth(const std::string& s, int max_depth = 128) {
    int depth = 0;
    bool in_string = false;
    bool escape_next = false;
    for (unsigned char c : s) {
        if (escape_next) { escape_next = false; continue; }
        if (c == '\\' && in_string) { escape_next = true; continue; }
        if (c == '"') { in_string = !in_string; continue; }
        if (!in_string) {
            if (c == '{' || c == '[') { if (++depth > max_depth) return false; }
            else if (c == '}' || c == ']') { if (depth > 0) --depth; }
        }
    }
    return true;
}

void LSPServer::run() {
    LSP_LOG(LogLevel::INFO, "NAAb LSP Server starting...");

    while (state_ != ServerState::Shutdown) {
        // Read message from client
        auto message_str = transport_.readMessage();
        if (!message_str) {
            // EOF or error - but check if we're initialized first
            // If we're uninitialized, we never got a message, so just exit
            // If we're initialized, this is a clean shutdown
            if (state_ == ServerState::Uninitialized) {
                LSP_LOG(LogLevel::WARN, "Server received EOF before initialization");
            }
            break;
        }

        // V-LSP-006: depth-guard before parse to prevent stack overflow DoS
        if (!checkJsonDepth(*message_str)) {
            LSP_LOG(LogLevel::WARN, "JSON-RPC message rejected: nesting depth exceeds limit");
            continue;
        }

        // Parse JSON
        json j;
        try {
            j = json::parse(*message_str);
        } catch (const json::parse_error& e) {
            LSP_LOG(LogLevel::ERROR, "JSON parse error: " << e.what());
            continue;
        }

        // Dispatch based on message type
        if (j.contains("id") && j.contains("method")) {
            // Request
            auto req = RequestMessage::fromJson(j);
            if (req) {
                dispatchRequest(*req);
            }
        } else if (j.contains("method")) {
            // Notification
            auto notif = NotificationMessage::fromJson(j);
            if (notif) {
                dispatchNotification(*notif);
            }
        }

        // IMPORTANT: Flush stdout after each response to ensure it's sent
        // This is critical for manual testing with pipes
        std::cout.flush();

        // For manual testing: if we processed an initialize request and stdin is closed,
        // give the client a chance to receive the response before we check for more input
        // This allows: echo '{"jsonrpc":"2.0","id":1,"method":"initialize",...}' | naab-lsp
        // to work correctly
    }

    LSP_LOG(LogLevel::INFO, "NAAb LSP Server exiting.");
}

void LSPServer::dispatchRequest(const RequestMessage& request) {
    LSP_LOG(LogLevel::DEBUG, "Request: " << request.method << " (id=" << request.id << ")");

    if (request.method == "initialize") {
        handleInitialize(request);
    } else if (request.method == "shutdown") {
        handleShutdown(request);
    } else if (request.method == "textDocument/completion") {
        handleCompletion(request);
    } else if (request.method == "textDocument/hover") {
        handleHover(request);
    } else if (request.method == "textDocument/definition") {
        handleDefinition(request);
    } else if (request.method == "textDocument/documentSymbol") {
        handleDocumentSymbol(request);
    } else if (request.method == "textDocument/codeAction") {
        handleCodeAction(request);
    } else if (request.method == "workspace/symbol") {
        handleWorkspaceSymbol(request);
    } else if (request.method == "textDocument/rename") {
        handleRename(request);
    } else {
        sendError(request.id, -32601, "Method not found: " + request.method);
    }
}

void LSPServer::dispatchNotification(const NotificationMessage& notification) {
    LSP_LOG(LogLevel::DEBUG, "Notification: " << notification.method);

    if (notification.method == "initialized") {
        handleInitialized(notification);
    } else if (notification.method == "exit") {
        handleExit(notification);
    } else if (notification.method == "textDocument/didOpen") {
        handleDidOpen(notification);
    } else if (notification.method == "textDocument/didChange") {
        handleDidChange(notification);
    } else if (notification.method == "textDocument/didClose") {
        handleDidClose(notification);
    }
    // Ignore unknown notifications
}

// ============================================================================
// Lifecycle
// ============================================================================

void LSPServer::handleInitialize(const RequestMessage& request) {
    state_ = ServerState::Initializing;

    json result = {
        {"capabilities", capabilities_.toJson()},
        {"serverInfo", {
            {"name", "naab-lsp"},
            {"version", "0.1.0"}
        }}
    };

    sendResponse(request.id, result);
}

void LSPServer::handleInitialized(const NotificationMessage& notification) {
    state_ = ServerState::Initialized;
    LSP_LOG(LogLevel::INFO, "Server initialized.");
}

void LSPServer::handleShutdown(const RequestMessage& request) {
    state_ = ServerState::ShuttingDown;
    sendResponse(request.id, nullptr);
}

void LSPServer::handleExit(const NotificationMessage& notification) {
    state_ = ServerState::Shutdown;
}

// ============================================================================
// Document Synchronization
// ============================================================================

void LSPServer::handleDidOpen(const NotificationMessage& notification) {
    auto params = notification.params;
    std::string uri = params["textDocument"]["uri"].get<std::string>();
    std::string text = params["textDocument"]["text"].get<std::string>();
    int version = params["textDocument"]["version"].get<int>();

    // Open document
    doc_manager_.open(uri, text, version);

    // Publish diagnostics
    Document* doc = doc_manager_.getDocument(uri);
    if (doc) {
        json diagnostics_json = json::array();
        for (const auto& diag : doc->getDiagnostics()) {
            diagnostics_json.push_back(diag.toJson());
        }

        sendNotification("textDocument/publishDiagnostics", {
            {"uri", uri},
            {"version", version},
            {"diagnostics", diagnostics_json}
        });
    }
}

void LSPServer::handleDidChange(const NotificationMessage& notification) {
    auto params = notification.params;
    std::string uri = params["textDocument"]["uri"].get<std::string>();
    int version = params["textDocument"]["version"].get<int>();

    // For full sync (TextDocumentSyncKind.Full = 1)
    std::string text = params["contentChanges"][0]["text"].get<std::string>();

    // Update document immediately (so completions see latest text)
    doc_manager_.update(uri, text, version);

    // Schedule debounced diagnostics update (reduce re-parsing on every keystroke)
    scheduleUpdate(uri, version);
}

void LSPServer::handleDidClose(const NotificationMessage& notification) {
    auto params = notification.params;
    std::string uri = params["textDocument"]["uri"].get<std::string>();

    doc_manager_.close(uri);

    // Clear diagnostics
    sendNotification("textDocument/publishDiagnostics", {
        {"uri", uri},
        {"diagnostics", json::array()}
    });
}

// ============================================================================
// Feature Handlers (Stubs)
// ============================================================================

void LSPServer::handleCompletion(const RequestMessage& request) {
    auto params = request.params;
    std::string uri = params["textDocument"]["uri"].get<std::string>();
    Position pos = Position::fromJson(params["position"]);

    Document* doc = doc_manager_.getDocument(uri);
    if (!doc) {
        CompletionList empty_list{false, {}};
        sendResponse(request.id, empty_list.toJson());
        return;
    }

    // Get completions
    auto completions = completion_provider_.getCompletions(*doc, pos);

    sendResponse(request.id, completions.toJson());
}

void LSPServer::handleHover(const RequestMessage& request) {
    auto params = request.params;
    std::string uri = params["textDocument"]["uri"].get<std::string>();
    Position pos = Position::fromJson(params["position"]);

    Document* doc = doc_manager_.getDocument(uri);
    if (!doc) {
        sendResponse(request.id, nullptr);
        return;
    }

    // Get hover
    auto hover = hover_provider_.getHover(*doc, pos);

    if (hover) {
        sendResponse(request.id, hover->toJson());
    } else {
        sendResponse(request.id, nullptr);
    }
}

void LSPServer::handleDefinition(const RequestMessage& request) {
    auto params = request.params;
    std::string uri = params["textDocument"]["uri"].get<std::string>();
    Position pos = Position::fromJson(params["position"]);

    Document* doc = doc_manager_.getDocument(uri);
    if (!doc) {
        sendResponse(request.id, json::array());
        return;
    }

    // Get definition
    auto locations = definition_provider_.getDefinition(*doc, pos);

    // Convert to JSON
    json locations_json = json::array();
    for (const auto& loc : locations) {
        locations_json.push_back(loc.toJson());
    }

    sendResponse(request.id, locations_json);
}

void LSPServer::handleDocumentSymbol(const RequestMessage& request) {
    auto params = request.params;
    std::string uri = params["textDocument"]["uri"].get<std::string>();

    Document* doc = doc_manager_.getDocument(uri);
    if (!doc) {
        sendResponse(request.id, json::array());
        return;
    }

    // Get symbols
    auto symbols = symbol_provider_.getDocumentSymbols(*doc);

    // Convert to JSON
    json symbols_json = json::array();
    for (const auto& symbol : symbols) {
        symbols_json.push_back(symbol.toJson());
    }

    sendResponse(request.id, symbols_json);
}

void LSPServer::handleCodeAction(const RequestMessage& request) {
    auto params = request.params;
    std::string uri = params["textDocument"]["uri"].get<std::string>();
    Range range = Range::fromJson(params["range"]);

    Document* doc = doc_manager_.getDocument(uri);
    if (!doc) {
        sendResponse(request.id, json::array());
        return;
    }

    // Collect diagnostics that overlap the requested range
    std::vector<Diagnostic> context_diags;
    for (const auto& diag : doc->getDiagnostics()) {
        if (diag.range.start.line <= range.end.line &&
            diag.range.end.line >= range.start.line) {
            context_diags.push_back(diag);
        }
    }

    // Also accept explicit context.diagnostics if provided
    if (params.contains("context") && params["context"].contains("diagnostics")) {
        for (const auto& d : params["context"]["diagnostics"]) {
            Diagnostic diag;
            diag.range = Range::fromJson(d["range"]);
            diag.severity = DiagnosticSeverity::Warning;
            if (d.contains("code") && d["code"].is_string())
                diag.code = d["code"].get<std::string>();
            if (d.contains("message") && d["message"].is_string())
                diag.message = d["message"].get<std::string>();
            context_diags.push_back(diag);
        }
    }

    auto actions = code_action_provider_.getCodeActions(*doc, range, context_diags);

    json result = json::array();
    for (const auto& action : actions) {
        result.push_back(action.toJson());
    }

    sendResponse(request.id, result);
}

void LSPServer::handleWorkspaceSymbol(const RequestMessage& request) {
    auto params = request.params;
    std::string query;
    if (params.contains("query") && params["query"].is_string()) {
        query = params["query"].get<std::string>();
    }

    // Collect symbols from all open documents
    // V-LSP-002: cap result size to prevent unbounded JSON serialization
    constexpr size_t MAX_WORKSPACE_SYMBOLS = 10000;
    bool truncated = false;

    json result = json::array();
    for (Document* doc : doc_manager_.getAllDocuments()) {
        if (truncated) break;
        auto symbols = symbol_provider_.getDocumentSymbols(*doc);
        for (const auto& sym : symbols) {
            if (result.size() >= MAX_WORKSPACE_SYMBOLS) {
                truncated = true;
                break;
            }
            // Filter by query (case-insensitive prefix or substring match)
            if (!query.empty()) {
                std::string sym_lower = sym.name;
                std::string query_lower = query;
                std::transform(sym_lower.begin(), sym_lower.end(), sym_lower.begin(), ::tolower);
                std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
                if (sym_lower.find(query_lower) == std::string::npos) {
                    continue;
                }
            }
            // Workspace symbol includes location with URI
            json sym_json = sym.toJson();
            sym_json["location"] = {
                {"uri", doc->getUri()},
                {"range", sym_json.value("range", json::object())}
            };
            sym_json.erase("range");
            result.push_back(sym_json);
        }
    }
    if (truncated) {
        fprintf(stderr, "[lsp] handleWorkspaceSymbol: result truncated at %zu symbols\n",
                MAX_WORKSPACE_SYMBOLS);
    }

    sendResponse(request.id, result);
}

void LSPServer::handleRename(const RequestMessage& request) {
    auto params = request.params;
    std::string uri = params["textDocument"]["uri"].get<std::string>();
    Position pos = Position::fromJson(params["position"]);
    std::string new_name = params["newName"].get<std::string>();

    Document* doc = doc_manager_.getDocument(uri);
    if (!doc) {
        sendResponse(request.id, nullptr);
        return;
    }

    // Find the word at cursor position
    std::string line_text = doc->getLineText(pos.line);
    if (line_text.empty()) {
        sendResponse(request.id, nullptr);
        return;
    }

    // Find word boundaries at the cursor
    int start_col = pos.character;
    int end_col = pos.character;

    auto is_word_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };

    while (start_col > 0 && is_word_char(line_text[start_col - 1])) --start_col;
    while (end_col < static_cast<int>(line_text.size()) && is_word_char(line_text[end_col])) ++end_col;

    if (start_col == end_col) {
        sendResponse(request.id, nullptr);
        return;
    }

    std::string old_name = line_text.substr(start_col, end_col - start_col);
    if (old_name.empty()) {
        sendResponse(request.id, nullptr);
        return;
    }

    // Collect all edits: scan every line of the document for occurrences of old_name
    // as a whole word (not part of a larger identifier)
    // V-LSP-001: bounds to prevent DoS on large documents
    constexpr size_t MAX_RENAME_FILE_BYTES = 1 * 1024 * 1024;  // 1 MiB
    constexpr size_t MAX_RENAME_EDITS     = 10000;

    json changes = json::object();
    json edits = json::array();

    const std::string& text = doc->getText();
    if (text.size() > MAX_RENAME_FILE_BYTES) {
        sendError(request.id, -32603,
            "Rename aborted: document exceeds 1 MiB size limit");
        return;
    }

    std::istringstream iss(text);
    std::string cur_line;
    int line_num = 0;

    while (std::getline(iss, cur_line)) {
        size_t search_pos = 0;
        while ((search_pos = cur_line.find(old_name, search_pos)) != std::string::npos) {
            // Check word boundaries
            bool left_ok = (search_pos == 0 || !is_word_char(cur_line[search_pos - 1]));
            bool right_ok = (search_pos + old_name.size() >= cur_line.size() ||
                             !is_word_char(cur_line[search_pos + old_name.size()]));

            if (left_ok && right_ok) {
                edits.push_back({
                    {"range", Range{
                        Position{line_num, static_cast<int>(search_pos)},
                        Position{line_num, static_cast<int>(search_pos + old_name.size())}
                    }.toJson()},
                    {"newText", new_name}
                });
                if (edits.size() >= MAX_RENAME_EDITS) {
                    sendError(request.id, -32603,
                        "Rename aborted: too many occurrences (limit: 10000 edits)");
                    return;
                }
            }

            search_pos += old_name.size();
        }
        ++line_num;
    }

    if (edits.empty()) {
        sendResponse(request.id, nullptr);
        return;
    }

    changes[uri] = edits;
    sendResponse(request.id, {{"changes", changes}});
}

// ============================================================================
// Response Helpers
// ============================================================================

void LSPServer::sendResponse(int id, const json& result) {
    ResponseMessage response;
    response.id = id;
    response.result = result;
    transport_.writeResponse(response);
}

void LSPServer::sendError(int id, int code, const std::string& message) {
    ResponseMessage response;
    response.id = id;
    response.error = json{
        {"code", code},
        {"message", message}
    };
    transport_.writeResponse(response);
}

void LSPServer::sendNotification(const std::string& method, const json& params) {
    NotificationMessage notif;
    notif.method = method;
    notif.params = params;
    std::string message = notif.toJson().dump();
    transport_.writeMessage(message);
}

// ============================================================================
// Debouncing
// ============================================================================

void LSPServer::debounceThread() {
    LSP_LOG(LogLevel::DEBUG, "[Debounce] Thread started");

    while (!should_stop_debounce_) {
        std::unique_lock<std::mutex> lock(debounce_mutex_);

        // Wait for updates or timeout (300ms debounce delay)
        debounce_cv_.wait_for(lock, std::chrono::milliseconds(300));

        if (should_stop_debounce_) break;

        // Process all pending updates
        auto updates = pending_updates_;  // Copy to avoid holding lock
        pending_updates_.clear();
        lock.unlock();

        for (const auto& [uri, version] : updates) {
            LSP_LOG(LogLevel::DEBUG, "[Debounce] Publishing diagnostics for: " << uri << " (v" << version << ")");
            publishDiagnostics(uri, version);
        }
    }

    LSP_LOG(LogLevel::DEBUG, "[Debounce] Thread stopped");
}

void LSPServer::scheduleUpdate(const std::string& uri, int version) {
    std::lock_guard<std::mutex> lock(debounce_mutex_);
    pending_updates_[uri] = version;  // Update version (last one wins)
    debounce_cv_.notify_one();
}

void LSPServer::publishDiagnostics(const std::string& uri, int version) {
    Document* doc = doc_manager_.getDocument(uri);
    if (!doc) return;

    json diagnostics_json = json::array();
    for (const auto& diag : doc->getDiagnostics()) {
        diagnostics_json.push_back(diag.toJson());
    }

    sendNotification("textDocument/publishDiagnostics", {
        {"uri", uri},
        {"version", version},
        {"diagnostics", diagnostics_json}
    });
}

} // namespace lsp
} // namespace naab
