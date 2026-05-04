/*++

Module Name:

    Monitor.c

Abstract:

    HelloDrvMonitor -- a user-mode subscriber for the HelloDrv kernel
    driver. Opens \\.\HelloDrv, blocks on IOCTL_HELLODRV_GET_NEXT_EVENT,
    and prints each event in a human-readable form (or as JSON for piping
    into log-analysis pipelines).

    Usage:
        HelloDrvMonitor.exe [options]

    Options:
        --json                   Emit one JSON object per event, one per line
                                 (jsonl). Suitable for `... | jq .` etc.
        --pid <PID>              Only show events for this process ID. May be
                                 repeated.
        --image <substring>      Only show events whose image-file path
                                 contains <substring> (case-insensitive).
        --no-images              Suppress image-load events (very noisy).
        --no-threads             Suppress thread create/exit events.
        --stats                  Print live stats every second to stderr.
        --reset-stats            Zero the driver's stat counters and exit.
        -h, --help               Show this help.

    Build (Windows, MSVC Developer Command Prompt):

        cl /W4 /WX /EHsc /Fe:HelloDrvMonitor.exe Monitor.c

    Build (Linux, MinGW cross-compile -- fully static, no DLLs to ship):

        x86_64-w64-mingw32-gcc -Wall -Wextra -static -O2 \
            -o HelloDrvMonitor.exe Monitor.c

    Run as Administrator on the test VM after the driver is loaded.

License:

    MIT-style; educational sample.

--*/

#include <windows.h>
#include <winioctl.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../HelloDrv/Public.h"

#define MAX_PID_FILTERS   16
#define MAX_IMAGE_FILTERS 8

typedef struct {
    bool        json;
    bool        showImages;
    bool        showThreads;
    bool        liveStats;
    DWORD       pidFilters[MAX_PID_FILTERS];
    size_t      pidFilterCount;
    char        imageFilters[MAX_IMAGE_FILTERS][260];
    size_t      imageFilterCount;
} CliOptions;

static volatile BOOL g_stop = FALSE;

static BOOL WINAPI
CtrlHandler(DWORD ctrlType)
{
    (void)ctrlType;
    g_stop = TRUE;
    return TRUE;
}

//
// FILETIME (100-ns since 1601) -> "YYYY-MM-DD HH:MM:SS.mmm" UTC.
//
static void
FormatTimestamp(LARGE_INTEGER ts, char* out, size_t outSize)
{
    FILETIME    ft;
    SYSTEMTIME  st;

    ft.dwLowDateTime  = ts.LowPart;
    ft.dwHighDateTime = (DWORD)ts.HighPart;
    if (!FileTimeToSystemTime(&ft, &st)) {
        snprintf(out, outSize, "0000-00-00 00:00:00.000");
        return;
    }
    snprintf(out, outSize, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
             (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
             (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
             (unsigned)st.wMilliseconds);
}

//
// Convert wide string to UTF-8 (best effort).
//
static void
WideToUtf8(const wchar_t* w, USHORT cch, char* out, size_t outSize)
{
    int written;
    if (cch == 0 || w == NULL) {
        if (outSize > 0) out[0] = '\0';
        return;
    }
    written = WideCharToMultiByte(CP_UTF8, 0, w, cch,
                                  out, (int)outSize - 1, NULL, NULL);
    if (written < 0) written = 0;
    if ((size_t)written >= outSize) written = (int)outSize - 1;
    out[written] = '\0';
}

//
// Escape a UTF-8 string for inclusion in a JSON string literal.
//
static void
JsonEscape(const char* in, char* out, size_t outSize)
{
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 6 < outSize; ++p) {
        unsigned char c = *p;
        switch (c) {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\b': out[o++] = '\\'; out[o++] = 'b';  break;
            case '\f': out[o++] = '\\'; out[o++] = 'f';  break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:
                if (c < 0x20) {
                    o += (size_t)snprintf(out + o, outSize - o, "\\u%04x", c);
                } else {
                    out[o++] = (char)c;
                }
                break;
        }
    }
    out[o] = '\0';
}

static const char*
EventTypeName(ULONG t)
{
    switch (t) {
        case HelloDrvEventProcessCreate: return "PROCESS_CREATE";
        case HelloDrvEventProcessExit:   return "PROCESS_EXIT";
        case HelloDrvEventImageLoad:     return "IMAGE_LOAD";
        case HelloDrvEventThreadCreate:  return "THREAD_CREATE";
        case HelloDrvEventThreadExit:    return "THREAD_EXIT";
        default:                         return "UNKNOWN";
    }
}

static bool
EventPassesFilters(const HELLODRV_EVENT* ev, const CliOptions* opt,
                   const char* imageUtf8)
{
    if (!opt->showImages && ev->Type == HelloDrvEventImageLoad)   return false;
    if (!opt->showThreads &&
        (ev->Type == HelloDrvEventThreadCreate ||
         ev->Type == HelloDrvEventThreadExit)) return false;

    if (opt->pidFilterCount > 0) {
        bool match = false;
        for (size_t i = 0; i < opt->pidFilterCount; ++i) {
            if (opt->pidFilters[i] == ev->ProcessId) { match = true; break; }
        }
        if (!match) return false;
    }

    if (opt->imageFilterCount > 0 && ev->ImageFileNameCch > 0) {
        bool match = false;
        for (size_t i = 0; i < opt->imageFilterCount; ++i) {
            // Case-insensitive substring search.
            const char* hay = imageUtf8;
            const char* needle = opt->imageFilters[i];
            size_t needleLen = strlen(needle);
            if (needleLen == 0) { match = true; break; }
            for (size_t j = 0; hay[j] != '\0'; ++j) {
                size_t k;
                for (k = 0; k < needleLen; ++k) {
                    char a = hay[j + k];
                    char b = needle[k];
                    if (a == '\0') break;
                    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                    if (a != b) break;
                }
                if (k == needleLen) { match = true; break; }
            }
            if (match) break;
        }
        if (!match) return false;
    }

    return true;
}

static void
PrintEvent(const HELLODRV_EVENT* ev, const CliOptions* opt)
{
    char ts[32];
    char imageUtf8[HELLODRV_MAX_PATH_CCH * 4];
    char cmdUtf8[HELLODRV_MAX_CMDLINE_CCH * 4];

    FormatTimestamp(ev->Timestamp, ts, sizeof(ts));
    WideToUtf8(ev->ImageFileName, ev->ImageFileNameCch, imageUtf8, sizeof(imageUtf8));
    WideToUtf8(ev->CommandLine,   ev->CommandLineCch,   cmdUtf8,   sizeof(cmdUtf8));

    if (!EventPassesFilters(ev, opt, imageUtf8)) return;

    if (opt->json) {
        char imageEsc[sizeof(imageUtf8) * 2 + 16];
        char cmdEsc[sizeof(cmdUtf8)   * 2 + 16];
        JsonEscape(imageUtf8, imageEsc, sizeof(imageEsc));
        JsonEscape(cmdUtf8,   cmdEsc,   sizeof(cmdEsc));

        printf(
            "{\"ts\":\"%s\",\"type\":\"%s\",\"pid\":%lu,\"ppid\":%lu,"
            "\"tid\":%lu,\"kernel_image\":%s,\"image\":\"%s\",\"cmdline\":\"%s\"}\n",
            ts,
            EventTypeName(ev->Type),
            (unsigned long)ev->ProcessId,
            (unsigned long)ev->ParentProcessId,
            (unsigned long)ev->ThreadId,
            ev->KernelInitiated ? "true" : "false",
            imageEsc,
            cmdEsc);
    } else {
        switch (ev->Type) {
            case HelloDrvEventProcessCreate:
                printf("[%s] %-14s pid=%-5lu ppid=%-5lu image=\"%s\" cmd=\"%s\"\n",
                       ts, EventTypeName(ev->Type),
                       (unsigned long)ev->ProcessId,
                       (unsigned long)ev->ParentProcessId,
                       imageUtf8, cmdUtf8);
                break;
            case HelloDrvEventProcessExit:
                printf("[%s] %-14s pid=%-5lu\n",
                       ts, EventTypeName(ev->Type),
                       (unsigned long)ev->ProcessId);
                break;
            case HelloDrvEventImageLoad:
                printf("[%s] %-14s pid=%-5lu kernel=%-5s image=\"%s\"\n",
                       ts, EventTypeName(ev->Type),
                       (unsigned long)ev->ProcessId,
                       ev->KernelInitiated ? "true" : "false",
                       imageUtf8);
                break;
            case HelloDrvEventThreadCreate:
            case HelloDrvEventThreadExit:
                printf("[%s] %-14s pid=%-5lu tid=%-5lu\n",
                       ts, EventTypeName(ev->Type),
                       (unsigned long)ev->ProcessId,
                       (unsigned long)ev->ThreadId);
                break;
            default:
                printf("[%s] %-14s pid=%-5lu (raw type=%lu)\n",
                       ts, EventTypeName(ev->Type),
                       (unsigned long)ev->ProcessId,
                       (unsigned long)ev->Type);
                break;
        }
        fflush(stdout);
    }
}

static DWORD WINAPI
StatsThread(LPVOID param)
{
    HANDLE         hDev = (HANDLE)param;
    HELLODRV_STATS stats;
    DWORD          bytes;

    while (!g_stop) {
        Sleep(1000);
        if (DeviceIoControl(hDev, IOCTL_HELLODRV_GET_STATS,
                            NULL, 0,
                            &stats, sizeof(stats),
                            &bytes, NULL)) {
            fprintf(stderr,
                    "[stats] proc=+%lu/-%lu  img=%lu  thr=+%lu/-%lu  "
                    "delivered=%lu  dropped(bp/alloc)=%lu/%lu  pending=%lu\n",
                    (unsigned long)stats.ProcessCreates,
                    (unsigned long)stats.ProcessExits,
                    (unsigned long)stats.ImageLoads,
                    (unsigned long)stats.ThreadCreates,
                    (unsigned long)stats.ThreadExits,
                    (unsigned long)stats.EventsDelivered,
                    (unsigned long)stats.EventsDroppedDueToBackpressure,
                    (unsigned long)stats.EventsDroppedDueToAllocFailure,
                    (unsigned long)stats.PendingRequests);
        }
    }
    return 0;
}

static void
PrintUsage(const char* prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s [--json] [--pid PID]... [--image SUBSTR]... [--no-images]\n"
        "  %*s [--no-threads] [--stats] [--reset-stats] [-h|--help]\n",
        prog, (int)strlen(prog), "");
}

int
main(int argc, char* argv[])
{
    CliOptions opt;
    memset(&opt, 0, sizeof(opt));
    opt.showImages  = true;
    opt.showThreads = true;

    bool resetStats = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else if (strcmp(a, "--json") == 0) {
            opt.json = true;
        } else if (strcmp(a, "--no-images") == 0) {
            opt.showImages = false;
        } else if (strcmp(a, "--no-threads") == 0) {
            opt.showThreads = false;
        } else if (strcmp(a, "--stats") == 0) {
            opt.liveStats = true;
        } else if (strcmp(a, "--reset-stats") == 0) {
            resetStats = true;
        } else if (strcmp(a, "--pid") == 0 && i + 1 < argc) {
            if (opt.pidFilterCount >= MAX_PID_FILTERS) {
                fprintf(stderr, "Too many --pid filters (max %d)\n", MAX_PID_FILTERS);
                return 1;
            }
            opt.pidFilters[opt.pidFilterCount++] = (DWORD)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(a, "--image") == 0 && i + 1 < argc) {
            if (opt.imageFilterCount >= MAX_IMAGE_FILTERS) {
                fprintf(stderr, "Too many --image filters (max %d)\n", MAX_IMAGE_FILTERS);
                return 1;
            }
            strncpy(opt.imageFilters[opt.imageFilterCount],
                    argv[++i],
                    sizeof(opt.imageFilters[0]) - 1);
            opt.imageFilters[opt.imageFilterCount][sizeof(opt.imageFilters[0]) - 1] = '\0';
            opt.imageFilterCount++;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", a);
            PrintUsage(argv[0]);
            return 1;
        }
    }

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    HANDLE hDev = CreateFileW(HELLODRV_USER_DEVICE_PATH,
                              GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL,
                              OPEN_EXISTING,
                              0,
                              NULL);
    if (hDev == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        fprintf(stderr,
                "CreateFile(\"\\\\.\\HelloDrv\") failed: error %lu\n"
                "Hints:\n"
                "  - Is the driver loaded?  `sc query HelloDrv`\n"
                "  - Are you running as Administrator?\n",
                (unsigned long)err);
        return 2;
    }

    if (resetStats) {
        DWORD bytes = 0;
        BOOL ok = DeviceIoControl(hDev, IOCTL_HELLODRV_RESET_STATS,
                                  NULL, 0, NULL, 0, &bytes, NULL);
        if (!ok) {
            fprintf(stderr, "RESET_STATS failed: error %lu\n",
                    (unsigned long)GetLastError());
            CloseHandle(hDev);
            return 3;
        }
        fprintf(stderr, "Stats reset.\n");
        CloseHandle(hDev);
        return 0;
    }

    HANDLE hStats = NULL;
    if (opt.liveStats) {
        hStats = CreateThread(NULL, 0, StatsThread, hDev, 0, NULL);
    }

    fprintf(stderr,
            "[HelloDrvMonitor] Subscribed. Press Ctrl+C to stop.\n");

    while (!g_stop) {
        HELLODRV_EVENT ev;
        DWORD bytes = 0;

        BOOL ok = DeviceIoControl(hDev, IOCTL_HELLODRV_GET_NEXT_EVENT,
                                  NULL, 0,
                                  &ev, sizeof(ev),
                                  &bytes, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED) break; // Driver unloading.
            fprintf(stderr, "GET_NEXT_EVENT failed: error %lu\n",
                    (unsigned long)err);
            break;
        }
        if (bytes < sizeof(ev)) {
            fprintf(stderr, "Short read: %lu bytes (expected %zu)\n",
                    (unsigned long)bytes, sizeof(ev));
            continue;
        }

        PrintEvent(&ev, &opt);
    }

    if (hStats != NULL) {
        WaitForSingleObject(hStats, 2000);
        CloseHandle(hStats);
    }
    CloseHandle(hDev);
    fprintf(stderr, "[HelloDrvMonitor] Done.\n");
    return 0;
}
