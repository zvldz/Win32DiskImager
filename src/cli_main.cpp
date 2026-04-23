#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <iostream>
#include <limits>
#include <malloc.h>
#include <memory>
#include <string>
#include <vector>

#include <zlib.h>
#include <lzma.h>

#include "iopipeline.h"
#include "version.h"

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
    bool noVerify = false;     // `write` runs verify afterwards by default
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

void printBanner()
{
    std::cout << "Win32DiskImager CLI " APP_VERSION << std::endl;
}

// "2m 15s" / "1h 5m 30s" — used by cmd* summaries and the total-time line
// printed after an auto-chained write+verify.
std::string formatElapsed(double seconds)
{
    uint64_t s = static_cast<uint64_t>(seconds);
    const uint64_t h = s / 3600;
    const uint64_t m = (s / 60) % 60;
    s %= 60;
    char buf[32];
    if (h > 0)      std::snprintf(buf, sizeof(buf), "%lluh %llum %llus",
                                  (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
    else if (m > 0) std::snprintf(buf, sizeof(buf), "%llum %llus",
                                  (unsigned long long)m, (unsigned long long)s);
    else            std::snprintf(buf, sizeof(buf), "%llus",
                                  (unsigned long long)s);
    return buf;
}

void printUsage()
{
    std::cout << "Usage:\n";
    std::cout << "  Win32DiskImager-cli.exe list\n";
    std::cout << "  Win32DiskImager-cli.exe write  --device E: --image C:\\path\\image.img[.gz|.xz] [--no-verify]\n";
    std::cout << "  Win32DiskImager-cli.exe read   --device E: --image C:\\path\\backup.img [--bytes N] [--allocated-only]\n";
    std::cout << "  Win32DiskImager-cli.exe verify --device E: --image C:\\path\\image.img[.gz|.xz]\n";
    std::cout << "\n";
    std::cout << "write / verify accept plain .img, gzipped .img.gz and xz-compressed .img.xz.\n";
    std::cout << "Format is detected by magic bytes, not file extension.\n";
    std::cout << "\n";
    std::cout << "`write` runs a verify pass afterwards by default; pass --no-verify to skip it.\n";
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

HANDLE openPhysicalDisk(DWORD diskNumber, DWORD access, DWORD extraFlags = 0)
{
    const std::wstring path = toPhysicalDrivePath(diskNumber);
    return CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, extraFlags, nullptr);
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

// Transient errors we see when something else grabs the disk briefly right
// after insertion — Google Drive / OneDrive / Explorer indexer all do this.
// Retry on those; fail fast on real problems.
bool isTransientDiskError(DWORD err)
{
    return err == ERROR_DEV_NOT_EXIST       // 55
        || err == ERROR_ACCESS_DENIED       // 5
        || err == ERROR_SHARING_VIOLATION   // 32
        || err == ERROR_LOCK_VIOLATION      // 33
        || err == ERROR_INVALID_FUNCTION;   // 1 (stale handle during replug)
}

bool getDiskGeometry(HANDLE hDisk, DiskGeometry &geometry)
{
    DISK_GEOMETRY_EX dg = {};
    DWORD bytesReturned = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (DeviceIoControl(hDisk,
                            IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                            nullptr, 0, &dg, sizeof(dg),
                            &bytesReturned, nullptr)) {
            geometry.bytesPerSector = static_cast<uint64_t>(dg.Geometry.BytesPerSector);
            geometry.totalBytes     = static_cast<uint64_t>(dg.DiskSize.QuadPart);
            return geometry.bytesPerSector > 0;
        }
        if (!isTransientDiskError(GetLastError())) break;
        Sleep(200);
    }
    return false;
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
        render(processedBytes);
        m_tick = now;
    }

    void done(uint64_t processedBytes)
    {
        render(processedBytes);           // force-draw the last state
        std::cout << '\n';
    }

    double elapsedSeconds() const
    {
        return std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::steady_clock::now() - m_start).count();
    }

private:
    static constexpr int BAR_WIDTH = 20;

    void render(uint64_t processedBytes)
    {
        const auto now   = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration_cast<std::chrono::duration<double>>(now - m_start).count();
        const double mbPerSec = (sec > 0.0)
            ? (static_cast<double>(processedBytes) / (1024.0 * 1024.0) / sec)
            : 0.0;

        char line[160];
        if (m_totalBytes > 0) {
            const double pct = 100.0 * static_cast<double>(processedBytes)
                                     / static_cast<double>(m_totalBytes);
            int filled = static_cast<int>((pct / 100.0) * BAR_WIDTH + 0.5);
            if (filled < 0) filled = 0;
            if (filled > BAR_WIDTH) filled = BAR_WIDTH;

            char bar[BAR_WIDTH + 1];
            for (int i = 0; i < BAR_WIDTH; ++i) bar[i] = (i < filled) ? '#' : '.';
            bar[BAR_WIDTH] = '\0';

            char eta[16] = "--:--";
            if (mbPerSec > 0.0 && processedBytes < m_totalBytes) {
                const double remSec = static_cast<double>(m_totalBytes - processedBytes)
                                    / (mbPerSec * 1024.0 * 1024.0);
                const uint64_t s = static_cast<uint64_t>(remSec);
                if (s >= 3600) {
                    std::snprintf(eta, sizeof(eta), "%u:%02u:%02u",
                                  static_cast<unsigned>(s / 3600),
                                  static_cast<unsigned>((s / 60) % 60),
                                  static_cast<unsigned>(s % 60));
                } else {
                    std::snprintf(eta, sizeof(eta), "%02u:%02u",
                                  static_cast<unsigned>(s / 60),
                                  static_cast<unsigned>(s % 60));
                }
            }

            std::snprintf(line, sizeof(line),
                          "\r[%s] %5.1f%%  %6.2f MB/s  ETA %-8s",
                          bar, pct, mbPerSec, eta);
        } else {
            std::snprintf(line, sizeof(line),
                          "\rProcessed: %8.2f MB  Speed: %6.2f MB/s         ",
                          static_cast<double>(processedBytes) / (1024.0 * 1024.0),
                          mbPerSec);
        }
        std::cout << line << std::flush;
    }

private:
    uint64_t m_totalBytes = 0;
    std::chrono::steady_clock::time_point m_start;
    std::chrono::steady_clock::time_point m_tick;
};

// If hVolume is passed in already valid (inherited from a prior cmdWrite
// that handed off an open+locked+dismounted volume), the open/lock/dismount
// steps are skipped. Closing the gap between Write and auto-Verify keeps
// Google Drive / Windows Search / antivirus from latching onto the freshly-
// written volume the moment a standalone Write would have released it.
bool prepareDiskHandles(const std::string &device,
                        DWORD diskAccess,
                        HANDLE &hVolume,
                        HANDLE &hDisk,
                        DWORD &diskNumber,
                        DiskGeometry &geometry,
                        DWORD diskExtraFlags = 0)
{
    const bool inherited = (hVolume != INVALID_HANDLE_VALUE);
    hDisk = INVALID_HANDLE_VALUE;

    if (!inherited) {
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
    }

    if (!getVolumeDiskNumber(hVolume, diskNumber)) {
        printWinError(L"Failed to map volume to physical disk.", GetLastError());
        if (inherited) unlockVolume(hVolume);
        CloseHandle(hVolume);
        hVolume = INVALID_HANDLE_VALUE;
        return false;
    }

    if (!inherited) {
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
    }

    hDisk = openPhysicalDisk(diskNumber, diskAccess, diskExtraFlags);
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

HANDLE openImageFileWrite(const std::string &path, DWORD extraFlags = 0)
{
    const std::wstring w = widen(path);
    const DWORD flags = extraFlags ? extraFlags : FILE_ATTRIBUTE_NORMAL;
    return CreateFileW(w.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, flags, nullptr);
}

// ---------------------------------------------------------------------------
// Image source abstraction (raw / gz / xz).
// ---------------------------------------------------------------------------
// Parallels the GUI's ImageReader. CLI stays Qt-free, so this is a local copy
// built on top of plain Win32 / zlib / liblzma. See src/imagereader.h for the
// Qt-flavored version used by mainwindow.cpp.

struct ImageSource {
    virtual ~ImageSource() = default;
    virtual int64_t read(void *buf, size_t maxBytes) = 0;
    virtual uint64_t uncompressedSize() const = 0;   // 0 if unknown
    virtual uint64_t compressedPos()    const = 0;
    virtual uint64_t compressedSize()   const = 0;
    virtual std::string errorString()   const = 0;
};

class RawImageSource : public ImageSource {
public:
    ~RawImageSource() override { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    bool open(const std::wstring &path, std::string *err) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) { if (err) *err = "Cannot open image."; return false; }
        LARGE_INTEGER s;
        if (!GetFileSizeEx(h_, &s)) { if (err) *err = "GetFileSizeEx failed."; return false; }
        size_ = (uint64_t)s.QuadPart;
        return true;
    }
    int64_t read(void *buf, size_t maxBytes) override {
        if (h_ == INVALID_HANDLE_VALUE || maxBytes == 0) return 0;
        DWORD got = 0;
        if (!ReadFile(h_, buf, (DWORD)maxBytes, &got, nullptr)) { err_ = "ReadFile failed."; return -1; }
        pos_ += got;
        return (int64_t)got;
    }
    uint64_t uncompressedSize() const override { return size_; }
    uint64_t compressedPos()    const override { return pos_; }
    uint64_t compressedSize()   const override { return size_; }
    std::string errorString()   const override { return err_; }
private:
    HANDLE   h_    = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0;
    uint64_t pos_  = 0;
    std::string err_;
};

class GzImageSource : public ImageSource {
public:
    ~GzImageSource() override { if (gz_) gzclose(gz_); }
    bool open(const std::wstring &path, std::string *err) {
        // Compressed file size
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { if (err) *err = "Cannot open gzip image."; return false; }
        LARGE_INTEGER s; GetFileSizeEx(h, &s);
        compressed_ = (uint64_t)s.QuadPart;
        // ISIZE trailer (last 4 bytes) — mod 2^32 so unreliable for > 4 GB inputs.
        if (compressed_ >= 4) {
            LARGE_INTEGER pos; pos.QuadPart = (LONGLONG)compressed_ - 4;
            SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
            unsigned char b[4]; DWORD got = 0;
            if (ReadFile(h, b, 4, &got, nullptr) && got == 4) {
                const uint32_t isize = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
                                     | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
                if (isize >= compressed_) uncompressed_ = isize;
            }
        }
        CloseHandle(h);
        gz_ = gzopen_w(path.c_str(), "rb");
        if (!gz_) { if (err) *err = "gzopen_w failed."; return false; }
        return true;
    }
    int64_t read(void *buf, size_t maxBytes) override {
        if (!gz_ || maxBytes == 0) return 0;
        int got = gzread(gz_, buf, (unsigned)maxBytes);
        if (got < 0) { int ec = 0; const char *m = gzerror(gz_, &ec); err_ = m ? m : "gz error"; return -1; }
        const z_off_t off = gzoffset(gz_);
        if (off >= 0) compressedPos_ = (uint64_t)off;
        return (int64_t)got;
    }
    uint64_t uncompressedSize() const override { return uncompressed_; }
    uint64_t compressedPos()    const override { return compressedPos_; }
    uint64_t compressedSize()   const override { return compressed_; }
    std::string errorString()   const override { return err_; }
private:
    gzFile   gz_            = nullptr;
    uint64_t compressed_    = 0;
    uint64_t compressedPos_ = 0;
    uint64_t uncompressed_  = 0;
    std::string err_;
};

class XzImageSource : public ImageSource {
public:
    ~XzImageSource() override {
        if (strmInit_) lzma_end(&strm_);
        if (fp_) fclose(fp_);
    }
    bool open(const std::wstring &path, std::string *err) {
        // file size + probe index for uncompressed size
        FILE *probe = _wfopen(path.c_str(), L"rb");
        if (!probe) { if (err) *err = "Cannot open xz image."; return false; }
        _fseeki64(probe, 0, SEEK_END);
        compressed_ = (uint64_t)_ftelli64(probe);
        uncompressed_ = probeXzIndex(probe, compressed_);
        fclose(probe);

        fp_ = _wfopen(path.c_str(), L"rb");
        if (!fp_) { if (err) *err = "Cannot open xz image."; return false; }

        // Try multi-threaded decoder first (up to 8 threads). Same fallback
        // story as the GUI XzImageReader — see src/xzimagereader.cpp.
        lzma_mt mtOpts = {};
        mtOpts.flags              = LZMA_CONCATENATED;
        mtOpts.threads            = std::min(8u, std::max(2u, std::thread::hardware_concurrency()));
        mtOpts.memlimit_threading = UINT64_MAX;
        mtOpts.memlimit_stop      = UINT64_MAX;
        lzma_ret r = lzma_stream_decoder_mt(&strm_, &mtOpts);
        if (r != LZMA_OK) {
            strm_ = LZMA_STREAM_INIT;
            r = lzma_stream_decoder(&strm_, UINT64_MAX, LZMA_CONCATENATED);
        }
        if (r != LZMA_OK) {
            if (err) *err = "lzma_stream_decoder failed.";
            fclose(fp_); fp_ = nullptr; return false;
        }
        strmInit_ = true;
        inBuf_.resize(64 * 1024);
        return true;
    }
    int64_t read(void *buf, size_t maxBytes) override {
        if (!fp_ || maxBytes == 0 || eof_) return 0;
        strm_.next_out  = (unsigned char *)buf;
        strm_.avail_out = maxBytes;
        while (strm_.avail_out > 0 && !eof_) {
            if (strm_.avail_in == 0 && !feof(fp_)) {
                size_t n = fread(inBuf_.data(), 1, inBuf_.size(), fp_);
                if (ferror(fp_)) { err_ = "fread on xz file failed."; return -1; }
                strm_.next_in = inBuf_.data();
                strm_.avail_in = n;
                compressedPos_ += n;
            }
            lzma_action action = feof(fp_) ? LZMA_FINISH : LZMA_RUN;
            lzma_ret rc = lzma_code(&strm_, action);
            if (rc == LZMA_STREAM_END) { eof_ = true; break; }
            if (rc != LZMA_OK) {
                switch (rc) {
                    case LZMA_FORMAT_ERROR: err_ = "Not a valid xz file."; break;
                    case LZMA_DATA_ERROR:   err_ = "Corrupted xz data.";   break;
                    case LZMA_BUF_ERROR:    err_ = "Unexpected end of xz stream."; break;
                    default:                err_ = "xz decoder error.";   break;
                }
                return -1;
            }
        }
        return (int64_t)maxBytes - (int64_t)strm_.avail_out;
    }
    uint64_t uncompressedSize() const override { return uncompressed_; }
    uint64_t compressedPos()    const override { return compressedPos_; }
    uint64_t compressedSize()   const override { return compressed_; }
    std::string errorString()   const override { return err_; }
private:
    static uint64_t probeXzIndex(FILE *f, uint64_t fileSize) {
        if (fileSize < 2 * LZMA_STREAM_HEADER_SIZE) return 0;
        unsigned char footer[LZMA_STREAM_HEADER_SIZE];
        _fseeki64(f, (long long)fileSize - LZMA_STREAM_HEADER_SIZE, SEEK_SET);
        if (fread(footer, 1, LZMA_STREAM_HEADER_SIZE, f) != LZMA_STREAM_HEADER_SIZE) return 0;
        lzma_stream_flags flags;
        if (lzma_stream_footer_decode(&flags, footer) != LZMA_OK) return 0;
        const uint64_t idxSz = flags.backward_size;
        if (idxSz == 0 || idxSz > fileSize - LZMA_STREAM_HEADER_SIZE) return 0;
        std::vector<unsigned char> idx((size_t)idxSz);
        _fseeki64(f, (long long)(fileSize - LZMA_STREAM_HEADER_SIZE - idxSz), SEEK_SET);
        if (fread(idx.data(), 1, (size_t)idxSz, f) != (size_t)idxSz) return 0;
        lzma_index *index = nullptr;
        uint64_t memlimit = UINT64_MAX;
        size_t inPos = 0;
        if (lzma_index_buffer_decode(&index, &memlimit, nullptr,
                                     idx.data(), &inPos, (size_t)idxSz) != LZMA_OK) return 0;
        const uint64_t total = (uint64_t)lzma_index_uncompressed_size(index);
        lzma_index_end(index, nullptr);
        return total;
    }
    FILE       *fp_          = nullptr;
    lzma_stream strm_        = LZMA_STREAM_INIT;
    bool        strmInit_    = false;
    bool        eof_         = false;
    std::vector<unsigned char> inBuf_;
    uint64_t    compressed_    = 0;
    uint64_t    compressedPos_ = 0;
    uint64_t    uncompressed_  = 0;
    std::string err_;
};

// Sniff first bytes to pick the right reader. Magic bytes beat file extension.
std::unique_ptr<ImageSource> openImageSource(const std::string &path, std::string *err)
{
    const std::wstring w = widen(path);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = "Cannot open image file."; return nullptr; }
    unsigned char magic[6] = {0};
    DWORD got = 0;
    ReadFile(h, magic, 6, &got, nullptr);
    CloseHandle(h);

    const bool isGz = (got >= 2) && magic[0] == 0x1F && magic[1] == 0x8B;
    const bool isXz = (got >= 6) && magic[0] == 0xFD && magic[1] == '7'
                   && magic[2] == 'z' && magic[3] == 'X'
                   && magic[4] == 'Z' && magic[5] == 0x00;

    std::unique_ptr<ImageSource> s;
    if (isGz)      s = std::make_unique<GzImageSource>();
    else if (isXz) s = std::make_unique<XzImageSource>();
    else           s = std::make_unique<RawImageSource>();

    bool ok = false;
    if (auto *r = dynamic_cast<RawImageSource *>(s.get())) ok = r->open(w, err);
    else if (auto *g = dynamic_cast<GzImageSource *>(s.get())) ok = g->open(w, err);
    else if (auto *x = dynamic_cast<XzImageSource *>(s.get())) ok = x->open(w, err);
    return ok ? std::move(s) : nullptr;
}

// Some old SDKs / MinGW headers ship without IOCTL_STORAGE_QUERY_PROPERTY.
#ifndef IOCTL_STORAGE_QUERY_PROPERTY
#define IOCTL_STORAGE_QUERY_PROPERTY CTL_CODE(IOCTL_STORAGE_BASE, 0x0500, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

// Bus type + size for `list`. Returns false if the device doesn't answer
// either IOCTL — typical for virtual filesystems (Google Drive, OneDrive,
// Dokany, ...). Caller treats failure as "skip". Size left at 0 on partial
// failure so the list still renders without a capacity column.
bool queryDiskInfo(DWORD diskNumber, BYTE &busTypeOut, uint64_t &sizeBytesOut)
{
    busTypeOut = 0;
    sizeBytesOut = 0;
    HANDLE h = openPhysicalDisk(diskNumber, 0);  // 0 access — query-only
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty, PropertyStandardQuery, {0} };
    BYTE buf[1024] = {0};
    DWORD got = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                         &q, sizeof(q), buf, sizeof(buf), &got, nullptr)
            || got < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        CloseHandle(h);
        return false;
    }
    auto *desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR *>(buf);
    busTypeOut = static_cast<BYTE>(desc->BusType);

    DISK_GEOMETRY_EX dg = {};
    if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                        nullptr, 0, &dg, sizeof(dg), &got, nullptr)) {
        sizeBytesOut = static_cast<uint64_t>(dg.DiskSize.QuadPart);
    }
    CloseHandle(h);
    return true;
}

std::string formatDeviceSize(uint64_t bytes)
{
    if (bytes == 0) return {};
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;
    constexpr double TB = GB * 1024.0;
    const double b = (double)bytes;
    char buf[32];
    if (b >= TB)      std::snprintf(buf, sizeof(buf), "%.1f TB", b / TB);
    else if (b >= GB) std::snprintf(buf, sizeof(buf), "%.1f GB", b / GB);
    else if (b >= MB) std::snprintf(buf, sizeof(buf), "%.0f MB", b / MB);
    else              std::snprintf(buf, sizeof(buf), "%.0f KB", b / KB);
    return buf;
}

// Mirrors the filter the GUI applies in checkDriveType (src/disk.cpp). Only
// devices that look like removable / USB / SD / MMC targets are listed —
// system SSDs and virtual filesystems are skipped so the user can't pick
// one by mistake.
bool isWritableTarget(UINT driveType, BYTE busType)
{
    if (driveType == DRIVE_REMOVABLE) return busType != BusTypeSata;
    if (driveType == DRIVE_FIXED) {
        return busType == BusTypeUsb
            || busType == BusTypeSd
            || busType == BusTypeMmc;
    }
    return false;
}

int cmdList()
{
    const DWORD mask = GetLogicalDrives();
    if (mask == 0) {
        printWinError(L"GetLogicalDrives failed.", GetLastError());
        return 1;
    }

    std::cout << "Available drives:" << std::endl;
    int shown = 0;
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
            continue;  // probe-style: silent skip (matches GUI behavior)
        }
        DWORD diskNumber = 0;
        const bool mapped = getVolumeDiskNumber(hVolume, diskNumber);
        CloseHandle(hVolume);
        if (!mapped) continue;

        BYTE busType = 0;
        uint64_t sizeBytes = 0;
        if (!queryDiskInfo(diskNumber, busType, sizeBytes)) continue;
        if (!isWritableTarget(type, busType)) continue;

        const char *typeStr = (type == DRIVE_REMOVABLE) ? "removable" : "fixed";
        std::cout << "  " << static_cast<char>(letter) << ": ("
                  << typeStr << ", PhysicalDrive" << diskNumber;
        const std::string sizeStr = formatDeviceSize(sizeBytes);
        if (!sizeStr.empty()) std::cout << ", " << sizeStr;
        std::cout << ")" << std::endl;
        ++shown;
    }
    if (shown == 0) {
        std::cout << "  (no removable / USB / SD / MMC targets found)" << std::endl;
    }
    return 0;
}

// If keepVolumeOut is non-null and the write succeeds, ownership of the
// still-locked, still-dismounted volume handle is handed to the caller so
// an auto-verify pass can reuse it without ever releasing the lock.
int cmdWrite(const std::string &imagePath, const std::string &device,
             HANDLE *keepVolumeOut = nullptr)
{
    if (keepVolumeOut) *keepVolumeOut = INVALID_HANDLE_VALUE;
    std::string srcErr;
    std::unique_ptr<ImageSource> src = openImageSource(imagePath, &srcErr);
    if (!src) {
        std::cerr << "Failed to open image: " << srcErr << std::endl;
        return 1;
    }

    HANDLE hVolume = INVALID_HANDLE_VALUE;
    HANDLE hDisk = INVALID_HANDLE_VALUE;
    DWORD diskNumber = 0;
    DiskGeometry geometry;
    // Direct I/O on the destination: honest MB/s, and "Write successful"
    // means data is on the device (no ghost flush after the CLI exits).
    if (!prepareDiskHandles(device, GENERIC_WRITE, hVolume, hDisk, diskNumber, geometry,
                            FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH)) {
        return 1;
    }

    int rc = 0;
    LARGE_INTEGER pos = {};
    if (!SetFilePointerEx(hDisk, pos, nullptr, FILE_BEGIN)) {
        printWinError(L"Failed to seek physical disk.", GetLastError());
        cleanupDiskHandles(hVolume, hDisk);
        return 1;
    }

    const uint64_t imageBytes = src->uncompressedSize();  // 0 if unknown
    if (imageBytes > 0) {
        const uint64_t paddedImageBytes =
            ((imageBytes + geometry.bytesPerSector - 1) / geometry.bytesPerSector) * geometry.bytesPerSector;
        if (paddedImageBytes > geometry.totalBytes) {
            std::cerr << "Image is larger than target disk." << std::endl;
            std::cerr << "Image bytes (padded): " << paddedImageBytes
                      << ", disk bytes: " << geometry.totalBytes << std::endl;
            cleanupDiskHandles(hVolume, hDisk);
            return 1;
        }
    }

    size_t chunk = 4 * 1024 * 1024;
    chunk -= chunk % static_cast<size_t>(geometry.bytesPerSector);
    if (chunk == 0) chunk = static_cast<size_t>(geometry.bytesPerSector);

    // Producer/consumer: decoder thread fills the queue, this thread drains
    // it to the device so decompression can overlap USB/SD writes.
    ChunkQueue queue(4);
    const uint64_t diskCap    = geometry.totalBytes;
    const size_t   sectorSize = (size_t)geometry.bytesPerSector;
    ImageSource *srcPtr = src.get();
    std::thread decoderThread([&queue, srcPtr, chunk, sectorSize, diskCap]() {
        uint64_t produced = 0;
        while (!queue.aborted()) {
            auto c = std::make_unique<IoChunk>();
            if (!c->allocate(chunk, sectorSize)) {
                c->err = "Out of memory allocating I/O buffer.";
                queue.push(std::move(c));
                return;
            }
            const size_t want = (size_t)std::min<uint64_t>(chunk, diskCap - produced);
            if (want == 0) {
                // Device already "full" from the decoder's view — stop clean.
                c->eof = true;
                queue.push(std::move(c));
                return;
            }
            int64_t got = srcPtr->read(c->data, want);
            if (got < 0) {
                c->err = "Image read error: " + srcPtr->errorString();
                queue.push(std::move(c));
                return;
            }
            if (got == 0) {
                c->eof = true;
                queue.push(std::move(c));
                return;
            }
            size_t toWrite = (size_t)got;
            const size_t rem = toWrite % sectorSize;
            if (rem != 0) {
                const size_t pad = sectorSize - rem;
                memset(c->data + toWrite, 0, pad);
                toWrite += pad;
            }
            c->length = toWrite;
            produced += toWrite;
            if (!queue.push(std::move(c))) return;  // aborted mid-push
        }
    });

    uint64_t processed = 0;       // bytes committed to device
    uint64_t compLast  = 0;
    std::cout << "Writing:" << std::endl;
    ProgressPrinter progress(imageBytes > 0 ? imageBytes : src->compressedSize());

    while (true) {
        std::unique_ptr<IoChunk> c = queue.pop();
        if (!c) break;
        if (!c->err.empty()) {
            std::cerr << "\n" << c->err << std::endl;
            rc = 1;
            break;
        }
        if (c->eof) break;

        if (processed + c->length > diskCap) {
            std::cerr << "\nImage exceeds target disk capacity ("
                      << diskCap << " bytes); aborting." << std::endl;
            rc = 1;
            break;
        }

        if (!writeAll(hDisk, c->data, (DWORD)c->length)) {
            printWinError(L"Failed while writing to physical disk.", GetLastError());
            rc = 1;
            break;
        }

        processed += c->length;
        if (imageBytes > 0) {
            progress.update(processed > imageBytes ? imageBytes : processed);
        } else {
            const uint64_t cp = src->compressedPos();
            if (cp != compLast) { progress.update(cp); compLast = cp; }
        }
    }
    queue.requestAbort();
    decoderThread.join();

    FlushFileBuffers(hDisk);
    if (rc == 0 && keepVolumeOut) {
        // Hand the still-locked volume to the chained verify so nothing
        // (Google Drive / indexer / antivirus) grabs the freshly-written
        // volume in the Write → Verify gap.
        if (hDisk != INVALID_HANDLE_VALUE) CloseHandle(hDisk);
        *keepVolumeOut = hVolume;
    } else {
        cleanupDiskHandles(hVolume, hDisk);
    }
    if (rc == 0) {
        progress.done(imageBytes > 0 ? imageBytes : src->compressedSize());
        std::cout << "Write successful. (" << formatElapsed(progress.elapsedSeconds()) << ")" << std::endl;
    }
    return rc;
}

int cmdRead(const std::string &imagePath, const std::string &device, uint64_t requestedBytes, bool allocatedOnly)
{
    // Direct I/O on the destination image file: "Read successful" means the
    // bytes are on disk (no ghost flush after the CLI exits).
    HANDLE hImage = openImageFileWrite(imagePath, FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
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

    size_t chunk = 4 * 1024 * 1024;
    chunk -= chunk % static_cast<size_t>(geometry.bytesPerSector);
    if (chunk == 0) chunk = static_cast<size_t>(geometry.bytesPerSector);

    // Sector-aligned buffer (required by NO_BUFFERING on hImage).
    char *buffer = static_cast<char *>(_aligned_malloc(chunk, (size_t)geometry.bytesPerSector));
    if (!buffer) {
        std::cerr << "Out of memory allocating I/O buffer." << std::endl;
        cleanupDiskHandles(hVolume, hDisk);
        CloseHandle(hImage);
        return 1;
    }

    uint64_t processed = 0;
    int rc = 0;
    std::cout << "Reading:" << std::endl;
    ProgressPrinter progress(bytesToRead);

    while (processed < bytesToRead) {
        const DWORD ask = static_cast<DWORD>((bytesToRead - processed) < chunk ? (bytesToRead - processed) : chunk);
        if (!readExact(hDisk, buffer, ask)) {
            printWinError(L"Failed while reading from physical disk.", GetLastError());
            rc = 1;
            break;
        }

        if (!writeAll(hImage, buffer, ask)) {
            printWinError(L"Failed while writing image file.", GetLastError());
            rc = 1;
            break;
        }

        processed += ask;
        progress.update(processed);
    }

    _aligned_free(buffer);
    FlushFileBuffers(hImage);
    cleanupDiskHandles(hVolume, hDisk);
    CloseHandle(hImage);
    if (rc == 0) {
        progress.done(processed);
        std::cout << "Read successful. (" << formatElapsed(progress.elapsedSeconds()) << ")" << std::endl;
    }
    return rc;
}

// If inheritVolume is passed in valid, it's the locked + dismounted handle
// an auto-verify after Write handed over; prepareDiskHandles reuses it
// instead of re-opening + re-locking.
int cmdVerify(const std::string &imagePath, const std::string &device,
              HANDLE inheritVolume = INVALID_HANDLE_VALUE)
{
    std::string srcErr;
    std::unique_ptr<ImageSource> src = openImageSource(imagePath, &srcErr);
    if (!src) {
        if (inheritVolume != INVALID_HANDLE_VALUE) {
            unlockVolume(inheritVolume);
            CloseHandle(inheritVolume);
        }
        std::cerr << "Failed to open image: " << srcErr << std::endl;
        return 1;
    }

    HANDLE hVolume = inheritVolume;
    HANDLE hDisk = INVALID_HANDLE_VALUE;
    DWORD diskNumber = 0;
    DiskGeometry geometry;
    if (!prepareDiskHandles(device, GENERIC_READ, hVolume, hDisk, diskNumber, geometry)) {
        return 1;
    }

    const uint64_t imageBytes = src->uncompressedSize();  // 0 if unknown
    if (imageBytes > 0) {
        const uint64_t padded =
            ((imageBytes + geometry.bytesPerSector - 1) / geometry.bytesPerSector) * geometry.bytesPerSector;
        if (padded > geometry.totalBytes) {
            std::cerr << "Image is larger than target disk." << std::endl;
            cleanupDiskHandles(hVolume, hDisk);
            return 1;
        }
    }

    LARGE_INTEGER pos = {};
    if (!SetFilePointerEx(hDisk, pos, nullptr, FILE_BEGIN)) {
        printWinError(L"Failed to seek physical disk.", GetLastError());
        cleanupDiskHandles(hVolume, hDisk);
        return 1;
    }

    size_t chunk = 4 * 1024 * 1024;
    chunk -= chunk % static_cast<size_t>(geometry.bytesPerSector);
    if (chunk == 0) chunk = static_cast<size_t>(geometry.bytesPerSector);

    // Producer/consumer: decoder thread fills the file-side queue, this
    // thread reads the disk and memcmp's. Decode overlaps disk-read + cmp.
    ChunkQueue queue(4);
    const uint64_t diskCap    = geometry.totalBytes;
    const size_t   sectorSize = (size_t)geometry.bytesPerSector;
    ImageSource *srcPtr = src.get();
    std::thread decoderThread([&queue, srcPtr, chunk, sectorSize, diskCap]() {
        uint64_t produced = 0;
        while (!queue.aborted()) {
            auto c = std::make_unique<IoChunk>();
            if (!c->allocate(chunk, sectorSize)) {
                c->err = "Out of memory allocating I/O buffer.";
                queue.push(std::move(c));
                return;
            }
            const size_t want = (size_t)std::min<uint64_t>(chunk, diskCap - produced);
            if (want == 0) {
                c->eof = true;
                queue.push(std::move(c));
                return;
            }
            int64_t got = srcPtr->read(c->data, want);
            if (got < 0) {
                c->err = "Image read error: " + srcPtr->errorString();
                queue.push(std::move(c));
                return;
            }
            if (got == 0) {
                c->eof = true;
                queue.push(std::move(c));
                return;
            }
            size_t ask = (size_t)got;
            const size_t rem = ask % sectorSize;
            if (rem != 0) {
                const size_t pad = sectorSize - rem;
                memset(c->data + ask, 0, pad);
                ask += pad;
            }
            c->length = ask;
            produced += ask;
            if (!queue.push(std::move(c))) return;
        }
    });

    std::vector<char> diskBuf(chunk, 0);
    uint64_t processed = 0;
    uint64_t compLast  = 0;
    int rc = 0;
    std::cout << "Verifying:" << std::endl;
    ProgressPrinter progress(imageBytes > 0 ? imageBytes : src->compressedSize());

    while (true) {
        std::unique_ptr<IoChunk> c = queue.pop();
        if (!c) break;
        if (!c->err.empty()) {
            std::cerr << "\n" << c->err << std::endl;
            rc = 1;
            break;
        }
        if (c->eof) break;

        if (processed + c->length > diskCap) {
            std::cerr << "\nImage exceeds target disk capacity; aborting." << std::endl;
            rc = 1;
            break;
        }
        if (!readExact(hDisk, diskBuf.data(), (DWORD)c->length)) {
            printWinError(L"Failed while reading from physical disk.", GetLastError());
            rc = 1;
            break;
        }
        if (memcmp(c->data, diskBuf.data(), c->length) != 0) {
            std::cerr << "\nVerify failed at byte offset " << processed << std::endl;
            rc = 1;
            break;
        }

        processed += c->length;
        if (imageBytes > 0) {
            progress.update(processed > imageBytes ? imageBytes : processed);
        } else {
            const uint64_t cp = src->compressedPos();
            if (cp != compLast) { progress.update(cp); compLast = cp; }
        }
    }
    queue.requestAbort();
    decoderThread.join();

    cleanupDiskHandles(hVolume, hDisk);
    if (rc == 0) {
        progress.done(imageBytes > 0 ? imageBytes : src->compressedSize());
        std::cout << "Verify successful. (" << formatElapsed(progress.elapsedSeconds()) << ")" << std::endl;
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
            std::cout << "Win32DiskImager-cli " APP_VERSION << std::endl;
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
        if (a == "--no-verify") {
            opt.noVerify = true;
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

    printBanner();

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
        const auto totalStart = std::chrono::steady_clock::now();
        HANDLE keepVolume = INVALID_HANDLE_VALUE;
        // When auto-verify is scheduled, ask cmdWrite to hand the locked
        // volume straight to cmdVerify — no third party gets a foothold
        // in the gap.
        const int wrc = cmdWrite(opt.image, opt.device,
                                 opt.noVerify ? nullptr : &keepVolume);
        if (wrc != 0 || opt.noVerify) return wrc;
        std::cout << std::endl;  // blank line between Write and auto-Verify
        const int vrc = cmdVerify(opt.image, opt.device, keepVolume);
        if (vrc == 0) {
            const double total = std::chrono::duration_cast<std::chrono::duration<double>>(
                std::chrono::steady_clock::now() - totalStart).count();
            std::cout << "\nTotal time: " << formatElapsed(total) << std::endl;
        }
        return vrc;
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
