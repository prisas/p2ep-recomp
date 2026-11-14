find_package(Stb REQUIRED)

add_executable(p2_make_icon make_icon.cpp)
target_include_directories(p2_make_icon PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(p2_make_icon PRIVATE p2ep_options)
