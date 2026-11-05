# Architecture

How the process is put together today, and the plan for loading and unloading plugins. The plan is not implemented. The refactor waits on a review of this file.

The product roadmap is [roadmap.md](roadmap.md). This file is the map of the running process.

## Pieces

| Piece | What it is | What it must not link |
| --- | --- | --- |
| `gekko-yomi` | The executable. `main` is `src/app/main.cpp`. | Qt |
| `gekko_yomi_app` | HTTP, the page, the scan pipeline, logging. | Qt |
| `gekko_yomi_core` | Plugin host, geometry, the scan buffer. Static library, position-independent. | Qt, spdlog, HTTP |
| `plugins/*.so` | One shared library per implementation. Loaded with `dlopen`. | The HTTP server |
| `workers/*.py` | A separate Python process for OCR or readings. Not the application. | — |

`gekko_yomi_core` is linked into the app and, where a plugin calls into it, into that plugin. Each plugin is opened with `RTLD_LOCAL`, so its symbols stay private to that library. The OCR worker code is linked into each OCR plugin, not shared as one global copy between them.

## Entry points

### Process

`main` in `src/app/main.cpp`:

1. `init_logging()`. Logs go to stderr. The pattern is time, logger name, level, message.
2. `Registry::load()`. This is the only plugin load.
3. Log each loaded capture, OCR, reading, and dictionary plugin. Load failures were stored and are logged here. They are not printed from inside `dlopen`.
4. Listen on `0.0.0.0`. `GEKKO_YOMI_PORT` if set, otherwise the first free port from 8765 through 8775.
5. Write one flushed stdout line, `http://127.0.0.1:PORT`, then `HttpServer::run()`.

A test reads that stdout line to learn the port. Anything else on stdout breaks that. Listen failures go to stderr and `main` returns 1.

`HttpServer` (`src/app/http_server.cpp`) accepts one connection at a time on the calling thread. Routes:

| Route | Role |
| --- | --- |
| `GET /` | The page. The HTML is compiled into the binary. |
| `GET /api/windows` | Windows from the first capture plugin. |
| `POST /api/read` | One scan of `{"window_id":...}`. Optional `"watch":true`. |
| `GET /api/frame.bmp` | The last grabbed frame. |

The server keeps one `OcrConsensus` for the window id it is reading. A new id clears it.

### One scan

`read_window` in `src/app/compose.cpp` is the scan.

1. The first `CaptureSource` lists windows and grabs the requested id.
2. Every loaded `OcrEngine` runs on that frame. Two or more engines run on separate threads and are joined before the function continues. One engine runs on the server thread.
3. Latin letters are stripped. A run that is only numbers or symbols is dropped.
4. When the server passed a buffer, each engine's runs update that engine's buffer, then the consensus buffer. A null buffer returns this scan's runs directly. Tests use that.
5. The first `ReadingProvider`, if one is loaded, splits the line. The first `Dictionary`, if one is loaded, glosses those parts.
6. The page list is sorted longest line first.

A capture failure returns before the buffer is touched, so a bad grab does not age tracks.

### Plugins

Every plugin exports one C function:

```cpp
extern "C" void gekko_yomi_register(gekko::yomi::Host* host);
```

The macro is `GEKKO_YOMI_PLUGIN_EXPORT` in `include/gekko_yomi/host.hpp`. `Registry::load` in `src/core/registry.cpp` is the only caller.

Directories, in order:

- If `GEKKO_YOMI_PLUGINS` is set, those colon-separated directories and no others.
- Otherwise `<executable>/plugins`, then `<executable>/../lib/gekko-yomi/plugins`.

A missing directory is skipped. Each regular `.so` is opened with `dlopen(path, RTLD_NOW | RTLD_LOCAL)`. `RTLD_NOW` resolves symbols immediately, so a broken plugin fails at load instead of on the first scan. A missing `gekko_yomi_register` is recorded in `load_errors` and that handle is closed. A library that loads does not prevent the next one from loading.

The interfaces live in `include/gekko_yomi/`. Each one has `id()`, `description()`, and the operation the scan or a later feature calls. The ones with an implementation today are `CaptureSource` (KWin), `OcrEngine` (meiki, ppocr-manga), `ReadingProvider` (kakasi), and `Dictionary` (TSV, and only when `GEKKO_YOMI_DICTIONARY` is set). The others are the contracts for later features. The roadmap names them.

## Loading today

`gekko_yomi_register` constructs an object and gives it away:

```cpp
void gekko_yomi_register(Host* host) {
    host->add(std::make_unique<PythonOcrEngine>(...));
}
```

`Host::add` takes ownership of that `unique_ptr`. After `add` returns, the plugin does not keep the object. The registry stores it in a typed list (`ocr_engines_`, and the same for the other interfaces). That part of the guess was right: registration installs a live instance, not a recipe for making one later.

What already matches the RAII shape, and only at process exit:

1. `Registry`'s destructor destroys every instance first. That runs the C++ destructor in the plugin, which is still mapped, so the code is still there.
2. Then it `dlclose`s each handle.

An OCR engine's destructor destroys its `WorkerSession`, and that destructor closes the Python process. The reading plugin does the same with its text worker. So a normal return from `main` does unload everything, in a safe order. `SIGKILL` does not run destructors. Nothing in the server catches `SIGTERM` either.

What is not possible while the process is running:

- Unload one `.so` and keep the others.
- Destroy one engine and create it again later. `register` is not called a second time. Calling it again on a library that is still open would `add` a second instance.
- Stop a Python worker without destroying the process. The worker starts on the first `recognize` or `analyze`, not in the constructor. It stops only when the engine object is destroyed, which is process exit. Skipping a later `recognize` call would leave a worker that had already started.

KWin does not hold a D-Bus connection on the object. It opens one for the list or grab call and drops it when that call returns. The expensive long-lived runtimes today are the Python workers.

In-process tests call `Host::add` with an object built inside the test binary. They never `dlopen`. That path has to stay. A factory-only boot would make those tests build a `.so` for every fake.

`RTLD_LOCAL` plus a static copy of the core inside a plugin means `dynamic_cast` from the app to a plugin type is the wrong tool. The cast can fail even when the object really is that type, because the two libraries can each carry their own type information. Virtual calls through the base class are the calls that are safe. The plan below does not use `dynamic_cast`.

## Load and unload plan

Three different lifetimes are being treated as one. Split them.

**Library.** The `.so` is mapped. A `PluginModule` object owns the `dlopen` handle. Creating the module loads the library and calls `gekko_yomi_register`. Destroying the module destroys every instance that came from it, drops the factories, then `dlclose`s. That is the RAII pair: construction loads, destruction unloads. The registry owns the modules. Its destructor already has the right order. The new part is doing that to one module while the others stay.

**Instance.** One `OcrEngine`, one `CaptureSource`, and so on. The plugin does not construct it during register. It registers a descriptor and a factory. The host calls the factory when it wants an instance, and it owns the `unique_ptr`. Destroying that pointer unloads the instance. For an OCR engine that also stops the Python worker, because the worker is a member and its destructor already closes the process.

**Runtime.** The long-lived resource inside an instance: a Python process, later a model kept in memory, later a D-Bus connection held open on purpose. The instance can exist while the runtime is stopped. `start`, `stop`, and `status` are for that, not for `dlopen`.

`start` may be called more than once and must be safe the second time. `stop` releases the resource and leaves the C++ object usable. `status` reports stopped, running, or failed, plus a short error string when it failed. `recognize` and `analyze` keep today's behavior of calling `start` when the runtime is stopped, so the first scan still brings the worker up. `stop` is what a disable uses so the CPU and the process actually go away. The destructor calls `stop`, so destroying the instance cannot leak the worker if the caller skipped `stop`.

Interfaces with nothing to start (a capture source that opens D-Bus per call, a dictionary that is just a table) leave the runtime empty. A virtual `runtime()` on each existing interface returns null by default. The OCR and reading plugins override it and return themselves. That is a normal virtual call. It does not need `dynamic_cast`.

### What a plugin registers

A descriptor and a factory, not an instance. The descriptor is the facts the host can show before it builds anything: the id the logs and the config file use, and a description. OCR also reports whether the model has character boxes, so a settings list does not have to construct the engine to find that out.

The factory is a function pointer. A lambda that captures nothing converts to a function pointer, and that is the construction lambda to use:

```cpp
extern "C" void gekko_yomi_register(Host* host) {
    host->register_ocr(
        OcrDescriptor{"meiki", "meikiocr worker (BGRA in, text runs out)", true},
        []() -> std::unique_ptr<OcrEngine> {
            return std::make_unique<PythonOcrEngine>(/* the same spec as today */);
        });
}
```

The host copies the id and the description into its own `std::string`s before `register` returns. It stores the function pointer. It does not store a `std::function`. A `std::function` may allocate the callable inside the plugin. Destroying that object after `dlclose` is a crash. A capture-less function pointer does not own plugin memory. If a factory needs data, the data belongs in the descriptor, or in a plain function written in the plugin, not in captured variables.

One `register_*` per interface, next to the `add` overload that already exists. The host calls the factory and then holds the same `unique_ptr` it holds today. Typed methods keep a capture plugin from being stored in the OCR list without a cast.

`Host::add` stays for tests and for anything the host built itself. Plugins shipped in `plugins/` use `register_*` only.

A factory may decline. The dictionary plugin currently returns from `gekko_yomi_register` without calling `add` when `GEKKO_YOMI_DICTIONARY` is unset, but the `.so` stays mapped with no record of why. After the change it still registers, and the factory returns no object plus an error string when the path is unset. The host records that error and does not treat it as a failed `dlopen`.

Two factories with the same id are a load error. The second is not created. Today the host does not check.

### Unload order

Unload of one module, on the server thread, and only when no scan is using that module:

1. Do not start a new call on its instances.
2. `stop` each runtime, then destroy each instance. The destructor repeats `stop`.
3. Drop the factory pointer.
4. `dlclose`.

The scan joins every OCR thread before `read_window` returns, and the server handles one request at a time. So "no scan is using it" is true between requests, as long as unload runs on that same thread. The first implementation does only that. It does not unload from a Qt thread or from inside `recognize`. If a later UI thread asks for an unload during a scan, the host queues it and runs the four steps after the scan joins. That queue is not part of the first refactor. The rule that makes the first one safe is: unload runs where `read_window` runs, and not while it is on the stack.

Disabling an engine in settings is the instance and the runtime, not the library. The `.so` stays mapped, so enabling it again is a factory call, not another `dlopen`. That is the operation that frees the ppocr worker. Unmapping the file is for removing the plugin, and for process exit.

At boot the host still creates one instance per factory that succeeds, which matches today's "everything that registered is live" behavior. A setting that names a subset creates only those instances. An instance left out is never started. Turning one off later is `stop`, destroy the instance, and keep the factory.

### What the refactor changes, and what it does not

Changes:

- `gekko_yomi_register` in each shipped plugin and in the test `.so` files registers a descriptor and a factory.
- `Registry` grows a module object per successful `dlopen`, with a way to destroy one module.
- OCR and reading gain `start`, `stop`, `status`, with the current lazy start kept.
- A test loads a fake plugin, creates the instance, destroys it, unloads the library, and loads it again.
- A test checks that `stop` on the OCR engine closes the worker.

Does not change:

- The scan, the page, the HTTP routes, or the buffer rules.
- `Host::add` for in-process fakes.
- `GEKKO_YOMI_PLUGINS`, `RTLD_NOW`, and `RTLD_LOCAL`.
- Which plugins exist.
- The Qt window. It is still later. When it exists, it calls this host API. It does not `dlopen` itself.
- A file watcher that loads a `.so` dropped into `plugins/` while the process is up. `PluginModule` is the API that would use. The first refactor does not watch the directory.

The core still does not link Qt. Plugin code still reaches the host only through `gekko_yomi_register` and the virtual calls on the interfaces.
