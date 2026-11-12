add_library(p2_recomp STATIC
    analysis.cpp
    emit.cpp
    generate.cpp)
target_link_libraries(p2_recomp PUBLIC p2_common p2_psp p2_allegrex)

add_executable(p2recomp
    game.cpp
    main.cpp)
target_link_libraries(p2recomp PRIVATE p2_recomp p2_disc)
