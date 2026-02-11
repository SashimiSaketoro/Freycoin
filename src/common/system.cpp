// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <freycoin-build-config.h> // IWYU pragma: keep

#include <common/system.h>

#include <logging.h>
#include <util/string.h>
#include <util/time.h>

#ifdef WIN32
#include <cassert>
#include <codecvt>
#include <compat/compat.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifdef HAVE_MALLOPT_ARENA_MAX
#include <malloc.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <locale>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using util::ReplaceAll;

// Application startup time (used for uptime calculation)
const int64_t nStartupTime = GetTime();

#ifndef WIN32
std::string ShellEscape(const std::string& arg)
{
    std::string escaped = arg;
    ReplaceAll(escaped, "'", "'\"'\"'");
    return "'" + escaped + "'";
}
#endif

#if HAVE_SYSTEM
void runCommand(const std::string& strCommand)
{
    if (strCommand.empty()) return;
#ifdef WIN32
    // SECURITY: Use CreateProcessW instead of _wsystem() to avoid shell
    // metacharacter injection via cmd.exe.  Analogous to the POSIX
    // fork+execvp approach below.
    //
    // Tokenize the command, then rebuild a properly-quoted command line
    // for CreateProcessW (which does NOT invoke cmd.exe).
    {
        std::vector<std::string> args;
        const char* p = strCommand.c_str();
        while (*p) {
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p) break;
            std::string arg;
            if (*p == '\'') {
                ++p;
                while (*p && *p != '\'') arg += *p++;
                if (*p == '\'') ++p;
            } else if (*p == '"') {
                ++p;
                while (*p && *p != '"') arg += *p++;
                if (*p == '"') ++p;
            } else {
                while (*p && *p != ' ' && *p != '\t') arg += *p++;
            }
            if (!arg.empty()) args.push_back(std::move(arg));
        }
        if (args.empty()) return;

        // Rebuild a safe command line: quote every argument individually
        // using Windows quoting rules (double-quotes, backslash-escape).
        std::wstring cmdline;
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> conv;
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) cmdline += L' ';
            std::wstring warg = conv.from_bytes(args[i]);
            cmdline += L'"';
            for (auto ch : warg) {
                if (ch == L'"') cmdline += L'\\';
                cmdline += ch;
            }
            cmdline += L'"';
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        // CreateProcessW needs a mutable command line buffer
        std::vector<wchar_t> cmd_buf(cmdline.begin(), cmdline.end());
        cmd_buf.push_back(L'\0');

        BOOL ok = CreateProcessW(
            nullptr,       // lpApplicationName — derive from cmdline
            cmd_buf.data(),
            nullptr,       // lpProcessAttributes
            nullptr,       // lpThreadAttributes
            FALSE,         // bInheritHandles
            0,             // dwCreationFlags
            nullptr,       // lpEnvironment
            nullptr,       // lpCurrentDirectory
            &si,
            &pi);

        if (!ok) {
            LogPrintf("runCommand error: CreateProcessW failed (%d) for: %s\n",
                      GetLastError(), strCommand);
        } else {
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            if (exitCode != 0) {
                LogPrintf("runCommand error: %s exited with code %lu\n",
                          strCommand, exitCode);
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }
#else
    // SECURITY: Use fork+execvp instead of system() to avoid shell metacharacter
    // injection. The command is split into argv[] and executed directly without
    // invoking /bin/sh. This is defense-in-depth — callers already sanitize input.
    //
    // Tokenization: split on whitespace, respecting single-quoted strings
    // (alertnotify wraps the %s substitution in single quotes).
    std::vector<std::string> args;
    {
        const char* p = strCommand.c_str();
        while (*p) {
            // Skip whitespace
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p) break;

            std::string arg;
            if (*p == '\'') {
                // Single-quoted string: take everything until closing quote
                ++p;
                while (*p && *p != '\'') {
                    arg += *p++;
                }
                if (*p == '\'') ++p;
            } else {
                // Unquoted token: take until whitespace
                while (*p && *p != ' ' && *p != '\t') {
                    arg += *p++;
                }
            }
            if (!arg.empty()) {
                args.push_back(std::move(arg));
            }
        }
    }

    if (args.empty()) return;

    // Build argv for execvp
    std::vector<char*> argv;
    for (auto& a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid == -1) {
        LogPrintf("runCommand error: fork() failed for command: %s\n", strCommand);
        return;
    }

    if (pid == 0) {
        // Child process
        execvp(argv[0], argv.data());
        // execvp only returns on error
        _exit(127);
    }

    // Parent: wait for child
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        LogPrintf("runCommand error: %s exited with code %d\n", strCommand, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        LogPrintf("runCommand error: %s killed by signal %d\n", strCommand, WTERMSIG(status));
    }
#endif
}
#endif

void SetupEnvironment()
{
#ifdef HAVE_MALLOPT_ARENA_MAX
    // glibc-specific: On 32-bit systems set the number of arenas to 1.
    // By default, since glibc 2.10, the C library will create up to two heap
    // arenas per core. This is known to cause excessive virtual address space
    // usage in our usage. Work around it by setting the maximum number of
    // arenas to 1.
    if (sizeof(void*) == 4) {
        mallopt(M_ARENA_MAX, 1);
    }
#endif
    // On most POSIX systems (e.g. Linux, but not BSD) the environment's locale
    // may be invalid, in which case the "C.UTF-8" locale is used as fallback.
#if !defined(WIN32) && !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__)
    try {
        std::locale(""); // Raises a runtime error if current locale is invalid
    } catch (const std::runtime_error&) {
        setenv("LC_ALL", "C.UTF-8", 1);
    }
#elif defined(WIN32)
    assert(GetACP() == CP_UTF8);
    // Set the default input/output charset is utf-8
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

#ifndef WIN32
    constexpr mode_t private_umask = 0077;
    umask(private_umask);
#endif
}

bool SetupNetworking()
{
#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR || LOBYTE(wsadata.wVersion ) != 2 || HIBYTE(wsadata.wVersion) != 2)
        return false;
#endif
    return true;
}

int GetNumCores()
{
    return std::thread::hardware_concurrency();
}

std::optional<size_t> GetTotalRAM()
{
    [[maybe_unused]] auto clamp{[](uint64_t v) { return size_t(std::min(v, uint64_t{std::numeric_limits<size_t>::max()})); }};
#ifdef WIN32
    if (MEMORYSTATUSEX m{}; (m.dwLength = sizeof(m), GlobalMemoryStatusEx(&m))) return clamp(m.ullTotalPhys);
#elif defined(__APPLE__) || \
      defined(__FreeBSD__) || \
      defined(__NetBSD__) || \
      defined(__OpenBSD__) || \
      defined(__illumos__) || \
      defined(__linux__)
    if (long p{sysconf(_SC_PHYS_PAGES)}, s{sysconf(_SC_PAGESIZE)}; p > 0 && s > 0) return clamp(1ULL * p * s);
#endif
    return std::nullopt;
}

// Obtain the application startup time (used for uptime calculation)
int64_t GetStartupTime()
{
    return nStartupTime;
}
