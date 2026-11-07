#include "window_ref.hpp"

#include "gekko_yomi/host.hpp"

#include <systemd/sd-bus.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gekko::yomi::kwin {
namespace {

constexpr std::uint64_t kCallTimeoutUsec = 20ull * 1000ull * 1000ull;
constexpr std::uint64_t kMaxScreenshotBytes = 256ull * 1024ull * 1024ull;

std::string errno_text(int rc) {
    return std::strerror(rc < 0 ? -rc : rc);
}

std::string dbus_text(int rc, const sd_bus_error& error) {
    std::string text = errno_text(rc);
    if (error.name != nullptr) {
        text += ": ";
        text += error.name;
    }
    if (error.message != nullptr) {
        text += ": ";
        text += error.message;
    }
    return text;
}

bool access_denied(const sd_bus_error& error) {
    const std::string name = error.name != nullptr ? error.name : "";
    const std::string message = error.message != nullptr ? error.message : "";
    return name.find("Access") != std::string::npos || name.find("Auth") != std::string::npos ||
           message.find("not authorized") != std::string::npos ||
           message.find("Restricted") != std::string::npos;
}

struct Bus {
    sd_bus* bus{nullptr};

    ~Bus() { sd_bus_unref(bus); }

    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;
    Bus() = default;

    bool open(std::string* error) {
        const int rc = sd_bus_open_user(&bus);
        if (rc < 0) {
            *error = "KWin is not available: " + errno_text(rc);
            return false;
        }
        return true;
    }

    bool owns(const char* name, std::string* error) {
        sd_bus_error dbus_error = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        const int rc = sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                          "org.freedesktop.DBus", "NameHasOwner", &dbus_error, &reply, "s",
                                          name);
        if (rc < 0) {
            *error = "KWin is not available: " + dbus_text(rc, dbus_error);
            sd_bus_error_free(&dbus_error);
            sd_bus_message_unref(reply);
            return false;
        }
        int has = 0;
        sd_bus_message_read_basic(reply, 'b', &has);
        sd_bus_message_unref(reply);
        sd_bus_error_free(&dbus_error);
        if (has == 0) {
            *error = std::string("KWin is not available: ") + name + " is not on the session bus";
            return false;
        }
        return true;
    }
};

struct Message {
    sd_bus_message* message{nullptr};
    ~Message() { sd_bus_message_unref(message); }
    Message() = default;
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;
};

int append_bool(sd_bus_message* message, const char* key, int value) {
    int rc = sd_bus_message_open_container(message, 'e', "sv");
    if (rc < 0) {
        return rc;
    }
    rc = sd_bus_message_append_basic(message, 's', key);
    if (rc < 0) {
        return rc;
    }
    rc = sd_bus_message_open_container(message, 'v', "b");
    if (rc < 0) {
        return rc;
    }
    rc = sd_bus_message_append_basic(message, 'b', &value);
    if (rc < 0) {
        return rc;
    }
    rc = sd_bus_message_close_container(message);
    if (rc < 0) {
        return rc;
    }
    return sd_bus_message_close_container(message);
}

bool skip_variant(sd_bus_message* message, std::string* error) {
    const int rc = sd_bus_message_skip(message, "v");
    if (rc < 0) {
        *error = "could not skip a D-Bus variant: " + errno_text(rc);
        return false;
    }
    return true;
}

bool read_string_variant(sd_bus_message* message, std::string* out, std::string* error) {
    const char* contents = nullptr;
    int rc = sd_bus_message_peek_type(message, nullptr, &contents);
    if (rc < 0 || contents == nullptr || std::strcmp(contents, "s") != 0) {
        return skip_variant(message, error);
    }
    rc = sd_bus_message_enter_container(message, 'v', "s");
    if (rc < 0) {
        *error = "could not read a string variant: " + errno_text(rc);
        return false;
    }
    const char* value = nullptr;
    rc = sd_bus_message_read_basic(message, 's', &value);
    if (rc < 0) {
        *error = "could not read a string: " + errno_text(rc);
        return false;
    }
    *out = value != nullptr ? value : "";
    sd_bus_message_exit_container(message);
    return true;
}

bool read_bool_variant(sd_bus_message* message, bool* out, std::string* error) {
    const char* contents = nullptr;
    int rc = sd_bus_message_peek_type(message, nullptr, &contents);
    if (rc < 0 || contents == nullptr || std::strcmp(contents, "b") != 0) {
        return skip_variant(message, error);
    }
    rc = sd_bus_message_enter_container(message, 'v', "b");
    if (rc < 0) {
        *error = "could not read a bool variant: " + errno_text(rc);
        return false;
    }
    int value = 0;
    rc = sd_bus_message_read_basic(message, 'b', &value);
    if (rc < 0) {
        *error = "could not read a bool: " + errno_text(rc);
        return false;
    }
    *out = value != 0;
    sd_bus_message_exit_container(message);
    return true;
}

bool read_number_variant(sd_bus_message* message, double* out, std::string* error) {
    const char* contents = nullptr;
    int rc = sd_bus_message_peek_type(message, nullptr, &contents);
    if (rc < 0 || contents == nullptr || contents[0] == '\0' || contents[1] != '\0') {
        return skip_variant(message, error);
    }
    const char kind = contents[0];
    if (kind != 'd' && kind != 'i' && kind != 'u' && kind != 'n' && kind != 'q' && kind != 'x' &&
        kind != 't') {
        return skip_variant(message, error);
    }
    rc = sd_bus_message_enter_container(message, 'v', contents);
    if (rc < 0) {
        *error = "could not read a number variant: " + errno_text(rc);
        return false;
    }
    if (kind == 'd') {
        rc = sd_bus_message_read_basic(message, 'd', out);
    } else if (kind == 't') {
        std::uint64_t wide = 0;
        rc = sd_bus_message_read_basic(message, 't', &wide);
        *out = static_cast<double>(wide);
    } else if (kind == 'x') {
        std::int64_t wide = 0;
        rc = sd_bus_message_read_basic(message, 'x', &wide);
        *out = static_cast<double>(wide);
    } else if (kind == 'u') {
        std::uint32_t narrow = 0;
        rc = sd_bus_message_read_basic(message, 'u', &narrow);
        *out = static_cast<double>(narrow);
    } else if (kind == 'q') {
        std::uint16_t narrow = 0;
        rc = sd_bus_message_read_basic(message, 'q', &narrow);
        *out = static_cast<double>(narrow);
    } else if (kind == 'n') {
        std::int16_t narrow = 0;
        rc = sd_bus_message_read_basic(message, 'n', &narrow);
        *out = static_cast<double>(narrow);
    } else {
        std::int32_t narrow = 0;
        rc = sd_bus_message_read_basic(message, 'i', &narrow);
        *out = static_cast<double>(narrow);
    }
    if (rc < 0) {
        *error = "could not read a number: " + errno_text(rc);
        return false;
    }
    sd_bus_message_exit_container(message);
    return true;
}

struct WindowDetails {
    WindowInfo info;
    int type{0};
    bool saw_type{false};
    bool saw_geometry_width{false};
};

bool read_window_details(Bus& bus, const std::string& uuid, WindowDetails* details, std::string* error) {
    sd_bus_error dbus_error = SD_BUS_ERROR_NULL;
    Message reply;
    const int rc = sd_bus_call_method(bus.bus, "org.kde.KWin", "/KWin", "org.kde.KWin", "getWindowInfo",
                                      &dbus_error, &reply.message, "s", uuid.c_str());
    if (rc < 0) {
        *error = "getWindowInfo failed: " + dbus_text(rc, dbus_error);
        sd_bus_error_free(&dbus_error);
        return false;
    }
    sd_bus_error_free(&dbus_error);

    int enter = sd_bus_message_enter_container(reply.message, 'a', "{sv}");
    if (enter < 0) {
        *error = "getWindowInfo reply was not a dict: " + errno_text(enter);
        return false;
    }
    details->info.id = uuid;
    while ((enter = sd_bus_message_enter_container(reply.message, 'e', "sv")) > 0) {
        const char* key = nullptr;
        const int key_rc = sd_bus_message_read_basic(reply.message, 's', &key);
        if (key_rc < 0 || key == nullptr) {
            *error = "getWindowInfo dict key was unreadable";
            return false;
        }
        const std::string name = key;
        bool ok = true;
        if (name == "caption") {
            ok = read_string_variant(reply.message, &details->info.title, error);
        } else if (name == "resourceClass") {
            std::string resource;
            ok = read_string_variant(reply.message, &resource, error);
            if (ok && !resource.empty()) {
                details->info.app_id = std::move(resource);
            }
        } else if (name == "desktopFile") {
            std::string desktop;
            ok = read_string_variant(reply.message, &desktop, error);
            if (ok && details->info.app_id.empty()) {
                details->info.app_id = desktop;
            }
        } else if (name == "uuid") {
            std::string id;
            ok = read_string_variant(reply.message, &id, error);
            if (ok && !id.empty()) {
                details->info.id = id;
            }
        } else if (name == "minimized") {
            ok = read_bool_variant(reply.message, &details->info.minimized, error);
        } else if (name == "type") {
            double type = 0;
            ok = read_number_variant(reply.message, &type, error);
            details->type = static_cast<int>(type);
            details->saw_type = true;
        } else if (name == "x") {
            ok = read_number_variant(reply.message, &details->info.geometry.x, error);
        } else if (name == "y") {
            ok = read_number_variant(reply.message, &details->info.geometry.y, error);
        } else if (name == "width") {
            ok = read_number_variant(reply.message, &details->info.geometry.width, error);
            details->saw_geometry_width = ok;
        } else if (name == "height") {
            ok = read_number_variant(reply.message, &details->info.geometry.height, error);
        } else {
            ok = skip_variant(reply.message, error);
        }
        if (!ok) {
            return false;
        }
        sd_bus_message_exit_container(reply.message);
    }
    if (enter < 0) {
        *error = "getWindowInfo dict ended early: " + errno_text(enter);
        return false;
    }
    sd_bus_message_exit_container(reply.message);
    return true;
}

std::string executable_path() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) {
        return {};
    }
    buf[n] = '\0';
    return std::string(buf, static_cast<std::size_t>(n));
}

int run_program(const char* name, int timeout_sec) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        ::execlp(name, name, static_cast<char*>(nullptr));
        _exit(127);
    }
    for (int i = 0; i < timeout_sec * 10; ++i) {
        int status = 0;
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        ::usleep(100000);
    }
    ::kill(pid, SIGTERM);
    ::waitpid(pid, nullptr, 0);
    return -1;
}

// Plasma used to authorize ScreenShot2 only when /proc/pid/exe matches a
// desktop file that lists the interface. KWin 6.8 does not check that in
// the screenshot interface itself. Write the file anyway, and refresh the
// service cache only when the file is new or a capture is refused.
void ensure_desktop_file() {
    const std::string exe = executable_path();
    if (exe.empty() || std::filesystem::path(exe).filename() != "gekko-yomi") {
        return;
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        return;
    }
    const auto path = std::filesystem::path(home) / ".local" / "share" / "applications" / "gekko-yomi.desktop";
    const std::string body = "[Desktop Entry]\n"
                             "Type=Application\n"
                             "Name=Gekko Yomi\n"
                             "NoDisplay=true\n"
                             "Exec=" +
                             exe +
                             "\n"
                             "X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n";
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        // A matching file is left alone so a grab does not rebuild the cache.
        std::FILE* existing = std::fopen(path.c_str(), "rb");
        if (existing != nullptr) {
            std::string current;
            char buf[256];
            std::size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), existing)) > 0) {
                current.append(buf, n);
            }
            std::fclose(existing);
            if (current == body) {
                return;
            }
        }
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return;
    }
    const bool wrote = std::fwrite(body.data(), 1, body.size(), file) == body.size();
    std::fclose(file);
    if (wrote) {
        run_program("kbuildsycoca6", 15);
    }
}

struct RawShot {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t stride{0};
    std::uint32_t format{0};
    double scale{0};
    std::vector<std::uint8_t> pixels;
};

bool read_exact(int fd, std::uint8_t* dest, std::uint64_t nbytes, std::string* error) {
    std::uint64_t got = 0;
    while (got < nbytes) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, 20000);
        if (ready == 0) {
            *error = "timed out reading the screenshot pipe";
            return false;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = std::string("poll on the screenshot pipe failed: ") + std::strerror(errno);
            return false;
        }
        const ssize_t n = ::read(fd, dest + got, static_cast<std::size_t>(nbytes - got));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            *error = std::string("read of the screenshot pipe failed: ") + std::strerror(errno);
            return false;
        }
        if (n == 0) {
            *error = "screenshot pipe closed after " + std::to_string(got) + " of " + std::to_string(nbytes) +
                     " bytes";
            return false;
        }
        got += static_cast<std::uint64_t>(n);
    }
    return true;
}

bool capture_once(Bus& bus, const std::string& uuid, RawShot* shot, sd_bus_error* dbus_error, std::string* error) {
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        *error = std::string("pipe failed: ") + std::strerror(errno);
        return false;
    }
    // A larger pipe is optional. The writer blocks, and we read after the reply.
    ::fcntl(fds[0], F_SETPIPE_SZ, 1 << 20);

    Message call;
    int rc = sd_bus_message_new_method_call(bus.bus, &call.message, "org.kde.KWin.ScreenShot2",
                                            "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2",
                                            "CaptureWindow");
    if (rc < 0) {
        *error = "could not build CaptureWindow: " + errno_text(rc);
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    rc = sd_bus_message_append_basic(call.message, 's', uuid.c_str());
    if (rc >= 0) {
        rc = sd_bus_message_open_container(call.message, 'a', "{sv}");
    }
    if (rc >= 0) {
        rc = append_bool(call.message, "include-cursor", 0);
    }
    if (rc >= 0) {
        rc = append_bool(call.message, "include-decoration", 0);
    }
    // KWin defaults include-shadow to true. Shadows are not part of the page image.
    if (rc >= 0) {
        rc = append_bool(call.message, "include-shadow", 0);
    }
    if (rc >= 0) {
        rc = append_bool(call.message, "native-resolution", 0);
    }
    if (rc >= 0) {
        rc = sd_bus_message_close_container(call.message);
    }
    if (rc >= 0) {
        rc = sd_bus_message_append_basic(call.message, 'h', &fds[1]);
    }
    if (rc < 0) {
        *error = "could not fill CaptureWindow: " + errno_text(rc);
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }

    Message reply;
    rc = sd_bus_call(bus.bus, call.message, kCallTimeoutUsec, dbus_error, &reply.message);
    ::close(fds[1]);
    fds[1] = -1;
    if (rc < 0) {
        *error = "CaptureWindow failed: " + dbus_text(rc, *dbus_error);
        ::close(fds[0]);
        return false;
    }

    rc = sd_bus_message_enter_container(reply.message, 'a', "{sv}");
    if (rc < 0) {
        *error = "CaptureWindow reply was not a dict: " + errno_text(rc);
        ::close(fds[0]);
        return false;
    }
    std::string type;
    while ((rc = sd_bus_message_enter_container(reply.message, 'e', "sv")) > 0) {
        const char* key = nullptr;
        if (sd_bus_message_read_basic(reply.message, 's', &key) < 0 || key == nullptr) {
            *error = "CaptureWindow dict key was unreadable";
            ::close(fds[0]);
            return false;
        }
        const std::string name = key;
        bool ok = true;
        if (name == "type") {
            ok = read_string_variant(reply.message, &type, error);
        } else if (name == "width") {
            double value = 0;
            ok = read_number_variant(reply.message, &value, error);
            shot->width = static_cast<std::uint32_t>(value);
        } else if (name == "height") {
            double value = 0;
            ok = read_number_variant(reply.message, &value, error);
            shot->height = static_cast<std::uint32_t>(value);
        } else if (name == "stride") {
            double value = 0;
            ok = read_number_variant(reply.message, &value, error);
            shot->stride = static_cast<std::uint32_t>(value);
        } else if (name == "format") {
            double value = 0;
            ok = read_number_variant(reply.message, &value, error);
            shot->format = static_cast<std::uint32_t>(value);
        } else if (name == "scale") {
            ok = read_number_variant(reply.message, &shot->scale, error);
        } else {
            ok = skip_variant(reply.message, error);
        }
        if (!ok) {
            ::close(fds[0]);
            return false;
        }
        sd_bus_message_exit_container(reply.message);
    }
    if (rc < 0) {
        *error = "CaptureWindow dict ended early: " + errno_text(rc);
        ::close(fds[0]);
        return false;
    }
    if (type != "raw") {
        *error = "CaptureWindow returned type '" + type + "'";
        ::close(fds[0]);
        return false;
    }
    if (shot->width == 0 || shot->height == 0 ||
        static_cast<std::uint64_t>(shot->stride) < static_cast<std::uint64_t>(shot->width) * 4) {
        *error = "CaptureWindow returned an empty image";
        ::close(fds[0]);
        return false;
    }
    const std::uint64_t nbytes = static_cast<std::uint64_t>(shot->stride) * shot->height;
    if (nbytes > kMaxScreenshotBytes) {
        *error = "screenshot is larger than 256MB";
        ::close(fds[0]);
        return false;
    }
    shot->pixels.resize(static_cast<std::size_t>(nbytes));
    if (!read_exact(fds[0], shot->pixels.data(), nbytes, error)) {
        ::close(fds[0]);
        return false;
    }
    ::close(fds[0]);
    return true;
}

}  // namespace

class KWinCapture final : public CaptureSource {
public:
    std::string id() const override { return "kwin"; }
    std::string description() const override { return "KWin ScreenShot2 window capture"; }

    Result<std::vector<WindowInfo>> list_windows() override {
        std::string error;
        Bus bus;
        if (!bus.open(&error) || !bus.owns("org.kde.KWin", &error)) {
            return Result<std::vector<WindowInfo>>::failure(std::move(error));
        }

        sd_bus_error dbus_error = SD_BUS_ERROR_NULL;
        Message reply;
        const int rc = sd_bus_call_method(bus.bus, "org.kde.KWin", "/WindowsRunner", "org.kde.krunner1", "Match",
                                          &dbus_error, &reply.message, "s", "");
        if (rc < 0) {
            const std::string text = "Match failed: " + dbus_text(rc, dbus_error);
            sd_bus_error_free(&dbus_error);
            return Result<std::vector<WindowInfo>>::failure(text);
        }
        sd_bus_error_free(&dbus_error);

        const char* element = nullptr;
        char container = 0;
        int enter = sd_bus_message_peek_type(reply.message, &container, &element);
        if (enter < 0 || container != 'a' || element == nullptr) {
            return Result<std::vector<WindowInfo>>::failure("Match reply was not an array");
        }
        // Introspection says a(sssuda{sv}). Qt sends the match type as a signed int.
        std::string fields = element;
        if (!fields.empty() && fields.front() == '(' && fields.back() == ')') {
            fields = fields.substr(1, fields.size() - 2);
        }
        if (fields != "sssida{sv}" && fields != "sssuda{sv}") {
            return Result<std::vector<WindowInfo>>::failure("unexpected Match element " + fields);
        }
        const char type_code = fields[3];
        enter = sd_bus_message_enter_container(reply.message, 'a', element);
        if (enter < 0) {
            return Result<std::vector<WindowInfo>>::failure("could not enter Match array: " + errno_text(enter));
        }

        std::vector<std::string> uuids;
        std::unordered_set<std::string> seen;
        while ((enter = sd_bus_message_enter_container(reply.message, 'r', fields.c_str())) > 0) {
            const char* runner_id = nullptr;
            const char* text = nullptr;
            const char* icon = nullptr;
            if (sd_bus_message_read_basic(reply.message, 's', &runner_id) < 0 ||
                sd_bus_message_read_basic(reply.message, 's', &text) < 0 ||
                sd_bus_message_read_basic(reply.message, 's', &icon) < 0) {
                return Result<std::vector<WindowInfo>>::failure("Match row was truncated");
            }
            if (type_code == 'u') {
                std::uint32_t ignored = 0;
                sd_bus_message_read_basic(reply.message, 'u', &ignored);
            } else {
                std::int32_t ignored = 0;
                sd_bus_message_read_basic(reply.message, 'i', &ignored);
            }
            double relevance = 0;
            sd_bus_message_read_basic(reply.message, 'd', &relevance);
            (void)text;
            (void)icon;
            (void)relevance;
            // properties often carries a full icon. Skip it.
            if (sd_bus_message_skip(reply.message, "a{sv}") < 0) {
                return Result<std::vector<WindowInfo>>::failure("could not skip Match properties");
            }
            sd_bus_message_exit_container(reply.message);
            if (runner_id == nullptr) {
                continue;
            }
            const auto uuid = window_uuid_from_runner_id(runner_id);
            if (!uuid || !seen.insert(*uuid).second) {
                continue;
            }
            uuids.push_back(*uuid);
        }
        if (enter < 0) {
            return Result<std::vector<WindowInfo>>::failure("Match array ended early: " + errno_text(enter));
        }

        std::vector<WindowInfo> windows;
        for (const auto& uuid : uuids) {
            WindowDetails details;
            if (!read_window_details(bus, uuid, &details, &error)) {
                return Result<std::vector<WindowInfo>>::failure(std::move(error));
            }
            if (details.info.title.empty()) {
                continue;
            }
            if (details.saw_type && !is_capture_window_type(details.type)) {
                continue;
            }
            windows.push_back(std::move(details.info));
        }
        return Result<std::vector<WindowInfo>>::success(std::move(windows));
    }

    Result<Frame> grab_window(std::string_view window_id) override {
        const auto uuid = window_uuid_from_runner_id(window_id);
        if (!uuid) {
            return Result<Frame>::failure("window id is not a KWin uuid");
        }

        std::string error;
        Bus bus;
        if (!bus.open(&error) || !bus.owns("org.kde.KWin.ScreenShot2", &error)) {
            return Result<Frame>::failure(std::move(error));
        }

        ensure_desktop_file();
        RawShot shot;
        sd_bus_error dbus_error = SD_BUS_ERROR_NULL;
        if (!capture_once(bus, *uuid, &shot, &dbus_error, &error)) {
            if (access_denied(dbus_error)) {
                sd_bus_error_free(&dbus_error);
                dbus_error = SD_BUS_ERROR_NULL;
                run_program("kbuildsycoca6", 15);
                error.clear();
                if (!capture_once(bus, *uuid, &shot, &dbus_error, &error)) {
                    sd_bus_error_free(&dbus_error);
                    return Result<Frame>::failure(std::move(error));
                }
            } else {
                sd_bus_error_free(&dbus_error);
                return Result<Frame>::failure(std::move(error));
            }
        }
        sd_bus_error_free(&dbus_error);

        if (!convert_qimage_to_bgra(shot.format, shot.pixels.data(), static_cast<int>(shot.width),
                                    static_cast<int>(shot.height), static_cast<int>(shot.stride))) {
            return Result<Frame>::failure("unsupported QImage format " + std::to_string(shot.format));
        }

        WindowDetails details;
        std::string info_error;
        const bool have_info = read_window_details(bus, *uuid, &details, &info_error);

        Frame frame;
        frame.width = static_cast<int>(shot.width);
        frame.height = static_cast<int>(shot.height);
        frame.stride_bytes = static_cast<int>(shot.stride);
        frame.pixels = std::move(shot.pixels);
        if (have_info) {
            frame.mapping.window_logical_x = details.info.geometry.x;
            frame.mapping.window_logical_y = details.info.geometry.y;
            frame.mapping.image_pixels_per_logical = image_pixels_per_logical(
                frame.width, details.saw_geometry_width ? details.info.geometry.width : 0, shot.scale);
        } else {
            frame.mapping.image_pixels_per_logical = image_pixels_per_logical(frame.width, 0, shot.scale);
        }
        return Result<Frame>::success(std::move(frame));
    }

    Result<Frame> grab_region(std::string_view window_id, const Rect& region) override {
        auto full = grab_window(window_id);
        if (!full.ok() || !full.value) {
            return full;
        }
        return crop_frame(*full.value, region);
    }
};

}  // namespace gekko::yomi::kwin

GEKKO_YOMI_PLUGIN_EXPORT
void gekko_yomi_register(gekko::yomi::Host* host) {
    host->register_capture(
        {"kwin", "KWin ScreenShot2 window capture"},
        [](std::string*) -> std::unique_ptr<gekko::yomi::CaptureSource> {
            return std::make_unique<gekko::yomi::kwin::KWinCapture>();
        });
}
