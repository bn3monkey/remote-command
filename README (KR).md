# remote-command

TCP 소켓 두 개를 이용해 원격 프로세스에 명령을 내리고, stdout/stderr 스트림을 실시간으로 수신하는 C++ 라이브러리입니다.

---

## 목차

- [개요](#개요)
- [아키텍처](#아키텍처)
- [기능](#기능)
- [제약 조건](#제약-조건)
- [지원 플랫폼 및 컴파일러](#지원-플랫폼-및-컴파일러)
- [CMake 통합 (FetchContent)](#cmake-통합-fetchcontent)
- [서버 바이너리 빌드](#서버-바이너리-빌드)
- [통합 테스트 빌드](#통합-테스트-빌드)
- [API 레퍼런스](#api-레퍼런스)
- [프로토콜](#프로토콜)

---

## 개요

```
[Client Process]                      [Server Process]
                                       (원격 또는 로컬)
  createRemoteCommandContext()
        │
        ├─── command_sock (TCP) ──────▶ 요청 / 응답 (동기)
        │
        └─── stream_sock  (TCP) ◀────── stdout / stderr 스트리밍 (비동기)
```

클라이언트는 command 소켓으로 요청을 보내고 응답을 동기적으로 기다립니다.
서버가 명령을 실행하는 동안 stdout/stderr는 stream 소켓을 통해 비동기적으로 클라이언트에 push됩니다.

---

## 아키텍처

| 구성 요소 | 라이브러리 | 역할 |
|----------|-----------|------|
| `remote_command_client` | static lib (C++11) | 원격 서버에 명령 전송, 결과 수신 |
| `remote_command_server` | static lib (C++17) | 명령 수신, 실행, 스트리밍 |
| `remote_command_server_app` | executable | 서버 바이너리 (prj/) |
| `integration_test` | executable | 통합 테스트 (gtest) |

**소스 트리**

```
remote-command/
├── include/
│   ├── remote_command_client.hpp   # 클라이언트 공개 API (C++11 호환)
│   └── remote_command_server.hpp   # 서버 공개 API
├── src/
│   ├── protocol/
│   │   └── remote_command_protocol.hpp  # 공유 이진 프로토콜 정의
│   ├── client/
│   │   └── remote_command_client.cpp
│   └── server/
│       └── remote_command_server.cpp
├── prj/
│   ├── CMakeLists.txt   # 서버 바이너리 빌드 진입점
│   └── main.cpp
├── test/
│   └── integration.cpp
└── CMakeLists.txt       # 라이브러리 빌드 + 테스트 정의
```

---

## 기능

### 디렉터리 조작

| 함수 | 설명 |
|------|------|
| `currentWorkingDirectory(client)` | 서버의 현재 작업 디렉터리 경로 반환 |
| `moveWorkingDirectory(client, path)` | 서버의 작업 디렉터리 이동 |
| `directoryExists(client, path)` | 디렉터리 존재 여부 확인 |
| `listDirectoryContents(client, path)` | 디렉터리 내 파일/폴더 목록 반환 |
| `createDirectory(client, path)` | 디렉터리 생성 (중첩 경로 포함) |
| `removeDirectory(client, path)` | 디렉터리 및 하위 항목 삭제 |
| `copyDirectory(client, from, to)` | 디렉터리 재귀 복사 |
| `moveDirectory(client, from, to)` | 디렉터리 이동/이름 변경 |

### 명령 실행

| 함수 | 설명 |
|------|------|
| `runCommand(client, fmt, ...)` | printf 형식 포맷 문자열로 명령 실행 |
| `runCommandImpl(client, cmd)` | 명령 문자열 직접 실행 |

- `runCommand` / `runCommandImpl` 은 **명령 완료까지 블로킹**합니다.
- 실행 중 stdout은 `onRemoteOutput` 콜백으로, stderr는 `onRemoteError` 콜백으로 비동기 전달됩니다.

### 콜백 등록

```cpp
Bn3Monkey::onRemoteOutput(client, [](const char* msg) {
    printf("[OUT] %s", msg);
});
Bn3Monkey::onRemoteError(client, [](const char* msg) {
    fprintf(stderr, "[ERR] %s", msg);
});
```

> **주의:** 콜백은 일반 함수 포인터(`void(*)(const char*)`)입니다. 람다를 사용하려면 캡처가 없는 람다만 가능합니다.

---

## 제약 조건

### 동시성

- **클라이언트 ↔ 서버 연결은 1:1입니다.** 서버는 한 번에 하나의 클라이언트만 처리합니다.
- 동일 클라이언트에서 **`runCommand`를 동시에 여러 스레드에서 호출하면 안 됩니다.** command 소켓이 공유되므로 요청/응답이 뒤섞입니다.
- 디렉터리 조작 함수들(`createDirectory` 등)도 마찬가지로 단일 스레드에서 순차 호출을 가정합니다.

### 스트림 순서 보장

- stdout/stderr 데이터는 **stream 소켓**, 명령 완료 응답은 **command 소켓**을 통해 전달됩니다.
- 두 TCP 연결은 독립적이므로, `runCommand` 반환 직후 일부 스트림 데이터가 클라이언트 측에서 아직 콜백으로 전달되지 않았을 수 있습니다.
- 정확한 스트림 수신을 보장하려면 `runCommand` 반환 후 짧은 지연이 필요할 수 있습니다.

### 경로

- 상대 경로는 **서버의 현재 작업 디렉터리** 기준으로 해석됩니다.
- `moveWorkingDirectory`는 서버 내부 상태(가상 CWD)만 변경하며, 서버 프로세스의 실제 OS CWD는 변경되지 않습니다.

### 명령 실행 환경

| 플랫폼 | 셸 | 명령 실행 방식 |
|--------|----|--------------|
| Linux / macOS | `/bin/sh -c "..."` | `fork` + `exec` |
| Windows | `cmd /c "..."` | `CreateProcess` |

### 네트워크

- 현재 IPv4만 지원합니다.
- 암호화(TLS) 및 인증 기능은 없습니다. **신뢰할 수 있는 네트워크 환경에서만 사용하세요.**

---

## 지원 플랫폼 및 컴파일러

### 클라이언트 라이브러리 (`remote_command_client`)

C++11 이상이면 사용 가능합니다.

| 플랫폼 | 컴파일러 | 최소 표준 |
|--------|---------|---------|
| Linux | GCC 4.8+, Clang 3.4+ | C++11 |
| macOS | Apple Clang 6.0+ | C++11 |
| Windows | MSVC 2015+, MinGW-w64 GCC 4.8+ | C++11 |

### 서버 라이브러리 (`remote_command_server`)

`std::filesystem` 사용으로 C++17 이상이 필요합니다.

| 플랫폼 | 컴파일러 | 최소 표준 | 비고 |
|--------|---------|---------|------|
| Linux | GCC 8+ | C++17 | GCC 8은 `-lstdc++fs` 필요; GCC 9+는 자동 링크 |
| macOS | Apple Clang 11+ (Xcode 11+) | C++17 | macOS 10.15+ |
| Windows | MSVC 2017 15.7+, MinGW-w64 GCC 8+ | C++17 | |

> **macOS 10.14 이하**: `std::filesystem`이 지원되지 않아 서버 라이브러리를 빌드할 수 없습니다.

---

## CMake 통합 (FetchContent)

### 클라이언트만 사용하는 경우

원격 서버에 명령을 보내는 클라이언트 코드에 통합할 때 사용합니다.

```cmake
include(FetchContent)

FetchContent_Declare(
    remote_command
    GIT_REPOSITORY https://github.com/your-org/remote-command.git
    GIT_TAG        main   # 또는 특정 태그/커밋 해시
)

# 테스트 바이너리는 빌드하지 않음
set(REMOTE_COMMAND_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(remote_command)

target_link_libraries(your_target
    PRIVATE remote_command_client
)
```

사용 예시:

```cpp
#include <remote_command_client.hpp>

int main() {
    auto* client = Bn3Monkey::createRemoteCommandContext(9001, 9002, "192.168.1.100");
    if (!client) return 1;

    Bn3Monkey::onRemoteOutput(client, [](const char* msg) { printf("%s", msg); });
    Bn3Monkey::onRemoteError (client, [](const char* msg) { fprintf(stderr, "%s", msg); });

    // 디렉터리 조작
    const char* cwd = Bn3Monkey::currentWorkingDirectory(client);
    Bn3Monkey::createDirectory(client, "build");

    // 명령 실행 (완료까지 블로킹)
    Bn3Monkey::runCommand(client, "cmake -B build -S .");
    Bn3Monkey::runCommand(client, "cmake --build build -j%d", 4);

    Bn3Monkey::releaseRemoteCommandContext(client);
    return 0;
}
```

### 서버도 함께 사용하는 경우

서버 기능이 필요한 경우 `remote_command_server` 타겟을 추가로 링크합니다.
단, **이 경우 소비자 프로젝트도 C++17 이상으로 컴파일해야 합니다.**

```cmake
FetchContent_MakeAvailable(remote_command)

# 서버를 내장하는 실행 파일
target_link_libraries(your_server_target
    PRIVATE remote_command_server
)

set_target_properties(your_server_target PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
```

---

## 서버 바이너리 빌드

`prj/` 디렉터리의 CMakeLists.txt가 루트를 `add_subdirectory`로 포함하여
서버 실행 파일을 빌드합니다.

```bash
# prj/ 를 루트로 빌드
cmake -S prj -B prj/build
cmake --build prj/build

# 실행 (기본 포트 9001/9002, 작업 디렉터리 현재 경로)
./prj/build/remote_command_server_app

# 포트와 작업 디렉터리 지정
./prj/build/remote_command_server_app 9001 9002 /home/user/workspace
```

```
사용법: remote_command_server_app [command_port] [stream_port] [working_directory]
  command_port      : 요청/응답 소켓 포트 (기본값: 9001)
  stream_port       : stdout/stderr 스트림 소켓 포트 (기본값: 9002)
  working_directory : 서버 초기 작업 디렉터리 (기본값: 현재 디렉터리)
```

서버는 클라이언트가 접속하면 처리를 시작하고, 클라이언트가 연결을 끊으면 다음 클라이언트를 기다립니다.
`Ctrl+C`(SIGINT) 또는 SIGTERM으로 정상 종료됩니다.

---

## 통합 테스트 빌드

테스트는 Google Test를 사용하며 **네트워크 포트 19001, 19002**를 사용합니다.

```bash
# 루트에서 테스트 포함 빌드
cmake -S . -B build -DREMOTE_COMMAND_BUILD_TESTS=ON
cmake --build build

# 실행
./build/integration_test

# 또는 ctest로 실행
cd build && ctest --output-on-failure
```

| 테스트 이름 | 검증 내용 |
|------------|---------|
| `Integration.currentWorkingDirectory` | 반환된 경로가 서버 CWD와 일치 |
| `Integration.moveWorkingDirectory` | 이동 성공/실패 및 CWD 변경 확인 |
| `Integration.directoryExists` | 존재/비존재 디렉터리 판별 |
| `Integration.listDirectoryContents` | 파일·디렉터리 목록 수 및 이름 검증 |
| `Integration.createDirectory` | 단순/중첩 경로 생성 후 filesystem 직접 확인 |
| `Integration.removeDirectory` | 삭제 후 filesystem 직접 확인 |
| `Integration.copyDirectory` | 원본 유지 + 사본 존재 확인 |
| `Integration.moveDirectory` | 원본 소멸 + 사본 존재 확인 |
| `Integration.runCommand` | stdout 캡처, 파일 생성, stderr 가시화 |

---

## API 레퍼런스

### 클라이언트 생성 / 해제

```cpp
// ip 기본값은 "127.0.0.1"
RemoteCommandClient* createRemoteCommandContext(
    int32_t command_port,
    int32_t stream_port,
    const char* ip = "127.0.0.1");

void releaseRemoteCommandContext(RemoteCommandClient* client);
```

### 콜백

```cpp
using OnRemoteOutput = void (*)(const char*);
using OnRemoteError  = void (*)(const char*);

void onRemoteOutput(RemoteCommandClient* client, OnRemoteOutput handler);
void onRemoteError (RemoteCommandClient* client, OnRemoteError  handler);
```

### 디렉터리 조작

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

`RemoteDirectoryContent` 구조체:

```cpp
enum class RemoteDirectoryContentType { FILE, DIRECTORY };

struct RemoteDirectoryContent {
    RemoteDirectoryContentType type;
    char name[128];
};
```

### 명령 실행

```cpp
// printf 형식 포맷 지원 (C++11 가변 인자 템플릿)
template<typename... Args>
void runCommand(RemoteCommandClient* client, const char* fmt, Args... args);

// 포맷 없이 직접 전달
void runCommandImpl(RemoteCommandClient* client, const char* cmd);
```

### 서버

```cpp
// current_working_directory 기본값은 "." (프로세스 CWD)
RemoteCommandServer* openRemoteCommandServer(
    int32_t command_port,
    int32_t stream_port,
    const char* current_working_directory = ".");

void closeRemoteCommandServer(RemoteCommandServer* server);
```

`openRemoteCommandServer`는 클라이언트의 접속을 수락할 때까지 **블로킹**합니다.

---

## 프로토콜

`src/protocol/remote_command_protocol.hpp`에 정의된 이진 프로토콜입니다.

### Command 소켓 (요청/응답)

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
    instruction[4]      반향(echo)된 instruction
    payload_length[4]
    padding[4]
  [payload : payload_length bytes]
    CURRENT_WORKING_DIRECTORY  → 경로 문자열
    LIST_DIRECTORY_CONTENTS    → uint32 count + RemoteDirectoryContentInner[]
    RUN_COMMAND                → (없음, 0 bytes)
    그 외                      → bool (1 byte)
```

### Stream 소켓 (서버 → 클라이언트 단방향)

```
[RemoteCommandStreamHeader : 16 bytes]
  magic[4]          "RMT_"
  type[4]           STREAM_OUTPUT(0x3000) | STREAM_ERROR(0x4000)
  payload_length[4]
  padding[4]
[payload : payload_length bytes]  ← null-terminated string
```
