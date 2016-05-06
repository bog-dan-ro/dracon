add_executable(dracon_library_tests
    dracon_tests.cpp
    dracon_utils.cpp
    dracon_restful_route.cpp
    dracon_http.cpp
    http_parser.cpp
    http_parser_fuzz.cpp)

target_link_libraries(dracon_library_tests PRIVATE
    Dracon::testsLib dracon::dracon Boost::unit_test_framework Threads::Threads)
target_compile_definitions(dracon_library_tests PRIVATE BOOST_TEST_DYN_LINK)
target_include_directories(dracon_library_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/server)

add_test(NAME dracon_library_tests COMMAND dracon_library_tests)
