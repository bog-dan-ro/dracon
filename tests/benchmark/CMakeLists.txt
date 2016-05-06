add_executable(dracon_benchmark main.cpp)
target_link_libraries(dracon_benchmark PRIVATE Dracon::testsLib Threads::Threads)
add_dependencies(dracon_benchmark dracon_server server_tests_plugin)
