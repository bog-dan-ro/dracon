include_directories(${CURL_INCLUDE_DIRS})
include_directories(${CMAKE_SOURCE_DIR}/src)

# The sanitizers are set up once, at the top level: adding -fsanitize=address
# only here used to mix it with the undefined/leak build of everything else

set(TEST_SRCS main.cpp devices.cpp)

add_executable(${PROJECT_NAME}Tests ${TEST_SRCS})
target_link_libraries(${PROJECT_NAME}Tests PRIVATE Dracon::testsLib ${CURL_LIBRARIES} ${Boost_LIBRARIES}
                                                   Boost::unit_test_framework Threads::Threads)
target_compile_definitions(${PROJECT_NAME}Tests PRIVATE BOOST_TEST_DYN_LINK)
add_dependencies(${PROJECT_NAME}Tests ${PROJECT_NAME})
add_test(NAME ${PROJECT_NAME}Tests COMMAND ${PROJECT_NAME}Tests)
