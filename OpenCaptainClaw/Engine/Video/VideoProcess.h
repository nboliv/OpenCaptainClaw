#ifndef __VIDEO_PROCESS_H__
#define __VIDEO_PROCESS_H__

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Absolute path of the ffmpeg binary, or an empty string when none is found.
// Resolved here instead of by a shell: a bare "ffmpeg" handed to a shell would
// let PATH decide what gets executed.
std::string FindFfmpeg();

// Runs a child process from an explicit argv list, optionally streaming its
// stdout through a FILE*. No shell is involved, so no argument is ever parsed
// for metacharacters and PATH is never consulted by the exec itself.
//
// ponytail: no stderr capture, no timeout. Callers want the child's error
// output where it always went and a process that either finishes or dies with
// the pipe.
class FfmpegProcess
{
public:
    FfmpegProcess();
    ~FfmpegProcess();

    bool Start(const std::vector<std::string>& args, bool captureStdout);
    FILE* Stream() const { return m_Stream; }

    // Closes the stream (which unblocks a still-writing child) and reaps the
    // process. Returns the exit code, or -1 when nothing was running.
    int WaitAndClose();

private:
    FILE* m_Stream;
    // pid_t on POSIX, process HANDLE on Windows. -1/0 = nothing running.
    intptr_t m_Handle;
};

#endif
