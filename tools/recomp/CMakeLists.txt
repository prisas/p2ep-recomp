add_executable(p2recomp
    game.cpp
    main.cpp)
target_link_libraries(p2recomp PRIVATE p2_common p2_disc p2_psp p2_allegrex)
