cmake_minimum_required(VERSION 3.30)

project(afsha VERSION 0.0.1)

configure_file(src/afsha_config.h.in src/afsha_config.h)

add_subdirectory(external/SDL)

add_executable(${PROJECT_NAME} src/main.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE SDL3::SDL3)
target_include_directories(${PROJECT_NAME} PUBLIC "${PROJECT_BINARY_DIR}")
