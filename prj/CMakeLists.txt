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
