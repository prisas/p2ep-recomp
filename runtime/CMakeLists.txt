include("${CMAKE_SOURCE_DIR}/cmake/GameCode.cmake")

p2ep_add_game_code(p2_game_code IMAGE "${P2EP_IMAGE}" SHARDS ${P2EP_SHARDS})

find_package(SDL3 CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_program(GLSLANG_EXECUTABLE NAMES glslang glslangValidator
    HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/glslang" REQUIRED)

# Shaders are compiled to SPIR-V at build time and embedded as C arrays.
set(shader_dir "${CMAKE_BINARY_DIR}/shaders")
set(shader_headers "")
foreach(shader ge.vert ge.frag)
    string(REPLACE "." "_" name "${shader}")
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/gfx/vk/shaders/${shader}")
    add_custom_command(
        OUTPUT "${shader_dir}/${name}.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${shader_dir}"
        COMMAND "${GLSLANG_EXECUTABLE}" -V --quiet --vn "${name}_spv" -o "${shader_dir}/${name}.h" "${source}"
        DEPENDS "${source}"
        COMMENT "Compiling shader ${shader}"
        VERBATIM)
    list(APPEND shader_headers "${shader_dir}/${name}.h")
endforeach()

add_executable(p2ep
    main.cpp
    cpu/rt.cpp
    ge/ge.cpp
    ge/ge_texture.cpp
    gfx/vk/vk_renderer.cpp
    hle/hle_display.cpp
    hle/hle_ge.cpp
    hle/hle_io.cpp
    hle/hle_kernel.cpp
    hle/hle_stubs.cpp
    hle/hle_system.cpp
    hle/hle_threadman.cpp
    kernel/core.cpp
    kernel/sysmem.cpp
    mem/memory.cpp
    platform/frame.cpp
    platform/host.cpp
    platform/window.cpp
    ${shader_headers}
    $<TARGET_OBJECTS:p2_game_code>)
add_dependencies(p2ep p2_game_code_generate)
target_include_directories(p2ep PRIVATE "${CMAKE_BINARY_DIR}/generated" "${shader_dir}" ${Stb_INCLUDE_DIR})
target_link_libraries(p2ep PRIVATE
    p2_common p2_disc p2_psp
    SDL3::SDL3 Vulkan::Headers volk::volk GPUOpen::VulkanMemoryAllocator)
if(P2EP_LTO)
    target_compile_options(p2ep PRIVATE /clang:-flto=thin)
    target_link_options(p2ep PRIVATE /clang:-flto=thin)
endif()
