#include "worker_session.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace gekko::yomi::ocr_worker {
namespace {

bool write_all(int fd, const char* data, std::size_t size, std::string* error) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t n = ::write(fd, data + sent, size - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = std::string("write to the OCR worker failed: ") + std::strerror(errno);
            return false;
        }
        if (n == 0) {
            *error = "write to the OCR worker wrote nothing";
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string executable_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) {
        return {};
    }
    buf[n] = '\0';
    return std::filesystem::path(buf).parent_path().string();
}

}  // namespace

WorkerSession::~WorkerSession() { close(); }

void WorkerSession::close() {
    if (to_child_ >= 0) {
        ::close(to_child_);
        to_child_ = -1;
    }
    if (from_child_ >= 0) {
        ::close(from_child_);
        from_child_ = -1;
    }
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        ::waitpid(pid_, nullptr, 0);
        pid_ = -1;
    }
}

bool WorkerSession::reap_if_exited() {
    if (pid_ <= 0) {
        return false;
    }
    int status = 0;
    const pid_t got = ::waitpid(pid_, &status, WNOHANG);
    if (got == 0) {
        return false;
    }
    if (got == pid_ || (got < 0 && errno == ECHILD)) {
        pid_ = -1;
        if (to_child_ >= 0) {
            ::close(to_child_);
            to_child_ = -1;
        }
        if (from_child_ >= 0) {
            ::close(from_child_);
            from_child_ = -1;
        }
        return true;
    }
    return false;
}

bool WorkerSession::running() {
    reap_if_exited();
    return pid_ > 0 && to_child_ >= 0 && from_child_ >= 0;
}

std::optional<std::string> WorkerSession::read_reply_line(int timeout_ms, std::string* error) {
    std::string line;
    const int slice = 200;
    int waited = 0;
    while (line.find('\n') == std::string::npos) {
        if (waited >= timeout_ms) {
            *error = line.empty() ? "timed out waiting for the OCR worker" : line;
            return std::nullopt;
        }
        pollfd pfd{};
        pfd.fd = from_child_;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, slice);
        waited += slice;
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = std::string("poll on the OCR worker failed: ") + std::strerror(errno);
            return std::nullopt;
        }
        if (ready == 0) {
            continue;
        }
        char buf[1024];
        const ssize_t n = ::read(from_child_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = std::string("read from the OCR worker failed: ") + std::strerror(errno);
            return std::nullopt;
        }
        if (n == 0) {
            *error = line.empty() ? "OCR worker closed stdout" : line;
            return std::nullopt;
        }
        line.append(buf, static_cast<std::size_t>(n));
        if (line.size() > 8 * 1024 * 1024) {
            *error = "OCR worker reply is too large";
            return std::nullopt;
        }
    }
    const auto newline = line.find('\n');
    line.resize(newline);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

bool WorkerSession::start(const std::string& python, const std::string& script, std::string* error) {
    // A client that goes away mid-write must not kill the process.
    ::signal(SIGPIPE, SIG_IGN);
    close();
    if (python.empty() || script.empty()) {
        *error = "OCR worker python or script path is empty";
        return false;
    }
    if (!std::filesystem::is_regular_file(script)) {
        *error = "OCR worker script not found: " + script;
        return false;
    }
    int inbound[2] = {-1, -1};
    int outbound[2] = {-1, -1};
    if (::pipe2(inbound, O_CLOEXEC) != 0) {
        *error = std::string("pipe failed: ") + std::strerror(errno);
        return false;
    }
    if (::pipe2(outbound, O_CLOEXEC) != 0) {
        *error = std::string("pipe failed: ") + std::strerror(errno);
        ::close(inbound[0]);
        ::close(inbound[1]);
        return false;
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        *error = std::string("fork failed: ") + std::strerror(errno);
        ::close(inbound[0]);
        ::close(inbound[1]);
        ::close(outbound[0]);
        ::close(outbound[1]);
        return false;
    }
    if (pid == 0) {
        ::dup2(inbound[0], STDIN_FILENO);
        ::dup2(outbound[1], STDOUT_FILENO);
        ::close(inbound[0]);
        ::close(inbound[1]);
        ::close(outbound[0]);
        ::close(outbound[1]);
        ::execlp(python.c_str(), python.c_str(), script.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    ::close(inbound[0]);
    ::close(outbound[1]);
    to_child_ = inbound[1];
    from_child_ = outbound[0];
    pid_ = pid;
    return true;
}

std::optional<std::string> WorkerSession::request(std::string_view header_json, const std::uint8_t* pixels,
                                                  std::size_t nbytes, int timeout_ms, std::string* error) {
    if (pid_ <= 0 || to_child_ < 0 || from_child_ < 0) {
        *error = "OCR worker is not running";
        return std::nullopt;
    }
    std::string header(header_json);
    header.push_back('\n');
    // A worker that exits before reading (import error) still writes one JSON
    // line. Prefer that line over the EPIPE from the pixel write.
    auto finish = [&](std::optional<std::string> line) {
        reap_if_exited();
        return line;
    };
    if (!write_all(to_child_, header.data(), header.size(), error)) {
        std::string write_error = *error;
        auto line = read_reply_line(300, error);
        if (!line) {
            *error = write_error;
        }
        return finish(std::move(line));
    }
    if (nbytes > 0 && !write_all(to_child_, reinterpret_cast<const char*>(pixels), nbytes, error)) {
        std::string write_error = *error;
        auto line = read_reply_line(300, error);
        if (!line) {
            *error = write_error;
        }
        return finish(std::move(line));
    }
    auto line = read_reply_line(timeout_ms, error);
    return finish(std::move(line));
}

std::string default_python() {
    if (const char* env = std::getenv("GEKKO_YOMI_PYTHON")) {
        if (env[0] != '\0') {
            return env;
        }
    }
    const auto dir = executable_dir();
    if (!dir.empty()) {
        const auto venv = std::filesystem::path(dir) / ".." / ".venv" / "bin" / "python";
        std::error_code ec;
        if (std::filesystem::is_regular_file(venv, ec)) {
            return venv.lexically_normal().string();
        }
    }
    return "python3";
}

std::string worker_script(const char* env_name, const char* filename) {
    if (env_name != nullptr) {
        if (const char* env = std::getenv(env_name)) {
            if (env[0] != '\0') {
                return env;
            }
        }
    }
    const auto dir = executable_dir();
    if (dir.empty() || filename == nullptr || filename[0] == '\0') {
        return {};
    }
    return (std::filesystem::path(dir) / "workers" / filename).lexically_normal().string();
}

}  // namespace gekko::yomi::ocr_worker
