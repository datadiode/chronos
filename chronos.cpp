/* chronos - a crude substitution for POSIX `time` command line utility
   on Windows.
   Aggregates and reports user and kernel times for process and its children.
   Attempts to mimic output format used on Linux

Copyright (c) 2016, Grigory Rechistov
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <cstdint>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include <windows.h>

/* Get a human-readable description of the last error.
   Kinda like POSIX's strerror() */
static std::wstring GetLastErrorDescription() {
    DWORD errcode = GetLastError();
    if (errcode == 0) return std::wstring();

    LPWSTR buf = NULL;
    size_t bufSize = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | 
                                   FORMAT_MESSAGE_IGNORE_INSERTS |
                                    FORMAT_MESSAGE_FROM_SYSTEM, 
                                    NULL, errcode,
                                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                    (LPWSTR)&buf, 0, NULL);
    std::wstring message(buf, bufSize);
    LocalFree(buf);
    return message;
}

/* Return true if s contains path to BAT file */
static bool IsBatFile(const std::wstring &s) {
    // XXX I cannot seem to invent a proper way to decide
    // if the file is a batch script. Use a name heuristic
    if (s.rfind(L".bat") == s.length() - 4)
        return true;
    return false;
}

static void UsageAndExit(wchar_t *argv[]) {
    std::wcerr <<
        "chronos - report wallclock, user and system times of process\n"
        "Copyright (c) 2016, Grigory Rechistov\n\n" 
        "Usage: " << argv[0] << " [-v] [-o file] [--] program [options]\n"
        "\n"
        "Run program and report its resources usage\n"
        "   --verbose, -v          produce results in verbose format\n"
        "   --wait, -w             wait before resuming program execution\n"
        "   --output, -o filename  write result to filename instead of stdout\n"
        "   --inject, -i filename  sideload a dll from filename\n"
        "   --memory, -m bytes     limit process memory as specified\n"
        "   program                program name to start\n"
        "   options                the program's own arguments\n" << std::endl;
    exit(1);
}

/* Discovered command line options */
struct CliParams {
    bool verbose; /* true if verbose output */
    bool wait; /* true if instructed to wait before resuming program execution */
    std::wstring outputFileName; /* file name to write results, or empty string */
    std::wstring injectFileName; /* file name to sideload dll, or empty string */
    std::wstring cmdLine; /* The rest of command line options combined in a string */
    std::wstring progName; /* Isolated program name to create */
    std::wstring memoryLimit; /* Amount of memory which the program may consume */
};

/* Parses a byte size, optionally followed by a metric unit prefix */
static size_t ParseSize(const wchar_t *str) {
    wchar_t *unit = NULL;
    size_t value = wcstoull(str, &unit, 0);
    switch (*unit) {
    case 'P': case 'p': value <<= 10; // fall through
    case 'T': case 't': value <<= 10; // fall through
    case 'G': case 'g': value <<= 10; // fall through
    case 'M': case 'm': value <<= 10; // fall through
    case 'K': case 'k': value <<= 10; // fall through
    }
    return value;
}

/* Returns true on success, false if parsing failed */
/* BUG: may not handle quoted arguments and spaces in them as a whole */
static bool ParseArgv(int argc, wchar_t *argv[], CliParams &result) {
    assert(argc >= 1);
    argv++;
    argc--;

    int argNo = 0;
    std::wstring *consumeNextPositionalArgument = nullptr;

    while (argNo < argc) {
        std::wstring curWord = argv[argNo];
        if (consumeNextPositionalArgument) {
            *consumeNextPositionalArgument = curWord;
            consumeNextPositionalArgument = nullptr;
            argNo++;
            continue;
        }

        if (!curWord.compare(L"--")) {
            /* Optional separator of flags and positional arguments */
            argNo++; /* skip the "--" itself */
            break;
        }
        /* Look for matches for supported options */
        if (int len = curWord.find(L"-o") == 0 ? 2 : curWord.find(L"--output") == 0 ? 8 : 0) {
            curWord.erase(0, len); /* remove the '-o' part */
            if (curWord.empty()) { /* must be the next word */
                consumeNextPositionalArgument = &result.outputFileName;
            } else { /* argument is attached to the flag */
                result.outputFileName = curWord;
            }
        } else if (len += curWord.find(L"-i") == 0 ? 2 : curWord.find(L"--inject") == 0 ? 8 : 0) {
            curWord.erase(0, len); /* remove the '-i' part */
            if (curWord.empty()) { /* must be the next word */
                consumeNextPositionalArgument = &result.injectFileName;
            } else {
                result.injectFileName = curWord;
            }
        } else if (len += curWord.find(L"-m") == 0 ? 2 : curWord.find(L"--memory") == 0 ? 8 : 0) {
            curWord.erase(0, len); /* remove the '-i' part */
            if (curWord.empty()) { /* must be the next word */
                consumeNextPositionalArgument = &result.memoryLimit;
            } else {
                result.memoryLimit = curWord;
            }
        } else if (!curWord.compare(L"-v")
                || !curWord.compare(L"--verbose")) {
            result.verbose = true;
        } else if (!curWord.compare(L"-w")
                || !curWord.compare(L"--wait")) {
            result.wait = true;
        } else if (!curWord.compare(L"-h")
                || !curWord.compare(L"--help")) {
            /* Help asked */
            return false;
        } else if (curWord.find(L"-") == 0) { /* Unknown option */
            std::wcerr << "Unknown option " << curWord << std::endl;
            return false;
            break;
        } else { /* Non positional arguments have started */
            break;
        }
        argNo++;
    }

    if (consumeNextPositionalArgument) {
        std::wcerr << "Missing positional argument" << std::endl;
        return false;
    }

    /* Check if there is at least one positional parameter left */
    if (argNo == argc) {
        std::wcerr << "Missing program name" << std::endl;
        return false;
    }
    result.progName = argv[argNo];
    result.cmdLine = result.progName;
    /* Concatenate all arguments into one line */
    for (int i = argNo + 1; i < argc; i++) {
        result.cmdLine.append(L" ").append(argv[i]);
    }
    return true;
}

int wmain(int argc, wchar_t* argv[]) {
    const double timeUnit = 1.0e-7; /* 100 nanoseconds time resolution unit */
    int ret = 0;
    /* Parse command line arguments */
    CliParams params = { 0 };
    if (!ParseArgv(argc, argv, params)) {
        UsageAndExit(argv);
    }

    /* Prepare limits */
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    limits.BasicLimitInformation.LimitFlags = 0;
    if (SIZE_T value = ParseSize(params.memoryLimit.c_str())) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = value;
    } else if (!params.memoryLimit.empty()) {
        std::wcerr << L"Invalid byte size argument: "
            << params.memoryLimit << std::endl;
        return 127;
    }

    /* Prepare to start application */
    STARTUPINFO startUp;
    GetStartupInfo(&startUp);

    /* Start program in paused state */
    PROCESS_INFORMATION procInfo;
    const wchar_t *programNamePtr = NULL;

    if (IsBatFile(params.progName)) {
        /* Running batch files is unnecessarily tricky.
           Leave parsing of command line arguments to the system.
           The drawback - potential security problem for program names
           with spaces */
        programNamePtr = NULL;
    } else {
        programNamePtr = params.progName.c_str();
    }
    if (!CreateProcess(programNamePtr, 
            const_cast<LPWSTR>(params.cmdLine.c_str()),
            NULL, NULL, TRUE,
            CREATE_SUSPENDED | NORMAL_PRIORITY_CLASS, 
            NULL, NULL, &startUp, &procInfo)) {
        std::wcerr << L"Unable to start the process: "
            << GetLastErrorDescription() << std::endl;
        return 127;
    }

    HANDLE hProcess = procInfo.hProcess;

    /* Create job object and attach the process to it */
    HANDLE hJob = CreateJobObject(NULL, NULL); // XXX no security attributes passed
    assert(hJob != NULL);
    ret = AssignProcessToJobObject(hJob, hProcess);
    assert(ret);

    if (limits.BasicLimitInformation.LimitFlags) {
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }

    if (!params.injectFileName.empty()) {
        LPVOID argBuffer = VirtualAllocEx(hProcess, NULL, 4096, MEM_COMMIT, PAGE_READWRITE);
        if (argBuffer == NULL) {
            std::wcerr << L"Failed at VirtualAllocEx(): " 
                << GetLastErrorDescription() << std::endl;
            return 127;
        }
        BOOL success = WriteProcessMemory(hProcess, argBuffer, params.injectFileName.c_str(), (params.injectFileName.length() + 1) * sizeof(wchar_t), NULL);
        if (!success) {
            std::wcerr << L"Failed at WriteProcessMemory(): " 
                << GetLastErrorDescription() << std::endl;
            return 127;
        }
        HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(LoadLibraryW), argBuffer, 0, NULL);
        if (hThread == NULL) {
            std::wcerr << L"Failed at CreateRemoteThread(): " 
                << GetLastErrorDescription() << std::endl;
            return 127;
        }

        WaitForSingleObject(hThread, INFINITE);

        VirtualFreeEx(hProcess, argBuffer, 0, MEM_RELEASE);

        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);
        if (exitCode == 0) {
            std::wcerr << L"Failed to sideload " << params.injectFileName << std::endl;
            return 127;
        }
    }

    if (params.wait) {
        std::wcerr << L"Hit the ENTER key to resume program execution";
        std::wstring input;
        std::getline(std::wcin, input);
    }

    /* Now run the process and allow it to spawn children */
    ResumeThread(procInfo.hThread);

    /* Block until the process terminates */
    if (WaitForSingleObject(hProcess, INFINITE) != WAIT_OBJECT_0) {
        std::wcerr << L"Failed waiting for process termination: " 
            << GetLastErrorDescription() << std::endl;
        return 127;
    }

    DWORD exitCode = 0;
    ret = GetExitCodeProcess(hProcess, &exitCode);
    assert(ret);

    /* Calculate wallclock time in hundreds of nanoseconds.
    Ignore user and kernel times (third and fourth return parameters) */
    FILETIME createTime, exitTime, unusedTime;
    ret = GetProcessTimes(hProcess, &createTime, &exitTime, &unusedTime, &unusedTime);
    assert(ret);

    LONGLONG createTime100Ns = (LONGLONG)createTime.dwHighDateTime << 32 | createTime.dwLowDateTime;
    LONGLONG exitTime100Ns = (LONGLONG)exitTime.dwHighDateTime << 32 | exitTime.dwLowDateTime;
    LONGLONG wallclockTime100Ns = exitTime100Ns - createTime100Ns;

    /* Get total user and kernel times for all processes of the job object */
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION jobInfo;
    ret = QueryInformationJobObject(hJob, JobObjectBasicAccountingInformation,
        &jobInfo, sizeof(jobInfo), NULL);
    assert(ret);
    /* Close unused handlers */
    CloseHandle(hProcess);
    CloseHandle(hJob);

    if (jobInfo.ActiveProcesses != 0) {
        std::cerr << "Warning: there are still "
            << jobInfo.ActiveProcesses
            << " alive children processes" << std::endl;
        /* We may kill survived processes, if desired */
        //std::cerr << "Killing them" << std::endl;
        //TerminateJobObject(hJob, 127);
    }

    /* If abused as a Windows substitute for the wine command, return without output */
    if (GetModuleHandle(L"wine.exe")) {
        return exitCode;
    }

    /* Get kernel and user times in hundreds of nanoseconds */
    LONGLONG kernelTime100Ns = jobInfo.TotalKernelTime.QuadPart;
    LONGLONG userTime100Ns = jobInfo.TotalUserTime.QuadPart;
    DWORD pageFaults = jobInfo.TotalPageFaultCount; /* Also available, why not report it as well */

    /* Choose where to print results - to a file or stdout */
    std::wstreambuf *buf;
    std::wofstream of;
    if (!params.outputFileName.empty()) {
        of.open(params.outputFileName);
        buf = of.rdbuf();
    } else {
        buf = std::wcout.rdbuf();
    }
    std::wostream out(buf);

    /* Print floats with two digits after the dot */
    out << std::fixed << std::setprecision(2);

    out << std::endl;
    if (params.verbose) {
        out << L"Command being timed: " << L"\"" << params.cmdLine << L"\"" << std::endl;

        out << "Elapsed (wall clock) time (seconds): " << timeUnit * wallclockTime100Ns << std::endl;
        out << "User time (seconds): " << timeUnit * userTime100Ns << std::endl;
        out << "System time (seconds): " << timeUnit * kernelTime100Ns << std::endl;
        out << "Page faults: " << pageFaults << std::endl;
        out << "Exit status: " << exitCode << std::endl;
    } else {
        /* Match POSIX time output */
        out << "real" << "\t" << timeUnit * wallclockTime100Ns << "s"<< std::endl;
        out << "user" << "\t" << timeUnit * userTime100Ns << "s" << std::endl;
        out << "sys" << "\t" << timeUnit * kernelTime100Ns << "s" << std::endl;
    }

    if (of.is_open()) {
        of.close();
    }
    return exitCode;
}
