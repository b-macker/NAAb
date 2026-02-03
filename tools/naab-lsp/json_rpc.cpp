#include "json_rpc.h"
#include <iostream>
#include <sstream>
#include <cstring>

namespace naab {
namespace lsp {

// ============================================================================
// RequestMessage
// ============================================================================

json RequestMessage::toJson() const {
    return {
        {"jsonrpc", jsonrpc},
        {"id", id},
        {"method", method},
        {"params", params}
    };
}

std::optional<RequestMessage> RequestMessage::fromJson(const json& j) {
    if (!j.contains("method") || !j.contains("id")) {
        return std::nullopt;
    }

    RequestMessage req;
    req.id = j["id"].get<int>();
    req.method = j["method"].get<std::string>();
    req.params = j.value("params", json::object());

    return req;
}

// ============================================================================
// ResponseMessage
// ============================================================================

json ResponseMessage::toJson() const {
    json j = {
        {"jsonrpc", jsonrpc},
        {"id", id}
    };

    if (result) {
        j["result"] = *result;
    }

    if (error) {
        j["error"] = *error;
    }

    return j;
}

// ============================================================================
// NotificationMessage
// ============================================================================

json NotificationMessage::toJson() const {
    return {
        {"jsonrpc", jsonrpc},
        {"method", method},
        {"params", params}
    };
}

std::optional<NotificationMessage> NotificationMessage::fromJson(const json& j) {
    if (!j.contains("method")) {
        return std::nullopt;
    }

    NotificationMessage notif;
    notif.method = j["method"].get<std::string>();
    notif.params = j.value("params", json::object());

    return notif;
}

// ============================================================================
// JsonRpcTransport
// ============================================================================

JsonRpcTransport::JsonRpcTransport() {
    // Set stdin/stdout to binary mode
    std::ios::sync_with_stdio(false);
}

std::optional<std::string> JsonRpcTransport::readMessage() {
    // Read headers
    std::string headers = readHeaders();
    if (headers.empty()) {
        return std::nullopt;
    }

    // Parse Content-Length
    size_t content_length = 0;
    std::istringstream iss(headers);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.find("Content-Length:") == 0) {
            content_length = std::stoul(line.substr(15));
            break;
        }
    }

    if (content_length == 0) {
        return std::nullopt;
    }

    // Read content
    return readContent(content_length);
}

void JsonRpcTransport::writeMessage(const std::string& message) {
    writeHeaders(message.length());
    std::cout << message << std::flush;
}

void JsonRpcTransport::writeResponse(const ResponseMessage& response) {
    std::string message = response.toJson().dump();
    writeMessage(message);
}

std::string JsonRpcTransport::readHeaders() {
    std::string headers;
    std::string line;

    while (std::getline(std::cin, line)) {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Empty line marks end of headers
        if (line.empty()) {
            break;
        }

        headers += line + "\n";
    }

    return headers;
}

std::string JsonRpcTransport::readContent(size_t length) {
    std::string content(length, '\0');
    std::cin.read(&content[0], length);
    return content;
}

void JsonRpcTransport::writeHeaders(size_t content_length) {
    std::cout << "Content-Length: " << content_length << "\r\n\r\n";
}

} // namespace lsp
} // namespace naab
