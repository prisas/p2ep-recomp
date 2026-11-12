include("${CMAKE_SOURCE_DIR}/cmake/GameCode.cmake")

p2ep_add_game_code(p2_game_code IMAGE "${P2EP_IMAGE}" SHARDS ${P2EP_SHARDS})

add_executable(p2ep
    main.cpp
    cpu/rt.cpp
    hle/hle_display.cpp
    hle/hle_io.cpp
    hle/hle_kernel.cpp
    hle/hle_stubs.cpp
    hle/hle_system.cpp
    hle/hle_threadman.cpp
    kernel/core.cpp
    kernel/sysmem.cpp
    mem/memory.cpp
    platform/host.cpp
    $<TARGET_OBJECTS:p2_game_code>)
add_dependencies(p2ep p2_game_code_generate)
target_include_directories(p2ep PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(p2ep PRIVATE p2_common p2_disc p2_psp)
if(P2EP_LTO)
    target_compile_options(p2ep PRIVATE /clang:-flto=thin)
    target_link_options(p2ep PRIVATE /clang:-flto=thin)
endif()
