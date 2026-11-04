cmake_minimum_required(VERSION 3.16)
project(gekko-yomi VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The tray and settings window is not in the tree yet, so the default is OFF
# and turning it on is an error. When that target exists, flip this default
# to ON. A normal build should include the Qt window. OFF stays available
# for a core-only build. gekko_yomi_core itself never links Qt.
option(GEKKO_YOMI_QT_SHELL "Build the Qt6 tray and settings window" OFF)
if(GEKKO_YOMI_QT_SHELL)
    message(FATAL_ERROR
        "GEKKO_YOMI_QT_SHELL is not implemented yet. "
        "Leave it OFF until the settings window lands. "
        "The default becomes ON at that point.")
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

add_executable(gekko-yomi src/app/main.cpp)
target_compile_options(gekko-yomi PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(gekko-yomi PRIVATE gekko_yomi_core)

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

    add_library(gekko_yomi_empty_plugin SHARED tests/empty_plugin.cpp)
    target_compile_options(gekko_yomi_empty_plugin PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(gekko_yomi_empty_plugin PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-plugins/bad
        OUTPUT_NAME empty
        PREFIX ""
    )

    add_executable(gekko-yomi-tests tests/tests.cpp)
    target_link_libraries(gekko-yomi-tests PRIVATE gekko_yomi_core)
    target_compile_options(gekko-yomi-tests PRIVATE -Wall -Wextra -Wpedantic)

    add_dependencies(gekko-yomi-tests gekko_yomi_fake_capture gekko_yomi_empty_plugin)

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
endif()
