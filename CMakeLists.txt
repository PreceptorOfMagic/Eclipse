cmake_minimum_required(VERSION 3.19)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Keep configuration deterministic and offline. The previous tag/API lookup was
# discarded immediately by this same version, but still made an untagged source
# checkout depend on git, curl and GitHub being available.
project(moonlight VERSION 1.3.4 LANGUAGES C CXX)

# To suppress warnings for MbedTLS
if (POLICY CMP0048)
    cmake_policy(SET CMP0048 NEW)
endif ()
# To suppress warnings for ExternalProject DOWNLOAD_EXTRACT_TIMESTAMP
if (POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif ()

# Add folder where are supportive functions
set(CMAKE_MODULE_PATH
        ${CMAKE_SOURCE_DIR}/cmake
        ${CMAKE_SOURCE_DIR}/third_party/commons/cmake
        ${CMAKE_SOURCE_DIR}/core/moonlight-common-c/cmake
        )
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GetCompilerTarget)

get_filename_component(CC_BASENAME ${CMAKE_C_COMPILER} NAME)

# Docker / easy_build: toolchain path is set before the compiler basename is visible.
if (NOT TARGET_WEBOS AND CMAKE_TOOLCHAIN_FILE MATCHES "webos|buildroot")
    set(TARGET_WEBOS ON)
    set(TARGET_WEBOS_ARCH "arm")
endif ()

if (CMAKE_C_COMPILER_TARGET STREQUAL "arm-webos-linux-gnueabi")
    set(TARGET_WEBOS ON)
    set(TARGET_WEBOS_ARCH "arm")
elseif(CMAKE_C_COMPILER_TARGET MATCHES "(i[3-6]86)-webos-linux-gnu")
    set(TARGET_WEBOS ON)
    set(TARGET_WEBOS_ARCH ${CMAKE_MATCH_1})
elseif (CMAKE_C_COMPILER_TARGET STREQUAL "armv7a-cros-linux-gnueabi")
    set(TARGET_STEAMLINK ON)
endif ()

# webOS: CMAKE_C_COMPILER_TARGET may be unset; detect cross gcc by basename (see third_party/ss4s).
if (NOT TARGET_WEBOS AND NOT TARGET_STEAMLINK AND CC_BASENAME MATCHES "^arm-webos-linux-gnueabi-(gcc|g\\+\\+)$")
    set(TARGET_WEBOS ON)
    set(TARGET_WEBOS_ARCH "arm")
endif ()

option(USE_MBEDTLS "" ON)
option(USE_WERROR "" OFF)

option(BUILD_EXAMPLES "" OFF)

option(COMMONS_LV_SDL_IMG_USE_IMAGE "Use SDL2_image API" ON)

set(COMMONS_LOGGING_SDL ON)
set(COMMONS_LOGGING_LVGL ON)
set(COMMONS_LOGGING_SS4S ON)

if (TARGET_WEBOS OR TARGET_STEAMLINK)
    option(BUILD_TESTS "" OFF)
else ()
    option(BUILD_TESTS "" ON)
endif ()

if (BUILD_TESTS)
    enable_testing()
endif ()

set(I18N_LOCALES
        "cs" "de" "es-ES" "fr" "it" "ja" "ko" "nl" "pl" "pt-BR"
        "ro" "ru" "tr" "uk" "zh-CN" "zh-TW")
list(LENGTH I18N_LOCALES I18N_LOCALES_LEN)

set(FEATURE_FORCE_FULLSCREEN OFF)
set(FEATURE_I18N_GETTEXT ON)
set(FEATURE_I18N_LANGUAGE_SETTINGS OFF)
set(FEATURE_SURROUND_SOUND ON)
set(FEATURE_CHECK_MODULE_OS_VERSION OFF)
set(FEATURE_INPUT_EVMOUSE OFF)
set(FEATURE_INPUT_LIBCEC ON)
set(FEATURE_EMBEDDED_SHELL OFF)
set(FEATURE_WINDOW_FULLSCREEN_DESKTOP ON)

include(LintOptions)

if (TARGET_WEBOS)
    # Cross-build: FindThreads try_compile spawns a sub-CMake without the toolchain compiler.
    set(THREADS_PTHREAD_ARG "2" CACHE STRING "Result from TRY_RUN" FORCE)
    set(CMAKE_THREAD_LIBS_INIT "-lpthread" CACHE INTERNAL "webOS pthread")
    set(OS_LINUX TRUE)

    # TWO INSTALLS SIDE BY SIDE ON ONE TV (2026-09-07). The beta (VEGA/AVC co-op) and the live
    # build must be installable together, so the app id is overridable from the command line:
    #     cmake -DWEBOS_APPINFO_ID=com.aurora.gamestream.beta -DWEBOS_APPINFO_TITLE="Eclipse Beta"
    # webOS keys EVERYTHING off the id -- install tree, $HOME, and therefore conf/ -- so a
    # different id gets its own settings, hosts.ini and pairing automatically, with no migration
    # and no risk to the live install's config. `APPID` (below) carries it into the C code; do
    # NOT reintroduce a hardcoded "com.aurora.gamestream" anywhere, or the beta will act on the
    # live app's behalf.
    if (NOT WEBOS_APPINFO_ID)
        set(WEBOS_APPINFO_ID "com.aurora.gamestream")
    endif ()
    # DERIVE the title from the id rather than take it from the command line: "Eclipse Beta"
    # contains a space, and it was silently word-split crossing the shell into cmake, producing a
    # beta package titled "Eclipse" -- two identically-named tiles on the TV launcher, which is
    # exactly the confusion a separate beta install exists to prevent.
    if (WEBOS_APPINFO_ID MATCHES "\\.beta$")
        set(WEBOS_APPINFO_TITLE "Eclipse Beta")
    else ()
        set(WEBOS_APPINFO_TITLE "Eclipse")
    endif ()
    message(STATUS "webOS app id: ${WEBOS_APPINFO_ID}  title: ${WEBOS_APPINFO_TITLE}")
    set(CMAKE_INSTALL_RPATH "$ORIGIN/lib/backports:$ORIGIN/../lib/backports:$ORIGIN")
elseif (TARGET_STEAMLINK)
    set(OS_LINUX TRUE)
    set(CMAKE_INSTALL_PREFIX moonlight-tv)
    set(CMAKE_INSTALL_BINDIR moonlight-tv/bin)
    set(CMAKE_INSTALL_LIBDIR moonlight-tv/lib)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(OS_LINUX TRUE)

    set(CMAKE_INSTALL_PREFIX /usr)
    include(GNUInstallDirs)
    set(CMAKE_INSTALL_FULL_LIBDIR ${CMAKE_INSTALL_FULL_LIBDIR}/moonlight-tv)
    set(CMAKE_INSTALL_LIBDIR ${CMAKE_INSTALL_FULL_LIBDIR})
    set(CMAKE_INSTALL_RPATH ${CMAKE_INSTALL_FULL_LIBDIR})
else ()
    if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(OS_DARWIN TRUE)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(OS_WINDOWS TRUE)
    else ()
        message(FATAL_ERROR "Unknown build platform")
    endif ()

    set(CMAKE_INSTALL_LIBDIR lib)
    set(CMAKE_INSTALL_BINDIR bin)
    set(CMAKE_INSTALL_DATAROOTDIR share)
endif ()

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
if (CMAKE_CROSSCOMPILING AND NOT DEFINED THREADS_PTHREAD_ARG)
    set(THREADS_PTHREAD_ARG "2" CACHE STRING "Cross-compile: skip pthread try_compile" FORCE)
endif ()
find_package(Threads REQUIRED)

if (WIN32)
    # WINSOCK FOR EVERY TARGET. htonl/ntohs/inet_pton and friends are in ws2_32 here, not
    # libc, and they are used across the app, libgamestream AND third_party/commons -- which
    # is a submodule and not ours to patch. Declaring it once, before the subdirectories are
    # added, fixes the whole class instead of chasing each target's link line.
    link_libraries(ws2_32)
endif ()

find_package(PkgConfig REQUIRED)

find_package(EXPAT REQUIRED)
find_package(Fontconfig REQUIRED)
find_package(Freetype REQUIRED)
find_package(CURL REQUIRED)

set(BUILD_SHARED_CORE_LIBS OFF)

if (TARGET_WEBOS)
    set(CMAKE_INSTALL_LIBDIR lib/backports)
    # Build the audited release source; do not download an unverified runtime binary.
    unset(SDL2_BACKPORT_RELEASE)
    unset(SDL2_BACKPORT_RELEASE CACHE)
    set(SDL2_BACKPORT_REVISION "9f30a1f01e4d36aa5d3e4e04df5921b7a9d093ee")
    include(ExternalSDL2BackportForWebOS)
    unset(CMAKE_INSTALL_LIBDIR)
else ()
    pkg_check_modules(SDL2 REQUIRED sdl2>=2.0.4)
endif()
pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)

find_package(MbedTLS)
if (NOT MBEDTLS_FOUND)
    list(APPEND CMAKE_MESSAGE_INDENT "  ")
    message(STATUS "Including mbedtls from source...")
    include(ExternalMbedTLS)
    list(POP_BACK CMAKE_MESSAGE_INDENT)
endif ()

pkg_check_modules(OPUS opus)
if (NOT OPUS_FOUND)
    list(APPEND CMAKE_MESSAGE_INDENT "  ")
    message(STATUS "Including opus from source...")
    include(ExternalOPUS)
    list(POP_BACK CMAKE_MESSAGE_INDENT)
endif ()

pkg_check_modules(INIH inih)
if (NOT INIH_FOUND)
    list(APPEND CMAKE_MESSAGE_INDENT "  ")
    message(STATUS "Including inih from source...")
    include(ExternalINIH)
    list(POP_BACK CMAKE_MESSAGE_INDENT)
endif ()

if (TARGET_WEBOS)
    pkg_check_modules(PBNJSON_C REQUIRED pbnjson_c)
    pkg_check_modules(PMLOG REQUIRED PmLogLib)
    pkg_check_modules(WEBOSI18N REQUIRED webosi18n)
elseif (OS_WINDOWS)
    # The application and both core libraries are one portable executable on
    # Windows. This is also the layout used by upstream's Windows build fix.
    set(BUILD_SHARED_CORE_LIBS OFF)
else ()
    set(BUILD_SHARED_CORE_LIBS ON)
endif ()

set(BUILD_SHARED_LIBS ${BUILD_SHARED_CORE_LIBS})
if (TARGET_WEBOS)
    # ENet is already linked into moonlight-common-c. Do not also install its
    # static archive and public headers into the consumer IPK.
    set(ENET_NO_INSTALL ON)
endif ()
add_subdirectory(core/moonlight-common-c)
add_subdirectory(core/libgamestream)
if (BUILD_SHARED_LIBS)
    install(TARGETS moonlight-common-c
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()
unset(BUILD_SHARED_LIBS)

if (TARGET_WEBOS)
    set(SS4S_MODULE_DISABLE_MMAL ON)
endif ()
option(SS4S_MODULE_BUILD_DUMMY "Enable SS4S dummy module" OFF)

add_subdirectory(third_party/ss4s)

# Desktop packages must contain a decoder that works immediately after download.
# The optional external `moonlight` wrapper remains available on Linux, but it is
# a fallback for an explicitly installed companion rather than a hidden runtime
# prerequisite. Keep this check here, after SS4S has discovered its modules, so
# Windows and a future Linux package share the same release gate.
if ((OS_WINDOWS OR (OS_LINUX AND NOT TARGET_WEBOS AND NOT TARGET_STEAMLINK))
        AND NOT TARGET ss4s-module-ffmpeg)
    message(FATAL_ERROR
            "Desktop builds require the native FFmpeg video module. Install the "
            "libavcodec/libavutil development packages; an external moonlight "
            "executable is not an acceptable implicit decoder dependency.")
endif ()

add_subdirectory(third_party/lvgl EXCLUDE_FROM_ALL)
target_include_directories(lvgl PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lvgl/src)
target_include_directories(lvgl PRIVATE ${CMAKE_SOURCE_DIR}/src/app/util)
target_include_directories(lvgl SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
target_include_directories(lvgl SYSTEM PRIVATE ${FREETYPE_INCLUDE_DIRS})
target_compile_definitions(lvgl PUBLIC LV_CONF_PATH=../../../src/app/lvgl/lv_conf.h)
target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES} ${FREETYPE_LIBRARIES})

add_subdirectory(third_party/commons)
if (OS_WINDOWS)
    # commons selects os_info_generic.c when uname is absent, which is true on
    # Windows. Mark that fallback non-compiling in the target's directory scope
    # and give commons the Win32 implementation directly, so there is exactly
    # one os_info_get owner instead of depending on static-archive extraction.
    set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/commons/os/info/os_info_generic.c"
            TARGET_DIRECTORY commons-os-info
            PROPERTIES HEADER_FILE_ONLY TRUE)
    target_sources(commons-os-info PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/app/platform/windows/os_info_windows.c")
endif ()

target_include_directories(lv_gridview SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
target_include_directories(lv_sdl_img SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS} ${SDL2_IMAGE_INCLUDE_DIRS})

add_executable(moonlight src/main.c)
if (OS_WINDOWS)
    # Produce a native GUI application rather than opening a console window next
    # to SDL. The resource also gives Explorer stable product/version metadata,
    # the project icon, DPI awareness and long-path support.
    enable_language(RC)
    configure_file(
            src/app/platform/windows/eclipse.manifest.in
            ${CMAKE_CURRENT_BINARY_DIR}/eclipse.manifest
            @ONLY)
    configure_file(
            src/app/platform/windows/eclipse.rc.in
            ${CMAKE_CURRENT_BINARY_DIR}/eclipse.rc
            @ONLY)
    target_sources(moonlight PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/eclipse.rc)
    set_target_properties(moonlight PROPERTIES WIN32_EXECUTABLE TRUE)
endif ()
add_library(moonlight-lib STATIC)
target_link_libraries(moonlight PRIVATE moonlight-lib)

target_compile_lint(moonlight-lib)

set_target_properties(moonlight-lib PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED TRUE
        )

target_compile_definitions(moonlight-lib PUBLIC "$<$<CONFIG:DEBUG>:DEBUG>")
# PI CONTROLLER HUB (src/app/stream/input/coop_pi_pads.c). OFF for every release: the hub is this
# project's own Raspberry Pi rig, not something Eclipse ships, so a release build never broadcasts
# the hub query from the co-op screens. Rig builds turn it on.
option(ECLIPSE_PI_HUB "Query the project's Raspberry Pi controller hub from the co-op screens (rig builds only)" OFF)
if (ECLIPSE_PI_HUB)
    target_compile_definitions(moonlight-lib PRIVATE ECLIPSE_PI_HUB=1)
endif ()
string(LENGTH ${CMAKE_SOURCE_DIR} SOURCE_DIR_LENGTH)
target_compile_definitions(moonlight-lib PUBLIC SOURCE_DIR_LENGTH=${SOURCE_DIR_LENGTH})
target_compile_definitions(moonlight-lib PUBLIC APP_VERSION="${PROJECT_VERSION}" APPLOG_HOST=1)
# WHICH BUILD A LOG CAME FROM (util/diag_report.c). The version above is not bumped per build, so support logs also
# carry the commit (and the running binary's SHA-256, computed at run time). "-dirty" = uncommitted changes.
execute_process(COMMAND git rev-parse --short=8 HEAD WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE ECLIPSE_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _eclipse_git_rc)
if (NOT _eclipse_git_rc EQUAL 0 OR "${ECLIPSE_BUILD_COMMIT}" STREQUAL "")
    set(ECLIPSE_BUILD_COMMIT "unknown")
else ()
    execute_process(COMMAND git status --porcelain --untracked-files=no WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE _eclipse_git_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (NOT "${_eclipse_git_dirty}" STREQUAL "")
        string(APPEND ECLIPSE_BUILD_COMMIT "-dirty")
    endif ()
endif ()
target_compile_definitions(moonlight-lib PRIVATE ECLIPSE_BUILD_COMMIT="${ECLIPSE_BUILD_COMMIT}")
# Co-op has one HEVC launch path; mapped AMD pairs use the beta host encoder.
# This is no longer an optional app variant or a cached build switch.
target_compile_definitions(moonlight-lib PRIVATE AURORA_GPU_HEVC_EXPERIMENTAL=1)

target_compile_definitions(moonlight-lib PUBLIC LV_LVGL_H_INCLUDE_SIMPLE=1)

target_include_directories(moonlight-lib PUBLIC ${CMAKE_SOURCE_DIR}/core/libgamestream)

target_include_directories(moonlight-lib PUBLIC ${CMAKE_SOURCE_DIR}/utils/include)

if (MINGW)
    target_link_libraries(moonlight-lib PUBLIC libintl.a libiconv.a)
elseif (OS_DARWIN)
    target_link_libraries(moonlight-lib PUBLIC libintl.a)
endif ()

target_include_directories(moonlight-lib SYSTEM PUBLIC ${SDL2_INCLUDE_DIRS} ${SDL2_IMAGE_INCLUDE_DIRS}
        ${FREETYPE_INCLUDE_DIRS} ${INIH_INCLUDE_DIRS})
target_link_libraries(moonlight-lib PUBLIC
        m ${CMAKE_DL_LIBS} Threads::Threads ${SDL2_LIBRARIES} ${SDL2_IMAGE_LIBRARIES}
        moonlight-common-c gamestream lvgl ${INIH_LIBRARIES} ${CURL_LIBRARIES}
        Fontconfig::Fontconfig ${FREETYPE_LIBRARIES})

target_link_libraries(moonlight-lib PUBLIC lv_gridview lv_sdl_img)

target_include_directories(moonlight-lib SYSTEM PUBLIC ${OPUS_INCLUDE_DIRS})
target_link_libraries(moonlight-lib PUBLIC ${OPUS_LIBRARIES})

target_include_directories(moonlight-lib SYSTEM PUBLIC ${MBEDTLS_INCLUDE_DIRS})
target_link_libraries(moonlight-lib PUBLIC ${MBEDCRYPTO_LIBRARY} ${MBEDX509_LIBRARY})

if (TARGET_WEBOS)
    set(FEATURE_FORCE_FULLSCREEN ON)
    set(FEATURE_I18N_GETTEXT OFF)
    set(FEATURE_I18N_LANGUAGE_SETTINGS ON)
    set(FEATURE_CHECK_MODULE_OS_VERSION ON)
    set(FEATURE_INPUT_LIBCEC OFF)
    set(FEATURE_WINDOW_FULLSCREEN_DESKTOP OFF)
    set(FEATURE_INPUT_EVMOUSE ON)

    # PER-VARIANT /tmp FILES. The two installs share /tmp but run as DIFFERENT uids, and a file
    # is owned by whichever app created it first -- so the second app's fopen(...,"a") returns
    # NULL and it silently loses that feature. Measured 2026-09-07: the beta app (uid 6973) could
    # not open the live app's /tmp/coop_multiview.log (uid 6584), and the screenshot probe bails
    # on `if (log == NULL) return 0;` BEFORE reading its script -- so the beta install had no
    # screenshots and no diag log, with nothing to say why.
    if (WEBOS_APPINFO_ID MATCHES "\\.beta$")
        set(AURORA_TMP_SUFFIX "-beta")
        # Test-only launch hooks (platform/webos/app_webos.c: aurora_test_crash) -- the BETA app id only.
        target_compile_definitions(moonlight-lib PRIVATE AURORA_TEST_HOOKS=1)
    else ()
        set(AURORA_TMP_SUFFIX "")
    endif ()
    target_compile_definitions(moonlight-lib PUBLIC TARGET_WEBOS OS_LINUX _GNU_SOURCE
            APPID="${WEBOS_APPINFO_ID}"
            AURORA_DIAG_LOG="/tmp/aurora-diag${AURORA_TMP_SUFFIX}.log"
            AURORA_SECONDARY_LOG="/tmp/coop_secondary${AURORA_TMP_SUFFIX}.log"
            AURORA_MULTIVIEW_LOG="/tmp/coop_multiview${AURORA_TMP_SUFFIX}.log"
            # CO-OP UNIX SOCKETS ARE PER-VARIANT for the same reason the logs are. They live at
            # FIXED /tmp paths, and /tmp has the sticky bit, so a socket left by the LIVE app
            # (uid 6584) cannot be unlink()ed by the BETA app (uid 6973) -- coop_ipc_listen's
            # pre-bind unlink() fails silently, bind() then hits EADDRINUSE, coop_coord_start
            # returns false, and the session dies at "[COOP-SPAWN] coordinator FAILED to start"
            # with the panel stuck on "Connecting..." (measured 2026-09-08, RUN A). Suffixing the
            # path per variant means the two installs never share a socket. Same value as the
            # header default when the suffix is empty, so the live build is byte-unchanged.
            COOP_COORD_SOCKET="/tmp/aurora-coop${AURORA_TMP_SUFFIX}.sock"
            COOP_INPUT_SOCKET="/tmp/aurora-coop-input${AURORA_TMP_SUFFIX}.sock"
            COOP_AUDIO_SOCKET="/tmp/aurora-coop-audio${AURORA_TMP_SUFFIX}.sock"
            # THE PER-PICTURE TRACE (passover 07 §3). Suffixed for the same reason as
            # everything above: /tmp is sticky, so a file created by one variant cannot be
            # reopened or removed by the other.
            AURORA_TRACE_SENTINEL="/tmp/aurora-agetrace${AURORA_TMP_SUFFIX}"
            AURORA_TRACE_LOG="/tmp/aurora-coop-trace${AURORA_TMP_SUFFIX}.csv")
    # The dynamically loaded decoder module does not inherit moonlight-lib's
    # definitions. Give it the same variant's sentinel and its own output path.
    if (TARGET ss4s-module-ndl-webos5)
        target_compile_definitions(ss4s-module-ndl-webos5 PRIVATE
                AURORA_NDL_TRACE_SENTINEL="/tmp/aurora-agetrace${AURORA_TMP_SUFFIX}"
                AURORA_NDL_TRACE_LOG="/tmp/aurora-ndl-trace${AURORA_TMP_SUFFIX}.csv")
    endif ()
    target_compile_definitions(moonlight-lib PUBLIC GAMECONTROLLERDB_PLATFORM="webOS" GAMECONTROLLERDB_PLATFORM_USE="Linux")

    target_include_directories(moonlight-lib SYSTEM PUBLIC ${PBNJSON_C_INCLUDE_DIRS} ${PMLOG_INCLUDE_DIRS}
            ${WEBOSI18N_INCLUDE_DIRS})
    target_link_libraries(moonlight-lib PUBLIC ${PBNJSON_C_LIBRARIES} ${PMLOG_LIBRARIES} ${WEBOSI18N_LIBRARIES})
    target_link_libraries(moonlight-lib PUBLIC commons-luna-sync)
    set_target_properties(moonlight PROPERTIES OUTPUT_NAME aurora)
    # VENDOR MEDIA DIAGNOSTICS (src/app/platform/webos/vendor_diag.c). OFF for every
    # release: it captures the vendor media libraries' PmLog output (the TV keeps no
    # readable log) and dumps thread stacks when a vendor call never returns.
    option(AURORA_VENDOR_DIAG "Capture vendor media library logs and hang stacks (diagnostic builds only)" OFF)
    # Separate NDL module: developer filesystem probes never ship in release.
    if (TARGET ss4s-module-ndl-webos5 AND (WEBOS_APPINFO_ID MATCHES "\\.beta$" OR AURORA_VENDOR_DIAG))
        target_compile_definitions(ss4s-module-ndl-webos5 PRIVATE AURORA_NDL_DIAGNOSTICS=1)
    endif ()
    set(_aurora_export_allow "_init,_fini,__bss_start,_edata,_end")
    if (AURORA_VENDOR_DIAG)
        message(WARNING "AURORA_VENDOR_DIAG is ON: this is a DIAGNOSTIC build -- do not ship it")
        target_sources(moonlight-lib PRIVATE src/app/platform/webos/vendor_diag.c)
        # Per-variant paths for the same reason as AURORA_DIAG_LOG above: sticky /tmp, two uids.
        target_compile_definitions(moonlight-lib PUBLIC AURORA_VENDOR_DIAG=1
                AURORA_VENDOR_LOG="/tmp/aurora-vendor${AURORA_TMP_SUFFIX}.log"
                AURORA_CRASH_LOG="/tmp/aurora-crash${AURORA_TMP_SUFFIX}.log"
                AURORA_STDERR_LOG="/tmp/aurora-stderr${AURORA_TMP_SUFFIX}.log")
        # Export ONLY the PmLog entry points (not -rdynamic: exporting every symbol would let
        # any same-named function in this binary interpose on a vendor library).
        target_link_options(moonlight PRIVATE
                "-Wl,--dynamic-list=${CMAKE_SOURCE_DIR}/src/app/platform/webos/vendor_diag.list")
        string(APPEND _aurora_export_allow ",PmLogGetContext,PmLogSetContextLevel,PmLogString_,_PmLogMsgKV")
    endif ()
    # The executable must export nothing: an exported global overrides the same name in the
    # TV's libraries (the webOS 26 `g_log` stream-start failure). cmake/CheckExecutableExports.cmake
    if (CMAKE_NM)
        add_custom_command(TARGET moonlight POST_BUILD
                COMMAND ${CMAKE_COMMAND} "-DNM=${CMAKE_NM}" "-DBIN=$<TARGET_FILE:moonlight>"
                "-DALLOW=${_aurora_export_allow}" -P "${CMAKE_SOURCE_DIR}/cmake/CheckExecutableExports.cmake"
                VERBATIM)
    else ()
        message(WARNING "Export guard SKIPPED: no CMAKE_NM, so exported-symbol clashes go unchecked")
    endif ()
else ()
    if (TARGET_STEAMLINK)
        set(FEATURE_FORCE_FULLSCREEN ON)
        set(FEATURE_WINDOW_FULLSCREEN_DESKTOP OFF)
        target_compile_definitions(moonlight-lib PUBLIC TARGET_STEAMLINK)
    endif ()
    if (OS_DARWIN)
        target_link_directories(moonlight-lib PUBLIC ${SDL2_LIBRARIES})
        target_compile_definitions(moonlight-lib PUBLIC OS_DARWIN)
        target_compile_definitions(moonlight-lib PUBLIC GAMECONTROLLERDB_PLATFORM="Mac OS X")
    elseif (OS_LINUX)
        # The self-contained desktop release uses its bundled native decoder.
        # Legacy appliance builds may still use an external Moonlight Embedded.
        if (TARGET_RASPI OR TARGET_STEAMLINK)
            set(FEATURE_EMBEDDED_SHELL ON)
        endif ()
        target_compile_definitions(moonlight-lib PUBLIC OS_LINUX _GNU_SOURCE)
        target_compile_definitions(moonlight-lib PUBLIC GAMECONTROLLERDB_PLATFORM="Linux")
        # Co-op diagnostics go to the user's preference directory, as on Windows (see the header).
        target_compile_options(moonlight-lib PUBLIC
                "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/app/platform/sdl/desktop_log_paths.h")
    elseif (OS_WINDOWS)
        target_compile_definitions(moonlight-lib PUBLIC OS_WINDOWS)
        target_compile_definitions(moonlight-lib PUBLIC GAMECONTROLLERDB_PLATFORM="Windows")
        # Diagnostic paths are function-valued macros from win_posix_bits.h. They resolve
        # under SDL's per-user preference directory, so shortcut/temp-directory launches
        # are writable and do not scatter logs into the caller's working directory.
        target_compile_definitions(moonlight-lib PUBLIC
                APPID="com.aurora.gamestream")
        # BSD SOCKET HEADERS DO NOT EXIST ON WINDOWS. The co-op sources (input, audio,
        # video, the Pi pad reader) include <sys/socket.h>, <sys/un.h>, <arpa/inet.h> and
        # friends, and they are byte-shared with the TV -- so the headers are provided
        # rather than the sources edited. Each stand-in pulls in coop_win_compat.h, which
        # does the real mapping onto winsock2/afunix. Only a translation unit that actually
        # includes one of these picks up the remapping, so nothing else in the app is
        # affected. Reused from the Qt client's co-op port rather than written again.
        target_include_directories(moonlight-lib PUBLIC
                "${CMAKE_CURRENT_SOURCE_DIR}/src/app/platform/windows"
                "${CMAKE_CURRENT_SOURCE_DIR}/src/app/platform/windows/winposix")
        target_link_libraries(moonlight-lib PUBLIC ws2_32)
        # Support report (util/diag_report.c: adapters, GPUs) and crash record (util/crash_log.c: stack walk).
        target_link_libraries(moonlight-lib PUBLIC iphlpapi dxgi dxguid dbghelp)
        # Co-op with this PC as one of the hosts: tells the local host's display hook to keep
        # the panel on for the client window (see the file's header).
        target_sources(moonlight-lib PRIVATE src/app/platform/windows/local_host_win.c)
        # ...and the mouse/keyboard hand-over to that host's own pane (see the file's header).
        target_sources(moonlight-lib PRIVATE src/app/platform/windows/local_input_win.c)
        # The mouse and keyboard's own names for the co-op controller routing (HidD_* = hid.dll).
        target_sources(moonlight-lib PRIVATE src/app/platform/windows/input_names_win.c)
        target_link_libraries(moonlight-lib PUBLIC hid)
        # The SAFE additions (setenv, inet_aton) go everywhere; the socket remapping does
        # not -- see the header's own note for why those are separate.
        target_compile_options(moonlight-lib PUBLIC -include win_posix_bits.h)
    else ()
        message(FATAL_ERROR "Unknown build platform")
    endif ()

    set_target_properties(moonlight PROPERTIES OUTPUT_NAME moonlight-tv)
endif ()

# Preserve the diagnostic harness's explicit global paths. Ordinary POSIX releases
# translate only the app's legacy temporary names into its owned cache namespace.
if ((TARGET_WEBOS OR OS_LINUX) AND NOT AURORA_VENDOR_DIAG AND
        NOT WEBOS_APPINFO_ID MATCHES "\\.beta$")
    target_sources(moonlight-lib PRIVATE src/app/platform/posix/runtime_files.c)
    target_compile_definitions(moonlight-lib PUBLIC ECLIPSE_POSIX_RUNTIME=1)
    target_compile_options(moonlight-lib PUBLIC
            -include "${CMAKE_CURRENT_SOURCE_DIR}/src/app/platform/posix/runtime_files.h")
endif ()

if (FEATURE_INPUT_LIBCEC AND TARGET commons-cec-sdl)
    target_link_libraries(moonlight-lib PUBLIC commons-cec-sdl)
else ()
    set(FEATURE_INPUT_LIBCEC OFF)
endif ()

if (FEATURE_INPUT_EVMOUSE AND TARGET commons-evmouse)
    target_link_libraries(moonlight-lib PUBLIC commons-evmouse)
else ()
    set(FEATURE_INPUT_EVMOUSE OFF)
endif ()

target_link_libraries(moonlight-lib PUBLIC commons-ss4s-modules-list commons-sps-parser)

# Co-op split-screen: bind a SECOND starfish media pipeline to its own exported
# window, alongside the primary SS4S/NDL plane. SS4S supports only one selected
# driver per process, so player 2 cannot go through SS4S -- it talks to
# StarfishMediaAPIs directly. Guarded so non-webOS builds are unaffected.
if(TARGET playerAPIs_C)
    # glib too: starfish delivers its load/state callbacks through a GLib main
    # context. Without pumping one, LOADCOMPLETED never arrives, the pipeline
    # never reaches PLAYING, and every feed is rejected.
    pkg_check_modules(COOP_GLIB glib-2.0)
    pkg_check_modules(COOP_WL wayland-client)
    target_link_libraries(moonlight-lib PRIVATE playerAPIs_C ${COOP_GLIB_LIBRARIES} ${COOP_WL_LIBRARIES} wayland-webos-client)
    target_include_directories(moonlight-lib PRIVATE ${COOP_GLIB_INCLUDE_DIRS})
    target_compile_definitions(moonlight-lib PRIVATE HAVE_STARFISH_DIRECT=1)
    # luna-service2: com.webos.service.videooutput is the layer that CONNECTS a decoder's sink
    # to a video PLANE (MAIN=44, SUB0=45) and sets z-order. Every method is in its `public`
    # group, but luna-send-pub is a silent no-op for the `prisoner` shell user -- the call has
    # to come from inside the app, with its own LS2 role. getStatus alone reports the
    # connected state and z-order of BOTH sinks, which is the diagnostic this whole
    # investigation has lacked.
    pkg_check_modules(COOP_LS2 luna-service2)
    if (COOP_LS2_FOUND)
        target_link_libraries(moonlight-lib PRIVATE ${COOP_LS2_LIBRARIES})
        target_include_directories(moonlight-lib PRIVATE ${COOP_LS2_INCLUDE_DIRS})
        target_compile_definitions(moonlight-lib PRIVATE HAVE_LUNA_SERVICE=1)
        message(STATUS "luna-service2 found -- videooutput probe enabled")
    else ()
        message(STATUS "luna-service2 NOT found -- videooutput probe disabled")
    endif ()
endif()

target_link_libraries(moonlight-lib PUBLIC commons-logging commons-gamecontrollerdb-updater commons-lazy
        commons-refcounter commons-executor commons-linked-list commons-wol commons-ini-writer commons-copyfile
        commons-sockaddr commons-hostport)

configure_file(src/app/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h @ONLY)
target_include_directories(moonlight-lib PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

if (OS_WINDOWS)
    target_link_libraries(moonlight-lib PUBLIC rpcrt4)
else ()
    pkg_check_modules(UUID REQUIRED uuid)
    target_link_libraries(moonlight-lib PUBLIC ${UUID_LIBRARIES})
endif ()

if (WITH_SANITIZE)
    target_compile_options(moonlight-lib PUBLIC -fsanitize=${WITH_SANITIZE} -g3 -O0)
    target_link_options(moonlight-lib PUBLIC -fsanitize=${WITH_SANITIZE} -static-libasan -lrt)
endif ()

add_subdirectory(src)

if (NOT DEFINED CMAKE_INSTALL_BINDIR)
    set(CMAKE_INSTALL_BINDIR bin)
endif ()
install(TARGETS moonlight RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if (OS_WINDOWS)
    # path_assets() resolves beside the executable in a portable build. Ship the
    # project-specific mappings there so first-run controller support does not depend
    # on the network updater succeeding.
    install(FILES deploy/webos/assets/gamecontrollerdb_extra.txt
            DESTINATION ${CMAKE_INSTALL_BINDIR})
    # A portable archive has no installer EULA step, so keep the application's
    # GPL licence in the archive itself.
    install(FILES LICENSE.txt
            DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()

if (BUILD_TESTS)
    add_subdirectory(third_party/Unity EXCLUDE_FROM_ALL)
    add_subdirectory(tests)
endif ()

if (FEATURE_I18N_GETTEXT)
    include(MoonlightI18n)
endif ()

if (TARGET_WEBOS)
    include(PackageWebOS)
elseif (TARGET_STEAMLINK)
    include(PackageSteamLink)
elseif (OS_LINUX)
    include(PackageDebian)
elseif (OS_WINDOWS)
endif ()
