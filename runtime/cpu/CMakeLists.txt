# g6cpu: what recompiled code runs on. GuestContext, the inline helpers generated code calls,
# and dispatch from guest addresses to recompiled functions. See docs/codegen.md.
add_library(g6cpu STATIC src/cpuid.cpp src/dispatch.cpp)
target_include_directories(g6cpu PUBLIC include)
target_link_libraries(g6cpu PUBLIC g6common)
# The guest CPU is Zen 2, so generated code uses AVX2 intrinsics; AVX2 is the minimum host.
target_compile_options(g6cpu PUBLIC /arch:AVX2)
g6_target_defaults(g6cpu)
