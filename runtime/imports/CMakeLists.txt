# g6imports: HLE for the system DLLs the game imports, one folder per DLL, and the default import
# policy that binds every import to it, to the host, or to a stub (docs/side-dlls.md and
# docs/hle-status.md).
add_library(g6imports STATIC
    command_line.cpp
    policy.cpp
    combase/class_activation.cpp
    d3d12_xs/device.cpp
    d3d12_xs/virtual_memory.cpp
    kernelbase/files.cpp
    kernelbase/registry.cpp
    ucrtbase/exit.cpp
    ucrtbase/paths.cpp
    xmem/xmem.cpp)
target_include_directories(g6imports PUBLIC include PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(g6imports PUBLIC g6loader g6io g6gpu g6common PRIVATE g6gdk g6input ole32 runtimeobject)
g6_target_defaults(g6imports)
