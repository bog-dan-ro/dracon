configure_file(${PROJECT_NAME}.conf ${CMAKE_BINARY_DIR}/etc/dracon/${PROJECT_NAME}.conf)

# The TLS certificate and its key are Dracon's, not this project's: it ships a
# self signed pair for its own https port and copies it next to server.conf.
# Never ship a key of your own in a source tree, generate it at install time.

install(FILES ${PROJECT_NAME}.conf DESTINATION etc/dracon)
