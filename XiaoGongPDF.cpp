
// XiaoGongPDF.cpp: 定义应用程序的类行为。
//

#include "pch.h"
#include "framework.h"
#include "XiaoGongPDF.h"
#include "XiaoGongPDFDlg.h"
#include <memory>


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

	// 检查是否为顶层可见窗口
	if (!IsWindowVisible(hWnd) || GetParent(hWnd) != NULL)
		return TRUE;

	// 获取窗口标题
	TCHAR szTitle[256] = { 0 };
	int titleLen = GetWindowText(hWnd, szTitle, 256);

	// 检查窗口标题是否包含 "XiaoGongPDF" 或 "小龚PDF阅读器"
	CString title(szTitle);
	if (title.Find(_T("XiaoGongPDF")) >= 0 || title.Find(_T("小龚PDF阅读器")) >= 0)
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
		// 已有实例在运行
		// 获取命令行参数
		CString cmdLine = m_lpCmdLine;
		cmdLine.Trim();

		// 去除引号
		if (!cmdLine.IsEmpty())
		{
			if (cmdLine[0] == _T('"'))
			{
				cmdLine = cmdLine.Mid(1);
			}
			if (!cmdLine.IsEmpty() && cmdLine[cmdLine.GetLength() - 1] == _T('"'))
			{
				cmdLine = cmdLine.Left(cmdLine.GetLength() - 1);
			}
		}

		// 如果有要打开的文件，发送给已存在的实例
		if (!cmdLine.IsEmpty() && PathFileExists(cmdLine))
		{
			// 查找已运行的 XiaoGongPDF 窗口
			HWND hWnd = FindXiaoGongPDFWindow();

			if (hWnd)
			{
				// 使用 WM_COPYDATA 消息发送文件路径
				COPYDATASTRUCT cds;
				cds.dwData = 1;  // 自定义数据标识，1 表示打开PDF文件
				cds.cbData = (cmdLine.GetLength() + 1) * sizeof(TCHAR);
				cds.lpData = (PVOID)(LPCTSTR)cmdLine;

				SendMessage(hWnd, WM_COPYDATA, 0, (LPARAM)&cds);

				// 激活已存在的窗口
				if (IsIconic(hWnd)) {
					ShowWindow(hWnd, SW_RESTORE);
				}
				SetForegroundWindow(hWnd);
			}
		}
		else
		{
			// 没有文件参数，只是激活已存在的窗口
			HWND hWnd = FindXiaoGongPDFWindow();
			if (hWnd)
			{
				if (IsIconic(hWnd)) {
					ShowWindow(hWnd, SW_RESTORE);
				}
				SetForegroundWindow(hWnd);
			}
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

	// 标准初始化
	// 如果未使用这些功能并希望减小
	// 最终可执行文件的大小，则应移除下列
	// 不需要的特定初始化例程
	// 更改用于存储设置的注册表项
	// TODO: 应适当修改该字符串，
	// 例如修改为公司或组织名
	SetRegistryKey(_T("应用程序向导生成的本地应用程序"));

	// 获取命令行参数
	CString cmdLine = m_lpCmdLine;
	cmdLine.Trim();

	// 去除命令行参数中的引号
	if (!cmdLine.IsEmpty())
	{
		if (cmdLine[0] == _T('"'))
		{
			cmdLine = cmdLine.Mid(1);
		}
		if (!cmdLine.IsEmpty() && cmdLine[cmdLine.GetLength() - 1] == _T('"'))
		{
			cmdLine = cmdLine.Left(cmdLine.GetLength() - 1);
		}
	}

	CXiaoGongPDFDlg dlg;
	m_pMainWnd = &dlg;

	// 如果有命令行参数（PDF文件路径），在对话框显示前先打开文件
	if (!cmdLine.IsEmpty() && PathFileExists(cmdLine))
	{
		// 将文件路径传递给对话框
		dlg.m_initialFilePath = cmdLine;
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

