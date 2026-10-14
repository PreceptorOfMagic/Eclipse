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
