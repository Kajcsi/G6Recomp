# Checks the user's game files in G6_GAME_DIR against game/config/known_builds.toml.
# Sets G6_HAVE_GAME (ON/OFF) and G6_BUILD_ID in the caller's scope.
#
# known_builds.toml is read with a deliberately small parser. Each build is a [[build]] table of
# `key = "value"` lines, optionally followed by a [build.dll_sha256] table of
# `"file name" = "sha256"` lines. Keep the file in that shape.

function(_g6_sha256_cached path out_var)
    file(SIZE "${path}" size)
    file(TIMESTAMP "${path}" mtime "%Y%m%d%H%M%S" UTC)
    set(key "${path}|${size}|${mtime}")
    string(SHA256 key_id "${path}")
    string(SUBSTRING "${key_id}" 0 16 key_id)
    if(DEFINED CACHE{G6_HASHCACHE_${key_id}_KEY} AND "${G6_HASHCACHE_${key_id}_KEY}" STREQUAL "${key}")
        set(${out_var} "${G6_HASHCACHE_${key_id}_VALUE}" PARENT_SCOPE)
        return()
    endif()
    file(SHA256 "${path}" hash)
    set(G6_HASHCACHE_${key_id}_KEY "${key}" CACHE INTERNAL "")
    set(G6_HASHCACHE_${key_id}_VALUE "${hash}" CACHE INTERNAL "")
    set(${out_var} "${hash}" PARENT_SCOPE)
endfunction()

function(_g6_toml_string block key out_var)
    string(REGEX MATCH "\n[ \t]*${key}[ \t]*=[ \t]*\"([^\"]*)\"" _ "\n${block}")
    set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(g6_check_game_files)
    set(G6_HAVE_GAME OFF PARENT_SCOPE)
    set(G6_BUILD_ID "" PARENT_SCOPE)

    if(G6_NOGAME)
        message(STATUS "G6_NOGAME=ON: skipping the game file check; game-dependent targets are off")
        return()
    endif()

    set(how_to_fix
        "Put your decrypted install in private/game, point -DG6_GAME_DIR=<path> at it, "
        "or configure with -DG6_NOGAME=ON to build only the tools and the nogame tests.")
    if(NOT IS_DIRECTORY "${G6_GAME_DIR}")
        message(FATAL_ERROR "Game files not found: G6_GAME_DIR=${G6_GAME_DIR} does not exist.\n" ${how_to_fix})
    endif()

    file(READ "${CMAKE_SOURCE_DIR}/game/config/known_builds.toml" text)
    string(REGEX REPLACE "#[^\n]*" "" text "${text}")
    # CMake treats [ ] specially in lists, so swap them out before splitting on [[build]].
    string(REPLACE "[[build]]" "@G6_BUILD@" text "${text}")
    string(REPLACE "[" "<" text "${text}")
    string(REPLACE "]" ">" text "${text}")
    string(REPLACE ";" "," text "${text}")
    string(REPLACE "@G6_BUILD@" ";" blocks "${text}")

    set(exe_names "")
    set(problems "")
    foreach(block IN LISTS blocks)
        _g6_toml_string("${block}" "id" id)
        if(id STREQUAL "")
            continue()
        endif()
        _g6_toml_string("${block}" "exe_name" exe_name)
        _g6_toml_string("${block}" "exe_sha256" want)
        list(APPEND exe_names "${exe_name}")

        set(exe "${G6_GAME_DIR}/${exe_name}")
        if(NOT EXISTS "${exe}")
            continue()
        endif()
        _g6_sha256_cached("${exe}" got)
        if(want STREQUAL "")
            list(APPEND problems "${exe_name} is present (SHA-256 ${got}) but build ${id} has no "
                "exe_sha256 recorded. If this is your copy of ${id}, record it in known_builds.toml.")
            continue()
        endif()
        if(NOT got STREQUAL want)
            list(APPEND problems "${exe_name} has SHA-256 ${got}, which is not build ${id}.")
            continue()
        endif()

        # The executable matches. Check the side DLLs from the same package.
        string(FIND "${block}" "<build.dll_sha256>" pos)
        if(pos GREATER -1)
            string(SUBSTRING "${block}" ${pos} -1 dll_table)
            string(REGEX MATCHALL "\"[^\"]+\"[ \t]*=[ \t]*\"[0-9a-fA-F]+\"" pairs "${dll_table}")
            set(mismatched "")
            foreach(pair IN LISTS pairs)
                string(REGEX REPLACE "^\"([^\"]+)\".*" "\\1" dll "${pair}")
                string(REGEX REPLACE ".*\"([0-9a-fA-F]+)\"$" "\\1" dll_want "${pair}")
                if(NOT EXISTS "${G6_GAME_DIR}/${dll}")
                    list(APPEND mismatched "${dll} (missing)")
                    continue()
                endif()
                _g6_sha256_cached("${G6_GAME_DIR}/${dll}" dll_got)
                if(NOT dll_got STREQUAL dll_want)
                    list(APPEND mismatched "${dll} (hash differs)")
                endif()
            endforeach()
            if(mismatched)
                list(JOIN mismatched ", " mismatched)
                message(FATAL_ERROR "The executable matches build ${id}, but these files from the same "
                    "package don't: ${mismatched}. The install in ${G6_GAME_DIR} is mixed or modified.")
            endif()
        endif()

        message(STATUS "Game files verified: build ${id} in ${G6_GAME_DIR}")
        set(G6_HAVE_GAME ON PARENT_SCOPE)
        set(G6_BUILD_ID "${id}" PARENT_SCOPE)
        return()
    endforeach()

    if(problems)
        list(JOIN problems "\n  " detail)
        message(FATAL_ERROR "No supported game build in ${G6_GAME_DIR}:\n  ${detail}\n" ${how_to_fix})
    endif()
    list(REMOVE_DUPLICATES exe_names)
    list(JOIN exe_names ", " exe_names)
    message(FATAL_ERROR "No main executable in ${G6_GAME_DIR} (expected one of: ${exe_names}).\n" ${how_to_fix})
endfunction()
