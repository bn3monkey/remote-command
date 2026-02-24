cmake_minimum_required(VERSION 3.15)

project(remote-command C CXX)


include(FetchContent)
FetchContent_Declare(
    kiotty_discover
    GIT_REPOSITORY https://github.com/kiotty/kiotty-discovery
    GIT_TAG 1.0.0
)
FetchContent_MakeAvailable(kiotty_discover)

# ---------------------------------------------------------------------------
# remote_command_client  —  C++11 static library
#   Header-only public interface in include/
#   Consumer can FetchContent this repo and link against this target.
# ---------------------------------------------------------------------------
add_library(remote_command_client STATIC
    include/remote_command_client.hpp
    src/client/remote_command_client.cpp
    src/protocol/remote_command_protocol.hpp
)

target_include_directories(remote_command_client
    PUBLIC  include
    PRIVATE src
)

set_target_properties(remote_command_client PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(remote_command_client PRIVATE kiotty_discovery_client)
if(WIN32)
    target_link_libraries(remote_command_client PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------------------
# remote_command_server  —  C++17 static library
#   Uses std::filesystem; must be compiled with C++17 or later.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE SERVER_SOURCES
    src/server/*.cpp
    src/server/*.hpp)

add_library(remote_command_server STATIC
    include/remote_command_server.hpp
    src/server/remote_command_server.cpp
    ${SERVER_SOURCES}
)

target_include_directories(remote_command_server
    PUBLIC  include
    PRIVATE src
)

set_target_properties(remote_command_server PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(remote_command_server PRIVATE kiotty_discovery_server)
if(WIN32)
    target_link_libraries(remote_command_server PRIVATE ws2_32)
endif()

# GCC < 9 needs explicit linkage for std::filesystem
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(remote_command_server PRIVATE stdc++fs)
endif()

# ---------------------------------------------------------------------------
# Integration tests  (opt-in: cmake -DREMOTE_COMMAND_BUILD_TESTS=ON ...)
# ---------------------------------------------------------------------------
option(REMOTE_COMMAND_BUILD_TESTS "Build integration tests (requires network access)" ON)

if(REMOTE_COMMAND_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
    )
    # Prevent overriding the parent project's compiler/linker settings on Windows
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(integration_test
        test/integration.cpp
    )

    target_link_libraries(integration_test
        PRIVATE remote_command_client
        PRIVATE remote_command_server
        PRIVATE GTest::gtest_main
    )

    set_target_properties(integration_test PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    if(WIN32)
        target_link_libraries(integration_test PRIVATE ws2_32)
    endif()

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
        target_link_libraries(integration_test PRIVATE stdc++fs)
    endif()

    include(GoogleTest)
    gtest_discover_tests(integration_test)
endif()
