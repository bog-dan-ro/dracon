# MODULE and not SHARED: a plugin is dlopen()ed, never linked against
add_library(server_tests_plugin MODULE test_plugin.cpp)
add_library(Dracon::server_tests_plugin ALIAS server_tests_plugin)

target_compile_definitions(server_tests_plugin PRIVATE
    BOOST_LOG_DYN_LINK BOOST_ALL_DYN_LINK
    $<$<BOOL:${ENABLE_TRACE_LOG}>:ENABLE_TRACE_LOG>
    $<$<BOOL:${ENABLE_DEBUG_LOG}>:ENABLE_DEBUG_LOG>)

target_link_libraries(server_tests_plugin PRIVATE
    dracon::dracon Boost::log Boost::log_setup Threads::Threads)

target_link_options(server_tests_plugin PRIVATE -Wl,--no-undefined)

target_set_sanitizers(server_tests_plugin)
