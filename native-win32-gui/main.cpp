#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <winioctl.h>
#include <bcrypt.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Bcrypt.lib")

namespace {
constexpr wchar_t kClassName[] = L"Win32DiskImagerNativeWindowClass";

constexpr int kIdImagePathEdit = 101;
constexpr int kIdBrowseButton = 102;
constexpr int kIdDeviceCombo = 103;
constexpr int kIdRefreshButton = 104;
constexpr int kIdHashTypeCombo = 105;
constexpr int kIdHashGenerateButton = 106;
constexpr int kIdHashCopyButton = 107;
constexpr int kIdHashOutputEdit = 108;
constexpr int kIdAllocatedOnlyCheck = 109;
constexpr int kIdProgressBar = 110;
constexpr int kIdCancelButton = 111;
constexpr int kIdReadButton = 112;
constexpr int kIdWriteButton = 113;
constexpr int kIdVerifyButton = 114;
constexpr int kIdWriteVerifyButton = 115;
constexpr int kIdExitButton = 116;
constexpr int kIdStatusLabel = 117;

constexpr UINT kMsgProgress = WM_APP + 1;
constexpr UINT kMsgOperationDone = WM_APP + 2;

enum HashType {
    HASH_NONE = 0,
    HASH_MD5 = 1,
    HASH_SHA1 = 2,
    HASH_SHA256 = 3
};

enum class OperationType {
    Read,
    Write,
    Verify,
    WriteVerify
};

struct OperationContext {
    OperationType type{};
    std::wstring imagePath;
    wchar_t driveLetter = L'\0';
    bool allocatedOnly = false;
};

struct OperationResult {
    bool success = false;
    bool canceled = false;
    std::wstring title;
    std::wstring message;
};

struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~HandleGuard() {
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};

HWND g_mainWindow = nullptr;
HWND g_imagePathEdit = nullptr;
HWND g_deviceCombo = nullptr;
HWND g_hashTypeCombo = nullptr;
HWND g_hashOutputEdit = nullptr;
HWND g_hashGenerateButton = nullptr;
HWND g_hashCopyButton = nullptr;
HWND g_allocatedOnlyCheck = nullptr;
HWND g_progressBar = nullptr;
HWND g_cancelButton = nullptr;
HWND g_readButton = nullptr;
HWND g_writeButton = nullptr;
HWND g_verifyButton = nullptr;
HWND g_writeVerifyButton = nullptr;
HWND g_browseButton = nullptr;
HWND g_refreshButton = nullptr;
HWND g_statusLabel = nullptr;

std::atomic<bool> g_busy{false};
std::atomic<bool> g_cancelRequested{false};
std::thread g_worker;

HMENU ToMenuId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

std::wstring GetWindowTextString(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(length), L'\0');
    GetWindowTextW(hwnd, text.data(), length + 1);
    return text;
}

void SetStatus(const wchar_t* text) {
    SetWindowTextW(g_statusLabel, text);
}

std::wstring WinErrorMessage(DWORD error) {
    wchar_t* raw = nullptr;
    FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&raw),
        0,
        nullptr);

    std::wstring msg = raw ? raw : L"Unknown error";
    if (raw) {
        LocalFree(raw);
    }
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n')) {
        msg.pop_back();
    }
    return msg;
}

std::wstring WinErrorWithCode(DWORD error) {
    return L"Error " + std::to_wstring(error) + L": " + WinErrorMessage(error);
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

bool IsExistingFile(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void SetBusy(bool busy, const wchar_t* status) {
    g_busy.store(busy);
    EnableWindow(g_cancelButton, busy);
    EnableWindow(g_browseButton, !busy);
    EnableWindow(g_refreshButton, !busy);
    EnableWindow(g_imagePathEdit, !busy);
    EnableWindow(g_deviceCombo, !busy);
    EnableWindow(g_hashTypeCombo, !busy);
    EnableWindow(g_hashGenerateButton, !busy);
    if (!busy) {
        SendMessageW(g_progressBar, PBM_SETPOS, 0, 0);
    }
    SetStatus(status);
}

void UpdateActionButtons() {
    std::wstring imagePath = GetWindowTextString(g_imagePathEdit);
    bool hasPath = !imagePath.empty();
    bool hasDevice = SendMessageW(g_deviceCombo, CB_GETCURSEL, 0, 0) != CB_ERR;
    bool fileExists = IsExistingFile(imagePath);
    int hashChoice = static_cast<int>(SendMessageW(g_hashTypeCombo, CB_GETCURSEL, 0, 0));
    bool hasHash = !GetWindowTextString(g_hashOutputEdit).empty();
    bool busy = g_busy.load();

    EnableWindow(g_readButton, !busy && hasPath && hasDevice);
    EnableWindow(g_writeButton, !busy && hasPath && hasDevice && fileExists);
    EnableWindow(g_verifyButton, !busy && hasPath && hasDevice && fileExists);
    EnableWindow(g_writeVerifyButton, !busy && hasPath && hasDevice && fileExists);
    EnableWindow(g_hashGenerateButton, !busy && fileExists && hashChoice > 0);
    EnableWindow(g_hashCopyButton, !busy && hasHash);
}

wchar_t SelectedDriveLetter() {
    int index = static_cast<int>(SendMessageW(g_deviceCombo, CB_GETCURSEL, 0, 0));
    if (index == CB_ERR) {
        return L'\0';
    }
    wchar_t buf[16] = {};
    SendMessageW(g_deviceCombo, CB_GETLBTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(buf));
    if (wcslen(buf) >= 2 && buf[0] == L'[' && buf[2] == L':') {
        return buf[1];
    }
    return L'\0';
}

void PopulateDevices() {
    SendMessageW(g_deviceCombo, CB_RESETCONTENT, 0, 0);

    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) {
            continue;
        }
        wchar_t root[] = L"A:\\";
        root[0] = static_cast<wchar_t>(L'A' + i);
        UINT driveType = GetDriveTypeW(root);
        if (driveType == DRIVE_REMOVABLE || driveType == DRIVE_FIXED) {
            wchar_t text[16] = {};
            wsprintfW(text, L"[%c:\\]", root[0]);
            SendMessageW(g_deviceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        }
    }

    if (SendMessageW(g_deviceCombo, CB_GETCOUNT, 0, 0) > 0) {
        SendMessageW(g_deviceCombo, CB_SETCURSEL, 0, 0);
        SetStatus(L"Devices refreshed.");
    } else {
        SetStatus(L"No removable/fixed drives detected.");
    }
    UpdateActionButtons();
}

void BrowseForImage(HWND owner) {
    wchar_t fileName[MAX_PATH] = {};
    std::wstring current = GetWindowTextString(g_imagePathEdit);
    if (!current.empty()) {
        wcsncpy_s(fileName, current.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Disk Images (*.img;*.iso)\0*.img;*.iso\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;

    if (GetSaveFileNameW(&ofn)) {
        SetWindowTextW(g_imagePathEdit, fileName);
        SetStatus(L"Image path selected.");
        UpdateActionButtons();
    }
}

bool CopyToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return false;
    }
    EmptyClipboard();
    size_t byteCount = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (!mem) {
        CloseClipboard();
        return false;
    }
    void* ptr = GlobalLock(mem);
    if (!ptr) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    memcpy(ptr, text.c_str(), byteCount);
    GlobalUnlock(mem);
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

bool ComputeFileHash(const std::wstring& path, HashType type, std::wstring& hashText, std::wstring& errorText) {
    LPCWSTR algorithmId = nullptr;
    switch (type) {
    case HASH_MD5:
        algorithmId = BCRYPT_MD5_ALGORITHM;
        break;
    case HASH_SHA1:
        algorithmId = BCRYPT_SHA1_ALGORITHM;
        break;
    case HASH_SHA256:
        algorithmId = BCRYPT_SHA256_ALGORITHM;
        break;
    default:
        errorText = L"Select a hash type first.";
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> hashObject;
    std::vector<UCHAR> hashBytes;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, algorithmId, nullptr, 0);
    if (status < 0) {
        errorText = L"Unable to initialize hashing algorithm.";
        return false;
    }

    DWORD objectSize = 0;
    DWORD hashLength = 0;
    DWORD resultSize = 0;
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &resultSize, 0);
    if (status >= 0) {
        status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &resultSize, 0);
    }
    if (status < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        errorText = L"Unable to query hash properties.";
        return false;
    }

    hashObject.resize(objectSize);
    hashBytes.resize(hashLength);
    status = BCryptCreateHash(algorithm, &hash, hashObject.data(), objectSize, nullptr, 0, 0);
    if (status < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        errorText = L"Unable to create hash object.";
        return false;
    }

    HandleGuard file;
    file.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.h == INVALID_HANDLE_VALUE) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        errorText = L"Unable to open file for hashing.";
        return false;
    }

    std::vector<UCHAR> buffer(1024 * 1024);
    DWORD bytesRead = 0;
    bool ok = true;
    while (ReadFile(file.h, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0) {
        status = BCryptHashData(hash, buffer.data(), bytesRead, 0);
        if (status < 0) {
            ok = false;
            errorText = L"Error while hashing file data.";
            break;
        }
    }
    if (ok && GetLastError() != ERROR_SUCCESS && GetLastError() != ERROR_HANDLE_EOF) {
        ok = false;
        errorText = L"Error while reading file.";
    }
    if (ok) {
        status = BCryptFinishHash(hash, hashBytes.data(), hashLength, 0);
        if (status < 0) {
            ok = false;
            errorText = L"Unable to finalize hash.";
        }
    }

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    if (!ok) {
        return false;
    }

    static const wchar_t hex[] = L"0123456789abcdef";
    hashText.clear();
    hashText.reserve(hashBytes.size() * 2);
    for (unsigned char b : hashBytes) {
        hashText.push_back(hex[(b >> 4) & 0xF]);
        hashText.push_back(hex[b & 0xF]);
    }
    return true;
}

bool SetPointerBySector(HANDLE h, unsigned long long sector, unsigned long long sectorSize) {
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(sector * sectorSize);
    return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) != 0;
}

bool GetDeviceIdFromVolume(HANDLE volume, DWORD& deviceId, std::wstring& error) {
    VOLUME_DISK_EXTENTS extents{};
    DWORD bytes = 0;
    if (!DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &extents, sizeof(extents), &bytes, nullptr)) {
        error = L"Failed to resolve physical device: " + WinErrorMessage(GetLastError());
        return false;
    }
    deviceId = extents.Extents[0].DiskNumber;
    return true;
}

bool GetNumberOfSectors(HANDLE disk, unsigned long long& numSectors, unsigned long long& sectorSize, std::wstring& error) {
    DISK_GEOMETRY_EX geometry{};
    DWORD bytes = 0;
    if (!DeviceIoControl(disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry, sizeof(geometry), &bytes, nullptr)) {
        error = L"Failed to read device geometry: " + WinErrorMessage(GetLastError());
        return false;
    }
    sectorSize = static_cast<unsigned long long>(geometry.Geometry.BytesPerSector);
    numSectors = static_cast<unsigned long long>(geometry.DiskSize.QuadPart) / sectorSize;
    return true;
}

bool GetFileSizeInSectors(HANDLE file, unsigned long long sectorSize, unsigned long long& sectors, std::wstring& error) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        error = L"Failed to read image file size: " + WinErrorMessage(GetLastError());
        return false;
    }
    unsigned long long bytes = static_cast<unsigned long long>(size.QuadPart);
    sectors = bytes / sectorSize + ((bytes % sectorSize) ? 1ull : 0ull);
    return true;
}

bool LockVolume(HANDLE volume, std::wstring& error) {
    DWORD junk = 0;
    for (int i = 0; i < 100; ++i) {
        if (DeviceIoControl(volume, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &junk, nullptr)) {
            return true;
        }
        DWORD e = GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) {
            error = L"Failed to lock volume: " + WinErrorMessage(e);
            return false;
        }
        Sleep(100);
    }
    error = L"Failed to lock volume after retries.";
    return false;
}

void UnlockVolume(HANDLE volume) {
    DWORD junk = 0;
    DeviceIoControl(volume, FSCTL_UNLOCK_VOLUME, nullptr, 0, nullptr, 0, &junk, nullptr);
}

bool DismountVolume(HANDLE volume, std::wstring& error) {
    DWORD junk = 0;
    if (!DeviceIoControl(volume, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &junk, nullptr)) {
        error = L"Failed to dismount volume: " + WinErrorMessage(GetLastError());
        return false;
    }
    return true;
}

void PostProgress(unsigned long long done, unsigned long long total) {
    if (total == 0) {
        return;
    }
    int pct = static_cast<int>((done * 100ull) / total);
    if (pct > 100) {
        pct = 100;
    }
    PostMessageW(g_mainWindow, kMsgProgress, static_cast<WPARAM>(pct), 0);
}

bool ValidateDeviceAndPath(HWND owner, bool requireExistingFile) {
    std::wstring imagePath = GetWindowTextString(g_imagePathEdit);
    if (imagePath.empty()) {
        MessageBoxW(owner, L"Select an image file path first.", L"Missing Image Path", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (requireExistingFile && !IsExistingFile(imagePath)) {
        MessageBoxW(owner, L"Selected image file does not exist.", L"Missing Image File", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (SelectedDriveLetter() == L'\0') {
        MessageBoxW(owner, L"Select a destination drive first.", L"Missing Device", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

void FinishAndPost(std::unique_ptr<OperationResult> result) {
    PostMessageW(g_mainWindow, kMsgOperationDone, 0, reinterpret_cast<LPARAM>(result.release()));
}

void RunRead(OperationContext ctx, OperationResult& result) {
    HandleGuard volume;
    HandleGuard rawDisk;
    HandleGuard imageFile;
    std::wstring error;

    wchar_t volumePath[] = L"\\\\.\\A:";
    volumePath[4] = ctx.driveLetter;
    volume.h = CreateFileW(volumePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume.h == INVALID_HANDLE_VALUE) {
        result.message = L"Failed to open volume: " + WinErrorMessage(GetLastError());
        return;
    }
    if (!LockVolume(volume.h, error) || !DismountVolume(volume.h, error)) {
        result.message = error;
        return;
    }

    DWORD diskId = 0;
    if (!GetDeviceIdFromVolume(volume.h, diskId, error)) {
        result.message = error;
        UnlockVolume(volume.h);
        return;
    }

    wchar_t rawPath[64] = {};
    wsprintfW(rawPath, L"\\\\.\\PhysicalDrive%lu", diskId);
    rawDisk.h = CreateFileW(rawPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (rawDisk.h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        result.message = L"Failed to open raw disk. " + WinErrorWithCode(err) +
                         L"\nRun Win32DiskImagerNative as Administrator and retry.";
        UnlockVolume(volume.h);
        return;
    }

    imageFile.h = CreateFileW(ctx.imagePath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (imageFile.h == INVALID_HANDLE_VALUE) {
        result.message = L"Failed to create image file: " + WinErrorMessage(GetLastError());
        UnlockVolume(volume.h);
        return;
    }

    unsigned long long numSectors = 0;
    unsigned long long sectorSize = 0;
    if (!GetNumberOfSectors(rawDisk.h, numSectors, sectorSize, error)) {
        result.message = error;
        UnlockVolume(volume.h);
        return;
    }

    if (ctx.allocatedOnly) {
        std::vector<char> mbr(512, 0);
        DWORD bytesRead = 0;
        if (SetPointerBySector(rawDisk.h, 0, 512) && ReadFile(rawDisk.h, mbr.data(), 512, &bytesRead, nullptr) && bytesRead == 512) {
            if (static_cast<unsigned char>(mbr[510]) == 0x55 && static_cast<unsigned char>(mbr[511]) == 0xAA) {
                uint32_t maxEnd = 0;
                for (int i = 0; i < 4; ++i) {
                    size_t off = 0x1BE + i * 16;
                    uint32_t start = 0;
                    uint32_t count = 0;
                    memcpy(&start, mbr.data() + off + 8, sizeof(uint32_t));
                    memcpy(&count, mbr.data() + off + 12, sizeof(uint32_t));
                    if (count > 0 && start + count > maxEnd) {
                        maxEnd = start + count;
                    }
                }
                if (maxEnd > 0 && maxEnd < numSectors) {
                    numSectors = maxEnd;
                }
            }
        }
    }

    std::vector<char> buffer(static_cast<size_t>(sectorSize * 1024ull), 0);
    for (unsigned long long i = 0; i < numSectors; i += 1024ull) {
        if (g_cancelRequested.load()) {
            result.canceled = true;
            result.message = L"Read canceled.";
            UnlockVolume(volume.h);
            return;
        }
        unsigned long long chunkSectors = (numSectors - i >= 1024ull) ? 1024ull : (numSectors - i);
        DWORD bytesToTransfer = static_cast<DWORD>(chunkSectors * sectorSize);
        DWORD bytesRead = 0;
        DWORD bytesWritten = 0;

        if (!SetPointerBySector(rawDisk.h, i, sectorSize) || !ReadFile(rawDisk.h, buffer.data(), bytesToTransfer, &bytesRead, nullptr)) {
            result.message = L"Read from device failed at sector " + std::to_wstring(i) + L": " + WinErrorMessage(GetLastError());
            UnlockVolume(volume.h);
            return;
        }
        if (bytesRead < bytesToTransfer) {
            memset(buffer.data() + bytesRead, 0, bytesToTransfer - bytesRead);
        }
        if (!WriteFile(imageFile.h, buffer.data(), bytesToTransfer, &bytesWritten, nullptr) || bytesWritten != bytesToTransfer) {
            result.message = L"Write to image file failed at sector " + std::to_wstring(i) + L": " + WinErrorMessage(GetLastError());
            UnlockVolume(volume.h);
            return;
        }
        PostProgress(i + chunkSectors, numSectors);
    }

    UnlockVolume(volume.h);
    result.success = true;
    result.message = L"Read successful.";
}

void RunWrite(OperationContext ctx, OperationResult& result) {
    HandleGuard volume;
    HandleGuard rawDisk;
    HandleGuard imageFile;
    std::wstring error;

    imageFile.h = CreateFileW(ctx.imagePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (imageFile.h == INVALID_HANDLE_VALUE) {
        result.message = L"Failed to open image file: " + WinErrorMessage(GetLastError());
        return;
    }

    wchar_t volumePath[] = L"\\\\.\\A:";
    volumePath[4] = ctx.driveLetter;
    volume.h = CreateFileW(volumePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume.h == INVALID_HANDLE_VALUE) {
        result.message = L"Failed to open volume: " + WinErrorMessage(GetLastError());
        return;
    }
    if (!LockVolume(volume.h, error) || !DismountVolume(volume.h, error)) {
        result.message = error;
        return;
    }

    DWORD diskId = 0;
    if (!GetDeviceIdFromVolume(volume.h, diskId, error)) {
        result.message = error;
        UnlockVolume(volume.h);
        return;
    }

    wchar_t rawPath[64] = {};
    wsprintfW(rawPath, L"\\\\.\\PhysicalDrive%lu", diskId);
    rawDisk.h = CreateFileW(rawPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (rawDisk.h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        result.message = L"Failed to open raw disk. " + WinErrorWithCode(err) +
                         L"\nRun Win32DiskImagerNative as Administrator and retry.";
        UnlockVolume(volume.h);
        return;
    }

    unsigned long long availableSectors = 0;
    unsigned long long sectorSize = 0;
    unsigned long long imageSectors = 0;
    if (!GetNumberOfSectors(rawDisk.h, availableSectors, sectorSize, error)) {
        result.message = error;
        UnlockVolume(volume.h);
        return;
    }
    if (!GetFileSizeInSectors(imageFile.h, sectorSize, imageSectors, error)) {
        result.message = error;
        UnlockVolume(volume.h);
        return;
    }
    if (imageSectors > availableSectors) {
        result.message = L"Image is larger than target device.";
        UnlockVolume(volume.h);
        return;
    }

    std::vector<char> buffer(static_cast<size_t>(sectorSize * 1024ull), 0);
    for (unsigned long long i = 0; i < imageSectors; i += 1024ull) {
        if (g_cancelRequested.load()) {
            result.canceled = true;
            result.message = L"Write canceled.";
            UnlockVolume(volume.h);
            return;
        }
        unsigned long long chunkSectors = (imageSectors - i >= 1024ull) ? 1024ull : (imageSectors - i);
        DWORD bytesToTransfer = static_cast<DWORD>(chunkSectors * sectorSize);
        DWORD bytesRead = 0;
        DWORD bytesWritten = 0;

        memset(buffer.data(), 0, bytesToTransfer);
        if (!ReadFile(imageFile.h, buffer.data(), bytesToTransfer, &bytesRead, nullptr)) {
            result.message = L"Read from image failed at sector " + std::to_wstring(i) + L": " + WinErrorMessage(GetLastError());
            UnlockVolume(volume.h);
            return;
        }
        if (!SetPointerBySector(rawDisk.h, i, sectorSize) || !WriteFile(rawDisk.h, buffer.data(), bytesToTransfer, &bytesWritten, nullptr) || bytesWritten != bytesToTransfer) {
            result.message = L"Write to device failed at sector " + std::to_wstring(i) + L": " + WinErrorMessage(GetLastError());
            UnlockVolume(volume.h);
            return;
        }
        PostProgress(i + chunkSectors, imageSectors);
    }

    UnlockVolume(volume.h);
    result.success = true;
    result.message = L"Write successful.";
}

void RunVerify(OperationContext ctx, OperationResult& result) {
    HandleGuard volume;
    HandleGuard rawDisk;
    HandleGuard imageFile;
    std::wstring error;

    imageFile.h = CreateFileW(ctx.imagePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (imageFile.h == INVALID_HANDLE_VALUE) {
        result.message = L"Failed to open image file: " + WinErrorMessage(GetLastError());
        return;
    }

    wchar_t volumePath[] = L"\\\\.\\A:";
    volumePath[4] = ctx.driveLetter;
    volume.h = CreateFileW(volumePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume.h == INVALID_HANDLE_VALUE) {
        result.message = L"Failed to open volume: " + WinErrorMessage(GetLastError());
        return;
    }
    if (!LockVolume(volume.h, error) || !DismountVolume(volume.h, error)) {
        result.message = error;
        return;
    }

    DWORD diskId = 0;
    if (!GetDeviceIdFromVolume(volume.h, diskId, error)) {
        result.message = error;
        UnlockVolume(volume.h);
        return;
    }

    wchar_t rawPath[64] = {};
    wsprintfW(rawPath, L"\\\\.\\PhysicalDrive%lu", diskId);
    rawDisk.h = CreateFileW(rawPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (rawDisk.h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        result.message = L"Failed to open raw disk. " + WinErrorWithCode(err) +
                         L"\nRun Win32DiskImagerNative as Administrator and retry.";
        UnlockVolume(volume.h);
        return;
    }

    unsigned long long availableSectors = 0;
    unsigned long long sectorSize = 0;
    unsigned long long imageSectors = 0;
    if (!GetNumberOfSectors(rawDisk.h, availableSectors, sectorSize, error)) {
        result.message = error;
        UnlockVolume(volume.h);
        return;
    }
    if (!GetFileSizeInSectors(imageFile.h, sectorSize, imageSectors, error)) {
        result.message = error;
        UnlockVolume(volume.h);
        return;
    }
    if (imageSectors > availableSectors) {
        result.message = L"Image is larger than target device; verify cannot continue.";
        UnlockVolume(volume.h);
        return;
    }

    std::vector<char> fileBuffer(static_cast<size_t>(sectorSize * 1024ull), 0);
    std::vector<char> diskBuffer(static_cast<size_t>(sectorSize * 1024ull), 0);
    for (unsigned long long i = 0; i < imageSectors; i += 1024ull) {
        if (g_cancelRequested.load()) {
            result.canceled = true;
            result.message = L"Verify canceled.";
            UnlockVolume(volume.h);
            return;
        }
        unsigned long long chunkSectors = (imageSectors - i >= 1024ull) ? 1024ull : (imageSectors - i);
        DWORD bytesToTransfer = static_cast<DWORD>(chunkSectors * sectorSize);
        DWORD bytesReadFile = 0;
        DWORD bytesReadDisk = 0;

        memset(fileBuffer.data(), 0, bytesToTransfer);
        if (!ReadFile(imageFile.h, fileBuffer.data(), bytesToTransfer, &bytesReadFile, nullptr)) {
            result.message = L"Read from image failed at sector " + std::to_wstring(i) + L": " + WinErrorMessage(GetLastError());
            UnlockVolume(volume.h);
            return;
        }
        if (!SetPointerBySector(rawDisk.h, i, sectorSize) || !ReadFile(rawDisk.h, diskBuffer.data(), bytesToTransfer, &bytesReadDisk, nullptr)) {
            result.message = L"Read from device failed at sector " + std::to_wstring(i) + L": " + WinErrorMessage(GetLastError());
            UnlockVolume(volume.h);
            return;
        }
        if (bytesReadDisk < bytesToTransfer) {
            memset(diskBuffer.data() + bytesReadDisk, 0, bytesToTransfer - bytesReadDisk);
        }
        if (memcmp(fileBuffer.data(), diskBuffer.data(), bytesToTransfer) != 0) {
            result.message = L"Verification failed at sector " + std::to_wstring(i) + L".";
            UnlockVolume(volume.h);
            return;
        }
        PostProgress(i + chunkSectors, imageSectors);
    }

    UnlockVolume(volume.h);
    result.success = true;
    result.message = L"Verify successful.";
}

void RunOperation(OperationContext ctx) {
    auto result = std::make_unique<OperationResult>();
    result->title = L"Operation";

    if (ctx.type == OperationType::Read) {
        result->title = L"Read";
        RunRead(ctx, *result);
    } else if (ctx.type == OperationType::Write) {
        result->title = L"Write";
        RunWrite(ctx, *result);
    } else if (ctx.type == OperationType::WriteVerify) {
        result->title = L"Write + Verify";
        RunWrite(ctx, *result);
        if (result->success && !result->canceled) {
            OperationResult verifyResult{};
            verifyResult.title = L"Write + Verify";
            RunVerify(ctx, verifyResult);
            if (!verifyResult.success || verifyResult.canceled) {
                result->success = verifyResult.success;
                result->canceled = verifyResult.canceled;
                result->message = verifyResult.message;
            } else {
                result->message = L"Write and verify successful.";
            }
        }
    } else {
        result->title = L"Verify";
        RunVerify(ctx, *result);
    }

    FinishAndPost(std::move(result));
}

void StartOperation(HWND owner, OperationType type) {
    if (!IsProcessElevated()) {
        MessageBoxW(
            owner,
            L"Raw disk access requires Administrator privileges.\n"
            L"Close this app and re-run it with 'Run as administrator'.",
            L"Administrator Required",
            MB_OK | MB_ICONWARNING);
        return;
    }

    bool requireExisting = type != OperationType::Read;
    if (!ValidateDeviceAndPath(owner, requireExisting)) {
        return;
    }

    std::wstring imagePath = GetWindowTextString(g_imagePathEdit);
    wchar_t driveLetter = SelectedDriveLetter();
    if (driveLetter == L'\0') {
        MessageBoxW(owner, L"Please select a device.", L"Device", MB_OK | MB_ICONWARNING);
        return;
    }

    if (type == OperationType::Write || type == OperationType::WriteVerify) {
        int confirm = MessageBoxW(owner, L"Writing can permanently overwrite data on the selected device.\nContinue?", L"Confirm Write", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (confirm != IDYES) {
            return;
        }
    } else if (type == OperationType::Read && IsExistingFile(imagePath)) {
        int overwrite = MessageBoxW(owner, L"The destination image file already exists.\nOverwrite it?", L"Confirm Overwrite", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (overwrite != IDYES) {
            return;
        }
    }

    if (g_worker.joinable()) {
        g_worker.join();
    }

    g_cancelRequested.store(false);
    const wchar_t* busyText = L"Verifying...";
    if (type == OperationType::Read) {
        busyText = L"Reading...";
    } else if (type == OperationType::Write) {
        busyText = L"Writing...";
    } else if (type == OperationType::WriteVerify) {
        busyText = L"Writing then verifying...";
    }
    SetBusy(true, busyText);
    UpdateActionButtons();

    OperationContext ctx{};
    ctx.type = type;
    ctx.imagePath = imagePath;
    ctx.driveLetter = driveLetter;
    ctx.allocatedOnly = SendMessageW(g_allocatedOnlyCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;

    g_worker = std::thread([ctx]() { RunOperation(ctx); });
}

void HandleGenerateHash(HWND owner) {
    std::wstring imagePath = GetWindowTextString(g_imagePathEdit);
    if (!IsExistingFile(imagePath)) {
        MessageBoxW(owner, L"Select an existing image file first.", L"Hash", MB_OK | MB_ICONWARNING);
        return;
    }
    int choice = static_cast<int>(SendMessageW(g_hashTypeCombo, CB_GETCURSEL, 0, 0));
    if (choice <= 0) {
        MessageBoxW(owner, L"Select hash type MD5, SHA1, or SHA256.", L"Hash", MB_OK | MB_ICONWARNING);
        return;
    }

    SetStatus(L"Generating hash...");
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    std::wstring hashText;
    std::wstring errorText;
    bool ok = ComputeFileHash(imagePath, static_cast<HashType>(choice), hashText, errorText);
    if (ok) {
        SetWindowTextW(g_hashOutputEdit, hashText.c_str());
        SetStatus(L"Hash generated.");
    } else {
        SetWindowTextW(g_hashOutputEdit, L"");
        MessageBoxW(owner, errorText.c_str(), L"Hash Error", MB_OK | MB_ICONERROR);
        SetStatus(L"Hash generation failed.");
    }
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    UpdateActionButtons();
}

void HandleCancel() {
    if (!g_busy.load()) {
        return;
    }
    g_cancelRequested.store(true);
    SetStatus(L"Cancel requested...");
}

void CreateUi(HWND hwnd) {
    HFONT uiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    HWND imageGroup = CreateWindowW(L"BUTTON", L"Image File", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 12, 10, 740, 64, hwnd, nullptr, nullptr, nullptr);
    g_imagePathEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 24, 36, 610, 24, hwnd, ToMenuId(kIdImagePathEdit), nullptr, nullptr);
    g_browseButton = CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 644, 36, 96, 24, hwnd, ToMenuId(kIdBrowseButton), nullptr, nullptr);

    HWND deviceGroup = CreateWindowW(L"BUTTON", L"Device", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 12, 80, 360, 100, hwnd, nullptr, nullptr, nullptr);
    g_deviceCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 24, 106, 250, 300, hwnd, ToMenuId(kIdDeviceCombo), nullptr, nullptr);
    g_refreshButton = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 282, 106, 78, 24, hwnd, ToMenuId(kIdRefreshButton), nullptr, nullptr);
    g_allocatedOnlyCheck = CreateWindowW(L"BUTTON", L"Read Only Allocated Partitions", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 24, 140, 250, 24, hwnd, ToMenuId(kIdAllocatedOnlyCheck), nullptr, nullptr);

    HWND hashGroup = CreateWindowW(L"BUTTON", L"Hash", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 380, 80, 372, 100, hwnd, nullptr, nullptr, nullptr);
    g_hashTypeCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 392, 106, 90, 300, hwnd, ToMenuId(kIdHashTypeCombo), nullptr, nullptr);
    g_hashGenerateButton = CreateWindowW(L"BUTTON", L"Generate", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 490, 106, 80, 24, hwnd, ToMenuId(kIdHashGenerateButton), nullptr, nullptr);
    g_hashCopyButton = CreateWindowW(L"BUTTON", L"Copy", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 578, 106, 70, 24, hwnd, ToMenuId(kIdHashCopyButton), nullptr, nullptr);
    g_hashOutputEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY, 392, 138, 348, 24, hwnd, ToMenuId(kIdHashOutputEdit), nullptr, nullptr);

    HWND progressGroup = CreateWindowW(L"BUTTON", L"Progress", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 12, 182, 740, 66, hwnd, nullptr, nullptr, nullptr);
    g_progressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 24, 208, 716, 24, hwnd, ToMenuId(kIdProgressBar), nullptr, nullptr);
    SendMessageW(g_progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(g_progressBar, PBM_SETPOS, 0, 0);

    g_cancelButton = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 12, 260, 92, 28, hwnd, ToMenuId(kIdCancelButton), nullptr, nullptr);
    g_readButton = CreateWindowW(L"BUTTON", L"Read", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 366, 260, 68, 28, hwnd, ToMenuId(kIdReadButton), nullptr, nullptr);
    g_writeButton = CreateWindowW(L"BUTTON", L"Write", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 440, 260, 68, 28, hwnd, ToMenuId(kIdWriteButton), nullptr, nullptr);
    g_writeVerifyButton = CreateWindowW(L"BUTTON", L"Write + Verify", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 514, 260, 96, 28, hwnd, ToMenuId(kIdWriteVerifyButton), nullptr, nullptr);
    g_verifyButton = CreateWindowW(L"BUTTON", L"Verify Only", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 616, 260, 80, 28, hwnd, ToMenuId(kIdVerifyButton), nullptr, nullptr);
    HWND exitButton = CreateWindowW(L"BUTTON", L"Exit", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 702, 260, 50, 28, hwnd, ToMenuId(kIdExitButton), nullptr, nullptr);
    g_statusLabel = CreateWindowW(L"STATIC", L"Ready.", WS_CHILD | WS_VISIBLE, 12, 296, 740, 22, hwnd, ToMenuId(kIdStatusLabel), nullptr, nullptr);

    for (HWND control : {imageGroup, g_imagePathEdit, g_browseButton, deviceGroup, g_deviceCombo, g_refreshButton, hashGroup,
                         g_hashTypeCombo, g_hashGenerateButton, g_hashCopyButton, g_hashOutputEdit, g_allocatedOnlyCheck,
                         progressGroup, g_progressBar, g_cancelButton, g_readButton, g_writeButton, g_writeVerifyButton, g_verifyButton, exitButton, g_statusLabel}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(uiFont), TRUE);
    }

    SendMessageW(g_hashTypeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"None"));
    SendMessageW(g_hashTypeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MD5"));
    SendMessageW(g_hashTypeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"SHA1"));
    SendMessageW(g_hashTypeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"SHA256"));
    SendMessageW(g_hashTypeCombo, CB_SETCURSEL, 0, 0);

    PopulateDevices();
    EnableWindow(g_cancelButton, FALSE);
    UpdateActionButtons();
}
}  // namespace

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_mainWindow = hwnd;
        CreateUi(hwnd);
        return 0;
    case WM_COMMAND: {
        int controlId = LOWORD(wParam);
        int notifyCode = HIWORD(wParam);
        if (controlId == kIdBrowseButton && notifyCode == BN_CLICKED) {
            BrowseForImage(hwnd);
        } else if (controlId == kIdRefreshButton && notifyCode == BN_CLICKED) {
            PopulateDevices();
        } else if (controlId == kIdHashGenerateButton && notifyCode == BN_CLICKED) {
            HandleGenerateHash(hwnd);
        } else if (controlId == kIdHashCopyButton && notifyCode == BN_CLICKED) {
            std::wstring hash = GetWindowTextString(g_hashOutputEdit);
            if (!hash.empty() && CopyToClipboard(hwnd, hash)) {
                SetStatus(L"Hash copied to clipboard.");
            }
        } else if (controlId == kIdReadButton && notifyCode == BN_CLICKED) {
            StartOperation(hwnd, OperationType::Read);
        } else if (controlId == kIdWriteButton && notifyCode == BN_CLICKED) {
            StartOperation(hwnd, OperationType::Write);
        } else if (controlId == kIdWriteVerifyButton && notifyCode == BN_CLICKED) {
            StartOperation(hwnd, OperationType::WriteVerify);
        } else if (controlId == kIdVerifyButton && notifyCode == BN_CLICKED) {
            StartOperation(hwnd, OperationType::Verify);
        } else if (controlId == kIdCancelButton && notifyCode == BN_CLICKED) {
            HandleCancel();
        } else if (controlId == kIdExitButton && notifyCode == BN_CLICKED) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else if (controlId == kIdImagePathEdit && notifyCode == EN_CHANGE) {
            UpdateActionButtons();
        } else if (controlId == kIdDeviceCombo && notifyCode == CBN_SELCHANGE) {
            UpdateActionButtons();
        } else if (controlId == kIdHashTypeCombo && notifyCode == CBN_SELCHANGE) {
            UpdateActionButtons();
        }
        return 0;
    }
    case kMsgProgress:
        SendMessageW(g_progressBar, PBM_SETPOS, wParam, 0);
        return 0;
    case kMsgOperationDone: {
        std::unique_ptr<OperationResult> result(reinterpret_cast<OperationResult*>(lParam));
        if (g_worker.joinable()) {
            g_worker.join();
        }
        SetBusy(false, result->canceled ? L"Operation canceled." : (result->success ? L"Done." : L"Operation failed."));
        UpdateActionButtons();

        UINT icon = result->success ? MB_ICONINFORMATION : MB_ICONERROR;
        if (result->canceled) {
            icon = MB_ICONWARNING;
        }
        MessageBoxW(hwnd, result->message.c_str(), result->title.c_str(), MB_OK | icon);
        return 0;
    }
    case WM_CLOSE:
        if (g_busy.load()) {
            int answer = MessageBoxW(hwnd, L"An operation is running. Cancel and exit?", L"Exit", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (answer == IDYES) {
                g_cancelRequested.store(true);
            }
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_worker.joinable()) {
            g_worker.join();
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc)) {
        return 1;
    }

    HWND window = CreateWindowExW(
        0,
        kClassName,
        L"Win32DiskImager Native (Win32 API)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        780,
        370,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!window) {
        return 1;
    }

    ShowWindow(window, showCmd);
    UpdateWindow(window);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
