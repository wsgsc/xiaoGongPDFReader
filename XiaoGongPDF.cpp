
// XiaoGongPDF.cpp: 定义应用程序的类行为。
//

#include "pch.h"
#include "framework.h"
#include "XiaoGongPDF.h"
#include "XiaoGongPDFDlg.h"
#include <memory>
#include <vector>
#include <shlwapi.h>
#include <shellapi.h>


#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// CXiaoGongPDFApp

BEGIN_MESSAGE_MAP(CXiaoGongPDFApp, CWinApp)
	ON_COMMAND(ID_HELP, &CWinApp::OnHelp)
END_MESSAGE_MAP()


// CXiaoGongPDFApp 构造

CXiaoGongPDFApp::CXiaoGongPDFApp()
	: m_hMutex(NULL), m_gdiplusToken(0)
{
	// 支持重新启动管理器
	m_dwRestartManagerSupportFlags = AFX_RESTART_MANAGER_SUPPORT_RESTART;

	// TODO: 在此处添加构造代码，
	// 将所有重要的初始化放置在 InitInstance 中
}

CXiaoGongPDFApp::~CXiaoGongPDFApp()
{
	// 释放互斥量
	if (m_hMutex) {
		CloseHandle(m_hMutex);
		m_hMutex = NULL;
	}
}

int CXiaoGongPDFApp::ExitInstance()
{
	// 关闭 GDI+
	Gdiplus::GdiplusShutdown(m_gdiplusToken);
	// 释放互斥量
	if (m_hMutex) {
		CloseHandle(m_hMutex);
		m_hMutex = NULL;
	}

	return CWinApp::ExitInstance();
}


// 唯一的 CXiaoGongPDFApp 对象

CXiaoGongPDFApp theApp;

// 枚举窗口回调函数，查找XiaoGongPDF主窗口
struct FindWindowData
{
	HWND hWnd;
	DWORD processId;
};

BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam)
{
	FindWindowData* pData = (FindWindowData*)lParam;

	// 主窗口即使暂时隐藏，也需要能被新进程找到并显示。
	if (GetParent(hWnd) != NULL)
		return TRUE;

	// 排除当前新启动进程自身的窗口
	DWORD wndProcessId = 0;
	GetWindowThreadProcessId(hWnd, &wndProcessId);
	if (wndProcessId == 0 || wndProcessId == pData->processId)
		return TRUE;

	// 通过主窗口专属属性精确匹配，避免误匹配标题含 XiaoGongPDF 的 IDE 或资源管理器窗口
	if (::GetProp(hWnd, XIAOGONG_MAINWND_PROP) != NULL)
	{
		pData->hWnd = hWnd;
		return FALSE;  // 停止枚举
	}

	return TRUE;  // 继续枚举
}

// 查找已运行的XiaoGongPDF主窗口
HWND FindXiaoGongPDFWindow()
{
	FindWindowData data;
	data.hWnd = NULL;
	data.processId = GetCurrentProcessId();

	EnumWindows(EnumWindowsProc, (LPARAM)&data);

	return data.hWnd;
}

void ActivateXiaoGongPDFWindow(HWND hWnd)
{
	if (IsIconic(hWnd))
		ShowWindow(hWnd, SW_RESTORE);
	else if (!IsWindowVisible(hWnd))
		ShowWindow(hWnd, SW_SHOW);

	// Windows may reject cross-process foreground activation. Raise the window
	// temporarily so it still appears in front, without leaving it always on top.
	const bool wasTopmost = (GetWindowLong(hWnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
	const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE;
	SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
	if (!wasTopmost)
		SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);

	SetForegroundWindow(hWnd);
}

// 解析命令行中的 PDF 路径（argv[0] 为程序自身；支持带引号、含空格的多个路径）
static std::vector<CString> CommandLinePdfPaths()
{
	std::vector<CString> paths;
	int argc = 0;
	LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
	if (!argv)
		return paths;

	for (int i = 1; i < argc; ++i)
	{
		CString path(argv[i]);
		path.Trim();
		if (path.IsEmpty() || !PathFileExists(path))
			continue;

		CString ext = path.Right(4);
		ext.MakeLower();
		if (ext == _T(".pdf"))
			paths.push_back(path);
	}

	::LocalFree(argv);
	return paths;
}

// CXiaoGongPDFApp 初始化

BOOL CXiaoGongPDFApp::InitInstance()
{
	// 如果一个运行在 Windows XP 上的应用程序清单指定要
	// 使用 ComCtl32.dll 版本 6 或更高版本来启用可视化方式，
	//则需要 InitCommonControlsEx()。  否则，将无法创建窗口。
	INITCOMMONCONTROLSEX InitCtrls;
	InitCtrls.dwSize = sizeof(InitCtrls);
	// 将它设置为包括所有要在应用程序中使用的
	// 公共控件类。
	InitCtrls.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&InitCtrls);

	CWinApp::InitInstance();
	// 初始化 GDI+ (用于绘制圆角按钮)
	Gdiplus::GdiplusStartupInput gdiplusStartupInput;
	Gdiplus::GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, NULL);

	// 单实例检测：创建命名互斥量
	m_hMutex = CreateMutex(NULL, FALSE, _T("XiaoGongPDF_SingleInstance_Mutex_2024"));
	DWORD dwError = GetLastError();

	if (dwError == ERROR_ALREADY_EXISTS)
	{
		// 已有实例在运行：把命令行中的全部 PDF 转发给它
		const std::vector<CString> paths = CommandLinePdfPaths();
		HWND hWnd = FindXiaoGongPDFWindow();

		if (hWnd)
		{
			ActivateXiaoGongPDFWindow(hWnd);

			if (!paths.empty())
			{
				// 多字符串格式：每个路径以 '\0' 结尾，整体再补一个 '\0'
				CString payload;
				for (const CString& path : paths)
				{
					payload += path;
					payload.AppendChar(_T('\0'));
				}
				payload.AppendChar(_T('\0'));

				COPYDATASTRUCT cds = {};
				cds.dwData = paths.size() > 1 ? 2 : 1;  // 1=单路径（兼容旧格式），2=多字符串
				cds.cbData = static_cast<DWORD>((payload.GetLength() + 1) * sizeof(TCHAR));
				cds.lpData = const_cast<LPTSTR>(static_cast<LPCTSTR>(payload));

				SendMessage(hWnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds));
			}

			// 加载窗口可能在处理消息时获得焦点，再激活主窗口。
			ActivateXiaoGongPDFWindow(hWnd);
		}

		// 退出当前实例
		return FALSE;
	}


	AfxEnableControlContainer();

	// 创建 shell 管理器，以防对话框包含
	// 任何 shell 树视图控件或 shell 列表视图控件。
	std::unique_ptr<CShellManager> pShellManager(new CShellManager);

	// 激活"Windows Native"视觉管理器，以便在 MFC 控件中启用主题
	CMFCVisualManager::SetDefaultManager(RUNTIME_CLASS(CMFCVisualManagerWindows));

	// 统一应用注册表根路径到 HKCU\Software\XiaoGongPDF，并自动迁移旧版向导默认路径下的配置
	{
		constexpr LPCTSTR kLegacyAppKey = _T("Software\\应用程序向导生成的本地应用程序\\XiaoGongPDF");
		constexpr LPCTSTR kLegacyParentKey = _T("Software\\应用程序向导生成的本地应用程序");
		constexpr LPCTSTR kUnifiedAppKey = _T("Software\\XiaoGongPDF\\XiaoGongPDF");
		HKEY hExistingNewKey = NULL;
		if (::RegOpenKeyEx(HKEY_CURRENT_USER, kUnifiedAppKey, 0, KEY_READ, &hExistingNewKey) == ERROR_SUCCESS)
		{
			::RegCloseKey(hExistingNewKey);
		}
		else
		{
			HKEY hLegacyKey = NULL;
			if (::RegOpenKeyEx(HKEY_CURRENT_USER, kLegacyAppKey, 0, KEY_READ, &hLegacyKey) == ERROR_SUCCESS)
			{
				::RegCloseKey(hLegacyKey);
				HKEY hDestKey = NULL;
				if (::RegCreateKeyEx(HKEY_CURRENT_USER, kUnifiedAppKey, 0, NULL,
					REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hDestKey, NULL) == ERROR_SUCCESS)
				{
					if (::SHCopyKey(HKEY_CURRENT_USER, kLegacyAppKey, hDestKey, 0) == ERROR_SUCCESS)
					{
						::SHDeleteKey(HKEY_CURRENT_USER, kLegacyAppKey);
						// 若旧父键已无其他子项则一并清理（非空时 RegDeleteKey 安全失败）
						::RegDeleteKey(HKEY_CURRENT_USER, kLegacyParentKey);
					}
					::RegCloseKey(hDestKey);
				}
			}
		}
	}
	SetRegistryKey(_T("XiaoGongPDF"));

	CXiaoGongPDFDlg dlg;
	m_pMainWnd = &dlg;

	// 命令行中的全部 PDF（支持一次传入多个文件；首个作为活动标签页）
	const std::vector<CString> startupPaths = CommandLinePdfPaths();
	if (!startupPaths.empty())
	{
		dlg.m_initialFilePaths = startupPaths;
		dlg.m_initialFilePath = startupPaths.front();
	}

	INT_PTR nResponse = dlg.DoModal();
	if (nResponse == IDOK)
	{
		// TODO: 在此放置处理何时用
		//  "确定"来关闭对话框的代码
	}
	else if (nResponse == IDCANCEL)
	{
		// TODO: 在此放置处理何时用
		//  "取消"来关闭对话框的代码
	}
	else if (nResponse == -1)
	{
		TRACE(traceAppMsg, 0, "警告: 对话框创建失败，应用程序将意外终止。\n");
		TRACE(traceAppMsg, 0, "警告: 如果您在对话框上使用 MFC 控件，则无法 #define _AFX_NO_MFC_CONTROLS_IN_DIALOGS。\n");
	}

	// 智能指针自动释放，不需要手动 delete

#if !defined(_AFXDLL) && !defined(_AFX_NO_MFC_CONTROLS_IN_DIALOGS)
	ControlBarCleanUp();
#endif

	// 由于对话框已关闭，所以将返回 FALSE 以便退出应用程序，
	//  而不是启动应用程序的消息泵。
	return FALSE;
}

