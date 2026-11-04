#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace gekko::yomi::kakasi {

// One persistent Python process. A request is one JSON line. The reply is
// one JSON line. stderr is inherited so import errors stay visible.
class TextWorker {
public:
    TextWorker() = default;
    ~TextWorker();

    TextWorker(const TextWorker&) = delete;
    TextWorker& operator=(const TextWorker&) = delete;

    bool start(const std::string& python, const std::string& script, std::string* error);
    void close();
    // Reaps a worker that already exited, so the next analyze() starts a new one.
    bool running();

    std::optional<std::string> request(std::string_view line, int timeout_ms, std::string* error);

private:
    bool reap_if_exited();
    std::optional<std::string> read_reply_line(int timeout_ms, std::string* error);

    int to_child_{-1};
    int from_child_{-1};
    int pid_{-1};
};

std::string default_python();
std::string default_worker_script();

}  // namespace gekko::yomi::kakasi
