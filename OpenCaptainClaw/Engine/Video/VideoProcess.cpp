#include "VideoProcess.h"

#include <cstdlib>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static bool FileExists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// ffmpeg is not guaranteed to be on PATH when the game is launched from Finder,
// and its name must never reach a shell: resolve a concrete executable path.
static std::string FindFfmpegOnPath(const char* pathEnv)
{
#ifdef _WIN32
    const char kSeparator = ';';
    const char kDirSeparator = '\\';
    const char kBinary[] = "ffmpeg.exe";
#else
    const char kSeparator = ':';
    const char kDirSeparator = '/';
    const char kBinary[] = "ffmpeg";
#endif

    const std::string paths(pathEnv ? pathEnv : "");
    size_t begin = 0;
    while (begin < paths.size())
    {
        size_t end = paths.find(kSeparator, begin);
        std::string dir = paths.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!dir.empty())
        {
            if (dir[dir.size() - 1] != kDirSeparator)
            {
                dir += kDirSeparator;
            }
            dir += kBinary;
            if (FileExists(dir))
            {
                return dir;
            }
        }

        if (end == std::string::npos)
        {
            break;
        }
        begin = end + 1;
    }

    return "";
}

std::string FindFfmpeg()
{
    static const std::string found = []() -> std::string
    {
        static const char* kKnown[] = {
            "/opt/homebrew/bin/ffmpeg",
            "/usr/local/bin/ffmpeg",
            "/usr/bin/ffmpeg",
        };

        for (const char* candidate : kKnown)
        {
            if (FileExists(candidate))
            {
                return candidate;
            }
        }

        return FindFfmpegOnPath(getenv("PATH"));
    }();

    return found;
}

FfmpegProcess::FfmpegProcess()
    : m_Stream(nullptr),
      m_Handle(-1)
{
}

FfmpegProcess::~FfmpegProcess()
{
    WaitAndClose();
}

#ifdef _WIN32

// argv-style quoting understood by the child's CRT/CommandLineToArgvW: wrap in
// double quotes and double every backslash that precedes a quote.
static std::string QuoteForWindows(const std::string& arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\"") == std::string::npos)
    {
        return arg;
    }

    std::string quoted = "\"";
    size_t backslashes = 0;
    for (char c : arg)
    {
        if (c == '\\')
        {
            ++backslashes;
            continue;
        }

        if (c == '"')
        {
            quoted.append(backslashes * 2 + 1, '\\');
            quoted += '"';
            backslashes = 0;
            continue;
        }

        quoted.append(backslashes, '\\');
        backslashes = 0;
        quoted += c;
    }

    quoted.append(backslashes * 2, '\\');
    quoted += '"';
    return quoted;
}

bool FfmpegProcess::Start(const std::vector<std::string>& args, bool captureStdout)
{
    if (args.empty())
    {
        return false;
    }

    std::string commandLine;
    for (const std::string& arg : args)
    {
        if (!commandLine.empty())
        {
            commandLine += ' ';
        }
        commandLine += QuoteForWindows(arg);
    }

    SECURITY_ATTRIBUTES inheritable = {};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE pReadPipe = nullptr;
    HANDLE pWritePipe = nullptr;
    if (captureStdout)
    {
        if (!CreatePipe(&pReadPipe, &pWritePipe, &inheritable, 0))
        {
            return false;
        }
        SetHandleInformation(pReadPipe, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA startupInfo = {};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startupInfo.hStdOutput = captureStdout ? pWritePipe : GetStdHandle(STD_OUTPUT_HANDLE);
    startupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION processInfo = {};
    std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back('\0');

    const BOOL created = CreateProcessA(nullptr, mutableCommandLine.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo);

    if (captureStdout)
    {
        CloseHandle(pWritePipe);
    }

    if (!created)
    {
        if (pReadPipe)
        {
            CloseHandle(pReadPipe);
        }
        return false;
    }

    CloseHandle(processInfo.hThread);
    m_Handle = (intptr_t)processInfo.hProcess;

    if (captureStdout)
    {
        const int fd = _open_osfhandle((intptr_t)pReadPipe, _O_RDONLY);
        if (fd == -1)
        {
            // The handle never became a CRT fd, so we still own it.
            CloseHandle(pReadPipe);
        }
        else
        {
            m_Stream = _fdopen(fd, "rb");
            if (!m_Stream)
            {
                _close(fd); // closes the underlying handle too
            }
        }
    }

    return true;
}

int FfmpegProcess::WaitAndClose()
{
    if (m_Stream)
    {
        fclose(m_Stream);
        m_Stream = nullptr;
    }

    if (!m_Handle || m_Handle == -1)
    {
        return -1;
    }

    HANDLE pProcess = (HANDLE)m_Handle;
    m_Handle = -1;

    WaitForSingleObject(pProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pProcess, &exitCode);
    CloseHandle(pProcess);

    return (int)exitCode;
}

#else

bool FfmpegProcess::Start(const std::vector<std::string>& args, bool captureStdout)
{
    if (args.empty())
    {
        return false;
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args)
    {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    int pipes[2] = { -1, -1 };
    if (captureStdout && pipe(pipes) != 0)
    {
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0)
    {
        if (captureStdout)
        {
            close(pipes[0]);
            close(pipes[1]);
        }
        return false;
    }

    if (pid == 0)
    {
        // Child: exec or die, never return into the engine.
        if (captureStdout)
        {
            dup2(pipes[1], STDOUT_FILENO);
            close(pipes[0]);
            close(pipes[1]);
        }
        execv(argv[0], argv.data());
        _exit(127);
    }

    m_Handle = (intptr_t)pid;

    if (captureStdout)
    {
        close(pipes[1]);
        m_Stream = fdopen(pipes[0], "rb");
        if (!m_Stream)
        {
            close(pipes[0]);
            return false;
        }
    }

    return true;
}

int FfmpegProcess::WaitAndClose()
{
    if (m_Stream)
    {
        fclose(m_Stream);
        m_Stream = nullptr;
    }

    if (m_Handle <= 0)
    {
        return -1;
    }

    const pid_t pid = (pid_t)m_Handle;
    m_Handle = -1;

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
    {
        return -1;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

#endif
