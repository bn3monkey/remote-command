# remote-command

A C++ library for sending commands to a remote process and receiving stdout/stderr streams in real time over two separate TCP sockets.

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Features](#features)
- [Constraints](#constraints)
- [Supported Platforms & Compilers](#supported-platforms--compilers)
- [CMake Integration (FetchContent)](#cmake-integration-fetchcontent)
- [Building the Server Binary](#building-the-server-binary)
- [Building the Integration Tests](#building-the-integration-tests)
- [API Reference](#api-reference)
- [Protocol](#protocol)

---

## Overview

```
[Client Process]                      [Server Process]
                                       (remote or local)
  createRemoteCommandContext()
        │
        ├─── command_sock (TCP) ──────▶ request / response  (synchronous)
        │
        └─── stream_sock  (TCP) ◀────── stdout / stderr     (asynchronous push)
```

The client sends requests over the command socket and waits synchronously for responses.
While the server executes a command, stdout and stderr are pushed asynchronously to the client over the stream socket.

---

## Architecture

| Component | Library | Role |
|-----------|---------|------|
| `remote_command_client` | static lib (C++11) | Send commands to a remote server, receive results |
| `remote_command_server` | static lib (C++17) | Receive commands, execute them, stream output |
| `remote_command_server_app` | executable | Stand-alone server binary (`prj/`) |
| `integration_test` | executable | Integration test suite (Google Test) |

**Source tree**

```
remote-command/
├── include/
│   ├── remote_command_client.hpp   # Public client API (C++11 compatible)
│   └── remote_command_server.hpp   # Public server API
├── src/
│   ├── protocol/
│   │   └── remote_command_protocol.hpp  # Shared binary protocol definitions
│   ├── client/
│   │   └── remote_command_client.cpp
│   └── server/
│       └── remote_command_server.cpp
├── prj/
│   ├── CMakeLists.txt   # Entry point for the server binary build
│   └── main.cpp
├── test/
│   └── integration.cpp
└── CMakeLists.txt       # Library targets + test definition
```

---

## Features

### Directory Operations

| Function | Description |
|----------|-------------|
| `currentWorkingDirectory(client)` | Return the server's current working directory path |
| `moveWorkingDirectory(client, path)` | Change the server's working directory |
| `directoryExists(client, path)` | Check whether a directory exists |
| `listDirectoryContents(client, path)` | List files and subdirectories |
| `createDirectory(client, path)` | Create a directory (including nested paths) |
| `removeDirectory(client, path)` | Recursively remove a directory |
| `copyDirectory(client, from, to)` | Recursively copy a directory |
| `moveDirectory(client, from, to)` | Move or rename a directory |

### Command Execution

| Function | Description |
|----------|-------------|
| `runCommand(client, fmt, ...)` | Execute a command with printf-style format string |
| `runCommandImpl(client, cmd)` | Execute a raw command string |

- Both functions **block until the command completes**.
- stdout is delivered asynchronously via the `onRemoteOutput` callback; stderr via `onRemoteError`.

### Registering Callbacks

```cpp
Bn3Monkey::onRemoteOutput(client, [](const char* msg) {
    printf("[OUT] %s", msg);
});
Bn3Monkey::onRemoteError(client, [](const char* msg) {
    fprintf(stderr, "[ERR] %s", msg);
});
```

> **Note:** Callbacks are plain function pointers (`void(*)(const char*)`).
> Lambdas are supported only if they have **no captures**.

---

## Constraints

### Concurrency

- **The client–server connection is 1-to-1.** The server handles one client at a time.
- **Do not call `runCommand` concurrently from multiple threads** on the same client. The command socket is shared, so interleaved calls will corrupt the request/response stream.
- Directory operation functions (`createDirectory`, etc.) also assume sequential, single-threaded calls.

### Stream Ordering

- stdout/stderr data travels over the **stream socket**; the command-completion response travels over the **command socket**.
- Because these are two independent TCP connections, a small amount of stream data may not yet have been delivered to the callbacks by the time `runCommand` returns.
- If exact stream delivery is required before proceeding, add a short delay after `runCommand`.

### Paths

- Relative paths are resolved against the **server's current working directory**.
- `moveWorkingDirectory` updates only the server's internal (virtual) CWD; it does **not** change the actual OS working directory of the server process.

### Command Execution Environment

| Platform | Shell | Execution method |
|----------|-------|-----------------|
| Linux / macOS | `/bin/sh -c "..."` | `fork` + `exec` |
| Windows | `cmd /c "..."` | `CreateProcess` |

### Network

- Only **IPv4** is currently supported.
- There is **no encryption (TLS) or authentication**. Use only on trusted networks.

---

## Supported Platforms & Compilers

### Client library (`remote_command_client`)

Requires C++11 or later.

| Platform | Compiler | Minimum standard |
|----------|----------|-----------------|
| Linux | GCC 4.8+, Clang 3.4+ | C++11 |
| macOS | Apple Clang 6.0+ | C++11 |
| Windows | MSVC 2015+, MinGW-w64 GCC 4.8+ | C++11 |

### Server library (`remote_command_server`)

Requires C++17 or later due to `std::filesystem`.

| Platform | Compiler | Minimum standard | Notes |
|----------|----------|-----------------|-------|
| Linux | GCC 8+ | C++17 | GCC 8 requires `-lstdc++fs`; GCC 9+ links automatically |
| macOS | Apple Clang 11+ (Xcode 11+) | C++17 | macOS 10.15+ |
| Windows | MSVC 2017 15.7+, MinGW-w64 GCC 8+ | C++17 | |

> **macOS 10.14 and earlier:** `std::filesystem` is not available; the server library cannot be built.

---

## CMake Integration (FetchContent)

### Client only

Use this when you only need to send commands to a remote server.

```cmake
include(FetchContent)

FetchContent_Declare(
    remote_command
    GIT_REPOSITORY https://github.com/your-org/remote-command.git
    GIT_TAG        main   # or a specific tag / commit hash
)

# Do not build the test binary
set(REMOTE_COMMAND_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(remote_command)

target_link_libraries(your_target
    PRIVATE remote_command_client
)
```

Usage example:

```cpp
#include <remote_command_client.hpp>

int main() {
    auto* client = Bn3Monkey::createRemoteCommandContext(9001, 9002, "192.168.1.100");
    if (!client) return 1;

    Bn3Monkey::onRemoteOutput(client, [](const char* msg) { printf("%s", msg); });
    Bn3Monkey::onRemoteError (client, [](const char* msg) { fprintf(stderr, "%s", msg); });

    // Directory operations
    const char* cwd = Bn3Monkey::currentWorkingDirectory(client);
    Bn3Monkey::createDirectory(client, "build");

    // Command execution (blocks until complete)
    Bn3Monkey::runCommand(client, "cmake -B build -S .");
    Bn3Monkey::runCommand(client, "cmake --build build -j%d", 4);

    Bn3Monkey::releaseRemoteCommandContext(client);
    return 0;
}
```

### Client + Server

Link against `remote_command_server` when the server needs to be embedded in your project.
**The consuming project must be compiled as C++17 or later.**

```cmake
FetchContent_MakeAvailable(remote_command)

target_link_libraries(your_server_target
    PRIVATE remote_command_server
)

set_target_properties(your_server_target PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
```

---

## Building the Server Binary

The `prj/` CMakeLists.txt includes the root via `add_subdirectory` and builds the server executable.

```bash
# Build from prj/ as the source root
cmake -S prj -B prj/build
cmake --build prj/build

# Run with default ports (9001/9002) and current directory as CWD
./prj/build/remote_command_server_app

# Specify ports and working directory
./prj/build/remote_command_server_app 9001 9002 /home/user/workspace
```

```
Usage: remote_command_server_app [command_port] [stream_port] [working_directory]
  command_port      : request/response socket port     (default: 9001)
  stream_port       : stdout/stderr stream socket port (default: 9002)
  working_directory : server initial working directory (default: current directory)
```

The server waits for a client to connect, serves it, and then waits for the next client once it disconnects.
It shuts down gracefully on `Ctrl+C` (SIGINT) or SIGTERM.

---

## Building the Integration Tests

The tests use Google Test and occupy **ports 19001 and 19002** on localhost.

```bash
# Build with tests enabled (from the repository root)
cmake -S . -B build -DREMOTE_COMMAND_BUILD_TESTS=ON
cmake --build build

# Run directly
./build/integration_test

# Or via ctest
cd build && ctest --output-on-failure
```

| Test name | What is verified |
|-----------|-----------------|
| `Integration.currentWorkingDirectory` | Returned path matches the server's initial CWD |
| `Integration.moveWorkingDirectory` | Success/failure of navigation and CWD change |
| `Integration.directoryExists` | Correct detection of existing vs. absent directories |
| `Integration.listDirectoryContents` | Entry count and names match pre-created files/dirs |
| `Integration.createDirectory` | Flat and nested paths verified via filesystem directly |
| `Integration.removeDirectory` | Absence of directory verified via filesystem directly |
| `Integration.copyDirectory` | Source intact + destination and its contents exist |
| `Integration.moveDirectory` | Source gone + destination and its contents exist |
| `Integration.runCommand` | stdout captured, file creation verified, stderr logged |

---

## API Reference

### Client lifecycle

```cpp
// ip defaults to "127.0.0.1"
RemoteCommandClient* createRemoteCommandContext(
    int32_t     command_port,
    int32_t     stream_port,
    const char* ip = "127.0.0.1");

void releaseRemoteCommandContext(RemoteCommandClient* client);
```

### Callbacks

```cpp
using OnRemoteOutput = void (*)(const char*);
using OnRemoteError  = void (*)(const char*);

void onRemoteOutput(RemoteCommandClient* client, OnRemoteOutput handler);
void onRemoteError (RemoteCommandClient* client, OnRemoteError  handler);
```

### Directory operations

```cpp
const char* currentWorkingDirectory(RemoteCommandClient* client);
bool moveWorkingDirectory  (RemoteCommandClient* client, const char* path);
bool directoryExists       (RemoteCommandClient* client, const char* path);

std::vector<RemoteDirectoryContent>
     listDirectoryContents (RemoteCommandClient* client, const char* path = ".");

bool createDirectory(RemoteCommandClient* client, const char* path);
bool removeDirectory(RemoteCommandClient* client, const char* path);
bool copyDirectory  (RemoteCommandClient* client, const char* from, const char* to);
bool moveDirectory  (RemoteCommandClient* client, const char* from, const char* to);
```

`RemoteDirectoryContent` struct:

```cpp
enum class RemoteDirectoryContentType { FILE, DIRECTORY };

struct RemoteDirectoryContent {
    RemoteDirectoryContentType type;
    char name[128];
};
```

### Command execution

```cpp
// printf-style format string (C++11 variadic template)
template<typename... Args>
void runCommand(RemoteCommandClient* client, const char* fmt, Args... args);

// Raw command string
void runCommandImpl(RemoteCommandClient* client, const char* cmd);
```

### Server

```cpp
// current_working_directory defaults to "." (process CWD)
RemoteCommandServer* openRemoteCommandServer(
    int32_t     command_port,
    int32_t     stream_port,
    const char* current_working_directory = ".");

void closeRemoteCommandServer(RemoteCommandServer* server);
```

`openRemoteCommandServer` **blocks** until a client connects on both sockets.

---

## Protocol

The binary protocol is defined in `src/protocol/remote_command_protocol.hpp`.

### Command socket (request / response)

```
Request:
  [RemoteCommandRequestHeader : 24 bytes]
    magic[4]            "RMT_"
    instruction[4]      RemoteCommandInstruction enum
    payload_0_length[4]
    payload_1_length[4]
    payload_2_length[4]
    payload_3_length[4]
  [payload_0 : payload_0_length bytes]
  [payload_1 : payload_1_length bytes]
  ...

Response:
  [RemoteCommandResponseHeader : 16 bytes]
    magic[4]            "RMT_"
    instruction[4]      echoed instruction
    payload_length[4]
    padding[4]
  [payload : payload_length bytes]
    CURRENT_WORKING_DIRECTORY  → path string
    LIST_DIRECTORY_CONTENTS    → uint32 count + RemoteDirectoryContentInner[]
    RUN_COMMAND                → (empty, 0 bytes)
    all others                 → bool (1 byte)
```

### Stream socket (server → client, unidirectional)

```
[RemoteCommandStreamHeader : 16 bytes]
  magic[4]          "RMT_"
  type[4]           STREAM_OUTPUT(0x3000) | STREAM_ERROR(0x4000)
  payload_length[4]
  padding[4]
[payload : payload_length bytes]  ← null-terminated string
```
