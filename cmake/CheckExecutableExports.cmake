# Post-link guard: the executable must not export symbols.
#
# GNU ld exports any executable global that a linked shared library references, and an
# exported definition in the executable comes first in symbol lookup for every library that
# reaches that name through its PLT. That is how a CTM log buffer named `g_log` became
# libgstreamer's g_log() on webOS 26 and stopped every stream start (docs/webos26-media-load.md).
# It sat unnoticed for about two months: the GStreamer warning that reached it was printed only
# on the first media load after the firmware update (measured on 11.2.0).
# Failing the build is the only point at which the next one is cheap to find.
#
# Usage: cmake -DNM=<nm> -DBIN=<executable> -DALLOW=<comma-separated names> -P <this file>
if (NOT NM OR NOT BIN)
    message(FATAL_ERROR "CheckExecutableExports: NM and BIN are required")
endif ()
string(REPLACE "," ";" _allow "${ALLOW}")
execute_process(COMMAND "${NM}" -D --defined-only "${BIN}"
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
if (NOT _rc EQUAL 0)
    message(FATAL_ERROR "CheckExecutableExports: ${NM} failed on ${BIN}: ${_err}")
endif ()
string(REPLACE "\n" ";" _lines "${_out}")
set(_bad)
foreach (_line IN LISTS _lines)
    if (_line MATCHES "^[0-9a-fA-F]+ ([A-Za-z]) (.+)$")
        if (NOT CMAKE_MATCH_2 IN_LIST _allow)
            list(APPEND _bad "${CMAKE_MATCH_1} ${CMAKE_MATCH_2}")
        endif ()
    endif ()
endforeach ()
if (_bad)
    list(JOIN _bad "\n    " _bad_text)
    message(FATAL_ERROR "${BIN} exports symbols that would override same-named symbols in the "
            "TV's own libraries:\n    ${_bad_text}\nRename them, or make them static.")
endif ()
message(STATUS "Export guard passed: ${BIN} exports only allowlisted symbols")
