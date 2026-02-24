#if !defined(__BN3MONKEY_REMOTE_COMMAND_PROTOCOL__)
#define __BN3MONKEY_REMOTE_COMMAND_PROTOCOL__

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace Bn3Monkey
{
    enum class RemoteCommandInstruction : int32_t
    {
        INSTRUCTION_EMPTY = 0x0000,

        INSTRUCTION_CURRENT_WORKING_DIRECTORY = 0x10001000,
        INSTRUCTION_MOVE_CURRENT_WORKING_DIRECTORY = 0x10001001,
        INSTRUCTION_DIRECTORY_EXISTS = 0x10001002,
        INSTRUCTION_LIST_DIRECTORY_CONTENTS = 0x10001003,
        INSTRUCTION_CREATE_DIRECTORY = 0x10001004,
        INSTRUCTION_REMOVE_DIRECTORY = 0x10001005,
        INSTRUCTION_COPY_DIRECTORY = 0x10001006,
        INSTRUCTION_MOVE_DIRECTORY = 0x10001007,

        INSTRUCTION_RUN_COMMAND   = 0x10002000,
        INSTRUCTION_OPEN_PROCESS  = 0x10002001,
        INSTRUCTION_CLOSE_PROCESS = 0x10002002,

        INSTRUCTION_UPLOAD_FILE   = 0x10003000,
        INSTRUCTION_DOWNLOAD_FILE = 0x10003001,
    };

    constexpr static const char REMOTE_COMMAND_MAGIC[] {'R', 'M', 'T', '_' };
    
    struct RemoteCommandRequestHeader
    {
        char magic[sizeof(REMOTE_COMMAND_MAGIC)] {0};
        RemoteCommandInstruction instruction {RemoteCommandInstruction::INSTRUCTION_EMPTY};
        uint32_t payload_0_length {0};
        uint32_t payload_1_length {0};
        uint32_t payload_2_length {0};
        uint32_t payload_3_length {0};

        explicit RemoteCommandRequestHeader(RemoteCommandInstruction instruction, 
            uint32_t payload_0_length = 0,
            uint32_t payload_1_length = 0,
            uint32_t payload_2_length = 0,
            uint32_t payload_3_length = 0) :
            instruction(instruction),
            payload_0_length(payload_0_length),
            payload_1_length(payload_1_length),
            payload_2_length(payload_2_length),
            payload_3_length(payload_3_length)
        {
            memcpy(magic, REMOTE_COMMAND_MAGIC, sizeof(magic));
        }

        inline bool valid() {
            return strncmp(magic, REMOTE_COMMAND_MAGIC, sizeof(magic)) == 0;
        }
    };

    // RemoteCommandRequest
    // - RemoteCommandRequestHeader (24byte)
    //   - magic (4byte)
    //   - instruction (4byte)
    //   - payload_0_length  (4byte)
    //   - paylaod_1_length  (4byte)
    //   - paylaod_2_length  (4byte)
    //   - paylaod_3_length  (4byte)
    // - payload_0 (payload_0_length byte)
    // - payload_1 (payload_1_length byte)
    // - payload_2 (payload_2_length byte)
    // - payload_3 (payload_3_length byte)

    struct RemoteCommandResponseHeader
    {
        char magic[sizeof(REMOTE_COMMAND_MAGIC)] {0};
        RemoteCommandInstruction instruction {RemoteCommandInstruction::INSTRUCTION_EMPTY};
        uint32_t payload_length {0};
        uint32_t padding {0};

         explicit RemoteCommandResponseHeader(
            RemoteCommandInstruction instruction, 
            uint32_t payload_length = 0
          ) : instruction(instruction), payload_length(payload_length) {
            
            memcpy(magic, REMOTE_COMMAND_MAGIC, sizeof(magic));
         }
         inline bool valid() {
            return strncmp(magic, REMOTE_COMMAND_MAGIC, sizeof(magic)) == 0;
        }
    };

    // RemoteCommandResponse
    // - RemoteCommandResponseHeader (16byte)
    //   - magic (4byte)
    //   - instruction (4byte)
    //   - payload_length  (4byte)
    // - payload
    //   if (header.instruction == INSTRUCTION_CURRENT_WORKING_DIRECTORY)
    //      - string (payload_length byte)
    //   else if (header.instruction == INSTRUCTION_LIST_DIRECTORY_CONTENTS)
    //      - num_of_directory_contents (4byte)
    //      - directory_contents (num_of_directory_contents * sizeof(RemoteDirectoryContentInner))
    //   else if (header.instruction == INSTRUCTION_RUN_COMMAND)
    //      - Empty
    //   else
    //      - true, false (sizeof(bool) byte)

    enum class RemoteDirectoryContentTypeInner : int32_t {
        INVALID = 0x0000,
        FILE = 0x1000,
        DIRECTORY = 0x2000,
    };

    struct RemoteDirectoryContentInner {
        RemoteDirectoryContentTypeInner type {RemoteDirectoryContentTypeInner::INVALID};
        char name[128] {0};

        explicit RemoteDirectoryContentInner(RemoteDirectoryContentTypeInner type, const char* name) : type(type) {
            snprintf(this->name, sizeof(this->name), "%s", name);
        }
    };


    enum class RemoteCommandStreamType : int32_t {
        INVALID = 0x0000,
        STREAM_OUTPUT = 0x3000,
        STREAM_ERROR = 0x4000,
    };
    struct RemoteCommandStreamHeader
    {
        char magic[sizeof(REMOTE_COMMAND_MAGIC)] {0};
        RemoteCommandStreamType type { RemoteCommandStreamType::INVALID };
        uint32_t payload_length {0};
        uint32_t padding {0};

        explicit RemoteCommandStreamHeader(RemoteCommandStreamType type, uint32_t payload_length) :
            type(type), payload_length(payload_length) {
            memcpy(magic, REMOTE_COMMAND_MAGIC, sizeof(magic));
        }

        inline bool valid() {
            return strncmp(magic, REMOTE_COMMAND_MAGIC, sizeof(magic)) == 0;
        }
    };

    // RemoteCommandStream
    // - RemoteCommandStreamHeader (16byte)
    //    - magic (4byte)
    //    - type (4byte)
    //    - payload_size (4byte)
    //    - padding (4byte)
    // - payload (payload_size byte)

    static constexpr const char PORT_COMMAND[] {"RC_CMD"};
    static constexpr const char PORT_STREAM [] {"RC_STREAM"};
}
#endif // __BN3MONKEY_REMOTE_COMMAND_PROTOCOL__