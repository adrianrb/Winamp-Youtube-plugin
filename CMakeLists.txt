cmake_minimum_required(VERSION 3.16)
project(gen_youtube LANGUAGES CXX RC)

option(GEN_YOUTUBE_NO_BUMP "Nao incrementar version.txt apos o build" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(gen_youtube SHARED
    src/gen_youtube.cpp
    src/gen_youtube.rc
)

target_include_directories(gen_youtube PRIVATE
    third_party/winamp
    src
)

target_compile_definitions(gen_youtube PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(gen_youtube PRIVATE
    user32
    gdi32
    shell32
    comdlg32
    ws2_32
)

if(MSVC)
    target_compile_options(gen_youtube PRIVATE /utf-8 /W3)
else()
    target_compile_options(gen_youtube PRIVATE -Wall -Wextra)
    target_link_options(gen_youtube PRIVATE -static -static-libgcc -static-libstdc++)
endif()

set_target_properties(gen_youtube PROPERTIES
    PREFIX ""
    OUTPUT_NAME "gen_youtube"
)

add_custom_command(
    TARGET gen_youtube POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DVERSION_FILE=${CMAKE_CURRENT_SOURCE_DIR}/version.txt
        -DGEN_YOUTUBE_NO_BUMP=${GEN_YOUTUBE_NO_BUMP}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/BumpVersion.cmake
    COMMENT "Atualizando version.txt"
    VERBATIM
)
