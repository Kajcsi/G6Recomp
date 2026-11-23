# g6loader: the native loader (docs/execution-model.md, "Loader"). It maps guest modules itself,
# binds their imports by policy, registers their exception tables, gives them static TLS, and
# initializes them, so their code runs directly on the host CPU.

set(G6_TLS_DONOR_COUNT 4)
set(G6_TLS_DONOR_CAPACITY 0x10000 CACHE STRING
    "Bytes of static TLS each donor DLL lends one guest module")

# A TLS donor: a support DLL that Windows loads normally on the loader's behalf. No C runtime and
# its own entry point, so its DllMain does almost nothing while Windows holds the loader lock.
function(g6_support_dll target source entry)
    add_library(${target} SHARED ${source})
    g6_target_defaults(${target})
    target_include_directories(${target} PRIVATE support)
    target_compile_options(${target} PRIVATE /O2 /GS- /Zl)
    target_link_options(${target} PRIVATE /NODEFAULTLIB /ENTRY:${entry} /INCREMENTAL:NO)
    target_link_libraries(${target} PRIVATE kernel32)
endfunction()

set(donors)
math(EXPR last_donor "${G6_TLS_DONOR_COUNT} - 1")
foreach(i RANGE ${last_donor})
    g6_support_dll(g6tls${i} support/tls_donor.c g6_tls_donor_entry)
    target_compile_definitions(g6tls${i} PRIVATE G6_TLS_DONOR_CAPACITY=${G6_TLS_DONOR_CAPACITY})
    # The TLS directory is only referenced by name from the linker, so keep it from being
    # discarded as unused.
    target_link_options(g6tls${i} PRIVATE /INCLUDE:_tls_used)
    list(APPEND donors g6tls${i})
endforeach()

# What an executable that hosts a guest executable links in place of the C runtime's TLS support,
# so the guest can have TLS index 0 (support/exe_tls.c). Object files rather than a static
# library: nothing refers to them by name, so a library member would be left out.
add_library(g6exetls OBJECT support/exe_tls.c)
g6_target_defaults(g6exetls)
target_compile_definitions(g6exetls PRIVATE G6_TLS_DONOR_CAPACITY=${G6_TLS_DONOR_CAPACITY})
target_link_options(g6exetls INTERFACE /INCLUDE:_tls_used /INCLUDE:g6_exe_tls_callbacks_end)

add_library(g6loader STATIC
    src/crash_report.cpp
    src/hle_loader.cpp
    src/import_registry.cpp
    src/loader.cpp
    src/patcher.cpp
    src/stubs.cpp
    src/thread_support.cpp
    # The thread notifier: a TLS callback that ends up in the host module. thread_support.cpp
    # refers to it, so every host of the loader links it.
    support/thread_notifier.c)
target_include_directories(g6loader PUBLIC include PRIVATE src support)
target_link_libraries(g6loader PUBLIC g6kernel g6common)
# The hook engine checks patch windows with the analysis library and answers trapped cpuid with
# g6cpu, both privately: g6cpu builds with AVX2 enabled, which mustn't spread to users.
target_link_libraries(g6loader PRIVATE x64analysis g6cpu)
target_compile_definitions(g6loader PRIVATE
    G6_TLS_DONOR_COUNT=${G6_TLS_DONOR_COUNT}
    G6_TLS_DONOR_CAPACITY=${G6_TLS_DONOR_CAPACITY})
g6_target_defaults(g6loader)
# The loader loads these from its own folder at run time.
add_dependencies(g6loader ${donors})
