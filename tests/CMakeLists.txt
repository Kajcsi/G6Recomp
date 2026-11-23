# Test suites. Label `nogame` runs everywhere, including CI; label `game` needs the user's files
# and only runs locally. A test that exits 77 is reported as skipped.

add_library(g6test STATIC framework/g6test_main.cpp)
target_include_directories(g6test PUBLIC framework)
target_link_libraries(g6test PUBLIC fmt::fmt)
g6_target_defaults(g6test)

function(g6_add_test name)
    cmake_parse_arguments(TEST "" "LABEL" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${TEST_SOURCES})
    target_link_libraries(${name} PRIVATE g6test ${TEST_LIBS})
    g6_target_defaults(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS ${TEST_LABEL} SKIP_RETURN_CODE 77)
endfunction()

g6_add_test(test_pe LABEL nogame
    SOURCES unit/test_image.cpp unit/test_unwind.cpp unit/test_cxx_eh.cpp
    LIBS g6common)

g6_add_test(test_inflate LABEL nogame
    SOURCES unit/test_inflate.cpp
    LIBS g6common)

g6_add_test(test_entropy LABEL nogame
    SOURCES unit/test_entropy.cpp
    LIBS g6common)

# Narrow strings cut at whole characters, as Windows' ANSI functions cut a result that doesn't fit.
g6_add_test(test_text LABEL nogame
    SOURCES unit/test_text.cpp
    LIBS g6common)

# Stand-in COM objects for interfaces stubbed before they are implemented.
g6_add_test(test_com LABEL nogame
    SOURCES unit/test_com_stand_in.cpp
    LIBS g6common)

# The RPF9 reader (docs/rpf.md) on an archive the test builds from made-up files.
g6_add_test(test_rpf9 LABEL nogame
    SOURCES unit/test_rpf9.cpp
    LIBS g6common)

# shaderrecomp's reader (docs/shaders.md) on a shader archive and a pipeline cache the tests build
# from made-up shaders, and its RDNA 2 decoder and register model on hand-encoded instructions.
g6_add_test(test_shaderrecomp LABEL nogame
    SOURCES unit/test_shader_archive.cpp unit/test_pscache.cpp unit/test_rdna2.cpp
            unit/test_regs.cpp
    LIBS shaderrecomp_core)

g6_add_test(test_recomp LABEL nogame
    SOURCES unit/test_code_walker.cpp unit/test_discovery.cpp unit/test_jump_table.cpp
        unit/test_scan.cpp unit/test_cxx_map.cpp
    LIBS x64recomp_core)

# Original fixture DLL, built optimized with CFG and a delay-loaded import so the tests see
# the same kinds of linker output a shipped binary has.
add_library(pe_sample SHARED fixtures/pe_sample/pe_sample.c)
target_compile_options(pe_sample PRIVATE /O2 /guard:cf)
target_link_options(pe_sample PRIVATE /guard:cf /DELAYLOAD:user32.dll /INCREMENTAL:NO)
target_link_libraries(pe_sample PRIVATE user32 delayimp)

g6_add_test(test_pe_sample LABEL nogame
    SOURCES fixture/test_pe_sample.cpp
    LIBS x64recomp_core)
target_compile_definitions(test_pe_sample PRIVATE G6_PE_SAMPLE_PATH="$<TARGET_FILE:pe_sample>")
add_dependencies(test_pe_sample pe_sample)

# The pre-flight scan's original test guest (docs/scan.md): hand-written functions in top-level
# assembly, each holding a pattern the scan must find, and compiled thread-local storage. Static C
# runtime for the TLS directory. The test reads it as a file and never loads it, so it has an
# output folder of its own that no loader test searches.
add_library(scan_guest SHARED
    fixtures/scan_guest/scan_guest.c
    fixtures/scan_guest/scan_guest_asm.c
    fixtures/scan_guest/scan_guest.def)
g6_target_defaults(scan_guest)
set_target_properties(scan_guest PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreaded
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/scan_guest)
target_compile_options(scan_guest PRIVATE /O2)
target_link_options(scan_guest PRIVATE /INCREMENTAL:NO)

g6_add_test(test_scan LABEL nogame
    SOURCES fixture/test_scan_guest.cpp
    LIBS x64recomp_core)
target_compile_definitions(test_scan PRIVATE G6_SCAN_GUEST_PATH="$<TARGET_FILE:scan_guest>")
add_dependencies(test_scan scan_guest)

# The RTTI scan's original test guest: polymorphic classes with every kind of inheritance, a
# template, a nested class, and a thrown type. Static C runtime, so the runtime's own classes are
# in the image too. Read as a file, never loaded, from a folder of its own.
add_library(rtti_guest SHARED fixtures/rtti_guest/rtti_guest.cpp)
g6_target_defaults(rtti_guest)
set_target_properties(rtti_guest PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreaded
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/rtti_guest)
target_compile_options(rtti_guest PRIVATE /O2 /GR)
target_link_options(rtti_guest PRIVATE /INCREMENTAL:NO)

g6_add_test(test_rtti LABEL nogame
    SOURCES fixture/test_rtti_guest.cpp
    LIBS x64analysis)
target_compile_definitions(test_rtti PRIVATE G6_RTTI_GUEST_PATH="$<TARGET_FILE:rtti_guest>")
add_dependencies(test_rtti rtti_guest)

# ---- Native loader ----

g6_add_test(test_import_policy LABEL nogame
    SOURCES loader/test_import_policy.cpp
    LIBS g6loader)

# Original guest modules for the loader tests, in a folder of their own that serves as the
# loader's search path. The made-up console DLLs they import exist only as import libraries
# made from .def files: the loader binds them through the import registry, and no such DLL is
# ever on disk.
set(G6_GUEST_DIR ${CMAKE_BINARY_DIR}/loader_guest)
set(G6_FIXTURE_GUEST ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/loader_guest)
set(fake_libs)
foreach(fake g6fakeos g6fakedelay)
    set(lib ${CMAKE_CURRENT_BINARY_DIR}/fake/${fake}.lib)
    add_custom_command(
        OUTPUT ${lib}
        COMMAND ${CMAKE_AR} /nologo /machine:x64 /def:${G6_FIXTURE_GUEST}/${fake}.def /out:${lib}
        DEPENDS ${G6_FIXTURE_GUEST}/${fake}.def
        COMMENT "Making the import library for the made-up ${fake}.dll"
        VERBATIM)
    list(APPEND fake_libs ${lib})
endforeach()
add_custom_target(g6fake_import_libs DEPENDS ${fake_libs})

# Static C runtime, so the guest's exception handling and TLS support are guest code; CFG and a
# delay-loaded import, like a shipped binary.
add_library(loader_guest SHARED
    fixtures/loader_guest/loader_guest.cpp
    fixtures/loader_guest/loader_guest_seh.c)
g6_target_defaults(loader_guest)
set_target_properties(loader_guest PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreaded
    RUNTIME_OUTPUT_DIRECTORY ${G6_GUEST_DIR})
target_compile_options(loader_guest PRIVATE /guard:cf)
target_link_options(loader_guest PRIVATE /guard:cf /INCREMENTAL:NO /DELAYLOAD:g6fakedelay.dll)
target_link_libraries(loader_guest PRIVATE ${fake_libs} delayimp)
add_dependencies(loader_guest g6fake_import_libs)

add_executable(loader_guest_exe fixtures/loader_guest/loader_guest_exe.c)
g6_target_defaults(loader_guest_exe)
set_target_properties(loader_guest_exe PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreaded
    RUNTIME_OUTPUT_DIRECTORY ${G6_GUEST_DIR})
target_compile_options(loader_guest_exe PRIVATE /GS-)
target_link_options(loader_guest_exe PRIVATE
    /ENTRY:g6_guest_exe_entry /SUBSYSTEM:CONSOLE /STACK:0x200000 /INCREMENTAL:NO)
target_link_libraries(loader_guest_exe PRIVATE loader_guest)

# A real DLL the replaced-DLL tests mark as replaced, and a guest that imports it. No C runtime
# and no entry point: all they need is one export each. With nothing to relocate the linker
# leaves out the relocation table, so each gets a preferred base of its own.
foreach(target g6replaced replaced_importer)
    add_library(${target} SHARED fixtures/loader_guest/${target}.c)
    g6_target_defaults(${target})
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${G6_GUEST_DIR})
    target_compile_options(${target} PRIVATE /GS-)
    target_link_options(${target} PRIVATE /NOENTRY /NODEFAULTLIB /INCREMENTAL:NO)
endforeach()
target_link_options(g6replaced PRIVATE /BASE:0x1C0000000)
target_link_options(replaced_importer PRIVATE /BASE:0x1D0000000)
target_link_libraries(replaced_importer PRIVATE g6replaced)

# A DLL whose TLS callback and entry point report every notification to a sink DLL, and whose
# entry point can fail. No C runtime: /ENTRY names the entry point and the source supplies the TLS
# directory, so the loader calls exactly those two routines. The test loads both with LoadLibrary
# to see what Windows does, then init_guest under the native loader with the sink replaced.
add_library(init_sink SHARED fixtures/loader_guest/init_sink.c)
add_library(init_guest SHARED fixtures/loader_guest/init_guest.c)
foreach(target init_sink init_guest)
    g6_target_defaults(${target})
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${G6_GUEST_DIR})
    target_compile_options(${target} PRIVATE /GS-)
    target_link_options(${target} PRIVATE /NODEFAULTLIB /INCREMENTAL:NO)
endforeach()
target_link_options(init_sink PRIVATE /NOENTRY)
target_link_options(init_guest PRIVATE /ENTRY:g6_init_guest_entry)
target_link_libraries(init_guest PRIVATE init_sink)

g6_add_test(test_loader LABEL nogame
    SOURCES loader/test_loader.cpp
    LIBS g6loader)
target_include_directories(test_loader PRIVATE ${G6_FIXTURE_GUEST})
target_compile_definitions(test_loader PRIVATE G6_GUEST_DIR="${G6_GUEST_DIR}")
add_dependencies(test_loader loader_guest loader_guest_exe g6replaced replaced_importer init_sink
    init_guest)

# Crash reports (docs/execution-model.md, "Crash reports"): an original guest DLL with frames for
# the stack walk, a leaf without unwind data that faults, and a call to SetUnhandledExceptionFilter.
# Optimized, with no C runtime and no entry point, and onecore_apiset for its one import, in a
# folder of its own that serves as the loader's search path.
set(G6_STACK_GUEST_DIR ${CMAKE_BINARY_DIR}/stack_guest)
add_library(stack_guest SHARED fixtures/stack_guest/stack_guest.c)
g6_target_defaults(stack_guest)
set_target_properties(stack_guest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${G6_STACK_GUEST_DIR})
target_compile_options(stack_guest PRIVATE /O2 /GS-)
target_link_options(stack_guest PRIVATE /NOENTRY /NODEFAULTLIB /INCREMENTAL:NO)
target_link_libraries(stack_guest PRIVATE onecore_apiset)

g6_add_test(test_crash_report LABEL nogame
    SOURCES loader/test_crash_report.cpp
    LIBS g6loader)
target_include_directories(test_crash_report PRIVATE fixtures/stack_guest)
target_compile_definitions(test_crash_report PRIVATE G6_STACK_GUEST_DIR="${G6_STACK_GUEST_DIR}")
add_dependencies(test_crash_report stack_guest)

# The hook engine's original test guest (docs/hooks.md): hand-written functions in top-level
# assembly, so each patch window holds exactly the instructions a test needs, and C and C++
# callers that catch exceptions thrown below them. Static C runtime, like loader_guest. /EHc-
# because the C++ callers catch exceptions thrown through extern "C" functions.
add_library(hook_guest SHARED
    fixtures/hook_guest/hook_guest.c
    fixtures/hook_guest/hook_guest_asm.c
    fixtures/hook_guest/hook_guest_cpp.cpp
    fixtures/hook_guest/hook_guest.def)
g6_target_defaults(hook_guest)
set_target_properties(hook_guest PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreaded
    RUNTIME_OUTPUT_DIRECTORY ${G6_GUEST_DIR})
target_compile_options(hook_guest PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/EHc->)
target_link_options(hook_guest PRIVATE /INCREMENTAL:NO)

# g6cpu for guest_cpuid, and for AVX2, which the vector register test's inline assembly uses.
g6_add_test(test_hooks LABEL nogame
    SOURCES loader/test_hooks.cpp
    LIBS g6loader g6cpu)
target_include_directories(test_hooks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/hook_guest)
target_compile_definitions(test_hooks PRIVATE G6_GUEST_DIR="${G6_GUEST_DIR}")
add_dependencies(test_hooks hook_guest)

# The C++ exception guest (docs/hooks.md, "C++ exception functions"), built four ways into a
# folder of its own: by clang-cl, which emits __CxxFrameHandler3 tables, and by cl.exe, which
# emits __CxxFrameHandler4 tables, each with and without an EH continuation table. Release /MD
# with /GS, like the side DLLs, and /EHs without /EHc, since the test catches what the guest's
# extern "C" functions throw. cl.exe comes from the developer prompt (CI sets one up); without
# it, the FH4 builds and their tests are skipped. The test copies the host's vcruntime140.dll and
# vcruntime140_1.dll into a subfolder at run time, to map them as guest modules.
set(G6_EH_GUEST_DIR ${CMAKE_BINARY_DIR}/eh_guest)
set(G6_EH_GUEST_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/eh_guest/eh_guest.cpp)
set(eh_guests)
foreach(variant fh3 fh3_ehcont)
    add_library(eh_guest_${variant} SHARED ${G6_EH_GUEST_SOURCE})
    g6_target_defaults(eh_guest_${variant})
    set_target_properties(eh_guest_${variant} PROPERTIES
        MSVC_RUNTIME_LIBRARY MultiThreadedDLL
        RUNTIME_OUTPUT_DIRECTORY ${G6_EH_GUEST_DIR})
    target_compile_options(eh_guest_${variant} PRIVATE /O2 /GS /EHs /EHc-)
    target_link_options(eh_guest_${variant} PRIVATE /INCREMENTAL:NO)
    list(APPEND eh_guests eh_guest_${variant})
endforeach()
target_compile_options(eh_guest_fh3_ehcont PRIVATE /guard:cf /guard:ehcont)
target_link_options(eh_guest_fh3_ehcont PRIVATE /guard:cf /guard:ehcont)

find_program(G6_MSVC_CL cl.exe)
if(G6_MSVC_CL)
    set(G6_EH_GUEST_FH4 1)
    foreach(variant fh4 fh4_ehcont)
        set(dll ${G6_EH_GUEST_DIR}/eh_guest_${variant}.dll)
        set(guard)
        if(variant STREQUAL "fh4_ehcont")
            set(guard /guard:cf /guard:ehcont)
        endif()
        add_custom_command(
            OUTPUT ${dll}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${G6_EH_GUEST_DIR}
            COMMAND ${G6_MSVC_CL} /nologo /LD /MD /O2 /GS /EHs ${guard} ${G6_EH_GUEST_SOURCE}
                /Fo${CMAKE_CURRENT_BINARY_DIR}/eh_guest_${variant}.obj /Fe${dll}
                /link /INCREMENTAL:NO ${guard}
            DEPENDS ${G6_EH_GUEST_SOURCE}
            COMMENT "Building eh_guest_${variant}.dll with cl.exe"
            VERBATIM)
        list(APPEND eh_guest_msvc_dlls ${dll})
    endforeach()
    add_custom_target(eh_guest_msvc DEPENDS ${eh_guest_msvc_dlls})
    list(APPEND eh_guests eh_guest_msvc)
else()
    set(G6_EH_GUEST_FH4 0)
    message(WARNING "cl.exe isn't on the path, so the FH4 builds of eh_guest and their tests are "
                    "skipped; configure from a Visual Studio developer prompt to include them")
endif()

g6_add_test(test_cxx_hooks LABEL nogame
    SOURCES loader/test_cxx_hooks.cpp
    LIBS g6loader x64analysis)
target_compile_definitions(test_cxx_hooks PRIVATE
    G6_EH_GUEST_DIR="${G6_EH_GUEST_DIR}"
    G6_EH_GUEST_FH4=${G6_EH_GUEST_FH4})
add_dependencies(test_cxx_hooks ${eh_guests})

# A DLL with static TLS and no C runtime, in a folder of its own. The test copies it under other
# names into a subfolder at run time, for Windows to load before the loader starts, and maps the
# original as a guest module (docs/execution-model.md, "TLS and the TEB").
add_library(tls_order SHARED fixtures/tls_order/tls_order.c)
g6_target_defaults(tls_order)
set_target_properties(tls_order PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tls_order)
target_compile_options(tls_order PRIVATE /GS-)
target_link_options(tls_order PRIVATE /NODEFAULTLIB /ENTRY:g6_tls_order_entry /INCREMENTAL:NO)
target_link_libraries(tls_order PRIVATE kernel32)

g6_add_test(test_tls_order LABEL nogame
    SOURCES loader/test_tls_order.cpp
    LIBS g6loader)
target_include_directories(test_tls_order PRIVATE fixtures/tls_order)
target_compile_definitions(test_tls_order PRIVATE G6_TLS_ORDER_DLL="$<TARGET_FILE:tls_order>")
add_dependencies(test_tls_order tls_order)

# A guest executable built with /GA, whose code reads its thread-local data from slot 0 of the
# TLS array, and a test executable that lends it that slot by linking g6exetls
# (runtime/loader/support/exe_tls.c). Static C runtime for the guest's TLS directory only; the
# entry point is ours. A folder of its own serves as the loader's search path.
set(G6_EXE_TLS_DIR ${CMAKE_BINARY_DIR}/exe_tls)
add_executable(ga_guest fixtures/exe_tls/ga_guest.c)
g6_target_defaults(ga_guest)
set_target_properties(ga_guest PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreaded
    RUNTIME_OUTPUT_DIRECTORY ${G6_EXE_TLS_DIR})
target_compile_options(ga_guest PRIVATE /O2 /GA /GS-)
target_link_options(ga_guest PRIVATE /ENTRY:g6_ga_entry /SUBSYSTEM:CONSOLE /INCREMENTAL:NO)

g6_add_test(test_exe_tls LABEL nogame
    SOURCES loader/test_exe_tls.cpp
    LIBS g6loader g6exetls x64analysis)
target_include_directories(test_exe_tls PRIVATE fixtures/exe_tls)
target_compile_definitions(test_exe_tls PRIVATE
    G6_EXE_TLS_DIR="${G6_EXE_TLS_DIR}"
    G6_TLS_DONOR_CAPACITY=${G6_TLS_DONOR_CAPACITY})
add_dependencies(test_exe_tls ga_guest)

# ---- Import policy and HLE ----

g6_add_test(test_io LABEL nogame
    SOURCES io/test_paths.cpp io/test_file_system.cpp
    LIBS g6io)

g6_add_test(test_imports LABEL nogame
    SOURCES imports/test_command_line.cpp imports/test_default_policy.cpp imports/test_files.cpp
            imports/test_registry.cpp imports/test_xmem.cpp
    LIBS g6imports g6gdk runtimeobject)

# memory_tracker (runtime/gpu): placeholders, physical ranges, and aligned allocations.
g6_add_test(test_memory_tracker LABEL nogame
    SOURCES gpu/test_memory_tracker.cpp
    LIBS g6gpu)

g6_add_test(test_gdk LABEL nogame
    SOURCES gdk/test_task_queue.cpp gdk/test_async.cpp gdk/test_game_runtime.cpp gdk/test_user.cpp
            gdk/test_game_save.cpp
    LIBS g6gdk)

# The guest executable for test_crt_exit: original, and release /MD like the game's executable,
# so atexit, static destructors, and thread_local destructors reach the CRT's exit functions
# through its imports. onecore_apiset instead of kernel32, as in console code. A folder of its own
# serves as the loader's search path; the test copies the host's vcruntime140.dll into a
# subfolder at run time.
set(G6_EXIT_GUEST_DIR ${CMAKE_BINARY_DIR}/exit_guest)
add_executable(exit_guest fixtures/exit_guest/exit_guest.cpp)
g6_target_defaults(exit_guest)
set_target_properties(exit_guest PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreadedDLL
    RUNTIME_OUTPUT_DIRECTORY ${G6_EXIT_GUEST_DIR})
target_link_options(exit_guest PRIVATE
    /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /NODEFAULTLIB:kernel32.lib)
target_link_libraries(exit_guest PRIVATE onecore_apiset)

g6_add_test(test_crt_exit LABEL nogame
    SOURCES imports/test_crt_exit.cpp
    LIBS g6imports)
target_include_directories(test_crt_exit PRIVATE fixtures/exit_guest)
target_compile_definitions(test_crt_exit PRIVATE G6_EXIT_GUEST_DIR="${G6_EXIT_GUEST_DIR}")
add_dependencies(test_crt_exit exit_guest)

# The guest executable and DLL for test_crt_args: original, release /MD, and onecore_apiset, as
# for test_crt_exit, in a folder of their own.
set(G6_ARGS_GUEST_DIR ${CMAKE_BINARY_DIR}/args_guest)
add_library(args_guest_dll SHARED fixtures/args_guest/args_guest_dll.c)
add_executable(args_guest fixtures/args_guest/args_guest.c)
foreach(target args_guest_dll args_guest)
    g6_target_defaults(${target})
    set_target_properties(${target} PROPERTIES
        MSVC_RUNTIME_LIBRARY MultiThreadedDLL
        RUNTIME_OUTPUT_DIRECTORY ${G6_ARGS_GUEST_DIR})
    target_link_options(${target} PRIVATE /INCREMENTAL:NO /NODEFAULTLIB:kernel32.lib)
    target_link_libraries(${target} PRIVATE onecore_apiset)
endforeach()
target_link_options(args_guest PRIVATE /SUBSYSTEM:CONSOLE)
target_link_libraries(args_guest PRIVATE args_guest_dll)

g6_add_test(test_crt_args LABEL nogame
    SOURCES imports/test_crt_args.cpp
    LIBS g6imports)
target_include_directories(test_crt_args PRIVATE fixtures/args_guest)
target_compile_definitions(test_crt_args PRIVATE G6_ARGS_GUEST_DIR="${G6_ARGS_GUEST_DIR}")
add_dependencies(test_crt_args args_guest)

# ---- The runner ----

# The guest executable for test_runner: original and onecore_apiset, with the universal CRT as
# DLL imports, as the game has it. The game's C++ runtime is the console's, which imports API sets;
# the host's vcruntime140.dll imports kernel32.dll, which the game's import policy stubs, so the
# fixture links the C++ runtime statically instead (/MT with the UCRT's import library). The test
# copies it into a folder that stands in for an install.
set(G6_RUNNER_GUEST_DIR ${CMAKE_BINARY_DIR}/runner_guest)
add_executable(runner_guest fixtures/runner_guest/runner_guest.c)
g6_target_defaults(runner_guest)
set_target_properties(runner_guest PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreaded
    RUNTIME_OUTPUT_DIRECTORY ${G6_RUNNER_GUEST_DIR})
target_link_options(runner_guest PRIVATE /INCREMENTAL:NO /NODEFAULTLIB:kernel32.lib
    /NODEFAULTLIB:libucrt.lib /SUBSYSTEM:CONSOLE)
target_link_libraries(runner_guest PRIVATE ucrt onecore_apiset)

g6_add_test(test_runner LABEL nogame
    SOURCES app/test_runner.cpp
    LIBS g6runner)
target_include_directories(test_runner PRIVATE fixtures/runner_guest)
target_compile_definitions(test_runner PRIVATE
    G6_RUNNER_GUEST_DIR="${G6_RUNNER_GUEST_DIR}"
    G6_RUNNER_EXE="$<TARGET_FILE:g6recomp>")
add_dependencies(test_runner runner_guest g6recomp)

g6_add_test(test_game_files LABEL game
    SOURCES game/test_game_files.cpp
    LIBS x64recomp_core)
set_tests_properties(test_game_files PROPERTIES ENVIRONMENT "G6_GAME_DIR=${G6_GAME_DIR}")

g6_add_test(test_shader_census LABEL game
    SOURCES game/test_shader_census.cpp
    LIBS shaderrecomp_core)
set_tests_properties(test_shader_census PROPERTIES ENVIRONMENT "G6_GAME_DIR=${G6_GAME_DIR}")

# The C++ runtime fixture for test_side_dlls (docs/side-dlls.md, "Tests"): release /MD, so its
# runtime imports name the DLLs the console ships, in a folder of its own, so the loader finds
# those DLLs in the user's install and nowhere else. onecore_apiset instead of kernel32, so its
# system imports are API sets, as in console code. vcomp for the OpenMP entry points.
add_library(crt_guest SHARED fixtures/crt_guest/crt_guest.cpp)
g6_target_defaults(crt_guest)
set_target_properties(crt_guest PROPERTIES
    MSVC_RUNTIME_LIBRARY MultiThreadedDLL
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/crt_guest)
target_compile_definitions(crt_guest PRIVATE _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
# /EHc- because crt_guest_throw throws out of an extern "C" function.
target_compile_options(crt_guest PRIVATE /EHc-)
target_link_options(crt_guest PRIVATE /INCREMENTAL:NO /NODEFAULTLIB:kernel32.lib)
target_link_libraries(crt_guest PRIVATE onecore_apiset vcomp)

g6_add_test(test_side_dlls LABEL game
    SOURCES game/test_side_dlls.cpp
    LIBS g6imports)
target_include_directories(test_side_dlls PRIVATE fixtures/crt_guest)
target_compile_definitions(test_side_dlls PRIVATE G6_CRT_GUEST_DIR="${CMAKE_BINARY_DIR}/crt_guest")
set_tests_properties(test_side_dlls PROPERTIES ENVIRONMENT "G6_GAME_DIR=${G6_GAME_DIR}")
add_dependencies(test_side_dlls crt_guest)

# Native-versus-recompiled differential tests on an original test program.
add_subdirectory(diff)
