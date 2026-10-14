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
