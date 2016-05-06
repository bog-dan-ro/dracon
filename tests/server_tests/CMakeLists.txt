# Integration tests: they start a real server and talk to it over http and https
add_executable(dracon_server_tests
    server_tests.cpp
    response_status_error_exceptions.cpp
    responses.cpp
    stress_server.cpp
    security.cpp
    utils.cpp)

target_link_libraries(dracon_server_tests PRIVATE
    Dracon::testsLib dracon::dracon Boost::unit_test_framework Threads::Threads)
target_compile_definitions(dracon_server_tests PRIVATE BOOST_TEST_DYN_LINK)

# The suite spawns the server binary and needs the test plugin next to it
add_dependencies(dracon_server_tests dracon_server server_tests_plugin static_content)

add_test(NAME dracon_server_tests COMMAND dracon_server_tests)
