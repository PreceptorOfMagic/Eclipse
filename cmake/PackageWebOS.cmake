find_program(AWK NAMES gawk awk REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Copy manifest
configure_file(deploy/webos/appinfo.json ./appinfo.json @ONLY)

# Copy all files under deploy/webos/ to package root
install(DIRECTORY deploy/webos/ DESTINATION . USE_SOURCE_PERMISSIONS PATTERN ".*" EXCLUDE PATTERN "*.in" EXCLUDE
        PATTERN "appinfo.json" EXCLUDE)
install(FILES "${CMAKE_BINARY_DIR}/appinfo.json" DESTINATION .)

# Existing read-only roots retain their modes across TV upgrades. Create only a
# writable container; the native app owns its conf/cache children. Never package
# a default conf directory or user files. AresPackage normalizes final tar modes.
install(DIRECTORY DESTINATION userdata DIRECTORY_PERMISSIONS
        OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_WRITE GROUP_EXECUTE
        WORLD_READ WORLD_WRITE WORLD_EXECUTE)

# Keep the project's own licence, derivative declaration and attribution next
# to appinfo.json inside the installed application. This is the minimum legal
# context for an offline IPK; artifact-specific third-party notices and the
# immutable corresponding-source reference remain separate release gates.
install(FILES
        LICENSE.txt
        COPYRIGHT
        ACKNOWLEDGEMENTS.md
        DESTINATION .)

# Ship the notices for code and data copied into the IPK. Keep the source
# filenames explicit so a missing submodule notice fails packaging instead of
# silently producing an incomplete legal bundle.
install(FILES docs/licenses/webos/NOTICE.md
        DESTINATION licenses)
install(FILES core/moonlight-common-c/enet/LICENSE
        DESTINATION licenses RENAME ENet-MIT.txt)
install(FILES third_party/commons/LICENSE
        DESTINATION licenses RENAME commons-c-MIT.txt)
install(FILES third_party/lvgl/LICENCE.txt
        DESTINATION licenses RENAME LVGL-MIT.txt)
install(FILES third_party/ss4s/LICENSE
        DESTINATION licenses RENAME SS4S-LGPL-3.0.txt)
install(FILES third_party/libmicrodns/COPYING
        DESTINATION licenses RENAME libmicrodns-LGPL-2.1.txt)
install(FILES third_party/libmicrodns/AUTHORS
        DESTINATION licenses RENAME libmicrodns-AUTHORS.txt)
install(FILES
        docs/licenses/common/Material-Icons-Apache-2.0.txt
        docs/licenses/common/Font-Awesome-Free-5.9.0-LICENSE.txt
        docs/licenses/common/LVGL-Embedded-Fonts-NOTICE.md
        docs/licenses/common/Montserrat-7.200-OFL-1.1.txt
        docs/licenses/common/SIL-OFL-1.1.txt
        docs/licenses/common/Unscii-8-1.0-PROVENANCE.txt
        docs/licenses/webos/libmicrodns-compat-ISC-style.txt
        docs/licenses/webos/Moonlight-Common-C-Reed-Solomon-BSD.txt
        docs/licenses/webos/SDL2-THIRD-PARTY-NOTICES.txt
        DESTINATION licenses)

# The webOS SDL backport is always an ExternalProject. The remaining fallback
# libraries are present only when the SDK did not supply a suitable version.
# Retrieve and consume SOURCE_DIR once per target: CMake overwrites the same
# variable on every ExternalProject_Get_Property() call.
ExternalProject_Get_Property(ext_sdl2_backport SOURCE_DIR)
if (DEFINED SDL2_BACKPORT_RELEASE)
    set(ECLIPSE_SDL_LICENSE "${SOURCE_DIR}/share/licenses/SDL2/LICENSE.txt")
else ()
    set(ECLIPSE_SDL_LICENSE "${SOURCE_DIR}/LICENSE.txt")
endif ()
install(FILES "${ECLIPSE_SDL_LICENSE}"
        DESTINATION licenses RENAME SDL2-zlib.txt)
# The pinned SDL_GameControllerDB revision carries this byte-identical zlib
# licence. Install the verified upstream bytes a second time under the data
# asset's own name so attribution remains explicit in the notice index.
install(FILES docs/licenses/webos/SDL_GameControllerDB-zlib.txt
        DESTINATION licenses)

if (TARGET ext_inih)
    ExternalProject_Get_Property(ext_inih SOURCE_DIR)
    install(FILES "${SOURCE_DIR}/LICENSE.txt"
            DESTINATION licenses RENAME inih-BSD-3-Clause.txt)
endif ()

if (TARGET ext_mbedtls)
    ExternalProject_Get_Property(ext_mbedtls SOURCE_DIR)
    install(FILES "${SOURCE_DIR}/LICENSE"
            DESTINATION licenses RENAME Mbed-TLS-Apache-2.0.txt)
endif ()

if (TARGET ext_opus)
    ExternalProject_Get_Property(ext_opus SOURCE_DIR)
    install(FILES "${SOURCE_DIR}/COPYING"
            DESTINATION licenses RENAME Opus-COPYING.txt)
    install(FILES "${SOURCE_DIR}/AUTHORS"
            DESTINATION licenses RENAME Opus-AUTHORS.txt)
endif ()

# Generate translations
foreach (I18N_LOCALE ${I18N_LOCALES})
    string(REPLACE "-" "/" I18N_JSON_DIR "resources/${I18N_LOCALE}")
    install(CODE "file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/${I18N_JSON_DIR}\")"
            CODE "execute_process(COMMAND ${AWK} -f scripts/webos/po2json.awk src/i18n/${I18N_LOCALE}/messages.po
                OUTPUT_FILE \"\${CMAKE_INSTALL_PREFIX}/${I18N_JSON_DIR}/cstrings.json\"
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND_ERROR_IS_FATAL ANY)")
endforeach ()

# Generation gamepad mapping
install(CODE "file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/assets\")"
        CODE "execute_process(COMMAND scripts/webos/gen_gamecontrollerdb.sh
            OUTPUT_FILE \"\${CMAKE_INSTALL_PREFIX}/assets/gamecontrollerdb.txt\"
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND_ERROR_IS_FATAL ANY)")

# Fake library for cURL ABI issue
add_dependencies(moonlight commons-curl-abi-fix)
install(TARGETS commons-curl-abi-fix LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} NAMELINK_SKIP)

set(CPACK_PACKAGE_NAME "${WEBOS_APPINFO_ID}")
set(CPACK_GENERATOR "External")
set(CPACK_EXTERNAL_PACKAGE_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/AresPackage.cmake")
set(CPACK_EXTERNAL_ENABLE_STAGING TRUE)
set(CPACK_MONOLITHIC_INSTALL TRUE)
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}_${PROJECT_VERSION}_${TARGET_WEBOS_ARCH}")
set(CPACK_PRE_BUILD_SCRIPTS "${CMAKE_SOURCE_DIR}/cmake/CleanupNameLink.cmake")

configure_file("${CMAKE_SOURCE_DIR}/cmake/CPackConfig.webOS.cmake.in" CPackConfig.webOS.cmake @ONLY)
set(CPACK_PROJECT_CONFIG_FILE "${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.webOS.cmake")

if (CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    list(APPEND CPACK_PRE_BUILD_SCRIPTS "${CMAKE_SOURCE_DIR}/cmake/ArchiveDebugSymbols.cmake")
endif ()

# Metadata must hash the final payload, after cleanup and optional symbol stripping.
configure_file(cmake/PackageWebOSMetadata.cmake.in PackageWebOSMetadata.cmake @ONLY)
list(APPEND CPACK_PRE_BUILD_SCRIPTS "${CMAKE_CURRENT_BINARY_DIR}/PackageWebOSMetadata.cmake")

# Will use all cores on CMake 3.20+
set(CPACK_THREADS 0)

set(CPACK_STRIP_FILES FALSE)

add_custom_target(webos-package-aurora COMMAND cpack DEPENDS moonlight)
add_dependencies(webos-package-aurora ext_sdl2_backport)
if (TARGET ext_inih)
    add_dependencies(webos-package-aurora ext_inih)
endif ()
if (TARGET ext_mbedtls)
    add_dependencies(webos-package-aurora ext_mbedtls)
endif ()
if (TARGET ext_opus)
    add_dependencies(webos-package-aurora ext_opus)
endif ()

if (NOT ENV{CI})
    add_custom_target(webos-verify-aurora COMMAND webosbrew-ipk-verify -S -d "${CPACK_PACKAGE_FILE_NAME}.ipk"
            WORKING_DIRECTORY ${CPACK_PACKAGE_DIRECTORY}
            DEPENDS webos-package-aurora)
    if (ENV{ARES_DEVICE})
        set(ares_arguments "-d" $ENV{ARES_DEVICE})
    endif ()
    add_custom_target(webos-install-aurora COMMAND ares-install "${CPACK_PACKAGE_FILE_NAME}.ipk" ${ares_arguments}
            WORKING_DIRECTORY ${CPACK_PACKAGE_DIRECTORY}
            DEPENDS webos-package-aurora)
    add_custom_target(webos-launch-aurora COMMAND ares-launch "${WEBOS_APPINFO_ID}" ${ares_arguments}
            DEPENDS webos-install-aurora)
endif ()

include(CPack)
