// process_monitor.cpp
//
// A small Windows utility that:
//   * Lists running processes via the Toolhelp snapshot API.
//   * Reads a region of memory from a target process and hex-dumps it.
//
// Intended for educational use, debugging, and inspection of processes
// the operator owns or has explicit permission to analyze. Reading from
// processes you do not own typically requires Administrator privileges.

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Print the Win32 error message associated with a numeric error code.
void PrintWin32Error(const char* prefix, DWORD code) {
    LPSTR buffer = nullptr;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);

    std::cerr << prefix << " (error " << code << ")";
    if (len && buffer) {
        std::string msg(buffer, len);
        // Trim trailing CR/LF.
        while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) {
            msg.pop_back();
        }
        std::cerr << ": " << msg;
    }
    std::cerr << std::endl;

    if (buffer) {
        LocalFree(buffer);
    }
}

int ListProcesses() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        PrintWin32Error("CreateToolhelp32Snapshot failed", GetLastError());
        return 1;
    }

    PROCESSENTRY32 entry;
    std::memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);

    if (!Process32First(snapshot, &entry)) {
        DWORD err = GetLastError();
        CloseHandle(snapshot);
        if (err == ERROR_NO_MORE_FILES) {
            std::cout << "No processes found." << std::endl;
            return 0;
        }
        PrintWin32Error("Process32First failed", err);
        return 1;
    }

    std::cout << std::left << std::setw(8) << "PID"
              << std::setw(8) << "PPID"
              << std::setw(8) << "THREADS"
              << "NAME" << std::endl;
    std::cout << std::string(60, '-') << std::endl;

    do {
        std::cout << std::left << std::setw(8) << entry.th32ProcessID
                  << std::setw(8) << entry.th32ParentProcessID
                  << std::setw(8) << entry.cntThreads
                  << entry.szExeFile << std::endl;
    } while (Process32Next(snapshot, &entry));

    DWORD err = GetLastError();
    CloseHandle(snapshot);
    if (err != ERROR_NO_MORE_FILES) {
        PrintWin32Error("Process32Next failed", err);
        return 1;
    }
    return 0;
}

// Parse an unsigned integer from a string. Hex when prefixed with 0x/0X,
// otherwise decimal. Returns false on parse error or overflow.
bool ParseU64(const std::string& s, uint64_t& out, bool force_hex = false) {
    if (s.empty()) return false;
    try {
        size_t consumed = 0;
        int base = force_hex ? 16 : 0;
        std::string token = s;
        if (force_hex && (token.rfind("0x", 0) == 0 || token.rfind("0X", 0) == 0)) {
            token = token.substr(2);
        }
        unsigned long long v = std::stoull(token, &consumed, base);
        if (consumed != token.size()) return false;
        out = static_cast<uint64_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseDword(const std::string& s, DWORD& out) {
    uint64_t v = 0;
    if (!ParseU64(s, v)) return false;
    if (v > 0xFFFFFFFFULL) return false;
    out = static_cast<DWORD>(v);
    return true;
}

void HexDump(uint64_t base_address, const std::vector<uint8_t>& data) {
    constexpr size_t kBytesPerLine = 16;
    std::ios old_state(nullptr);
    old_state.copyfmt(std::cout);

    for (size_t i = 0; i < data.size(); i += kBytesPerLine) {
        std::cout << std::hex << std::setw(16) << std::setfill('0')
                  << (base_address + i) << "  ";

        // Hex bytes.
        for (size_t j = 0; j < kBytesPerLine; ++j) {
            if (i + j < data.size()) {
                std::cout << std::hex << std::setw(2) << std::setfill('0')
                          << static_cast<int>(data[i + j]) << ' ';
            } else {
                std::cout << "   ";
            }
            if (j == 7) std::cout << ' ';
        }

        std::cout << " |";
        for (size_t j = 0; j < kBytesPerLine && (i + j) < data.size(); ++j) {
            uint8_t b = data[i + j];
            char c = (b >= 0x20 && b < 0x7F) ? static_cast<char>(b) : '.';
            std::cout << c;
        }
        std::cout << '|' << std::endl;
    }

    std::cout.copyfmt(old_state);
}

int ReadMemory(const std::string& pid_str,
               const std::string& addr_str,
               const std::string& size_str) {
    DWORD pid = 0;
    if (!ParseDword(pid_str, pid) || pid == 0) {
        std::cerr << "Invalid PID: " << pid_str << std::endl;
        return 2;
    }

    uint64_t address = 0;
    if (!ParseU64(addr_str, address, /*force_hex=*/true)) {
        std::cerr << "Invalid hex address: " << addr_str
                  << " (expected hex, optionally prefixed with 0x)" << std::endl;
        return 2;
    }

    uint64_t size64 = 0;
    if (!ParseU64(size_str, size64) || size64 == 0) {
        std::cerr << "Invalid size: " << size_str << std::endl;
        return 2;
    }
    constexpr uint64_t kMaxSize = 16ULL * 1024 * 1024;  // 16 MiB cap.
    if (size64 > kMaxSize) {
        std::cerr << "Size too large (max " << kMaxSize << " bytes)" << std::endl;
        return 2;
    }
    SIZE_T size = static_cast<SIZE_T>(size64);

    HANDLE proc = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            std::cerr << "Access denied opening PID " << pid
                      << ". Try running as Administrator, or pick a process "
                         "you own." << std::endl;
        } else if (err == ERROR_INVALID_PARAMETER) {
            std::cerr << "Invalid PID " << pid
                      << " (process may not exist)." << std::endl;
        } else {
            PrintWin32Error("OpenProcess failed", err);
        }
        return 3;
    }

    std::vector<uint8_t> buffer(size);
    SIZE_T bytes_read = 0;
    BOOL ok = ReadProcessMemory(
        proc,
        reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(address)),
        buffer.data(),
        size,
        &bytes_read);

    if (!ok && bytes_read == 0) {
        DWORD err = GetLastError();
        CloseHandle(proc);
        if (err == ERROR_PARTIAL_COPY || err == ERROR_NOACCESS) {
            std::cerr << "Bad address or unreadable memory at 0x"
                      << std::hex << address << std::dec << std::endl;
        } else {
            PrintWin32Error("ReadProcessMemory failed", err);
        }
        return 4;
    }

    CloseHandle(proc);

    if (bytes_read < size) {
        std::cerr << "Warning: only read " << bytes_read << " of " << size
                  << " bytes (partial read)." << std::endl;
        buffer.resize(bytes_read);
    }

    std::cout << "Read " << bytes_read << " bytes from PID " << pid
              << " at 0x" << std::hex << address << std::dec << ":" << std::endl;
    HexDump(address, buffer);
    return 0;
}

void PrintUsage(const char* prog) {
    std::cout
        << "Usage:\n"
        << "  " << prog << " --list\n"
        << "      Enumerate running processes (PID, PPID, threads, name).\n"
        << "  " << prog << " --read <PID> <HEX_ADDRESS> <SIZE>\n"
        << "      Read SIZE bytes from PID at HEX_ADDRESS and hex-dump.\n"
        << "      HEX_ADDRESS may be specified with or without a 0x prefix.\n"
        << "      Reading from processes you do not own usually requires\n"
        << "      running as Administrator.\n"
        << "  " << prog << " --help\n"
        << "      Show this help text.\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];
    if (cmd == "--help" || cmd == "-h") {
        PrintUsage(argv[0]);
        return 0;
    }
    if (cmd == "--list") {
        return ListProcesses();
    }
    if (cmd == "--read") {
        if (argc != 5) {
            std::cerr << "--read requires <PID> <HEX_ADDRESS> <SIZE>"
                      << std::endl;
            PrintUsage(argv[0]);
            return 2;
        }
        return ReadMemory(argv[2], argv[3], argv[4]);
    }

    std::cerr << "Unknown command: " << cmd << std::endl;
    PrintUsage(argv[0]);
    return 1;
}
