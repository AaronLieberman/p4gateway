add_executable(p4gw_tests
    test_main.cpp
    test_config.cpp
    test_p4view.cpp
    test_prepare.cpp
    test_mirror.cpp
    test_manifest.cpp
    test_status.cpp
    test_shelf.cpp
    test_preflight.cpp
    test_subprocess.cpp
)
target_link_libraries(p4gw_tests PRIVATE p4gw_core)

add_test(NAME unit COMMAND p4gw_tests)
add_test(NAME cli_help COMMAND gw --help)
