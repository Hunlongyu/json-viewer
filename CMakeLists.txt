cmake_minimum_required(VERSION 3.28)

project(json_viewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options("/utf-8")
endif()

include(FetchContent)
FetchContent_Declare(
    sw
    GIT_REPOSITORY https://github.com/Mzying2001/sw
    GIT_TAG        0.0.7
    SOURCE_SUBDIR  sw
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(sw)

FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson
    GIT_TAG v4.1.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(simdjson)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG v3.12.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)

set(Files 
    "src/main.cpp"
    "src/views/viewer.cpp"
)

enable_language(RC)
set(CMAKE_RC_COMPILER_INIT windres)

if(CMAKE_BUILD_TYPE STREQUAL "Debug") 
    add_executable(${PROJECT_NAME} ${Files})
else()
    add_executable(${PROJECT_NAME} WIN32 ${Files})
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/assets
)

target_link_libraries(${PROJECT_NAME} PRIVATE 
    sw
    simdjson
    nlohmann_json::nlohmann_json
)