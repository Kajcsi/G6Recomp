# g6common: code shared by the tools and the runtime (logging, PE image parsing, DEFLATE, entropy,
# read-only file mapping, and the RPF9 archive reader the analysis tools use).
# The runtime needs the PE parser too: the loader maps guest images, binds their imports,
# and registers their .pdata/.xdata with Windows.
add_library(g6common STATIC
    src/entropy.cpp
    src/inflate.cpp
    src/log.cpp
    src/mapped_file.cpp
    src/rpf9.cpp
    src/pe/image.cpp
    src/pe/imports.cpp
    src/pe/exports.cpp
    src/pe/relocations.cpp
    src/pe/tls.cpp
    src/pe/load_config.cpp
    src/pe/unwind.cpp
    src/pe/cxx_eh.cpp)
target_include_directories(g6common PUBLIC include)
target_link_libraries(g6common PUBLIC fmt::fmt)
g6_target_defaults(g6common)
