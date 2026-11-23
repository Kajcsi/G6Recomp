# g6kernel: console OS services. So far only the image loader, which maps a guest module at its
# assigned base so that guest pointers are host pointers.
add_library(g6kernel STATIC src/image_loader.cpp)
target_include_directories(g6kernel PUBLIC include)
target_link_libraries(g6kernel PUBLIC g6common)
g6_target_defaults(g6kernel)
