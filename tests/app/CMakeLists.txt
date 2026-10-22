add_subdirectory(e2e)

add_unit_test(test_settings test_settings.c)
add_unit_test(test_coop_input_lifecycle test_coop_input_lifecycle.c)
add_unit_test(test_img_loader_shutdown test_img_loader_shutdown.c)
add_unit_test(test_start_wheel test_start_wheel.c)

# The virtual mouse in co-op points like the magic remote (session_virt_mouse.c with the real coop_pointer.c, both
# included rather than linked, so it does not take moonlight-lib): it crosses the divider and clicks on the PC it is over.
add_executable(test_vmouse_coop test_vmouse_coop.c ${CMAKE_SOURCE_DIR}/src/app/stream/input/coop_pointer.c)
target_compile_definitions(test_vmouse_coop PRIVATE
        SESSION_VIRT_MOUSE_SOURCE="${CMAKE_SOURCE_DIR}/src/app/stream/input/session_virt_mouse.c"
        COOP_PTR_VMIRROR_SENTINEL="${CMAKE_CURRENT_BINARY_DIR}/test-vmouse-coop-vmirror"
        AURORA_DIAG_LOG="${CMAKE_CURRENT_BINARY_DIR}/test-vmouse-coop-diag.log")
target_include_directories(test_vmouse_coop PRIVATE $<TARGET_PROPERTY:moonlight-lib,INCLUDE_DIRECTORIES>)
target_compile_definitions(test_vmouse_coop PRIVATE $<TARGET_PROPERTY:moonlight-lib,COMPILE_DEFINITIONS>)
target_link_libraries(test_vmouse_coop PRIVATE ${SDL2_LIBRARIES})
add_test(test_vmouse_coop test_vmouse_coop)

add_subdirectory(backend)
add_subdirectory(ui)
