# rpfscan: a census of the game's RPF9 archives (docs/rpf.md), carving each data area into zlib
# streams without the tables of contents. The reader is in g6common. Its output is derived from
# the user's files and stays on their machine.
add_executable(rpfscan src/main.cpp)
target_link_libraries(rpfscan PRIVATE g6common)
g6_target_defaults(rpfscan)
