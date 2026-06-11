add_executable(p4gw_tests
    test_main.cpp
    test_config.cpp
)
target_link_libraries(p4gw_tests PRIVATE p4gw_core)

add_test(NAME unit COMMAND p4gw_tests)
add_test(NAME cli_help COMMAND gw --help)
