# MODULE and not SHARED: a plugin is dlopen()ed, never linked against
add_library(static_content MODULE static_content_plugin.cpp)
add_library(Dracon::static_content ALIAS static_content)

target_compile_definitions(static_content PRIVATE
    BOOST_LOG_DYN_LINK BOOST_ALL_DYN_LINK
    $<$<BOOL:${ENABLE_TRACE_LOG}>:ENABLE_TRACE_LOG>
    $<$<BOOL:${ENABLE_DEBUG_LOG}>:ENABLE_DEBUG_LOG>)

target_link_libraries(static_content PRIVATE
    dracon::dracon Boost::iostreams Boost::log Boost::log_setup)

target_link_options(static_content PRIVATE -Wl,--no-undefined)

target_set_sanitizers(static_content)

install(TARGETS static_content LIBRARY DESTINATION lib/dracon/plugins)

# The default document root content (a landing page pointing at the docs). It
# is installed next to the plugin rather than into /var/www itself: the
# document root belongs to the administrator, so the packaging only seeds it
# from here when it is missing or empty.
install(DIRECTORY ${CMAKE_SOURCE_DIR}/www/ DESTINATION ${CMAKE_INSTALL_DATADIR}/dracon/www)
