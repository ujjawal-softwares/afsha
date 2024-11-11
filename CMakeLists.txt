cmake_minimum_required(VERSION 3.30)

set (CMAKE_CXX_STANDARD 11)

project(afsha VERSION 0.0.1)

configure_file(src/afsha_config.h.in src/afsha_config.h)

add_subdirectory(external/SDL)
# use for -DSDL_SHARED=OFF -DSDL_STATIC=ON
# add_definitions()
include_directories(external/imgui/backends external/imgui)

file(GLOB IMGUI_SRC
    "external/imgui/*.h"
    "external/imgui/*.cpp"
    "external/imgui/backends/imgui_impl_sdlrenderer3.h"
    "external/imgui/backends/imgui_impl_sdlrenderer3.cpp"
    "external/imgui/backends/imgui_impl_sdl3.h"
    "external/imgui/backends/imgui_impl_sdl3.cpp"
)

add_library(imgui STATIC "${IMGUI_SRC}")

add_executable(${PROJECT_NAME} "${imgui}" src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)
target_link_libraries(imgui PRIVATE SDL3::SDL3)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui)
target_include_directories(${PROJECT_NAME} PUBLIC "${PROJECT_BINARY_DIR}")
