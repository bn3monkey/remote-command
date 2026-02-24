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
#include <vector>

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
    static constexpr int DISC_PORT = 19003;
    static constexpr int CMD_PORT  = 19001;
    static constexpr int STR_PORT  = 19002;

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
        server = openRemoteCommandServer(DISC_PORT, CMD_PORT, STR_PORT, test_dir.string().c_str());
        ASSERT_NE(server, nullptr) << "Failed to start server";

        // Connect client via UDP discovery — discoverRemoteCommandContext blocks
        // until it receives a response from the server's discovery service.
        client = discoverRemoteCommandClient(DISC_PORT);
        ASSERT_NE(client, nullptr) << "Client failed to discover/connect to server";

        // Verify the discovered server address is valid
        const char* server_ip = getRemoteCommandServerAddress(client);
        ASSERT_NE(server_ip, nullptr) << "Server IP should not be null";
        ASSERT_NE(std::string(server_ip), "") << "Server IP should not be empty";
        std::printf("  Discovered server IP : %s\n", server_ip);
        std::fflush(stdout);

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
            releaseRemoteCommandClient(client);
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

// ---------------------------------------------------------------------------
TEST_F(Integration, uploadFile)
{
    // Prepare a local file with known binary content
    fs::path local_src = fs::temp_directory_path() / "rcs_upload_src.bin";
    const std::string content = "Hello, Remote Server!\nLine two.\n";
    {
        std::ofstream f(local_src, std::ios::binary);
        f << content;
    }

    bool ok = uploadFile(client, local_src.string().c_str(), "uploaded.bin");
    EXPECT_TRUE(ok) << "uploadFile should succeed";

    // Verify the file appeared in the server's working directory
    fs::path remote = test_dir / "uploaded.bin";
    EXPECT_TRUE(fs::exists(remote)) << "uploaded.bin should exist on server side";

    {
        std::ifstream f(remote, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        EXPECT_EQ(got, content) << "File contents should match";
    }

    // Uploading a non-existent local file should fail
    bool fail = uploadFile(client, "/nonexistent_local_file_xyz.bin", "fail.bin");
    EXPECT_FALSE(fail) << "uploadFile with missing local file should fail";

    // Cleanup
    std::error_code ec;
    fs::remove(local_src, ec);
}

// ---------------------------------------------------------------------------
TEST_F(Integration, downloadFile)
{
    // Create a file on the server side
    const std::string content = "Hello, Local Client!\nBinary \x01\x02\x03 data.\n";
    {
        std::ofstream f(test_dir / "server_data.bin", std::ios::binary);
        f << content;
    }

    fs::path local_dst = fs::temp_directory_path() / "rcs_download_dst.bin";
    {
        std::error_code ec;
        fs::remove(local_dst, ec); // ensure clean slate
    }

    bool ok = downloadFile(client, local_dst.string().c_str(), "server_data.bin");
    EXPECT_TRUE(ok) << "downloadFile should succeed";
    EXPECT_TRUE(fs::exists(local_dst)) << "downloaded file should exist locally";

    {
        std::ifstream f(local_dst, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        EXPECT_EQ(got, content) << "Downloaded content should match server file";
    }

    // Downloading a non-existent remote file should fail
    bool fail = downloadFile(client, local_dst.string().c_str(),
                             "nonexistent_remote.bin");
    EXPECT_FALSE(fail) << "downloadFile for missing remote file should fail";

    // Cleanup
    std::error_code ec;
    fs::remove(local_dst, ec);
}

// ---------------------------------------------------------------------------
TEST_F(Integration, openProcess_and_closeProcess)
{
    // Start a long-running process that will NOT finish during the test
#ifdef _WIN32
    int32_t pid = openProcess(client, "ping -n 20 127.0.0.1");
#else
    int32_t pid = openProcess(client, "sleep 5");
#endif
    EXPECT_GT(pid, 0) << "openProcess should return a positive ID";

    // Let it run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // closeProcess should terminate it and block until cleanup is done
    closeProcess(client, pid);

    // Calling closeProcess on an already-closed or unknown ID must be a no-op
    closeProcess(client, pid);  // second call on same ID
    closeProcess(client, -1);   // invalid ID
}

// ---------------------------------------------------------------------------
TEST_F(Integration, openProcess_output)
{
    {
        std::lock_guard<std::mutex> lk(g_buf_mutex);
        g_stdout_buf.clear();
        g_stderr_buf.clear();
    }

    // Start a quick process that prints one line then exits
    int32_t pid = openProcess(client, "echo hello_from_openprocess");
    EXPECT_GT(pid, 0) << "openProcess should return a positive ID";

    // Wait long enough for the process to finish and stream to deliver
    flushStream();

    // closeProcess is graceful even if the process already exited
    closeProcess(client, pid);

    {
        std::lock_guard<std::mutex> lk(g_buf_mutex);
        std::printf("  Captured stdout: [%s]\n", g_stdout_buf.c_str());
        EXPECT_NE(g_stdout_buf.find("hello_from_openprocess"), std::string::npos)
            << "stdout should contain 'hello_from_openprocess'";
    }
}
