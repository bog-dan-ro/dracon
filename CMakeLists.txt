cmake_minimum_required(VERSION 3.28)

project(dracon LANGUAGES CXX VERSION 1.0)

include(GNUInstallDirs)
include(CheckCXXCompilerFlag)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # -std=c++23 rather than -std=gnu++23
set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # -fPIE for the executable, -fPIC for the plugins
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # compile_commands.json for clangd & co

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/dracon/plugins)

option(ENABLE_TRACE_LOG "Enable the TRACE() log macros" OFF)
option(ENABLE_DEBUG_LOG "Enable the DEBUG() log macros" OFF)
option(ENABLE_TESTS "Build the test suites" ON)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    option(ENABLE_SANITIZERS "Build with the undefined behaviour and leak sanitizers" ON)
else()
    option(ENABLE_SANITIZERS "Build with the undefined behaviour and leak sanitizers" OFF)
endif()

set(Boost_USE_MULTITHREADED ON)
find_package(Boost 1.70 REQUIRED CONFIG
             COMPONENTS coroutine iostreams log log_setup program_options)
find_package(OpenSSL 1.1 REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra)
foreach(hardeningFlag -fstack-protector-strong -fstack-clash-protection)
    string(MAKE_C_IDENTIFIER "HAVE_${hardeningFlag}" hardeningFlagVar)
    check_cxx_compiler_flag(${hardeningFlag} ${hardeningFlagVar})
    if (${hardeningFlagVar})
        add_compile_options(${hardeningFlag})
    endif()
endforeach()
add_compile_options($<$<NOT:$<CONFIG:Debug>>:-U_FORTIFY_SOURCE>
                    $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=2>)
add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)

function(target_set_sanitizers target)
    if (ENABLE_SANITIZERS)
        set(sanitizerFlags -fno-omit-frame-pointer -fsanitize=undefined -fsanitize=leak)
        target_compile_definitions(${target} PUBLIC BOOST_USE_ASAN)
        target_compile_options(${target} PUBLIC ${sanitizerFlags})
        target_link_options(${target} PUBLIC ${sanitizerFlags})
    endif()
endfunction()

add_subdirectory(conf)
add_subdirectory(include)
add_subdirectory(src)

if (ENABLE_TESTS)
    find_package(Boost 1.70 CONFIG COMPONENTS unit_test_framework)
    find_package(CURL)
    if (TARGET Boost::unit_test_framework AND TARGET CURL::libcurl)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(STATUS "Boost.Test or libcurl not found, the tests are disabled")
    endif()
endif()
