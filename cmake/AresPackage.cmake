execute_process(COMMAND ares-package "${CPACK_TEMPORARY_DIRECTORY}" -o "${CPACK_PACKAGE_DIRECTORY}"
        -e include
        -e cmake
        -e "lib/static"
        -e "lib/pkgconfig"
        COMMAND_ERROR_IS_FATAL ANY
)

# The SDK's npm tar strips sticky bits and emits world-writable code directories.
# Correct only this application's directory headers, preserving payload bytes.
execute_process(COMMAND python3 "${CMAKE_CURRENT_LIST_DIR}/../scripts/webos/normalize_ipk_dirs.py"
        "${CPACK_PACKAGE_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}.ipk" "${CPACK_PACKAGE_NAME}"
        COMMAND_ERROR_IS_FATAL ANY)

find_program(GEN_MANIFEST webosbrew-gen-manifest)
if (NOT GEN_MANIFEST)
    message(STATUS "Manifest generator not found, skipping manifest generation")
    return()
endif ()

execute_process(COMMAND ${GEN_MANIFEST} -p "${CPACK_PACKAGE_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}.ipk"
        -o "${CPACK_PACKAGE_DIRECTORY}/${CPACK_PACKAGE_NAME}.manifest.json"
        -i "https://raw.githubusercontent.com/PreceptorOfMagic/Eclipse/main/deploy/webos/icon.png"
        -l "https://github.com/PreceptorOfMagic/Eclipse"
        COMMAND_ERROR_IS_FATAL ANY
)
