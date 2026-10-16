// ==WindhawkMod==
// @id              copy-queue-per-disk
// @name            Copy Queue - Per Disk (Intelligent Drive-Aware)
// @description     Queues Explorer copy operations per physical disk/drive, allowing independent drives to copy simultaneously in parallel.
// @version         1.0.0
// @author          Nerdworld & Jakub
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -luuid
// @license         MIT
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Intelligent Per-Disk Sequential Explorer Copy Queue

This mod queues Windows Explorer copy operations **per physical disk/drive**, rather than using a single global bottleneck.

### How it works:
- **Independent transfers run in PARALLEL:** Copying from SSD to Pendrive while simultaneously copying between two other drives runs concurrently at full speed.
- **Conflicting transfers are QUEUED:** If an operation shares a physical disk with an ongoing copy (same destination drive, same source drive, or cross-read/write), it waits until that disk is free.
- **Physical Drive Detection:** Identifies physical drive numbers (e.g. partition `C:` and `D:` on the same NVMe/HDD are recognized as the same physical device).
- **Deadlock Free:** Guarantees strict lexicographical lock ordering across processes.
*/
// ==/WindhawkModReadme==

#include <windows.h>
#include <winioctl.h>
#include <objbase.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// IFileOperation vtable indices
constexpr size_t kCopyItemIndex = 16;
constexpr size_t kCopyItemsIndex = 17;
constexpr size_t kPerformOperationsIndex = 21;

using CopyItem_t = HRESULT(STDMETHODCALLTYPE*)(
    IFileOperation* self,
    IShellItem* source,
    IShellItem* destinationFolder,
    LPCWSTR copyName,
    IFileOperationProgressSink* progressSink);

using CopyItems_t = HRESULT(STDMETHODCALLTYPE*)(
    IFileOperation* self,
    IUnknown* items,
    IShellItem* destinationFolder);

using PerformOperations_t = HRESULT(STDMETHODCALLTYPE*)(IFileOperation* self);

CopyItem_t g_copyItemOriginal = nullptr;
CopyItems_t g_copyItemsOriginal = nullptr;
PerformOperations_t g_performOperationsOriginal = nullptr;

SRWLOCK g_stateLock = SRWLOCK_INIT;
CONDITION_VARIABLE g_queueChanged = CONDITION_VARIABLE_INIT;

// Tracks operations and their detected disk keys
std::unordered_map<IFileOperation*, std::set<std::wstring>> g_pendingDisks;

HANDLE g_shutdownEvent = nullptr;
std::atomic_bool g_unloading = false;
thread_local bool g_insidePerformHook = false;

uint64_t g_nextOperationId = 1;

enum class TurnState {
    Waiting,
    AcquiringCrossProcessMutex,
    Ready,
    Aborted,
};

struct QueueEntry {
    uint64_t id = 0;
    std::set<std::wstring> diskKeys;
    std::vector<HANDLE> mutexHandles;
    TurnState state = TurnState::Waiting;
    bool queued = false;
    bool ownsCrossProcessMutexes = false;
};

std::deque<QueueEntry*> g_queue;

struct HookTargets {
    HRESULT result = E_FAIL;
    void* copyItem = nullptr;
    void* copyItems = nullptr;
    void* performOperations = nullptr;
};

// --- Disk Detection Helpers ---

std::wstring SanitizeForMutex(const std::wstring& str) {
    std::wstring result = str;
    for (wchar_t& ch : result) {
        if (!iswalnum(ch) && ch != L'_' && ch != L'-') {
            ch = L'_';
        }
    }
    return result;
}

std::wstring GetDiskKeyFromPath(const std::wstring& path) {
    if (path.empty()) {
        return L"Generic";
    }

    // UNC Network Path: \\server\share\...
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        size_t nextSlash = path.find(L'\\', 2);
        std::wstring server = (nextSlash != std::wstring::npos)
            ? path.substr(2, nextSlash - 2)
            : path.substr(2);
        std::transform(server.begin(), server.end(), server.begin(), ::towlower);
        return L"Net_" + SanitizeForMutex(server);
    }

    // Determine volume root (e.g. "C:\")
    WCHAR volumeRoot[MAX_PATH] = { 0 };
    if (!GetVolumePathNameW(path.c_str(), volumeRoot, MAX_PATH)) {
        if (path.size() >= 2 && path[1] == L':') {
            WCHAR letter = static_cast<WCHAR>(towupper(path[0]));
            return std::wstring(L"Drive_") + letter;
        }
        return L"Generic";
    }

    // Query physical device number via IOCTL
    size_t rootLen = wcslen(volumeRoot);
    if (rootLen > 0 && volumeRoot[rootLen - 1] == L'\\') {
        volumeRoot[rootLen - 1] = L'\0';
    }

    WCHAR volumeDevice[MAX_PATH] = { 0 };
    swprintf_s(volumeDevice, L"\\\\.\\%s", volumeRoot);

    HANDLE hDevice = CreateFileW(
        volumeDevice,
        0, // No access right required for query
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (hDevice != INVALID_HANDLE_VALUE) {
        STORAGE_DEVICE_NUMBER sdn = {};
        DWORD bytesReturned = 0;
        if (DeviceIoControl(
                hDevice,
                IOCTL_STORAGE_GET_DEVICE_NUMBER,
                nullptr,
                0,
                &sdn,
                sizeof(sdn),
                &bytesReturned,
                nullptr)) {
            CloseHandle(hDevice);
            return L"Disk_" + std::to_wstring(sdn.DeviceNumber);
        }
        CloseHandle(hDevice);
    }

    // Fallback: Drive letter
    if (rootLen > 0 && volumeRoot[1] == L':') {
        WCHAR letter = static_cast<WCHAR>(towupper(volumeRoot[0]));
        return std::wstring(L"Drive_") + letter;
    }

    return L"Generic";
}

std::wstring GetDiskKeyFromShellItem(IShellItem* item) {
    if (!item) return L"";
    PWSTR pszPath = nullptr;
    HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    if (FAILED(hr) || !pszPath) {
        return L"";
    }
    std::wstring path(pszPath);
    CoTaskMemFree(pszPath);
    return GetDiskKeyFromPath(path);
}

void RegisterOperationDisks(IFileOperation* op, const std::wstring& key) {
    if (key.empty()) return;
    AcquireSRWLockExclusive(&g_stateLock);
    g_pendingDisks[op].insert(key);
    ReleaseSRWLockExclusive(&g_stateLock);
}

std::set<std::wstring> TakeOperationDisks(IFileOperation* op) {
    AcquireSRWLockExclusive(&g_stateLock);
    std::set<std::wstring> disks;
    auto it = g_pendingDisks.find(op);
    if (it != g_pendingDisks.end()) {
        disks = std::move(it->second);
        g_pendingDisks.erase(it);
    }
    ReleaseSRWLockExclusive(&g_stateLock);

    if (disks.empty()) {
        disks.insert(L"Generic");
    }
    return disks;
}

// --- Queue & Synchronization Logic ---

bool HasConflictWithPreceding(const QueueEntry* entry) {
    for (const auto* other : g_queue) {
        if (other == entry) {
            break; // Reached ourselves, no earlier entry in local queue conflicts!
        }
        for (const auto& key : entry->diskKeys) {
            if (other->diskKeys.find(key) != other->diskKeys.end()) {
                return true; // Conflict with an earlier queued entry!
            }
        }
    }
    return false;
}

void RegisterQueueEntry(QueueEntry* entry, std::set<std::wstring> disks) {
    AcquireSRWLockExclusive(&g_stateLock);

    entry->id = g_nextOperationId++;
    entry->state = TurnState::Waiting;
    entry->queued = true;
    entry->ownsCrossProcessMutexes = false;
    entry->diskKeys = std::move(disks);

    // Create named mutex handles in sorted order to guarantee no deadlocks
    for (const auto& key : entry->diskKeys) {
        std::wstring mutexName = L"Local\\Windhawk.CopyQueue." + key;
        HANDLE hMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (hMutex) {
            entry->mutexHandles.push_back(hMutex);
        }
    }

    g_queue.push_back(entry);

    std::wstring diskList;
    for (const auto& k : entry->diskKeys) {
        if (!diskList.empty()) diskList += L", ";
        diskList += k;
    }

    Wh_Log(
        L"Copy operation registered: id=%llu, disks=[%s], position=%llu",
        static_cast<unsigned long long>(entry->id),
        diskList.c_str(),
        static_cast<unsigned long long>(g_queue.size()));

    ReleaseSRWLockExclusive(&g_stateLock);
}

bool AcquireCrossProcessTurn(QueueEntry* entry) {
    if (entry->mutexHandles.empty()) {
        return true;
    }

    const DWORD count = static_cast<DWORD>(entry->mutexHandles.size());

    while (!g_unloading.load(std::memory_order_acquire)) {
        if (WaitForSingleObject(g_shutdownEvent, 0) == WAIT_OBJECT_0) {
            return false;
        }

        // Wait for ALL required disk mutexes atomically
        DWORD waitResult = WaitForMultipleObjects(
            count,
            entry->mutexHandles.data(),
            TRUE,
            500);

        if ((waitResult >= WAIT_OBJECT_0 && waitResult < WAIT_OBJECT_0 + count) ||
            (waitResult >= WAIT_ABANDONED_0 && waitResult < WAIT_ABANDONED_0 + count)) {
            entry->ownsCrossProcessMutexes = true;
            return true;
        }

        if (waitResult == WAIT_TIMEOUT) {
            continue;
        }

        break;
    }

    return false;
}

bool WaitForQueueTurn(QueueEntry* entry) {
    bool acquireCrossProcess = false;

    for (;;) {
        AcquireSRWLockExclusive(&g_stateLock);

        if (g_unloading.load(std::memory_order_acquire) || !entry->queued) {
            entry->state = TurnState::Aborted;
            ReleaseSRWLockExclusive(&g_stateLock);
            return false;
        }

        if (entry->state == TurnState::Ready) {
            ReleaseSRWLockExclusive(&g_stateLock);
            return true;
        }

        if (entry->state == TurnState::Aborted) {
            ReleaseSRWLockExclusive(&g_stateLock);
            return false;
        }

        const bool canAcquire = !HasConflictWithPreceding(entry);

        if (canAcquire && entry->state == TurnState::Waiting) {
            entry->state = TurnState::AcquiringCrossProcessMutex;
            acquireCrossProcess = true;
            ReleaseSRWLockExclusive(&g_stateLock);
            break;
        }

        SleepConditionVariableSRW(
            &g_queueChanged,
            &g_stateLock,
            INFINITE,
            0);
        ReleaseSRWLockExclusive(&g_stateLock);
    }

    const bool acquired = acquireCrossProcess &&
                          AcquireCrossProcessTurn(entry);

    AcquireSRWLockExclusive(&g_stateLock);

    if (acquired &&
        !g_unloading.load(std::memory_order_acquire) &&
        entry->queued) {
        entry->state = TurnState::Ready;
        Wh_Log(
            L"Copy operation released for execution: id=%llu",
            static_cast<unsigned long long>(entry->id));
    } else {
        entry->state = TurnState::Aborted;
    }

    WakeAllConditionVariable(&g_queueChanged);
    const bool ready = entry->state == TurnState::Ready;
    ReleaseSRWLockExclusive(&g_stateLock);

    return ready;
}

void CompleteQueueEntry(QueueEntry* entry) {
    if (entry->ownsCrossProcessMutexes) {
        for (HANDLE h : entry->mutexHandles) {
            if (h) {
                ReleaseMutex(h);
            }
        }
        entry->ownsCrossProcessMutexes = false;
    }

    for (HANDLE h : entry->mutexHandles) {
        if (h) {
            CloseHandle(h);
        }
    }
    entry->mutexHandles.clear();

    AcquireSRWLockExclusive(&g_stateLock);

    auto iterator = std::find(g_queue.begin(), g_queue.end(), entry);
    if (iterator != g_queue.end()) {
        g_queue.erase(iterator);
    }

    entry->queued = false;
    if (entry->state != TurnState::Ready) {
        entry->state = TurnState::Aborted;
    }

    WakeAllConditionVariable(&g_queueChanged);
    ReleaseSRWLockExclusive(&g_stateLock);
}

// --- Progress Sink ---

class QueueProgressSink final : public IFileOperationProgressSink {
public:
    explicit QueueProgressSink(QueueEntry* entry) : entry_(entry) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID riid,
        void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;

        if (IsEqualIID(riid, IID_IUnknown) ||
            IsEqualIID(riid, __uuidof(IFileOperationProgressSink))) {
            *object = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&referenceCount_));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG count = InterlockedDecrement(&referenceCount_);
        if (count == 0) {
            delete this;
        }
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE StartOperations() override {
        Wh_Log(
            L"Explorer started copy dialog for id=%llu",
            static_cast<unsigned long long>(entry_->id));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE FinishOperations(HRESULT result) override {
        Wh_Log(
            L"Explorer finished copy dialog for id=%llu, result=0x%08X",
            static_cast<unsigned long long>(entry_->id),
            static_cast<unsigned int>(result));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE PreCopyItem(
        DWORD,
        IShellItem*,
        IShellItem*,
        LPCWSTR) override {
        return WaitBeforeTransfer();
    }

    HRESULT STDMETHODCALLTYPE PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }

    HRESULT STDMETHODCALLTYPE PreNewItem(
        DWORD,
        IShellItem*,
        LPCWSTR) override {
        return WaitBeforeTransfer();
    }

    HRESULT STDMETHODCALLTYPE PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateProgress(UINT, UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResetTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE PauseTimer() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ResumeTimer() override { return S_OK; }

private:
    HRESULT WaitBeforeTransfer() {
        return WaitForQueueTurn(entry_)
                   ? S_OK
                   : HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    volatile LONG referenceCount_ = 1;
    QueueEntry* entry_;
};

// --- Hook Implementations ---

HRESULT STDMETHODCALLTYPE CopyItemHook(
    IFileOperation* self,
    IShellItem* source,
    IShellItem* destinationFolder,
    LPCWSTR copyName,
    IFileOperationProgressSink* progressSink) {

    RegisterOperationDisks(self, GetDiskKeyFromShellItem(source));
    RegisterOperationDisks(self, GetDiskKeyFromShellItem(destinationFolder));

    return g_copyItemOriginal(self, source, destinationFolder, copyName, progressSink);
}

HRESULT STDMETHODCALLTYPE CopyItemsHook(
    IFileOperation* self,
    IUnknown* items,
    IShellItem* destinationFolder) {

    RegisterOperationDisks(self, GetDiskKeyFromShellItem(destinationFolder));

    if (items) {
        IShellItemArray* itemArray = nullptr;
        if (SUCCEEDED(items->QueryInterface(IID_PPV_ARGS(&itemArray)))) {
            DWORD count = 0;
            if (SUCCEEDED(itemArray->GetCount(&count))) {
                DWORD checkCount = (std::min)(count, 10UL);
                for (DWORD i = 0; i < checkCount; ++i) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(itemArray->GetItemAt(i, &item))) {
                        RegisterOperationDisks(self, GetDiskKeyFromShellItem(item));
                        item->Release();
                    }
                }
            }
            itemArray->Release();
        }
    }

    return g_copyItemsOriginal(self, items, destinationFolder);
}

HRESULT STDMETHODCALLTYPE PerformOperationsHook(IFileOperation* self) {
    if (g_insidePerformHook ||
        g_unloading.load(std::memory_order_acquire)) {
        return g_performOperationsOriginal(self);
    }

    std::set<std::wstring> disks = TakeOperationDisks(self);

    QueueEntry entry;
    RegisterQueueEntry(&entry, std::move(disks));

    auto* progressSink = new QueueProgressSink(&entry);
    DWORD adviseCookie = 0;
    HRESULT adviseResult = self->Advise(progressSink, &adviseCookie);

    if (FAILED(adviseResult)) {
        Wh_Log(
            L"IFileOperation::Advise failed for id=%llu: 0x%08X",
            static_cast<unsigned long long>(entry.id),
            static_cast<unsigned int>(adviseResult));
        WaitForQueueTurn(&entry);
    }

    g_insidePerformHook = true;
    HRESULT result = g_performOperationsOriginal(self);
    g_insidePerformHook = false;

    if (SUCCEEDED(adviseResult)) {
        self->Unadvise(adviseCookie);
    }

    progressSink->Release();

    Wh_Log(
        L"PerformOperations returned: id=%llu, result=0x%08X",
        static_cast<unsigned long long>(entry.id),
        static_cast<unsigned int>(result));

    CompleteQueueEntry(&entry);
    return result;
}

DWORD WINAPI ResolveHookTargetsThread(void* parameter) {
    auto* targets = static_cast<HookTargets*>(parameter);

    HRESULT initializeResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initializeResult)) {
        targets->result = initializeResult;
        return 0;
    }

    IFileOperation* fileOperation = nullptr;
    HRESULT result = CoCreateInstance(
        CLSID_FileOperation,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IFileOperation,
        reinterpret_cast<void**>(&fileOperation));

    if (SUCCEEDED(result) && fileOperation) {
        void** vtable = *reinterpret_cast<void***>(fileOperation);
        targets->copyItem = vtable[kCopyItemIndex];
        targets->copyItems = vtable[kCopyItemsIndex];
        targets->performOperations = vtable[kPerformOperationsIndex];
        fileOperation->Release();
    }

    targets->result = result;
    CoUninitialize();
    return 0;
}

bool ResolveHookTargets(HookTargets* targets) {
    HANDLE thread = CreateThread(
        nullptr, 0, ResolveHookTargetsThread, targets, 0, nullptr);
    if (!thread) {
        targets->result = HRESULT_FROM_WIN32(GetLastError());
        return false;
    }

    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);

    return SUCCEEDED(targets->result) &&
           targets->copyItem &&
           targets->copyItems &&
           targets->performOperations;
}

}  // namespace

BOOL Wh_ModInit() {
    Wh_Log(L"Initializing Copy Queue - Per Disk 1.0.0");

    g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_shutdownEvent) {
        Wh_Log(L"Failed to create shutdown event: %u", GetLastError());
        return FALSE;
    }

    HookTargets targets;
    if (!ResolveHookTargets(&targets)) {
        Wh_Log(
            L"Failed to resolve IFileOperation methods: 0x%08X",
            static_cast<unsigned int>(targets.result));
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            targets.copyItem,
            reinterpret_cast<void*>(CopyItemHook),
            reinterpret_cast<void**>(&g_copyItemOriginal)) ||
        !Wh_SetFunctionHook(
            targets.copyItems,
            reinterpret_cast<void*>(CopyItemsHook),
            reinterpret_cast<void**>(&g_copyItemsOriginal)) ||
        !Wh_SetFunctionHook(
            targets.performOperations,
            reinterpret_cast<void*>(PerformOperationsHook),
            reinterpret_cast<void**>(&g_performOperationsOriginal))) {
        Wh_Log(L"Failed to register hooks");
        return FALSE;
    }

    Wh_Log(L"Per-disk copy queue hooks initialized successfully");
    return TRUE;
}

void Wh_ModBeforeUninit() {
    g_unloading.store(true, std::memory_order_release);

    if (g_shutdownEvent) {
        SetEvent(g_shutdownEvent);
    }

    WakeAllConditionVariable(&g_queueChanged);
}

void Wh_ModUninit() {
    Wh_Log(L"Copy Queue - Per Disk unloaded");
}
