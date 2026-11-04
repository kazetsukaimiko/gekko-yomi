#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace gekko::yomi::meiki {

// One persistent Python process. A request is a JSON line plus raw bytes.
// The reply is one JSON line. stderr is inherited so model logs stay visible.
class WorkerSession {
public:
    WorkerSession() = default;
    ~WorkerSession();

    WorkerSession(const WorkerSession&) = delete;
    WorkerSession& operator=(const WorkerSession&) = delete;

    bool start(const std::string& python, const std::string& script, std::string* error);
    void close();
    // Reaps a worker that already exited, so the next recognize() starts a new one.
    bool running();

    // timeout_ms waits for the reply line. The write of the image can block
    // until the worker reads, which is what keeps a full pipe from deadlocking.
    std::optional<std::string> request(std::string_view header_json, const std::uint8_t* pixels, std::size_t nbytes,
                                       int timeout_ms, std::string* error);

private:
    // True when the child has exited. Closes the pipes in that case.
    bool reap_if_exited();
    // One reply line, if the worker wrote one before timeout_ms.
    std::optional<std::string> read_reply_line(int timeout_ms, std::string* error);

    int to_child_{-1};
    int from_child_{-1};
    int pid_{-1};
};

std::string default_python();
std::string default_worker_script();

}  // namespace gekko::yomi::meiki
