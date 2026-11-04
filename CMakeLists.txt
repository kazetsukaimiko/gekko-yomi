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
target_link_libraries(gekko_yomi_core PUBLIC ${CMAKE_DL_LIBS})

add_executable(gekko-yomi src/app/main.cpp)
target_compile_options(gekko-yomi PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(gekko-yomi PRIVATE gekko_yomi_core)
