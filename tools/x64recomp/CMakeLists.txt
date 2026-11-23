# x64analysis: code analysis on a PE image (code walker, function discovery, jump tables, the
# pre-flight scan, and RTTI). The x64recomp tool builds on it, and the loader uses it to check
# patch windows and find cpuid sites. dbghelp undecorates RTTI type names.
add_library(x64analysis STATIC
    src/code_walker.cpp
    src/cxx_exceptions.cpp
    src/function_discovery.cpp
    src/jump_table.cpp
    src/rtti.cpp
    src/scan.cpp)
target_include_directories(x64analysis PUBLIC src)
target_link_libraries(x64analysis PUBLIC g6common Zydis PRIVATE dbghelp)
g6_target_defaults(x64analysis)

# x64recomp_core: the frozen C++ generator (docs/codegen.md), a library so tests can link it.
add_library(x64recomp_core STATIC
    src/emit_vector.cpp
    src/emitter.cpp
    src/function_emitter.cpp)
target_link_libraries(x64recomp_core PUBLIC x64analysis)
g6_target_defaults(x64recomp_core)

# x64recomp: the command-line tool.
add_executable(x64recomp src/main.cpp)
target_link_libraries(x64recomp PRIVATE x64recomp_core)
g6_target_defaults(x64recomp)
