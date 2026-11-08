#include "gekko_yomi/geometry.hpp"
#include "gekko_yomi/registry.hpp"
#include "gekko_yomi/result.hpp"
#include "bmp.hpp"
#include "compose.hpp"
#include "config.hpp"
#include "http_server.hpp"
#include "page.hpp"
#include "json_mini.hpp"
#include "crescent.hpp"
#include "window_ref.hpp"
#include "worker_session.hpp"
#include "python_ocr.hpp"

#include "gekko_yomi/runtime.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gekko_yomi/ocr_buffer.hpp"

#include <atomic>
#include <csignal>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

bool listening_on_any(int port);

void check(bool ok, const char* expr, int line) {
    if (!ok) {
        std::cerr << "FAIL " << line << ": " << expr << '\n';
        ++g_failures;
    }
}

#define CHECK(expr) check(static_cast<bool>(expr), #expr, __LINE__)

bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

void test_geometry() {
    gekko::yomi::FrameMapping mapping;
    mapping.window_logical_x = 100;
    mapping.window_logical_y = 40;
    mapping.image_pixels_per_logical = 2;
    mapping.client_inset_image_x = 4;
    mapping.client_inset_image_y = 6;

    const gekko::yomi::Rect image{4, 6, 20, 10};
    const auto overlay = gekko::yomi::image_rect_to_overlay(image, mapping);
    CHECK(near(overlay.x, 100));
    CHECK(near(overlay.y, 40));
    CHECK(near(overlay.width, 10));
    CHECK(near(overlay.height, 5));

    gekko::yomi::FrameMapping identity;
    identity.window_logical_x = 3;
    identity.window_logical_y = 4;
    const auto same = gekko::yomi::image_rect_to_overlay({1, 2, 3, 4}, identity);
    CHECK(near(same.x, 4));
    CHECK(near(same.y, 6));
    CHECK(near(same.width, 3));
    CHECK(near(same.height, 4));

    gekko::yomi::FrameMapping zero = identity;
    zero.image_pixels_per_logical = 0;
    const auto fallback = gekko::yomi::image_rect_to_overlay({8, 0, 2, 2}, zero);
    CHECK(near(fallback.x, 11));
    CHECK(near(fallback.width, 2));
}

void test_result() {
    const auto ok = gekko::yomi::Result<int>::success(7);
    CHECK(ok.ok());
    CHECK(ok.value.has_value() && *ok.value == 7);

    const auto bad = gekko::yomi::Result<int>::failure("nope");
    CHECK(!bad.ok());
    CHECK(!bad.value.has_value());
    CHECK(bad.error == "nope");

    CHECK(gekko::yomi::Status::success().ok());
    CHECK(!gekko::yomi::Status::failure("x").ok());
}

void test_registry_loads_fake() {
    const char* dir = std::getenv("GEKKO_YOMI_FAKE_DIR");
    CHECK(dir != nullptr);
    if (dir == nullptr) {
        return;
    }
    setenv("GEKKO_YOMI_PLUGINS", dir, 1);
    gekko::yomi::Registry registry;
    registry.load();
    CHECK(registry.load_errors().empty());
    CHECK(registry.capture_sources().size() == 1);
    CHECK(registry.ocr_engines().empty());
    CHECK(registry.implementation_count() == 1);
    if (!registry.capture_sources().empty()) {
        CHECK(registry.capture_sources().front()->id() == "fake");
        const auto windows = registry.capture_sources().front()->list_windows();
        CHECK(windows.ok());
        CHECK(windows.value && windows.value->size() == 1);
        CHECK(windows.value && windows.value->front().title == "Fixture");
    }
}

void test_registry_reports_missing_symbol() {
    const char* dir = std::getenv("GEKKO_YOMI_EMPTY_DIR");
    CHECK(dir != nullptr);
    if (dir == nullptr) {
        return;
    }
    setenv("GEKKO_YOMI_PLUGINS", dir, 1);
    gekko::yomi::Registry registry;
    registry.load();
    CHECK(registry.implementation_count() == 0);
    CHECK(registry.load_errors().size() == 1);
    if (!registry.load_errors().empty()) {
        CHECK(registry.load_errors().front().find("gekko_yomi_register") != std::string::npos);
    }
}

void test_explicit_plugin_path_replaces_defaults() {
    const auto empty = std::filesystem::temp_directory_path() / "gekko-yomi-empty-plugins";
    std::filesystem::create_directories(empty);
    setenv("GEKKO_YOMI_PLUGINS", empty.c_str(), 1);
    gekko::yomi::Registry registry;
    registry.load();
    CHECK(registry.implementation_count() == 0);
    CHECK(registry.load_errors().empty());
}

std::string http_exchange(int port, const std::string& request) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return {};
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return {};
    }
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
        if (n <= 0) {
            break;
        }
        sent += static_cast<std::size_t>(n);
    }
    std::string response;
    char buf[1024];
    ssize_t n = 0;
    while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return response;
}

int port_from_url(const std::string& line) {
    const auto colon = line.rfind(':');
    if (colon == std::string::npos) {
        return 0;
    }
    return std::atoi(line.c_str() + colon + 1);
}

void test_app_serves_without_plugins() {
    const char* app = std::getenv("GEKKO_YOMI_APP");
    CHECK(app != nullptr);
    if (app == nullptr) {
        return;
    }
    const auto empty = std::filesystem::temp_directory_path() / "gekko-yomi-empty-plugins";
    std::filesystem::create_directories(empty);

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        setenv("GEKKO_YOMI_PLUGINS", empty.c_str(), 1);
        setenv("GEKKO_YOMI_PORT", "0", 1);
        unsetenv("DISPLAY");
        unsetenv("WAYLAND_DISPLAY");
        execl(app, app, static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipefd[1]);
    std::string line;
    char byte = 0;
    while (line.size() < 200) {
        const ssize_t n = ::read(pipefd[0], &byte, 1);
        if (n <= 0) {
            break;
        }
        if (byte == '\n') {
            break;
        }
        line.push_back(byte);
    }
    close(pipefd[0]);
    const int port = port_from_url(line);
    CHECK(port > 0);
    if (port > 0) {
        const std::string response = http_exchange(port, "GET /api/windows HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        CHECK(response.find("no capture plugin is loaded") != std::string::npos);
        const std::string page = http_exchange(port, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        CHECK(page.find("月読") != std::string::npos);
    }
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) || WIFEXITED(status));
}

class FixtureCapture final : public gekko::yomi::CaptureSource {
public:
    std::string id() const override { return "fake"; }
    std::string description() const override { return "in-process"; }
    gekko::yomi::Result<std::vector<gekko::yomi::WindowInfo>> list_windows() override {
        gekko::yomi::WindowInfo window;
        window.id = "window-1";
        window.title = "Fixture";
        window.geometry = {0, 0, 2, 1};
        return gekko::yomi::Result<std::vector<gekko::yomi::WindowInfo>>::success(
            std::vector<gekko::yomi::WindowInfo>{window});
    }
    gekko::yomi::Result<gekko::yomi::Frame> grab_window(std::string_view) override {
        gekko::yomi::Frame frame;
        frame.width = 2;
        frame.height = 1;
        frame.stride_bytes = 8;
        frame.pixels = {9, 8, 7, 255, 6, 5, 4, 255};
        return gekko::yomi::Result<gekko::yomi::Frame>::success(std::move(frame));
    }
    gekko::yomi::Result<gekko::yomi::Frame> grab_region(std::string_view, const gekko::yomi::Rect&) override {
        return grab_window({});
    }
};

class FixtureOcr final : public gekko::yomi::OcrEngine {
public:
    std::string id() const override { return "fake-ocr"; }
    std::string description() const override { return "in-process"; }
    bool provides_character_boxes() const override { return true; }
    gekko::yomi::Result<gekko::yomi::OcrPass> recognize(const gekko::yomi::Frame&,
                                                        const gekko::yomi::OcrRequest&) override {
        gekko::yomi::OcrPass pass;
        pass.engine_id = "fake-ocr";
        gekko::yomi::TextRun run;
        run.text = "漢字";
        run.box = {0, 0, 2, 1};
        run.characters.push_back({"漢", {0, 0, 1, 1}, 1});
        run.characters.push_back({"字", {1, 0, 1, 1}, 1});
        pass.runs.push_back(std::move(run));
        return gekko::yomi::Result<gekko::yomi::OcrPass>::success(std::move(pass));
    }
};

class FixtureReading final : public gekko::yomi::ReadingProvider {
public:
    std::string id() const override { return "fake-reading"; }
    std::string description() const override { return "in-process"; }
    gekko::yomi::Result<std::vector<gekko::yomi::Morpheme>> analyze(std::string_view surface) override {
        gekko::yomi::Morpheme morpheme;
        morpheme.surface = std::string(surface);
        morpheme.reading = "かんじ";
        morpheme.dictionary_form = std::string(surface);
        morpheme.end_byte = surface.size();
        return gekko::yomi::Result<std::vector<gekko::yomi::Morpheme>>::success(
            std::vector<gekko::yomi::Morpheme>{std::move(morpheme)});
    }
};

class FixtureDictionary final : public gekko::yomi::Dictionary {
public:
    std::string id() const override { return "fake-dict"; }
    std::string description() const override { return "in-process"; }
    gekko::yomi::Result<std::vector<gekko::yomi::Gloss>> lookup(std::string_view) override {
        gekko::yomi::Gloss gloss;
        gloss.dictionary_form = "漢字";
        gloss.english.push_back("Chinese characters");
        return gekko::yomi::Result<std::vector<gekko::yomi::Gloss>>::success(
            std::vector<gekko::yomi::Gloss>{std::move(gloss)});
    }
};

void test_bmp_and_json() {
    gekko::yomi::Frame frame;
    frame.width = 1;
    frame.height = 1;
    frame.stride_bytes = 4;
    frame.pixels = {10, 20, 30, 255};
    const auto bmp = gekko::yomi::encode_bmp(frame);
    CHECK(bmp.size() == 14 + 40 + 4);
    CHECK(bmp[0] == 'B' && bmp[1] == 'M');
    CHECK(bmp[54] == 10 && bmp[55] == 20 && bmp[56] == 30);

    const auto quoted = gekko::yomi::json_quote("a\"b\\c");
    std::string error;
    const auto parsed = gekko::yomi::json_parse("{\"window_id\":" + quoted + ",\"n\":1}", &error);
    CHECK(parsed.has_value());
    CHECK(error.empty());
    if (parsed) {
        CHECK(parsed->field_string("window_id") == "a\"b\\c");
        CHECK(parsed->field("n") && parsed->field("n")->number == 1);
    }
    CHECK(!gekko::yomi::json_parse("{", &error));
}

void test_compose_fills_readings_when_present() {
    gekko::yomi::Registry registry;
    registry.add(std::make_unique<FixtureCapture>());
    registry.add(std::make_unique<FixtureOcr>());
    auto plain = gekko::yomi::read_window(registry, "window-1");
    CHECK(plain.capture_error.empty());
    CHECK(plain.ocr_error.empty());
    CHECK(plain.runs.size() == 1);
    CHECK(plain.runs[0].text == "漢字");
    CHECK(plain.runs[0].morphemes.empty());
    CHECK(plain.image_width == 2);

    registry.add(std::make_unique<FixtureReading>());
    registry.add(std::make_unique<FixtureDictionary>());
    auto rich = gekko::yomi::read_window(registry, "window-1");
    CHECK(rich.runs.size() == 1);
    CHECK(rich.runs[0].morphemes.size() == 1);
    CHECK(rich.runs[0].morphemes[0].reading == "かんじ");
    CHECK(rich.runs[0].morphemes[0].glosses.size() == 1);
    CHECK(rich.runs[0].morphemes[0].glosses[0] == "Chinese characters");
    const std::string json = gekko::yomi::read_json(rich);
    CHECK(json.find("かんじ") != std::string::npos);
    CHECK(json.find("Chinese characters") != std::string::npos);
}

void test_http_with_fakes() {
    const char* capture_dir = std::getenv("GEKKO_YOMI_FAKE_DIR");
    const char* ocr_dir = std::getenv("GEKKO_YOMI_FAKE_OCR_DIR");
    CHECK(capture_dir != nullptr && ocr_dir != nullptr);
    if (capture_dir == nullptr || ocr_dir == nullptr) {
        return;
    }
    const std::string plugins = std::string(capture_dir) + ":" + ocr_dir;
    setenv("GEKKO_YOMI_PLUGINS", plugins.c_str(), 1);
    gekko::yomi::Registry registry;
    registry.load();
    CHECK(registry.load_errors().empty());
    CHECK(registry.capture_sources().size() == 1);
    CHECK(registry.ocr_engines().size() == 1);

    gekko::yomi::HttpServer server(&registry);
    std::string error;
    CHECK(server.listen(0, &error));
    CHECK(listening_on_any(server.port()));
    if (server.port() == 0) {
        std::cerr << error << '\n';
        return;
    }
    std::thread thread([&server]() { server.run(); });
    const std::string windows =
        http_exchange(server.port(), "GET /api/windows HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK(windows.find("window-1") != std::string::npos);
    const std::string body = "{\"window_id\":\"window-1\"}";
    const std::string request = "POST /api/read HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    const std::string first_read = http_exchange(server.port(), request);
    CHECK(first_read.find("\"settling\":true") != std::string::npos);
    CHECK(first_read.find("漢字") == std::string::npos);
    const std::string read = http_exchange(server.port(), request);
    CHECK(read.find("漢字") != std::string::npos);
    CHECK(read.find("\"timings_ms\"") != std::string::npos);
    CHECK(read.find("\"ocr_error\":\"\"") != std::string::npos);
    const std::string bmp =
        http_exchange(server.port(), "GET /api/frame.bmp HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    CHECK(bmp.find("BM") != std::string::npos);
    server.stop();
    thread.join();
}

void test_fake_worker() {
    const char* script = std::getenv("GEKKO_YOMI_FAKE_WORKER");
    CHECK(script != nullptr);
    if (script == nullptr) {
        return;
    }
    gekko::yomi::ocr_worker::WorkerSession session;
    std::string error;
    const bool started = session.start("python3", script, &error);
    if (!started) {
        std::cerr << error << '\n';
    }
    CHECK(started);
    if (!started) {
        return;
    }
    const std::uint8_t pixels[8] = {1, 2, 3, 255, 4, 5, 6, 255};
    const auto line = session.request("{\"width\":2,\"height\":1,\"stride\":8}", pixels, 8, 5000, &error);
    if (!line) {
        std::cerr << error << '\n';
    }
    CHECK(line.has_value());
    if (!line) {
        return;
    }
    const auto json = gekko::yomi::json_parse(*line, &error);
    CHECK(json.has_value());
    if (!json) {
        std::cerr << error << '\n';
        return;
    }
    const auto* runs = json->field("runs");
    CHECK(runs != nullptr && runs->type == gekko::yomi::Json::Type::Array && !runs->array.empty());
    if (runs != nullptr && !runs->array.empty()) {
        CHECK(runs->array[0].field_string("text") == "漢字");
    }
}

void test_worker_that_exits_is_not_reused() {
    const auto path = std::filesystem::temp_directory_path() / "gekko-yomi-exit-worker.py";
    {
        std::ofstream out(path);
        out << "import json, sys\n"
            << "sys.stdout.buffer.write((json.dumps({\"error\": \"missing model\"}) + \"\\n\").encode())\n"
            << "sys.stdout.buffer.flush()\n"
            << "raise SystemExit(1)\n";
    }
    gekko::yomi::ocr_worker::WorkerSession session;
    std::string error;
    const bool started = session.start("python3", path.string(), &error);
    if (!started) {
        std::cerr << error << '\n';
    }
    CHECK(started);
    if (!started) {
        return;
    }
    const std::uint8_t pixels[8] = {1, 2, 3, 255, 4, 5, 6, 255};
    const auto line = session.request("{\"width\":2,\"height\":1,\"stride\":8}", pixels, 8, 5000, &error);
    if (!line) {
        std::cerr << error << '\n';
    }
    CHECK(line.has_value());
    if (line) {
        CHECK(line->find("missing model") != std::string::npos);
    }
    for (int i = 0; i < 50 && session.running(); ++i) {
        ::usleep(10000);
    }
    CHECK(!session.running());
    std::string again;
    const auto second = session.request("{\"width\":2,\"height\":1,\"stride\":8}", pixels, 8, 1000, &again);
    CHECK(!second.has_value());
    CHECK(again.find("not running") != std::string::npos);
}

std::filesystem::path link_plugins(const std::vector<std::pair<const char*, const char*>>& named) {
    const auto dir = std::filesystem::temp_directory_path() / "gekko-yomi-lang-plugins";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    for (const auto& item : named) {
        if (item.second == nullptr || item.second[0] == '\0') {
            continue;
        }
        std::filesystem::create_symlink(item.second, dir / item.first);
    }
    return dir;
}

void test_language_plugins() {
    const char* reading_so = std::getenv("GEKKO_YOMI_READING_SO");
    const char* dictionary_so = std::getenv("GEKKO_YOMI_DICTIONARY_SO");
    const char* kakasi = std::getenv("GEKKO_YOMI_FAKE_KAKASI");
    const char* capture_dir = std::getenv("GEKKO_YOMI_FAKE_DIR");
    const char* ocr_dir = std::getenv("GEKKO_YOMI_FAKE_OCR_DIR");
    CHECK(reading_so != nullptr && dictionary_so != nullptr && kakasi != nullptr);
    CHECK(capture_dir != nullptr && ocr_dir != nullptr);
    if (reading_so == nullptr || dictionary_so == nullptr || kakasi == nullptr || capture_dir == nullptr ||
        ocr_dir == nullptr) {
        return;
    }

    unsetenv("GEKKO_YOMI_DICTIONARY");
    const auto alone = link_plugins({{"reading_kakasi.so", reading_so}, {"dictionary_tsv.so", dictionary_so}});
    setenv("GEKKO_YOMI_PLUGINS", alone.c_str(), 1);
    setenv("GEKKO_YOMI_KAKASI_WORKER", kakasi, 1);
    {
        gekko::yomi::Registry registry;
        registry.load();
        CHECK(registry.reading_providers().size() == 1);
        CHECK(registry.dictionaries().empty());
        CHECK(registry.module_paths().size() == 2);
        CHECK(registry.load_errors().size() == 1);
        if (!registry.load_errors().empty()) {
            CHECK(registry.load_errors().front().find("GEKKO_YOMI_DICTIONARY") != std::string::npos);
        }
    }

    const auto missing = std::filesystem::temp_directory_path() / "gekko-yomi-missing-dictionary.tsv";
    setenv("GEKKO_YOMI_DICTIONARY", missing.c_str(), 1);
    {
        gekko::yomi::Registry registry;
        registry.load();
        CHECK(registry.dictionaries().size() == 1);
        const auto looked = registry.dictionaries().front()->lookup("漢字");
        CHECK(!looked.ok());
        CHECK(looked.error.find("not found") != std::string::npos);
    }

    const auto table = std::filesystem::temp_directory_path() / "gekko-yomi-dictionary.tsv";
    {
        std::ofstream out(table);
        out << "# comment\n\n";
        out << "漢字\tn\tChinese character / kanji\n";
        out << "漢字\tn\ta logogram\n";
        out << "月\tmoon\n";
    }
    setenv("GEKKO_YOMI_DICTIONARY", table.c_str(), 1);
    const auto capture_so = std::filesystem::path(capture_dir) / "fake_capture.so";
    const auto ocr_so = std::filesystem::path(ocr_dir) / "fake_ocr.so";
    const auto dir = link_plugins({
        {"reading_kakasi.so", reading_so},
        {"dictionary_tsv.so", dictionary_so},
        {"fake_capture.so", capture_so.c_str()},
        {"fake_ocr.so", ocr_so.c_str()},
    });
    setenv("GEKKO_YOMI_PLUGINS", dir.c_str(), 1);
    gekko::yomi::Registry registry;
    registry.load();
    if (!registry.load_errors().empty()) {
        std::cerr << registry.load_errors().front() << '\n';
    }
    CHECK(registry.load_errors().empty());
    CHECK(registry.capture_sources().size() == 1);
    CHECK(registry.ocr_engines().size() == 1);
    CHECK(registry.reading_providers().size() == 1);
    CHECK(registry.dictionaries().size() == 1);
    const auto moon = registry.dictionaries().front()->lookup("月");
    CHECK(moon.ok() && moon.value && moon.value->size() == 1);
    if (moon.ok() && moon.value && !moon.value->empty()) {
        CHECK(moon.value->front().part_of_speech.empty());
        CHECK(moon.value->front().english.size() == 1);
        CHECK(moon.value->front().english.front() == "moon");
    }
    const auto kanji = registry.dictionaries().front()->lookup("漢字");
    CHECK(kanji.ok() && kanji.value && kanji.value->size() == 2);
    if (kanji.ok() && kanji.value && kanji.value->size() == 2) {
        CHECK(kanji.value->front().english.size() == 2);
        CHECK(kanji.value->front().english[0] == "Chinese character");
        CHECK(kanji.value->front().english[1] == "kanji");
    }
    const auto unknown = registry.dictionaries().front()->lookup("ない");
    CHECK(unknown.ok() && unknown.value && unknown.value->empty());

    auto view = gekko::yomi::read_window(registry, "window-1");
    CHECK(view.capture_error.empty());
    CHECK(view.ocr_error.empty());
    CHECK(view.reading_error.empty());
    CHECK(view.dictionary_error.empty());
    CHECK(view.runs.size() == 1);
    if (!view.runs.empty()) {
        CHECK(view.runs[0].morphemes.size() == 1);
        if (!view.runs[0].morphemes.empty()) {
            CHECK(view.runs[0].morphemes[0].reading == "よみ");
            CHECK(view.runs[0].morphemes[0].end_byte == std::string("漢字").size());
            CHECK(view.runs[0].morphemes[0].glosses.size() == 3);
        }
    }
    const std::string json = gekko::yomi::read_json(view);
    CHECK(json.find("よみ") != std::string::npos);
    CHECK(json.find("Chinese character") != std::string::npos);
    CHECK(json.find("\"reading_error\":\"\"") != std::string::npos);

    const auto exit_script = std::filesystem::temp_directory_path() / "gekko-yomi-exit-kakasi.py";
    {
        std::ofstream out(exit_script);
        out << "import json, sys\n"
            << "sys.stdout.buffer.write((json.dumps({\"error\": \"missing kakasi\"}) + \"\\n\").encode())\n"
            << "sys.stdout.buffer.flush()\n"
            << "raise SystemExit(1)\n";
    }
    setenv("GEKKO_YOMI_KAKASI_WORKER", exit_script.c_str(), 1);
    gekko::yomi::Registry again;
    again.load();
    CHECK(again.reading_providers().size() == 1);
    if (!again.reading_providers().empty()) {
        const auto first = again.reading_providers().front()->analyze("漢字");
        CHECK(!first.ok());
        CHECK(first.error.find("missing kakasi") != std::string::npos);
        const auto second = again.reading_providers().front()->analyze("漢字");
        if (!second.ok() && second.error.find("missing kakasi") == std::string::npos) {
            std::cerr << "second reading error: " << second.error << '\n';
        }
        CHECK(!second.ok());
        CHECK(second.error.find("missing kakasi") != std::string::npos);
        CHECK(second.error.find("Broken pipe") == std::string::npos);
    }
}

class MixedOcr final : public gekko::yomi::OcrEngine {
public:
    std::string id() const override { return "mixed-ocr"; }
    std::string description() const override { return "in-process"; }
    bool provides_character_boxes() const override { return true; }
    gekko::yomi::Result<gekko::yomi::OcrPass> recognize(const gekko::yomi::Frame&,
                                                        const gekko::yomi::OcrRequest&) override {
        gekko::yomi::OcrPass pass;
        pass.engine_id = "mixed-ocr";
        gekko::yomi::TextRun mixed;
        mixed.text = "Hello 漢字";
        mixed.characters.push_back({"H", {0, 0, 1, 1}, 1});
        mixed.characters.push_back({"漢", {1, 0, 1, 1}, 1});
        mixed.characters.push_back({"字", {2, 0, 1, 1}, 1});
        pass.runs.push_back(std::move(mixed));
        gekko::yomi::TextRun english;
        english.text = "File!";
        english.characters.push_back({"F", {0, 1, 1, 1}, 1});
        pass.runs.push_back(std::move(english));
        gekko::yomi::TextRun fullwidth;
        fullwidth.text = "ＡＢＣテスト";
        pass.runs.push_back(std::move(fullwidth));
        gekko::yomi::TextRun number;
        number.text = "Chapter 2";
        pass.runs.push_back(std::move(number));
        gekko::yomi::TextRun digits;
        digits.text = "123";
        pass.runs.push_back(std::move(digits));
        gekko::yomi::TextRun clock;
        clock.text = "12:30";
        pass.runs.push_back(std::move(clock));
        gekko::yomi::TextRun marks;
        marks.text = "、。！……";
        pass.runs.push_back(std::move(marks));
        gekko::yomi::TextRun chapter;
        chapter.text = "第2話";
        chapter.characters.push_back({"第", {0, 2, 1, 1}, 1});
        chapter.characters.push_back({"2", {1, 2, 1, 1}, 1});
        chapter.characters.push_back({"話", {2, 2, 1, 1}, 1});
        pass.runs.push_back(std::move(chapter));
        return gekko::yomi::Result<gekko::yomi::OcrPass>::success(std::move(pass));
    }
};

void test_compose_drops_english_letters() {
    gekko::yomi::Registry registry;
    registry.add(std::make_unique<FixtureCapture>());
    registry.add(std::make_unique<MixedOcr>());
    registry.add(std::make_unique<FixtureReading>());
    const auto view = gekko::yomi::read_window(registry, "window-1");
    CHECK(view.ocr_error.empty());
    CHECK(view.runs.size() == 3);
    if (view.runs.size() == 3) {
        CHECK(view.runs[0].text == "テスト");
        CHECK(view.runs[1].text == "第2話");
        CHECK(view.runs[1].characters.size() == 2);
        if (view.runs[1].characters.size() == 2) {
            CHECK(view.runs[1].characters[0].text == "第");
            CHECK(view.runs[1].characters[1].text == "話");
        }
        CHECK(view.runs[2].text == "漢字");
        CHECK(view.runs[2].characters.size() == 2);
        CHECK(view.runs[2].characters[0].text == "漢");
        CHECK(view.runs[2].characters[1].text == "字");
        CHECK(view.runs[2].morphemes.size() == 1);
        CHECK(view.runs[2].morphemes[0].surface == "漢字");
    }
    const std::string json = gekko::yomi::read_json(view);
    CHECK(json.find("Hello") == std::string::npos);
    CHECK(json.find("File") == std::string::npos);
    CHECK(json.find("Chapter") == std::string::npos);
    CHECK(json.find("123") == std::string::npos);
    CHECK(json.find("12:30") == std::string::npos);
    CHECK(json.find("第2話") != std::string::npos);
    CHECK(json.find("\"timings_ms\"") != std::string::npos);
    CHECK(view.timings.engines.size() == 1);
    if (!view.timings.engines.empty()) {
        CHECK(view.timings.engines[0].id == "mixed-ocr");
    }
    CHECK(view.timings.total_ms >= view.timings.ocr_ms);
    const std::string page = gekko::yomi::page_html();
    CHECK(page.find("readSelected(true)") != std::string::npos);
    CHECK(page.find("watch: automatic") != std::string::npos);
    CHECK(page.find("Settling this pass") != std::string::npos);
    CHECK(page.find("id=\"poll\"") != std::string::npos);
    CHECK(page.find("value=\"50\"") != std::string::npos);
    CHECK(page.find("value=\"1000\" selected") != std::string::npos);
    CHECK(page.find("id=\"times\"") != std::string::npos);
    CHECK(page.find("timings_ms") != std::string::npos);
}

bool listening_on_any(int port) {
    std::ifstream tcp("/proc/net/tcp");
    char hex[8];
    std::snprintf(hex, sizeof(hex), "%04X", port);
    const std::string needle = std::string("00000000:") + hex;
    std::string line;
    while (std::getline(tcp, line)) {
        if (line.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::atomic<int>* g_ocr_gate = nullptr;

bool ocr_gate_wait() {
    if (g_ocr_gate == nullptr) {
        return true;
    }
    const int ticket = g_ocr_gate->fetch_add(1);
    const int need = (ticket / 2 + 1) * 2;
    for (int i = 0; i < 200 && g_ocr_gate->load() < need; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return g_ocr_gate->load() >= need;
}

class ParallelShortOcr final : public gekko::yomi::OcrEngine {
public:
    std::string id() const override { return "short-ocr"; }
    std::string description() const override { return "in-process"; }
    bool provides_character_boxes() const override { return false; }
    gekko::yomi::Result<gekko::yomi::OcrPass> recognize(const gekko::yomi::Frame&,
                                                        const gekko::yomi::OcrRequest&) override {
        if (!ocr_gate_wait()) {
            return gekko::yomi::Result<gekko::yomi::OcrPass>::failure("engines did not run together");
        }
        gekko::yomi::OcrPass pass;
        gekko::yomi::TextRun run;
        run.text = "だが、それは中わ恋だった";
        run.box = {10, 20, 300, 40};
        pass.runs.push_back(std::move(run));
        return gekko::yomi::Result<gekko::yomi::OcrPass>::success(std::move(pass));
    }
};

class ParallelLongOcr final : public gekko::yomi::OcrEngine {
public:
    std::string id() const override { return "long-ocr"; }
    std::string description() const override { return "in-process"; }
    bool provides_character_boxes() const override { return false; }
    gekko::yomi::Result<gekko::yomi::OcrPass> recognize(const gekko::yomi::Frame&,
                                                        const gekko::yomi::OcrRequest&) override {
        if (!ocr_gate_wait()) {
            return gekko::yomi::Result<gekko::yomi::OcrPass>::failure("engines did not run together");
        }
        gekko::yomi::OcrPass pass;
        gekko::yomi::TextRun run;
        run.text = "だが、それはかなわぬ恋だった";
        run.box = {10, 20, 300, 40};
        pass.runs.push_back(std::move(run));
        return gekko::yomi::Result<gekko::yomi::OcrPass>::success(std::move(pass));
    }
};

void test_parallel_engines_agree_on_the_longer_line() {
    std::atomic<int> entered{0};
    g_ocr_gate = &entered;
    gekko::yomi::Registry registry;
    registry.add(std::make_unique<FixtureCapture>());
    registry.add(std::make_unique<ParallelShortOcr>());
    registry.add(std::make_unique<ParallelLongOcr>());
    gekko::yomi::OcrConsensus memory;
    const auto first = gekko::yomi::read_window(registry, "window-1", &memory);
    CHECK(first.ocr_error.empty());
    CHECK(first.settling);
    CHECK(first.runs.empty());
    CHECK(entered.load() >= 2);
    const auto second = gekko::yomi::read_window(registry, "window-1", &memory);
    CHECK(second.ocr_error.empty());
    CHECK(!second.settling);
    CHECK(second.runs.size() == 1);
    if (!second.runs.empty()) {
        CHECK(second.runs[0].text == "だが、それはかなわぬ恋だった");
    }
    CHECK(second.engine_id == "short-ocr+long-ocr");
    g_ocr_gate = nullptr;
}

void test_ppocr_manga_plugin_returns_a_line() {
    const char* library = std::getenv("GEKKO_YOMI_PPOCR_SO");
    CHECK(library != nullptr);
    if (library == nullptr) {
        return;
    }
    const auto script = std::filesystem::temp_directory_path() / "gekko-yomi-ppocr-fake.py";
    {
        std::ofstream out(script);
        out << "import json, sys\n"
            << "def read_line(stream):\n"
            << "    data = bytearray()\n"
            << "    while True:\n"
            << "        chunk = stream.read(1)\n"
            << "        if not chunk:\n"
            << "            return None\n"
            << "        if chunk == b'\\n':\n"
            << "            return data.decode('utf-8')\n"
            << "        data.extend(chunk)\n"
            << "stream = sys.stdin.buffer\n"
            << "while True:\n"
            << "    header = read_line(stream)\n"
            << "    if header is None:\n"
            << "        raise SystemExit(0)\n"
            << "    meta = json.loads(header)\n"
            << "    nbytes = int(meta['stride']) * int(meta['height'])\n"
            << "    pixels = stream.read(nbytes)\n"
            << "    if len(pixels) != nbytes:\n"
            << "        payload = {'error': 'short pixel read'}\n"
            << "    else:\n"
            << "        payload = {'runs': [{'text': '日本語', 'vertical': True, 'confidence': 0.75, "
            << "'box': [4, 5, 30, 12]}]}\n"
            << "    sys.stdout.buffer.write((json.dumps(payload, ensure_ascii=False) + '\\n').encode())\n"
            << "    sys.stdout.buffer.flush()\n";
    }
    const auto dir = link_plugins({{"ocr_ppocr_manga.so", library}});
    setenv("GEKKO_YOMI_PLUGINS", dir.c_str(), 1);
    setenv("GEKKO_YOMI_PPOCR_MANGA_WORKER", script.c_str(), 1);
    gekko::yomi::Registry registry;
    registry.add(std::make_unique<FixtureCapture>());
    registry.load();
    CHECK(registry.load_errors().empty());
    if (!registry.load_errors().empty()) {
        for (const auto& error : registry.load_errors()) {
            std::cerr << error << '\n';
        }
    }
    CHECK(registry.ocr_engines().size() == 1);
    if (!registry.ocr_engines().empty()) {
        CHECK(registry.ocr_engines().front()->id() == "ppocr-manga");
        CHECK(!registry.ocr_engines().front()->provides_character_boxes());
    }
    const auto view = gekko::yomi::read_window(registry, "window-1");
    CHECK(view.ocr_error.empty());
    if (!view.ocr_error.empty()) {
        std::cerr << view.ocr_error << '\n';
    }
    CHECK(view.engine_id == "ppocr-manga");
    CHECK(view.runs.size() == 1);
    if (!view.runs.empty()) {
        CHECK(view.runs[0].text == "日本語");
        CHECK(view.runs[0].vertical);
        CHECK(view.runs[0].characters.empty());
        CHECK(view.runs[0].box.x == 4);
        CHECK(view.runs[0].box.width == 30);
    }
    unsetenv("GEKKO_YOMI_PLUGINS");
    unsetenv("GEKKO_YOMI_PPOCR_MANGA_WORKER");
}

void test_app_suite() {
    test_bmp_and_json();
    test_compose_fills_readings_when_present();
    test_compose_drops_english_letters();
    test_parallel_engines_agree_on_the_longer_line();
    test_http_with_fakes();
    test_fake_worker();
    test_worker_that_exits_is_not_reused();
    test_ppocr_manga_plugin_returns_a_line();
    test_language_plugins();
}

void test_kwin_ids_and_pixels() {
    const auto uuid = gekko::yomi::kwin::window_uuid_from_runner_id(
        "0_{8ceda663-7dc9-4b1a-be2c-e54c215f4285}");
    CHECK(uuid && *uuid == "{8ceda663-7dc9-4b1a-be2c-e54c215f4285}");
    const auto bare = gekko::yomi::kwin::window_uuid_from_runner_id("{8CEDA663-7DC9-4B1A-BE2C-E54C215F4285}");
    CHECK(bare && *bare == "{8CEDA663-7DC9-4B1A-BE2C-E54C215F4285}");
    CHECK(!gekko::yomi::kwin::window_uuid_from_runner_id("8_a3b75867-c3d8-403b-88b0-26805e8466fe"));
    CHECK(!gekko::yomi::kwin::window_uuid_from_runner_id("0_{not-a-uuid}"));
    CHECK(!gekko::yomi::kwin::window_uuid_from_runner_id(""));

    CHECK(gekko::yomi::kwin::is_capture_window_type(0));
    CHECK(gekko::yomi::kwin::is_capture_window_type(5));
    CHECK(!gekko::yomi::kwin::is_capture_window_type(1));
    CHECK(!gekko::yomi::kwin::is_capture_window_type(2));

    CHECK(near(gekko::yomi::kwin::image_pixels_per_logical(1920, 1280, 1.5), 1.5));
    CHECK(near(gekko::yomi::kwin::image_pixels_per_logical(100, 100, 1.5), 1));
    CHECK(near(gekko::yomi::kwin::image_pixels_per_logical(0, 0, 1.5), 1.5));
    CHECK(near(gekko::yomi::kwin::image_pixels_per_logical(0, 0, 0), 1));

    std::uint8_t rgba[8] = {1, 2, 3, 255, 9, 9, 9, 9};
    CHECK(gekko::yomi::kwin::convert_qimage_to_bgra(18, rgba, 1, 1, 8));
    CHECK(rgba[0] == 3 && rgba[1] == 2 && rgba[2] == 1 && rgba[3] == 255);
    CHECK(rgba[4] == 9 && rgba[7] == 9);
    std::uint8_t bgra[4] = {4, 5, 6, 255};
    CHECK(gekko::yomi::kwin::convert_qimage_to_bgra(6, bgra, 1, 1, 4));
    CHECK(bgra[0] == 4 && bgra[2] == 6);
    CHECK(!gekko::yomi::kwin::convert_qimage_to_bgra(7, bgra, 1, 1, 4));

    gekko::yomi::Frame frame;
    frame.width = 2;
    frame.height = 2;
    frame.stride_bytes = 12;
    frame.pixels = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                    13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    frame.mapping.window_logical_x = 10;
    frame.mapping.image_pixels_per_logical = 2;
    const auto cropped = gekko::yomi::kwin::crop_frame(frame, {1, 1, 1, 1});
    CHECK(cropped.ok());
    if (cropped.value) {
        CHECK(cropped.value->width == 1);
        CHECK(cropped.value->height == 1);
        CHECK(cropped.value->pixels.size() == 4);
        CHECK(cropped.value->pixels[0] == 17 && cropped.value->pixels[3] == 20);
        CHECK(near(cropped.value->mapping.client_inset_image_x, -1));
        CHECK(near(cropped.value->mapping.client_inset_image_y, -1));
        CHECK(near(cropped.value->mapping.window_logical_x, 10));
    }
    CHECK(!gekko::yomi::kwin::crop_frame(frame, {8, 8, 1, 1}).ok());
}

void test_kwin_session() {
    const char* dir = std::getenv("GEKKO_YOMI_KWIN_DIR");
    CHECK(dir != nullptr);
    if (dir == nullptr) {
        return;
    }
    setenv("GEKKO_YOMI_PLUGINS", dir, 1);
    gekko::yomi::Registry registry;
    registry.load();
    // The KWin .so lives in the same directory as the other plugins. The
    // dictionary factory declines when GEKKO_YOMI_DICTIONARY is unset. That
    // is a recorded decline, not a failed dlopen. Anything else is a failure.
    for (const auto& load_error : registry.load_errors()) {
        if (load_error.find("GEKKO_YOMI_DICTIONARY") == std::string::npos) {
            std::cerr << load_error << '\n';
            CHECK(false);
        }
    }
    gekko::yomi::CaptureSource* kwin = nullptr;
    for (const auto& source : registry.capture_sources()) {
        if (source->id() == "kwin") {
            kwin = source.get();
        }
    }
    CHECK(kwin != nullptr);
    if (kwin == nullptr) {
        return;
    }
    const auto windows = kwin->list_windows();
    if (!windows.ok() && windows.error.find("KWin is not available") != std::string::npos) {
        std::cerr << "SKIP " << windows.error << '\n';
        std::exit(77);
    }
    if (!windows.ok()) {
        std::cerr << windows.error << '\n';
    }
    CHECK(windows.ok());
    if (!windows.value) {
        return;
    }
    const gekko::yomi::WindowInfo* pick = nullptr;
    double area = 1e300;
    for (const auto& window : *windows.value) {
        CHECK(window.id.size() == 38);
        CHECK(!window.id.empty() && window.id.front() == '{' && window.id.back() == '}');
        CHECK(!window.title.empty());
        if (window.minimized || window.geometry.width <= 0 || window.geometry.height <= 0) {
            continue;
        }
        const double this_area = window.geometry.width * window.geometry.height;
        if (this_area < area) {
            area = this_area;
            pick = &window;
        }
    }
    if (pick == nullptr) {
        return;
    }
    const auto frame = kwin->grab_window(pick->id);
    if (!frame.ok()) {
        std::cerr << frame.error << '\n';
    }
    CHECK(frame.ok());
    if (!frame.value) {
        return;
    }
    CHECK(frame.value->width > 0);
    CHECK(frame.value->height > 0);
    CHECK(frame.value->stride_bytes >= frame.value->width * 4);
    CHECK(frame.value->pixels.size() >=
          static_cast<std::size_t>(frame.value->stride_bytes) * static_cast<std::size_t>(frame.value->height));
    // List and grab are separate calls. A one-pixel compositor change still
    // has to land on the width ratio, not on a hardcoded scale of 1.
    const double ratio = static_cast<double>(frame.value->width) / pick->geometry.width;
    CHECK(std::abs(frame.value->mapping.image_pixels_per_logical - ratio) < 1e-3);
    CHECK(std::abs(frame.value->mapping.window_logical_x - pick->geometry.x) < 1);
    CHECK(std::abs(frame.value->mapping.window_logical_y - pick->geometry.y) < 1);
    CHECK(near(frame.value->mapping.client_inset_image_x, 0));
}

gekko::yomi::TextRun sample_line(std::string text, double x, double y, double width, double height) {
    gekko::yomi::TextRun run;
    run.text = std::move(text);
    run.box = {x, y, width, height};
    return run;
}

void test_ocr_buffer() {
    const auto full = sample_line("だが、それはかなわぬ恋だった", 0, 0, 200, 40);
    const auto piece = sample_line("恋", 80, 0, 40, 40);
    gekko::yomi::OcrBuffer buffer;
    const auto hidden = buffer.observe({piece, full});
    CHECK(hidden.empty());
    CHECK(buffer.warming());
    const auto shown = buffer.observe({piece, full});
    CHECK(shown.size() == 1);
    if (!shown.empty()) {
        CHECK(shown[0].text == full.text);
    }
    const auto held = buffer.observe({sample_line("だが、それは中わ恋だった", 0, 0, 200, 40)});
    CHECK(held.size() == 1);
    if (!held.empty()) {
        CHECK(held[0].text == full.text);
    }

    gekko::yomi::OcrBufferOptions quick;
    quick.promote_after = 1;
    gekko::yomi::OcrBuffer aging(quick);
    CHECK(aging.observe({sample_line("漢字", 0, 0, 20, 20)}).size() == 1);
    CHECK(aging.observe({}).size() == 1);
    CHECK(aging.observe({}).empty());

    gekko::yomi::OcrBuffer places(quick);
    const auto both = places.observe(
        {sample_line("上", 0, 0, 20, 20), sample_line("下", 0, 80, 20, 20)});
    CHECK(both.size() == 2);
    const auto missed_once = places.observe({sample_line("下", 0, 80, 20, 20)});
    CHECK(missed_once.size() == 2);
    const auto dropped = places.observe({sample_line("下", 0, 80, 20, 20)});
    CHECK(dropped.size() == 1);
    if (!dropped.empty()) {
        CHECK(dropped[0].text == "下");
    }

    gekko::yomi::OcrBuffer spaced(quick);
    const auto pair = spaced.observe(
        {sample_line("上", 0, 0, 20, 20), sample_line("下", 0, 80, 20, 20)});
    CHECK(pair.size() == 2);

    gekko::yomi::TextRun dragon = sample_line("竜", 0, 0, 30, 30);
    dragon.confidence = 0.9F;
    gekko::yomi::TextRun someone = sample_line("誰", 0, 0, 30, 30);
    someone.confidence = 0.2F;
    gekko::yomi::OcrBuffer votes(quick);
    CHECK(votes.observe({someone})[0].text == "誰");
    CHECK(votes.observe({dragon})[0].text == "竜");

    gekko::yomi::OcrConsensus consensus;
    const auto waiting = consensus.observe(
        {{"meiki", {full}}, {"other", {sample_line("だが、それは中わ恋だった", 0, 0, 200, 40)}}});
    CHECK(waiting.empty());
    CHECK(consensus.settling());
    const auto agreed = consensus.observe(
        {{"meiki", {full}}, {"other", {sample_line("だが、それは中わ恋だった", 0, 0, 200, 40)}}});
    CHECK(agreed.size() == 1);
    if (!agreed.empty()) {
        CHECK(agreed[0].text == full.text);
    }
    CHECK(consensus.engine("meiki") != nullptr);
    CHECK(consensus.engine("missing") == nullptr);
    consensus.clear();
    CHECK(consensus.observe({{"meiki", {full}}}).empty());

    gekko::yomi::OcrConsensus places_across;
    const auto top = sample_line("上", 0, 0, 20, 20);
    const auto bottom = sample_line("下", 0, 80, 20, 20);
    CHECK(places_across.observe({{"meiki", {top, bottom}}, {"other", {top, bottom}}}).empty());
    const auto placed = places_across.observe({{"meiki", {top, bottom}}, {"other", {top, bottom}}});
    CHECK(placed.size() == 2);
    const auto one_quiet = places_across.observe({{"meiki", {bottom}}, {"other", {bottom}}});
    CHECK(one_quiet.size() == 2);
    const auto top_gone = places_across.observe({{"meiki", {bottom}}, {"other", {bottom}}});
    CHECK(top_gone.size() == 1);
    if (!top_gone.empty()) {
        CHECK(top_gone[0].text == "下");
    }
    const auto either = places_across.observe({{"meiki", {bottom}}, {"other", {}}});
    CHECK(either.size() == 1);
    CHECK(places_across.observe({{"meiki", {}}, {"other", {}}}).size() == 1);
    CHECK(places_across.observe({{"meiki", {}}, {"other", {}}}).empty());

    gekko::yomi::OcrBufferOptions defaults;
    CHECK(defaults.ttl_scans == 8);
    CHECK(defaults.positional_invalidation_scans == 2);

    const auto old_line = sample_line("古い文", 0, 0, 80, 20);
    const auto new_line = sample_line("新", 0, 0, 80, 20);
    gekko::yomi::OcrBufferOptions fade;
    fade.promote_after = 1;
    fade.ttl_scans = 1;
    fade.positional_invalidation_scans = 4;
    gekko::yomi::OcrConsensus faded(fade);
    const auto faded_first = faded.observe({{"meiki", {old_line}}});
    CHECK(faded_first.size() == 1);
    if (!faded_first.empty()) {
        CHECK(faded_first[0].text == old_line.text);
    }
    faded.observe({{"meiki", {old_line}}});
    faded.observe({{"meiki", {old_line}}});
    const auto replaced = faded.observe({{"meiki", {new_line}}});
    CHECK(replaced.size() == 1);
    if (!replaced.empty()) {
        CHECK(replaced[0].text == new_line.text);
    }

    gekko::yomi::OcrBufferOptions sticky_options;
    sticky_options.promote_after = 1;
    sticky_options.ttl_scans = 8;
    gekko::yomi::OcrConsensus sticky(sticky_options);
    const auto sticky_first = sticky.observe({{"meiki", {old_line}}});
    CHECK(sticky_first.size() == 1);
    if (!sticky_first.empty()) {
        CHECK(sticky_first[0].text == old_line.text);
    }
    sticky.observe({{"meiki", {old_line}}});
    sticky.observe({{"meiki", {old_line}}});
    const auto still_old = sticky.observe({{"meiki", {new_line}}});
    CHECK(still_old.size() == 1);
    if (!still_old.empty()) {
        CHECK(still_old[0].text == old_line.text);
    }

    gekko::yomi::OcrBufferOptions once;
    once.promote_after = 1;
    once.positional_invalidation_scans = 1;
    gekko::yomi::OcrBuffer strict(once);
    CHECK(strict.observe({sample_line("上", 0, 0, 20, 20)}).size() == 1);
    CHECK(strict.observe({}).empty());

    gekko::yomi::OcrBufferOptions loose_options;
    loose_options.promote_after = 1;
    loose_options.positional_invalidation_scans = 3;
    gekko::yomi::OcrBuffer loose(loose_options);
    CHECK(loose.observe({sample_line("上", 0, 0, 20, 20)}).size() == 1);
    CHECK(loose.observe({}).size() == 1);
    CHECK(loose.observe({}).size() == 1);
    CHECK(loose.observe({}).empty());
}

class NamedOcr final : public gekko::yomi::OcrEngine {
public:
    NamedOcr(std::string id, std::string text) : id_(std::move(id)), text_(std::move(text)) {}

    std::string id() const override { return id_; }
    std::string description() const override { return "scripted"; }
    bool provides_character_boxes() const override { return false; }

    gekko::yomi::Result<gekko::yomi::OcrPass> recognize(const gekko::yomi::Frame&,
                                                        const gekko::yomi::OcrRequest&) override {
        gekko::yomi::OcrPass pass;
        pass.engine_id = id_;
        gekko::yomi::TextRun run;
        run.text = text_;
        run.box = {0, 0, 10, 10};
        pass.runs.push_back(std::move(run));
        return gekko::yomi::Result<gekko::yomi::OcrPass>::success(std::move(pass));
    }

private:
    std::string id_;
    std::string text_;
};

void test_core_config_file() {
    const char* home = std::getenv("HOME");
    const auto path = gekko::yomi::default_core_config_path();
    if (home != nullptr && home[0] != '\0') {
        CHECK(path == std::string(home) + "/.config/gekko-yomi/core.json");
    }
    unsetenv("HOME");
    CHECK(gekko::yomi::default_core_config_path().empty());
    if (home != nullptr) {
        setenv("HOME", home, 1);
    }

    const auto missing = std::filesystem::temp_directory_path() / "gekko-yomi-missing-core.json";
    std::filesystem::remove(missing);
    const auto absent = gekko::yomi::load_core_config(missing.string(), false);
    CHECK(absent.ok());
    CHECK(absent.used_defaults);
    CHECK(absent.config.buffer.ttl_scans == 8);
    CHECK(!absent.config.ocr_engines.has_value());
    const auto required = gekko::yomi::load_core_config(missing.string(), true);
    CHECK(!required.ok());

    const auto dir = std::filesystem::temp_directory_path() / "gekko-yomi-core-config";
    std::filesystem::create_directories(dir);
    const auto file = dir / "core.json";
    {
        std::ofstream out(file);
        out << "{\n"
            << "  \"buffer\": {\"ttl_scans\": 3, \"positional_invalidation_scans\": 4},\n"
            << "  \"ocr\": {\"engines\": [\"meiki\"]},\n"
            << "  \"later\": true\n"
            << "}\n";
    }
    const auto parsed = gekko::yomi::load_core_config(file.string(), true);
    CHECK(parsed.ok());
    CHECK(!parsed.used_defaults);
    CHECK(parsed.config.buffer.ttl_scans == 3);
    CHECK(parsed.config.buffer.positional_invalidation_scans == 4);
    CHECK(parsed.config.ocr_engines.has_value());
    if (parsed.config.ocr_engines) {
        CHECK(parsed.config.ocr_engines->size() == 1);
        CHECK(parsed.config.ocr_engines->front() == "meiki");
    }
    CHECK(!parsed.warnings.empty());

    {
        std::ofstream out(file);
        out << "{\"ocr\":{\"engines\":[]}}";
    }
    const auto none = gekko::yomi::load_core_config(file.string(), true);
    CHECK(none.ok());
    CHECK(none.config.ocr_engines.has_value());
    CHECK(none.config.ocr_engines->empty());
    CHECK(none.config.buffer.ttl_scans == 8);

    {
        std::ofstream out(file);
        out << "{\"buffer\":{\"ttl_scans\":1.5}}";
    }
    CHECK(!gekko::yomi::load_core_config(file.string(), true).ok());
    {
        std::ofstream out(file);
        out << "{\"buffer\":{\"ttl_scans\":0}}";
    }
    CHECK(!gekko::yomi::load_core_config(file.string(), true).ok());
    {
        std::ofstream out(file);
        out << "{";
    }
    CHECK(!gekko::yomi::load_core_config(file.string(), false).ok());
    {
        std::ofstream out(file);
        out << "{\"ocr\":{\"engines\":[\"\"]}}";
    }
    CHECK(!gekko::yomi::load_core_config(file.string(), true).ok());

    gekko::yomi::CoreConfig saved;
    saved.buffer.ttl_scans = 6;
    saved.buffer.positional_invalidation_scans = 3;
    saved.ocr_engines = std::vector<std::string>{"meiki", "quote\"id"};
    const auto written = dir / "saved.json";
    CHECK(gekko::yomi::save_core_config(written.string(), saved).empty());
    const auto round = gekko::yomi::load_core_config(written.string(), true);
    CHECK(round.ok());
    CHECK(round.warnings.empty());
    CHECK(round.config.buffer.ttl_scans == 6);
    CHECK(round.config.buffer.positional_invalidation_scans == 3);
    CHECK(round.config.ocr_engines.has_value());
    if (round.config.ocr_engines) {
        CHECK(round.config.ocr_engines->size() == 2);
        CHECK((*round.config.ocr_engines)[0] == "meiki");
        CHECK((*round.config.ocr_engines)[1] == "quote\"id");
    }
    saved.ocr_engines.reset();
    CHECK(gekko::yomi::save_core_config(written.string(), saved).empty());
    const auto all = gekko::yomi::load_core_config(written.string(), true);
    CHECK(all.ok());
    CHECK(!all.config.ocr_engines.has_value());
    CHECK(all.config.buffer.ttl_scans == 6);
}

void test_crescent_pluses() {
    const auto marks = gekko::yomi::crescent_plus_marks();
    CHECK(marks.size() >= 8);
    double min_x = 1;
    double max_x = 0;
    double min_y = 1;
    double max_y = 0;
    double mean_x = 0;
    for (const auto& mark : marks) {
        CHECK(mark.x >= 0);
        CHECK(mark.x <= 1);
        CHECK(mark.y >= 0);
        CHECK(mark.y <= 1);
        min_x = std::min(min_x, mark.x);
        max_x = std::max(max_x, mark.x);
        min_y = std::min(min_y, mark.y);
        max_y = std::max(max_y, mark.y);
        mean_x += mark.x;
    }
    mean_x /= static_cast<double>(marks.size());
    // Belly on the left, horns reaching the right, tall enough to be a crescent.
    CHECK(mean_x < 0.5);
    CHECK(min_x < 0.3);
    CHECK(max_x > 0.55);
    CHECK(min_y < 0.2);
    CHECK(max_y > 0.8);
    CHECK(max_x - min_x > 0.25);
}

void test_enabled_engines() {
    gekko::yomi::Registry registry;
    registry.add(std::make_unique<FixtureCapture>());
    registry.add(std::make_unique<NamedOcr>("meiki", "漢字"));
    registry.add(std::make_unique<NamedOcr>("ppocr-manga", "日本語"));

    const auto both = gekko::yomi::read_window(registry, "window-1");
    CHECK(both.ocr_error.empty());
    CHECK(both.engine_id == "meiki+ppocr-manga");
    CHECK(both.runs.size() == 2);
    if (both.runs.size() == 2) {
        CHECK(both.runs[0].text == "日本語");
        CHECK(both.runs[1].text == "漢字");
    }

    const std::vector<std::string> only{"meiki"};
    const auto filtered = gekko::yomi::read_window(registry, "window-1", nullptr, &only);
    CHECK(filtered.engine_id == "meiki");
    CHECK(filtered.runs.size() == 1);
    if (!filtered.runs.empty()) {
        CHECK(filtered.runs[0].text == "漢字");
    }

    const std::vector<std::string> none;
    const auto disabled = gekko::yomi::read_window(registry, "window-1", nullptr, &none);
    CHECK(disabled.runs.empty());
    CHECK(disabled.ocr_error.find("enabled") != std::string::npos);

    gekko::yomi::OcrBufferOptions buffer;
    buffer.promote_after = 1;
    gekko::yomi::HttpServer server(&registry, buffer, only);
    std::string error;
    CHECK(server.listen(0, &error));
    if (server.port() == 0) {
        std::cerr << error << '\n';
        return;
    }
    std::thread thread([&server]() { server.run(); });
    const std::string body = "{\"window_id\":\"window-1\"}";
    const std::string request = "POST /api/read HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                 "Content-Length: " +
                                 std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    const std::string read = http_exchange(server.port(), request);
    CHECK(read.find("\"engine\":\"meiki\"") != std::string::npos);
    CHECK(read.find("漢字") != std::string::npos);
    CHECK(read.find("ppocr-manga") == std::string::npos);
    CHECK(read.find("日本語") == std::string::npos);
    server.stop();
    thread.join();
}

struct ChildRun {
    int status{0};
    std::string output;
    bool finished{false};
};

ChildRun run_gekko(const std::vector<std::string>& args) {
    ChildRun run;
    const char* app = std::getenv("GEKKO_YOMI_APP");
    if (app == nullptr) {
        return run;
    }
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return run;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return run;
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        unsetenv("DISPLAY");
        unsetenv("WAYLAND_DISPLAY");
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(app));
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execv(app, argv.data());
        _exit(127);
    }
    close(pipefd[1]);
    char buf[512];
    ssize_t n = 0;
    while ((n = ::read(pipefd[0], buf, sizeof(buf))) > 0) {
        run.output.append(buf, static_cast<std::size_t>(n));
    }
    close(pipefd[0]);
    for (int i = 0; i < 50 && !run.finished; ++i) {
        const pid_t done = waitpid(pid, &run.status, WNOHANG);
        if (done == pid) {
            run.finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!run.finished) {
        kill(pid, SIGTERM);
        waitpid(pid, &run.status, 0);
    }
    return run;
}

void test_cli_config() {
    const char* app = std::getenv("GEKKO_YOMI_APP");
    CHECK(app != nullptr);
    if (app == nullptr) {
        return;
    }
    const auto help = run_gekko({"--help"});
    CHECK(help.finished);
    CHECK(WIFEXITED(help.status));
    CHECK(WEXITSTATUS(help.status) == 0);
    CHECK(help.output.find("--config") != std::string::npos);

    const auto missing = std::filesystem::temp_directory_path() / "gekko-yomi-cli-missing.json";
    std::filesystem::remove(missing);
    const auto gone = run_gekko({"--config", missing.string()});
    CHECK(gone.finished);
    CHECK(WIFEXITED(gone.status));
    CHECK(WEXITSTATUS(gone.status) != 0);

    const auto bad = std::filesystem::temp_directory_path() / "gekko-yomi-cli-bad.json";
    {
        std::ofstream out(bad);
        out << "{";
    }
    const auto invalid = run_gekko({"--config", bad.string()});
    CHECK(invalid.finished);
    CHECK(WIFEXITED(invalid.status));
    CHECK(WEXITSTATUS(invalid.status) != 0);

    const auto good = std::filesystem::temp_directory_path() / "gekko-yomi-cli-good.json";
    {
        std::ofstream out(good);
        out << "{\"buffer\":{\"ttl_scans\":5,\"positional_invalidation_scans\":2},\"ocr\":{\"engines\":[]}}";
    }
    const auto empty = std::filesystem::temp_directory_path() / "gekko-yomi-cli-plugins";
    std::filesystem::create_directories(empty);
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        setenv("GEKKO_YOMI_PLUGINS", empty.c_str(), 1);
        setenv("GEKKO_YOMI_PORT", "0", 1);
        unsetenv("DISPLAY");
        unsetenv("WAYLAND_DISPLAY");
        const std::string path = good.string();
        execl(app, app, "--config", path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipefd[1]);
    std::string line;
    char byte = 0;
    while (line.size() < 200) {
        const ssize_t n = ::read(pipefd[0], &byte, 1);
        if (n <= 0) {
            break;
        }
        if (byte == '\n') {
            break;
        }
        line.push_back(byte);
    }
    close(pipefd[0]);
    CHECK(port_from_url(line) > 0);
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFSIGNALED(status) || WIFEXITED(status));
}

bool maps_contain(const std::string& path) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line)) {
        if (line.find(path) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool load_errors_contain(const gekko::yomi::Registry& registry, const char* needle) {
    for (const auto& error : registry.load_errors()) {
        if (error.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void test_plugin_lifetime() {
    const char* dir = std::getenv("GEKKO_YOMI_PROBE_DIR");
    CHECK(dir != nullptr);
    if (dir == nullptr) {
        return;
    }
    setenv("GEKKO_YOMI_PLUGINS", dir, 1);

    gekko::yomi::Registry registry;
    registry.load();
    CHECK(registry.ocr_engines().size() == 1);
    CHECK(registry.dictionaries().empty());
    CHECK(registry.module_paths().size() == 1);
    CHECK(load_errors_contain(registry, "duplicate plugin id probe"));
    CHECK(load_errors_contain(registry, "probe dictionary declined"));
    if (registry.ocr_engines().empty() || registry.module_paths().empty()) {
        return;
    }

    auto* runtime = registry.ocr_engines().front()->runtime();
    CHECK(runtime != nullptr);
    if (runtime == nullptr) {
        return;
    }
    CHECK(runtime->status().state == gekko::yomi::RuntimeState::Stopped);
    CHECK(runtime->start().ok());
    CHECK(runtime->status().state == gekko::yomi::RuntimeState::Running);
    CHECK(runtime->start().ok());
    CHECK(runtime->status().state == gekko::yomi::RuntimeState::Running);
    runtime->stop();
    CHECK(runtime->status().state == gekko::yomi::RuntimeState::Stopped);

    const std::string path = registry.module_paths().front();
    CHECK(maps_contain(path));
    registry.begin_scan();
    const auto blocked = registry.unload(path);
    CHECK(!blocked.ok());
    CHECK(blocked.error.find("scan") != std::string::npos);
    CHECK(registry.ocr_engines().size() == 1);
    CHECK(!registry.release_instance("probe").ok());
    CHECK(!registry.create_instance("probe").ok());
    registry.end_scan();

    CHECK(registry.release_instance("probe").ok());
    CHECK(registry.ocr_engines().empty());
    CHECK(registry.module_paths().size() == 1);
    CHECK(maps_contain(path));
    CHECK(registry.create_instance("probe").ok());
    CHECK(registry.ocr_engines().size() == 1);
    if (!registry.ocr_engines().empty()) {
        CHECK(registry.ocr_engines().front()->id() == "probe");
    }
    CHECK(!registry.create_instance("probe").ok());
    const auto declined = registry.create_instance("probe-dict");
    CHECK(!declined.ok());
    CHECK(declined.error.find("declined") != std::string::npos);
    CHECK(registry.dictionaries().empty());

    CHECK(registry.unload(path).ok());
    CHECK(registry.ocr_engines().empty());
    CHECK(registry.module_paths().empty());
    CHECK(!maps_contain(path));
    CHECK(!registry.unload(path).ok());

    registry.load();
    CHECK(registry.ocr_engines().size() == 1);
    CHECK(registry.module_paths().size() == 1);

    gekko::yomi::Registry limited;
    limited.limit_ocr_engines({});
    limited.load();
    CHECK(limited.ocr_engines().empty());
    CHECK(limited.module_paths().size() == 1);
    CHECK(limited.create_instance("probe").ok());
    CHECK(limited.ocr_engines().size() == 1);
}

void test_ocr_stop_closes_worker() {
    const char* script = std::getenv("GEKKO_YOMI_FAKE_WORKER");
    CHECK(script != nullptr);
    if (script == nullptr) {
        return;
    }
    gekko::yomi::ocr_worker::PythonOcrSpec spec;
    spec.id = "meiki";
    spec.description = "fixture";
    spec.model = "fixture";
    spec.character_boxes = true;
    spec.script_env = "GEKKO_YOMI_FAKE_WORKER";
    spec.script_filename = "fake_ocr_worker.py";
    gekko::yomi::ocr_worker::PythonOcrEngine engine(std::move(spec));
    CHECK(engine.runtime() == &engine);
    CHECK(engine.status().state == gekko::yomi::RuntimeState::Stopped);
    const auto started = engine.start();
    if (!started.ok()) {
        std::cerr << started.error << '\n';
    }
    CHECK(started.ok());
    if (!started.ok()) {
        return;
    }
    CHECK(engine.status().state == gekko::yomi::RuntimeState::Running);
    CHECK(engine.start().ok());
    gekko::yomi::Frame frame;
    frame.width = 2;
    frame.height = 1;
    frame.stride_bytes = 8;
    frame.pixels = {1, 2, 3, 255, 4, 5, 6, 255};
    const auto pass = engine.recognize(frame, {});
    if (!pass.ok()) {
        std::cerr << pass.error << '\n';
    }
    CHECK(pass.ok());
    if (pass.ok() && pass.value && !pass.value->runs.empty()) {
        CHECK(pass.value->runs.front().text == "漢字");
    }
    engine.stop();
    CHECK(engine.status().state == gekko::yomi::RuntimeState::Stopped);
    const auto restarted = engine.start();
    if (!restarted.ok()) {
        std::cerr << restarted.error << '\n';
    }
    CHECK(restarted.ok());
    CHECK(engine.status().state == gekko::yomi::RuntimeState::Running);
    engine.stop();
    CHECK(engine.status().state == gekko::yomi::RuntimeState::Stopped);
}

void test_wayland_runs_same_geometry() {
    if (std::getenv("WAYLAND_DISPLAY") == nullptr) {
        std::cerr << "SKIP no WAYLAND_DISPLAY\n";
        std::exit(77);
    }
    // Same assertions as the unit geometry test. The fixture is the session:
    // this process is running on a Wayland display. The numbers do not change.
    test_geometry();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "core";
    if (which == "wayland") {
        test_wayland_runs_same_geometry();
    } else if (which == "core") {
        test_geometry();
        test_result();
        test_registry_loads_fake();
        test_registry_reports_missing_symbol();
        test_plugin_lifetime();
        test_ocr_stop_closes_worker();
        test_explicit_plugin_path_replaces_defaults();
        test_ocr_buffer();
        test_core_config_file();
        test_crescent_pluses();
        test_enabled_engines();
        test_cli_config();
        test_app_serves_without_plugins();
        test_kwin_ids_and_pixels();
    } else if (which == "app") {
        test_app_suite();
    } else if (which == "kwin") {
        test_kwin_session();
    } else {
        std::cerr << "unknown suite: " << which << '\n';
        return 2;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " failed\n";
        return 1;
    }
    return 0;
}
