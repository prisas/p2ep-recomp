add_executable(p2_tests
    test_main.cpp
    test_common.cpp)
target_link_libraries(p2_tests PRIVATE p2_common)

add_test(NAME unit COMMAND p2_tests)
