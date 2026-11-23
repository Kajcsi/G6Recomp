# The runtime that guest code runs on. Each subdirectory is one library; see CLAUDE.md for what
# belongs where.
add_subdirectory(cpu)
add_subdirectory(kernel)
add_subdirectory(loader)
add_subdirectory(io)
add_subdirectory(imports)
