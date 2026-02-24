#if !defined (__REMOTE_COMMAND_SERVER_HELPER__)
#define __REMOTE_COMMAND_SERVER_HELPER__

#include <thread>
#include <string>
#include <cstring>

#if defined(_WIN32)
    #include <windows.h>
#elif defined(__linux__)
    #include <pthread.h>
    #include <errno.h>
#endif

namespace Bn3Monkey
{
    inline void setCurrentThreadName(const char* name) noexcept
    {
    #if defined(_WIN32)

        using SetThreadDescriptionFunc = HRESULT(WINAPI*)(HANDLE, PCWSTR);

        HMODULE hKernel = GetModuleHandleA("kernel32.dll");
        if (!hKernel)
            return;

        auto setDesc = reinterpret_cast<SetThreadDescriptionFunc>(
            GetProcAddress(hKernel, "SetThreadDescription"));

        if (!setDesc)
            return;

        int len = MultiByteToWideChar(CP_UTF8, 0, name, -1, nullptr, 0);
        if (len <= 0)
            return;

        std::wstring wname(static_cast<size_t>(len), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, name, -1, &wname[0], len);

        HRESULT hr = setDesc(GetCurrentThread(), wname.c_str());
        return;

    #elif defined(__linux__)

        // Linux limit: 16 bytes including null
        char buffer[16] = {0};
        snprintf(buffer, sizeof(buffer), "%s", name);

        int ret = pthread_setname_np(pthread_self(), buffer);
        return;

    #else
        (void)name;
        return;
    #endif
    }
}

#endif // __REMOTE_COMMAND_SERVER_HELPER__