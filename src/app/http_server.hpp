#pragma once

#include "config.hpp"

#include "gekko_yomi/frame.hpp"
#include "gekko_yomi/ocr_buffer.hpp"
#include "gekko_yomi/registry.hpp"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gekko::yomi {

// One connection at a time, on the calling thread. The socket is 0.0.0.0.
class HttpServer {
public:
    // buffer is the scan memory. ocr_engines nullopt runs every loaded engine.
    explicit HttpServer(Registry* registry, OcrBufferOptions buffer = {},
                        std::optional<std::vector<std::string>> ocr_engines = std::nullopt);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // port 0 asks the kernel for a free port. On failure error is set.
    bool listen(int port, std::string* error);
    int port() const { return port_; }

    // Returns when stop() is called. Polls the socket so stop is prompt.
    void run();
    void stop();

    // The Qt thread posts a config. run() applies it on the server thread
    // between requests: buffer knobs, which engines are called, and
    // release_instance / create_instance for engines that changed.
    void request_config(CoreConfig config);

    const Frame* last_frame() const { return have_frame_ ? &last_frame_ : nullptr; }

private:
    void handle_client(int client);
    void apply_pending_config();

    Registry* registry_;
    int listen_fd_{-1};
    int port_{0};
    std::atomic<bool> stop_{false};
    Frame last_frame_;
    bool have_frame_{false};
    // Buffers stay with the window currently being read. A new id clears them.
    OcrConsensus memory_;
    std::string memory_window_;
    std::optional<std::vector<std::string>> ocr_engines_;
    std::mutex pending_mu_;
    bool pending_ready_{false};
    CoreConfig pending_{};
};

}  // namespace gekko::yomi
