include(FetchContent)

# --- third-party: compressed disc image support -------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(INSTALL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ZLIB OFF CACHE BOOL "" FORCE)
set(WITH_SYSTEM_ZSTD OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZER OFF CACHE BOOL "" FORCE)
set(BUILD_LTO OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libchdr
    GIT_REPOSITORY https://github.com/rtissera/libchdr.git
    GIT_TAG 47d794151b2b64f6aba60304e6480f4f5a2bc24b
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(libchdr)

# --- first-party libraries ----------------------------------------------------------------------------------
add_library(p2_common STATIC
    common/file.cpp
    common/hash.cpp
    common/log.cpp)
target_link_libraries(p2_common PUBLIC p2ep_options)

add_library(p2_disc STATIC
    disc/image.cpp
    disc/iso9660.cpp)
target_link_libraries(p2_disc PUBLIC p2_common PRIVATE chdr-static)

add_library(p2_psp STATIC
    psp/nid.cpp
    psp/prx.cpp
    psp/sfo.cpp)
target_link_libraries(p2_psp PUBLIC p2_common)

add_library(p2_allegrex STATIC
    allegrex/decode.cpp
    allegrex/disasm.cpp)
target_link_libraries(p2_allegrex PUBLIC p2_common)
