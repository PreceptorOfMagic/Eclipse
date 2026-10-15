add_subdirectory(e2e)

add_unit_test(test_settings test_settings.c)
add_unit_test(test_coop_input_lifecycle test_coop_input_lifecycle.c)
if (NOT WIN32)   # the coordinator spawns its secondary with fork/exec there
    add_unit_test(test_coop_start_error test_coop_start_error.c)
endif ()
add_unit_test(test_img_loader_shutdown test_img_loader_shutdown.c)

add_subdirectory(backend)
add_subdirectory(ui)
