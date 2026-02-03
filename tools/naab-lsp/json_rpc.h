#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <optional>

namespace naab {
namespace lsp {

using json = nlohmann::json;

// JSON-RPC message types
enum class MessageType {
    Request,      // Expects response
    Response,     // Response to request
    Notification  // No response expected
};

// Base message
struct Message {
    std::string jsonrpc = "2.0";  // Always "2.0"
    MessageType type;

    virtual ~Message() = default;
    virtual json toJson() const = 0;
};

// Request message (from client)
struct RequestMessage : public Message {
    int id;
    std::string method;
    json params;

    RequestMessage() { type = MessageType::Request; }
    json toJson() const override;

    static std::optional<RequestMessage> fromJson(const json& j);
};

// Response message (to client)
struct ResponseMessage : public Message {
    int id;
    std::optional<json> result;
    std::optional<json> error;

    ResponseMessage() { type = MessageType::Response; }
    json toJson() const override;
};

// Notification message (no response)
struct NotificationMessage : public Message {
    std::string method;
    json params;

    NotificationMessage() { type = MessageType::Notification; }
    json toJson() const override;

    static std::optional<NotificationMessage> fromJson(const json& j);
};

// JSON-RPC Transport (stdin/stdout)
class JsonRpcTransport {
public:
    JsonRpcTransport();

    // Read message from stdin
    std::optional<std::string> readMessage();

    // Write message to stdout
    void writeMessage(const std::string& message);

    // Write JSON response
    void writeResponse(const ResponseMessage& response);

private:
    std::string readHeaders();
    std::string readContent(size_t length);
    void writeHeaders(size_t content_length);
};

} // namespace lsp
} // namespace naab
