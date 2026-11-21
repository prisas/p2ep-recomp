find_package(Stb REQUIRED)
find_package(FFMPEG REQUIRED)
# FFmpeg's filter library, looked up on its own: a build folder configured before it was part of
# the build keeps an FFmpeg library list without it.
find_library(P2EP_AVFILTER_LIBRARY NAMES avfilter HINTS ${FFMPEG_LIBRARY_DIRS} REQUIRED)

# The pixel work and the FFmpeg filter graph, shared with the unit tests.
add_library(p2_texup STATIC texture_ops.cpp upscaler.cpp realesrgan.cpp)
target_include_directories(p2_texup SYSTEM PUBLIC ${FFMPEG_INCLUDE_DIRS})
target_link_directories(p2_texup PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(p2_texup PUBLIC p2_common "${P2EP_AVFILTER_LIBRARY}" ${FFMPEG_LIBRARIES})

# p2texup.exe: makes a texture pack from the game's dumped textures.
add_executable(p2texup main.cpp)
target_include_directories(p2texup PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(p2texup PRIVATE p2_texup)
