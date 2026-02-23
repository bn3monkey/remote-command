#include <gtest/gtest.h>

#include "remote_command_client.hpp"
#include "remote_command_server.hpp"

#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <string>
#include <cstdio>

namespace fs = std::filesystem;
using namespace Bn3Monkey;

// ---------------------------------------------------------------------------
// Shared output capture (plain function pointers — no std::function)
// ---------------------------------------------------------------------------
static std::string  g_stdout_buf;
static std::string  g_stderr_buf;
static std::mutex   g_buf_mutex;

static void onOutput(const char* msg)
{
    std::printf("[STDOUT] %s", msg);
    std::fflush(stdout);
    std::lock_guard<std::mutex> lk(g_buf_mutex);
    g_stdout_buf += msg;
}

static void onError(const char* msg)
{
    std::printf("[STDERR] %s", msg);
    std::fflush(stdout);
    std::lock_guard<std::mutex> lk(g_buf_mutex);
    g_stderr_buf += msg;
}

// ---------------------------------------------------------------------------
// Test fixture
//
// openRemoteCommandServer() is now non-blocking: it binds/listens and starts
// an internal serverThread, then returns immediately.  SetUp just calls it
// directly, connects the client, and waits briefly for serverThread to accept.
// ---------------------------------------------------------------------------
class Integration : public ::testing::Test
{
protected:
    static constexpr int CMD_PORT = 19001;
    static constexpr int STR_PORT = 19002;

    RemoteCommandServer* server = nullptr;
    RemoteCommandClient* client = nullptr;
    fs::path             test_dir;

    void SetUp() override
    {
        // Fresh, empty working directory for each test
        test_dir = fs::temp_directory_path() / "rcs_integration_test";
        std::error_code ec;
        fs::remove_all(test_dir, ec);
        fs::create_directories(test_dir, ec);

        // Clear output capture
        {
            std::lock_guard<std::mutex> lk(g_buf_mutex);
            g_stdout_buf.clear();
            g_stderr_buf.clear();
        }

        // openRemoteCommandServer creates/binds/listens and returns immediately.
        // Sockets are already in LISTEN state when the call returns.
        server = openRemoteCommandServer(CMD_PORT, STR_PORT, test_dir.string().c_str());
        ASSERT_NE(server, nullptr) << "Failed to start server";

        // Connect client — the OS queues the connection in the listen backlog,
        // so no sleep is needed before connecting.
        client = createRemoteCommandContext(CMD_PORT, STR_PORT);
        ASSERT_NE(client, nullptr) << "Client failed to connect to server";

        // Give serverThread time to accept() the queued connection and enter
        // handleRequests() before the test body starts issuing commands.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Register callbacks so the tester can see live output
        onRemoteOutput(client, onOutput);
        onRemoteError(client, onError);
    }

    void TearDown() override
    {
        // Release client first — closes its sockets, causing handleRequests()
        // to exit (recv returns 0), and the serverThread loops back to accept.
        if (client) {
            releaseRemoteCommandContext(client);
            client = nullptr;
        }

        // closeRemoteCommandServer sets running=false, closes any active client
        // sockets, and joins the internal serverThread before returning.
        if (server) {
            closeRemoteCommandServer(server);
            server = nullptr;
        }

        // Remove temp dir (best-effort)
        std::error_code ec;
        fs::remove_all(test_dir, ec);
    }

    // Helper — wait for the stream thread to drain any in-flight data
    static void flushStream()
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
};

// ===========================================================================
// Tests
// ===========================================================================

// ---------------------------------------------------------------------------
TEST_F(Integration, currentWorkingDirectory)
{
    const char* cwd = currentWorkingDirectory(client);
    ASSERT_NE(cwd, nullptr);

    std::error_code ec;
    fs::path returned  = fs::path(cwd);
    fs::path expected  = fs::canonical(test_dir, ec);

    std::printf("  Returned CWD : %s\n", cwd);
    std::printf("  Expected CWD : %s\n", expected.string().c_str());

    EXPECT_EQ(returned, expected);
}

// ---------------------------------------------------------------------------
TEST_F(Integration, moveWorkingDirectory)
{
    // Pre-create a sub-directory so the server can navigate into it
    fs::create_directory(test_dir / "subdir");

    // Move into subdir
    bool ok = moveWorkingDirectory(client, "subdir");
    EXPECT_TRUE(ok) << "moveWorkingDirectory into 'subdir' should succeed";

    const char* cwd = currentWorkingDirectory(client);
    ASSERT_NE(cwd, nullptr);
    std::printf("  New CWD : %s\n", cwd);
    EXPECT_NE(std::string(cwd).find("subdir"), std::string::npos)
        << "CWD should contain 'subdir'";

    // Try navigating into a non-existent path
    bool fail = moveWorkingDirectory(client, "does_not_exist");
    EXPECT_FALSE(fail) << "moveWorkingDirectory into non-existent dir should fail";
}

// ---------------------------------------------------------------------------
TEST_F(Integration, directoryExists)
{
    // The test_dir itself exists — '.' should be true
    EXPECT_TRUE(directoryExists(client, "."));

    // Create a directory on the server side and check
    fs::create_directory(test_dir / "present");
    EXPECT_TRUE(directoryExists(client, "present"));

    // Should not exist
    EXPECT_FALSE(directoryExists(client, "absent"));
}

// ---------------------------------------------------------------------------
TEST_F(Integration, listDirectoryContents)
{
    // Pre-populate: 2 directories + 2 files
    fs::create_directory(test_dir / "dir_a");
    fs::create_directory(test_dir / "dir_b");
    std::ofstream(test_dir / "file_a.txt") << "hello";
    std::ofstream(test_dir / "file_b.txt") << "world";

    auto contents = listDirectoryContents(client, ".");
    ASSERT_EQ(contents.size(), 4u) << "Expected 4 entries (2 dirs + 2 files)";

    int dirs = 0, files = 0;
    auto hasItem = [&](const char* name) {
        for (const auto& c : contents)
            if (std::string(c.name) == name) return true;
        return false;
    };

    for (const auto& item : contents) {
        std::printf("  [%s] %s\n",
            item.type == RemoteDirectoryContentType::DIRECTORY ? "DIR " : "FILE",
            item.name);
        if (item.type == RemoteDirectoryContentType::DIRECTORY) ++dirs;
        else                                                     ++files;
    }

    EXPECT_EQ(dirs,  2);
    EXPECT_EQ(files, 2);
    EXPECT_TRUE(hasItem("dir_a"));
    EXPECT_TRUE(hasItem("dir_b"));
    EXPECT_TRUE(hasItem("file_a.txt"));
    EXPECT_TRUE(hasItem("file_b.txt"));
}

// ---------------------------------------------------------------------------
TEST_F(Integration, createDirectory)
{
    // Flat directory
    bool ok = createDirectory(client, "brand_new");
    EXPECT_TRUE(ok);
    EXPECT_TRUE(fs::is_directory(test_dir / "brand_new"));

    // Deeply nested (create_directories semantics)
    bool ok2 = createDirectory(client, "nested/deep/path");
    EXPECT_TRUE(ok2);
    EXPECT_TRUE(fs::is_directory(test_dir / "nested" / "deep" / "path"));
}

// ---------------------------------------------------------------------------
TEST_F(Integration, removeDirectory)
{
    // Create directory with content inside
    fs::path target = test_dir / "to_remove";
    fs::create_directory(target);
    std::ofstream(target / "inside.txt") << "data";

    ASSERT_TRUE(fs::exists(target));

    bool ok = removeDirectory(client, "to_remove");
    EXPECT_TRUE(ok);
    EXPECT_FALSE(fs::exists(target)) << "Directory should be gone after removeDirectory";
}

// ---------------------------------------------------------------------------
TEST_F(Integration, copyDirectory)
{
    // Source with one file
    fs::path src = test_dir / "copy_src";
    fs::create_directory(src);
    std::ofstream(src / "data.txt") << "copy_content";

    bool ok = copyDirectory(client, "copy_src", "copy_dst");
    EXPECT_TRUE(ok);

    EXPECT_TRUE(fs::exists(src))                          << "Source should still exist";
    EXPECT_TRUE(fs::is_directory(test_dir / "copy_dst"))  << "Destination directory should exist";
    EXPECT_TRUE(fs::exists(test_dir / "copy_dst" / "data.txt"))
        << "Destination file should be copied";
}

// ---------------------------------------------------------------------------
TEST_F(Integration, moveDirectory)
{
    // Source with one file
    fs::path src = test_dir / "move_src";
    fs::create_directory(src);
    std::ofstream(src / "stuff.txt") << "move_content";

    bool ok = moveDirectory(client, "move_src", "move_dst");
    EXPECT_TRUE(ok);

    EXPECT_FALSE(fs::exists(src))                         << "Source should be gone";
    EXPECT_TRUE(fs::is_directory(test_dir / "move_dst"))  << "Destination directory should exist";
    EXPECT_TRUE(fs::exists(test_dir / "move_dst" / "stuff.txt"))
        << "Destination file should exist";
}

// ---------------------------------------------------------------------------
TEST_F(Integration, runCommand)
{
    // ---- 1. stdout capture ----
    runCommandImpl(client, "echo remote_hello");
    flushStream();   // let stream thread deliver the packet

    {
        std::lock_guard<std::mutex> lk(g_buf_mutex);
        std::printf("  Captured stdout : [%s]\n", g_stdout_buf.c_str());
        EXPECT_NE(g_stdout_buf.find("remote_hello"), std::string::npos)
            << "stdout should contain 'remote_hello'";
        g_stdout_buf.clear();
    }

    // ---- 2. file creation via command ----
    runCommandImpl(client, "echo created_by_cmd > cmd_output.txt");
    flushStream();

    EXPECT_TRUE(fs::exists(test_dir / "cmd_output.txt"))
        << "Command should have created cmd_output.txt in server CWD";

    // ---- 3. stderr capture (intentionally bad command) ----
    {
        std::lock_guard<std::mutex> lk(g_buf_mutex);
        g_stderr_buf.clear();
    }

#ifdef _WIN32
    runCommandImpl(client, "nonexistent_cmd_xyz 2>&1");
#else
    runCommandImpl(client, "nonexistent_cmd_xyz_abc_123");
#endif
    flushStream();

    {
        std::lock_guard<std::mutex> lk(g_buf_mutex);
        std::printf("  Captured stderr : [%s]\n", g_stderr_buf.c_str());
        // Just log it — exact wording is platform-specific, but something should arrive
        EXPECT_FALSE(g_stdout_buf.empty() && g_stderr_buf.empty())
            << "Expected some output from a bad command";
    }
}
