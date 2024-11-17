cmake_minimum_required(VERSION 3.30)

set (CMAKE_CXX_STANDARD 11)

project(afsha VERSION 0.0.1)
if(APPLE)
    enable_language(OBJC)
endif()

configure_file(src/app_config.h.in src/app_config.h)

find_package(OpenGL REQUIRED)
add_subdirectory(external/SDL)
add_subdirectory(external/whisper.cpp)
add_definitions(-DSDL_SHARED=OFF -DSDL_STATIC=ON -DBUILD_SHARED_LIBS=OFF)
include_directories(external/imgui/backends external/imgui ${OPENGL_INCLUDE_DIRS} src/include)

file(GLOB IMGUI_SRC
    "external/imgui/*.h"
    "external/imgui/*.cpp"
    "external/imgui/backends/imgui_impl_opengl3.h"
    "external/imgui/backends/imgui_impl_opengl3.cpp"
    "external/imgui/backends/imgui_impl_sdl3.h"
    "external/imgui/backends/imgui_impl_sdl3.cpp"
)

add_library(imgui STATIC "${IMGUI_SRC}")

add_executable(${PROJECT_NAME} "${imgui}" src/main.cpp src/include/wav_writer.h)
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)
target_link_libraries(${PROJECT_NAME} PRIVATE whisper)
target_link_libraries(imgui PRIVATE SDL3::SDL3)
target_link_libraries(${PROJECT_NAME} PRIVATE imgui)
target_link_libraries(${PROJECT_NAME} PRIVATE ${OPENGL_LIBRARIES})
target_include_directories(${PROJECT_NAME} PUBLIC "${PROJECT_BINARY_DIR}")

