# app_wakelock_webos.c talks to tvpower over a luna SUBSCRIPTION, so it needs HContext
# from libhelpers.h. commons-luna-sync links the same library but marks its include dirs
# PRIVATE, so moonlight-lib does not inherit them -- rediscover them here rather than
# reach into third_party or hand-copy an ABI-sensitive struct.
if (NOT HELPERS_FOUND)
    pkg_check_modules(HELPERS helpers)
endif ()
target_include_directories(moonlight-lib SYSTEM PRIVATE ${HELPERS_INCLUDE_DIRS})

target_sources(moonlight-lib PRIVATE
        app_wakelock_webos.c
        keyboard_webos.c
        navkey_sdl_webos.c
        pbnjson_compat.c)

# Tells platform/common not to also compile app_wakelock_common.c, whose
# SDL_DisableScreenSaver() is a no-op against webOS's own tvpower screensaver.
# PARENT_SCOPE because platform/common is a SIBLING subdirectory, added by the parent
# after this one -- a plain set() here would be invisible there and both files would
# compile, colliding on app_set_keep_awake.
set(APP_WAKELOCK_IMPLEMENTED ON PARENT_SCOPE)