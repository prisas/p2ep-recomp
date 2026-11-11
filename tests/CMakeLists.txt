add_executable(p2_tests
    test_main.cpp
    test_allegrex.cpp
    test_common.cpp
    test_disc.cpp
    test_psp.cpp)
target_link_libraries(p2_tests PRIVATE p2_common p2_disc p2_psp p2_allegrex)

add_test(NAME unit COMMAND p2_tests)
