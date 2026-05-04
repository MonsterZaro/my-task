// process_monitor.cpp
//
// Windows process inspection utility.
//   --list                       Enumerate running processes (PID, name).
//   --read <PID> <ADDR> <SIZE>   Read SIZE bytes from ADDR in process PID.
//
// Build (cross from Linux with MinGW):
//   x86_64-w64-mingw32-g++ -static -o process_monitor.exe src/process_monitor.cpp -lpsapi
//
// Build (native on Windows with MinGW):
//   g++ -static -o process_monitor.exe src/process_monitor.cpp -lpsapi
//
// Notes:
//   - Memory reading typically requires Administrator privileges.
//   - Addresses for --read may be given in decimal (e.g. 4194304) or hex
//     (e.g. 0x400000).

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {

void PrintUsage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog << " --list\n"
        << "  " << prog << " --read <PID> <ADDRESS> <SIZE>\n"
        << "\n"
        << "  ADDRESS may be decimal (e.g. 4194304) or hex (e.g. 0x400000).\n"
        << "  SIZE is in bytes (decimal).\n";
}

std::string FormatLastError(DWORD code) {
    LPSTR buf = nullptr;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf), 0, nullptr);

    std::string msg;
    if (len && buf) {
        msg.assign(buf, len);
        while (!msg.empty() &&
               (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ')) {
            msg.pop_back();
        }
    } else {
        std::ostringstream oss;
        oss << "error code " << code;
        msg = oss.str();
    }
    if (buf) {
        LocalFree(buf);
    }
    return msg;
}

int ListProcesses() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        std::cerr << "CreateToolhelp32Snapshot failed: "
                  << FormatLastError(err) << " (code " << err << ")\n";
        return 1;
    }

    PROCESSENTRY32 entry;
    std::memset(&entry, 0, sizeof(entry));
    entry.dwSize = sizeof(entry);

    if (!Process32First(snapshot, &entry)) {
        DWORD err = GetLastError();
        std::cerr << "Process32First failed: " << FormatLastError(err)
                  << " (code " << err << ")\n";
        CloseHandle(snapshot);
        return 1;
    }

    std::cout << std::left << std::setw(8) << "PID"
              << std::setw(8) << "PPID"
              << "NAME\n";

    do {
        std::cout << std::left << std::setw(8) << entry.th32ProcessID
                  << std::setw(8) << entry.th32ParentProcessID
                  << entry.szExeFile << '\n';
    } while (Process32Next(snapshot, &entry));

    CloseHandle(snapshot);
    return 0;
}

bool ParseUint32(const char* s, std::uint32_t& out) {
    if (!s || !*s) return false;
    std::istringstream iss(s);
    iss >> out;
    return static_cast<bool>(iss) && iss.eof();
}

bool ParseUintptr(const char* s, std::uintptr_t& out) {
    if (!s || !*s) return false;
    std::string str(s);
    std::istringstream iss(str);
    if (str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        iss.ignore(2);
        iss >> std::hex >> out;
    } else {
        iss >> out;
    }
    return static_cast<bool>(iss) && iss.eof();
}

bool ParseSize(const char* s, std::size_t& out) {
    if (!s || !*s) return false;
    std::istringstream iss(s);
    iss >> out;
    return static_cast<bool>(iss) && iss.eof();
}

void HexDump(const unsigned char* data, std::size_t size,
             std::uintptr_t base_addr) {
    constexpr std::size_t kRow = 16;
    for (std::size_t i = 0; i < size; i += kRow) {
        std::ostringstream line;
        line << "0x" << std::hex << std::setw(sizeof(void*) * 2)
             << std::setfill('0') << (base_addr + i) << "  ";

        std::size_t row_end = std::min(i + kRow, size);
        for (std::size_t j = i; j < i + kRow; ++j) {
            if (j < row_end) {
                line << std::hex << std::setw(2) << std::setfill('0')
                     << static_cast<int>(data[j]) << ' ';
            } else {
                line << "   ";
            }
        }
        line << " |";
        for (std::size_t j = i; j < row_end; ++j) {
            unsigned char c = data[j];
            line << static_cast<char>((c >= 0x20 && c < 0x7f) ? c : '.');
        }
        line << "|";

        std::cout << line.str() << '\n';
    }
}

int ReadMemory(std::uint32_t pid, std::uintptr_t address, std::size_t size) {
    if (size == 0) {
        std::cerr << "SIZE must be > 0.\n";
        return 1;
    }

    HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                              FALSE, static_cast<DWORD>(pid));
    if (proc == nullptr) {
        DWORD err = GetLastError();
        std::cerr << "OpenProcess(pid=" << pid << ") failed: "
                  << FormatLastError(err) << " (code " << err << ")\n";
        if (err == ERROR_ACCESS_DENIED) {
            std::cerr << "Hint: try running this program as Administrator.\n";
        } else if (err == ERROR_INVALID_PARAMETER) {
            std::cerr << "Hint: the PID may not exist or may be a protected "
                         "system process.\n";
        }
        return 1;
    }

    unsigned char* buffer = new unsigned char[size];
    SIZE_T bytes_read = 0;
    BOOL ok = ReadProcessMemory(proc, reinterpret_cast<LPCVOID>(address),
                                buffer, static_cast<SIZE_T>(size), &bytes_read);
    if (!ok) {
        DWORD err = GetLastError();
        std::cerr << "ReadProcessMemory failed: " << FormatLastError(err)
                  << " (code " << err << ")\n";
        if (bytes_read > 0) {
            std::cerr << "Partial read: " << bytes_read << " bytes.\n";
            HexDump(buffer, bytes_read, address);
        }
        delete[] buffer;
        CloseHandle(proc);
        return 1;
    }

    std::cout << "Read " << bytes_read << " byte(s) from PID " << pid
              << " at 0x" << std::hex << address << std::dec << ":\n";
    HexDump(buffer, bytes_read, address);

    delete[] buffer;
    CloseHandle(proc);
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "--list") {
        if (argc != 2) {
            PrintUsage(argv[0]);
            return 1;
        }
        return ListProcesses();
    }

    if (mode == "--read") {
        if (argc != 5) {
            PrintUsage(argv[0]);
            return 1;
        }
        std::uint32_t pid = 0;
        std::uintptr_t address = 0;
        std::size_t size = 0;
        if (!ParseUint32(argv[2], pid)) {
            std::cerr << "Invalid PID: " << argv[2] << "\n";
            return 1;
        }
        if (!ParseUintptr(argv[3], address)) {
            std::cerr << "Invalid ADDRESS: " << argv[3]
                      << " (use decimal or 0x-prefixed hex)\n";
            return 1;
        }
        if (!ParseSize(argv[4], size)) {
            std::cerr << "Invalid SIZE: " << argv[4] << "\n";
            return 1;
        }
        return ReadMemory(pid, address, size);
    }

    if (mode == "-h" || mode == "--help") {
        PrintUsage(argv[0]);
        return 0;
    }

    std::cerr << "Unknown mode: " << mode << "\n";
    PrintUsage(argv[0]);
    return 1;
}
