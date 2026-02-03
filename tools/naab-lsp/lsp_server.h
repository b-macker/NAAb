#pragma once

#include "json_rpc.h"
#include "document_manager.h"
#include "symbol_provider.h"
#include "hover_provider.h"
#include "completion_provider.h"
#include "definition_provider.h"
#include <memory>
#include <map>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace naab {
namespace lsp {

// LSP Server Capabilities (what features we support)
struct ServerCapabilities {
    bool textDocumentSync = true;
    bool completionProvider = true;
    bool hoverProvider = true;
    bool definitionProvider = true;
    bool documentSymbolProvider = true;
    bool diagnosticProvider = true;

    json toJson() const;
};

// LSP Server State
enum class ServerState {
    Uninitialized,
    Initializing,
    Initialized,
    ShuttingDown,
    Shutdown
};

// Main LSP Server
class LSPServer {
public:
    LSPServer();
    ~LSPServer();

    // Main loop - read messages and dispatch
    void run();

    // Lifecycle methods
    void handleInitialize(const RequestMessage& request);
    void handleInitialized(const NotificationMessage& notification);
    void handleShutdown(const RequestMessage& request);
    void handleExit(const NotificationMessage& notification);

    // Document synchronization
    void handleDidOpen(const NotificationMessage& notification);
    void handleDidChange(const NotificationMessage& notification);
    void handleDidClose(const NotificationMessage& notification);

    // Feature handlers (stubs for now, implement in Week 2-3)
    void handleCompletion(const RequestMessage& request);
    void handleHover(const RequestMessage& request);
    void handleDefinition(const RequestMessage& request);
    void handleDocumentSymbol(const RequestMessage& request);

private:
    JsonRpcTransport transport_;
    ServerState state_;
    ServerCapabilities capabilities_;
    DocumentManager doc_manager_;
    SymbolProvider symbol_provider_;
    HoverProvider hover_provider_;
    CompletionProvider completion_provider_;
    DefinitionProvider definition_provider_;

    // Debouncing for document updates
    std::mutex debounce_mutex_;
    std::condition_variable debounce_cv_;
    std::map<std::string, int> pending_updates_;  // uri -> version
    std::thread debounce_thread_;
    bool should_stop_debounce_ = false;

    void debounceThread();
    void scheduleUpdate(const std::string& uri, int version);
    void publishDiagnostics(const std::string& uri, int version);

    // Message routing
    void dispatchRequest(const RequestMessage& request);
    void dispatchNotification(const NotificationMessage& notification);

    // Response helpers
    void sendResponse(int id, const json& result);
    void sendError(int id, int code, const std::string& message);
    void sendNotification(const std::string& method, const json& params);
};

} // namespace lsp
} // namespace naab
