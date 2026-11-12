add_executable(p2_tests
    test_main.cpp
    test_allegrex.cpp
    test_common.cpp
    test_cpu_ops.cpp
    test_disc.cpp
    test_psp.cpp)
target_link_libraries(p2_tests PRIVATE p2_common p2_disc p2_psp p2_allegrex)

add_test(NAME unit COMMAND p2_tests)

# --- end-to-end recompiler test ------------------------------------------------------------------------------
# A generator assembles a synthetic guest program and recompiles it; the runner is built from the
# generated C++ and checks every routine against reference results.
add_executable(p2_recomp_testgen recomp_testgen.cpp)
target_link_libraries(p2_recomp_testgen PRIVATE p2_recomp)

set(recomp_test_dir "${CMAKE_BINARY_DIR}/recomp_test")
set(recomp_test_sources "${recomp_test_dir}/gen_000.cpp" "${recomp_test_dir}/gen_table.cpp")
add_custom_command(
    OUTPUT ${recomp_test_sources} "${recomp_test_dir}/gen_funcs.h" "${recomp_test_dir}/gen_config.h"
           "${recomp_test_dir}/gen_cases.inc"
    COMMAND p2_recomp_testgen "${recomp_test_dir}"
    DEPENDS p2_recomp_testgen
    COMMENT "Recompiling the synthetic test program"
    VERBATIM)

add_executable(p2_recomp_tests recomp_run.cpp ${recomp_test_sources})
target_include_directories(p2_recomp_tests PRIVATE "${CMAKE_SOURCE_DIR}" "${recomp_test_dir}")
target_compile_definitions(p2_recomp_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN P2EP_DEV=1)
target_compile_options(p2_recomp_tests PRIVATE
    /O2 /GS- /GR- /EHs-c- /utf-8
    /clang:-ffp-contract=off /clang:-fno-math-errno /clang:-fno-strict-aliasing
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-parameter)

add_test(NAME recomp COMMAND p2_recomp_tests)
