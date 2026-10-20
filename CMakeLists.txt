cmake_minimum_required(VERSION 3.20)
project(danin_fighters C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz)
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(raylib json)

add_executable(briga src/main.cpp src/fighter.cpp src/sprites.cpp src/typography.cpp)
target_link_libraries(briga PRIVATE raylib nlohmann_json::nlohmann_json)

if(EMSCRIPTEN)  # celular/navegador: build-web/index.html + .wasm + .data (assets) + arquivos do app (web/)
  set_target_properties(briga PROPERTIES OUTPUT_NAME index SUFFIX ".html" LINK_DEPENDS "${CMAKE_SOURCE_DIR}/web/shell.html")
  target_link_options(briga PRIVATE -sUSE_GLFW=3 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=268435456 -sSTACK_SIZE=1048576 -sEXPORTED_RUNTIME_METHODS=HEAPF32  # HEAPF32: áudio do raylib
    "SHELL:--preload-file \"${CMAKE_SOURCE_DIR}/assets@assets\""
    "SHELL:--shell-file \"${CMAKE_SOURCE_DIR}/web/shell.html\"")
  foreach(f manifest.webmanifest sw.js icon-192.png icon-512.png)
    configure_file(web/${f} ${CMAKE_BINARY_DIR}/${f} COPYONLY)
  endforeach()
  return()
endif()

add_executable(fight_test src/fight_test.cpp src/fighter.cpp src/sprites.cpp src/typography.cpp)
target_link_libraries(fight_test PRIVATE raylib nlohmann_json::nlohmann_json)

if(MSVC)
  target_compile_options(briga PRIVATE /utf-8 /W3)
  target_compile_options(fight_test PRIVATE /utf-8)
endif()
