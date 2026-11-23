# shaderrecomp_core: reading the console's precompiled shaders (docs/shaders.md): DXBC containers,
# the console's additions to them, the shader records in shaders.rpf, and the RDNA 2 decoder. A library so the tests
# can link it.
add_library(shaderrecomp_core STATIC
    src/archive.cpp
    src/census.cpp
    src/console.cpp
    src/dxbc.cpp
    src/pscache.cpp
    src/rdna2.cpp
    src/rdna2_names.cpp
    src/regs.cpp)
target_include_directories(shaderrecomp_core PUBLIC src)
target_link_libraries(shaderrecomp_core PUBLIC g6common)
g6_target_defaults(shaderrecomp_core)

# shaderrecomp: the command-line tool. Its output is derived from the user's files and stays on
# their machine.
add_executable(shaderrecomp src/main.cpp)
target_link_libraries(shaderrecomp PRIVATE shaderrecomp_core)
g6_target_defaults(shaderrecomp)
