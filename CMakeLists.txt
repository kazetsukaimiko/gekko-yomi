cmake_minimum_required(VERSION 3.16)
project(gekko-yomi VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The tray shell is a later component. The core and the web UI do not link Qt.
# Turning this on is a hard error until that target exists, so a build that
# asked for Qt cannot silently succeed without it.
option(GEKKO_YOMI_QT_SHELL "Build the optional Qt6 tray and settings shell" OFF)
if(GEKKO_YOMI_QT_SHELL)
    message(FATAL_ERROR
        "GEKKO_YOMI_QT_SHELL is not implemented yet. "
        "Leave it OFF. The core builds without Qt.")
endif()

add_library(gekko_yomi_core
    src/core/geometry.cpp
    src/core/registry.cpp
)
target_include_directories(gekko_yomi_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_features(gekko_yomi_core PUBLIC cxx_std_17)
target_compile_options(gekko_yomi_core PRIVATE -Wall -Wextra -Wpedantic)
# Plugins are shared libraries. A static core they link must be built PIC.
set_target_properties(gekko_yomi_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(gekko_yomi_core PUBLIC ${CMAKE_DL_LIBS})

add_library(gekko_yomi_json STATIC src/json/json_mini.cpp)
target_include_directories(gekko_yomi_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/json)
target_compile_options(gekko_yomi_json PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(gekko_yomi_json PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

add_library(gekko_yomi_app STATIC
    src/app/bmp.cpp
    src/app/compose.cpp
    src/app/http_server.cpp
    src/app/page.cpp
)
target_include_directories(gekko_yomi_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
target_link_libraries(gekko_yomi_app PUBLIC gekko_yomi_core gekko_yomi_json Threads::Threads)
target_compile_options(gekko_yomi_app PRIVATE -Wall -Wextra -Wpedantic)

add_executable(gekko-yomi src/app/main.cpp)
target_compile_options(gekko-yomi PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(gekko-yomi PRIVATE gekko_yomi_app)
add_custom_command(TARGET gekko-yomi POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/workers
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/workers/meiki_ocr.py
        ${CMAKE_BINARY_DIR}/workers/meiki_ocr.py
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/workers/kakasi_worker.py
        ${CMAKE_BINARY_DIR}/workers/kakasi_worker.py
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

add_library(gekko_yomi_capture_kwin SHARED
    plugins/capture_kwin/window_ref.cpp
    plugins/capture_kwin/kwin_capture.cpp
)
target_link_libraries(gekko_yomi_capture_kwin PRIVATE gekko_yomi_core PkgConfig::SYSTEMD)
target_compile_options(gekko_yomi_capture_kwin PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(gekko_yomi_capture_kwin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    OUTPUT_NAME capture_kwin
    PREFIX ""
)

add_library(gekko_yomi_ocr_meiki SHARED
    plugins/ocr_meiki/meiki_ocr.cpp
    plugins/ocr_meiki/worker_session.cpp
)
target_link_libraries(gekko_yomi_ocr_meiki PRIVATE gekko_yomi_core gekko_yomi_json)
target_compile_options(gekko_yomi_ocr_meiki PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(gekko_yomi_ocr_meiki PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    OUTPUT_NAME ocr_meiki
    PREFIX ""
)

add_library(gekko_yomi_reading_kakasi SHARED
    plugins/reading_kakasi/text_worker.cpp
    plugins/reading_kakasi/kakasi_reading.cpp
)
target_link_libraries(gekko_yomi_reading_kakasi PRIVATE gekko_yomi_core gekko_yomi_json)
target_compile_options(gekko_yomi_reading_kakasi PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(gekko_yomi_reading_kakasi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/plugins/reading_kakasi
)
set_target_properties(gekko_yomi_reading_kakasi PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    OUTPUT_NAME reading_kakasi
    PREFIX ""
)

add_library(gekko_yomi_dictionary_tsv SHARED
    plugins/dictionary_tsv/tsv_dictionary.cpp
)
target_link_libraries(gekko_yomi_dictionary_tsv PRIVATE gekko_yomi_core)
target_compile_options(gekko_yomi_dictionary_tsv PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(gekko_yomi_dictionary_tsv PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    OUTPUT_NAME dictionary_tsv
    PREFIX ""
)

option(GEKKO_YOMI_BUILD_TESTS "Build unit tests" ON)
if(GEKKO_YOMI_BUILD_TESTS)
    enable_testing()

    add_library(gekko_yomi_fake_capture SHARED tests/fake_capture.cpp)
    target_link_libraries(gekko_yomi_fake_capture PRIVATE gekko_yomi_core)
    target_compile_options(gekko_yomi_fake_capture PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(gekko_yomi_fake_capture PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-plugins/good
        OUTPUT_NAME fake_capture
        PREFIX ""
    )

    add_library(gekko_yomi_fake_ocr SHARED tests/fake_ocr.cpp)
    target_link_libraries(gekko_yomi_fake_ocr PRIVATE gekko_yomi_core)
    target_compile_options(gekko_yomi_fake_ocr PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(gekko_yomi_fake_ocr PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-plugins/ocr
        OUTPUT_NAME fake_ocr
        PREFIX ""
    )

    add_library(gekko_yomi_empty_plugin SHARED tests/empty_plugin.cpp)
    target_compile_options(gekko_yomi_empty_plugin PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(gekko_yomi_empty_plugin PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-plugins/bad
        OUTPUT_NAME empty
        PREFIX ""
    )

    add_executable(gekko-yomi-tests
        tests/tests.cpp
        plugins/capture_kwin/window_ref.cpp
        plugins/ocr_meiki/worker_session.cpp
    )
    target_include_directories(gekko-yomi-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins/capture_kwin
        ${CMAKE_CURRENT_SOURCE_DIR}/plugins/ocr_meiki
    )
    target_link_libraries(gekko-yomi-tests PRIVATE gekko_yomi_app)
    target_compile_options(gekko-yomi-tests PRIVATE -Wall -Wextra -Wpedantic)

    add_dependencies(gekko-yomi-tests
        gekko_yomi_fake_capture
        gekko_yomi_fake_ocr
        gekko_yomi_empty_plugin
        gekko_yomi_capture_kwin
        gekko_yomi_ocr_meiki
        gekko_yomi_reading_kakasi
        gekko_yomi_dictionary_tsv
    )

    # Generator expressions in ENVIRONMENT need CMake 3.22. The command line
    # has accepted them for much longer, which is what the Docker image uses.
    add_test(
        NAME core
        COMMAND
            ${CMAKE_COMMAND} -E env
            "GEKKO_YOMI_APP=$<TARGET_FILE:gekko-yomi>"
            "GEKKO_YOMI_FAKE_DIR=$<TARGET_FILE_DIR:gekko_yomi_fake_capture>"
            "GEKKO_YOMI_EMPTY_DIR=$<TARGET_FILE_DIR:gekko_yomi_empty_plugin>"
            $<TARGET_FILE:gekko-yomi-tests>
            core
    )
    set_tests_properties(core PROPERTIES LABELS unit)

    add_test(NAME wayland_geometry COMMAND gekko-yomi-tests wayland)
    set_tests_properties(wayland_geometry PROPERTIES
        LABELS wayland
        SKIP_RETURN_CODE 77
    )

    add_test(
        NAME kwin_capture
        COMMAND
            ${CMAKE_COMMAND} -E env
            "GEKKO_YOMI_KWIN_DIR=$<TARGET_FILE_DIR:gekko_yomi_capture_kwin>"
            $<TARGET_FILE:gekko-yomi-tests>
            kwin
    )
    set_tests_properties(kwin_capture PROPERTIES
        LABELS wayland
        SKIP_RETURN_CODE 77
    )

    add_test(
        NAME app
        COMMAND
            ${CMAKE_COMMAND} -E env
            "GEKKO_YOMI_APP=$<TARGET_FILE:gekko-yomi>"
            "GEKKO_YOMI_FAKE_DIR=$<TARGET_FILE_DIR:gekko_yomi_fake_capture>"
            "GEKKO_YOMI_FAKE_OCR_DIR=$<TARGET_FILE_DIR:gekko_yomi_fake_ocr>"
            "GEKKO_YOMI_FAKE_WORKER=${CMAKE_SOURCE_DIR}/workers/fake_ocr_worker.py"
            "GEKKO_YOMI_FAKE_KAKASI=${CMAKE_SOURCE_DIR}/workers/fake_kakasi_worker.py"
            "GEKKO_YOMI_READING_SO=$<TARGET_FILE:gekko_yomi_reading_kakasi>"
            "GEKKO_YOMI_DICTIONARY_SO=$<TARGET_FILE:gekko_yomi_dictionary_tsv>"
            $<TARGET_FILE:gekko-yomi-tests>
            app
    )
    set_tests_properties(app PROPERTIES LABELS unit)
endif()
