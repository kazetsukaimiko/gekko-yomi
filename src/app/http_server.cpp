#include "http_server.hpp"

#include "bmp.hpp"
#include "compose.hpp"
#include "json_mini.hpp"
#include "log.hpp"
#include "page.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gekko::yomi {
namespace {

constexpr std::size_t kMaxHeader = 64 * 1024;
constexpr std::size_t kMaxBody = 1024 * 1024;

bool write_all(int fd, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

void respond(int fd, int status, const char* reason, const std::string& type, const std::uint8_t* body,
             std::size_t size) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                       "\r\nContent-Type: " + type +
                       "\r\nContent-Length: " + std::to_string(size) +
                       "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    write_all(fd, head.data(), head.size());
    if (size > 0 && body != nullptr) {
        write_all(fd, reinterpret_cast<const char*>(body), size);
    }
}

void respond_text(int fd, int status, const char* reason, const std::string& type, const std::string& body) {
    respond(fd, status, reason, type, reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
}

bool read_headers(int fd, std::string* headers, std::string* error) {
    headers->clear();
    while (headers->find("\r\n\r\n") == std::string::npos) {
        if (headers->size() > kMaxHeader) {
            *error = "headers are too large";
            return false;
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 5000);
        if (ready <= 0) {
            *error = "timed out reading headers";
            return false;
        }
        char buf[2048];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = "recv failed";
            return false;
        }
        if (n == 0) {
            *error = "client closed before headers ended";
            return false;
        }
        headers->append(buf, static_cast<std::size_t>(n));
    }
    return true;
}

bool read_body(int fd, std::string* buffer, std::size_t already, std::size_t length, std::string* error) {
    while (buffer->size() < already + length) {
        if (buffer->size() > kMaxHeader + kMaxBody) {
            *error = "body is too large";
            return false;
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 5000);
        if (ready <= 0) {
            *error = "timed out reading body";
            return false;
        }
        char buf[4096];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = "recv failed";
            return false;
        }
        if (n == 0) {
            break;
        }
        buffer->append(buf, static_cast<std::size_t>(n));
    }
    return buffer->size() >= already + length;
}

std::string path_only(std::string target) {
    const auto query = target.find('?');
    if (query != std::string::npos) {
        target.resize(query);
    }
    return target;
}

}  // namespace

HttpServer::HttpServer(Registry* registry, OcrBufferOptions buffer,
                       std::optional<std::vector<std::string>> ocr_engines)
    : registry_(registry), memory_(std::move(buffer)), ocr_engines_(std::move(ocr_engines)) {}

HttpServer::~HttpServer() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
    }
}

bool HttpServer::listen(int port, std::string* error) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        *error = "socket failed";
        return false;
    }
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        *error = std::string("bind failed: ") + std::strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 16) != 0) {
        *error = "listen failed";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        *error = "getsockname failed";
        return false;
    }
    port_ = ntohs(bound.sin_port);
    return true;
}

void HttpServer::stop() { stop_.store(true); }

void HttpServer::request_config(CoreConfig config) {
    std::lock_guard<std::mutex> lock(pending_mu_);
    pending_ = std::move(config);
    pending_ready_ = true;
}

void HttpServer::apply_pending_config() {
    CoreConfig config;
    {
        std::lock_guard<std::mutex> lock(pending_mu_);
        if (!pending_ready_) {
            return;
        }
        config = std::move(pending_);
        pending_ready_ = false;
    }

    memory_.set_options(config.buffer);
    const std::vector<std::string> known = registry_ == nullptr ? std::vector<std::string>{} : registry_->registered_ocr_ids();
    auto wanted = [&](const std::string& id) {
        if (!config.ocr_engines) {
            return true;
        }
        return std::find(config.ocr_engines->begin(), config.ocr_engines->end(), id) != config.ocr_engines->end();
    };
    if (registry_ != nullptr) {
        for (const auto& id : known) {
            bool live = false;
            for (const auto& engine : registry_->ocr_engines()) {
                if (engine && engine->id() == id) {
                    live = true;
                    break;
                }
            }
            if (wanted(id) && !live) {
                const Status created = registry_->create_instance(id);
                if (!created.ok()) {
                    logger("app").error("could not start {}: {}", id, created.error);
                }
            } else if (!wanted(id) && live) {
                const Status released = registry_->release_instance(id);
                if (!released.ok()) {
                    logger("app").error("could not stop {}: {}", id, released.error);
                }
            }
        }
        if (config.ocr_engines) {
            for (const auto& id : *config.ocr_engines) {
                if (std::find(known.begin(), known.end(), id) == known.end()) {
                    logger("app").warn("ocr engine {} is not loaded", id);
                }
            }
        }
    }
    ocr_engines_ = std::move(config.ocr_engines);
    if (ocr_engines_) {
        std::string joined;
        for (const auto& id : *ocr_engines_) {
            if (!joined.empty()) {
                joined += ", ";
            }
            joined += id;
        }
        logger("app").info("config applied ttl_scans={} positional_invalidation_scans={} engines={}",
                           config.buffer.ttl_scans, config.buffer.positional_invalidation_scans,
                           joined.empty() ? "none" : joined);
    } else {
        logger("app").info("config applied ttl_scans={} positional_invalidation_scans={} engines=all loaded",
                           config.buffer.ttl_scans, config.buffer.positional_invalidation_scans);
    }
}

void HttpServer::run() {
    while (!stop_.load()) {
        apply_pending_config();
        pollfd pfd{};
        pfd.fd = listen_fd_;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 200);
        if (stop_.load()) {
            break;
        }
        if (ready <= 0) {
            continue;
        }
        const int client = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            continue;
        }
        handle_client(client);
        ::close(client);
    }
}

void HttpServer::handle_client(int client) {
    std::string message;
    std::string error;
    if (!read_headers(client, &message, &error)) {
        respond_text(client, 400, "Bad Request", "text/plain; charset=utf-8", error);
        return;
    }
    const auto header_end = message.find("\r\n\r\n");
    const std::string headers = message.substr(0, header_end);
    const auto line_end = headers.find("\r\n");
    const std::string request_line = headers.substr(0, line_end);
    const auto sp1 = request_line.find(' ');
    const auto sp2 = request_line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        respond_text(client, 400, "Bad Request", "text/plain; charset=utf-8", "bad request line");
        return;
    }
    const std::string method = request_line.substr(0, sp1);
    const std::string path = path_only(request_line.substr(sp1 + 1, sp2 - sp1 - 1));

    std::size_t content_length = 0;
    const auto length_at = headers.find("Content-Length:");
    if (length_at == std::string::npos) {
        const auto lower = headers.find("content-length:");
        if (lower != std::string::npos) {
            content_length = static_cast<std::size_t>(std::strtoul(headers.c_str() + lower + 15, nullptr, 10));
        }
    } else {
        content_length = static_cast<std::size_t>(std::strtoul(headers.c_str() + length_at + 15, nullptr, 10));
    }
    if (content_length > kMaxBody) {
        respond_text(client, 413, "Payload Too Large", "text/plain; charset=utf-8", "body is too large");
        return;
    }
    const std::size_t body_at = header_end + 4;
    if (content_length > 0 && !read_body(client, &message, body_at, content_length, &error)) {
        respond_text(client, 400, "Bad Request", "text/plain; charset=utf-8", error);
        return;
    }
    const std::string body = message.substr(body_at, content_length);

    if (method == "GET" && path == "/") {
        logger("http").debug("GET /");
        const std::string page = page_html();
        respond_text(client, 200, "OK", "text/html; charset=utf-8", page);
        return;
    }
    if (method == "GET" && path == "/api/windows") {
        logger("http").debug("GET /api/windows");
        const std::string json = windows_json(*registry_);
        if (registry_->capture_sources().empty()) {
            logger("capture").warn("no capture plugin is loaded");
        } else if (json.find("\"capture_error\":\"\"") == std::string::npos) {
            logger("capture").warn("{}", json);
        }
        respond_text(client, 200, "OK", "application/json; charset=utf-8", json);
        return;
    }
    if (method == "GET" && path == "/api/frame.bmp") {
        if (!have_frame_) {
            logger("http").info("GET /api/frame.bmp: no frame yet");
            respond_text(client, 404, "Not Found", "text/plain; charset=utf-8", "no frame yet");
            return;
        }
        const auto bmp = encode_bmp(last_frame_);
        if (bmp.empty()) {
            logger("http").error("GET /api/frame.bmp: could not encode bmp");
            respond_text(client, 500, "Internal Server Error", "text/plain; charset=utf-8", "could not encode bmp");
            return;
        }
        logger("http").info("GET /api/frame.bmp {}x{}", last_frame_.width, last_frame_.height);
        respond(client, 200, "OK", "image/bmp", bmp.data(), bmp.size());
        return;
    }
    if (method == "POST" && path == "/api/read") {
        std::string parse_error;
        const auto json = json_parse(body, &parse_error);
        if (!json) {
            logger("http").warn("POST /api/read: {}", parse_error);
            respond_text(client, 400, "Bad Request", "text/plain; charset=utf-8", parse_error);
            return;
        }
        const std::string window_id = json->field_string("window_id");
        if (window_id.empty()) {
            logger("http").warn("POST /api/read: window_id is required");
            respond_text(client, 400, "Bad Request", "text/plain; charset=utf-8", "window_id is required");
            return;
        }
        const Json* watch_field = json->field("watch");
        const bool watch = watch_field != nullptr && watch_field->type == Json::Type::Bool && watch_field->boolean;
        const auto noted = watch ? spdlog::level::debug : spdlog::level::info;
        logger("http").log(noted, "POST /api/read {}", window_id);
        if (window_id != memory_window_) {
            memory_.clear();
            memory_window_ = window_id;
        }
        const std::vector<std::string>* enabled = ocr_engines_ ? &(*ocr_engines_) : nullptr;
        ReadView view = read_window(*registry_, window_id, &memory_, enabled);
        if (view.capture_error.empty() && view.image_width > 0) {
            logger("capture").log(noted, "{}x{} \"{}\"", view.image_width, view.image_height, view.title);
            last_frame_ = std::move(view.frame);
            have_frame_ = true;
        } else {
            logger("capture").error("{}", view.capture_error.empty() ? "capture failed" : view.capture_error);
        }
        if (!view.ocr_error.empty()) {
            logger("ocr").error("{}", view.ocr_error);
        } else if (!view.engine_id.empty()) {
            logger("ocr").log(noted, "{} runs {}", view.engine_id, view.runs.size());
        }
        if (!view.reading_error.empty()) {
            logger("reading").error("{}", view.reading_error);
        }
        if (!view.dictionary_error.empty()) {
            logger("dictionary").error("{}", view.dictionary_error);
        }
        std::string engines;
        for (const auto& engine : view.timings.engines) {
            engines += ' ';
            engines += engine.id;
            engines += '=';
            engines += std::to_string(engine.milliseconds);
            engines += "ms";
        }
        // Info on a watch too. The other watch lines stay at debug, and this
        // is the line that shows where a scan spent its time.
        logger("http").info("scan \"{}\" total={}ms list={}ms capture={}ms ocr={}ms{} buffer={}ms language={}ms runs={}",
                            view.title, view.timings.total_ms, view.timings.list_ms, view.timings.capture_ms,
                            view.timings.ocr_ms, engines, view.timings.buffer_ms, view.timings.language_ms,
                            view.runs.size());
        respond_text(client, 200, "OK", "application/json; charset=utf-8", read_json(view));
        return;
    }
    if (path == "/favicon.ico") {
        respond_text(client, 204, "No Content", "text/plain", "");
        return;
    }
    logger("http").info("{} {} not found", method, path);
    respond_text(client, 404, "Not Found", "text/plain; charset=utf-8", "not found");
}

}  // namespace gekko::yomi
