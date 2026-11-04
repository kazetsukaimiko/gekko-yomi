#include "gekko_yomi/geometry.hpp"
#include "gekko_yomi/registry.hpp"
#include "gekko_yomi/result.hpp"
#include "bmp.hpp"
#include "compose.hpp"
#include "http_server.hpp"
#include "json_mini.hpp"
#include "window_ref.hpp"
#include "worker_session.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cmath>
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
    const std::string read = http_exchange(server.port(), request);
    CHECK(read.find("漢字") != std::string::npos);
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
    gekko::yomi::meiki::WorkerSession session;
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
    gekko::yomi::meiki::WorkerSession session;
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
        CHECK(registry.load_errors().empty());
        CHECK(registry.reading_providers().size() == 1);
        CHECK(registry.dictionaries().empty());
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

void test_app_suite() {
    test_bmp_and_json();
    test_compose_fills_readings_when_present();
    test_http_with_fakes();
    test_fake_worker();
    test_worker_that_exits_is_not_reused();
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
    for (const auto& load_error : registry.load_errors()) {
        std::cerr << load_error << '\n';
    }
    CHECK(registry.load_errors().empty());
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
        test_explicit_plugin_path_replaces_defaults();
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
