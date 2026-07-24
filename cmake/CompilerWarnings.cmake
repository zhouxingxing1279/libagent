# Apply consistent warning flags to a target. Honors LIBAGENT_WARNINGS_AS_ERRORS.
function(libagent_target_warnings target)
    set(clang_gcc_flags
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wno-unknown-pragmas
        # Partial aggregate init is idiomatic for value types like Message
        # (role+content is the common case).
        -Wno-missing-field-initializers)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${target} PRIVATE ${clang_gcc_flags})
        if(LIBAGENT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Wv-)
        if(LIBAGENT_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    endif()
endfunction()
