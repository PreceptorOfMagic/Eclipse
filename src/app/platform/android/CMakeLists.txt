target_sources(moonlight-lib PRIVATE
        android_platform.c
        controller_mapping_android.c
        audio_outputs_android.c
        display_android.c
        keyboard_android.c
        navkey_sdl_android.c)

# TEST BUILDS ONLY: the emulator smoke tests (selftest_android.c). Never in a release.
option(ECLIPSE_ANDROID_SELFTEST "Build the Android emulator self-test hooks (test builds only)" OFF)
if (ECLIPSE_ANDROID_SELFTEST)
    message(WARNING "ECLIPSE_ANDROID_SELFTEST is ON: this is a TEST build -- do not ship it")
    target_sources(moonlight-lib PRIVATE selftest_android.c)
    target_compile_definitions(moonlight-lib PUBLIC ECLIPSE_ANDROID_SELFTEST=1)
endif ()
