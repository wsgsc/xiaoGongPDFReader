#pragma once

#include <afxwin.h>
#include <map>
#include <set>

#define WM_FILE_CHANGED (WM_USER + 109)

class CFileWatcher
{
public:
    CFileWatcher();
    ~CFileWatcher();

    void AddWatch(const CString& filePath, HWND hNotifyWnd);
    void RemoveWatch(const CString& filePath);
    void Start();
    void Stop();
    bool IsRunning() const { return m_running; }

private:
    struct DirWatchInfo {
        CString dirPath;
        HANDLE hDir;
        HANDLE hStopEvent;
        HANDLE hThread;
        std::set<CString> watchedFiles;
        HWND hNotifyWnd;
        CFileWatcher* pOwner;
    };

    static DWORD WINAPI WatchThreadProc(LPVOID pParam);
    void StartDirWatch(DirWatchInfo* pInfo);
    void StopDirWatch(DirWatchInfo* pInfo);
    void CleanupDirWatch(DirWatchInfo* pInfo);

    std::map<CString, DirWatchInfo*> m_dirWatchers;
    CRITICAL_SECTION m_cs;
    bool m_running;
};
