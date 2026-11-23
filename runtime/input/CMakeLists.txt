# g6input: GameInput for the guest. So far a stand-in with no devices, so the boot can go on;
# keyboard, mouse, and gamepads on the host come at M4.
add_library(g6input STATIC
    game_input.cpp)
target_include_directories(g6input PUBLIC include)
target_link_libraries(g6input PUBLIC g6common)
g6_target_defaults(g6input)
