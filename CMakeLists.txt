cmake_minimum_required(VERSION 3.23.0)

project(http-proxy)

set(CMAKE_CXX_STANDARD 23)

set(OPTFLAGS -gdwarf-2)
set(WARNINGS -Wall -Wno-missing-field-initializers -Wno-ignored-qualifiers)
add_compile_options(${OPTFLAGS} ${WARNINGS})

if(NOT EXISTS "${CMAKE_BINARY_DIR}/xrepo.cmake")
    message(STATUS "Downloading xrepo.cmake from https://github.com/xmake-io/xrepo-cmake/")
    # mirror https://cdn.jsdelivr.net/gh/xmake-io/xrepo-cmake@main/xrepo.cmake
    file(DOWNLOAD "https://raw.githubusercontent.com/xmake-io/xrepo-cmake/main/xrepo.cmake"
                  "${CMAKE_BINARY_DIR}/xrepo.cmake"
                  TLS_VERIFY ON)
endif()

set(XREPO_PACKAGE_VERBOSE ON)
set(XREPO_BOOTSTRAP_XMAKE ON)
set(XREPO_BUILD_PARALLEL_JOBS 12)
set(XMAKE_RELEASE_LATEST v2.7.2)

include(${CMAKE_BINARY_DIR}/xrepo.cmake)
if(NOT EXISTS "$ENV{HOME}/.xmake/repositories/my_private_repo/xmake.lua")
    message(STATUS "add my_private_repo")
    if(EXISTS ${CMAKE_BINARY_DIR}/xmake/install/bin/xmake)
        execute_process(COMMAND ${CMAKE_BINARY_DIR}/xmake/install/bin/xmake repo -a -g my_private_repo https://github.com/fantasy-peak/xmake-repo.git)
    else()
        execute_process(COMMAND xmake repo -a -g my_private_repo https://github.com/fantasy-peak/xmake-repo.git)
    endif()
endif()
#----------------------------------------------------------------------------------
xrepo_package("fmt")
xrepo_package("spdlog")
xrepo_package("folly" DEPS "conan::libiberty/9.1.0" "boost" "openssl" "libevent" "gflags" "glog" "fmt" "zlib" "double-conversion" "bzip2" "lz4" "zstd")
#-----------------------------------------------------------------------------------
include_directories(include)

add_executable(http-proxy
    src/main.cpp
)
xrepo_target_packages(http-proxy PUBLIC folly fmt spdlog NO_LINK_LIBRARIES)
target_link_libraries(http-proxy PUBLIC
    folly uv glog gflags double-conversion zstd lz4 event event_core event_extra iberty event_openssl event_pthreads fmt
    boost_context-mt boost_program_options-mt boost_system-mt boost_thread-mt boost_regex-mt boost_filesystem-mt boost_atomic-mt
    ssl crypto uuid pthread dl
)
