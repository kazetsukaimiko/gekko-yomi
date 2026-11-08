#pragma once

#include "config.hpp"

#include <string>

namespace gekko::yomi {

class HttpServer;
class Registry;

// Qt on the calling thread. The server runs on a side thread until the tray
// quits. Returns the QApplication exit code. Call this only when a display is set.
int run_qt_shell(int argc, char** argv, HttpServer* server, Registry* registry, const std::string& config_path,
                 CoreConfig config, const std::string& page_url);

}  // namespace gekko::yomi
