# The public plugin API: header only, so an INTERFACE library. The target is
# lower case so that the exported name matches the in tree alias and a
# downstream project links dracon::dracon in both cases.
set(TARGET_NAME dracon)
set(PACKAGE_NAME dracon)

add_library(${TARGET_NAME} INTERFACE)
add_library(dracon::dracon ALIAS ${TARGET_NAME})

set(PUBLIC_HEADERS
    dracon/exceptions.h
    dracon/http.h
    dracon/logging.h
    dracon/plugin.h
    dracon/restful.h
    dracon/stream.h
    dracon/thread_worker.h
    dracon/utils.h)

# FILE_SET HEADERS gives the include directory (build and install) and the
# install rule in one go.
target_sources(${TARGET_NAME} INTERFACE
  FILE_SET HEADERS
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
  FILES ${PUBLIC_HEADERS})

# The same headers again, PRIVATE this time. Nothing is compiled from them, but
# the CMake file API only reports non INTERFACE sources, so without this the
# public API is an empty node in IDE project trees (Qt Creator, CLion, VS).
target_sources(${TARGET_NAME} PRIVATE ${PUBLIC_HEADERS})

target_compile_features(${TARGET_NAME} INTERFACE cxx_std_23)

# A plugin *must* be compiled with this: it is what turns a null pointer
# dereference into dracon::segmentation_fault_error instead of killing the
# server. Carried by the API so no plugin can forget it.
target_compile_options(${TARGET_NAME} INTERFACE -fnon-call-exceptions)

install(TARGETS ${TARGET_NAME}
        EXPORT ${TARGET_NAME}_Targets
        FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/${PACKAGE_NAME}ConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/${PACKAGE_NAME}Config.cmake.in"
  "${PROJECT_BINARY_DIR}/${PACKAGE_NAME}Config.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PACKAGE_NAME})

install(
  EXPORT ${TARGET_NAME}_Targets
  FILE ${PACKAGE_NAME}Targets.cmake
  NAMESPACE ${PACKAGE_NAME}::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PACKAGE_NAME})

install(FILES "${PROJECT_BINARY_DIR}/${PACKAGE_NAME}Config.cmake"
              "${PROJECT_BINARY_DIR}/${PACKAGE_NAME}ConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PACKAGE_NAME})
