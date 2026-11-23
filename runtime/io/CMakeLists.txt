# g6io: the console's file system on the host (docs/io.md). Console paths resolve to the user's
# install, mounted read-only, or to a per-user data directory; the HLE file functions in
# runtime/imports open files only through it.
add_library(g6io STATIC
    src/file_system.cpp
    src/path.cpp)
target_include_directories(g6io PUBLIC include)
target_link_libraries(g6io PUBLIC g6common)
g6_target_defaults(g6io)
