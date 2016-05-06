# The target is dracon_server, the binary it produces is "dracon": the plain
# "dracon" name belongs to the project and to the public API target
add_executable(dracon_server
    http_parser.h
    main.cpp
    server.cpp server.h
    server_logger.h
    server_plugin.cpp server_plugin.h
    server_service_sessions.cpp server_service_sessions.h
    server_session.cpp server_session.h
    sessions_event_loop.cpp sessions_event_loop.h
    streams.cpp streams.h
)

set_target_properties(dracon_server PROPERTIES OUTPUT_NAME dracon)

target_compile_definitions(dracon_server PRIVATE
    BOOST_LOG_DYN_LINK BOOST_ALL_DYN_LINK
    $<$<BOOL:${ENABLE_TRACE_LOG}>:ENABLE_TRACE_LOG>
    $<$<BOOL:${ENABLE_DEBUG_LOG}>:ENABLE_DEBUG_LOG>)

target_link_libraries(dracon_server PRIVATE
    dracon::dracon
    Boost::coroutine Boost::log Boost::log_setup Boost::program_options
    OpenSSL::SSL OpenSSL::Crypto
    Threads::Threads
    ${CMAKE_DL_LIBS})

target_set_sanitizers(dracon_server)

install(TARGETS dracon_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
