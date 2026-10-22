# Host tests of the Android port's portable pieces, run on the desktop build (the Android code itself is
# exercised on an emulator by .github/workflows/build-android.yml).
find_package(Python3 COMPONENTS Interpreter)

# The catalogue reader against gettext, every string of every catalogue (needs the desktop's compiled .mo; the
# moonlight-i18n target is defined after the tests, at the end of the top-level CMakeLists.txt).
if (Python3_FOUND AND FEATURE_I18N_GETTEXT)
    add_executable(test_android_i18n_dump test_i18n_mo_dump.c ${CMAKE_SOURCE_DIR}/src/app/platform/android/i18n_android.c)
    target_include_directories(test_android_i18n_dump PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
    target_link_libraries(test_android_i18n_dump PRIVATE commons-logging)
    add_dependencies(test_android_i18n_dump moonlight-i18n)
    add_test(NAME test_android_i18n_mo COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_i18n_mo.py
            $<TARGET_FILE:test_android_i18n_dump> ${CMAKE_BINARY_DIR}/mo)
endif ()

# commons' Android UUID generator.
add_executable(test_android_uuid test_uuid_android.c ${CMAKE_SOURCE_DIR}/third_party/commons/util/uuidstr/uuidstr.c
        ${CMAKE_SOURCE_DIR}/third_party/commons/util/uuidstr/uuidstr_android.c)
target_include_directories(test_android_uuid PRIVATE ${CMAKE_SOURCE_DIR}/third_party/commons/util/uuidstr)
target_compile_definitions(test_android_uuid PRIVATE _DEFAULT_SOURCE)
add_test(NAME test_android_uuid COMMAND test_android_uuid)

# The MediaCodec module's feeding, with NDK fakes, on real and co-op composite streams.
find_package(JNI QUIET)
if (JNI_FOUND)
    add_executable(test_android_mediacodec_feed test_mediacodec_feed.c)
    target_compile_definitions(test_android_mediacodec_feed PRIVATE __ANDROID__=1 SDL_MAIN_HANDLED
            MEDIACODEC_SOURCE="${CMAKE_SOURCE_DIR}/third_party/ss4s/modules/android/mediacodec.c")
    target_include_directories(test_android_mediacodec_feed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ndk-stubs/include
            ${JNI_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/third_party/ss4s/include
            ${CMAKE_SOURCE_DIR}/third_party/ss4s/modules/interface/include)
    target_include_directories(test_android_mediacodec_feed SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(test_android_mediacodec_feed PRIVATE ${SDL2_LIBRARIES} Threads::Threads)
    add_executable(test_android_hevc_tilecols ${CMAKE_SOURCE_DIR}/tests/coop/hevc_tilecols_cli.c
            ${CMAKE_SOURCE_DIR}/src/app/stream/video/coop_splice.c ${CMAKE_SOURCE_DIR}/src/app/stream/video/coop_poc.c)
    target_include_directories(test_android_hevc_tilecols PRIVATE ${CMAKE_SOURCE_DIR}/src/app/stream/video)
    add_test(NAME test_android_mediacodec_feed COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_mediacodec_test.sh
            $<TARGET_FILE:test_android_mediacodec_feed> $<TARGET_FILE:test_android_hevc_tilecols>
            ${CMAKE_CURRENT_BINARY_DIR}/mediacodec-fixtures)
    set_tests_properties(test_android_mediacodec_feed PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(test_android_hevc_zerowords ${CMAKE_SOURCE_DIR}/tests/coop/hevc_zerowords_cli.c
            ${CMAKE_SOURCE_DIR}/src/app/stream/video/coop_splice.c ${CMAKE_SOURCE_DIR}/src/app/stream/video/coop_poc.c)
    target_include_directories(test_android_hevc_zerowords PRIVATE ${CMAKE_SOURCE_DIR}/src/app/stream/video)
    add_test(NAME test_android_hevc_zerowords COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_zerowords_test.sh
            $<TARGET_FILE:test_android_hevc_tilecols> $<TARGET_FILE:test_android_hevc_zerowords>
            ${CMAKE_CURRENT_BINARY_DIR}/zerowords-fixtures)
    set_tests_properties(test_android_hevc_zerowords PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(test_android_mediacodec_restart test_mediacodec_restart.c)
    target_compile_definitions(test_android_mediacodec_restart PRIVATE __ANDROID__=1 SDL_MAIN_HANDLED
            MEDIACODEC_SOURCE="${CMAKE_SOURCE_DIR}/third_party/ss4s/modules/android/mediacodec.c")
    target_include_directories(test_android_mediacodec_restart PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ndk-stubs/include
            ${JNI_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/third_party/ss4s/include
            ${CMAKE_SOURCE_DIR}/third_party/ss4s/modules/interface/include)
    target_include_directories(test_android_mediacodec_restart SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(test_android_mediacodec_restart PRIVATE ${SDL2_LIBRARIES} Threads::Threads)
    add_test(NAME test_android_mediacodec_restart COMMAND test_android_mediacodec_restart)
endif ()

# commons' logcat backend.
add_executable(test_android_logging test_logging_android.c ${CMAKE_SOURCE_DIR}/third_party/commons/util/logging/logging_common.c
        ${CMAKE_SOURCE_DIR}/third_party/commons/util/logging/logging_android.c)
target_compile_definitions(test_android_logging PRIVATE _DEFAULT_SOURCE)
target_include_directories(test_android_logging PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ndk-stubs/include
        ${CMAKE_SOURCE_DIR}/third_party/commons/util/logging)
add_test(NAME test_android_logging COMMAND test_android_logging)

# The Android app's plain-Java helpers, on the host JDK.
find_package(Java COMPONENTS Development Runtime QUIET)
if (Java_FOUND)
    set(ECLIPSE_ANDROID_JAVA ${CMAKE_SOURCE_DIR}/deploy/android/app/src/main/java/io/github/preceptorofmagic/eclipse)
    add_test(NAME test_android_audio_outputs COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_java_test.sh
            ${Java_JAVAC_EXECUTABLE} ${Java_JAVA_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/java-audio-outputs
            io.github.preceptorofmagic.eclipse.AudioOutputsTest ${ECLIPSE_ANDROID_JAVA}/AudioOutputs.java
            ${CMAKE_CURRENT_SOURCE_DIR}/java/io/github/preceptorofmagic/eclipse/AudioOutputsTest.java)
endif ()

# The SS4S SDL audio module's Android output choice, with the activity and SDL's device open faked.
if (JNI_FOUND)
    add_executable(test_android_sdl_audio_output test_sdl_audio_output.c
            ${CMAKE_SOURCE_DIR}/third_party/ss4s/modules/utils/ringbuf/ringbuf.c)
    target_compile_definitions(test_android_sdl_audio_output PRIVATE __ANDROID__=1 SDL_MAIN_HANDLED
            MODULE_SOURCE="${CMAKE_SOURCE_DIR}/third_party/ss4s/modules/sdl/sdl.c")
    target_include_directories(test_android_sdl_audio_output PRIVATE ${JNI_INCLUDE_DIRS}
            ${CMAKE_SOURCE_DIR}/third_party/ss4s/include ${CMAKE_SOURCE_DIR}/third_party/ss4s/modules/interface/include
            ${CMAKE_SOURCE_DIR}/third_party/ss4s/modules/utils/ringbuf)
    target_link_libraries(test_android_sdl_audio_output PRIVATE ${SDL2_LIBRARIES})
    add_test(NAME test_android_sdl_audio_output COMMAND test_android_sdl_audio_output)
endif ()

# The audio outputs list as the settings read it.
add_executable(test_android_audio_outputs_parse test_audio_outputs_parse.c
        ${CMAKE_SOURCE_DIR}/src/app/platform/android/audio_outputs_android.c)
target_include_directories(test_android_audio_outputs_parse PRIVATE ${CMAKE_SOURCE_DIR}/src/app/platform/android
        ${CMAKE_SOURCE_DIR}/src/app)
target_include_directories(test_android_audio_outputs_parse SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
add_test(NAME test_android_audio_outputs_parse COMMAND test_android_audio_outputs_parse)

# Which mapping a controller gets on Android, judged by the buttons it can produce (input_gamepad_mapping.c).
add_executable(test_android_controller_mapping test_controller_mapping.c
        ${CMAKE_SOURCE_DIR}/src/app/platform/android/controller_mapping_android.c)
target_include_directories(test_android_controller_mapping PRIVATE ${CMAKE_SOURCE_DIR}/src/app/platform/android)
target_include_directories(test_android_controller_mapping SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(test_android_controller_mapping PRIVATE ${SDL2_LIBRARIES})
add_test(NAME test_android_controller_mapping COMMAND test_android_controller_mapping)
if (Java_FOUND)
    add_test(NAME test_android_display_modes COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_java_test.sh
            ${Java_JAVAC_EXECUTABLE} ${Java_JAVA_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/java-display-modes
            io.github.preceptorofmagic.eclipse.DisplayModesTest ${ECLIPSE_ANDROID_JAVA}/DisplayModes.java
            ${CMAKE_CURRENT_SOURCE_DIR}/java/io/github/preceptorofmagic/eclipse/DisplayModesTest.java)
endif ()

# The refresh rate an Android stream asks its host for.
add_executable(test_android_display_refresh test_display_refresh.c
        ${CMAKE_SOURCE_DIR}/src/app/platform/android/display_android.c)
target_include_directories(test_android_display_refresh PRIVATE ${CMAKE_SOURCE_DIR}/src/app/platform/android
        ${CMAKE_SOURCE_DIR}/src/app)
add_test(NAME test_android_display_refresh COMMAND test_android_display_refresh)

# Every C source of the Android app, compiled (syntax only) as the Android build configures it.
if (Python3_FOUND AND JNI_FOUND)
    add_test(NAME test_android_typecheck COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/typecheck_android.py
            ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_BINARY_DIR}/typecheck ${JNI_INCLUDE_DIRS})
endif ()

# Android's paths and launch arguments (app.h stood in for by tests/android/stubs/app.h).
add_executable(test_android_paths_launch test_paths_launch.c
        ${CMAKE_SOURCE_DIR}/src/app/platform/android/path_android.c
        ${CMAKE_SOURCE_DIR}/src/app/platform/android/app_android.c
        ${CMAKE_SOURCE_DIR}/src/app/util/path.c)
target_compile_definitions(test_android_paths_launch PRIVATE TARGET_ANDROID=1 _GNU_SOURCE)
target_include_directories(test_android_paths_launch BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_include_directories(test_android_paths_launch PRIVATE ${CMAKE_SOURCE_DIR}/src/app/platform/android
        ${CMAKE_SOURCE_DIR}/src/app ${CMAKE_SOURCE_DIR}/core/moonlight-common-c/src
        ${CMAKE_SOURCE_DIR}/third_party/ss4s/include)
target_include_directories(test_android_paths_launch SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(test_android_paths_launch PRIVATE commons-uuidstr commons-logging ${SDL2_LIBRARIES})
add_test(NAME test_android_paths_launch COMMAND test_android_paths_launch)

# config.h as the Android build makes it (CMakeLists.txt, TARGET_ANDROID: always fullscreen, the in-app language
# setting, no libcec), for tests that compile the app's Android code.
function(eclipse_android_config dir)
    set(FEATURE_FORCE_FULLSCREEN ON)
    set(FEATURE_I18N_LANGUAGE_SETTINGS ON)
    set(FEATURE_SURROUND_SOUND ON)
    set(FEATURE_CHECK_MODULE_OS_VERSION OFF)
    set(FEATURE_INPUT_EVMOUSE OFF)
    set(FEATURE_INPUT_LIBCEC OFF)
    set(FEATURE_WINDOW_FULLSCREEN_DESKTOP ON)
    set(FEATURE_EMBEDDED_SHELL OFF)
    configure_file(${CMAKE_SOURCE_DIR}/src/app/config.h.in ${dir}/config.h @ONLY)
endfunction()
eclipse_android_config(${CMAKE_CURRENT_BINARY_DIR}/android-config)

# The Android TV remote during a stream (session_keyboard.c and keyboard_android.c, compiled for Android).
add_executable(test_android_remote_keys test_remote_keys.c)
target_compile_definitions(test_android_remote_keys PRIVATE TARGET_ANDROID=1 _GNU_SOURCE
        KEYBOARD_SOURCE="${CMAKE_SOURCE_DIR}/src/app/platform/android/keyboard_android.c"
        SESSION_KEYBOARD_SOURCE="${CMAKE_SOURCE_DIR}/src/app/stream/input/session_keyboard.c")
target_include_directories(test_android_remote_keys BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/android-config)
target_include_directories(test_android_remote_keys PRIVATE ${CMAKE_SOURCE_DIR}/src/app/platform/android
        ${CMAKE_SOURCE_DIR}/src/app ${CMAKE_SOURCE_DIR}/core/moonlight-common-c/src
        ${CMAKE_SOURCE_DIR}/third_party/ss4s/include ${CMAKE_SOURCE_DIR}/third_party/commons/util/linked-list
        ${CMAKE_SOURCE_DIR}/core/libgamestream/libgamestream ${CMAKE_SOURCE_DIR}/core/libgamestream)
target_include_directories(test_android_remote_keys SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(test_android_remote_keys PRIVATE lvgl commons-logging commons-uuidstr commons-lazy commons-refcounter
        commons-executor commons-linked-list commons-ss4s-modules-list commons-os-info commons-version-info
        commons-array-list commons-sockaddr commons-hostport commons-wol commons-ini-writer commons-copyfile
        commons-gamecontrollerdb-updater ${SDL2_LIBRARIES})
add_test(NAME test_android_remote_keys COMMAND test_android_remote_keys)

# View + Start shows the home screen (session_gamepad.c, compiled for Android): the SHIELD TV's Guide button.
add_executable(test_android_gamepad_home test_gamepad_home.c ${CMAKE_SOURCE_DIR}/src/app/stream/input/start_wheel.c)
target_compile_definitions(test_android_gamepad_home PRIVATE TARGET_ANDROID=1 _GNU_SOURCE
        SESSION_GAMEPAD_SOURCE="${CMAKE_SOURCE_DIR}/src/app/stream/input/session_gamepad.c")
target_include_directories(test_android_gamepad_home BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/android-config)
target_include_directories(test_android_gamepad_home PRIVATE ${CMAKE_SOURCE_DIR}/src/app/platform/android
        ${CMAKE_SOURCE_DIR}/src/app ${CMAKE_SOURCE_DIR}/core/moonlight-common-c/src
        ${CMAKE_SOURCE_DIR}/third_party/ss4s/include ${CMAKE_SOURCE_DIR}/third_party/commons/util/linked-list
        ${CMAKE_SOURCE_DIR}/core/libgamestream/libgamestream ${CMAKE_SOURCE_DIR}/core/libgamestream)
target_include_directories(test_android_gamepad_home SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(test_android_gamepad_home PRIVATE lvgl commons-logging commons-uuidstr commons-lazy
        commons-refcounter commons-executor commons-linked-list commons-ss4s-modules-list commons-os-info
        commons-version-info commons-array-list commons-sockaddr commons-hostport commons-wol commons-ini-writer
        commons-copyfile commons-gamecontrollerdb-updater ${SDL2_LIBRARIES})
add_test(NAME test_android_gamepad_home COMMAND test_android_gamepad_home)

# A peer IDR converted to point at the SPS's reference set, as the slice beside it does (the SHIELD's decoder turned
# a pane green when the two stated it differently).
add_executable(test_android_coop_idr_sps test_coop_idr_sps.c ${CMAKE_SOURCE_DIR}/src/app/stream/video/coop_poc.c)
target_include_directories(test_android_coop_idr_sps PRIVATE ${CMAKE_SOURCE_DIR}/src/app/stream/video)
add_test(NAME test_android_coop_idr_sps COMMAND test_android_coop_idr_sps)

# The virtual mouse in co-op points like the magic remote (session_virt_mouse.c compiled for Android, with the real
# coop_pointer.c): it crosses the divider and clicks on the PC it is over.
add_executable(test_android_vmouse_coop test_vmouse_coop.c ${CMAKE_SOURCE_DIR}/src/app/stream/input/coop_pointer.c)
target_compile_definitions(test_android_vmouse_coop PRIVATE TARGET_ANDROID=1 _GNU_SOURCE
        SESSION_VIRT_MOUSE_SOURCE="${CMAKE_SOURCE_DIR}/src/app/stream/input/session_virt_mouse.c"
        COOP_PTR_VMIRROR_SENTINEL="${CMAKE_CURRENT_BINARY_DIR}/test-vmouse-coop-vmirror"
        AURORA_DIAG_LOG="${CMAKE_CURRENT_BINARY_DIR}/test-vmouse-coop-diag.log")
target_include_directories(test_android_vmouse_coop BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/android-config)
target_include_directories(test_android_vmouse_coop PRIVATE ${CMAKE_SOURCE_DIR}/src/app/platform/android
        ${CMAKE_SOURCE_DIR}/src/app ${CMAKE_SOURCE_DIR}/src/app/stream/input ${CMAKE_SOURCE_DIR}/core/moonlight-common-c/src
        ${CMAKE_SOURCE_DIR}/third_party/ss4s/include ${CMAKE_SOURCE_DIR}/third_party/commons/util/linked-list
        ${CMAKE_SOURCE_DIR}/core/libgamestream/libgamestream ${CMAKE_SOURCE_DIR}/core/libgamestream)
target_include_directories(test_android_vmouse_coop SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(test_android_vmouse_coop PRIVATE lvgl commons-logging commons-uuidstr commons-lazy
        commons-refcounter commons-executor commons-linked-list commons-ss4s-modules-list commons-os-info
        commons-version-info commons-array-list commons-sockaddr commons-hostport commons-wol commons-ini-writer
        commons-copyfile commons-gamecontrollerdb-updater ${SDL2_LIBRARIES})
add_test(NAME test_android_vmouse_coop COMMAND test_android_vmouse_coop)

# Stopping discovery inside the event pass (discovery.c, compiled for Android): it deadlocked on the SHIELD TV.
add_executable(test_android_discovery_stop test_discovery_stop_in_events.c)
target_compile_definitions(test_android_discovery_stop PRIVATE TARGET_ANDROID=1 _GNU_SOURCE
        DISCOVERY_SOURCE="${CMAKE_SOURCE_DIR}/src/app/backend/pcmanager/discovery/discovery.c")
target_include_directories(test_android_discovery_stop BEFORE PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/android-config)
target_include_directories(test_android_discovery_stop PRIVATE ${CMAKE_SOURCE_DIR}/src/app
        ${CMAKE_SOURCE_DIR}/src/app/backend/pcmanager/discovery)
target_include_directories(test_android_discovery_stop SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(test_android_discovery_stop PRIVATE commons-logging commons-sockaddr Threads::Threads ${SDL2_LIBRARIES})
add_test(NAME test_android_discovery_stop COMMAND test_android_discovery_stop)
set_tests_properties(test_android_discovery_stop PROPERTIES TIMEOUT 30)
if (Java_FOUND)
    add_test(NAME test_android_tv_keys COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_java_test.sh
            ${Java_JAVAC_EXECUTABLE} ${Java_JAVA_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/java-tv-keys
            io.github.preceptorofmagic.eclipse.TvKeysTest ${ECLIPSE_ANDROID_JAVA}/TvKeys.java
            ${CMAKE_CURRENT_SOURCE_DIR}/java/io/github/preceptorofmagic/eclipse/TvKeysTest.java)
endif ()

# src/app/platform/android/env_android.c: readers while the environment changes, under AddressSanitizer.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address")
check_c_source_compiles("int main(void) { return 0; }" ECLIPSE_HAVE_ASAN)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if (ECLIPSE_HAVE_ASAN AND NOT WIN32)
    add_executable(test_android_env test_env_android.c ${CMAKE_SOURCE_DIR}/src/app/platform/android/env_android.c)
    target_compile_options(test_android_env PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(test_android_env PRIVATE -fsanitize=address "LINKER:--wrap=setenv" "LINKER:--wrap=unsetenv")
    target_link_libraries(test_android_env PRIVATE Threads::Threads)
    add_test(NAME test_android_env COMMAND test_android_env)
    set_tests_properties(test_android_env PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1")
endif ()
