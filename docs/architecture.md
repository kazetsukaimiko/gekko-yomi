# Architecture

How the process is put together, including how plugins load and unload.

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
2. If `core.json` sets `ocr.engines`, `Registry::limit_ocr_engines` runs with that list. Then `Registry::load()`. Engines named by the list are the ones constructed. Omit the key and every factory that succeeds is constructed. An empty list constructs none. The `.so` files still load either way.
3. Log each loaded capture, OCR, reading, and dictionary plugin. Load failures were stored and are logged here. They are not printed from inside `dlopen`.
4. Listen on `0.0.0.0`. `GEKKO_YOMI_PORT` if set, otherwise the first free port from 8765 through 8775.
5. Write one flushed stdout line, `http://127.0.0.1:PORT`. Then, when the build includes the Qt shell and `DISPLAY` or `WAYLAND_DISPLAY` is set, the tray runs on this thread and `HttpServer::run()` runs on a side thread. Otherwise `run()` stays on this thread. The tray icon is a crescent of plus marks. Its menu opens that URL, edits `core.json`, and quits. A settings save is applied on the server thread between requests.

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
2. Every constructed `OcrEngine` runs on that frame. When the server was given an engine list, an id that is not on it is skipped. Two or more engines run on separate threads and are joined before the function continues. One engine runs on the server thread. `read_window` holds `begin_scan` / `end_scan` for the whole call, so a plugin cannot be unloaded while that scan is on the stack.
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

The interfaces live in `include/gekko_yomi/`. Each one has `id()`, `description()`, and the operation the scan or a later feature calls. The ones with an implementation today are `CaptureSource` (KWin), `OcrEngine` (meiki, ppocr-manga), `ReadingProvider` (kakasi), and `Dictionary` (TSV). The dictionary factory declines when `GEKKO_YOMI_DICTIONARY` is unset. The `.so` stays mapped, and the error is recorded. It is not a failed `dlopen`. The other interfaces are the contracts for later features. The roadmap names them.

`Host::add` still takes a finished object. In-process tests use it. Shipped plugins and the test `.so` files use `register_*` only. An `add` that happens during `gekko_yomi_register` is recorded on that module, so unloading the module destroys it too.

`RTLD_LOCAL` plus a static copy of the core inside a plugin means `dynamic_cast` from the app to a plugin type is the wrong tool. The cast can fail even when the object really is that type, because the two libraries can each carry their own type information. Virtual calls through the base class are the calls that are safe. Nothing here uses `dynamic_cast`.

## Three lifetimes

A library, an instance, and a runtime are three different lifetimes.

**Library.** The `.so` is mapped. One module object per successful `dlopen` owns the handle. Creating the module loads the library and calls `gekko_yomi_register`. Destroying the module destroys every instance that came from it, drops the factories, then `dlclose`s. That is the RAII pair: construction loads, destruction unloads. The registry owns the modules. `Registry::unload(path)` does this to one module and leaves the others mapped. The registry destructor does it for every module. `SIGKILL` does not run destructors. The server does not catch `SIGTERM`.

**Instance.** One `OcrEngine`, one `CaptureSource`, and so on. The plugin does not construct it during register. It registers a descriptor and a factory. The host calls the factory and owns the `unique_ptr`. `Registry::release_instance(id)` stops that object's runtime and destroys it. The factory stays. `Registry::create_instance(id)` calls the factory again, so turning an engine back on is not another `dlopen`. For an OCR engine, destroying the instance also stops the Python worker, because the session is a member and its destructor closes the process.

**Runtime.** The long-lived resource inside an instance: a Python process, later a model kept in memory, later a D-Bus connection held open on purpose. The instance can exist while the runtime is stopped. `start`, `stop`, and `status` are for that, not for `dlopen`.

`start` may be called more than once and must succeed when the runtime is already running. `stop` releases the resource and leaves the C++ object usable. `status` reports `Stopped`, `Running`, or `Failed`, plus a short error string when it failed. It is not const: a status check reaps a worker that has already exited. `recognize` and `analyze` call `start` when the runtime is stopped, so the first scan still brings the worker up. `stop` is what a disable uses so the CPU and the process actually go away. The destructor calls `stop`, so destroying the instance cannot leak the worker if the caller skipped `stop`.

KWin does not hold a D-Bus connection on the object. It opens one for the list or grab call and drops it when that call returns. The dictionary is a table. Those interfaces leave `runtime()` empty: the virtual method returns null by default. OCR and reading override it and return themselves. That is a normal virtual call.

### What a plugin registers

A descriptor and a factory, not an instance. The descriptor is the facts the host can show before it builds anything: the id the logs and the config file use, and a description. OCR also reports whether the model has character boxes, so a settings list does not have to construct the engine to find that out.

The factory is a function pointer. A lambda that captures nothing converts to a function pointer, and that is the construction lambda to use:

```cpp
extern "C" void gekko_yomi_register(Host* host) {
    host->register_ocr(
        OcrDescriptor{"meiki", "meikiocr worker (BGRA in, text runs out)", true},
        [](std::string*) -> std::unique_ptr<OcrEngine> {
            return std::make_unique<PythonOcrEngine>(/* the same spec as today */);
        });
}
```

The host copies the id and the description into its own `std::string`s before `register` returns. It stores the function pointer. It does not store a `std::function`. A `std::function` may allocate the callable inside the plugin. Destroying that object after `dlclose` is a crash. A capture-less function pointer does not own plugin memory. If a factory needs data, the data belongs in the descriptor, or in a plain function written in the plugin, not in captured variables.

One `register_*` per interface, next to the `add` overload that already exists. The host calls the factory and then holds the same `unique_ptr` it holds today. Typed methods keep a capture plugin from being stored in the OCR list without a cast.

`Host::add` stays for tests and for anything the host built itself. Plugins shipped in `plugins/` use `register_*` only.

A factory may decline. It returns a null `unique_ptr` and writes the reason into the `std::string*` argument. The dictionary factory does that when `GEKKO_YOMI_DICTIONARY` is unset. The host records the error, keeps the factory, and does not treat it as a failed `dlopen`. Calling `create_instance` later tries the factory again.

Two factories with the same id are a load error. The second is not stored and not created. The first factory stays.

### Unload order

Unload of one module, on the server thread, and only when no scan is using that module:

1. Fail if a scan is on the stack.
2. `stop` each runtime, then destroy each instance. The destructor repeats `stop`.
3. Drop the factory pointer.
4. `dlclose`.

`read_window` calls `begin_scan` on entry and `end_scan` when it returns. The scan joins every OCR thread before that return, and the server handles one request at a time. `unload`, `release_instance`, and `create_instance` fail while a scan is on the stack. They are meant to run on the server thread, between requests. They do not run from inside `recognize`. The settings window does not call them itself. It posts the new config, and the server thread applies it before the next accept. There is no queue for an unload that arrives from another thread during a scan.

Disabling an engine is the instance and the runtime, not the library. `ocr.engines` in `core.json` chooses which OCR factories are called at boot. An engine left out is never started, and its worker does not run. The `.so` stays mapped. Turning one off later without leaving the process is `release_instance`. Turning it back on is `create_instance`. That is the operation that frees the ppocr worker. Unmapping the file is `unload`, and process exit.

### What this does not do

- The scan, the page, the HTTP routes, and the buffer rules are unchanged.
- `Host::add` for in-process fakes stays.
- `GEKKO_YOMI_PLUGINS`, `RTLD_NOW`, and `RTLD_LOCAL` stay.
- The set of plugins is unchanged.
- The tray does not `dlopen`. It uses the factories the registry already stored. Module files under `modules.d/` are still unread.
- Nothing watches `plugins/` for a `.so` dropped in while the process is up. `unload` and `load` are the calls that would use.

The core still does not link Qt. Plugin code still reaches the host only through `gekko_yomi_register` and the virtual calls on the interfaces.
