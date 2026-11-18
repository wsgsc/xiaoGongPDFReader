#include "pch.h"
#include "FileWatcher.h"
#include <vector>

CFileWatcher::CFileWatcher()
    : m_running(false)
{
    InitializeCriticalSection(&m_cs);
}

CFileWatcher::~CFileWatcher()
{
    Stop();
    for (auto& pair : m_dirWatchers)
        delete pair.second;
    m_dirWatchers.clear();
    DeleteCriticalSection(&m_cs);
}

void CFileWatcher::AddWatch(const CString& filePath, HWND hNotifyWnd)
{
    CString fullPath = filePath;
    int lastSlash = max(fullPath.ReverseFind(_T('\\')), fullPath.ReverseFind(_T('/')));
    if (lastSlash < 0)
        return;

    CString dirPath = fullPath.Left(lastSlash);
    CString fileName = fullPath.Mid(lastSlash + 1);
    dirPath.MakeLower();
    fileName.MakeLower();

    EnterCriticalSection(&m_cs);

    auto it = m_dirWatchers.find(dirPath);
    if (it != m_dirWatchers.end())
    {
        it->second->watchedFiles.insert(fileName);
        it->second->hNotifyWnd = hNotifyWnd;
    }
    else
    {
        DirWatchInfo* pInfo = new DirWatchInfo();
        pInfo->dirPath = dirPath;
        pInfo->hDir = INVALID_HANDLE_VALUE;
        pInfo->hStopEvent = NULL;
        pInfo->hThread = NULL;
        pInfo->hNotifyWnd = hNotifyWnd;
        pInfo->pOwner = this;
        pInfo->watchedFiles.insert(fileName);
        m_dirWatchers[dirPath] = pInfo;

        if (m_running)
            StartDirWatch(pInfo);
    }

    LeaveCriticalSection(&m_cs);
}

void CFileWatcher::RemoveWatch(const CString& filePath)
{
    CString fullPath = filePath;
    int lastSlash = max(fullPath.ReverseFind(_T('\\')), fullPath.ReverseFind(_T('/')));
    if (lastSlash < 0)
        return;

    CString dirPath = fullPath.Left(lastSlash);
    CString fileName = fullPath.Mid(lastSlash + 1);
    dirPath.MakeLower();
    fileName.MakeLower();

    DirWatchInfo* pToCleanup = nullptr;

    EnterCriticalSection(&m_cs);

    auto it = m_dirWatchers.find(dirPath);
    if (it != m_dirWatchers.end())
    {
        it->second->watchedFiles.erase(fileName);
        if (it->second->watchedFiles.empty())
        {
            pToCleanup = it->second;
            if (pToCleanup->hStopEvent)
                SetEvent(pToCleanup->hStopEvent);
            m_dirWatchers.erase(it);
        }
    }

    LeaveCriticalSection(&m_cs);

    if (pToCleanup)
    {
        CleanupDirWatch(pToCleanup);
    }
}

void CFileWatcher::Start()
{
    EnterCriticalSection(&m_cs);
    if (m_running)
    {
        LeaveCriticalSection(&m_cs);
        return;
    }
    m_running = true;
    for (auto& pair : m_dirWatchers)
    {
        if (!pair.second->watchedFiles.empty() && pair.second->hThread == NULL)
            StartDirWatch(pair.second);
    }
    LeaveCriticalSection(&m_cs);
}

void CFileWatcher::Stop()
{
    std::vector<DirWatchInfo*> watchers;

    EnterCriticalSection(&m_cs);
    if (!m_running)
    {
        LeaveCriticalSection(&m_cs);
        return;
    }
    m_running = false;

    watchers.reserve(m_dirWatchers.size());
    for (auto& pair : m_dirWatchers)
    {
        if (pair.second->hStopEvent)
            SetEvent(pair.second->hStopEvent);
        watchers.push_back(pair.second);
    }
    LeaveCriticalSection(&m_cs);

    for (DirWatchInfo* pInfo : watchers)
    {
        StopDirWatch(pInfo);
    }
}

void CFileWatcher::StartDirWatch(DirWatchInfo* pInfo)
{
    if (pInfo->hThread)
        return;

    CString actualDir = pInfo->dirPath;
    HANDLE hDir = CreateFile(
        actualDir,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL);

    if (hDir == INVALID_HANDLE_VALUE)
    {
        TRACE(_T("FileWatcher: 无法打开目录 %s, 错误=%d\n"), (LPCTSTR)actualDir, GetLastError());
        return;
    }
    pInfo->hDir = hDir;

    pInfo->hStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!pInfo->hStopEvent)
    {
        CloseHandle(pInfo->hDir);
        pInfo->hDir = INVALID_HANDLE_VALUE;
        return;
    }

    pInfo->hThread = CreateThread(NULL, 0, WatchThreadProc, pInfo, 0, NULL);
    if (!pInfo->hThread)
    {
        CloseHandle(pInfo->hStopEvent);
        pInfo->hStopEvent = NULL;
        CloseHandle(pInfo->hDir);
        pInfo->hDir = INVALID_HANDLE_VALUE;
    }
}

void CFileWatcher::StopDirWatch(DirWatchInfo* pInfo)
{
    if (pInfo->hStopEvent)
        SetEvent(pInfo->hStopEvent);

    if (pInfo->hThread)
    {
        WaitForSingleObject(pInfo->hThread, INFINITE);
        CloseHandle(pInfo->hThread);
        pInfo->hThread = NULL;
    }
    if (pInfo->hStopEvent)
    {
        CloseHandle(pInfo->hStopEvent);
        pInfo->hStopEvent = NULL;
    }
    if (pInfo->hDir != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pInfo->hDir);
        pInfo->hDir = INVALID_HANDLE_VALUE;
    }
}

void CFileWatcher::CleanupDirWatch(DirWatchInfo* pInfo)
{
    StopDirWatch(pInfo);
    delete pInfo;
}

DWORD WINAPI CFileWatcher::WatchThreadProc(LPVOID pParam)
{
    DirWatchInfo* pInfo = static_cast<DirWatchInfo*>(pParam);
    if (!pInfo || pInfo->hDir == INVALID_HANDLE_VALUE)
        return 1;

    BYTE buffer[4096];
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent)
        return 1;

    HANDLE waitHandles[2] = { overlapped.hEvent, pInfo->hStopEvent };

    while (true)
    {
        ResetEvent(overlapped.hEvent);
        DWORD bytesReturned = 0;

        BOOL ok = ReadDirectoryChangesW(
            pInfo->hDir,
            buffer,
            sizeof(buffer),
            FALSE,
            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
            &bytesReturned,
            &overlapped,
            NULL);

        if (!ok)
        {
            DWORD err = GetLastError();
            if (err == ERROR_OPERATION_ABORTED)
                break;
            TRACE(_T("FileWatcher: ReadDirectoryChangesW 失败, 错误=%d\n"), err);
            break;
        }

        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0 + 1)
        {
            CancelIo(pInfo->hDir);
            break;
        }

        if (waitResult != WAIT_OBJECT_0)
            break;

        if (!GetOverlappedResult(pInfo->hDir, &overlapped, &bytesReturned, FALSE))
            break;

        if (bytesReturned == 0)
            continue;

        FILE_NOTIFY_INFORMATION* pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
        while (true)
        {
            CString changedName(pNotify->FileName, pNotify->FileNameLength / sizeof(WCHAR));
            int slash = max(changedName.ReverseFind(_T('\\')), changedName.ReverseFind(_T('/')));
            if (slash >= 0)
                changedName = changedName.Mid(slash + 1);
            changedName.MakeLower();

            bool matched = false;
            CString fullPath;

            EnterCriticalSection(&pInfo->pOwner->m_cs);
            if (pInfo->watchedFiles.find(changedName) != pInfo->watchedFiles.end())
            {
                matched = true;
                fullPath = pInfo->dirPath + _T("\\") + changedName;
            }
            LeaveCriticalSection(&pInfo->pOwner->m_cs);

            if (matched)
            {
                if (WaitForSingleObject(pInfo->hStopEvent, 500) == WAIT_OBJECT_0)
                    break;
                CString* pPath = new CString(fullPath);
                if (!PostMessage(pInfo->hNotifyWnd, WM_FILE_CHANGED, 0, reinterpret_cast<LPARAM>(pPath)))
                    delete pPath;
            }

            if (pNotify->NextEntryOffset == 0)
                break;
            pNotify = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<BYTE*>(pNotify) + pNotify->NextEntryOffset);
        }
        if (WaitForSingleObject(pInfo->hStopEvent, 0) == WAIT_OBJECT_0)
            break;
    }

    CloseHandle(overlapped.hEvent);
    return 0;
}
