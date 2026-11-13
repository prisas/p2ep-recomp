include("${CMAKE_SOURCE_DIR}/cmake/GameCode.cmake")

p2ep_add_game_code(p2_game_code IMAGE "${P2EP_IMAGE}" SHARDS ${P2EP_SHARDS})

find_package(SDL3 CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(Stb REQUIRED)
find_package(FFMPEG REQUIRED)
find_program(GLSLANG_EXECUTABLE NAMES glslang glslangValidator
    HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/glslang" REQUIRED)

# Dear ImGui draws the settings overlay and the system dialogs. It ships as plain sources.
include(FetchContent)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9b
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(p2_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(p2_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(p2_imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)
target_compile_options(p2_imgui PRIVATE /w)
target_link_libraries(p2_imgui PUBLIC SDL3::SDL3 Vulkan::Headers volk::volk)

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
    audio/audio_out.cpp
    cpu/rt.cpp
    ge/ge.cpp
    ge/ge_texture.cpp
    gfx/vk/vk_renderer.cpp
    hle/hle_atrac.cpp
    hle/hle_audio.cpp
    hle/hle_display.cpp
    hle/hle_ge.cpp
    hle/hle_io.cpp
    hle/hle_kernel.cpp
    hle/hle_mpeg.cpp
    hle/hle_sas.cpp
    hle/hle_system.cpp
    hle/hle_threadman.cpp
    hle/hle_utility.cpp
    kernel/core.cpp
    kernel/sysmem.cpp
    media/codec.cpp
    mem/memory.cpp
    platform/config.cpp
    platform/frame.cpp
    platform/host.cpp
    platform/window.cpp
    ui/overlay.cpp
    ${shader_headers}
    $<TARGET_OBJECTS:p2_game_code>)
add_dependencies(p2ep p2_game_code_generate)
target_include_directories(p2ep PRIVATE
    "${CMAKE_BINARY_DIR}/generated" "${shader_dir}" ${Stb_INCLUDE_DIR} ${FFMPEG_INCLUDE_DIRS})
target_link_directories(p2ep PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(p2ep PRIVATE
    p2_common p2_disc p2_psp p2_imgui
    SDL3::SDL3 Vulkan::Headers volk::volk GPUOpen::VulkanMemoryAllocator ${FFMPEG_LIBRARIES})
if(P2EP_LTO)
    target_compile_options(p2ep PRIVATE /clang:-flto=thin)
    target_link_options(p2ep PRIVATE /clang:-flto=thin)
endif()
