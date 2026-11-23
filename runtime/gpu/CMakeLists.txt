# g6gpu: the GPU side of the runtime (CLAUDE.md, "GPU"). So far only memory_tracker, which owns
# the title's memory as the console's memory functions describe it; the D3D12.X wrapper joins
# it at M3. onecore for VirtualAlloc2 and the other placeholder functions.
add_library(g6gpu STATIC
    memory_tracker.cpp)
target_include_directories(g6gpu PUBLIC include)
target_link_libraries(g6gpu PUBLIC g6common PRIVATE onecore)
g6_target_defaults(g6gpu)
