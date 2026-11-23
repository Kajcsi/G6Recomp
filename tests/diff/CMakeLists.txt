# Differential tests for the recompiler (see test_diff.cpp). The original test program in
# programs/ is built twice as a plain DLL with no CRT, x64recomp translates each build at build
# time, and the generated code is linked into test_diff, which runs both versions side by side.

set(G6_DIFF_CHUNKS 4)

# g6_diff_program(<variant> <assigned base> <extra compile options>...)
function(g6_diff_program variant base)
    set(dll g6diff_${variant})
    set(name diff_${variant})
    add_library(${dll} SHARED programs/diff_program.c)
    set_target_properties(${dll} PROPERTIES OUTPUT_NAME ${name})
    target_compile_options(${dll} PRIVATE /O2 /GS- /Zl ${ARGN})
    target_link_options(${dll} PRIVATE /NOENTRY /NODEFAULTLIB /INCREMENTAL:NO)

    set(out ${CMAKE_CURRENT_BINARY_DIR}/generated/${variant})
    set(sources ${out}/table.cpp)
    math(EXPR last "${G6_DIFF_CHUNKS} - 1")
    foreach(i RANGE ${last})
        list(APPEND sources ${out}/functions_${i}.cpp)
    endforeach()
    # x64recomp rewrites only files whose contents changed, so a stamp records the run.
    add_custom_command(
        OUTPUT ${out}/emit.stamp
        BYPRODUCTS ${sources} ${out}/functions.h
        COMMAND x64recomp emit $<TARGET_FILE:${dll}> --out ${out} --name ${name} --base ${base}
                --chunks ${G6_DIFF_CHUNKS} --strict
        COMMAND ${CMAKE_COMMAND} -E touch ${out}/emit.stamp
        DEPENDS ${dll} x64recomp
        COMMENT "Recompiling ${name}"
        VERBATIM)

    # Generated code: optimized like a release build, and no warnings (it is not hand-written).
    add_library(${name}_recompiled STATIC ${sources} ${out}/emit.stamp)
    target_link_libraries(${name}_recompiled PUBLIC g6cpu)
    target_compile_options(${name}_recompiled PRIVATE /O2 /arch:AVX2 /w)
endfunction()

g6_diff_program(sse2 0x1000000000 /DG6_DIFF_PROBES)
g6_diff_program(avx2 0x1010000000 /arch:AVX2)

g6_add_test(test_diff LABEL nogame
    SOURCES test_diff.cpp
    LIBS g6kernel diff_sse2_recompiled diff_avx2_recompiled)
target_include_directories(test_diff PRIVATE programs)
target_compile_definitions(test_diff PRIVATE
    G6_DIFF_SSE2_PATH="$<TARGET_FILE:g6diff_sse2>"
    G6_DIFF_AVX2_PATH="$<TARGET_FILE:g6diff_avx2>")
add_dependencies(test_diff g6diff_sse2 g6diff_avx2)
