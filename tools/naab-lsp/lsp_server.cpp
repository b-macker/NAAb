#include "lsp_server.h"
#include <iostream>
#include <chrono>

namespace naab {
namespace lsp {

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
        {"documentSymbolProvider", true}
    };
}

// ============================================================================
// LSPServer
// ============================================================================

LSPServer::LSPServer()
    : state_(ServerState::Uninitialized) {
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

void LSPServer::run() {
    std::cerr << "NAAb LSP Server starting...\n";

    while (state_ != ServerState::Shutdown) {
        // Read message from client
        auto message_str = transport_.readMessage();
        if (!message_str) {
            break;  // EOF or error
        }

        // Parse JSON
        json j;
        try {
            j = json::parse(*message_str);
        } catch (const json::parse_error& e) {
            std::cerr << "JSON parse error: " << e.what() << "\n";
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
    }

    std::cerr << "NAAb LSP Server exiting.\n";
}

void LSPServer::dispatchRequest(const RequestMessage& request) {
    std::cerr << "Request: " << request.method << " (id=" << request.id << ")\n";

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
    } else {
        sendError(request.id, -32601, "Method not found: " + request.method);
    }
}

void LSPServer::dispatchNotification(const NotificationMessage& notification) {
    std::cerr << "Notification: " << notification.method << "\n";

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
    std::cerr << "Server initialized.\n";
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
    std::cerr << "[Debounce] Thread started\n";

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
            std::cerr << "[Debounce] Publishing diagnostics for: " << uri << " (v" << version << ")\n";
            publishDiagnostics(uri, version);
        }
    }

    std::cerr << "[Debounce] Thread stopped\n";
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
