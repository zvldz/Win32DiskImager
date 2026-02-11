#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

struct DiskGeometry {
    uint64_t bytesPerSector = 0;
    uint64_t totalBytes = 0;
};

struct CliOptions {
    std::string command;
    std::string device;
    std::string image;
    uint64_t bytes = 0;
    bool bytesSet = false;
    bool allocatedOnly = false;
};

std::wstring widen(const std::string &s)
{
    if (s.empty()) {
        return std::wstring();
    }
    const int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, &out[0], len);
    return out;
}

std::string lower(const std::string &s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void printUsage()
{
    std::cout << "Usage:\n";
    std::cout << "  Win32DiskImager-cli.exe list\n";
    std::cout << "  Win32DiskImager-cli.exe write  --device E: --image C:\\path\\image.img\n";
    std::cout << "  Win32DiskImager-cli.exe read   --device E: --image C:\\path\\backup.img [--bytes N] [--allocated-only]\n";
    std::cout << "  Win32DiskImager-cli.exe verify --device E: --image C:\\path\\image.img\n";
}

std::wstring formatWinError(DWORD code)
{
    wchar_t *msg = nullptr;
    const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS;
    FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring out = msg ? msg : L"Unknown error";
    if (msg) {
        LocalFree(msg);
    }
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' ')) {
        out.pop_back();
    }
    return out;
}

void printWinError(const std::wstring &context, DWORD code)
{
    std::wcerr << context << L"\nError " << code << L": " << formatWinError(code) << std::endl;
}

bool parseDriveLetter(const std::string &input, wchar_t &letter)
{
    std::string s = input;
    while (!s.empty() && (s.back() == '\\' || s.back() == '/')) {
        s.pop_back();
    }
    if (s.size() >= 2 && s[1] == ':') {
        s = s.substr(0, 1);
    }
    if (s.size() != 1) {
        return false;
    }
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    if (c < 'A' || c > 'Z') {
        return false;
    }
    letter = static_cast<wchar_t>(c);
    return true;
}

std::wstring toVolumePath(wchar_t driveLetter)
{
    std::wstring p = L"\\\\.\\A:";
    p[4] = driveLetter;
    return p;
}

std::wstring toPhysicalDrivePath(DWORD diskNumber)
{
    return L"\\\\.\\PhysicalDrive" + std::to_wstring(diskNumber);
}

HANDLE openVolume(wchar_t driveLetter, DWORD access)
{
    const std::wstring path = toVolumePath(driveLetter);
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

HANDLE openPhysicalDisk(DWORD diskNumber, DWORD access)
{
    const std::wstring path = toPhysicalDrivePath(diskNumber);
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

bool getVolumeDiskNumber(HANDLE hVolume, DWORD &diskNumber)
{
    VOLUME_DISK_EXTENTS extents = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hVolume,
                         IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         nullptr,
                         0,
                         &extents,
                         sizeof(extents),
                         &bytesReturned,
                         nullptr)) {
        return false;
    }
    diskNumber = extents.Extents[0].DiskNumber;
    return true;
}

bool getDiskGeometry(HANDLE hDisk, DiskGeometry &geometry)
{
    DISK_GEOMETRY_EX dg = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(hDisk,
                         IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                         nullptr,
                         0,
                         &dg,
                         sizeof(dg),
                         &bytesReturned,
                         nullptr)) {
        return false;
    }
    geometry.bytesPerSector = static_cast<uint64_t>(dg.Geometry.BytesPerSector);
    geometry.totalBytes = static_cast<uint64_t>(dg.DiskSize.QuadPart);
    return geometry.bytesPerSector > 0;
}

bool lockVolumeWithRetry(HANDLE hVolume, int retries = 100, DWORD delayMs = 100)
{
    DWORD bytesReturned = 0;
    for (int i = 0; i < retries; ++i) {
        if (DeviceIoControl(hVolume, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr)) {
            return true;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION) {
            return false;
        }
        if (i + 1 < retries) {
            Sleep(delayMs);
        }
    }
    return false;
}

bool dismountVolumeWithRetry(HANDLE hVolume, int retries = 50, DWORD delayMs = 100)
{
    DWORD bytesReturned = 0;
    for (int i = 0; i < retries; ++i) {
        if (DeviceIoControl(hVolume, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr)) {
            return true;
        }
        const DWORD err = GetLastError();
        if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION) {
            return false;
        }
        if (i + 1 < retries) {
            Sleep(delayMs);
        }
    }
    return false;
}

void unlockVolume(HANDLE hVolume)
{
    DWORD bytesReturned = 0;
    DeviceIoControl(hVolume, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &bytesReturned, nullptr);
}

bool writeAll(HANDLE handle, const char *data, DWORD length)
{
    DWORD total = 0;
    while (total < length) {
        DWORD written = 0;
        if (!WriteFile(handle, data + total, length - total, &written, nullptr)) {
            return false;
        }
        if (written == 0) {
            return false;
        }
        total += written;
    }
    return true;
}

bool readExact(HANDLE handle, char *data, DWORD length)
{
    DWORD total = 0;
    while (total < length) {
        DWORD n = 0;
        if (!ReadFile(handle, data + total, length - total, &n, nullptr)) {
            return false;
        }
        if (n == 0) {
            return false;
        }
        total += n;
    }
    return true;
}

uint32_t readLe32(const char *p)
{
    const unsigned char *u = reinterpret_cast<const unsigned char *>(p);
    return static_cast<uint32_t>(u[0]) |
           (static_cast<uint32_t>(u[1]) << 8) |
           (static_cast<uint32_t>(u[2]) << 16) |
           (static_cast<uint32_t>(u[3]) << 24);
}

bool getAllocatedReadBytesFromMbr(HANDLE hDisk, const DiskGeometry &geometry, uint64_t &bytesToRead)
{
    if (geometry.bytesPerSector == 0) {
        return false;
    }

    LARGE_INTEGER pos = {};
    if (!SetFilePointerEx(hDisk, pos, nullptr, FILE_BEGIN)) {
        return false;
    }

    std::vector<char> sector(static_cast<size_t>(geometry.bytesPerSector), 0);
    if (!readExact(hDisk, sector.data(), static_cast<DWORD>(geometry.bytesPerSector))) {
        return false;
    }

    uint64_t numSectors = 1;
    for (int i = 0; i < 4; ++i) {
        const int base = 0x1BE + (16 * i);
        if (base + 16 > static_cast<int>(sector.size())) {
            break;
        }
        const uint32_t start = readLe32(sector.data() + base + 8);
        const uint32_t count = readLe32(sector.data() + base + 12);
        const uint64_t end = static_cast<uint64_t>(start) + static_cast<uint64_t>(count);
        if (end > numSectors) {
            numSectors = end;
        }
    }

    bytesToRead = numSectors * geometry.bytesPerSector;
    if (bytesToRead > geometry.totalBytes) {
        bytesToRead = geometry.totalBytes;
    }
    return true;
}

class ProgressPrinter {
public:
    explicit ProgressPrinter(uint64_t totalBytes)
        : m_totalBytes(totalBytes), m_start(std::chrono::steady_clock::now()), m_tick(m_start)
    {}

    void update(uint64_t processedBytes)
    {
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_tick).count() < 500) {
            return;
        }
        const double sec = std::chrono::duration_cast<std::chrono::duration<double>>(now - m_start).count();
        const double mbPerSec = (sec > 0.0) ? ((processedBytes / 1024.0 / 1024.0) / sec) : 0.0;

        if (m_totalBytes > 0) {
            const double pct = (100.0 * processedBytes) / static_cast<double>(m_totalBytes);
            std::cout << "\rProgress: " << pct << "%  Speed: " << mbPerSec << " MB/s" << std::flush;
        } else {
            std::cout << "\rProcessed: " << (processedBytes / 1024.0 / 1024.0) << " MB  Speed: " << mbPerSec << " MB/s" << std::flush;
        }
        m_tick = now;
    }

    void done(uint64_t processedBytes)
    {
        update(processedBytes);
        std::cout << std::endl;
    }

private:
    uint64_t m_totalBytes = 0;
    std::chrono::steady_clock::time_point m_start;
    std::chrono::steady_clock::time_point m_tick;
};

bool prepareDiskHandles(const std::string &device,
                        DWORD diskAccess,
                        HANDLE &hVolume,
                        HANDLE &hDisk,
                        DWORD &diskNumber,
                        DiskGeometry &geometry)
{
    hVolume = INVALID_HANDLE_VALUE;
    hDisk = INVALID_HANDLE_VALUE;

    wchar_t driveLetter = 0;
    if (!parseDriveLetter(device, driveLetter)) {
        std::cerr << "Invalid device. Use a drive letter such as E: or E." << std::endl;
        return false;
    }

    hVolume = openVolume(driveLetter, GENERIC_READ | GENERIC_WRITE);
    if (hVolume == INVALID_HANDLE_VALUE) {
        printWinError(L"Failed to open volume handle.", GetLastError());
        return false;
    }

    if (!getVolumeDiskNumber(hVolume, diskNumber)) {
        printWinError(L"Failed to map volume to physical disk.", GetLastError());
        CloseHandle(hVolume);
        hVolume = INVALID_HANDLE_VALUE;
        return false;
    }

    if (!lockVolumeWithRetry(hVolume)) {
        printWinError(L"Failed to lock volume.", GetLastError());
        CloseHandle(hVolume);
        hVolume = INVALID_HANDLE_VALUE;
        return false;
    }

    if (!dismountVolumeWithRetry(hVolume)) {
        printWinError(L"Failed to dismount volume.", GetLastError());
        unlockVolume(hVolume);
        CloseHandle(hVolume);
        hVolume = INVALID_HANDLE_VALUE;
        return false;
    }

    hDisk = openPhysicalDisk(diskNumber, diskAccess);
    if (hDisk == INVALID_HANDLE_VALUE) {
        printWinError(L"Failed to open physical disk.", GetLastError());
        unlockVolume(hVolume);
        CloseHandle(hVolume);
        hVolume = INVALID_HANDLE_VALUE;
        return false;
    }

    if (!getDiskGeometry(hDisk, geometry)) {
        printWinError(L"Failed to get disk geometry.", GetLastError());
        CloseHandle(hDisk);
        hDisk = INVALID_HANDLE_VALUE;
        unlockVolume(hVolume);
        CloseHandle(hVolume);
        hVolume = INVALID_HANDLE_VALUE;
        return false;
    }

    return true;
}

void cleanupDiskHandles(HANDLE &hVolume, HANDLE &hDisk)
{
    if (hDisk != INVALID_HANDLE_VALUE) {
        CloseHandle(hDisk);
        hDisk = INVALID_HANDLE_VALUE;
    }
    if (hVolume != INVALID_HANDLE_VALUE) {
        unlockVolume(hVolume);
        CloseHandle(hVolume);
        hVolume = INVALID_HANDLE_VALUE;
    }
}

HANDLE openImageFileRead(const std::string &path)
{
    const std::wstring w = widen(path);
    return CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

HANDLE openImageFileWrite(const std::string &path)
{
    const std::wstring w = widen(path);
    return CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

uint64_t getHandleSize(HANDLE h)
{
    LARGE_INTEGER li = {};
    if (!GetFileSizeEx(h, &li)) {
        return 0;
    }
    return static_cast<uint64_t>(li.QuadPart);
}

int cmdList()
{
    const DWORD mask = GetLogicalDrives();
    if (mask == 0) {
        printWinError(L"GetLogicalDrives failed.", GetLastError());
        return 1;
    }

    std::cout << "Available drives:" << std::endl;
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) {
            continue;
        }
        const wchar_t letter = static_cast<wchar_t>(L'A' + i);
        wchar_t root[] = L"A:\\";
        root[0] = letter;

        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_REMOVABLE && type != DRIVE_FIXED) {
            continue;
        }

        HANDLE hVolume = openVolume(letter, FILE_READ_ATTRIBUTES);
        if (hVolume == INVALID_HANDLE_VALUE) {
            std::cout << "  " << static_cast<char>(letter) << ": (unavailable)" << std::endl;
            continue;
        }

        DWORD diskNumber = 0;
        const bool mapped = getVolumeDiskNumber(hVolume, diskNumber);
        CloseHandle(hVolume);

        const char *typeStr = (type == DRIVE_REMOVABLE) ? "removable" : "fixed";
        if (mapped) {
            std::cout << "  " << static_cast<char>(letter) << ": (" << typeStr << ", PhysicalDrive" << diskNumber << ")" << std::endl;
        } else {
            std::cout << "  " << static_cast<char>(letter) << ": (" << typeStr << ", physical unknown)" << std::endl;
        }
    }
    return 0;
}

int cmdWrite(const std::string &imagePath, const std::string &device)
{
    HANDLE hImage = openImageFileRead(imagePath);
    if (hImage == INVALID_HANDLE_VALUE) {
        printWinError(L"Failed to open image file for reading.", GetLastError());
        return 1;
    }

    HANDLE hVolume = INVALID_HANDLE_VALUE;
    HANDLE hDisk = INVALID_HANDLE_VALUE;
    DWORD diskNumber = 0;
    DiskGeometry geometry;
    if (!prepareDiskHandles(device, GENERIC_WRITE, hVolume, hDisk, diskNumber, geometry)) {
        CloseHandle(hImage);
        return 1;
    }

    int rc = 0;
    LARGE_INTEGER pos = {};
    if (!SetFilePointerEx(hDisk, pos, nullptr, FILE_BEGIN)) {
        printWinError(L"Failed to seek physical disk.", GetLastError());
        rc = 1;
        cleanupDiskHandles(hVolume, hDisk);
        CloseHandle(hImage);
        return rc;
    }

    const uint64_t imageBytes = getHandleSize(hImage);
    const uint64_t paddedImageBytes =
        ((imageBytes + geometry.bytesPerSector - 1) / geometry.bytesPerSector) * geometry.bytesPerSector;

    if (paddedImageBytes > geometry.totalBytes) {
        std::cerr << "Image is larger than target disk." << std::endl;
        std::cerr << "Image bytes (padded): " << paddedImageBytes << ", disk bytes: " << geometry.totalBytes << std::endl;
        cleanupDiskHandles(hVolume, hDisk);
        CloseHandle(hImage);
        return 1;
    }

    DWORD chunk = 4 * 1024 * 1024;
    chunk -= chunk % static_cast<DWORD>(geometry.bytesPerSector);
    if (chunk == 0) {
        chunk = static_cast<DWORD>(geometry.bytesPerSector);
    }

    std::vector<char> buffer(chunk, 0);
    uint64_t processed = 0;
    ProgressPrinter progress(imageBytes);

    while (true) {
        DWORD got = 0;
        if (!ReadFile(hImage, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
            printWinError(L"Error while reading image file.", GetLastError());
            rc = 1;
            break;
        }
        if (got == 0) {
            break;
        }

        DWORD toWrite = got;
        const DWORD rem = toWrite % static_cast<DWORD>(geometry.bytesPerSector);
        if (rem != 0) {
            const DWORD pad = static_cast<DWORD>(geometry.bytesPerSector) - rem;
            memset(buffer.data() + toWrite, 0, pad);
            toWrite += pad;
        }

        if (!writeAll(hDisk, buffer.data(), toWrite)) {
            printWinError(L"Failed while writing to physical disk.", GetLastError());
            rc = 1;
            break;
        }

        processed += got;
        progress.update(processed);
    }

    FlushFileBuffers(hDisk);
    cleanupDiskHandles(hVolume, hDisk);
    CloseHandle(hImage);
    if (rc == 0) {
        progress.done(processed);
        std::cout << "Write successful." << std::endl;
    }
    return rc;
}

int cmdRead(const std::string &imagePath, const std::string &device, uint64_t requestedBytes, bool allocatedOnly)
{
    HANDLE hImage = openImageFileWrite(imagePath);
    if (hImage == INVALID_HANDLE_VALUE) {
        printWinError(L"Failed to open image file for writing.", GetLastError());
        return 1;
    }

    HANDLE hVolume = INVALID_HANDLE_VALUE;
    HANDLE hDisk = INVALID_HANDLE_VALUE;
    DWORD diskNumber = 0;
    DiskGeometry geometry;
    if (!prepareDiskHandles(device, GENERIC_READ, hVolume, hDisk, diskNumber, geometry)) {
        CloseHandle(hImage);
        return 1;
    }

    uint64_t bytesToRead = geometry.totalBytes;
    if (allocatedOnly) {
        uint64_t allocatedBytes = 0;
        if (!getAllocatedReadBytesFromMbr(hDisk, geometry, allocatedBytes)) {
            printWinError(L"Failed to compute allocated partition range from MBR.", GetLastError());
            cleanupDiskHandles(hVolume, hDisk);
            CloseHandle(hImage);
            return 1;
        }
        bytesToRead = allocatedBytes;
    }
    if (requestedBytes != 0) {
        bytesToRead = (requestedBytes < bytesToRead) ? requestedBytes : bytesToRead;
    }
    if (bytesToRead > geometry.totalBytes) {
        std::cerr << "Requested bytes exceed disk size." << std::endl;
        cleanupDiskHandles(hVolume, hDisk);
        CloseHandle(hImage);
        return 1;
    }
    bytesToRead = (bytesToRead / geometry.bytesPerSector) * geometry.bytesPerSector;

    LARGE_INTEGER pos = {};
    if (!SetFilePointerEx(hDisk, pos, nullptr, FILE_BEGIN)) {
        printWinError(L"Failed to seek physical disk.", GetLastError());
        cleanupDiskHandles(hVolume, hDisk);
        CloseHandle(hImage);
        return 1;
    }

    DWORD chunk = 4 * 1024 * 1024;
    chunk -= chunk % static_cast<DWORD>(geometry.bytesPerSector);
    if (chunk == 0) {
        chunk = static_cast<DWORD>(geometry.bytesPerSector);
    }

    std::vector<char> buffer(chunk, 0);
    uint64_t processed = 0;
    int rc = 0;
    ProgressPrinter progress(bytesToRead);

    while (processed < bytesToRead) {
        const DWORD ask = static_cast<DWORD>((bytesToRead - processed) < chunk ? (bytesToRead - processed) : chunk);
        if (!readExact(hDisk, buffer.data(), ask)) {
            printWinError(L"Failed while reading from physical disk.", GetLastError());
            rc = 1;
            break;
        }

        if (!writeAll(hImage, buffer.data(), ask)) {
            printWinError(L"Failed while writing image file.", GetLastError());
            rc = 1;
            break;
        }

        processed += ask;
        progress.update(processed);
    }

    FlushFileBuffers(hImage);
    cleanupDiskHandles(hVolume, hDisk);
    CloseHandle(hImage);
    if (rc == 0) {
        progress.done(processed);
        std::cout << "Read successful." << std::endl;
    }
    return rc;
}

int cmdVerify(const std::string &imagePath, const std::string &device)
{
    HANDLE hImage = openImageFileRead(imagePath);
    if (hImage == INVALID_HANDLE_VALUE) {
        printWinError(L"Failed to open image file for reading.", GetLastError());
        return 1;
    }

    HANDLE hVolume = INVALID_HANDLE_VALUE;
    HANDLE hDisk = INVALID_HANDLE_VALUE;
    DWORD diskNumber = 0;
    DiskGeometry geometry;
    if (!prepareDiskHandles(device, GENERIC_READ, hVolume, hDisk, diskNumber, geometry)) {
        CloseHandle(hImage);
        return 1;
    }

    const uint64_t imageBytes = getHandleSize(hImage);
    const uint64_t paddedImageBytes =
        ((imageBytes + geometry.bytesPerSector - 1) / geometry.bytesPerSector) * geometry.bytesPerSector;
    if (paddedImageBytes > geometry.totalBytes) {
        std::cerr << "Image is larger than target disk." << std::endl;
        cleanupDiskHandles(hVolume, hDisk);
        CloseHandle(hImage);
        return 1;
    }

    LARGE_INTEGER pos = {};
    if (!SetFilePointerEx(hDisk, pos, nullptr, FILE_BEGIN)) {
        printWinError(L"Failed to seek physical disk.", GetLastError());
        cleanupDiskHandles(hVolume, hDisk);
        CloseHandle(hImage);
        return 1;
    }

    DWORD chunk = 4 * 1024 * 1024;
    chunk -= chunk % static_cast<DWORD>(geometry.bytesPerSector);
    if (chunk == 0) {
        chunk = static_cast<DWORD>(geometry.bytesPerSector);
    }

    std::vector<char> fileBuf(chunk, 0);
    std::vector<char> diskBuf(chunk, 0);
    uint64_t processed = 0;
    int rc = 0;
    ProgressPrinter progress(imageBytes);

    while (processed < imageBytes) {
        DWORD got = 0;
        if (!ReadFile(hImage, fileBuf.data(), static_cast<DWORD>(fileBuf.size()), &got, nullptr)) {
            printWinError(L"Error while reading image file.", GetLastError());
            rc = 1;
            break;
        }
        if (got == 0) {
            rc = 1;
            std::cerr << "Unexpected end of image file." << std::endl;
            break;
        }

        DWORD ask = got;
        const DWORD rem = ask % static_cast<DWORD>(geometry.bytesPerSector);
        if (rem != 0) {
            const DWORD pad = static_cast<DWORD>(geometry.bytesPerSector) - rem;
            memset(fileBuf.data() + ask, 0, pad);
            ask += pad;
        }

        if (!readExact(hDisk, diskBuf.data(), ask)) {
            printWinError(L"Failed while reading from physical disk.", GetLastError());
            rc = 1;
            break;
        }

        if (memcmp(fileBuf.data(), diskBuf.data(), ask) != 0) {
            std::cerr << "Verify failed at byte offset " << processed << std::endl;
            rc = 1;
            break;
        }

        processed += got;
        progress.update(processed);
    }

    cleanupDiskHandles(hVolume, hDisk);
    CloseHandle(hImage);
    if (rc == 0) {
        progress.done(processed);
        std::cout << "Verify successful." << std::endl;
    }
    return rc;
}

bool parseArgs(int argc, char *argv[], CliOptions &opt)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h" || a == "/?") {
            printUsage();
            return false;
        }
        if (a == "--version") {
            std::cout << "Win32DiskImager-cli 2.0" << std::endl;
            return false;
        }

        if (a == "--device" || a == "-d") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << a << std::endl;
                return false;
            }
            opt.device = argv[++i];
            continue;
        }
        if (a == "--image" || a == "-i") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << a << std::endl;
                return false;
            }
            opt.image = argv[++i];
            continue;
        }
        if (a == "--bytes" || a == "-b") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << a << std::endl;
                return false;
            }
            try {
                opt.bytes = std::stoull(argv[++i]);
                opt.bytesSet = true;
            } catch (...) {
                std::cerr << "Invalid --bytes value." << std::endl;
                return false;
            }
            continue;
        }
        if (a == "--allocated-only" || a == "-a") {
            opt.allocatedOnly = true;
            continue;
        }

        if (!a.empty() && a[0] == '-') {
            std::cerr << "Unknown option: " << a << std::endl;
            return false;
        }

        if (opt.command.empty()) {
            opt.command = lower(a);
        } else {
            std::cerr << "Unexpected argument: " << a << std::endl;
            return false;
        }
    }
    return true;
}

bool isRunningAsAdmin()
{
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminGroup = nullptr;
    if (!AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                  &adminGroup)) {
        return false;
    }
    CheckTokenMembership(nullptr, adminGroup, &isAdmin);
    FreeSid(adminGroup);
    return isAdmin == TRUE;
}

} // namespace

int main(int argc, char *argv[])
{
    CliOptions opt;
    if (!parseArgs(argc, argv, opt)) {
        return 1;
    }

    if (opt.command.empty()) {
        printUsage();
        return 1;
    }

    if (opt.command == "list") {
        return cmdList();
    }

    if (!isRunningAsAdmin()) {
        std::cerr << "Administrator privileges are required for read/write/verify on physical disks." << std::endl;
        std::cerr << "Please run this command from an elevated terminal." << std::endl;
        return 1;
    }

    if (opt.device.empty() || opt.image.empty()) {
        std::cerr << "This command requires --device and --image." << std::endl;
        printUsage();
        return 1;
    }

    if (opt.command == "write") {
        return cmdWrite(opt.image, opt.device);
    }
    if (opt.command == "read") {
        const uint64_t bytes = opt.bytesSet ? opt.bytes : 0;
        return cmdRead(opt.image, opt.device, bytes, opt.allocatedOnly);
    }
    if (opt.command == "verify") {
        return cmdVerify(opt.image, opt.device);
    }

    std::cerr << "Unknown command: " << opt.command << std::endl;
    printUsage();
    return 1;
}
