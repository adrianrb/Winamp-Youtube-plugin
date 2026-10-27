if(NOT DEFINED VERSION_FILE)
    message(FATAL_ERROR "VERSION_FILE nao definido.")
endif()

if(GEN_YOUTUBE_NO_BUMP)
    message(STATUS "Versao mantida (GEN_YOUTUBE_NO_BUMP).")
    return()
endif()

file(READ "${VERSION_FILE}" VERSION)
string(STRIP "${VERSION}" VERSION)
if(NOT VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR "Versao invalida em ${VERSION_FILE}: '${VERSION}' (esperado X.Y.Z).")
endif()

math(EXPR NEXT_PATCH "${CMAKE_MATCH_3} + 1")
file(WRITE "${VERSION_FILE}" "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${NEXT_PATCH}\n")
message(STATUS "Proxima versao: ${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${NEXT_PATCH}")
