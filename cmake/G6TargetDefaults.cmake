# Compiler settings shared by every first-party target. Third-party code keeps its own flags.
function(g6_target_defaults target)
    target_compile_options(${target} PRIVATE
        /W4
        /utf-8
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # Zydis and Win32 headers use designated sizes and casts that -Wextra flags noisily.
        target_compile_options(${target} PRIVATE -Wno-missing-field-initializers)
    endif()
    target_compile_definitions(${target} PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE)
endfunction()
