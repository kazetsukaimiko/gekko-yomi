#pragma once

#include "gekko_yomi/result.hpp"

#include <string>

namespace gekko::yomi {

// A process or other resource held inside a plugin instance. This is not
// the .so. start may be called again and must succeed when already running.
// stop releases the resource and leaves the instance usable. The destructor
// of the instance calls stop.
enum class RuntimeState { Stopped, Running, Failed };

struct RuntimeStatus {
    RuntimeState state{RuntimeState::Stopped};
    std::string detail;
};

class Runtime {
public:
    virtual ~Runtime() = default;

    virtual Status start() = 0;
    virtual void stop() = 0;
    // Not const: a status check reaps a worker that has already exited.
    virtual RuntimeStatus status() = 0;
};

}  // namespace gekko::yomi
