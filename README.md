# remote-command

A C++ library for sending commands to a remote process, transferring files, and receiving stdout/stderr streams in real time over two separate TCP sockets. Supports automatic server discovery via UDP.

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
  discoverRemoteCommandClient()
        │
        ├─── UDP broadcast ──────────▶ KiottyDiscovery (discovery port)
        │◀── server IP + ports ───────
        │
        ├─── command_sock (TCP) ──────▶ request / response  (synchronous)
        │
        └─── stream_sock  (TCP) ◀────── stdout / stderr     (asynchronous push)
```

The client locates the server via UDP discovery, then opens two TCP connections.
Requests are sent over the command socket and responses are received synchronously.
While the server executes a command, stdout and stderr are pushed asynchronously to the client over the stream socket.

If the server IP and ports are already known, you can skip discovery and connect directly.

---

## Architecture

| Component | Library | Role |
|-----------|---------|------|
| `remote_command_client` | static lib (C++11) | Send commands to a remote server, receive results |
| `remote_command_server` | static lib (C++17) | Receive commands, execute them, stream output |
| `remote_command_server_app` | executable | Stand-alone server binary (`prj/`) |
| `integration_test` | executable | Integration test suite (Google Test) |

**Dependencies**

| Library | Source | Used by |
|---------|--------|---------|
| [`kiotty_discover`](https://github.com/kiotty/kiotty-discovery) | FetchContent (tag `1.0.0`) | client + server |

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

### Server Discovery

| Function | Description |
|----------|-------------|
| `discoverRemoteCommandClient(discovery_port)` | Broadcast a UDP discovery request and wait for the server to respond. Returns a connected client. |
| `getRemoteCommandServerAddress(client)` | Return the IP address string of the connected server. |

- `discoverRemoteCommandClient` **blocks** until a response is received from the server.
- Discovery uses the [`kiotty_discover`](https://github.com/kiotty/kiotty-discovery) library.

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

### File Transfer

| Function | Description |
|----------|-------------|
| `uploadFile(client, local, remote)` | Send a local file to the server's filesystem |
| `downloadFile(client, local, remote)` | Receive a file from the server's filesystem |

- Both `local` and `remote` paths may be absolute or relative.
- Relative paths are resolved against the **server's current working directory** (remote) or the **client process's CWD** (local).
- `uploadFile` creates intermediate parent directories on the server automatically.
- Both functions transfer raw binary data; they are safe for any file type.

### Command Execution

| Function | Description |
|----------|-------------|
| `runCommand(client, fmt, ...)` | Execute a command; **blocks** until it completes |
| `runCommandImpl(client, cmd)` | Execute a raw command string; **blocks** until it completes |
| `openProcess(client, fmt, ...)` | Start a process in the background; returns immediately with a process ID |
| `closeProcess(client, process_id)` | Terminate a background process; **blocks** until fully cleaned up |

- `runCommand` / `runCommandImpl` stream stdout via `onRemoteOutput` and stderr via `onRemoteError` while blocking.
- `openProcess` also streams output via the same callbacks while the background process runs.
- Calling `closeProcess` with an invalid or already-closed ID is a safe no-op.
- If a client disconnects while background processes are still running, the server automatically kills and cleans them up.

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
- **Do not call command-socket functions concurrently from multiple threads** on the same client. The command socket is shared and not thread-safe; interleaved calls will corrupt the request/response stream.
- `openProcess` IO threads write to the stream socket concurrently with `runCommand` IO threads. The server serialises these writes internally with a mutex, so stream data is always well-formed. However, chunks from different processes may be interleaved.

### Stream Ordering

- stdout/stderr data travels over the **stream socket**; the command-completion response travels over the **command socket**.
- Because these are two independent TCP connections, a small amount of stream data may not yet have been delivered to the callbacks by the time `runCommand` or `closeProcess` returns.
- If exact stream delivery is required before proceeding, add a short delay.

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

The `kiotty_discover` dependency is fetched automatically by the root `CMakeLists.txt`; no manual step is required.

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

Usage example (with discovery):

```cpp
#include <remote_command_client.hpp>

static void on_out(const char* msg) { printf("%s", msg); }
static void on_err(const char* msg) { fprintf(stderr, "%s", msg); }

int main() {
    // Discover the server on the local network via UDP
    auto* client = Bn3Monkey::discoverRemoteCommandClient(9000);
    if (!client) return 1;

    printf("Connected to: %s\n", Bn3Monkey::getRemoteCommandServerAddress(client));

    Bn3Monkey::onRemoteOutput(client, on_out);
    Bn3Monkey::onRemoteError (client, on_err);

    // Directory operations
    const char* cwd = Bn3Monkey::currentWorkingDirectory(client);
    Bn3Monkey::createDirectory(client, "build");

    // File transfer
    Bn3Monkey::uploadFile(client, "local/config.json", "config.json");
    Bn3Monkey::downloadFile(client, "result.tar.gz", "artifacts/result.tar.gz");

    // Blocking command execution (stdout/stderr delivered via callbacks)
    Bn3Monkey::runCommand(client, "cmake -B build -S .");
    Bn3Monkey::runCommand(client, "cmake --build build -j%d", 4);

    // Non-blocking background process
    int32_t pid = Bn3Monkey::openProcess(client, "./server_app --port 8080");
    // ... do other work ...
    Bn3Monkey::closeProcess(client, pid);   // terminates and waits for cleanup

    Bn3Monkey::releaseRemoteCommandClient(client);
    return 0;
}
```

Usage example (direct connection, without discovery):

```cpp
// Connect directly if IP and ports are already known
auto* client = Bn3Monkey::createRemoteCommandClient(9001, 9002, "192.168.1.100");
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

# Run with default ports (discovery=9000, command=9001, stream=9002)
./prj/build/remote_command_server_app

# Specify ports and working directory
./prj/build/remote_command_server_app 9000 9001 9002 /home/user/workspace
```

```
Usage: remote_command_server_app [discovery_port] [command_port] [stream_port] [working_directory]
  discovery_port    : UDP discovery port                        (default: 9000)
  command_port      : request/response socket port             (default: 9001)
  stream_port       : stdout/stderr stream socket port         (default: 9002)
  working_directory : server initial working directory         (default: current directory)
```

The server accepts connections asynchronously in a background thread. A UDP discovery service runs in parallel so clients can locate the server automatically. When a client connects, its IP and port are printed. When it disconnects, any processes started with `openProcess` are automatically killed and cleaned up before the server waits for the next client.
It shuts down gracefully on `Ctrl+C` (SIGINT) or SIGTERM.

---

## Building the Integration Tests

The tests use Google Test and occupy **ports 19001, 19002, and 19003** on localhost.

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
| `Integration.uploadFile` | File content round-trips correctly; missing local file fails |
| `Integration.downloadFile` | File content round-trips correctly; missing remote file fails |
| `Integration.openProcess_and_closeProcess` | Long-running process is terminated cleanly; double-close is a no-op |
| `Integration.openProcess_output` | stdout from a short process is captured via the stream callback |

Each test's `SetUp` connects via `discoverRemoteCommandClient` and verifies the returned server IP is non-empty.

---

## API Reference

### Client lifecycle

```cpp
// Discover server via UDP and connect (blocks until a response is received)
RemoteCommandClient* discoverRemoteCommandClient(int32_t discovery_port);

// Connect directly when IP and ports are already known (ip defaults to "127.0.0.1")
RemoteCommandClient* createRemoteCommandClient(
    int32_t     command_port,
    int32_t     stream_port,
    const char* ip = "127.0.0.1");

void releaseRemoteCommandClient(RemoteCommandClient* client);

// Return the IP address of the connected server
const char* getRemoteCommandServerAddress(RemoteCommandClient* client);
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

### File transfer

```cpp
// Upload a local file to the server (creates parent dirs automatically)
bool uploadFile(RemoteCommandClient* client,
                const char* local_file,
                const char* remote_file);

// Download a file from the server to the local filesystem
bool downloadFile(RemoteCommandClient* client,
                  const char* local_file,
                  const char* remote_file);
```

Both functions return `true` on success, `false` on any error (file not found, I/O error, etc.).

### Command execution

```cpp
// Blocking: wait for the command to finish (printf-style format string)
template<typename... Args>
void runCommand(RemoteCommandClient* client, const char* fmt, Args... args);

// Blocking: wait for the command to finish (raw string)
void runCommandImpl(RemoteCommandClient* client, const char* cmd);

// Non-blocking: start a background process, return its ID (-1 on failure)
template<typename... Args>
int32_t openProcess(RemoteCommandClient* client, const char* fmt, Args... args);

// Blocking: terminate the background process and wait for full cleanup
void closeProcess(RemoteCommandClient* client, int32_t process_id);
```

### Server

```cpp
// Non-blocking: creates sockets, starts the accept loop and discovery service
// in background threads, and returns immediately.
RemoteCommandServer* openRemoteCommandServer(
    int32_t     discovery_port,
    int32_t     command_port,
    int32_t     stream_port,
    const char* current_working_directory = ".");

// Blocking: signals all background threads to stop and waits for them to join.
void closeRemoteCommandServer(RemoteCommandServer* server);
```

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
```

Payload layout per instruction:

| Instruction | Request payload | Response payload |
|-------------|----------------|-----------------|
| `CURRENT_WORKING_DIRECTORY` | — | path string |
| `MOVE_CURRENT_WORKING_DIRECTORY` | p0: new path | bool |
| `DIRECTORY_EXISTS` | p0: path | bool |
| `LIST_DIRECTORY_CONTENTS` | p0: path | uint32 count + `RemoteDirectoryContentInner[]` |
| `CREATE_DIRECTORY` | p0: path | bool |
| `REMOVE_DIRECTORY` | p0: path | bool |
| `COPY_DIRECTORY` | p0: from, p1: to | bool |
| `MOVE_DIRECTORY` | p0: from, p1: to | bool |
| `RUN_COMMAND` | p0: command string | — (0 bytes, signals completion) |
| `OPEN_PROCESS` | p0: command string | int32_t process ID (−1 on failure) |
| `CLOSE_PROCESS` | p0: int32_t process ID (binary) | — (0 bytes, signals cleanup done) |
| `UPLOAD_FILE` | p0: remote path, p1: file data (binary) | bool |
| `DOWNLOAD_FILE` | p0: remote path | `0x01` + file data on success; `0x00` on failure |

### Stream socket (server → client, unidirectional)

```
[RemoteCommandStreamHeader : 16 bytes]
  magic[4]          "RMT_"
  type[4]           STREAM_OUTPUT(0x3000) | STREAM_ERROR(0x4000)
  payload_length[4]
  padding[4]
[payload : payload_length bytes]  ← null-terminated string
```

Both `runCommand` and `openProcess` deliver output via this socket. The server uses a mutex to ensure that concurrent writes from multiple background processes do not corrupt individual stream packets.
