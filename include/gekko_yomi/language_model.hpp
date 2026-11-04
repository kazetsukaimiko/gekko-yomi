#pragma once

#include "gekko_yomi/frame.hpp"
#include "gekko_yomi/result.hpp"

#include <string>
#include <vector>

namespace gekko::yomi {

// One turn in an OpenAI-compatible chat request.
// The plugin turns this into the JSON that llama-run (or any other
// server on that API) expects. The core does not speak HTTP.
struct ChatMessage {
    std::string role;
    std::string text;
};

struct ChatRequest {
    // Empty means the plugin's default model name.
    std::string model;
    std::vector<ChatMessage> messages;
    // Attached to the conversation by the plugin, typically the last user turn.
    // Empty for a text-only call.
    std::vector<Frame> images;
};

struct ChatResponse {
    std::string text;
    std::string model;
};

class LanguageModel {
public:
    virtual ~LanguageModel() = default;

    virtual std::string id() const = 0;
    virtual std::string description() const = 0;

    virtual Result<ChatResponse> complete(const ChatRequest& request) = 0;
};

}  // namespace gekko::yomi
