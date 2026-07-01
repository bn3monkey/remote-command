cmake_minimum_required(VERSION 3.15)

project(remote-command-app C CXX)

# Pull in the two libraries from the root
add_subdirectory(.. "${CMAKE_CURRENT_BINARY_DIR}/remote-command-source")

# Server executable
add_executable(remote_command_server_app
    main.cpp
)

target_link_libraries(remote_command_server_app
    PRIVATE remote_command_server
)

set_target_properties(remote_command_server_app PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# ---------------------------------------------------------------------------
# Fully static link the server binary on Linux (GCC/Clang).
#
# A dynamically linked binary carries versioned symbol requirements from the
# build host's glibc/libstdc++ (e.g. GLIBC_2.34 on ubuntu-22.04) and fails on
# older servers with "version `GLIBC_2.3x' not found". Linking statically bakes
# glibc, libstdc++ and libgcc into the executable so it has no such runtime
# dependency and runs on much older Linux distributions.
option(REMOTE_COMMAND_STATIC "Fully static-link the server binary (Linux)" ON)
if(REMOTE_COMMAND_STATIC AND UNIX AND NOT APPLE AND
   (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
    target_link_options(remote_command_server_app PRIVATE
        -static -static-libgcc -static-libstdc++)
endif()
