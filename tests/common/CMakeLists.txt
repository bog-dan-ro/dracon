add_library(DraconTestsLib STATIC
    easy_curl.cpp
    dracon_server.cpp
    raw_client.cpp)
add_library(Dracon::testsLib ALIAS DraconTestsLib)

target_sources(DraconTestsLib PUBLIC
  FILE_SET HEADERS
  BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}
  FILES
    dracon_server.h
    easy_curl.h
    raw_client.h)

target_link_libraries(DraconTestsLib PUBLIC CURL::libcurl)
target_compile_features(DraconTestsLib PUBLIC cxx_std_23)
target_set_sanitizers(DraconTestsLib)

install(TARGETS DraconTestsLib
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/dracon
        FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dracon/test)
