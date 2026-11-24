# g6gpu: the GPU side of the runtime (CLAUDE.md, "GPU"): memory_tracker, which owns the title's
# memory as the console's memory functions describe it, and the D3D12.X device the title gets
# (docs/d3d12x.md, "Our device"), with the command queues and lists it makes. onecore for
# VirtualAlloc2 and the other placeholder functions.
add_library(g6gpu STATIC
    command.cpp
    device.cpp
    memory_tracker.cpp
    proxy.cpp)
target_include_directories(g6gpu PUBLIC include)
target_link_libraries(g6gpu PUBLIC g6common d3d12 PRIVATE onecore)
g6_target_defaults(g6gpu)
