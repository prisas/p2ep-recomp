add_library(p2_common STATIC
    common/file.cpp
    common/hash.cpp
    common/log.cpp)
target_link_libraries(p2_common PUBLIC p2ep_options)
