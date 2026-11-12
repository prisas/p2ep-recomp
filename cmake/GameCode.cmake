# Recompiles the game executable into C++ at build time and compiles the result as an object library.
#
#   p2ep_add_game_code(<target> IMAGE <disc image> SHARDS <count>)
#
# The generated sources live in the build tree and are never part of the repository.

function(p2ep_add_game_code target)
    cmake_parse_arguments(ARG "" "IMAGE;SHARDS" "" ${ARGN})

    set(gen_dir "${CMAKE_BINARY_DIR}/generated")
    set(api_dir "${CMAKE_SOURCE_DIR}/data/psp_api")
    set(entries_file "${CMAKE_SOURCE_DIR}/data/extra_entries.txt")

    set(sources "${gen_dir}/gen_table.cpp")
    math(EXPR last "${ARG_SHARDS} - 1")
    foreach(index RANGE 0 ${last})
        if(index LESS 10)
            set(name "gen_00${index}.cpp")
        elseif(index LESS 100)
            set(name "gen_0${index}.cpp")
        else()
            set(name "gen_${index}.cpp")
        endif()
        list(APPEND sources "${gen_dir}/${name}")
    endforeach()

    file(GLOB api_files CONFIGURE_DEPENDS "${api_dir}/*.txt")
    set(entries_args "")
    set(entries_deps "")
    if(EXISTS "${entries_file}")
        set(entries_args --entries "${entries_file}")
        set(entries_deps "${entries_file}")
    endif()

    set(blobs_file "${CMAKE_SOURCE_DIR}/data/code_blobs.txt")
    if(EXISTS "${blobs_file}")
        list(APPEND entries_args --blobs "${blobs_file}")
        list(APPEND entries_deps "${blobs_file}")
    endif()

    # The tool rewrites only the files whose content changed, so an unchanged shard is not recompiled.
    add_custom_command(
        OUTPUT "${gen_dir}/gen.stamp"
        BYPRODUCTS ${sources} "${gen_dir}/gen_funcs.h" "${gen_dir}/gen_config.h" "${gen_dir}/gen_report.txt"
        COMMAND p2recomp gen
                --image "${ARG_IMAGE}"
                --out "${gen_dir}"
                --api "${api_dir}"
                --shards ${ARG_SHARDS}
                --stamp "${gen_dir}/gen.stamp"
                ${entries_args}
        DEPENDS p2recomp "${ARG_IMAGE}" ${api_files} ${entries_deps}
        COMMENT "Recompiling the game executable to C++"
        VERBATIM)
    add_custom_target(${target}_generate DEPENDS "${gen_dir}/gen.stamp")

    add_library(${target} OBJECT ${sources})
    add_dependencies(${target} ${target}_generate)
    target_include_directories(${target} PUBLIC "${CMAKE_SOURCE_DIR}" "${gen_dir}")
    target_compile_definitions(${target} PRIVATE NOMINMAX $<$<BOOL:${P2EP_DEV}>:P2EP_DEV=1>)
    # Exact single-precision arithmetic (no contraction), no exceptions, no stack cookies. The
    # generated code is warning-free by construction except for labels and locals it does not use.
    target_compile_options(${target} PRIVATE
        /O2 /Ob2 /GS- /Gw /Gy /GR- /EHs-c- /utf-8
        /clang:-ffp-contract=off /clang:-fno-math-errno /clang:-fno-strict-aliasing /clang:-g0
        -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter)
    if(P2EP_LTO)
        target_compile_options(${target} PRIVATE /clang:-flto=thin)
    endif()
endfunction()
