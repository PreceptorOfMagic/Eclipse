get_property(_ENABLED_LANGUAGES GLOBAL PROPERTY ENABLED_LANGUAGES)

foreach (_LANG ${_ENABLED_LANGUAGES})
    # GNU windres treats `-v` as verbose compilation and waits for resource input
    # on stdin. It has no compiler target for the platform-selection logic below,
    # so probing it here hangs Windows configuration indefinitely.
    if (_LANG STREQUAL "RC")
        continue()
    endif ()
    if (DEFINED CMAKE_${_LANG}_COMPILER_TARGET)
        continue()
    endif ()
    execute_process(COMMAND "${CMAKE_${_LANG}_COMPILER}" "-v" ERROR_VARIABLE _VERSION_INFO)
    string(REGEX REPLACE ".*Target: ([^\n]+).*" "\\1" CMAKE_${_LANG}_COMPILER_TARGET "${_VERSION_INFO}")
endforeach ()

unset(_VERSION_INFO)
unset(_ENABLED_LANGUAGES)
