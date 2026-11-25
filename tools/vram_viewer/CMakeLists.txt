find_package(SDL2 REQUIRED)

add_executable(vram_viewer
    main.c
    vram.c
    pal_sdl2.c
)

target_include_directories(vram_viewer PRIVATE .)
target_link_libraries(vram_viewer PRIVATE SDL2::SDL2)
target_compile_options(vram_viewer PRIVATE -Wall -Wextra)
