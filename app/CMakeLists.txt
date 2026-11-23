# The runner, g6recomp.exe (docs/execution-model.md, phase 5, "The runner"). It identifies the
# build, mounts the console's drives, loads the main executable under the default import policy,
# and runs it. The work is in a library, so the tests reach it without starting a process.
g6_write_known_builds(${CMAKE_CURRENT_BINARY_DIR}/include/g6/app/known_builds.h)

add_library(g6runner STATIC g6recomp/src/runner.cpp)
target_include_directories(g6runner PUBLIC g6recomp/include ${CMAKE_CURRENT_BINARY_DIR}/include)
target_link_libraries(g6runner PUBLIC g6imports g6loader g6io g6gdk g6common PRIVATE bcrypt)
g6_target_defaults(g6runner)

# The guest executable's static TLS takes the runner's slot, index 0 (docs/execution-model.md,
# "TLS and the TEB"), so the runner links g6exetls and declares no TLS of its own.
add_executable(g6recomp g6recomp/src/main.cpp)
target_link_libraries(g6recomp PRIVATE g6runner g6exetls)
target_link_options(g6recomp PRIVATE /SUBSYSTEM:CONSOLE)
g6_target_defaults(g6recomp)
