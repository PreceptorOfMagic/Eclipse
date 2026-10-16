if (NOT BUILD_TESTS)
    return()
endif ()

add_executable(test-gamestream-file-compat
        file_compat.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/file_compat.c)
target_include_directories(test-gamestream-file-compat PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME test-gamestream-file-compat COMMAND test-gamestream-file-compat)

if (UNIX)
    add_executable(test-gamestream-private-key-permissions
            private_key_permissions.c
            ${CMAKE_CURRENT_SOURCE_DIR}/../src/file_compat.c)
    target_include_directories(test-gamestream-private-key-permissions PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    add_test(NAME test-gamestream-private-key-permissions COMMAND test-gamestream-private-key-permissions)
    set_tests_properties(test-gamestream-private-key-permissions PROPERTIES TIMEOUT 10)
endif()

if (UNIX)
    # The client identity after a first start that did not finish (conf.c).
    add_executable(test-gamestream-conf-recovery conf_recovery.c)
    target_include_directories(test-gamestream-conf-recovery PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../libgamestream)
    target_link_libraries(test-gamestream-conf-recovery PRIVATE gamestream moonlight-common-c)
    add_test(NAME test-gamestream-conf-recovery COMMAND test-gamestream-conf-recovery)
endif ()

add_subdirectory(crypt)
