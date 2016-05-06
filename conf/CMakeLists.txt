configure_file(server.conf ${CMAKE_BINARY_DIR}/etc/dracon/server.conf)
configure_file(server_logging.conf ${CMAKE_BINARY_DIR}/etc/dracon/server_logging.conf)
configure_file(server_ssl_ctx.conf ${CMAKE_BINARY_DIR}/etc/dracon/server_ssl_ctx.conf)
configure_file(static_files.conf ${CMAKE_BINARY_DIR}/etc/dracon/static_files.conf)
configure_file(server.crt ${CMAKE_BINARY_DIR}/etc/dracon/server.crt COPYONLY)
configure_file(server.key ${CMAKE_BINARY_DIR}/etc/dracon/server.key COPYONLY)

set(DRACON_SYSCONFDIR "${CMAKE_INSTALL_SYSCONFDIR}/dracon")
if (CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    set(DRACON_SYSCONFDIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/dracon")
endif()

install(DIRECTORY ${CMAKE_BINARY_DIR}/etc/dracon/ DESTINATION ${DRACON_SYSCONFDIR}
    FILES_MATCHING PATTERN "*.conf"
    PATTERN "CMake*" EXCLUDE)

set(SYSTEMD_UNIT_DIR "lib/systemd/system" CACHE STRING "where the systemd unit is installed")
set(SYSUSERS_DIR "lib/sysusers.d" CACHE STRING "where the sysusers.d snippet is installed")
configure_file(dracon.service.in ${CMAKE_BINARY_DIR}/${SYSTEMD_UNIT_DIR}/dracon.service @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/${SYSTEMD_UNIT_DIR}/dracon.service DESTINATION ${SYSTEMD_UNIT_DIR})
install(FILES dracon.sysusers DESTINATION ${SYSUSERS_DIR} RENAME dracon.conf)

set(TMPFILES_DIR "lib/tmpfiles.d" CACHE STRING "where the tmpfiles.d snippet is installed")
install(FILES dracon-staticcontent.tmpfiles DESTINATION ${TMPFILES_DIR}
        RENAME dracon-staticcontent.conf)
