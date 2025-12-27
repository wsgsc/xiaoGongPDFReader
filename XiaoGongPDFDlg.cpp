#include "pch.h"
#include "framework.h"
#include <commctrl.h>
#include "XiaoGongPDF.h"
#include "XiaoGongPDFDlg.h"
#include "PDFEditDialog.h"
#include "afxdialogex.h"
#include <mupdf/fitz.h>
#include <vector>
#include <WinUser.h>  // 添加这个头文件来获取 HTSCROLLBAR 定义

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// ============================================================================
// CPDFViewCtrl 实现（自定义PDF预览控件，支持滚动条）
// ============================================================================

BEGIN_MESSAGE_MAP(CPDFViewCtrl, CStatic)
	ON_WM_VSCROLL()
END_MESSAGE_MAP()

void CPDFViewCtrl::SubclassWindow()
{
	// 使用SetWindowLongPtr替换窗口过程，拦截WM_VSCROLL消息
	m_oldWndProc = (WNDPROC)::SetWindowLongPtr(GetSafeHwnd(), GWLP_WNDPROC, (LONG_PTR)StaticWndProc);

	// 将this指针保存到窗口的用户数据中，以便在静态函数中访问
	::SetWindowLongPtr(GetSafeHwnd(), GWLP_USERDATA, (LONG_PTR)this);
}

LRESULT CALLBACK CPDFViewCtrl::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// 从窗口的用户数据中获取this指针
	CPDFViewCtrl* pThis = (CPDFViewCtrl*)::GetWindowLongPtr(hwnd, GWLP_USERDATA);

	// 处理 WM_VSCROLL：转发滚动消息给父对话框（用于鼠标滚轮等操作）
	if (msg == WM_VSCROLL)
	{
		if (pThis && pThis->m_pParentDlg)
		{
			pThis->m_pParentDlg->SendMessage(WM_VSCROLL, wParam, (LPARAM)hwnd);
			return 0;
		}
	}

	// 调用原始窗口过程
	if (pThis && pThis->m_oldWndProc)
	{
		return ::CallWindowProc(pThis->m_oldWndProc, hwnd, msg, wParam, lParam);
	}

	return ::DefWindowProc(hwnd, msg, wParam, lParam);
}

void CPDFViewCtrl::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	// 将滚动条消息转发给父对话框处理
	if (m_pParentDlg)
	{
		m_pParentDlg->SendMessage(WM_VSCROLL, MAKEWPARAM(nSBCode, nPos), (LPARAM)GetSafeHwnd());
	}
}

// ============================================================================
// CXiaoGongPDFDlg 实现
// ============================================================================

CXiaoGongPDFDlg::CXiaoGongPDFDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_MOUTAIPDF_DIALOG, pParent)
	, m_ctx(nullptr)
	, m_doc(nullptr)
	, m_currentPage(0)
	, m_totalPages(0)
	, m_currentPageObj(nullptr)
	, m_hCurrentBitmap(nullptr)
	, m_thumbnailPicHeight(0)
	, m_renderLock()
	, m_isFullscreen(false)
	, m_zoomMode(ZOOM_FIT_PAGE)
	, m_customZoom(1.0f)
	, m_documentMaxPageWidth(0.0f)  // ★★★ 初始化文档最大页面宽度
	, m_documentUniformScale(1.0f)  // ★★★ 初始化文档统一缩放比例
	, m_thumbnailVisible(false)  // 默认隐藏缩略图面板
	, m_isDragging(false)       // 初始化拖拽状态
	, m_panOffset(0, 0)         // 初始化平移偏移量
	, m_hHandCursor(nullptr)    // 初始化手形光标
	, m_hHandCursorGrab(nullptr) // 初始化抓取光标
	, m_hPanPageBitmap(nullptr) // 初始化拖拽缓存位图
	, m_canDrag(false)          // ★★★ 初始化拖拽判断缓存
	, m_activeDocIndex(-1)      // 初始化活动文档索引（-1表示无文档）
	, m_continuousScrollMode(true)  // ★★★ 默认启用连续滚动模式
	, m_scrollPosition(0)        // 初始化滚动位置
	, m_totalScrollHeight(0)     // 初始化总滚动高度
	, m_uniformScale(1.0f)       // ★★★ 初始化统一缩放比例（连续滚动模式用）
	, m_hContinuousViewBitmap(nullptr)  // ★★★ 初始化连续视图位图
	, m_isDraggingScrollbar(false)  // 初始化滚动条拖拽状态
	, m_scrollbarDragStartY(0)      // 初始化滚动条拖拽起始Y
	, m_scrollbarDragStartPos(0)    // 初始化滚动条拖拽起始位置
	, m_currentMatchIndex(-1)       // 初始化当前匹配项索引
	, m_searchCaseSensitive(false)  // 默认不区分大小写
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	m_highlightBrush.CreateSolidBrush(RGB(173, 216, 230));  // 浅蓝色
	m_whiteBrush.CreateSolidBrush(RGB(255, 255, 255));  // 白色（用于标签页背景）
	LoadRecentFiles();

	// 加载手形光标
	m_hHandCursor = LoadCursor(NULL, IDC_HAND);
	// 注意：Windows没有内置的"抓取"光标，我们暂时使用手形光标
	m_hHandCursorGrab = LoadCursor(NULL, IDC_HAND);
}

CXiaoGongPDFDlg::~CXiaoGongPDFDlg()
{
#ifdef _DEBUG
	TRACE(_T("~CXiaoGongPDFDlg() 析构函数开始\n"));
#endif

	// 析构仅做"纯资源"清理，不碰 UI

	// ★★★ 重要：在删除文档对象之前，先释放主对话框持有的页面对象
	// 页面对象持有对文档的引用，必须先释放才能让文档的引用计数归零
	if (m_currentPageObj && m_ctx)
	{
#ifdef _DEBUG
		TRACE(_T("释放主对话框的页面对象: %p\n"), m_currentPageObj);
#endif
		fz_drop_page(m_ctx, m_currentPageObj);
		m_currentPageObj = nullptr;
#ifdef _DEBUG
		TRACE(_T("页面对象已释放\n"));
#endif
	}

	// ★★★ 重要：在删除文档对象之前，先将当前活动文档的资源保存回文档对象
	// 这样所有资源都由文档对象管理，避免重复释放
	if (m_activeDocIndex >= 0 && m_activeDocIndex < (int)m_documents.size())
	{
#ifdef _DEBUG
		TRACE(_T("保存当前文档状态，索引: %d\n"), m_activeDocIndex);
#endif
		CPDFDocument* activeDoc = m_documents[m_activeDocIndex];
		if (activeDoc)
		{
			// 保存当前页面状态
			activeDoc->SaveCurrentPageZoomState();
			activeDoc->SetCurrentPage(m_currentPage);

			// 转移位图所有权（只转移有效的位图）
			if (m_hCurrentBitmap)
			{
				// 验证位图是否有效
				BITMAP bm;
				if (::GetObject(m_hCurrentBitmap, sizeof(BITMAP), &bm) != 0)
				{
					activeDoc->SetCurrentBitmap(m_hCurrentBitmap);
					m_hCurrentBitmap = NULL;
				}
				else
				{
					m_hCurrentBitmap = NULL;  // 清空无效句柄
				}
			}

			if (m_hPanPageBitmap)
			{
				// 验证位图是否有效
				BITMAP bm;
				if (::GetObject(m_hPanPageBitmap, sizeof(BITMAP), &bm) != 0)
				{
					activeDoc->SetPanPageBitmap(m_hPanPageBitmap);
					m_hPanPageBitmap = NULL;
				}
				else
				{
					m_hPanPageBitmap = NULL;  // 清空无效句柄
				}
			}

			// 转移缩略图缓存（使用swap避免浅拷贝）
			activeDoc->GetThumbnailCache().swap(m_thumbnailCache);

			// 保存缩略图尺寸
			activeDoc->SetThumbnailPicWidth(m_thumbnailPicWidth);
			activeDoc->SetThumbnailPicHeight(m_thumbnailPicHeight);
		}
	}

	// 清理所有打开的文档
#ifdef _DEBUG
	TRACE(_T("开始清理 %d 个文档对象\n"), (int)m_documents.size());
#endif
	for (auto doc : m_documents)
	{
		if (doc)
		{
#ifdef _DEBUG
			TRACE(_T("删除文档对象: %p\n"), doc);
#endif
			delete doc;
		}
	}
	m_documents.clear();
	m_activeDocIndex = -1;
#ifdef _DEBUG
	TRACE(_T("文档对象清理完成\n"));
#endif

	// ★★★ 重要：清空对话框中的所有资源指针，防止 ResourceRelease() 重复释放
	// 因为 CPDFDocument 的析构函数已经释放了这些资源
	m_doc = nullptr;
	m_currentPageObj = nullptr;
	m_hCurrentBitmap = NULL;
	m_hPanPageBitmap = NULL;
	m_thumbnailCache.clear();  // 已经转移所有权，这里只是清空map
	// 注意：m_pageCache 不在这里清空，让 ResourceRelease() 来清理
	// 否则位图不会被释放，导致内存泄漏

	// ★★★ 清理连续滚动视图位图
	if (m_hContinuousViewBitmap)
	{
		DeleteObject(m_hContinuousViewBitmap);
		m_hContinuousViewBitmap = NULL;
	}

	ResourceRelease();

	// 在清理其他资源前先释放字体和画刷对象
	if (m_buttonFont.GetSafeHandle())
		m_buttonFont.DeleteObject();
	if (m_labelFont.GetSafeHandle())
		m_labelFont.DeleteObject();
	if (m_highlightBrush.GetSafeHandle())
		m_highlightBrush.DeleteObject();
	if (m_whiteBrush.GetSafeHandle())
		m_whiteBrush.DeleteObject();
}

void CXiaoGongPDFDlg::CleanupBitmap()
{
	if (m_hCurrentBitmap)
	{
		// 验证是否是有效的 GDI 对象
		BITMAP bm;
		if (::GetObject(m_hCurrentBitmap, sizeof(BITMAP), &bm) != 0)
		{
			// 是有效的位图，可以安全删除
			DeleteObject(m_hCurrentBitmap);
		}
		m_hCurrentBitmap = NULL;
	}

	// ★★★ 清理连续滚动视图位图
	if (m_hContinuousViewBitmap)
	{
		DeleteObject(m_hContinuousViewBitmap);
		m_hContinuousViewBitmap = NULL;
	}

	// 清理 CStatic 控件中的位图
	m_pdfView.SetBitmap(NULL);
}

void CXiaoGongPDFDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_TAB_CONTROL, m_tabCtrl);
	DDX_Control(pDX, IDC_PDF_VIEW, m_pdfView);
	DDX_Control(pDX, IDC_THUMBNAIL_LIST, m_thumbnailList);
	DDX_Control(pDX, IDC_TOOLBAR, m_toolbar);
	DDX_Control(pDX, IDC_BTN_FIRST, m_btnFirst);
	DDX_Control(pDX, IDC_BTN_LAST, m_btnLast);
	DDX_Control(pDX, IDC_BTN_FULLSCREEN, m_btnFullscreen);
	DDX_Control(pDX, IDC_BTN_ROTATE_LEFT, m_btnRotateLeft);
	DDX_Control(pDX, IDC_BTN_ROTATE_RIGHT, m_btnRotateRight);
	DDX_Control(pDX, IDC_BTN_EDIT, m_btnEdit);
	DDX_Control(pDX, IDC_CHECK_THUMBNAIL, m_checkThumbnail);
	DDX_Control(pDX, IDC_EDIT_CURRENT, m_editCurrent);
	DDX_Control(pDX, IDC_STATUS_BAR, m_statusBar);

	// 搜索控件绑定
	DDX_Control(pDX, IDC_EDIT_SEARCH, m_editSearch);
	DDX_Control(pDX, IDC_BTN_FIND, m_btnFind);
	DDX_Control(pDX, IDC_BTN_PREV_MATCH, m_btnPrevMatch);
	DDX_Control(pDX, IDC_BTN_NEXT_MATCH, m_btnNextMatch);
}

BEGIN_MESSAGE_MAP(CXiaoGongPDFDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_WM_CTLCOLOR()
	ON_COMMAND(ID_MENU_OPEN, &CXiaoGongPDFDlg::onMenuOpen)
	ON_COMMAND(ID_MENU_EXIT,&CXiaoGongPDFDlg::onMenuExit)
	ON_COMMAND(ID_MENU_ABOUT,&CXiaoGongPDFDlg::onMenuAbout)
	ON_COMMAND(ID_MENU_PRINT, &CXiaoGongPDFDlg::onMenuPrint)
	ON_COMMAND(ID_MENU_SET_DEFAULT, &CXiaoGongPDFDlg::onMenuSetDefault)
	ON_COMMAND(ID_MENU_SHORTCUTS, &CXiaoGongPDFDlg::onMenuShortcuts)
	ON_COMMAND_RANGE(ID_MENU_RECENT_FILE_1, ID_MENU_RECENT_FILE_10, &CXiaoGongPDFDlg::onMenuRecentFile)
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_THUMBNAIL_LIST, &CXiaoGongPDFDlg::OnThumbnailItemChanged)
	ON_NOTIFY(TCN_SELCHANGE, IDC_TAB_CONTROL, &CXiaoGongPDFDlg::OnTabSelChange)
	ON_MESSAGE(WM_APP + 100, &CXiaoGongPDFDlg::OnTabCloseButton)  // 标签页关闭按钮消息
	ON_WM_SIZE()
	ON_MESSAGE(WM_USER + 100, &CXiaoGongPDFDlg::OnRenderThumbnailsAsync)
	ON_MESSAGE(WM_USER + 101, &CXiaoGongPDFDlg::OnOpenInitialFile)  // 延迟打开初始文件
	ON_WM_MOUSEWHEEL()
	ON_BN_CLICKED(IDC_BTN_FIRST, &CXiaoGongPDFDlg::OnBtnFirst)
	ON_BN_CLICKED(IDC_BTN_LAST, &CXiaoGongPDFDlg::OnBtnLast)
	ON_BN_CLICKED(IDC_BTN_FULLSCREEN, &CXiaoGongPDFDlg::OnBtnFullscreen)
	ON_BN_CLICKED(IDC_BTN_ROTATE_LEFT, &CXiaoGongPDFDlg::OnBtnRotateLeft)
	ON_BN_CLICKED(IDC_BTN_ROTATE_RIGHT, &CXiaoGongPDFDlg::OnBtnRotateRight)
	ON_BN_CLICKED(IDC_BTN_EDIT, &CXiaoGongPDFDlg::OnBtnEdit)
	ON_BN_CLICKED(IDC_CHECK_THUMBNAIL, &CXiaoGongPDFDlg::OnCheckThumbnail)
	ON_EN_CHANGE(IDC_EDIT_CURRENT, &CXiaoGongPDFDlg::OnEnChangeEditCurrent)
	ON_EN_KILLFOCUS(IDC_EDIT_CURRENT, &CXiaoGongPDFDlg::OnEditCurrentKillFocus)
	ON_EN_CHANGE(IDC_EDIT_SEARCH, &CXiaoGongPDFDlg::OnEnChangeEditSearch)
	ON_BN_CLICKED(IDC_BTN_FIND, &CXiaoGongPDFDlg::OnBtnFind)
	ON_BN_CLICKED(IDC_BTN_PREV_MATCH, &CXiaoGongPDFDlg::OnBtnPrevMatch)
	ON_BN_CLICKED(IDC_BTN_NEXT_MATCH, &CXiaoGongPDFDlg::OnBtnNextMatch)
	ON_WM_KEYDOWN()
	ON_WM_GETMINMAXINFO()
	ON_WM_DROPFILES()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_MOUSEMOVE()
	ON_WM_SETCURSOR()
	ON_WM_COPYDATA()
	ON_WM_DESTROY()
	ON_WM_VSCROLL()  // ★★★ 添加垂直滚动条消息处理（连续滚动模式）
END_MESSAGE_MAP()

void CXiaoGongPDFDlg::onMenuOpen() {
#ifdef _DEBUG
	TRACE(_T("onMenuOpen() \n"));
#endif

	CFileDialog fileDlg(TRUE, _T("pdf"), NULL,
		OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
		_T("PDF Files (*.pdf)|*.pdf|All Files (*.*)|*.*||"));

	if (fileDlg.DoModal() == IDOK)
	{
		CString fullPath = fileDlg.GetPathName();

		// 在新标签页中打开PDF
		OpenPDFInNewTab(fullPath);
	}
}

void CXiaoGongPDFDlg::onMenuExit() {
#ifdef _DEBUG
	TRACE(_T("onMenuExit() \n"));
#endif

	PostMessage(WM_CLOSE);
}

void CXiaoGongPDFDlg::onMenuAbout() {
#ifdef _DEBUG
	TRACE(_T("onMenuAbout() \n"));
#endif
	CDialogEx aboutDlg(IDD_ABOUTBOX);
	aboutDlg.DoModal();

}

void CXiaoGongPDFDlg::onMenuSetDefault()
{
#ifdef _DEBUG
	TRACE(_T("onMenuSetDefault() \n"));
#endif

	// 获取当前程序路径
	TCHAR szPath[MAX_PATH];
	GetModuleFileName(NULL, szPath, MAX_PATH);

	CString exePath = szPath;
	CString commandLine;
	// 设置命令行格式，%1 会被 Windows 替换为文件路径
	commandLine.Format(_T("\"%s\" \"%%1\""), exePath);

	// 设置注册表项，将此程序设为PDF默认阅读器
	HKEY hKey;
	LONG result;
	DWORD dwDisposition;

	// 1. 创建 .pdf 文件关联
	result = RegCreateKeyEx(HKEY_CURRENT_USER, _T("Software\\Classes\\.pdf"), 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		RegSetValueEx(hKey, NULL, 0, REG_SZ, (BYTE*)_T("XiaoGongPDF.Document"),
			(_tcslen(_T("XiaoGongPDF.Document")) + 1) * sizeof(TCHAR));
		RegCloseKey(hKey);
	}

	// 2. 创建程序标识符
	result = RegCreateKeyEx(HKEY_CURRENT_USER, _T("Software\\Classes\\XiaoGongPDF.Document"), 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		RegSetValueEx(hKey, NULL, 0, REG_SZ, (BYTE*)_T("PDF Document"),
			(_tcslen(_T("PDF Document")) + 1) * sizeof(TCHAR));
		RegCloseKey(hKey);
	}

	// 2.5. 设置默认图标
	CString iconPath;
	iconPath.Format(_T("\"%s\",0"), exePath);  // 使用程序的第一个图标资源
	result = RegCreateKeyEx(HKEY_CURRENT_USER, _T("Software\\Classes\\XiaoGongPDF.Document\\DefaultIcon"),
		0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		RegSetValueEx(hKey, NULL, 0, REG_SZ, (BYTE*)(LPCTSTR)iconPath,
			(iconPath.GetLength() + 1) * sizeof(TCHAR));
		RegCloseKey(hKey);
	}

	// 3. 设置打开命令
	result = RegCreateKeyEx(HKEY_CURRENT_USER, _T("Software\\Classes\\XiaoGongPDF.Document\\shell\\open\\command"),
		0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		RegSetValueEx(hKey, NULL, 0, REG_SZ, (BYTE*)(LPCTSTR)commandLine,
			(commandLine.GetLength() + 1) * sizeof(TCHAR));
		RegCloseKey(hKey);
	}

	// 4. 在 HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.pdf\UserChoice 设置
	// 注意：Windows 10+ 会对 UserChoice 进行哈希保护，直接修改可能无效
	// 删除现有的 UserChoice 键（需要管理员权限或用户权限）
	RegDeleteTree(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.pdf\\UserChoice"));

	// 5. 注册应用程序能力
	CString appName = _T("XiaoGongPDF.exe");
	CString capabilitiesPath;
	capabilitiesPath.Format(_T("Software\\%s\\Capabilities"), appName);

	result = RegCreateKeyEx(HKEY_CURRENT_USER, capabilitiesPath, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		CString appDesc = _T("小龚PDF阅读器");
		RegSetValueEx(hKey, _T("ApplicationDescription"), 0, REG_SZ,
			(BYTE*)(LPCTSTR)appDesc, (appDesc.GetLength() + 1) * sizeof(TCHAR));
		RegSetValueEx(hKey, _T("ApplicationName"), 0, REG_SZ,
			(BYTE*)(LPCTSTR)appDesc, (appDesc.GetLength() + 1) * sizeof(TCHAR));
		RegCloseKey(hKey);
	}

	// 6. 注册文件关联
	CString fileAssocPath;
	fileAssocPath.Format(_T("%s\\FileAssociations"), capabilitiesPath);
	result = RegCreateKeyEx(HKEY_CURRENT_USER, fileAssocPath, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		RegSetValueEx(hKey, _T(".pdf"), 0, REG_SZ, (BYTE*)_T("XiaoGongPDF.Document"),
			(DWORD)((_tcslen(_T("XiaoGongPDF.Document")) + 1) * sizeof(TCHAR)));
		RegCloseKey(hKey);
	}

	// 7. 注册到已注册应用程序列表
	CString regAppsPath = _T("Software\\RegisteredApplications");
	result = RegCreateKeyEx(HKEY_CURRENT_USER, regAppsPath, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		CString capabilitiesRegPath;
		capabilitiesRegPath.Format(_T("Software\\%s\\Capabilities"), appName);
		RegSetValueEx(hKey, appName, 0, REG_SZ, (BYTE*)(LPCTSTR)capabilitiesRegPath,
			(capabilitiesRegPath.GetLength() + 1) * sizeof(TCHAR));
		RegCloseKey(hKey);
	}

	// 通知系统文件关联已更改
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);

	// 检测Windows版本，如果是Windows 10及以上，打开系统设置
	OSVERSIONINFOEX osvi;
	ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
	osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);

	// 使用 RtlGetVersion 获取真实的系统版本（GetVersionEx 在 Windows 8.1+ 可能返回错误信息）
	typedef LONG(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
	HMODULE hNtdll = GetModuleHandle(_T("ntdll.dll"));
	if (hNtdll)
	{
		RtlGetVersionPtr RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hNtdll, "RtlGetVersion");
		if (RtlGetVersion)
		{
			RTL_OSVERSIONINFOW rovi = { 0 };
			rovi.dwOSVersionInfoSize = sizeof(rovi);
			if (RtlGetVersion(&rovi) == 0)
			{
				// Windows 10 的版本号是 10.0
				if (rovi.dwMajorVersion >= 10)
				{
					// Windows 10 及以上，打开默认应用设置页面
					ShellExecute(NULL, _T("open"), _T("ms-settings:defaultapps"), NULL, NULL, SW_SHOW);
					return; // 直接返回，不显示消息框
				}
			}
		}
	}

	// Windows 10 以下版本，显示成功消息
	MessageBox(_T("已成功设置为默认PDF阅读器！"), _T("提示"), MB_OK | MB_ICONINFORMATION);
}

void CXiaoGongPDFDlg::onMenuShortcuts()
{
#ifdef _DEBUG
	TRACE(_T("onMenuShortcuts() \n"));
#endif

	// 显示快捷键设置对话框
	if (m_shortcutsDialog.GetSafeHwnd() == NULL)
	{
		m_shortcutsDialog.Create(IDD_SHORTCUTS_DIALOG, this);
	}
	m_shortcutsDialog.ShowWindow(SW_SHOW);
	m_shortcutsDialog.SetForegroundWindow();
}

BOOL CXiaoGongPDFDlg::OnInitDialog()
{
#ifdef _DEBUG
	TRACE(_T("OnInitDialog() \n"));
#endif

	CDialogEx::OnInitDialog();

	// 添加最大化、最小化按钮
	ModifyStyle(0, WS_MINIMIZEBOX | WS_MAXIMIZEBOX);

	SetIcon(m_hIcon, TRUE);
	SetIcon(m_hIcon, FALSE);

	// 增加菜单
	SetMenu(CMenu::FromHandle(::LoadMenu(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDR_MENU))));

	// 显示PDF 需要设置控件类型如下
	// 设置 Picture Control 样式
	DWORD style = m_pdfView.GetStyle();
	style &= ~(SS_SIMPLE | SS_NOTIFY);  // 移除其他样式
	style |= SS_BITMAP | SS_CENTERIMAGE | WS_VSCROLL;  // ★★★ 添加垂直滚动条样式（连续滚动模式需要）
	::SetWindowLong(m_pdfView.GetSafeHwnd(), GWL_STYLE, style);

	// 更新控件
	m_pdfView.ModifyStyle(0, SS_BITMAP | SS_CENTERIMAGE | WS_VSCROLL);
	m_pdfView.Invalidate();
	m_pdfView.UpdateWindow();

	// ★★★ 初始化自定义PDF预览控件，使其能够转发滚动条消息
	m_pdfView.SetParentDlg(this);
	m_pdfView.SubclassWindow();

	// ★★★ 显示垂直滚动条（连续滚动模式需要）
	m_pdfView.ShowScrollBar(SB_VERT, TRUE);
	m_pdfView.EnableScrollBarCtrl(SB_VERT, TRUE);

	// 获取系统垂直滚动条的宽度
	m_scrollBarWidth = GetSystemMetrics(SM_CXVSCROLL);

	m_thumbnailList.ModifyStyle(WS_VISIBLE, 0);

	// 初始化状态栏
	m_statusBar.SetWindowText(_T("页码: 0 / 0"));

	// 初始化MuPDF上下文（多文档共享）
	if (!m_ctx) {
		m_ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
		if (!m_ctx) {
			MessageBox(_T("无法创建MuPDF上下文"), _T("错误"), MB_OK | MB_ICONERROR);
			return FALSE;
		}
#ifdef _DEBUG
		TRACE(_T("MuPDF上下文已创建: %p\n"), m_ctx);
#endif
		// 注册文档类型
		fz_try(m_ctx)
		{
			fz_register_document_handlers(m_ctx);
		}
		fz_catch(m_ctx)
		{
			MessageBox(_T("无法注册文档处理器"), _T("错误"), MB_OK | MB_ICONERROR);
			fz_drop_context(m_ctx);
			m_ctx = nullptr;
			return FALSE;
		}
	}

	// 初始化工具栏
	InitializeToolbar();

	// 初始化缩略图复选框（默认不勾选）
	m_checkThumbnail.SetCheck(BST_UNCHECKED);

	// 初始化标签页控件
	InitializeTabControl();

	// 设置对话框为可接收键盘消息的窗口
	SetFocus();

	// 启用拖放文件功能
	DragAcceptFiles(TRUE);

	// 更新最近文件菜单
	UpdateRecentFilesMenu();

	// 如果有从命令行传入的初始文件路径，延迟打开该文件
	// 使用PostMessage延迟打开，避免阻塞窗口显示
	if (!m_initialFilePath.IsEmpty())
	{
		// 使用PostMessage延迟打开，让窗口先显示
		PostMessage(WM_USER + 101, 0, 0);
	}


	// ===== 自定义按钮颜色配置 =====
	// 蓝色 SetColors(RGB(70, 130, 180), RGB(100, 149, 237), RGB(65, 105, 225));
	// 深邃蓝色 SetColors(RGB(41, 128, 185), RGB(52, 152, 219), RGB(31, 97, 141));
	// 活力橙色 SetColors(RGB(230, 126, 34), RGB(243, 156, 18), RGB(211, 84, 0));
	// 清新绿色  SetColors(RGB(39, 174, 96), RGB(46, 204, 113), RGB(30, 130, 76));
	// 警示红色  SetColors(RGB(192, 57, 43), RGB(231, 76, 60), RGB(169, 50, 38));
	// 优雅青色  SetColors(RGB(22, 160, 133), RGB(26, 188, 156), RGB(17, 122, 101));
	
	// 导航按钮 - 专业蓝色主题
	m_btnFirst.SetColors(RGB(70, 130, 180), RGB(100, 149, 237), RGB(65, 105, 225));
	m_btnLast.SetColors(RGB(70, 130, 180), RGB(100, 149, 237), RGB(65, 105, 225));
	
	// 全屏按钮
	m_btnFullscreen.SetColors(RGB(70, 130, 180), RGB(100, 149, 237), RGB(65, 105, 225));
	
	// 旋转按钮
	m_btnRotateLeft.SetColors(RGB(70, 130, 180), RGB(100, 149, 237), RGB(65, 105, 225));
	m_btnRotateRight.SetColors(RGB(70, 130, 180), RGB(100, 149, 237), RGB(65, 105, 225));

	// 编辑按钮
	m_btnEdit.SetColors(RGB(70, 130, 180), RGB(100, 149, 237), RGB(65, 105, 225));

	// ===== 颜色配置结束 =====
	return TRUE;  // 除非将焦点设置到控件，否则返回 TRUE
} 

void CXiaoGongPDFDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	CDialogEx::OnSysCommand(nID, lParam);
}

void CXiaoGongPDFDlg::OnPaint()
{
#ifdef _DEBUG
	TRACE(_T("OnPaint() \n"));
#endif

	if (IsIconic())
	{
		CPaintDC dc(this);

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

HBRUSH CXiaoGongPDFDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	HBRUSH hbr = CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);

	// 如果是标签页控件，返回白色背景
	if (pWnd->GetSafeHwnd() == m_tabCtrl.GetSafeHwnd())
	{
		pDC->SetBkColor(RGB(255, 255, 255));  // 设置文本背景为白色
		return m_whiteBrush;
	}

	return hbr;
}

HCURSOR CXiaoGongPDFDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

void CXiaoGongPDFDlg::CleanupCurrentPage()
{
#ifdef _DEBUG
	TRACE(_T("CleanupCurrentPage() \n"));
#endif

	if (m_currentPageObj && m_ctx)
	{
		fz_try(m_ctx)
		{
			fz_drop_page(m_ctx, m_currentPageObj);
			m_currentPageObj = nullptr;
		}
		fz_catch(m_ctx)
		{
			// Handle error if needed
		}
	}
}

bool CXiaoGongPDFDlg::RenderPage(int pageNumber)
{
#ifdef _DEBUG
	TRACE(_T("RenderPage() :  %d\n"), pageNumber);
#endif

	if (!m_ctx || !m_doc || pageNumber < 0 || pageNumber >= m_totalPages)
		return false;

	// ★★★ 切换页面时保存当前页面状态并恢复目标页面状态
	if (pageNumber != m_currentPage && !m_isDragging)
	{
		// 先保存当前页面的状态（包括缩放和平移位置）
		SaveCurrentPageZoomState();

		// 恢复目标页面的状态（包括缩放和平移位置）
		RestorePageZoomState(pageNumber);

		// ★★★ 注意：不再调用 ResetPanOffset()，因为平移位置已在 RestorePageZoomState 中恢复
		// 清除拖拽缓存位图（因为切换到了新页面）
		if (m_hPanPageBitmap)
		{
			DeleteObject(m_hPanPageBitmap);
			m_hPanPageBitmap = NULL;
		}
	}

	//增加页面渲染缓存机制 start
	// 按分辨率缓存，进行优化，避免大界面小图片缓存
	CRect rect;
	m_pdfView.GetClientRect(&rect);
	int renderWidth = rect.Width();
	int renderHeight = rect.Height();

	// ★★★ 获取旋转角度，包含在缓存键中，确保不同旋转角度的页面使用不同的缓存
	int rotation = GetPageRotation(pageNumber);

	// 生成缓存键（包含旋转角度）
	PageCacheKey key{ pageNumber, renderWidth, renderHeight, rotation };

	// ★★★ 检查缓存是否存在（拖拽时有偏移量，不使用缓存）
	bool hasPanOffset = (m_panOffset.x != 0 || m_panOffset.y != 0);
	auto it = m_pageCache.find(key);
	if (it != m_pageCache.end() && !hasPanOffset) {
		TRACE(_T("[CACHE] 命中页面 %d @ %dx%d, 旋转=%d°\n"), pageNumber, renderWidth, renderHeight, rotation);

		// ★★★ 重要：即使使用缓存，也要更新 m_currentPageObj
		// 因为拖拽功能依赖 m_currentPageObj 来渲染当前页
		CSingleLock lock(&m_renderLock, TRUE);

		// 先清理旧页面对象
		if (m_currentPageObj && pageNumber != m_currentPage)
		{
			fz_try(m_ctx)
			{
				fz_drop_page(m_ctx, m_currentPageObj);
				m_currentPageObj = nullptr;
			}
			fz_catch(m_ctx)
			{
				// 忽略错误
			}
		}

		// 加载新页面对象
		if (pageNumber != m_currentPage || !m_currentPageObj)
		{
			fz_try(m_ctx)
			{
				m_currentPageObj = fz_load_page(m_ctx, m_doc, pageNumber);
			}
			fz_catch(m_ctx)
			{
				m_currentPageObj = nullptr;
			}
		}

		// ★★★ 重要：先释放旧的 m_hCurrentBitmap，因为我们要从缓存中使用位图
		if (m_hCurrentBitmap)
		{
			DeleteObject(m_hCurrentBitmap);
			m_hCurrentBitmap = NULL;
		}

		// ★★★ 复制缓存中的位图到 m_hCurrentBitmap，保持 m_hCurrentBitmap 的所有权语义
		m_hCurrentBitmap = (HBITMAP)CopyImage(it->second.hBitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

		if (!m_hCurrentBitmap)
		{
			// 如果复制失败，直接使用缓存中的位图（虽然不理想，但至少能显示）
			m_pdfView.SetBitmap(it->second.hBitmap);
		}
		else
		{
			// 在位图上绘制搜索高亮（如果有搜索结果）
			if (!m_searchMatches.empty() && m_hCurrentBitmap)
			{
				CDC* pDC = m_pdfView.GetDC();
				CDC memDCHighlight;
				memDCHighlight.CreateCompatibleDC(pDC);
				HBITMAP hOldBmpHighlight = (HBITMAP)memDCHighlight.SelectObject(m_hCurrentBitmap);

				// 调用高亮绘制函数
				HighlightSearchMatches(&memDCHighlight, pageNumber);

				memDCHighlight.SelectObject(hOldBmpHighlight);
				m_pdfView.ReleaseDC(pDC);
			}

			// 设置复制后的位图到控件
			m_pdfView.SetBitmap(m_hCurrentBitmap);
		}

		// 强制刷新控件显示
		m_pdfView.Invalidate(FALSE);
		m_pdfView.UpdateWindow();

		m_currentPage = pageNumber;
		UpdatePageControls();
		HighlightCurrentThumbnail();

		// 更新使用顺序（LRU）
		for (auto listIt = m_cacheOrder.begin(); listIt != m_cacheOrder.end(); ++listIt) {
			if (*listIt == key) {
				m_cacheOrder.erase(listIt);
				break;
			}
		}
		m_cacheOrder.push_front(key);

		return true;
	}

	// end

	/*没有缓存正常渲染*/

	// 确保先清理旧资源
	CleanupBitmap();
	CleanupCurrentPage();   // ★★★ 新增：避免旧页面对象泄漏

	// ★★★ 添加线程锁保护 MuPDF 调用
	CSingleLock lock(&m_renderLock, TRUE);

	fz_pixmap* pixmap = nullptr;
	fz_device* dev = nullptr;

	fz_try(m_ctx)
	{
		// 加载新页面
		m_currentPageObj = fz_load_page(m_ctx, m_doc, pageNumber);
		if (!m_currentPageObj) {
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "无法加载页面");
		}

		// 获取页面大小
		fz_rect bounds = fz_bound_page(m_ctx, m_currentPageObj);

		// 获取控件大小
		CRect rect;
		m_pdfView.GetClientRect(&rect);

		// 计算缩放比例
		float origPageWidth = bounds.x1 - bounds.x0;
		float origPageHeight = bounds.y1 - bounds.y0;

		// ★★★ 获取旋转角度
		int rotation = GetPageRotation(pageNumber);

		// ★★★ 计算旋转后的有效页面尺寸（用于缩放计算）
		// 如果旋转90°或270°，页面的宽高会交换
		float effectivePageWidth = origPageWidth;
		float effectivePageHeight = origPageHeight;
		if (rotation == 90 || rotation == 270)
		{
			effectivePageWidth = origPageHeight;
			effectivePageHeight = origPageWidth;
		}

		// 视图尺寸保持不变
		int viewWidth = rect.Width();
		int viewHeight = rect.Height();

#ifdef _DEBUG
		TRACE(_T("页面尺寸: %.2f x %.2f, 旋转=%d°, 有效页面尺寸: %.2f x %.2f, 视图尺寸=%d x %d\n"),
			origPageWidth, origPageHeight, rotation, effectivePageWidth, effectivePageHeight, viewWidth, viewHeight);
#endif

		float scale;

		// 基于旋转后的有效页面尺寸和视图尺寸计算scale
		switch (m_zoomMode)
		{
		case ZOOM_FIT_WIDTH:
			scale = viewWidth / effectivePageWidth * 0.95f;
			break;
		case ZOOM_FIT_PAGE:
			{
				float scaleX = viewWidth / effectivePageWidth;
				float scaleY = viewHeight / effectivePageHeight;
				scale = min(scaleX, scaleY) * 0.95f;
			}
			break;
		case ZOOM_CUSTOM:
			scale = m_customZoom;
			break;
		default:
			{
				float scaleX = viewWidth / effectivePageWidth;
				float scaleY = viewHeight / effectivePageHeight;
				scale = min(scaleX, scaleY) * 0.95f;
			}
			break;
		}

#ifdef _DEBUG
		TRACE(_T("计算缩放比例: scale=%.4f, viewSize=(%d,%d)\n"), scale, rect.Width(), rect.Height());
#endif

		// ★★★ 创建pixmap：MuPDF总是按原始方向渲染，所以用原始页面尺寸
		int width = (int)(origPageWidth * scale);
		int height = (int)(origPageHeight * scale);
		pixmap = fz_new_pixmap(m_ctx, fz_device_rgb(m_ctx), width, height, nullptr, 1);
		fz_clear_pixmap_with_value(m_ctx, pixmap, 0xff);

		// 渲染页面
		fz_matrix ctm = fz_scale(scale, scale);
		dev = fz_new_draw_device(m_ctx, ctm, pixmap);
		fz_run_page(m_ctx, m_currentPageObj, dev, fz_identity, nullptr);
		fz_close_device(m_ctx, dev);
		fz_drop_device(m_ctx, dev);
		dev = nullptr;

		// 创建DIB
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = width;
		bmi.bmiHeader.biHeight = -height;  // 负值表示自上而下的位图
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		// 创建DIB位图
		CDC* pDC = m_pdfView.GetDC();
		BYTE* pbBits = nullptr;
		m_hCurrentBitmap = CreateDIBSection(pDC->GetSafeHdc(), &bmi,
			DIB_RGB_COLORS, (void**)&pbBits, NULL, 0);

		if (m_hCurrentBitmap && pbBits)
		{
			// 复制像素数据
			unsigned char* samples = fz_pixmap_samples(m_ctx, pixmap);
			int stride = (width * 3 + 3) & ~3;
			int n = fz_pixmap_components(m_ctx, pixmap);

			for (int y = 0; y < height; y++) {
				for (int x = 0; x < width; x++) {
					pbBits[y * stride + x * 3 + 0] = samples[(y * width + x) * n + 2];
					pbBits[y * stride + x * 3 + 1] = samples[(y * width + x) * n + 1];
					pbBits[y * stride + x * 3 + 2] = samples[(y * width + x) * n + 0];
				}
			}

			// 应用旋转（如果需要）- rotation 已在前面第849行定义过
			// int rotation = GetPageRotation(pageNumber);  // 注释掉，避免重复定义
			if (rotation != 0)
			{
				HBITMAP hRotatedBitmap = RotateBitmap(m_hCurrentBitmap, rotation);
				if (hRotatedBitmap)
				{
					// 替换为旋转后的位图
					DeleteObject(m_hCurrentBitmap);
					m_hCurrentBitmap = hRotatedBitmap;

					// 更新旋转后的尺寸
					if (rotation == 90 || rotation == 270)
					{
						int temp = width;
						width = height;
						height = temp;
					}
				}
			}

			// ★★★ 只在自定义缩放模式+有平移偏移量时才添加背景和居中
			// 旋转不应该触发强制居中，应该按正常缩放模式显示
			bool needsBackground = (m_zoomMode == ZOOM_CUSTOM && (m_panOffset.x != 0 || m_panOffset.y != 0));

#ifdef _DEBUG
			TRACE(_T("检查是否需要背景: rotation=%d, zoomMode=%d, panOffset=(%d,%d), needsBackground=%d\n"),
				rotation, m_zoomMode, m_panOffset.x, m_panOffset.y, needsBackground);
#endif

			if (needsBackground)
			{
				// ★★★ 获取旋转后位图的实际尺寸
				BITMAP bm;
				if (!GetObject(m_hCurrentBitmap, sizeof(BITMAP), &bm))
				{
					// 获取位图信息失败，使用变量值
					bm.bmWidth = width;
					bm.bmHeight = height;
				}
				int actualWidth = bm.bmWidth;
				int actualHeight = bm.bmHeight;

#ifdef _DEBUG
				TRACE(_T("旋转后位图实际尺寸: %d x %d (原始变量: %d x %d)\n"),
					actualWidth, actualHeight, width, height);
#endif

				// 获取PDF视图控件的大小
				CRect viewRect;
				m_pdfView.GetClientRect(&viewRect);
				int viewWidth = viewRect.Width();
				int viewHeight = viewRect.Height();

#ifdef _DEBUG
				TRACE(_T("视图控件尺寸: %d x %d\n"), viewWidth, viewHeight);
#endif

				// 创建一个与视图大小相同的新位图
				BITMAPINFO newBmi = { 0 };
				newBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				newBmi.bmiHeader.biWidth = viewWidth;
				newBmi.bmiHeader.biHeight = -viewHeight;
				newBmi.bmiHeader.biPlanes = 1;
				newBmi.bmiHeader.biBitCount = 24;
				newBmi.bmiHeader.biCompression = BI_RGB;

				BYTE* pNewBits = nullptr;
				HBITMAP hNewBitmap = CreateDIBSection(pDC->GetSafeHdc(), &newBmi,
					DIB_RGB_COLORS, (void**)&pNewBits, NULL, 0);

				if (hNewBitmap && pNewBits)
				{
					// 创建内存DC
					CDC memDC, targetDC;
					memDC.CreateCompatibleDC(pDC);
					targetDC.CreateCompatibleDC(pDC);

					HBITMAP hOldBmp1 = (HBITMAP)memDC.SelectObject(m_hCurrentBitmap);
					HBITMAP hOldBmp2 = (HBITMAP)targetDC.SelectObject(hNewBitmap);

					// 填充浅灰色背景（让白色PDF页面更明显）
					targetDC.FillSolidRect(0, 0, viewWidth, viewHeight, RGB(240, 240, 240));

					// ★★★ 使用实际位图尺寸计算居中位置（带偏移）
					int xPos = (viewWidth - actualWidth) / 2 + m_panOffset.x;
					int yPos = (viewHeight - actualHeight) / 2 + m_panOffset.y;

#ifdef _DEBUG
					TRACE(_T("居中位置: xPos=%d, yPos=%d, panOffset=(%d, %d)\n"),
						xPos, yPos, m_panOffset.x, m_panOffset.y);
#endif

					// ★★★ 绘制PDF页面（使用实际位图尺寸）
					targetDC.BitBlt(xPos, yPos, actualWidth, actualHeight, &memDC, 0, 0, SRCCOPY);

					// ★★★ 绘制页面边框（深灰色，使用实际位图尺寸）
					CPen borderPen(PS_SOLID, 1, RGB(128, 128, 128));
					CPen* pOldPen = targetDC.SelectObject(&borderPen);
					targetDC.SelectStockObject(NULL_BRUSH);  // 不填充
					targetDC.Rectangle(xPos - 1, yPos - 1, xPos + actualWidth + 1, yPos + actualHeight + 1);
					targetDC.SelectObject(pOldPen);

					memDC.SelectObject(hOldBmp1);
					targetDC.SelectObject(hOldBmp2);

					// 替换位图
					DeleteObject(m_hCurrentBitmap);
					m_hCurrentBitmap = hNewBitmap;
				}
			}

			// 在位图上绘制搜索高亮（如果有搜索结果）
			if (!m_searchMatches.empty() && m_hCurrentBitmap)
			{
				CDC memDCHighlight;
				memDCHighlight.CreateCompatibleDC(pDC);
				HBITMAP hOldBmpHighlight = (HBITMAP)memDCHighlight.SelectObject(m_hCurrentBitmap);

				// 调用高亮绘制函数
				HighlightSearchMatches(&memDCHighlight, pageNumber);

				memDCHighlight.SelectObject(hOldBmpHighlight);
			}

			// 设置新位图到控件
			m_pdfView.SetBitmap(m_hCurrentBitmap);

			// 强制刷新控件显示
			m_pdfView.Invalidate(FALSE);
			m_pdfView.UpdateWindow();
		}

		// 清理资源
		fz_drop_pixmap(m_ctx, pixmap);
		pixmap = nullptr;
		m_pdfView.ReleaseDC(pDC);

		// 更新当前页码
		m_currentPage = pageNumber;

		// 更新工具栏上的页码显示
		UpdatePageControls();

		HighlightCurrentThumbnail();


		// 增加页面渲染缓存机制 start
		// 按分辨率缓存，进行优化，避免大界面小图片缓存
		// ★★★ 只在没有平移偏移量时才缓存
		if (m_hCurrentBitmap && !hasPanOffset)
		{
			// 控制缓存上限
			if ((int)m_pageCache.size() >= CACHE_LIMIT)
			{
				// 删除最久未使用的项（LRU）
				auto oldestKey = m_cacheOrder.back();
				m_cacheOrder.pop_back();

				auto oldIt = m_pageCache.find(oldestKey);
				if (oldIt != m_pageCache.end()) {
					if (oldIt->second.hBitmap)
						DeleteObject(oldIt->second.hBitmap);
					m_pageCache.erase(oldIt);
				}
			}

			// 拷贝当前页的位图（防止后续被释放）
			HBITMAP hCopy = (HBITMAP)CopyImage(
				m_hCurrentBitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

			if (hCopy)
			{
				PageCacheItem item{ hCopy };
				m_pageCache[key] = item;
				m_cacheOrder.push_front(key);

				TRACE(_T("[CACHE] 新增页面 %d @ %dx%d, 旋转=%d°\n"),
					pageNumber, renderWidth, renderHeight, rotation);
			}
		}
		// end
	}
	fz_catch(m_ctx)
	{
		// ★★★ 修复：获取并显示详细的 MuPDF 错误信息
		const char* errorMsg = fz_caught_message(m_ctx);
		CString errorText;
		errorText.Format(_T("渲染页面 %d 时发生错误:\n%S"), pageNumber + 1, errorMsg ? errorMsg : "未知错误");

		// 清理资源
		if (dev) fz_drop_device(m_ctx, dev);
		if (pixmap) fz_drop_pixmap(m_ctx, pixmap);
		CleanupBitmap();

#ifdef _DEBUG
		TRACE(_T("RenderPage 错误: %s\n"), errorText);
#endif

		MessageBox(errorText, _T("PDF渲染错误"), MB_OK | MB_ICONERROR);
		m_canDrag = false;  // ★★★ 渲染失败，禁用拖动
		return false;
	}

	// ★★★ 更新拖动判断缓存
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	BITMAP bm;
	if (m_hCurrentBitmap && GetObject(m_hCurrentBitmap, sizeof(bm), &bm))
	{
		// 注意：这里使用 m_hCurrentBitmap 可能是带偏移的视图大小位图
		// 但在非拖动状态下，它就是原始页面大小的位图
		// 如果有 panOffset，说明页面已经被拖动过，应该能继续拖动
		bool hasPanOffset = (m_panOffset.x != 0 || m_panOffset.y != 0);
		if (hasPanOffset)
		{
			// 有偏移，说明之前能拖动，现在也能拖动
			m_canDrag = true;
		}
		else
		{
			// 没有偏移，正常判断
			m_canDrag = (bm.bmWidth > viewRect.Width()) || (bm.bmHeight > viewRect.Height());
		}
	}
	else
	{
		m_canDrag = false;
	}

	return true;
}

// ============================================================================
// 统一的页面跳转函数（支持分页模式和连续滚动模式）
// ============================================================================
void CXiaoGongPDFDlg::GoToPage(int pageNumber)
{
	// 验证页码范围
	if (pageNumber < 0 || pageNumber >= m_totalPages)
		return;

	if (m_continuousScrollMode)
	{
		// 连续滚动模式：滚动到目标页面位置
		if (!m_pageYPositions.empty() && pageNumber < (int)m_pageYPositions.size())
		{
			// 滚动到该页面的顶部
			m_scrollPosition = m_pageYPositions[pageNumber];

			// 限制滚动范围
			CRect viewRect;
			m_pdfView.GetClientRect(&viewRect);
			int maxScroll = m_totalScrollHeight - viewRect.Height();
			if (maxScroll < 0) maxScroll = 0;
			if (m_scrollPosition > maxScroll) m_scrollPosition = maxScroll;

			// 更新滚动条和显示
			UpdateScrollBar();
			RenderVisiblePages();
		}
	}
	else
	{
		// 分页模式：渲染目标页面
		if (pageNumber != m_currentPage)
		{
			CleanupBitmap();
			RenderPage(pageNumber);
		}
	}
}

void CXiaoGongPDFDlg::RenderPDF(const char* filename)
{
#ifdef _DEBUG
	TRACE(_T("RenderPDF() :  %s\n"), filename);
#endif
	// ★★★ 清空缓存（只调用一次统一的清理函数）
	ClearPageCache();

	// 根据 m_thumbnailVisible 设置缩略图列表的显示状态
	if (m_thumbnailVisible)
		m_thumbnailList.ModifyStyle(0, WS_VISIBLE);
	else
		m_thumbnailList.ModifyStyle(WS_VISIBLE, 0);

	// 清理之前的资源
	CleanupThumbnails();  // 清理缩略图
	CleanupBitmap();      // 清理当前页面位图
	CleanupCurrentPage(); // 清理当前页面对象

	m_statusBar.SetWindowText(_T("正在加载，请稍候..."));
	UpdateWindow();

	// ★★★ 添加线程锁保护 MuPDF 调用
	CSingleLock lock(&m_renderLock, TRUE);

	if (m_doc) {
		fz_drop_document(m_ctx, m_doc);
		m_doc = nullptr;
	}
	if (m_ctx) {
		fz_drop_context(m_ctx);
		m_ctx = nullptr;
	}

	// 重置页码相关变量
	m_currentPage = 0;
	m_totalPages = 0;
	m_thumbnailPicWidth = 0;
	m_thumbnailPicHeight = 0;

	// ★★★ 清除所有页面状态
	m_pageRotations.clear();      // 清除旋转信息
	m_pageZoomStates.clear();     // 清除缩放状态

	// ★★★ 重置全局缩放状态为默认值
	m_zoomMode = ZOOM_FIT_PAGE;   // 默认适应页面
	m_customZoom = 1.0f;          // 默认100%

	// ★★★ 清除平移偏移量
	ResetPanOffset();

	// 创建新的context
	m_ctx = fz_new_context(nullptr, nullptr, FZ_STORE_UNLIMITED);
	if (!m_ctx) {
		MessageBox(_T("无法创建MuPDF上下文"), _T("错误"), MB_OK | MB_ICONERROR);
		return;
	}

	fz_try(m_ctx)
	{
		// 注册文档处理器
		fz_register_document_handlers(m_ctx);

		// 打开文档
		m_doc = fz_open_document(m_ctx, filename);
		if (!m_doc) {
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "无法打开PDF文档");
		}

		// 获取总页数
		m_totalPages = fz_count_pages(m_ctx, m_doc);

		// ★★★ 计算文档统一缩放比例（确保所有页面宽度一致）
		// 遍历所有页面，找到最大宽度
		m_documentMaxPageWidth = 0.0f;
		for (int i = 0; i < m_totalPages; i++)
		{
			fz_page* page = fz_load_page(m_ctx, m_doc, i);
			if (page)
			{
				fz_rect bounds = fz_bound_page(m_ctx, page);
				float pageWidth = bounds.x1 - bounds.x0;
				float pageHeight = bounds.y1 - bounds.y0;

				// 根据旋转角度调整（如果有保存的旋转状态）
				int rotation = GetPageRotation(i);
				if (rotation == 90 || rotation == 270)
				{
					float temp = pageWidth;
					pageWidth = pageHeight;
					pageHeight = temp;
				}

				if (pageWidth > m_documentMaxPageWidth)
					m_documentMaxPageWidth = pageWidth;

				fz_drop_page(m_ctx, page);
			}
		}

		// 基于最大宽度计算统一的缩放比例
		// 使用预览区域的宽度作为参考
		CRect viewRect;
		m_pdfView.GetClientRect(&viewRect);
		m_documentUniformScale = (m_documentMaxPageWidth > 0) ? ((float)viewRect.Width() / m_documentMaxPageWidth * 0.95f) : 1.0f;

#ifdef _DEBUG
		TRACE(_T("文档最大页面宽度: %.2f, 统一缩放比例: %.4f\n"), m_documentMaxPageWidth, m_documentUniformScale);
#endif

		// 初始化缩略图
		UpdateThumbnails();

		// ★★★ 根据模式渲染页面
		m_currentPage = 0;
		if (m_continuousScrollMode)
		{
			// 连续滚动模式：计算所有页面位置并渲染可见页面
			CalculatePagePositions();
			UpdateScrollBar();

			// ★★★ 确保滚动条可见
			m_pdfView.ShowScrollBar(SB_VERT, TRUE);
			m_pdfView.EnableScrollBarCtrl(SB_VERT, TRUE);

			RenderVisiblePages();
		}
		else
		{
			// 分页模式：渲染第一页
			RenderPage(m_currentPage);
		}

		// 更新页码控件
		UpdatePageControls();
	}
	fz_catch(m_ctx)
	{
		// ★★★ 修复：获取并显示详细的 MuPDF 错误信息
		const char* errorMsg = fz_caught_message(m_ctx);
		CString errorText;
		errorText.Format(_T("加载PDF文件时发生错误:\n文件: %S\n错误: %S"),
			filename, errorMsg ? errorMsg : "未知错误");

#ifdef _DEBUG
		TRACE(_T("RenderPDF 错误: %s\n"), errorText);
#endif

		MessageBox(errorText, _T("PDF加载错误"), MB_OK | MB_ICONERROR);

		// 恢复状态栏显示
		m_statusBar.SetWindowText(_T("页码: 0 / 0"));
	}
}

void CXiaoGongPDFDlg::InitializeThumbnailList()
{
#ifdef _DEBUG
	TRACE(_T("InitializeThumbnailList()"));
#endif

	if (!m_thumbnailList.GetSafeHwnd())
		return;

	// 移除所有其他视图样式，设置为单列图标视图
	DWORD removeStyle = LVS_TYPEMASK | LVS_ALIGNMASK;
	DWORD addStyle = LVS_ICON | LVS_ALIGNTOP | LVS_NOCOLUMNHEADER;
	m_thumbnailList.ModifyStyle(removeStyle, addStyle);

	// 设置扩展样式 - 禁用水平滚动条
	DWORD exStyle = m_thumbnailList.GetExtendedStyle();
	m_thumbnailList.SetExtendedStyle(exStyle | LVS_EX_FULLROWSELECT | LVS_EX_AUTOSIZECOLUMNS);

	// 清除所有列和项
	m_thumbnailList.DeleteAllItems();
	while (m_thumbnailList.DeleteColumn(0)) {}

	// 设置单列宽度等于控件宽度
	m_thumbnailList.InsertColumn(0, _T(""), LVCFMT_LEFT, LIST_WIDTH);

	// 确保缩略图列表可见（如果缩略图面板开启）
	if (m_thumbnailVisible)
	{
		m_thumbnailList.ShowWindow(SW_SHOW);
	}
}

bool CXiaoGongPDFDlg::RenderThumbnail(int pageNumber)
{
#ifdef _DEBUG
	TRACE(_T("RenderThumbnail() :  %d\n"), pageNumber);
#endif

	if (!m_ctx || !m_doc || pageNumber < 0 || pageNumber >= m_totalPages)
		return false;

	// 检查缓存
	auto it = m_thumbnailCache.find(pageNumber);
	bool useCache = (it != m_thumbnailCache.end());

	HBITMAP hBitmap = NULL;
	int thumbWidth = THUMBNAIL_WIDTH;
	int thumbHeight = 0;

	if (useCache)
	{
		// 使用缓存的位图
		hBitmap = it->second.hBitmap;

		// 获取缓存位图的尺寸
		BITMAP bmpInfo;
		if (::GetObject(hBitmap, sizeof(BITMAP), &bmpInfo))
		{
			thumbWidth = bmpInfo.bmWidth;
			thumbHeight = bmpInfo.bmHeight;
		}

#ifdef _DEBUG
		TRACE(_T("使用缓存的缩略图: 页面 %d, 尺寸 %d x %d\n"), pageNumber, thumbWidth, thumbHeight);
#endif
	}
	else
	{
		// 需要渲染新缩略图
		// ★★★ 添加线程锁保护 MuPDF 调用
		CSingleLock lock(&m_renderLock, TRUE);

		fz_page* page = nullptr;
		fz_pixmap* pixmap = nullptr;
		fz_device* dev = nullptr;

		fz_try(m_ctx)
		{
			// 加载页面
			page = fz_load_page(m_ctx, m_doc, pageNumber);
			if (!page)
			{
				fz_throw(m_ctx, FZ_ERROR_GENERIC, "无法加载页面");
			}

			// 获取页面大小
			fz_rect bounds = fz_bound_page(m_ctx, page);
			float pageWidth = bounds.x1 - bounds.x0;
			float pageHeight = bounds.y1 - bounds.y0;

			// 计算缩放比例,保持宽度为220
			thumbWidth = THUMBNAIL_WIDTH;
			float scale = thumbWidth / pageWidth;
			int pageThumbHeight = (int)(pageHeight * scale);

			// 使用统一的缩略图高度(取较大值以容纳所有页面)
			// 第一次渲染时确定统一高度
			if (m_thumbnailPicHeight == 0)
			{
				m_thumbnailPicHeight = pageThumbHeight;
				m_thumbnailPicWidth = thumbWidth;
			}

			// 使用已确定的统一高度,如果当前页需要更大高度则更新
			thumbHeight = max(m_thumbnailPicHeight, pageThumbHeight);
			if (thumbHeight > m_thumbnailPicHeight)
			{
				m_thumbnailPicHeight = thumbHeight;
			}

			// 创建pixmap用于渲染PDF内容
			pixmap = fz_new_pixmap(m_ctx, fz_device_rgb(m_ctx), thumbWidth, pageThumbHeight, nullptr, 1);
			fz_clear_pixmap_with_value(m_ctx, pixmap, 0xff);

			// 渲染页面
			fz_matrix ctm = fz_scale(scale, scale);
			dev = fz_new_draw_device(m_ctx, ctm, pixmap);
			fz_run_page(m_ctx, page, dev, fz_identity, nullptr);
			fz_close_device(m_ctx, dev);
			fz_drop_device(m_ctx, dev);
			dev = nullptr;

			// 创建统一尺寸的DIB位图(宽度220,高度为统一高度)
			BITMAPINFO bmi = { 0 };
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = thumbWidth;
			bmi.bmiHeader.biHeight = -thumbHeight;  // 使用统一高度
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 24;
			bmi.bmiHeader.biCompression = BI_RGB;

			// 创建缩略图位图
			CDC dc;
			dc.CreateCompatibleDC(NULL);
			BYTE* pbBits = nullptr;
			hBitmap = CreateDIBSection(dc.GetSafeHdc(), &bmi, DIB_RGB_COLORS, (void**)&pbBits, NULL, 0);

			if (hBitmap && pbBits)
			{
				// 先用浅灰色填充整个位图（作为边框的背景色）
				int stride = (thumbWidth * 3 + 3) & ~3;
				// 使用浅灰色 RGB(240, 240, 240) 作为背景
				for (int y = 0; y < thumbHeight; y++) {
					for (int x = 0; x < thumbWidth; x++) {
						pbBits[y * stride + x * 3 + 0] = 240;  // B
						pbBits[y * stride + x * 3 + 1] = 240;  // G
						pbBits[y * stride + x * 3 + 2] = 240;  // R
					}
				}

				// 计算内容区域（留出边框空间）
				const int borderWidth = 4;  // 边框宽度为4像素
				int contentWidth = thumbWidth - borderWidth * 2;
				int contentHeight = thumbHeight - borderWidth * 2;

				// 先用白色填充内容区域
				for (int y = borderWidth; y < thumbHeight - borderWidth; y++) {
					for (int x = borderWidth; x < thumbWidth - borderWidth; x++) {
						pbBits[y * stride + x * 3 + 0] = 255;  // B
						pbBits[y * stride + x * 3 + 1] = 255;  // G
						pbBits[y * stride + x * 3 + 2] = 255;  // R
					}
				}

				// 计算垂直居中的偏移量（在内容区域内）
				int yOffset = borderWidth + (contentHeight - pageThumbHeight) / 2;

				// 复制PDF渲染的像素数据到位图(在内容区域内居中放置)
				unsigned char* samples = fz_pixmap_samples(m_ctx, pixmap);
				int n = fz_pixmap_components(m_ctx, pixmap);

				for (int y = 0; y < pageThumbHeight; y++) {
					int destY = y + yOffset;
					for (int x = 0; x < thumbWidth; x++) {
						int destX = x;
						// 确保在内容区域内
						if (destX >= borderWidth && destX < thumbWidth - borderWidth &&
							destY >= borderWidth && destY < thumbHeight - borderWidth) {
							pbBits[destY * stride + destX * 3 + 0] = samples[(y * thumbWidth + x) * n + 2];  // B
							pbBits[destY * stride + destX * 3 + 1] = samples[(y * thumbWidth + x) * n + 1];  // G
							pbBits[destY * stride + destX * 3 + 2] = samples[(y * thumbWidth + x) * n + 0];  // R
						}
					}
				}

				// 应用旋转（如果需要）
				int rotation = GetPageRotation(pageNumber);
				if (rotation != 0)
				{
					HBITMAP hRotatedBitmap = RotateBitmap(hBitmap, rotation);
					if (hRotatedBitmap)
					{
						// 替换为旋转后的位图
						DeleteObject(hBitmap);
						hBitmap = hRotatedBitmap;

						// 更新旋转后的尺寸
						if (rotation == 90 || rotation == 270)
						{
							int temp = thumbWidth;
							thumbWidth = thumbHeight;
							thumbHeight = temp;
						}

#ifdef _DEBUG
						TRACE(_T("缩略图应用旋转: 页面 %d, 角度 %d, 新尺寸 %d x %d\n"),
							pageNumber, rotation, thumbWidth, thumbHeight);
#endif
					}
				}

				// 添加到缓存
				ThumbnailInfo info = { hBitmap, pageNumber, pageNumber == m_currentPage };
				m_thumbnailCache[pageNumber] = info;

#ifdef _DEBUG
				TRACE(_T("新渲染缩略图: 页面 %d, 尺寸 %d x %d\n"), pageNumber, thumbWidth, thumbHeight);
#endif
			}
			else
			{
				fz_throw(m_ctx, FZ_ERROR_GENERIC, "无法创建位图");
			}

			// 清理资源
			fz_drop_pixmap(m_ctx, pixmap);
			pixmap = nullptr;
			fz_drop_page(m_ctx, page);
			page = nullptr;
		}
		fz_catch(m_ctx)
		{
			// ★★★ 修复：获取并显示详细的 MuPDF 错误信息
			const char* errorMsg = fz_caught_message(m_ctx);

#ifdef _DEBUG
			TRACE(_T("RenderThumbnail 错误 (页面 %d): %S\n"), pageNumber + 1, errorMsg ? errorMsg : "未知错误");
#endif

			// 确保所有资源被释放
			if (dev) fz_drop_device(m_ctx, dev);
			if (pixmap) fz_drop_pixmap(m_ctx, pixmap);
			if (page) fz_drop_page(m_ctx, page);
			return false;
		}
	}

	// ★★★ 添加到列表控件 - 无论是使用缓存还是新渲染，都需要添加到ImageList
	if (hBitmap && m_thumbnailList.GetSafeHwnd())
	{
		// 获取或创建图片列表
		CImageList* pImageList = m_thumbnailList.GetImageList(LVSIL_NORMAL);
		if (!pImageList)
		{
			pImageList = new CImageList();
			// 使用统一高度创建ImageList
			if (thumbHeight == 0)
			{
				thumbHeight = m_thumbnailPicHeight > 0 ? m_thumbnailPicHeight : 300;  // 默认高度
			}
			if (pImageList->Create(thumbWidth, thumbHeight, ILC_COLOR24, m_totalPages, 1))
			{
				m_thumbnailList.SetImageList(pImageList, LVSIL_NORMAL);
				pImageList->Detach();
				delete pImageList;
				pImageList = m_thumbnailList.GetImageList(LVSIL_NORMAL);
			}
			else
			{
				delete pImageList;
				return false;
			}
		}
		else
		{
			// 如果ImageList已存在,但当前高度更大,需要重建ImageList
			IMAGEINFO imgInfo;
			if (pImageList->GetImageInfo(0, &imgInfo))
			{
				int currentHeight = imgInfo.rcImage.bottom - imgInfo.rcImage.top;
				if (thumbHeight > currentHeight)
				{
					// 需要重建ImageList以使用更大的高度
					// 暂时先不处理这种情况,因为我们会先遍历所有页面找最大高度
				}
			}
		}

		if (pImageList)
		{
			int imageIndex = pImageList->Add(CBitmap::FromHandle(hBitmap), RGB(0, 0, 0));
			if (imageIndex != -1)
			{
				CString strPageNum;
				strPageNum.Format(_T("第 %d 页"), pageNumber + 1);

				// 检查该页面是否已经在列表中
				int itemIndex = -1;
				for (int i = 0; i < m_thumbnailList.GetItemCount(); i++)
				{
					if (m_thumbnailList.GetItemData(i) == (DWORD_PTR)pageNumber)
					{
						itemIndex = i;
						break;
					}
				}

				if (itemIndex == -1)
				{
					// 如果不存在，插入到正确的位置
					// 找到应该插入的位置（保持页面顺序）
					int insertPos = 0;
					for (int i = 0; i < m_thumbnailList.GetItemCount(); i++)
					{
						if ((int)m_thumbnailList.GetItemData(i) < pageNumber)
						{
							insertPos = i + 1;
						}
						else
						{
							break;
						}
					}
					itemIndex = m_thumbnailList.InsertItem(insertPos, strPageNum, imageIndex);
					// 设置项的数据为页面号，用于后续查找
					m_thumbnailList.SetItemData(itemIndex, (DWORD_PTR)pageNumber);
				}
				else
				{
					// 如果已存在，更新该项
					m_thumbnailList.SetItemText(itemIndex, 0, strPageNum);
					m_thumbnailList.SetItem(itemIndex, 0, LVIF_IMAGE, NULL, imageIndex, 0, 0, 0);
				}
			}

#ifdef _DEBUG
			TRACE(_T("添加缩略图到ImageList: 页面 %d, 索引 %d\n"), pageNumber, imageIndex);
#endif
		}
	}

	// 设置图标间距 - 水平间距设置为缩略图宽度，强制单列显示
	// 移除所有其他视图样式，设置为单列图标视图
	if (m_thumbnailList.GetSafeHwnd())
	{
		DWORD removeStyle = LVS_TYPEMASK | LVS_ALIGNMASK;
		DWORD addStyle = LVS_ICON | LVS_ALIGNTOP | LVS_NOCOLUMNHEADER;
		m_thumbnailList.ModifyStyle(removeStyle, addStyle);

		// 设置扩展样式 - 禁用水平滚动条
		DWORD exStyle = m_thumbnailList.GetExtendedStyle();
		m_thumbnailList.SetExtendedStyle(exStyle | LVS_EX_FULLROWSELECT | LVS_EX_AUTOSIZECOLUMNS);

		if (thumbHeight > 0)
		{
			// 增加垂直间距以使缩略图之间有更明显的分割
			m_thumbnailList.SetIconSpacing(CSize(thumbWidth, thumbHeight + 60));
		}
	}

	return true;
}

void CXiaoGongPDFDlg::UpdateThumbnails()
{
#ifdef _DEBUG
	TRACE(_T("UpdateThumbnails() 开始\n"));
	TRACE(_T("m_doc: %p, m_totalPages: %d\n"), m_doc, m_totalPages);
	TRACE(_T("缩略图缓存数量: %d\n"), (int)m_thumbnailCache.size());
#endif

	if (!m_doc || m_totalPages <= 0)
	{
#ifdef _DEBUG
		TRACE(_T("UpdateThumbnails() 跳过（无文档或无页面）\n"));
#endif
		return;
	}

	// ★★★ 清理列表控件和ImageList（缓存已从文档对象加载，但ImageList需要重建）
	if (m_thumbnailList.GetSafeHwnd())
	{
		// 销毁旧的ImageList句柄
		CImageList* pOldImageList = m_thumbnailList.GetImageList(LVSIL_NORMAL);
		if (pOldImageList)
		{
#ifdef _DEBUG
			TRACE(_T("销毁旧的ImageList句柄\n"));
#endif
			// 分离句柄并手动销毁
			HIMAGELIST hOld = pOldImageList->Detach();
			if (hOld)
			{
				::ImageList_Destroy(hOld);
			}
		}

		// 解除ImageList关联
		m_thumbnailList.SetImageList(nullptr, LVSIL_NORMAL);

		// 清空列表项
		m_thumbnailList.DeleteAllItems();

#ifdef _DEBUG
		TRACE(_T("已清理列表控件和ImageList\n"));
#endif
	}

	// 重新初始化缩略图列表
	InitializeThumbnailList();

	// ★★★ 优化：先只渲染前面部分缩略图，让界面快速响应
	// 对于页数多的PDF，避免阻塞UI线程
	const int INITIAL_RENDER_COUNT = 3;  // 先渲染前3页，让预览框更快显示
	int renderCount = min(INITIAL_RENDER_COUNT, m_totalPages);

#ifdef _DEBUG
	TRACE(_T("先渲染前 %d 页缩略图\n"), renderCount);
#endif

	// 渲染初始缩略图（会使用缓存，如果存在）
	for (int i = 0; i < renderCount; i++)
	{
		RenderThumbnail(i);
	}

	// 如果还有剩余页面，稍后异步渲染
	if (m_totalPages > renderCount)
	{
#ifdef _DEBUG
		TRACE(_T("剩余 %d 页将异步渲染\n"), m_totalPages - renderCount);
#endif
		// 使用PostMessage异步渲染剩余缩略图
		PostMessage(WM_USER + 100, renderCount, m_totalPages);
	}

#ifdef _DEBUG
	TRACE(_T("UpdateThumbnails() 完成，已渲染: %d/%d\n"), renderCount, m_totalPages);
#endif

	HighlightCurrentThumbnail();
}

// 异步渲染剩余缩略图
LRESULT CXiaoGongPDFDlg::OnRenderThumbnailsAsync(WPARAM wParam, LPARAM lParam)
{
	int startPage = (int)wParam;
	int endPage = (int)lParam;

#ifdef _DEBUG
	TRACE(_T("OnRenderThumbnailsAsync: 渲染 %d - %d 页\n"), startPage, endPage - 1);
#endif

	// 检查文档是否仍然有效
	if (!m_doc || m_totalPages <= 0)
	{
#ifdef _DEBUG
		TRACE(_T("文档已关闭，取消异步渲染\n"));
#endif
		return 0;
	}

	// 分批渲染，每次渲染5页，避免一次性阻塞太久
	const int BATCH_SIZE = 5;
	int batchEnd = min(startPage + BATCH_SIZE, endPage);

	for (int i = startPage; i < batchEnd; i++)
	{
		// 再次检查文档是否有效（可能在渲染过程中被关闭）
		if (!m_doc || i >= m_totalPages)
			break;

		RenderThumbnail(i);
	}

#ifdef _DEBUG
	TRACE(_T("已完成 %d - %d 页\n"), startPage, batchEnd - 1);
#endif

	// 如果还有剩余页面，继续异步渲染
	if (batchEnd < endPage && m_doc && batchEnd < m_totalPages)
	{
#ifdef _DEBUG
		TRACE(_T("继续渲染剩余 %d 页\n"), endPage - batchEnd);
#endif
		PostMessage(WM_USER + 100, batchEnd, endPage);
	}
	else
	{
#ifdef _DEBUG
		TRACE(_T("所有缩略图渲染完成\n"));
#endif
	}

	return 0;
}

// 延迟打开初始文件的消息处理函数
LRESULT CXiaoGongPDFDlg::OnOpenInitialFile(WPARAM wParam, LPARAM lParam)
{
	if (!m_initialFilePath.IsEmpty())
	{
		OpenPDFInNewTab(m_initialFilePath);
	}

	return 0;
}

void CXiaoGongPDFDlg::CleanupThumbnails()
{
#ifdef _DEBUG
	TRACE(_T("CleanupThumbnails() "));
#endif

	// 清理列表控件
	if (m_thumbnailList.GetSafeHwnd())
	{
		// ★★★ 重要：ImageList 通过 Detach 转移所有权后由控件自动管理
		// 只需要移除关联，不需要手动 delete
		m_thumbnailList.SetImageList(nullptr, LVSIL_NORMAL);

		// 清理列表项
		m_thumbnailList.DeleteAllItems();
	}

	// 清理缓存的缩略图
	for (auto& pair : m_thumbnailCache)
	{
		if (pair.second.hBitmap)
		{
			DeleteObject(pair.second.hBitmap);
			pair.second.hBitmap = nullptr;
		}
	}
	m_thumbnailCache.clear();
}

void CXiaoGongPDFDlg::HighlightCurrentThumbnail()
{
#ifdef _DEBUG
	TRACE(_T("HighlightCurrentThumbnail() - 当前页: %d\n"), m_currentPage);
#endif

	// 清除所有项的选中状态
	for (int i = 0; i < m_thumbnailList.GetItemCount(); i++)
	{
		m_thumbnailList.SetItemState(i, 0, LVIS_SELECTED);
	}

	// 查找对应当前页面的列表项索引
	if (m_currentPage >= 0 && m_currentPage < m_totalPages)
	{
		int itemIndex = -1;
		for (int i = 0; i < m_thumbnailList.GetItemCount(); i++)
		{
			if ((int)m_thumbnailList.GetItemData(i) == m_currentPage)
			{
				itemIndex = i;
				break;
			}
		}

		// 设置找到的项为选中状态
		if (itemIndex != -1)
		{
#ifdef _DEBUG
			TRACE(_T("找到页面 %d 对应的列表项: %d\n"), m_currentPage, itemIndex);
#endif
			m_thumbnailList.SetItemState(itemIndex, LVIS_SELECTED, LVIS_SELECTED);
			m_thumbnailList.EnsureVisible(itemIndex, FALSE);
		}
		else
		{
#ifdef _DEBUG
			TRACE(_T("未找到页面 %d 对应的列表项\n"), m_currentPage);
#endif
		}
	}
}

void CXiaoGongPDFDlg::OnThumbnailItemChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	LPNMLISTVIEW pNMLV = reinterpret_cast<LPNMLISTVIEW>(pNMHDR);

#ifdef _DEBUG
	TRACE(_T("OnThumbnailItemChanged() - iItem: %d, uChanged: %u, uNewState: %u, uOldState: %u, m_currentPage: %d\n"),
		pNMLV->iItem, pNMLV->uChanged, pNMLV->uNewState, pNMLV->uOldState, m_currentPage);
#endif

	// 只在项被选中时才切换页面（不处理取消选中的情况）
	// uNewState & LVIS_SELECTED 表示新状态是选中
	// !(uOldState & LVIS_SELECTED) 表示旧状态不是选中
	// 这样可以捕获从"未选中"到"选中"的状态变化
	if (pNMLV->iItem != -1 &&
		(pNMLV->uNewState & LVIS_SELECTED) &&
		!(pNMLV->uOldState & LVIS_SELECTED))
	{
		// 获取该列表项对应的实际页面号
		int pageNumber = (int)m_thumbnailList.GetItemData(pNMLV->iItem);

#ifdef _DEBUG
		TRACE(_T("列表项 %d 对应页面: %d\n"), pNMLV->iItem, pageNumber);
#endif

		// 只在页面号有效且与当前页不同时才切换
		if (pageNumber >= 0 && pageNumber < m_totalPages && pageNumber != m_currentPage)
		{
#ifdef _DEBUG
			TRACE(_T("切换到页面: %d (从页面 %d)\n"), pageNumber, m_currentPage);
#endif
			// ★★★ 使用统一的 GoToPage() 函数，支持分页和连续滚动两种模式
			GoToPage(pageNumber);
		}
		else
		{
#ifdef _DEBUG
			TRACE(_T("跳过切换，页面号=%d, 当前页=%d\n"), pageNumber, m_currentPage);
#endif
		}
	}
	else
	{
#ifdef _DEBUG
		TRACE(_T("不满足切换条件：iItem=%d, uNewState&LVIS_SELECTED=%d, uOldState&LVIS_SELECTED=%d\n"),
			pNMLV->iItem,
			(pNMLV->uNewState & LVIS_SELECTED) ? 1 : 0,
			(pNMLV->uOldState & LVIS_SELECTED) ? 1 : 0);
#endif
	}

	*pResult = 0;
}

void CXiaoGongPDFDlg::OnSize(UINT nType, int cx, int cy)
{
#ifdef _DEBUG
	TRACE(_T("OnSize() "));
#endif

	CDialogEx::OnSize(nType, cx, cy);

	// 全屏模式下不进行常规控件调整
	if (m_isFullscreen && m_pdfView.GetSafeHwnd())
	{
		// 在全屏模式下，只调整PDF预览控件大小为整个客户区
		CRect clientRect;
		GetClientRect(&clientRect);
		m_pdfView.MoveWindow(0, 0, clientRect.Width(), clientRect.Height());
		return;
	}

	// 检查控件是否有效，以及窗口是否为最小化状态
	if (m_thumbnailList.GetSafeHwnd() && m_pdfView.GetSafeHwnd() && m_toolbar.GetSafeHwnd() && nType != SIZE_MINIMIZED)
	{
		// 增加工具栏高度为40
		const int TOOLBAR_HEIGHT = 40;
		const int TAB_HEIGHT = 35;      // 标签页控件高度（增加到35以完整显示标签）
		const int BTN_HEIGHT = 28;      // 按钮高度

		// 工具栏
		m_toolbar.MoveWindow(MARGIN, MARGIN, cx - MARGIN * 2, TOOLBAR_HEIGHT);

		// 标签页控件（在工具栏下方）
		if (m_tabCtrl.GetSafeHwnd())
		{
			m_tabCtrl.MoveWindow(MARGIN, MARGIN * 2 + TOOLBAR_HEIGHT,
				cx - MARGIN * 2, TAB_HEIGHT);
		}

		// 定义各按钮的原始尺寸 - 这些是理想尺寸
		const int BTN_WIDTH_FIRST = 50;  // "首页"按钮宽度
		const int BTN_WIDTH_LAST = 50;   // "末页"按钮宽度
		const int BTN_WIDTH_FULLSCREEN = 50;  // "全屏"按钮宽度
		const int BTN_WIDTH_ROTATE = 35;  // "旋转"按钮宽度
		const int BTN_WIDTH_EDIT = 50;   // "编辑"按钮宽度
		const int EDIT_WIDTH = 40;       // 编辑框宽度
		const int CONTROL_SPACING = 8;  // 控件间距

		// 计算所有控件的总宽度（7个按钮 + 编辑框 + 7个间距）
		int controlsTotalWidth = BTN_WIDTH_FIRST + BTN_WIDTH_LAST + BTN_WIDTH_FULLSCREEN +
			EDIT_WIDTH + BTN_WIDTH_ROTATE * 2 + BTN_WIDTH_EDIT +
			CONTROL_SPACING * 7;
		
		// 计算工具栏的可用宽度
		int availableWidth = cx - MARGIN * 4;

		// 防止溢出 - 确保可用宽度是正数
		if (availableWidth <= 0)
			availableWidth = cx / 2; // 半个窗口宽度

		// 从左边界开始，留一点边距
		int startX = MARGIN * 2;

		// 计算缩放因子 - 自适应宽度
		float scaleFactor = 1.0f;
		if (availableWidth < controlsTotalWidth)
		{
			scaleFactor = (float)availableWidth / controlsTotalWidth;

			// 限制最小缩放因子防止控件太小
			const float MIN_SCALE = 0.6f;
			if (scaleFactor < MIN_SCALE)
				scaleFactor = MIN_SCALE;
		}
		
		// 定义最小控件尺寸
		const int MIN_BTN_WIDTH = 40;
		const int MIN_EDIT_WIDTH = 30;
		const int MIN_SPACING = 4;

		// 根据缩放因子计算实际宽度，并确保不小于最小尺寸
		int widthFirst = max(MIN_BTN_WIDTH, (int)(BTN_WIDTH_FIRST * scaleFactor));
		int widthLast = max(MIN_BTN_WIDTH, (int)(BTN_WIDTH_LAST * scaleFactor));
		int widthFullscreen = max(MIN_BTN_WIDTH, (int)(BTN_WIDTH_FULLSCREEN * scaleFactor));
		int widthRotateLeft = max(MIN_BTN_WIDTH, (int)(BTN_WIDTH_ROTATE * scaleFactor));
		int widthRotateRight = max(MIN_BTN_WIDTH, (int)(BTN_WIDTH_ROTATE * scaleFactor));
		int widthEditBtn = max(MIN_BTN_WIDTH, (int)(BTN_WIDTH_EDIT * scaleFactor));
		int widthEdit = max(MIN_EDIT_WIDTH, (int)(EDIT_WIDTH * scaleFactor));
		int spacing = max(MIN_SPACING, (int)(CONTROL_SPACING * scaleFactor));

		// 检查窗口总宽度，在极窄状态下，确保工具栏按钮间距不会太大
		int actualTotalWidth = widthFirst + widthLast + widthFullscreen + widthEdit +
			widthRotateLeft + widthRotateRight + widthEditBtn +
			spacing * 7;

		// 如果总宽度超过可用空间，重新调整
		if (actualTotalWidth > availableWidth)
		{
			// 优先压缩间距
			spacing = MIN_SPACING;
			actualTotalWidth = widthFirst + widthLast + widthFullscreen + widthEdit +
				widthRotateLeft + widthRotateRight + widthEditBtn +
				spacing * 7;

			// 如果仍然太宽，则等比压缩所有控件
			if (actualTotalWidth > availableWidth)
			{
				float adjustScale = (float)availableWidth / actualTotalWidth;
				widthFirst = max(MIN_BTN_WIDTH, (int)(widthFirst * adjustScale));
				widthLast = max(MIN_BTN_WIDTH, (int)(widthLast * adjustScale));
				widthFullscreen = max(MIN_BTN_WIDTH, (int)(widthFullscreen * adjustScale));
				widthRotateLeft = max(MIN_BTN_WIDTH, (int)(widthRotateLeft * adjustScale));
				widthRotateRight = max(MIN_BTN_WIDTH, (int)(widthRotateRight * adjustScale));
				widthEditBtn = max(MIN_BTN_WIDTH, (int)(widthEditBtn * adjustScale));
				widthEdit = max(MIN_EDIT_WIDTH, (int)(widthEdit * adjustScale));
			}
		}

		// 垂直居中位置
		int btnY = MARGIN + (TOOLBAR_HEIGHT - BTN_HEIGHT) / 2;

		// 更新控件位置和大小 - 新顺序：首页、末页、全屏、当前页码框、左旋、右旋
		int x = startX;
		m_btnFirst.MoveWindow(x, btnY, widthFirst, BTN_HEIGHT);

		x += widthFirst + spacing;
		m_btnLast.MoveWindow(x, btnY, widthLast, BTN_HEIGHT);

		x += widthLast + spacing;
		m_btnFullscreen.MoveWindow(x, btnY, widthFullscreen, BTN_HEIGHT);

		x += widthFullscreen + spacing;
		m_editCurrent.MoveWindow(x, btnY, widthEdit, BTN_HEIGHT);

		x += widthEdit + spacing;
		m_btnRotateLeft.MoveWindow(x, btnY, widthRotateLeft, BTN_HEIGHT);

		x += widthRotateLeft + spacing;
		m_btnRotateRight.MoveWindow(x, btnY, widthRotateRight, BTN_HEIGHT);

		// 缩略图复选框
		x += widthRotateRight + spacing;
		const int CHECK_WIDTH = 100;  // 复选框宽度
		m_checkThumbnail.MoveWindow(x, btnY, CHECK_WIDTH, BTN_HEIGHT);

		// 搜索控件（紧跟在缩略图复选框后面）
		x += CHECK_WIDTH + spacing;
		const int SEARCH_EDIT_WIDTH = 150;  // 搜索框宽度
		m_editSearch.MoveWindow(x, btnY, SEARCH_EDIT_WIDTH, BTN_HEIGHT);

		x += SEARCH_EDIT_WIDTH + spacing;
		const int BTN_FIND_WIDTH = 50;  // 查找按钮宽度
		m_btnFind.MoveWindow(x, btnY, BTN_FIND_WIDTH, BTN_HEIGHT);

		x += BTN_FIND_WIDTH + spacing;
		const int BTN_NAV_WIDTH = 80;  // 导航按钮宽度
		m_btnPrevMatch.MoveWindow(x, btnY, BTN_NAV_WIDTH, BTN_HEIGHT);

		x += BTN_NAV_WIDTH + spacing;
		m_btnNextMatch.MoveWindow(x, btnY, BTN_NAV_WIDTH, BTN_HEIGHT);

		// 编辑按钮（在下一个匹配按钮后面，工具栏最右侧）
		x += BTN_NAV_WIDTH + spacing;
		m_btnEdit.MoveWindow(x, btnY, widthEditBtn, BTN_HEIGHT);

		// 根据缩略图面板可见性调整布局
		int pdfX, pdfWidth;
		const int STATUS_BAR_HEIGHT = 25;  // 状态栏高度

		// 计算内容区域的起始Y坐标（工具栏 + 标签页 + 间距）
		// 标签页顶部: MARGIN * 2 + TOOLBAR_HEIGHT = 14 + 40 = 54
		// 标签页底部: MARGIN * 2 + TOOLBAR_HEIGHT + TAB_HEIGHT = 54 + 35 = 89
		// 内容区域顶部: 标签页底部 + MARGIN * 2（增加更多间距避免遮挡）
		int contentY = MARGIN * 4 + TOOLBAR_HEIGHT + TAB_HEIGHT;  // 增加间距
		int contentHeight = cy - contentY - MARGIN - STATUS_BAR_HEIGHT;

		if (m_thumbnailVisible)
		{
			// 缩略图可见时，显示在左侧
			m_thumbnailList.MoveWindow(MARGIN, contentY,
				LIST_WIDTH + m_scrollBarWidth, contentHeight);
			// 增加垂直间距以使缩略图之间有更明显的分割
			m_thumbnailList.SetIconSpacing(CSize(m_thumbnailPicWidth, m_thumbnailPicHeight + 60));

			// PDF 预览占右侧剩余空间
			pdfX = MARGIN * 2 + LIST_WIDTH + m_scrollBarWidth;
			pdfWidth = cx - pdfX - MARGIN;
		}
		else
		{
			// 缩略图隐藏时，PDF 预览占满整个宽度
			pdfX = MARGIN;
			pdfWidth = cx - MARGIN * 2;
		}

		// PDF视图，为状态栏留出空间
		m_pdfView.MoveWindow(pdfX, contentY, pdfWidth, contentHeight);

		// ★★★ MoveWindow 会重置控件状态，需要立即根据模式设置滚动条
		if (m_continuousScrollMode)
		{
			// 连续滚动模式：显示滚动条
			m_pdfView.ShowScrollBar(SB_VERT, TRUE);
			m_pdfView.EnableScrollBarCtrl(SB_VERT, TRUE);
		}
		else
		{
			// 分页模式：隐藏滚动条
			m_pdfView.ShowScrollBar(SB_VERT, FALSE);
		}

		// 状态栏在最底部
		m_statusBar.MoveWindow(MARGIN, cy - MARGIN - STATUS_BAR_HEIGHT,
			cx - MARGIN * 2, STATUS_BAR_HEIGHT);

		// 更新工具栏控件状态 - 仅在文档已加载时更新
		if (m_doc)
		{
			// 更新页码编辑框的显示
			CString currentPage;
			currentPage.Format(_T("%d"), m_currentPage + 1);
			m_editCurrent.SetWindowText(currentPage);

			// 更新状态栏
			UpdateStatusBar();

			// 立即重绘按钮和状态栏，确保状态变化可见
			m_btnFirst.Invalidate();
			m_btnLast.Invalidate();
			m_btnFullscreen.Invalidate();
			m_btnRotateLeft.Invalidate();
			m_btnRotateRight.Invalidate();
			m_editCurrent.Invalidate();
		}

		// 无论是否有文档，都需要重绘状态栏（修复未打开PDF时横向伸缩显示异常）
		m_statusBar.Invalidate();
		m_statusBar.UpdateWindow();

		// 如果有文档加载，重新渲染当前页
		if (m_doc && m_currentPage >= 0)
		{
			if (m_continuousScrollMode)
			{
				// ★★★ 连续滚动模式：重新计算页面位置并渲染
				CalculatePagePositions();

				// ★★★ 确保滚动条可见
				m_pdfView.ShowScrollBar(SB_VERT, TRUE);
				m_pdfView.EnableScrollBarCtrl(SB_VERT, TRUE);

				UpdateScrollBar();
				RenderVisiblePages();
			}
			else
			{
				// ★★★ 分页模式：重新计算统一缩放比例并渲染当前页

				// ★★★ 分页模式下隐藏滚动条
				m_pdfView.ShowScrollBar(SB_VERT, FALSE);

				if (m_documentMaxPageWidth > 0)
				{
					CRect viewRect;
					m_pdfView.GetClientRect(&viewRect);
					m_documentUniformScale = (float)viewRect.Width() / m_documentMaxPageWidth * 0.95f;
#ifdef _DEBUG
					TRACE(_T("OnSize: 重新计算统一缩放比例: %.4f\n"), m_documentUniformScale);
#endif
				}

				// ★★★ 窗口大小改变时，重置平移偏移量
				ResetPanOffset();

				CleanupBitmap();
				RenderPage(m_currentPage);
			}
		}
	}
}

BOOL CXiaoGongPDFDlg::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
#ifdef _DEBUG
	TRACE(_T("OnMouseWheel() "));
#endif

	// 检查文档是否加载
	if (!m_doc || m_totalPages <= 0)
		return CDialogEx::OnMouseWheel(nFlags, zDelta, pt);

	// 使用静态变量防止重入
	static bool isRendering = false;
	if (isRendering)
		return TRUE;

	// 将屏幕坐标转换为客户区坐标
	CPoint clientPt = pt;
	m_pdfView.ScreenToClient(&clientPt);

	// 检查鼠标是否在预览区域内
	CRect rcPreview;
	m_pdfView.GetClientRect(&rcPreview);
	if (rcPreview.PtInRect(clientPt))
	{
		// 检查是否按下Ctrl键进行缩放
		if (nFlags & MK_CONTROL)
		{
			try
			{
				isRendering = true;  // 标记开始渲染

				// 计算新的缩放级别
				float zoomChange = (zDelta > 0) ? 1.1f : 0.9f;
				float newZoom = m_customZoom * zoomChange;

				// 限制缩放范围 0.25x ~ 4.0x
				if (newZoom < 0.25f) newZoom = 0.25f;
				if (newZoom > 4.0f) newZoom = 4.0f;

				// ★★★ 只有当缩放值真正改变时才调用 SetZoom
				if (fabs(newZoom - m_customZoom) > 0.001f)
				{
					SetZoom(newZoom, ZOOM_CUSTOM);
				}

				isRendering = false;  // 标记渲染完成
				return TRUE;
			}
			catch (...)
			{
				isRendering = false;  // 确保发生异常时也重置标记
				MessageBox(_T("缩放页面时发生错误"), _T("错误"), MB_OK | MB_ICONERROR);
				return FALSE;
			}
		}
		else
		{
			// ★★★ 根据模式处理滚轮
			if (m_continuousScrollMode)
			{
				// 连续滚动模式：滚动页面
				CRect viewRect;
				m_pdfView.GetClientRect(&viewRect);

				int scrollAmount = (zDelta > 0) ? -120 : 120;  // 向上滚动为负，向下滚动为正
				int newPos = m_scrollPosition + scrollAmount;

				// 限制范围
				int maxScroll = m_totalScrollHeight - viewRect.Height();
				if (maxScroll < 0) maxScroll = 0;
				if (newPos < 0) newPos = 0;
				if (newPos > maxScroll) newPos = maxScroll;

				if (newPos != m_scrollPosition)
				{
					m_scrollPosition = newPos;
					UpdateScrollBar();
					RenderVisiblePages();
				}

				return TRUE;
			}
			else
			{
				// 分页模式：翻页
				try
				{
					isRendering = true;  // 标记开始渲染

					// 向上滚动，显示上一页
					if (zDelta > 0 && m_currentPage > 0)
					{
						CleanupBitmap();
						RenderPage(m_currentPage - 1);
					}
					// 向下滚动，显示下一页
					else if (zDelta < 0 && m_currentPage < m_totalPages - 1)
					{
						CleanupBitmap();
						RenderPage(m_currentPage + 1);
					}

					isRendering = false;  // 标记渲染完成
					return TRUE;
				}
				catch (...)
				{
					isRendering = false;  // 确保发生异常时也重置标记
					MessageBox(_T("渲染页面时发生错误"), _T("错误"), MB_OK | MB_ICONERROR);
					return FALSE;
				}
			}
		}
	}

	isRendering = false;  // 确保最后重置标记
	return CDialogEx::OnMouseWheel(nFlags, zDelta, pt);
}

// 工具栏功能实现
void CXiaoGongPDFDlg::UpdatePageControls()
{
	if (!m_doc) return;

	// 更新当前页码显示
	CString currentPage;
	currentPage.Format(_T("%d"), m_currentPage + 1);
	m_editCurrent.SetWindowText(currentPage);
	m_editCurrent.UpdateWindow();  // 强制立即重绘，确保快速滚动时也能实时显示

	// 更新状态栏
	UpdateStatusBar();

	// 立即重绘按钮，确保状态变化可见
	m_btnFirst.Invalidate();
	m_btnLast.Invalidate();
	m_btnFullscreen.Invalidate();
}

void CXiaoGongPDFDlg::UpdateStatusBar()
{
	if (!m_doc)
	{
		m_statusBar.SetWindowText(_T("页码: 0 / 0"));
		return;
	}

	CString title;
	title.Format(_T("第 %d/%d 页"),
		m_currentPage + 1, m_totalPages);
	m_statusBar.SetWindowText(title);

}

void CXiaoGongPDFDlg::OnBtnFirst()
{
	if (!m_doc || m_totalPages <= 0)
		return;

	GoToPage(0);
}

void CXiaoGongPDFDlg::OnBtnLast()
{
	if (!m_doc || m_totalPages <= 0)
		return;

	GoToPage(m_totalPages - 1);
}

void CXiaoGongPDFDlg::OnBtnFullscreen()
{
	// 切换全屏状态
	if (!m_isFullscreen)
		EnterFullscreen();
	else
		ExitFullscreen();
}

// ============ 旋转功能实现 ============

void CXiaoGongPDFDlg::OnBtnRotateLeft()
{
#ifdef _DEBUG
	TRACE(_T("OnBtnRotateLeft()\n"));
#endif

	if (!m_doc || m_totalPages <= 0)
		return;

	// 向左旋转（逆时针90度）
	RotatePage(-90);
}

void CXiaoGongPDFDlg::OnBtnRotateRight()
{
#ifdef _DEBUG
	TRACE(_T("OnBtnRotateRight()\n"));
#endif

	if (!m_doc || m_totalPages <= 0)
		return;

	// 向右旋转（顺时针90度）
	RotatePage(90);
}

// 编辑按钮点击事件
void CXiaoGongPDFDlg::OnBtnEdit()
{
#ifdef _DEBUG
	TRACE(_T("OnBtnEdit()\n"));
#endif

	// 检查是否有打开的文档
	if (!m_doc || m_totalPages <= 0)
	{
		MessageBox(_T("请先打开PDF文件"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	// 获取当前活动文档
	CPDFDocument* pDoc = GetActiveDocument();
	if (!pDoc)
	{
		MessageBox(_T("无法获取当前文档"), _T("错误"), MB_OK | MB_ICONERROR);
		return;
	}

	// 创建编辑对话框
	CPDFEditDialog editDlg(m_ctx, m_doc, pDoc->GetFilePath(), this);

	// 显示对话框
	if (editDlg.DoModal() == IDOK)
	{
		// 编辑成功，获取保存的文件路径
		CString newFilePath = editDlg.GetSavedFilePath();
		if (!newFilePath.IsEmpty())
		{
			// 在新标签页中打开编辑后的文件
			OpenPDFInNewTab(newFilePath);

			// 提示用户
			CString msg;
			msg.Format(_T("PDF已保存为：\n%s\n\n已在新标签页中打开编辑后的文件。"), newFilePath);
			MessageBox(msg, _T("编辑成功"), MB_OK | MB_ICONINFORMATION);
		}
	}
}

// 缩略图复选框点击事件
void CXiaoGongPDFDlg::OnCheckThumbnail()
{
#ifdef _DEBUG
	TRACE(_T("OnCheckThumbnail()\n"));
#endif

	// 获取复选框状态
	BOOL isChecked = m_checkThumbnail.GetCheck();

	// 根据复选框状态设置缩略图面板的可见性
	m_thumbnailVisible = (isChecked == BST_CHECKED);

	// 显示/隐藏缩略图列表
	m_thumbnailList.ShowWindow(m_thumbnailVisible ? SW_SHOW : SW_HIDE);

	// 触发窗口重新布局
	CRect rect;
	GetClientRect(&rect);
	OnSize(SIZE_RESTORED, rect.Width(), rect.Height());

	// 如果有文档加载，根据当前模式重新渲染
	if (m_doc && m_currentPage >= 0)
	{
		if (m_continuousScrollMode)
		{
			RenderVisiblePages();
		}
		else
		{
			RenderPage(m_currentPage);
		}
	}
}

void CXiaoGongPDFDlg::RotatePage(int degrees)
{
#ifdef _DEBUG
	TRACE(_T("RotatePage() : degrees=%d\n"), degrees);
#endif

	if (!m_doc || m_totalPages <= 0 || m_currentPage < 0)
		return;

	// 获取当前页面的旋转角度
	int currentRotation = GetPageRotation(m_currentPage);

	// 计算新的旋转角度（保持在0-270范围内）
	int newRotation = (currentRotation + degrees + 360) % 360;

	// 更新旋转角度存储
	m_pageRotations[m_currentPage] = newRotation;

	// ★★★ 清除当前页的所有缓存（因为旋转角度变了）
	// 遍历所有缓存，删除与当前页相关的所有条目（不同尺寸的缓存）
	for (auto it = m_pageCache.begin(); it != m_pageCache.end(); )
	{
		if (it->first.pageNumber == m_currentPage)
		{
			// 删除位图
			if (it->second.hBitmap)
				DeleteObject(it->second.hBitmap);

			// 从LRU列表中移除
			for (auto listIt = m_cacheOrder.begin(); listIt != m_cacheOrder.end(); ++listIt)
			{
				if (*listIt == it->first)
				{
					m_cacheOrder.erase(listIt);
					break;
				}
			}

#ifdef _DEBUG
			TRACE(_T("清除页面 %d 的缓存: %dx%d\n"),
				m_currentPage, it->first.width, it->first.height);
#endif

			// 删除缓存条目
			it = m_pageCache.erase(it);
		}
		else
		{
			++it;
		}
	}

	// 清除当前页的缩略图缓存（因为旋转角度变了）
	auto thumbIt = m_thumbnailCache.find(m_currentPage);
	if (thumbIt != m_thumbnailCache.end())
	{
		if (thumbIt->second.hBitmap)
		{
			DeleteObject(thumbIt->second.hBitmap);
#ifdef _DEBUG
			TRACE(_T("清除旋转页面的缩略图缓存: 页面 %d\n"), m_currentPage);
#endif
		}
		m_thumbnailCache.erase(thumbIt);
	}

	// ★★★ 旋转后需要重置平移偏移量（因为页面尺寸改变了）
	// 注意：由于旋转是在同一页面上操作（pageNumber == m_currentPage），
	// RenderPage() 不会调用 RestorePageZoomState()，所以需要手动重置
	m_panOffset.x = 0;
	m_panOffset.y = 0;

#ifdef _DEBUG
	TRACE(_T("旋转前重置平移偏移量: (%d, %d)\n"), m_panOffset.x, m_panOffset.y);
#endif

	// ★★★ 根据当前模式选择正确的渲染方式
	if (m_continuousScrollMode)
	{
		// 连续滚动模式：重新计算页面位置并渲染所有可见页面
		CalculatePagePositions();
		UpdateScrollBar();
		RenderVisiblePages();
	}
	else
	{
		// 分页模式：只渲染当前页
		CleanupBitmap();
		RenderPage(m_currentPage);
	}

	// ★★★ 渲染后确保平移偏移量保持为 (0, 0)

	// 清理拖拽缓存位图
	if (m_hPanPageBitmap)
	{
		DeleteObject(m_hPanPageBitmap);
		m_hPanPageBitmap = NULL;
	}

	// 保存当前状态（平移偏移量已重置）
	SaveCurrentPageZoomState();

	// 重新渲染当前页的缩略图
	if (RenderThumbnail(m_currentPage))
	{
		// 刷新缩略图列表显示
		m_thumbnailList.Invalidate();
		HighlightCurrentThumbnail();
#ifdef _DEBUG
		TRACE(_T("旋转后刷新缩略图显示: 页面 %d\n"), m_currentPage);
#endif
	}
}

int CXiaoGongPDFDlg::GetPageRotation(int pageNumber)
{
	auto it = m_pageRotations.find(pageNumber);
	if (it != m_pageRotations.end())
		return it->second;
	return 0;  // 默认无旋转
}

HBITMAP CXiaoGongPDFDlg::RotateBitmap(HBITMAP hSrcBitmap, int rotation)
{
	if (!hSrcBitmap || rotation == 0)
		return NULL;

	// 只支持90度的倍数
	if (rotation % 90 != 0)
		return NULL;

	// 标准化旋转角度到0-270
	rotation = (rotation + 360) % 360;
	if (rotation == 0)
		return NULL;

	// 获取源位图信息
	BITMAP bm;
	if (!GetObject(hSrcBitmap, sizeof(BITMAP), &bm))
		return NULL;

	// 创建内存DC
	CDC dcSrc, dcDest;
	if (!dcSrc.CreateCompatibleDC(NULL) || !dcDest.CreateCompatibleDC(NULL))
		return NULL;

	// 选择源位图
	HBITMAP hOldSrc = (HBITMAP)dcSrc.SelectObject(hSrcBitmap);
	if (!hOldSrc)
	{
		dcSrc.DeleteDC();
		dcDest.DeleteDC();
		return NULL;
	}

	// 计算旋转后的尺寸
	int newWidth, newHeight;
	if (rotation == 90 || rotation == 270)
	{
		newWidth = bm.bmHeight;
		newHeight = bm.bmWidth;
	}
	else  // 180度
	{
		newWidth = bm.bmWidth;
		newHeight = bm.bmHeight;
	}

	// 创建目标位图
	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = newWidth;
	bmi.bmiHeader.biHeight = -newHeight;  // 自上而下
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 24;
	bmi.bmiHeader.biCompression = BI_RGB;

	BYTE* pBits = NULL;
	HBITMAP hDestBitmap = CreateDIBSection(dcDest.GetSafeHdc(), &bmi, DIB_RGB_COLORS, (void**)&pBits, NULL, 0);
	if (!hDestBitmap)
	{
		dcSrc.SelectObject(hOldSrc);
		dcSrc.DeleteDC();
		dcDest.DeleteDC();
		return NULL;
	}

	HBITMAP hOldDest = (HBITMAP)dcDest.SelectObject(hDestBitmap);
	if (!hOldDest)
	{
		DeleteObject(hDestBitmap);
		dcSrc.SelectObject(hOldSrc);
		dcSrc.DeleteDC();
		dcDest.DeleteDC();
		return NULL;
	}

	// 设置背景为白色
	dcDest.FillSolidRect(0, 0, newWidth, newHeight, RGB(255, 255, 255));

	// 设置图形模式
	int oldMode = dcDest.SetGraphicsMode(GM_ADVANCED);

	// 设置旋转变换矩阵
	XFORM xform = { 0 };
	float radian = rotation * 3.14159265f / 180.0f;
	xform.eM11 = cos(radian);
	xform.eM12 = sin(radian);
	xform.eM21 = -sin(radian);
	xform.eM22 = cos(radian);

	// 设置平移量（确保图像在可见区域内）
	switch (rotation)
	{
	case 90:
		xform.eDx = (float)newWidth;
		xform.eDy = 0.0f;
		break;
	case 180:
		xform.eDx = (float)newWidth;
		xform.eDy = (float)newHeight;
		break;
	case 270:
		xform.eDx = 0.0f;
		xform.eDy = (float)newHeight;
		break;
	}

	dcDest.SetWorldTransform(&xform);

	// 执行位图拷贝（带变换）
	BOOL bResult = dcDest.BitBlt(0, 0, bm.bmWidth, bm.bmHeight, &dcSrc, 0, 0, SRCCOPY);

	// 恢复图形模式
	dcDest.SetGraphicsMode(oldMode);

	// 清理
	dcDest.SelectObject(hOldDest);
	dcSrc.SelectObject(hOldSrc);
	dcSrc.DeleteDC();
	dcDest.DeleteDC();

	if (!bResult)
	{
		DeleteObject(hDestBitmap);
		return NULL;
	}

	return hDestBitmap;
}

void CXiaoGongPDFDlg::OnEnChangeEditCurrent()
{
	// 实时验证在 OnEditCurrentKillFocus 中处理
	// 这里保持空实现，避免输入时频繁触发
}

void CXiaoGongPDFDlg::OnEnChangeEditSearch()
{
	// 获取搜索框当前文本
	CString searchText;
	m_editSearch.GetWindowText(searchText);
	searchText.Trim();

	// 如果搜索框为空，清除搜索结果和标记
	if (searchText.IsEmpty())
	{
		// 清除搜索结果
		m_searchMatches.clear();
		m_currentMatchIndex = -1;

		// 清除状态栏显示
		m_statusBar.SetWindowText(_T(""));

		// 重新渲染页面以移除高亮标记
		if (m_continuousScrollMode)
		{
			RenderVisiblePages();
		}
		else
		{
			RenderPage(m_currentPage);
		}
	}
}

void CXiaoGongPDFDlg::OnEditCurrentKillFocus()
{
	if (!m_doc || m_totalPages <= 0)
		return;

	// 获取当前输入的页码
	CString strPageNum;
	m_editCurrent.GetWindowText(strPageNum);

	// 如果为空或不合法，恢复为当前页码
	if (strPageNum.IsEmpty())
	{
		CString currentPage;
		currentPage.Format(_T("%d"), m_currentPage + 1);
		m_editCurrent.SetWindowText(currentPage);
		return;
	}

	// 转换为整数
	int pageNum = _ttoi(strPageNum);

	// 检查页码范围 (1到m_totalPages)
	if (pageNum < 1 || pageNum > m_totalPages)
	{
		// 页码无效，恢复为当前页码
		CString currentPage;
		currentPage.Format(_T("%d"), m_currentPage + 1);
		m_editCurrent.SetWindowText(currentPage);

		// 提示用户
		CString msg;
		msg.Format(_T("页码必须在 1 到 %d 之间"), m_totalPages);
		MessageBox(msg, _T("提示"), MB_OK | MB_ICONINFORMATION);
	}
	else
	{
		// 有效页码，跳转到该页（即使是当前页也跳转，确保连续滚动模式下滚动到正确位置）
		GoToPage(pageNum - 1);
	}
}

void CXiaoGongPDFDlg::InitializeToolbar()
{
	// 修改工具栏样式，使其更加明显
	m_toolbar.ModifyStyle(SS_BLACKFRAME, SS_WHITEFRAME);

	// 创建字体
	LOGFONT lf;
	memset(&lf, 0, sizeof(LOGFONT));
	lf.lfHeight = 20;         // 字体高度
	lf.lfWeight = FW_BOLD;    // 粗体
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = DEFAULT_QUALITY;
	lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
	_tcscpy_s(lf.lfFaceName, _T("微软雅黑"));  // 使用微软雅黑字体

	// 确保先删除可能存在的字体对象
	m_buttonFont.DeleteObject();
	m_labelFont.DeleteObject();

	// 创建新的字体对象
	m_buttonFont.CreateFontIndirect(&lf);
	
	// 为所有按钮设置新字体
	m_btnFirst.SetFont(&m_buttonFont);
	m_btnLast.SetFont(&m_buttonFont);
	m_btnFullscreen.SetFont(&m_buttonFont);
	m_btnRotateLeft.SetFont(&m_buttonFont);
	m_btnRotateRight.SetFont(&m_buttonFont);
	m_btnEdit.SetFont(&m_buttonFont);

	// 设置编辑框的字体和外观
	m_editCurrent.SetFont(&m_buttonFont);

	// 设置按钮文本
	m_btnFirst.SetWindowText(_T("首页"));
	m_btnLast.SetWindowText(_T("末页"));
	m_btnFullscreen.SetWindowText(_T("全屏"));
	m_btnRotateLeft.SetWindowText(_T("左旋"));
	m_btnRotateRight.SetWindowText(_T("右旋"));
	m_btnEdit.SetWindowText(_T("编辑"));
	m_btnPrevMatch.SetWindowText(_T("上一个"));
	m_btnNextMatch.SetWindowText(_T("下一个"));

	// 设置初始文本
	m_editCurrent.SetWindowText(_T("0"));

	// 确保所有按钮默认启用
	m_btnFirst.EnableWindow(TRUE);
	m_btnLast.EnableWindow(TRUE);
	m_btnFullscreen.EnableWindow(TRUE);
	m_btnRotateLeft.EnableWindow(TRUE);
	m_btnRotateRight.EnableWindow(TRUE);
	m_btnEdit.EnableWindow(TRUE);
	m_editCurrent.EnableWindow(TRUE);
}

void CXiaoGongPDFDlg::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
{
	// 如果没有加载文档，直接返回到默认处理
	if (!m_doc || m_totalPages <= 0)
	{
		CDialogEx::OnKeyDown(nChar, nRepCnt, nFlags);
		return;
	}

	// 根据按键处理不同的操作
	switch (nChar)
	{
	case VK_LEFT:  // 左方向键 - 上一页
		if (m_currentPage > 0)
			GoToPage(m_currentPage - 1);
		break;

	case VK_RIGHT:  // 右方向键 - 下一页
		if (m_currentPage < m_totalPages - 1)
			GoToPage(m_currentPage + 1);
		break;

	case VK_PRIOR:  // Page Up键 - 上一页
		if (m_currentPage > 0)
			GoToPage(m_currentPage - 1);
		break;

	case VK_NEXT:  // Page Down键 - 下一页
		if (m_currentPage < m_totalPages - 1)
			GoToPage(m_currentPage + 1);
		break;

	case VK_HOME:  // Home键 - 首页
		if (m_currentPage != 0)
			GoToPage(0);
		break;

	case VK_END:  // End键 - 末页
		if (m_currentPage != m_totalPages - 1)
			GoToPage(m_totalPages - 1);
		break;

	default:
		CDialogEx::OnKeyDown(nChar, nRepCnt, nFlags);
		break;
	}
}

BOOL CXiaoGongPDFDlg::PreTranslateMessage(MSG* pMsg)
{
	// 如果是键盘消息
	if (pMsg->message == WM_KEYDOWN)
	{
		// 处理ESC键 - 在全屏模式下退出全屏，否则忽略它(阻止关闭对话框)
		if (pMsg->wParam == VK_ESCAPE)
		{
			if (m_isFullscreen)
				ExitFullscreen();
			return TRUE; // 消息已处理，阻止默认行为
		}

		// 处理Ctrl + O - 文件打开快捷键
		if ((GetKeyState(VK_CONTROL) & 0x8000) && pMsg->wParam == 'O') {
			onMenuOpen();
			return TRUE;
		}

		// 处理Ctrl + P - 打印快捷键
		if ((GetKeyState(VK_CONTROL) & 0x8000) && pMsg->wParam == 'P') {
			onMenuPrint();
			return TRUE;
		}

		// 处理F9 - 切换缩略图面板
		if (pMsg->wParam == VK_F9)
		{
			ToggleThumbnailPanel();
			return TRUE;
		}

		// 处理Ctrl + F - 聚焦到搜索框
		if ((GetKeyState(VK_CONTROL) & 0x8000) && pMsg->wParam == 'F')
		{
			m_editSearch.SetFocus();
			m_editSearch.SetSel(0, -1);  // 全选文本
			return TRUE;
		}

		// 处理F3 - 查找下一个
		if (pMsg->wParam == VK_F3)
		{
			if (GetKeyState(VK_SHIFT) & 0x8000)
			{
				// Shift+F3 - 查找上一个
				GoToPrevMatch();
			}
			else
			{
				// F3 - 查找下一个
				GoToNextMatch();
			}
			return TRUE;
		}

		// 处理F11 - 全屏快捷键
		if (pMsg->wParam == VK_F11) {
			OnBtnFullscreen();
			return TRUE;
		}

		// 处理Ctrl+L - 向左旋转
		if ((GetKeyState(VK_CONTROL) & 0x8000) && pMsg->wParam == 'L')
		{
			OnBtnRotateLeft();
			return TRUE;
		}

		// 处理Ctrl+R - 向右旋转
		if ((GetKeyState(VK_CONTROL) & 0x8000) && pMsg->wParam == 'R')
		{
			OnBtnRotateRight();
			return TRUE;
		}

		// 处理Enter键 - 在页码编辑框或搜索框中按Enter键
		if (pMsg->wParam == VK_RETURN)
		{
			// 检查焦点是否在页码编辑框上
			CWnd* pFocusWnd = GetFocus();
			if (pFocusWnd && pFocusWnd->GetSafeHwnd() == m_editCurrent.GetSafeHwnd())
			{
				// 获取输入的页码
				CString strPageNum;
				m_editCurrent.GetWindowText(strPageNum);

				if (!strPageNum.IsEmpty())
				{
					int pageNum = _ttoi(strPageNum);

					// 验证页码范围
					if (pageNum >= 1 && pageNum <= m_totalPages)
					{
						// 跳转到目标页面
						GoToPage(pageNum - 1);
					}
					else
					{
						// 页码无效，恢复为当前页码
						CString currentPage;
						currentPage.Format(_T("%d"), m_currentPage + 1);
						m_editCurrent.SetWindowText(currentPage);

						// 提示用户
						CString msg;
						msg.Format(_T("页码必须在 1 到 %d 之间"), m_totalPages);
						MessageBox(msg, _T("提示"), MB_OK | MB_ICONINFORMATION);
					}
				}

				return TRUE;
			}
			// 检查焦点是否在搜索框上
			else if (pFocusWnd && pFocusWnd->GetSafeHwnd() == m_editSearch.GetSafeHwnd())
			{
				// 执行搜索
				OnBtnFind();
				return TRUE;
			}
		}

		// ★★★ 处理导航键（左右箭头、Page Up/Down、Home/End）- 翻页功能
		// 无论焦点在哪里都执行翻页，不需要特殊处理
		if (pMsg->wParam == VK_LEFT || pMsg->wParam == VK_RIGHT ||
			pMsg->wParam == VK_PRIOR || pMsg->wParam == VK_NEXT ||
			pMsg->wParam == VK_HOME || pMsg->wParam == VK_END)
		{
			OnKeyDown((UINT)pMsg->wParam, 1, 0);
			return TRUE;
		}
	}

	return CDialogEx::PreTranslateMessage(pMsg);
}

void CXiaoGongPDFDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	// 设置窗口的最小跟踪尺寸（这是用户通过拖动窗口边缘能够调整的最小大小）
	lpMMI->ptMinTrackSize.x = APP_MINWIDTH;
	lpMMI->ptMinTrackSize.y = APP_MINHEIGHT;

	CDialogEx::OnGetMinMaxInfo(lpMMI);
}

// 进入全屏模式
void CXiaoGongPDFDlg::EnterFullscreen()
{
	if (m_isFullscreen)
		return;

	// 保存当前窗口位置和大小用于恢复
	GetWindowRect(&m_windowRect);

	// ★★★ 先设置全屏标志，避免SetWindowPos触发OnSize时重新布局控件
	m_isFullscreen = true;

	// 移除窗口边框和标题栏
	ModifyStyle(WS_CAPTION | WS_THICKFRAME, 0, SWP_FRAMECHANGED);

	// 获取屏幕尺寸
	int screenWidth = GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = GetSystemMetrics(SM_CYSCREEN);

	// 调整窗口大小为全屏
	SetWindowPos(NULL, 0, 0, screenWidth, screenHeight, SWP_NOZORDER);

	// 隐藏菜单
	SetMenu(NULL);

	// 隐藏工具栏、标签页、缩略图和其他控件
	if (m_toolbar.GetSafeHwnd())
		m_toolbar.ShowWindow(SW_HIDE);

	if (m_tabCtrl.GetSafeHwnd())
		m_tabCtrl.ShowWindow(SW_HIDE);

	if (m_thumbnailList.GetSafeHwnd())
		m_thumbnailList.ShowWindow(SW_HIDE);

	if (m_btnFirst.GetSafeHwnd())
		m_btnFirst.ShowWindow(SW_HIDE);

	if (m_btnLast.GetSafeHwnd())
		m_btnLast.ShowWindow(SW_HIDE);

	if (m_btnFullscreen.GetSafeHwnd())
		m_btnFullscreen.ShowWindow(SW_HIDE);

	if (m_btnRotateLeft.GetSafeHwnd())
		m_btnRotateLeft.ShowWindow(SW_HIDE);

	if (m_btnRotateRight.GetSafeHwnd())
		m_btnRotateRight.ShowWindow(SW_HIDE);

	if (m_btnEdit.GetSafeHwnd())
		m_btnEdit.ShowWindow(SW_HIDE);

	if (m_editCurrent.GetSafeHwnd())
		m_editCurrent.ShowWindow(SW_HIDE);

	if (m_statusBar.GetSafeHwnd())
		m_statusBar.ShowWindow(SW_HIDE);

	if (m_checkThumbnail.GetSafeHwnd())
		m_checkThumbnail.ShowWindow(SW_HIDE);

	// 调整预览窗口为整个客户区
	CRect clientRect;
	GetClientRect(&clientRect);
	m_pdfView.MoveWindow(0, 0, clientRect.Width(), clientRect.Height());

	// 重绘窗口
	Invalidate();
	UpdateWindow();
}

// 退出全屏模式
void CXiaoGongPDFDlg::ExitFullscreen()
{
	if (!m_isFullscreen)
		return;

	// 恢复窗口样式
	ModifyStyle(0, WS_CAPTION | WS_THICKFRAME, SWP_FRAMECHANGED);

	// 恢复菜单
	SetMenu(CMenu::FromHandle(::LoadMenu(AfxGetInstanceHandle(), MAKEINTRESOURCE(IDR_MENU))));

	// ★★★ 重新填充最近文件列表（因为菜单是重新加载的）
	UpdateRecentFilesMenu();

	// 恢复窗口位置和大小
	SetWindowPos(NULL, m_windowRect.left, m_windowRect.top,
		m_windowRect.Width(), m_windowRect.Height(), SWP_NOZORDER);

	// 显示隐藏的控件
	if (m_toolbar.GetSafeHwnd())
		m_toolbar.ShowWindow(SW_SHOW);

	// 显示标签页控件
	if (m_tabCtrl.GetSafeHwnd())
		m_tabCtrl.ShowWindow(SW_SHOW);

	if (m_thumbnailList.GetSafeHwnd())
		m_thumbnailList.ShowWindow(m_thumbnailVisible ? SW_SHOW : SW_HIDE);

	if (m_btnFirst.GetSafeHwnd())
		m_btnFirst.ShowWindow(SW_SHOW);

	if (m_btnLast.GetSafeHwnd())
		m_btnLast.ShowWindow(SW_SHOW);

	if (m_btnFullscreen.GetSafeHwnd())
		m_btnFullscreen.ShowWindow(SW_SHOW);

	if (m_btnRotateLeft.GetSafeHwnd())
		m_btnRotateLeft.ShowWindow(SW_SHOW);

	if (m_btnRotateRight.GetSafeHwnd())
		m_btnRotateRight.ShowWindow(SW_SHOW);

	if (m_btnEdit.GetSafeHwnd())
		m_btnEdit.ShowWindow(SW_SHOW);

	if (m_editCurrent.GetSafeHwnd())
		m_editCurrent.ShowWindow(SW_SHOW);

	if (m_statusBar.GetSafeHwnd())
		m_statusBar.ShowWindow(SW_SHOW);

	if (m_checkThumbnail.GetSafeHwnd())
		m_checkThumbnail.ShowWindow(SW_SHOW);

	// 重设全屏标志
	m_isFullscreen = false;

	// 调用OnSize以重新布局所有控件
	CRect clientRect;
	GetClientRect(&clientRect);
	OnSize(SIZE_RESTORED, clientRect.Width(), clientRect.Height());

	// 如果有文档加载，根据当前模式重新渲染
	if (m_doc && m_currentPage >= 0)
	{
		if (m_continuousScrollMode)
		{
			// 连续滚动模式：重新计算页面位置并渲染可见页面
			CalculatePagePositions();
			UpdateScrollBar();
			RenderVisiblePages();
		}
		else
		{
			// 分页模式：重新渲染当前页
			CleanupBitmap();
			RenderPage(m_currentPage);
		}
	}

	// 确保窗口和所有控件更新
	Invalidate();
	UpdateWindow();
}

// ============ 缩放状态管理函数 ============

// 保存当前页面的缩放状态和平移位置
void CXiaoGongPDFDlg::SaveCurrentPageZoomState()
{
	if (m_currentPage >= 0 && m_currentPage < m_totalPages)
	{
		m_pageZoomStates[m_currentPage] = PageZoomState(m_zoomMode, m_customZoom, m_panOffset);
#ifdef _DEBUG
		TRACE(_T("保存页面 %d 的状态: mode=%d, zoom=%.2f, offset=(%d,%d)\n"),
			m_currentPage, m_zoomMode, m_customZoom, m_panOffset.x, m_panOffset.y);
#endif
	}
}

// 恢复指定页面的缩放状态和平移位置
void CXiaoGongPDFDlg::RestorePageZoomState(int pageNumber)
{
	if (pageNumber < 0 || pageNumber >= m_totalPages)
		return;

	auto it = m_pageZoomStates.find(pageNumber);
	if (it != m_pageZoomStates.end())
	{
		// 找到了该页面的状态，恢复它
		m_zoomMode = it->second.zoomMode;
		m_customZoom = it->second.customZoom;
		m_panOffset = it->second.panOffset;  // ★★★ 恢复平移位置
#ifdef _DEBUG
		TRACE(_T("恢复页面 %d 的状态: mode=%d, zoom=%.2f, offset=(%d,%d)\n"),
			pageNumber, m_zoomMode, m_customZoom, m_panOffset.x, m_panOffset.y);
#endif
	}
	else
	{
		// 没有保存过该页面的状态，使用默认值
		m_zoomMode = ZOOM_FIT_PAGE;
		m_customZoom = 1.0f;
		m_panOffset = CPoint(0, 0);  // ★★★ 重置平移位置
#ifdef _DEBUG
		TRACE(_T("页面 %d 使用默认状态\n"), pageNumber);
#endif
	}
}

// 获取页面缩放状态
CXiaoGongPDFDlg::PageZoomState CXiaoGongPDFDlg::GetPageZoomState(int pageNumber)
{
	auto it = m_pageZoomStates.find(pageNumber);
	if (it != m_pageZoomStates.end())
	{
		return it->second;
	}
	return PageZoomState();  // 返回默认状态
}

// ============ 缩放功能实现 ============

void CXiaoGongPDFDlg::SetZoom(float zoom, ZoomMode mode)
{
#ifdef _DEBUG
	TRACE(_T("SetZoom() : zoom=%.2f, mode=%d\n"), zoom, mode);
#endif

	if (!m_doc || m_totalPages <= 0)
		return;

	// ★★★ 如果缩放值和模式都没有变化，直接返回，避免不必要的重新渲染
	if (m_zoomMode == mode && fabs(m_customZoom - zoom) < 0.001f)
	{
#ifdef _DEBUG
		TRACE(_T("SetZoom() : 缩放未改变，跳过渲染\n"));
#endif
		return;
	}

	m_zoomMode = mode;
	m_customZoom = zoom;

	// ★★★ 保存当前页面的缩放状态
	SaveCurrentPageZoomState();

	// ★★★ 切换缩放模式时重置平移偏移量
	if (mode != ZOOM_CUSTOM)
	{
		ResetPanOffset();
	}

	// ★★★ 清除当前页面的所有缩存（因为缩放级别变了）
	// 遍历并删除所有与当前页面相关的缓存项
	std::vector<PageCacheKey> keysToRemove;
	for (auto it = m_pageCache.begin(); it != m_pageCache.end(); ++it)
	{
		if (it->first.pageNumber == m_currentPage)
		{
			keysToRemove.push_back(it->first);
		}
	}

	// 删除找到的缓存项
	for (const auto& key : keysToRemove)
	{
		auto it = m_pageCache.find(key);
		if (it != m_pageCache.end())
		{
			if (it->second.hBitmap)
				DeleteObject(it->second.hBitmap);
			m_pageCache.erase(it);

			// 从LRU列表中移除
			for (auto listIt = m_cacheOrder.begin(); listIt != m_cacheOrder.end(); ++listIt)
			{
				if (*listIt == key)
				{
					m_cacheOrder.erase(listIt);
					break;
				}
			}
		}
	}

	// ★★★ 根据显示模式选择不同的渲染方式
	if (m_continuousScrollMode)
	{
		// 连续滚动模式：更新统一缩放比例并重新渲染所有可见页面
		// 保存当前的滚动位置比例（相对于总高度）
		float scrollRatio = 0.0f;
		if (m_totalScrollHeight > 0)
		{
			scrollRatio = (float)m_scrollPosition / m_totalScrollHeight;
		}

		// 更新统一缩放比例（基于自定义缩放值）
		CRect viewRect;
		m_pdfView.GetClientRect(&viewRect);
		int viewWidth = viewRect.Width() - 40;  // 留出左右边距各20像素

		// ★★★ 在连续滚动模式下，使用 m_customZoom 调整 m_uniformScale
		// m_uniformScale 的基准是适应宽度的缩放，m_customZoom 是在此基础上的倍数
		if (m_documentMaxPageWidth > 0)
		{
			float baseScale = (float)viewWidth / m_documentMaxPageWidth * 0.95f;
			m_uniformScale = baseScale * m_customZoom;
		}

		// 重新计算所有页面位置
		CalculatePagePositions();

		// 恢复滚动位置（按比例）
		if (m_totalScrollHeight > 0)
		{
			m_scrollPosition = (int)(scrollRatio * m_totalScrollHeight);

			// 限制范围
			int maxScroll = m_totalScrollHeight - viewRect.Height();
			if (maxScroll < 0) maxScroll = 0;
			if (m_scrollPosition > maxScroll) m_scrollPosition = maxScroll;
			if (m_scrollPosition < 0) m_scrollPosition = 0;
		}

		// 更新滚动条并重新渲染可见页面
		UpdateScrollBar();
		RenderVisiblePages();
	}
	else
	{
		// 分页模式：重新渲染当前页
		CleanupBitmap();
		RenderPage(m_currentPage);
	}

	// 更新状态栏
	UpdateStatusBar();
}

// ============ 占位函数实现（其他功能） ============

void CXiaoGongPDFDlg::onMenuPrint()
{
#ifdef _DEBUG
	TRACE(_T("onMenuPrint()\n"));
#endif

	if (!m_doc || m_totalPages <= 0)
	{
		MessageBox(_T("请先打开PDF文档"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	// 创建自定义打印对话框
	CCustomPrintDlg printDlg(m_totalPages, this);

	if (printDlg.DoModal() == IDOK)
	{
		// 获取打印机DC
		HDC hPrinterDC = CreateDC(NULL, printDlg.m_printerName, NULL, NULL);
		if (!hPrinterDC)
		{
			MessageBox(_T("无法获取打印机设备"), _T("错误"), MB_OK | MB_ICONERROR);
			return;
		}

		// 创建打印DC
		CDC dcPrint;
		dcPrint.Attach(hPrinterDC);

		// 获取打印机信息
		int horRes = dcPrint.GetDeviceCaps(HORZRES);  // 打印机水平分辨率
		int verRes = dcPrint.GetDeviceCaps(VERTRES);  // 打印机垂直分辨率

		// 准备打印文档信息
		DOCINFO di;
		memset(&di, 0, sizeof(DOCINFO));
		di.cbSize = sizeof(DOCINFO);
		di.lpszDocName = _T("小龚PDF阅读器 打印");

		// 开始打印文档
		if (dcPrint.StartDoc(&di) < 0)
		{
			MessageBox(_T("无法开始打印"), _T("错误"), MB_OK | MB_ICONERROR);
			dcPrint.DeleteDC();
			return;
		}

		// 确定要打印的页面范围
		int startPage = printDlg.m_pageFrom - 1;
		int endPage = printDlg.m_pageTo - 1;

		// 显示进度
		bool cancelled = false;

		try
		{
			// 如果需要自动分页，处理多份打印
			int totalCopies = printDlg.m_copies;

			if (printDlg.m_bCollate)
			{
				// 自动分页：逐份打印 (1,2,3) (1,2,3) (1,2,3)
				for (int copy = 0; copy < totalCopies && !cancelled; copy++)
				{
					for (int pageNum = startPage; pageNum <= endPage && !cancelled; pageNum++)
					{
						PrintSinglePage(dcPrint, pageNum, horRes, verRes, printDlg.m_bDuplex);
					}
				}
			}
			else
			{
				// 不自动分页：按页打印 (1,1,1) (2,2,2) (3,3,3)
				for (int pageNum = startPage; pageNum <= endPage && !cancelled; pageNum++)
				{
					for (int copy = 0; copy < totalCopies && !cancelled; copy++)
					{
						PrintSinglePage(dcPrint, pageNum, horRes, verRes, printDlg.m_bDuplex);
					}
				}
			}

			// 结束打印文档
			dcPrint.EndDoc();

			MessageBox(_T("打印完成"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		}
		catch (...)
		{
			dcPrint.AbortDoc();
			MessageBox(_T("打印过程中发生错误"), _T("错误"), MB_OK | MB_ICONERROR);
		}

		dcPrint.DeleteDC();
	}
}
void CXiaoGongPDFDlg::onMenuRecentFile(UINT nID)
{
#ifdef _DEBUG
	TRACE(_T("onMenuRecentFile() : ID=%d\n"), nID);
#endif

	// 计算文件索引
	int index = nID - ID_MENU_RECENT_FILE_1;

	if (index >= 0 && index < (int)m_recentFiles.size())
	{
		CString filePath = m_recentFiles[index];

		// 检查文件是否存在
		if (!PathFileExists(filePath))
		{
			MessageBox(_T("文件不存在或已被删除"), _T("错误"), MB_OK | MB_ICONERROR);

			// 从列表中移除
			m_recentFiles.erase(m_recentFiles.begin() + index);
			SaveRecentFiles();
			UpdateRecentFilesMenu();
			return;
		}

		// 使用新的多文档模式打开
		OpenPDFInNewTab(filePath);
	}
}


void CXiaoGongPDFDlg::LoadRecentFiles()
{
#ifdef _DEBUG
	TRACE(_T("LoadRecentFiles()\n"));
#endif

	m_recentFiles.clear();

	// 从注册表加载最近文件列表
	// 注册表路径: HKEY_CURRENT_USER\Software\XiaoGongPDF\RecentFiles
	CRegKey regKey;
	LONG result = regKey.Open(HKEY_CURRENT_USER, _T("Software\\XiaoGongPDF\\RecentFiles"), KEY_READ);

	if (result == ERROR_SUCCESS)
	{
		// 读取最多5个最近文件
		for (int i = 0; i < RECENT_FILE_NUMS; i++)
		{
			CString valueName;
			valueName.Format(_T("File%d"), i);

			TCHAR filePath[MAX_PATH] = {0};
			ULONG size = MAX_PATH;

			if (regKey.QueryStringValue(valueName, filePath, &size) == ERROR_SUCCESS)
			{
				// 检查文件是否存在
				if (PathFileExists(filePath))
				{
					m_recentFiles.push_back(filePath);
				}
			}
		}

		regKey.Close();
	}
}

void CXiaoGongPDFDlg::SaveRecentFiles()
{
#ifdef _DEBUG
	TRACE(_T("SaveRecentFiles()\n"));
#endif

	// 保存最近文件列表到注册表
	CRegKey regKey;
	LONG result = regKey.Create(HKEY_CURRENT_USER, _T("Software\\XiaoGongPDF\\RecentFiles"));

	if (result == ERROR_SUCCESS)
	{
		// 保存最近文件（数量由 RECENT_FILE_NUMS 定义）
		int count = min((int)m_recentFiles.size(), RECENT_FILE_NUMS);
		for (int i = 0; i < count; i++)
		{
			CString valueName;
			valueName.Format(_T("File%d"), i);
			regKey.SetStringValue(valueName, m_recentFiles[i]);
		}

		regKey.Close();
	}
}

void CXiaoGongPDFDlg::AddRecentFile(const CString& filePath)
{
#ifdef _DEBUG
	TRACE(_T("AddRecentFile() : %s\n"), filePath.GetString());
#endif

	// 如果文件已存在，先移除
	for (auto it = m_recentFiles.begin(); it != m_recentFiles.end(); ++it)
	{
		if (it->CompareNoCase(filePath) == 0)
		{
			m_recentFiles.erase(it);
			break;
		}
	}

	// 添加到列表开头
	m_recentFiles.insert(m_recentFiles.begin(), filePath);

	// 保持最近文件数量限制（由 RECENT_FILE_NUMS 定义）
	if (m_recentFiles.size() > RECENT_FILE_NUMS)
	{
		m_recentFiles.resize(RECENT_FILE_NUMS);
	}

	// 保存到注册表
	SaveRecentFiles();

	// 更新菜单
	UpdateRecentFilesMenu();
}

void CXiaoGongPDFDlg::UpdateRecentFilesMenu()
{
#ifdef _DEBUG
	TRACE(_T("UpdateRecentFilesMenu()\n"));
#endif

	// 获取菜单
	CMenu* pMenu = GetMenu();
	if (!pMenu) return;

	// 获取"文件"菜单（假设是第一个菜单）
	CMenu* pFileMenu = pMenu->GetSubMenu(0);
	if (!pFileMenu) return;

	// 查找"打开"和"退出"菜单项的位置
	int openPos = -1;
	int exitPos = -1;
	for (int i = 0; i < (int)pFileMenu->GetMenuItemCount(); i++)
	{
		UINT itemID = pFileMenu->GetMenuItemID(i);
		if (itemID == ID_MENU_OPEN)
		{
			openPos = i;
		}
		else if (itemID == ID_MENU_EXIT)
		{
			exitPos = i;
			break;  // 找到退出菜单后可以停止
		}
	}

	// 删除"打开"和"退出"之间的所有项（包括历史文件菜单项和分隔符）
	if (openPos >= 0 && exitPos > openPos + 1)
	{
		// 从后往前删除，避免索引变化的问题
		for (int i = exitPos - 1; i > openPos; i--)
		{
			pFileMenu->RemoveMenu(i, MF_BYPOSITION);
		}
		// 删除后，退出菜单紧跟在打开菜单后面
		exitPos = openPos + 1;
	}

	// 添加最近文件菜单项
	if (!m_recentFiles.empty())
	{
		if (exitPos > 0)
		{
			// 插入分隔符
			pFileMenu->InsertMenu(exitPos, MF_BYPOSITION | MF_SEPARATOR);

			// 插入最近文件（数量由 RECENT_FILE_NUMS 定义）
			int count = min((int)m_recentFiles.size(), RECENT_FILE_NUMS);
			for (int i = 0; i < count; i++)
			{
				CString menuText;
				// 提取文件名
				CString fileName = m_recentFiles[i];
				int pos = fileName.ReverseFind(_T('\\'));
				if (pos != -1)
					fileName = fileName.Mid(pos + 1);

				menuText.Format(_T("&%d %s"), i + 1, fileName.GetString());
				pFileMenu->InsertMenu(exitPos + i + 1, MF_BYPOSITION | MF_STRING,
					ID_MENU_RECENT_FILE_1 + i, menuText);
			}
		}
	}

	DrawMenuBar();
}

void CXiaoGongPDFDlg::OnDropFiles(HDROP hDropInfo)
{
#ifdef _DEBUG
	TRACE(_T("OnDropFiles()\n"));
#endif

	// 获取拖放的文件数量
	UINT fileCount = DragQueryFile(hDropInfo, 0xFFFFFFFF, NULL, 0);

	if (fileCount > 0)
	{
		// 只处理第一个文件
		TCHAR filePath[MAX_PATH];
		DragQueryFile(hDropInfo, 0, filePath, MAX_PATH);

		// 检查是否是PDF文件
		CString strPath(filePath);
		strPath.MakeLower();
		if (strPath.Right(4) == _T(".pdf"))
		{
			// 在新标签页中打开PDF
			OpenPDFInNewTab(CString(filePath));
		}
		else
		{
			MessageBox(_T("请拖放PDF文件"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		}
	}

	DragFinish(hDropInfo);
}

// ============ 进程间通信：接收新打开的PDF文件路径 ============

BOOL CXiaoGongPDFDlg::OnCopyData(CWnd* pWnd, COPYDATASTRUCT* pCopyDataStruct)
{
	if (pCopyDataStruct && pCopyDataStruct->dwData == 1)
	{
		// dwData == 1 表示打开PDF文件
		CString filePath((LPCTSTR)pCopyDataStruct->lpData);

		// 检查文件是否存在
		if (PathFileExists(filePath))
		{
			// 在新标签页中打开PDF
			OpenPDFInNewTab(filePath);
		}

		return TRUE;
	}

	return CDialogEx::OnCopyData(pWnd, pCopyDataStruct);
}

// ============ 多文档标签页管理功能 ============

void CXiaoGongPDFDlg::InitializeTabControl()
{
#ifdef _DEBUG
	TRACE(_T("InitializeTabControl()\n"));
	TRACE(_T("标签页控件句柄: %p\n"), m_tabCtrl.GetSafeHwnd());
#endif

	if (!m_tabCtrl.GetSafeHwnd())
	{
#ifdef _DEBUG
		TRACE(_T("错误: 标签页控件句柄无效!\n"));
#endif
		return;
	}

	// 配置现代化标签页控件
	m_tabCtrl.EnableCloseButton(TRUE);  // 启用关闭按钮
	m_tabCtrl.SetTabHeight(36);         // 设置标签页高度

	// 设置颜色方案（可选，使用默认值也很美观）
	// m_tabCtrl.SetColors(RGB(240, 240, 240), RGB(230, 230, 230), RGB(255, 255, 255));

	// 添加滚动按钮样式和工具提示（ModernTabCtrl也支持这些）
	// 添加TCS_FIXEDWIDTH样式使所有标签页宽度一致
	m_tabCtrl.ModifyStyle(0, TCS_SCROLLOPPOSITE | TCS_TOOLTIPS | TCS_FIXEDWIDTH);

	// 设置标签页固定宽度为100像素
	m_tabCtrl.SendMessage(TCM_SETITEMSIZE, 0, MAKELPARAM(100, 36));

#ifdef _DEBUG
	TRACE(_T("现代化标签页控件初始化完成\n"));
#endif

	// 确保标签页可见
	m_tabCtrl.ShowWindow(SW_SHOW);
}

CPDFDocument* CXiaoGongPDFDlg::GetActiveDocument()
{
	if (m_activeDocIndex >= 0 && m_activeDocIndex < (int)m_documents.size())
	{
		return m_documents[m_activeDocIndex];
	}
	return nullptr;
}

bool CXiaoGongPDFDlg::OpenPDFInNewTab(const CString& filePath)
{
#ifdef _DEBUG
	TRACE(_T("OpenPDFInNewTab: %s\n"), filePath);
	TRACE(_T("当前已打开文档数: %d\n"), (int)m_documents.size());
	TRACE(_T("标签页控件句柄: %p\n"), m_tabCtrl.GetSafeHwnd());
#endif

	// 检查文件是否已经打开
	// 规范化路径用于比较（转为小写、获取完整路径）
	TCHAR fullPath[MAX_PATH];
	GetFullPathName(filePath, MAX_PATH, fullPath, NULL);
	CString normalizedNewPath(fullPath);
	normalizedNewPath.MakeLower();

#ifdef _DEBUG
	TRACE(_T("规范化路径: %s\n"), normalizedNewPath);
#endif

	// 遍历已打开的文档，检查是否重复
	for (int i = 0; i < (int)m_documents.size(); i++)
	{
		CString existingPath = m_documents[i]->GetFilePath();

		// 规范化已打开的文件路径
		TCHAR existingFullPath[MAX_PATH];
		GetFullPathName(existingPath, MAX_PATH, existingFullPath, NULL);
		CString normalizedExistingPath(existingFullPath);
		normalizedExistingPath.MakeLower();

		// 比较路径
		if (normalizedNewPath == normalizedExistingPath)
		{
#ifdef _DEBUG
			TRACE(_T("文件已打开，切换到标签页索引: %d\n"), i);
#endif
			// 文件已打开，切换到对应标签页
			m_tabCtrl.SetCurSel(i);
			SwitchToDocument(i);

			// 显示提示消息
			CString msg;
			msg.Format(_T("该文件已经打开，已切换到对应标签页"));
			MessageBox(msg, _T("提示"), MB_OK | MB_ICONINFORMATION);

			return true;
		}
	}

	// 转换为UTF-8
	CStringW wPath(filePath);
#ifdef _DEBUG
	TRACE(_T("原始文件路径: %s\n"), (LPCTSTR)filePath);
	TRACE(_T("宽字符路径: %s\n"), (LPCWSTR)wPath);
#endif

	int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wPath, -1, NULL, 0, NULL, NULL);
	if (utf8Length <= 0)
	{
#ifdef _DEBUG
		TRACE(_T("UTF-8转换失败: utf8Length=%d\n"), utf8Length);
#endif
		return false;
	}

#ifdef _DEBUG
	TRACE(_T("UTF-8长度: %d\n"), utf8Length);
#endif

	std::vector<char> utf8Path(utf8Length);
	int convertResult = WideCharToMultiByte(CP_UTF8, 0, wPath, -1, utf8Path.data(), utf8Length, NULL, NULL);
	if (convertResult <= 0)
	{
#ifdef _DEBUG
		TRACE(_T("UTF-8转换失败: convertResult=%d\n"), convertResult);
#endif
		return false;
	}

#ifdef _DEBUG
	TRACE(_T("UTF-8转换成功，长度: %d\n"), convertResult);
	TRACE(_T("UTF-8路径(原始字节): "));
	for (int i = 0; i < min(utf8Length, 100); i++) {
		TRACE(_T("%02X "), (unsigned char)utf8Path[i]);
	}
	TRACE(_T("\n"));
#endif

	// 创建新文档
#ifdef _DEBUG
	TRACE(_T("创建新文档对象...\n"));
#endif

	// 显示"正在加载..."提示
	m_statusBar.SetWindowText(_T("正在加载..."));
	m_statusBar.UpdateWindow();  // 立即刷新显示

	CPDFDocument* newDoc = new CPDFDocument(m_ctx);
	if (!newDoc->OpenDocument(utf8Path.data()))
	{
		delete newDoc;
#ifdef _DEBUG
		TRACE(_T("打开PDF文件失败\n"));
#endif
		MessageBox(_T("无法打开PDF文件"), _T("错误"), MB_OK | MB_ICONERROR);

		// 恢复状态栏显示
		UpdateStatusBar();
		return false;
	}

#ifdef _DEBUG
	TRACE(_T("PDF文件打开成功\n"));
#endif

	// 添加到文档列表
	m_documents.push_back(newDoc);
	int newIndex = (int)m_documents.size() - 1;

#ifdef _DEBUG
	TRACE(_T("新文档索引: %d\n"), newIndex);
	TRACE(_T("当前标签页控件句柄: %p\n"), m_tabCtrl.GetSafeHwnd());
	TRACE(_T("当前标签页数量: %d\n"), m_tabCtrl.GetItemCount());
#endif

	// 添加标签页
	if (m_tabCtrl.GetSafeHwnd())
	{
#ifdef _DEBUG
		TRACE(_T("准备添加标签页...\n"));
#endif
		TCITEM tci = {0};
		tci.mask = TCIF_TEXT;
		CString fullFileName = newDoc->GetFileName();
		CString tabTitle = fullFileName;

		// 去掉.pdf后缀
		int dotPos = tabTitle.ReverseFind(_T('.'));
		if (dotPos != -1)
		{
			CString ext = tabTitle.Mid(dotPos);
			ext.MakeLower();
			if (ext == _T(".pdf"))
			{
				tabTitle = tabTitle.Left(dotPos);
			}
		}

		// 截断过长的文件名，避免标签太宽
		const int MAX_TAB_LENGTH = 12;  // 固定宽度100像素，大约可显示12个字符
		if (tabTitle.GetLength() > MAX_TAB_LENGTH)
		{
			// 截断并添加省略号
			tabTitle = tabTitle.Left(MAX_TAB_LENGTH - 3) + _T("...");
#ifdef _DEBUG 
			TRACE(_T("文件名过长，已截断: [%s] -> [%s]\n"), fullFileName, tabTitle);
#endif
		}

		tci.pszText = (LPTSTR)(LPCTSTR)tabTitle;

#ifdef _DEBUG
		TRACE(_T("标签页标题: [%s]\n"), tabTitle);
		TRACE(_T("准备插入位置: %d\n"), newIndex);
#endif

		int insertResult = m_tabCtrl.InsertItem(newIndex, &tci);

#ifdef _DEBUG
		TRACE(_T("InsertItem返回值: %d\n"), insertResult);
		TRACE(_T("插入后标签页总数: %d\n"), m_tabCtrl.GetItemCount());
#endif

		if (insertResult == -1)
		{
#ifdef _DEBUG
			TRACE(_T("错误: InsertItem失败!\n"));
#endif
			MessageBox(_T("添加标签页失败"), _T("调试"), MB_OK);
		}

		// 切换到新文档
		m_tabCtrl.SetCurSel(newIndex);

#ifdef _DEBUG
		TRACE(_T("SetCurSel: %d\n"), newIndex);
		TRACE(_T("当前选中标签页: %d\n"), m_tabCtrl.GetCurSel());
#endif

		// 强制重绘标签页控件
		m_tabCtrl.Invalidate();
		m_tabCtrl.UpdateWindow();
	}
	else
	{
#ifdef _DEBUG
		TRACE(_T("错误: 标签页控件句柄无效!\n"));
#endif
		MessageBox(_T("标签页控件未初始化"), _T("错误"), MB_OK | MB_ICONERROR);
	}

	SwitchToDocument(newIndex);

	// 添加到最近文件列表
	AddRecentFile(filePath);

	// 刷新布局
	CRect rect;
	GetClientRect(&rect);
	OnSize(SIZE_RESTORED, rect.Width(), rect.Height());

	// ★★★ 在布局完成后，异步渲染缩略图
	// 这样预览框可以快速显示，缩略图在后台加载
	UpdateThumbnails();

	return true;
}

void CXiaoGongPDFDlg::SwitchToDocument(int index)
{
#ifdef _DEBUG
	TRACE(_T("======== SwitchToDocument: %d ========\n"), index);
	TRACE(_T("文档总数: %d\n"), (int)m_documents.size());
#endif

	if (index < 0 || index >= (int)m_documents.size())
	{
#ifdef _DEBUG
		TRACE(_T("错误: 无效的文档索引 %d\n"), index);
#endif
		return;
	}

	// 保存当前文档的状态
	if (m_activeDocIndex >= 0 && m_activeDocIndex < (int)m_documents.size())
	{
		CPDFDocument* oldDoc = m_documents[m_activeDocIndex];
		if (oldDoc)
		{
#ifdef _DEBUG
			TRACE(_T("保存旧文档 %d 的状态\n"), m_activeDocIndex);
			TRACE(_T("旧文档缩略图数量: %d\n"), (int)m_thumbnailCache.size());
#endif
			// 保存当前页面状态
			oldDoc->SaveCurrentPageZoomState();
			oldDoc->SetCurrentPage(m_currentPage);

			// ★★★ 保存文档级别的缩放状态（每个PDF文档有自己的缩放比例）
			oldDoc->SetZoom(m_customZoom, m_zoomMode);

			oldDoc->SetCurrentBitmap(m_hCurrentBitmap);
			m_hCurrentBitmap = NULL;  // 转移所有权给文档对象

			// ★★★ 保存缩略图缓存到文档对象（使用swap避免浅拷贝导致的重复删除）
			oldDoc->GetThumbnailCache().swap(m_thumbnailCache);

			// ★★★ 保存缩略图尺寸信息
			oldDoc->SetThumbnailPicWidth(m_thumbnailPicWidth);
			oldDoc->SetThumbnailPicHeight(m_thumbnailPicHeight);

			// ★★★ 保存搜索信息到文档对象
			oldDoc->SetSearchMatches(m_searchMatches);
			oldDoc->SetCurrentMatchIndex(m_currentMatchIndex);
			CString searchKeyword;
			m_editSearch.GetWindowText(searchKeyword);
			oldDoc->SetSearchKeyword(searchKeyword);

			// ★★★ 保存滚动位置到文档对象
			oldDoc->SetScrollPosition(m_scrollPosition);

			// ★★★ 保存旋转状态到文档对象（使用swap避免浅拷贝）
			oldDoc->GetPageRotations().swap(m_pageRotations);

#ifdef _DEBUG
			TRACE(_T("已保存缩略图缓存到文档对象，尺寸: %d x %d\n"),
				m_thumbnailPicWidth, m_thumbnailPicHeight);
			TRACE(_T("已保存滚动位置: %d\n"), m_scrollPosition);
#endif
		}
	}

	// 切换到新文档
	m_activeDocIndex = index;
	CPDFDocument* newDoc = m_documents[index];

#ifdef _DEBUG
	TRACE(_T("切换到新文档 %d\n"), index);
	TRACE(_T("新文档指针: %p\n"), newDoc);
	TRACE(_T("新文档文件名: %s\n"), newDoc->GetFileName());
#endif

	// ★★★ 清空旧文档的缩放状态（每个文档应该有独立的状态）
	m_pageZoomStates.clear();

	// ★★★ 重置缩放和平移状态为默认值（稍后会从新文档恢复）
	m_zoomMode = ZOOM_FIT_PAGE;
	m_customZoom = 1.0f;
	m_panOffset = CPoint(0, 0);
	m_canDrag = false;

	// 更新对话框的成员变量（从CPDFDocument复制到对话框）
	m_doc = newDoc->GetDocument();
	m_totalPages = newDoc->GetTotalPages();
	m_currentPage = newDoc->GetCurrentPage();

	// ★★★ 立即清空页面位置缓存，防止在CalculatePagePositions()调用前触发RenderVisiblePages()导致越界
	m_pageYPositions.clear();
	m_pageHeights.clear();

#ifdef _DEBUG
	TRACE(_T("m_doc: %p, 总页数: %d, 当前页: %d\n"), m_doc, m_totalPages, m_currentPage);
#endif

	// 转移位图所有权（使用 TransferCurrentBitmap 避免误删除）
	m_hCurrentBitmap = newDoc->TransferCurrentBitmap();

#ifdef _DEBUG
	TRACE(_T("位图句柄: %p\n"), m_hCurrentBitmap);
#endif

	// ★★★ 恢复缩略图缓存（使用swap避免浅拷贝导致的重复删除）
	m_thumbnailCache.swap(newDoc->GetThumbnailCache());

	// ★★★ 恢复缩略图尺寸信息
	m_thumbnailPicWidth = newDoc->GetThumbnailPicWidth();
	m_thumbnailPicHeight = newDoc->GetThumbnailPicHeight();

#ifdef _DEBUG
	TRACE(_T("恢复缩略图缓存，数量: %d，尺寸: %d x %d\n"),
		(int)m_thumbnailCache.size(), m_thumbnailPicWidth, m_thumbnailPicHeight);
#endif

	// ★★★ 恢复页面旋转状态（使用swap避免浅拷贝）
	m_pageRotations.swap(newDoc->GetPageRotations());

	// ★★★ 恢复文档级别的缩放状态（每个PDF文档有自己的缩放比例）
	m_zoom = newDoc->GetZoom();
	m_zoomMode = newDoc->GetZoomMode();
	m_customZoom = newDoc->GetCustomZoom();

	// ★★★ 在连续滚动模式下，重置平移位置（连续滚动不使用拖拽平移）
	m_panOffset = CPoint(0, 0);
	m_canDrag = false;

#ifdef _DEBUG
	TRACE(_T("恢复旋转状态，旋转信息数量: %d\n"), (int)m_pageRotations.size());
	TRACE(_T("恢复文档缩放状态: zoom=%.2f, mode=%d, customZoom=%.2f\n"),
		m_zoom, m_zoomMode, m_customZoom);
#endif

	// 更新窗口标题
	CString title;
	title.Format(_T("小龚PDF阅读器 - %s"), newDoc->GetFileName());
	SetWindowText(title);

	// 显示缩略图列表（如果之前隐藏了）
	if (m_thumbnailVisible && m_thumbnailList.GetSafeHwnd())
	{
		m_thumbnailList.ShowWindow(SW_SHOW);
	}

	// ★★★ 清空页面缓存，避免显示旧文档的缓存内容
	// 因为m_pageCache是全局的，缓存键不包含文档标识，切换文档时必须清空
	ClearPageCache();

#ifdef _DEBUG
	TRACE(_T("已清空页面缓存，准备渲染新文档\n"));
#endif

	// ★★★ 恢复搜索信息（每个文档的搜索结果应该独立）
	m_searchMatches = newDoc->GetSearchMatches();
	m_currentMatchIndex = newDoc->GetCurrentMatchIndex();
	m_editSearch.SetWindowText(newDoc->GetSearchKeyword());

#ifdef _DEBUG
	TRACE(_T("已恢复搜索信息: %d 个匹配项, 当前索引=%d\n"),
		(int)m_searchMatches.size(), m_currentMatchIndex);
#endif

	// ★★★ 恢复滚动位置（保持用户之前的浏览位置）
	m_scrollPosition = newDoc->GetScrollPosition();

#ifdef _DEBUG
	TRACE(_T("已恢复滚动位置: %d\n"), m_scrollPosition);
#endif

	// ★★★ 连续滚动模式：重新计算页面位置并渲染可见页面
	CalculatePagePositions();
	UpdateScrollBar();
	RenderVisiblePages();

	// 更新UI控件
	UpdatePageControls();
	UpdateStatusBar();

	// ★★★ 更新缩略图列表显示
	// 切换标签页时需要刷新缩略图列表，使用已恢复的缓存
	UpdateThumbnails();
}

void CXiaoGongPDFDlg::CloseDocument(int index)
{
#ifdef _DEBUG
	TRACE(_T("CloseDocument: %d\n"), index);
#endif

	if (index < 0 || index >= (int)m_documents.size())
	{
		return;
	}

	// 删除文档对象
	CPDFDocument* doc = m_documents[index];
	if (doc)
	{
		delete doc;
	}

	// 从列表中移除
	m_documents.erase(m_documents.begin() + index);

	// 删除标签页
	m_tabCtrl.DeleteItem(index);

	// 如果关闭的是当前文档
	if (index == m_activeDocIndex)
	{
		if (m_documents.empty())
		{
			// 没有文档了，重置状态
			m_activeDocIndex = -1;
			m_doc = nullptr;
			m_totalPages = 0;
			m_currentPage = 0;
			SetWindowText(_T("小龚PDF阅读器"));

			// 隐藏标签页控件
			if (m_tabCtrl.GetSafeHwnd())
			{
				m_tabCtrl.ShowWindow(SW_HIDE);
			}

			// 清空显示
			CleanupBitmap();
			CleanupThumbnails();
			UpdatePageControls();
			UpdateStatusBar();

			// 刷新布局
			CRect rect;
			GetClientRect(&rect);
			OnSize(SIZE_RESTORED, rect.Width(), rect.Height());
		}
		else
		{
			// 切换到其他文档
			int newIndex = (index > 0) ? (index - 1) : 0;
			if (m_tabCtrl.GetSafeHwnd())
			{
				m_tabCtrl.SetCurSel(newIndex);
			}
			SwitchToDocument(newIndex);
		}
	}
	else if (index < m_activeDocIndex)
	{
		// 关闭的文档在当前文档之前，需要调整索引
		m_activeDocIndex--;
	}
}

void CXiaoGongPDFDlg::CloseCurrentDocument()
{
	if (m_activeDocIndex >= 0)
	{
		CloseDocument(m_activeDocIndex);
	}
}

void CXiaoGongPDFDlg::UpdateTabControl()
{
	// 更新所有标签页的标题
	for (int i = 0; i < (int)m_documents.size(); i++)
	{
		TCITEM tci = {0};
		tci.mask = TCIF_TEXT;
		CString tabTitle = m_documents[i]->GetFileName();
		tci.pszText = (LPTSTR)(LPCTSTR)tabTitle;
		m_tabCtrl.SetItem(i, &tci);
	}
}

void CXiaoGongPDFDlg::OnTabSelChange(NMHDR* pNMHDR, LRESULT* pResult)
{
#ifdef _DEBUG
	TRACE(_T("OnTabSelChange 被调用\n"));
#endif

	int newSel = m_tabCtrl.GetCurSel();

#ifdef _DEBUG
	TRACE(_T("当前选中标签: %d, 当前活动文档: %d\n"), newSel, m_activeDocIndex);
#endif

	if (newSel != m_activeDocIndex && newSel >= 0)
	{
#ifdef _DEBUG
		TRACE(_T("切换到文档 %d\n"), newSel);
#endif
		SwitchToDocument(newSel);
	}
	else
	{
#ifdef _DEBUG
		TRACE(_T("不需要切换（相同文档或索引无效）\n"));
#endif
	}
	*pResult = 0;
}

// 处理标签页关闭按钮点击事件
LRESULT CXiaoGongPDFDlg::OnTabCloseButton(WPARAM wParam, LPARAM lParam)
{
	int index = (int)wParam;

#ifdef _DEBUG
	TRACE(_T("OnTabCloseButton: 关闭标签 %d\n"), index);
#endif

	// 关闭指定索引的文档
	if (index >= 0 && index < (int)m_documents.size())
	{
		CloseDocument(index);
	}

	return 0;
}

// ============ 缩略图面板切换功能 ============

void CXiaoGongPDFDlg::ToggleThumbnailPanel()
{
#ifdef _DEBUG
	TRACE(_T("ToggleThumbnailPanel()\n"));
#endif

	// 切换状态
	m_thumbnailVisible = !m_thumbnailVisible;

	// 显示/隐藏缩略图列表
	m_thumbnailList.ShowWindow(m_thumbnailVisible ? SW_SHOW : SW_HIDE);

	// 触发窗口重新布局
	CRect rect;
	GetClientRect(&rect);
	OnSize(SIZE_RESTORED, rect.Width(), rect.Height());

	// 如果有文档加载，根据当前模式重新渲染
	if (m_doc && m_currentPage >= 0)
	{
		if (m_continuousScrollMode)
		{
			// 连续滚动模式：重新计算页面位置并渲染可见页面
			CalculatePagePositions();

			// ★★★ 确保滚动条可见
			m_pdfView.ShowScrollBar(SB_VERT, TRUE);
			m_pdfView.EnableScrollBarCtrl(SB_VERT, TRUE);

			UpdateScrollBar();
			RenderVisiblePages();
		}
		else
		{
			// 分页模式：重新渲染当前页

			// ★★★ 分页模式下隐藏滚动条
			m_pdfView.ShowScrollBar(SB_VERT, FALSE);

			CleanupBitmap();
			RenderPage(m_currentPage);
		}
	}
}

void CXiaoGongPDFDlg::OnBtnToggleThumbnail()
{
#ifdef _DEBUG
	TRACE(_T("OnBtnToggleThumbnail()\n"));
#endif

	ToggleThumbnailPanel();
}

//自定义ok 函数,取消内部实现,不然会触发断言失败
/*
1. 主对话框 IDD_MOUTAIPDF_DIALOG 中没有 IDOK 按钮(从第93 - 107行可以看到)
2. 但编辑框 IDC_EDIT_CURRENT 有 ES_NUMBER 样式(第105行)
3. 当在编辑框中按回车键时:
- MFC 的默认行为会查找对话框中的 DEFPUSHBUTTON(默认按钮,ID 为 IDOK)
- 由于主对话框中没有 IDOK 按钮
- MFC 尝试调用 OnOK() 函数,但该函数期望找到 IDOK 控件
- 因为控件不存在,触发了 afxwin2.inl:613 的断言失败
*/

void CXiaoGongPDFDlg::OnOK()
{
	// 空实现,防止按回车键时触发断言
}

// 自定义Cancel函数,防止关闭时发出警告音
void CXiaoGongPDFDlg::OnCancel()
{
	// 直接调用基类的OnCancel来正常关闭对话框
	CDialogEx::OnCancel();
}

void CXiaoGongPDFDlg::OnDestroy()
{
	TRACE(_T("OnDestroy() called\n"));
	UIRelease();             // 先在 HWND 有效时解绑 UI
	CDialogEx::OnDestroy();
}

void CXiaoGongPDFDlg::PostNcDestroy()
{
	TRACE(_T("PostNcDestroy() called\n"));
	CDialogEx::PostNcDestroy();
}

// 只能在 HWND 仍有效时调用（OnDestroy 里）
void CXiaoGongPDFDlg::UIRelease()
{
	// 清理 PDF 视图控件
	HWND h = m_pdfView.GetSafeHwnd();
	if (h && ::IsWindow(h))
	{
		m_pdfView.SetBitmap(NULL);
	}

	// 清理缩略图列表控件
	HWND hThumb = m_thumbnailList.GetSafeHwnd();
	if (hThumb && ::IsWindow(hThumb))
	{
		// 获取并销毁 ImageList（如果存在）
		CImageList* pImageList = m_thumbnailList.GetImageList(LVSIL_NORMAL);
		if (pImageList)
		{
			// 分离句柄并手动销毁（与 UpdateThumbnails() 中的处理方式一致）
			HIMAGELIST hImageList = pImageList->Detach();
			if (hImageList)
			{
				::ImageList_Destroy(hImageList);
			}
		}
		// 解除关联
		m_thumbnailList.SetImageList(nullptr, LVSIL_NORMAL);
		m_thumbnailList.DeleteAllItems();
	}

	// 清理标签页控件
	HWND hTab = m_tabCtrl.GetSafeHwnd();
	if (hTab && ::IsWindow(hTab))
	{
		m_tabCtrl.DeleteAllItems();
	}

	// 清理快捷键对话框
	if (m_shortcutsDialog.GetSafeHwnd() && ::IsWindow(m_shortcutsDialog.GetSafeHwnd()))
	{
		m_shortcutsDialog.DestroyWindow();
	}
}

// 析构里调用：只释放纯资源（GDI/MuPDF），不动 UI
void CXiaoGongPDFDlg::ResourceRelease()
{
	// 位图句柄
	if (m_hCurrentBitmap) {
		DeleteObject(m_hCurrentBitmap);
		m_hCurrentBitmap = NULL;
	}

	// 拖拽缓存位图
	if (m_hPanPageBitmap) {
		DeleteObject(m_hPanPageBitmap);
		m_hPanPageBitmap = NULL;
	}

	// 缩略图缓存
	for (auto& kv : m_thumbnailCache)
		if (kv.second.hBitmap) DeleteObject(kv.second.hBitmap);
	m_thumbnailCache.clear();

	// MuPDF 对象
	if (m_currentPageObj) {
		fz_drop_page(m_ctx, m_currentPageObj);
		m_currentPageObj = nullptr;
	}
	if (m_doc) {
		fz_drop_document(m_ctx, m_doc);
		m_doc = nullptr;
	}
	if (m_ctx) {
		fz_drop_context(m_ctx);
		m_ctx = nullptr;
	}

	// 页面缓存 - 使用 ClearPageCache() 统一清理
	ClearPageCache();
}


// 缓存机制-按分辨率优化 start
void CXiaoGongPDFDlg::ClearPageCache()
{
	for (auto& kv : m_pageCache)
	{
		if (kv.second.hBitmap)
			DeleteObject(kv.second.hBitmap);
	}
	m_pageCache.clear();
	m_cacheOrder.clear();
}
// end

// ============================================================================
// 自定义打印对话框实现
// ============================================================================

// ============================================================================
// 鼠标拖拽平移功能实现
// ============================================================================

void CXiaoGongPDFDlg::OnLButtonDown(UINT nFlags, CPoint point)
{
	// 检查鼠标是否在PDF视图区域内
	CRect pdfRect;
	m_pdfView.GetWindowRect(&pdfRect);
	ScreenToClient(&pdfRect);

	if (pdfRect.PtInRect(point))
	{
		// ★★★ 将对话框客户区坐标转换为PDF视图客户区坐标（修复拖拽偏移问题）
		CPoint pdfViewPoint = point;
		pdfViewPoint.Offset(-pdfRect.left, -pdfRect.top);

#ifdef _DEBUG
		TRACE(_T("OnLButtonDown: point=(%d,%d), pdfRect=(%d,%d,%d,%d), pdfViewPoint=(%d,%d)\n"),
			point.x, point.y, pdfRect.left, pdfRect.top, pdfRect.right, pdfRect.bottom,
			pdfViewPoint.x, pdfViewPoint.y);
#endif

		// 检查是否点击在滚动条区域
		CRect pdfClientRect;
		m_pdfView.GetClientRect(&pdfClientRect);
		int scrollBarWidth = GetSystemMetrics(SM_CXVSCROLL);

		if (pdfViewPoint.x >= pdfClientRect.right - scrollBarWidth)
		{
			// 在连续滚动模式下，手动处理滚动条拖拽
			if (m_continuousScrollMode)
			{
				m_isDraggingScrollbar = true;
				m_scrollbarDragStartY = point.y;
				m_scrollbarDragStartPos = m_scrollPosition;
				SetCapture();
				return;
			}
			else
			{
				return;
			}
		}

		// ★★★ 在连续滚动模式下，支持自由拖拽（当页面放大后超出视图时）
		if (m_continuousScrollMode && m_doc != nullptr && m_totalPages > 0)
		{
			// 获取视图尺寸
			CRect viewRect;
			m_pdfView.GetClientRect(&viewRect);
			int viewWidth = viewRect.Width() - 40;  // 减去边距
			int viewHeight = viewRect.Height();

			// 在连续滚动模式下，判断是否需要拖拽
			// 通过检查任意页面的渲染尺寸是否超出视图
			bool needDrag = false;

			{
				CSingleLock lock(&m_renderLock, TRUE);
				// 检查第一页作为参考
				fz_page* page = nullptr;
				fz_try(m_ctx)
				{
					page = fz_load_page(m_ctx, m_doc, 0);
					if (page)
					{
						fz_rect bounds = fz_bound_page(m_ctx, page);
						float pageWidth = bounds.x1 - bounds.x0;
						float pageHeight = bounds.y1 - bounds.y0;
						// 使用连续滚动模式的统一缩放比例
						float renderWidth = pageWidth * m_uniformScale;
						float renderHeight = pageHeight * m_uniformScale;

						// ★★★ 修复：添加容差（10像素）避免小数误差，且只在明显超出时启用拖拽
						// 同时检查当前是否已有平移偏移，如果没有偏移则更严格判断
						const int DRAG_TOLERANCE = 10;
						bool widthExceeds = (renderWidth > viewWidth + DRAG_TOLERANCE);
						bool heightExceeds = (renderHeight > viewHeight + DRAG_TOLERANCE);

						// 只有在页面明显超出视图，或者已经有平移偏移时才启用拖拽
						if (widthExceeds || heightExceeds)
						{
							needDrag = true;
						}
						else if (m_panOffset.x != 0 || m_panOffset.y != 0)
						{
							// 如果已经有偏移（之前缩放过），允许拖拽以恢复位置
							needDrag = true;
						}

						fz_drop_page(m_ctx, page);
					}
				}
				fz_catch(m_ctx)
				{
					if (page)
						fz_drop_page(m_ctx, page);
				}
			}

			if (needDrag)
			{
				m_canDrag = true;
				m_isDragging = true;
				m_lastMousePos = pdfViewPoint;  // ★★★ 保存PDF视图客户区坐标
				SetCapture();
				SetCursor(m_hHandCursorGrab);

#ifdef _DEBUG
				TRACE(_T("连续滚动模式拖拽开始: m_lastMousePos=(%d,%d), m_panOffset=(%d,%d)\n"),
					m_lastMousePos.x, m_lastMousePos.y, m_panOffset.x, m_panOffset.y);
#endif
			}
		}
		// ★★★ 分页模式下的拖拽判断
		else if (!m_continuousScrollMode && m_doc != nullptr && m_currentPageObj != nullptr)
		{
			// 获取视图尺寸
			CRect viewRect;
			m_pdfView.GetClientRect(&viewRect);

			// ★★★ 关键修复：从 m_currentPageObj 计算实际页面渲染尺寸
			// 不能使用 m_hCurrentBitmap，因为拖动后它是视图大小的位图（包含背景）
			float pageWidth, pageHeight, scale;
			int renderWidth, renderHeight;

			// ★★★ 在一个锁内完成所有 MuPDF 调用和计算
			{
				CSingleLock lock(&m_renderLock, TRUE);
				fz_rect bounds = fz_bound_page(m_ctx, m_currentPageObj);
				pageWidth = bounds.x1 - bounds.x0;
				pageHeight = bounds.y1 - bounds.y0;

				// 根据当前缩放模式计算实际缩放比例
				switch (m_zoomMode)
				{
				case ZOOM_FIT_WIDTH:
					scale = viewRect.Width() / pageWidth * 0.95f;
					break;
				case ZOOM_FIT_PAGE:
				{
					float scaleX = viewRect.Width() / pageWidth;
					float scaleY = viewRect.Height() / pageHeight;
					scale = min(scaleX, scaleY) * 0.95f;
				}
				break;
				case ZOOM_CUSTOM:
					scale = m_customZoom;
					break;
				default:
				{
					float scaleX = viewRect.Width() / pageWidth;
					float scaleY = viewRect.Height() / pageHeight;
					scale = min(scaleX, scaleY) * 0.95f;
				}
				break;
				}

				// 计算实际渲染尺寸
				renderWidth = (int)(pageWidth * scale);
				renderHeight = (int)(pageHeight * scale);
			}
			// ★★★ 锁在这里释放

			// 只有当页面尺寸大于视图尺寸时才启用拖动
			bool needDrag = (renderWidth > viewRect.Width()) || (renderHeight > viewRect.Height());
			m_canDrag = needDrag;  // ★★★ 更新缓存，供 OnSetCursor 使用

			if (needDrag)
		{
			// ★★★ 在开始拖拽前，先缓存当前PDF页面的渲染结果
			if (!m_hPanPageBitmap && m_ctx && m_currentPageObj)
			{
				// 渲染PDF页面到缓存位图（不带偏移）
				CSingleLock lock(&m_renderLock, TRUE);  // ★★★ 现在可以安全加锁

				CDC* pDC = nullptr;  // ★★★ 提前声明，确保异常时也能释放
				fz_pixmap* pixmap = nullptr;  // ★★★ 提前声明，确保异常时也能释放

				fz_try(m_ctx)
				{
					// 获取页面大小
					fz_rect bounds = fz_bound_page(m_ctx, m_currentPageObj);
					float pageWidth = bounds.x1 - bounds.x0;
					float pageHeight = bounds.y1 - bounds.y0;

					// ★★★ 修复：使用前面计算的正确 scale，而不是硬编码 m_customZoom
					// 这样在 ZOOM_FIT_WIDTH/ZOOM_FIT_PAGE 模式下也能正确渲染

					// 计算渲染尺寸
					int width = (int)(pageWidth * scale);
					int height = (int)(pageHeight * scale);

					// 创建pixmap
					pixmap = fz_new_pixmap(m_ctx, fz_device_rgb(m_ctx), width, height, nullptr, 1);
					fz_clear_pixmap_with_value(m_ctx, pixmap, 0xff);

					// 渲染页面
					fz_matrix ctm = fz_scale(scale, scale);
					fz_device* dev = fz_new_draw_device(m_ctx, ctm, pixmap);
					fz_run_page(m_ctx, m_currentPageObj, dev, fz_identity, nullptr);
					fz_close_device(m_ctx, dev);
					fz_drop_device(m_ctx, dev);
					dev = nullptr;

					// 创建DIB位图
					BITMAPINFO bmi = { 0 };
					bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
					bmi.bmiHeader.biWidth = width;
					bmi.bmiHeader.biHeight = -height;
					bmi.bmiHeader.biPlanes = 1;
					bmi.bmiHeader.biBitCount = 24;
					bmi.bmiHeader.biCompression = BI_RGB;

					pDC = m_pdfView.GetDC();  // ★★★ 获取DC
					BYTE* pbBits = nullptr;
					m_hPanPageBitmap = CreateDIBSection(pDC->GetSafeHdc(), &bmi,
						DIB_RGB_COLORS, (void**)&pbBits, NULL, 0);

					if (m_hPanPageBitmap && pbBits)
					{
						// 复制像素数据
						unsigned char* samples = fz_pixmap_samples(m_ctx, pixmap);
						int stride = (width * 3 + 3) & ~3;
						int n = fz_pixmap_components(m_ctx, pixmap);

						for (int y = 0; y < height; y++) {
							for (int x = 0; x < width; x++) {
								pbBits[y * stride + x * 3 + 0] = samples[(y * width + x) * n + 2];
								pbBits[y * stride + x * 3 + 1] = samples[(y * width + x) * n + 1];
								pbBits[y * stride + x * 3 + 2] = samples[(y * width + x) * n + 0];
							}
						}

						// 应用旋转（如果需要）
						int rotation = GetPageRotation(m_currentPage);
						if (rotation != 0)
						{
							HBITMAP hRotatedBitmap = RotateBitmap(m_hPanPageBitmap, rotation);
							if (hRotatedBitmap)
							{
								DeleteObject(m_hPanPageBitmap);
								m_hPanPageBitmap = hRotatedBitmap;
							}
						}
					}

					// 清理资源
					fz_drop_pixmap(m_ctx, pixmap);
				}
				fz_catch(m_ctx)
				{
					// ★★★ 修复：渲染失败时也要释放 pixmap
					if (pixmap)
						fz_drop_pixmap(m_ctx, pixmap);

					// 渲染失败，清理位图
					if (m_hPanPageBitmap)
					{
						DeleteObject(m_hPanPageBitmap);
						m_hPanPageBitmap = NULL;
					}
				}

				// ★★★ 确保释放DC（无论成功还是失败）
				if (pDC)
					m_pdfView.ReleaseDC(pDC);
			}

			m_isDragging = true;
			m_lastMousePos = pdfViewPoint;  // ★★★ 保存PDF视图客户区坐标
			SetCapture();  // 捕获鼠标
			SetCursor(m_hHandCursorGrab);

#ifdef _DEBUG
			TRACE(_T("分页模式拖拽开始: m_lastMousePos=(%d,%d), m_panOffset=(%d,%d)\n"),
				m_lastMousePos.x, m_lastMousePos.y, m_panOffset.x, m_panOffset.y);
#endif
			}
		}
	}

	CDialogEx::OnLButtonDown(nFlags, point);
}

void CXiaoGongPDFDlg::OnLButtonUp(UINT nFlags, CPoint point)
{
#ifdef _DEBUG
	TRACE(_T("OnLButtonUp: 被调用! point=(%d,%d), m_isDragging=%d, m_isDraggingScrollbar=%d\n"),
		point.x, point.y, m_isDragging, m_isDraggingScrollbar);
#endif

	// 处理滚动条拖拽结束
	if (m_isDraggingScrollbar)
	{
#ifdef _DEBUG
		TRACE(_T("OnLButtonUp: 结束滚动条拖拽\n"));
#endif
		m_isDraggingScrollbar = false;
		ReleaseCapture();
		::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
		return;
	}

	if (m_isDragging)
	{
#ifdef _DEBUG
		TRACE(_T("OnLButtonUp: 结束PDF拖拽，恢复光标\n"));
#endif
		m_isDragging = false;
		ReleaseCapture();
		::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
		SaveCurrentPageZoomState();
	}

	CDialogEx::OnLButtonUp(nFlags, point);
}

void CXiaoGongPDFDlg::OnMouseMove(UINT nFlags, CPoint point)
{
	// 处理滚动条拖拽
	if (m_isDraggingScrollbar && (nFlags & MK_LBUTTON))
	{
		int deltaY = point.y - m_scrollbarDragStartY;

		CRect viewRect;
		m_pdfView.GetClientRect(&viewRect);
		int viewHeight = viewRect.Height();

		int scrollRange = m_totalScrollHeight - viewHeight;
		if (scrollRange < 0) scrollRange = 0;

		float ratio = (float)deltaY / viewHeight;
		int newPos = m_scrollbarDragStartPos + (int)(ratio * scrollRange);

		if (newPos < 0) newPos = 0;
		if (newPos > scrollRange) newPos = scrollRange;

		if (newPos != m_scrollPosition)
		{
			m_scrollPosition = newPos;
			UpdateScrollBar();
			RenderVisiblePages();
		}
		return;
	}

	if (m_isDragging && (nFlags & MK_LBUTTON))
	{
		// ★★★ 将对话框客户区坐标转换为PDF视图客户区坐标（修复拖拽偏移问题）
		CRect pdfRect;
		m_pdfView.GetWindowRect(&pdfRect);
		ScreenToClient(&pdfRect);

		CPoint pdfViewPoint = point;
		pdfViewPoint.Offset(-pdfRect.left, -pdfRect.top);

		// 计算鼠标移动距离（使用PDF视图客户区坐标）
		CPoint delta = pdfViewPoint - m_lastMousePos;

#ifdef _DEBUG
		TRACE(_T("OnMouseMove: point=(%d,%d), pdfViewPoint=(%d,%d), lastPos=(%d,%d), delta=(%d,%d)\n"),
			point.x, point.y, pdfViewPoint.x, pdfViewPoint.y,
			m_lastMousePos.x, m_lastMousePos.y, delta.x, delta.y);
#endif

		m_lastMousePos = pdfViewPoint;

		// ★★★ 连续滚动模式 vs 分页模式：不同的拖拽处理
		if (m_continuousScrollMode)
		{
			// ★★★ 连续滚动模式：垂直拖拽修改滚动位置（浏览多页），水平拖拽修改偏移（平移）
			// 水平方向：修改 m_panOffset.x 用于平移
			m_panOffset.x += delta.x;

			// 垂直方向：修改 m_scrollPosition 用于滚动文档
			int oldScrollPos = m_scrollPosition;
			m_scrollPosition -= delta.y;  // 向下拖拽（delta.y<0）时增加滚动位置

			// 限制滚动范围
			CRect viewRect;
			m_pdfView.GetClientRect(&viewRect);
			int viewHeight = viewRect.Height();
			int scrollRange = m_totalScrollHeight - viewHeight;
			if (scrollRange < 0) scrollRange = 0;
			if (m_scrollPosition < 0) m_scrollPosition = 0;
			if (m_scrollPosition > scrollRange) m_scrollPosition = scrollRange;

#ifdef _DEBUG
			TRACE(_T("  连续滚动: delta.y=%d, scrollPos: %d -> %d, panOffset.x: %d, scrollRange=%d\n"),
				delta.y, oldScrollPos, m_scrollPosition, m_panOffset.x, scrollRange);
#endif

			// 更新滚动条
			UpdateScrollBar();
		}
		else
		{
			// ★★★ 分页模式：水平和垂直都使用 m_panOffset
			CPoint oldPanOffset = m_panOffset;
			m_panOffset.x += delta.x;
			m_panOffset.y += delta.y;

#ifdef _DEBUG
			TRACE(_T("  偏移量更新: old=(%d,%d) -> new=(%d,%d)\n"),
				oldPanOffset.x, oldPanOffset.y, m_panOffset.x, m_panOffset.y);
#endif
		}

		// ★★★ 限制拖拽范围
		if (m_continuousScrollMode && m_doc && m_totalPages > 0)
		{
			CRect viewRect;
			m_pdfView.GetClientRect(&viewRect);
			int viewWidth = viewRect.Width();

			// 获取页面渲染尺寸（用于水平限制）
			float pageRenderWidth = 0;
			fz_page* page = nullptr;
			fz_try(m_ctx)
			{
				page = fz_load_page(m_ctx, m_doc, 0);
				if (page)
				{
					fz_rect bounds = fz_bound_page(m_ctx, page);
					pageRenderWidth = (bounds.x1 - bounds.x0) * m_uniformScale;
					fz_drop_page(m_ctx, page);
				}
			}
			fz_catch(m_ctx)
			{
				if (page)
					fz_drop_page(m_ctx, page);
			}

			// ★★★ 连续滚动模式：只限制水平偏移，垂直方向使用滚动
			if (pageRenderWidth > 0)
			{
				if (pageRenderWidth > viewWidth)
				{
					// 页面超出视图，允许水平拖拽并限制范围
					int minVisibleX = min(100, (int)(pageRenderWidth * 0.2f));
					int maxOffsetX = (int)((pageRenderWidth - viewWidth) / 2 + minVisibleX);
					int minOffsetX = -maxOffsetX;
					if (m_panOffset.x > maxOffsetX) m_panOffset.x = maxOffsetX;
					if (m_panOffset.x < minOffsetX) m_panOffset.x = minOffsetX;
				}
				else
				{
					// 页面未超出视图，不允许水平拖拽，保持居中
					m_panOffset.x = 0;
				}

				// 垂直方向始终为0（使用滚动位置）
				m_panOffset.y = 0;

#ifdef _DEBUG
				TRACE(_T("  连续滚动限制: m_panOffset.x=%d, pageWidth=%.0f, viewWidth=%d\n"),
					m_panOffset.x, pageRenderWidth, viewWidth);
#endif
			}
		}

		// ★★★ 连续滚动模式：拖拽时重新渲染可见页面（应用水平偏移）
		if (m_continuousScrollMode)
		{
			RenderVisiblePages();
		}
		// ★★★ 分页模式：拖拽时快速重绘已缓存的位图
		else if (m_hPanPageBitmap && m_pdfView.GetSafeHwnd())
		{
			// ★★★ 优化：只获取一次视图和位图信息
			CRect viewRect;
			m_pdfView.GetClientRect(&viewRect);
			int viewWidth = viewRect.Width();
			int viewHeight = viewRect.Height();

			BITMAP bm;
			GetObject(m_hPanPageBitmap, sizeof(BITMAP), &bm);

			// ★★★ 限制拖拽边界：确保页面至少有一部分保持可见
			{
				// ★★★ 修复：页面小于视图时不允许拖拽
				if (bm.bmWidth > viewWidth)
				{
					// 页面宽度超出视图，允许水平拖拽并限制范围
					int minVisibleWidth = min(100, bm.bmWidth / 2);
					int maxOffsetX = (viewWidth + bm.bmWidth) / 2 - minVisibleWidth;
					int minOffsetX = -(viewWidth + bm.bmWidth) / 2 + minVisibleWidth;
					if (m_panOffset.x > maxOffsetX) m_panOffset.x = maxOffsetX;
					if (m_panOffset.x < minOffsetX) m_panOffset.x = minOffsetX;
				}
				else
				{
					// 页面宽度未超出视图，不允许水平拖拽
					m_panOffset.x = 0;
				}

				if (bm.bmHeight > viewHeight)
				{
					// 页面高度超出视图，允许垂直拖拽并限制范围
					int minVisibleHeight = min(100, bm.bmHeight / 2);
					int maxOffsetY = (viewHeight + bm.bmHeight) / 2 - minVisibleHeight;
					int minOffsetY = -(viewHeight + bm.bmHeight) / 2 + minVisibleHeight;
					if (m_panOffset.y > maxOffsetY) m_panOffset.y = maxOffsetY;
					if (m_panOffset.y < minOffsetY) m_panOffset.y = minOffsetY;
				}
				else
				{
					// 页面高度未超出视图，不允许垂直拖拽
					m_panOffset.y = 0;
				}

#ifdef _DEBUG
				TRACE(_T("  分页模式限制后偏移量: m_panOffset=(%d,%d), bitmapSize=(%d,%d), viewSize=(%d,%d)\n"),
					m_panOffset.x, m_panOffset.y, bm.bmWidth, bm.bmHeight, viewWidth, viewHeight);
#endif
			}

			// 创建新的显示位图
			CDC* pDC = m_pdfView.GetDC();
			if (!pDC)
				return;

			CDC memDC, targetDC;
			if (!memDC.CreateCompatibleDC(pDC) || !targetDC.CreateCompatibleDC(pDC))
			{
				m_pdfView.ReleaseDC(pDC);
				return;
			}

			// 创建目标位图
			BITMAPINFO newBmi = { 0 };
			newBmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			newBmi.bmiHeader.biWidth = viewWidth;
			newBmi.bmiHeader.biHeight = -viewHeight;
			newBmi.bmiHeader.biPlanes = 1;
			newBmi.bmiHeader.biBitCount = 24;
			newBmi.bmiHeader.biCompression = BI_RGB;

			BYTE* pNewBits = nullptr;
			HBITMAP hNewBitmap = CreateDIBSection(pDC->GetSafeHdc(), &newBmi,
				DIB_RGB_COLORS, (void**)&pNewBits, NULL, 0);

			if (hNewBitmap && pNewBits)
			{
				HBITMAP hOldBmp1 = (HBITMAP)memDC.SelectObject(m_hPanPageBitmap);
				HBITMAP hOldBmp2 = (HBITMAP)targetDC.SelectObject(hNewBitmap);

				// 填充浅灰色背景（让白色PDF页面更明显）
				targetDC.FillSolidRect(0, 0, viewWidth, viewHeight, RGB(240, 240, 240));

				// 计算居中位置（带偏移）
				int xPos = (viewWidth - bm.bmWidth) / 2 + m_panOffset.x;
				int yPos = (viewHeight - bm.bmHeight) / 2 + m_panOffset.y;

				// 快速绘制PDF页面
				targetDC.BitBlt(xPos, yPos, bm.bmWidth, bm.bmHeight, &memDC, 0, 0, SRCCOPY);

				// ★★★ 绘制页面边框（深灰色）
				CPen borderPen(PS_SOLID, 1, RGB(128, 128, 128));
				CPen* pOldPen = targetDC.SelectObject(&borderPen);
				targetDC.SelectStockObject(NULL_BRUSH);  // 不填充
				targetDC.Rectangle(xPos - 1, yPos - 1, xPos + bm.bmWidth + 1, yPos + bm.bmHeight + 1);
				targetDC.SelectObject(pOldPen);

				memDC.SelectObject(hOldBmp1);
				targetDC.SelectObject(hOldBmp2);

				// 更新显示
				if (m_hCurrentBitmap)
					DeleteObject(m_hCurrentBitmap);
				m_hCurrentBitmap = hNewBitmap;
				m_pdfView.SetBitmap(m_hCurrentBitmap);
			}
			else
			{
				// 创建失败，清理
				if (hNewBitmap)
					DeleteObject(hNewBitmap);
			}

			m_pdfView.ReleaseDC(pDC);
		}
	}

	CDialogEx::OnMouseMove(nFlags, point);
}

BOOL CXiaoGongPDFDlg::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
	// 检查鼠标是否在PDF视图区域内
	CPoint point;
	GetCursorPos(&point);

	CRect pdfRect;
	m_pdfView.GetWindowRect(&pdfRect);

	if (pdfRect.PtInRect(point))
	{
		// ★★★ 性能优化：直接使用缓存的 m_canDrag，避免频繁调用 MuPDF 函数
		// m_canDrag 在 OnLButtonDown 和 RenderPage 中更新
		if (m_canDrag)
		{
			if (m_isDragging)
			{
				SetCursor(m_hHandCursorGrab);
				return TRUE;
			}
			else
			{
				// 拖拽结束后使用箭头光标
				SetCursor(::LoadCursor(nullptr, IDC_ARROW));
				return TRUE;
			}
		}
	}

	return CDialogEx::OnSetCursor(pWnd, nHitTest, message);
}

void CXiaoGongPDFDlg::ResetPanOffset()
{
	m_panOffset.x = 0;
	m_panOffset.y = 0;

	// 清理拖拽缓存位图
	if (m_hPanPageBitmap)
	{
		DeleteObject(m_hPanPageBitmap);
		m_hPanPageBitmap = NULL;
	}
}

// ============================================================================
// 自定义打印对话框实现
// ============================================================================

BEGIN_MESSAGE_MAP(CCustomPrintDlg, CDialogEx)
	ON_CBN_SELCHANGE(IDC_PRINTER_COMBO, &CCustomPrintDlg::OnPrinterChange)
	ON_BN_CLICKED(IDC_PRINT_ALL, &CCustomPrintDlg::OnPrintAllRadio)
	ON_BN_CLICKED(IDC_PRINT_RANGE, &CCustomPrintDlg::OnPrintRangeRadio)
	ON_BN_CLICKED(IDC_PRINTER_PROPERTIES, &CCustomPrintDlg::OnPrinterProperties)
END_MESSAGE_MAP()

CCustomPrintDlg::CCustomPrintDlg(int totalPages, CWnd* pParent)
	: CDialogEx(IDD_PRINT_DIALOG, pParent)
	, m_totalPages(totalPages)
	, m_bPrintAll(TRUE)
	, m_pageFrom(1)
	, m_pageTo(totalPages)
	, m_copies(1)
	, m_bCollate(TRUE)
	, m_bDuplex(FALSE)
{
}

CCustomPrintDlg::~CCustomPrintDlg()
{
}

void CCustomPrintDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_PRINTER_COMBO, m_printerCombo);
	DDX_Control(pDX, IDC_PRINT_ALL, m_printAllRadio);
	DDX_Control(pDX, IDC_PRINT_RANGE, m_printRangeRadio);
	DDX_Control(pDX, IDC_PAGE_FROM, m_pageFromEdit);
	DDX_Control(pDX, IDC_PAGE_TO, m_pageToEdit);
	DDX_Control(pDX, IDC_COPIES, m_copiesEdit);
	DDX_Control(pDX, IDC_COLLATE, m_collateCheck);
	DDX_Control(pDX, IDC_DUPLEX, m_duplexCheck);
}

BOOL CCustomPrintDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// 枚举打印机
	EnumeratePrinters();

	// 设置默认值
	m_printAllRadio.SetCheck(BST_CHECKED);
	m_printRangeRadio.SetCheck(BST_UNCHECKED);

	CString str;
	str.Format(_T("%d"), m_pageFrom);
	m_pageFromEdit.SetWindowText(str);
	str.Format(_T("%d"), m_pageTo);
	m_pageToEdit.SetWindowText(str);
	str.Format(_T("%d"), m_copies);
	m_copiesEdit.SetWindowText(str);

	m_collateCheck.SetCheck(BST_CHECKED);
	m_duplexCheck.SetCheck(BST_UNCHECKED);

	// 更新控件状态
	UpdateControls();

	return TRUE;
}

void CCustomPrintDlg::EnumeratePrinters()
{
	// 枚举系统中所有打印机
	DWORD dwNeeded = 0;
	DWORD dwReturned = 0;

	// 第一次调用获取需要的缓冲区大小
	EnumPrinters(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &dwNeeded, &dwReturned);

	if (dwNeeded > 0)
	{
		BYTE* pPrinterInfo = new BYTE[dwNeeded];
		if (EnumPrinters(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, pPrinterInfo, dwNeeded, &dwNeeded, &dwReturned))
		{
			PRINTER_INFO_2* pInfo = (PRINTER_INFO_2*)pPrinterInfo;
			for (DWORD i = 0; i < dwReturned; i++)
			{
				m_printerCombo.AddString(pInfo[i].pPrinterName);
			}
		}
		delete[] pPrinterInfo;
	}

	// 获取默认打印机
	TCHAR szDefaultPrinter[256] = {0};
	DWORD dwSize = 256;
	if (GetDefaultPrinter(szDefaultPrinter, &dwSize))
	{
		int index = m_printerCombo.FindStringExact(-1, szDefaultPrinter);
		if (index != CB_ERR)
		{
			m_printerCombo.SetCurSel(index);
		}
		else if (m_printerCombo.GetCount() > 0)
		{
			m_printerCombo.SetCurSel(0);
		}
	}
	else if (m_printerCombo.GetCount() > 0)
	{
		m_printerCombo.SetCurSel(0);
	}
}

void CCustomPrintDlg::UpdateControls()
{
	// 根据"全部"/"页码范围"单选按钮状态，启用/禁用页码输入框
	BOOL bEnableRange = (m_printRangeRadio.GetCheck() == BST_CHECKED);
	m_pageFromEdit.EnableWindow(bEnableRange);
	m_pageToEdit.EnableWindow(bEnableRange);
}

void CCustomPrintDlg::OnPrinterChange()
{
	// 打印机选择改变
	int index = m_printerCombo.GetCurSel();
	if (index != CB_ERR)
	{
		m_printerCombo.GetLBText(index, m_printerName);
	}
}

void CCustomPrintDlg::OnPrintAllRadio()
{
	UpdateControls();
}

void CCustomPrintDlg::OnPrintRangeRadio()
{
	UpdateControls();
}

void CCustomPrintDlg::OnPrinterProperties()
{
	// 打开打印机属性对话框
	int index = m_printerCombo.GetCurSel();
	if (index == CB_ERR)
	{
		MessageBox(_T("请先选择打印机"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	CString printerName;
	m_printerCombo.GetLBText(index, printerName);

	// 打开打印机属性
	HANDLE hPrinter = NULL;
	if (OpenPrinter((LPTSTR)(LPCTSTR)printerName, &hPrinter, NULL))
	{
		PRINTER_DEFAULTS pd = {0};
		pd.DesiredAccess = PRINTER_ACCESS_USE;

		LONG result = DocumentProperties(m_hWnd, hPrinter, (LPTSTR)(LPCTSTR)printerName, NULL, NULL, 0);
		if (result > 0)
		{
			LPDEVMODE pDevMode = (LPDEVMODE)GlobalAlloc(GPTR, result);
			if (pDevMode)
			{
				result = DocumentProperties(m_hWnd, hPrinter, (LPTSTR)(LPCTSTR)printerName, pDevMode, NULL, DM_OUT_BUFFER);
				if (result == IDOK)
				{
					result = DocumentProperties(m_hWnd, hPrinter, (LPTSTR)(LPCTSTR)printerName, pDevMode, pDevMode, DM_IN_BUFFER | DM_PROMPT);
				}
				GlobalFree(pDevMode);
			}
		}
		ClosePrinter(hPrinter);
	}
}

void CCustomPrintDlg::OnOK()
{
	// 获取打印机名称
	int index = m_printerCombo.GetCurSel();
	if (index == CB_ERR)
	{
		MessageBox(_T("请选择打印机"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}
	m_printerCombo.GetLBText(index, m_printerName);

	// 获取打印范围
	m_bPrintAll = (m_printAllRadio.GetCheck() == BST_CHECKED);

	if (!m_bPrintAll)
	{
		CString str;
		m_pageFromEdit.GetWindowText(str);
		m_pageFrom = _ttoi(str);
		m_pageToEdit.GetWindowText(str);
		m_pageTo = _ttoi(str);

		// 验证页码范围
		if (m_pageFrom < 1 || m_pageFrom > m_totalPages ||
			m_pageTo < 1 || m_pageTo > m_totalPages ||
			m_pageFrom > m_pageTo)
		{
			CString msg;
			msg.Format(_T("页码范围无效！请输入 1 到 %d 之间的页码。"), m_totalPages);
			MessageBox(msg, _T("提示"), MB_OK | MB_ICONWARNING);
			return;
		}
	}
	else
	{
		m_pageFrom = 1;
		m_pageTo = m_totalPages;
	}

	// 获取份数
	CString str;
	m_copiesEdit.GetWindowText(str);
	m_copies = _ttoi(str);
	if (m_copies < 1)
	{
		MessageBox(_T("份数必须大于0"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	// 获取复选框状态
	m_bCollate = (m_collateCheck.GetCheck() == BST_CHECKED);
	m_bDuplex = (m_duplexCheck.GetCheck() == BST_CHECKED);

	CDialogEx::OnOK();
}

// ============================================================================
// 打印单个页面的辅助函数
// ============================================================================
void CXiaoGongPDFDlg::PrintSinglePage(CDC& dcPrint, int pageNum, int horRes, int verRes, BOOL bDuplex)
{
	// 开始新页
	dcPrint.StartPage();

	// 加载PDF页面
	CSingleLock lock(&m_renderLock, TRUE);
	fz_page* page = fz_load_page(m_ctx, m_doc, pageNum);
	if (page)
	{
		// 获取页面大小
		fz_rect bounds = fz_bound_page(m_ctx, page);
		float pageWidth = bounds.x1 - bounds.x0;
		float pageHeight = bounds.y1 - bounds.y0;

		// 计算缩放比例以适应打印机页面
		float scaleX = (float)horRes / pageWidth;
		float scaleY = (float)verRes / pageHeight;
		float scale = min(scaleX, scaleY);

		// 计算渲染尺寸
		int renderWidth = (int)(pageWidth * scale);
		int renderHeight = (int)(pageHeight * scale);

		// 创建pixmap
		fz_pixmap* pixmap = fz_new_pixmap(m_ctx, fz_device_rgb(m_ctx), renderWidth, renderHeight, nullptr, 1);
		fz_clear_pixmap_with_value(m_ctx, pixmap, 0xff);

		// 渲染页面
		fz_matrix ctm = fz_scale(scale, scale);
		fz_device* dev = fz_new_draw_device(m_ctx, ctm, pixmap);
		fz_run_page(m_ctx, page, dev, fz_identity, nullptr);
		fz_close_device(m_ctx, dev);
		fz_drop_device(m_ctx, dev);

		// 创建DIB用于打印
		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = renderWidth;
		bmi.bmiHeader.biHeight = -renderHeight;  // 负值表示自上而下
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		// 创建DIB位图
		BYTE* pbBits = nullptr;
		HBITMAP hBitmap = CreateDIBSection(dcPrint.GetSafeHdc(), &bmi, DIB_RGB_COLORS, (void**)&pbBits, NULL, 0);

		if (hBitmap && pbBits)
		{
			// 复制像素数据
			unsigned char* samples = fz_pixmap_samples(m_ctx, pixmap);
			int stride = (renderWidth * 3 + 3) & ~3;
			int n = fz_pixmap_components(m_ctx, pixmap);

			for (int y = 0; y < renderHeight; y++) {
				for (int x = 0; x < renderWidth; x++) {
					pbBits[y * stride + x * 3 + 0] = samples[(y * renderWidth + x) * n + 2];  // B
					pbBits[y * stride + x * 3 + 1] = samples[(y * renderWidth + x) * n + 1];  // G
					pbBits[y * stride + x * 3 + 2] = samples[(y * renderWidth + x) * n + 0];  // R
				}
			}

			// 绘制到打印机DC
			CDC memDC;
			memDC.CreateCompatibleDC(&dcPrint);
			HBITMAP oldBitmap = (HBITMAP)memDC.SelectObject(hBitmap);

			// 居中打印
			int xOffset = (horRes - renderWidth) / 2;
			int yOffset = (verRes - renderHeight) / 2;

			dcPrint.StretchBlt(xOffset, yOffset, renderWidth, renderHeight,
				&memDC, 0, 0, renderWidth, renderHeight, SRCCOPY);

			memDC.SelectObject(oldBitmap);
			DeleteObject(hBitmap);
		}

		// 清理资源
		fz_drop_pixmap(m_ctx, pixmap);
		fz_drop_page(m_ctx, page);
	}

	// 结束当前页
	dcPrint.EndPage();
}

// ============================================================================
// 连续滚动模式实现
// ============================================================================

// 计算所有页面的位置和高度（使用统一缩放比例确保宽度一致）
void CXiaoGongPDFDlg::CalculatePagePositions()
{
	if (!m_doc || m_totalPages <= 0)
		return;

	m_pageYPositions.clear();
	m_pageHeights.clear();
	m_pageYPositions.resize(m_totalPages);
	m_pageHeights.resize(m_totalPages);

	// 获取预览区域宽度
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	int viewWidth = viewRect.Width() - 40;  // 留出左右边距各20像素

	// ★★★ 第一步：遍历所有页面，找到最大宽度（用于统一缩放）
	float maxPageWidth = 0.0f;
	for (int i = 0; i < m_totalPages; i++)
	{
		fz_page* page = nullptr;
		fz_try(m_ctx)
		{
			page = fz_load_page(m_ctx, m_doc, i);
			if (page)
			{
				fz_rect bounds = fz_bound_page(m_ctx, page);
				float pageWidth = bounds.x1 - bounds.x0;
				float pageHeight = bounds.y1 - bounds.y0;

				// 根据旋转角度调整
				int rotation = GetPageRotation(i);
				if (rotation == 90 || rotation == 270)
				{
					float temp = pageWidth;
					pageWidth = pageHeight;
					pageHeight = temp;
				}

				if (pageWidth > maxPageWidth)
					maxPageWidth = pageWidth;

				fz_drop_page(m_ctx, page);
			}
		}
		fz_catch(m_ctx)
		{
			if (page)
				fz_drop_page(m_ctx, page);
		}
	}

	// ★★★ 计算统一的缩放比例（基于最大宽度）并保存到成员变量
	// 基准缩放 = 适应宽度的缩放
	float baseScale = (maxPageWidth > 0) ? ((float)viewWidth / maxPageWidth * 0.95f) : 1.0f;
	// 实际缩放 = 基准缩放 × 自定义缩放倍数（这样可以支持放大/缩小功能）
	m_uniformScale = baseScale * m_customZoom;

#ifdef _DEBUG
	TRACE(_T("连续滚动模式: 最大页面宽度=%.2f, 基准缩放=%.4f, customZoom=%.2f, 统一缩放比例=%.4f\n"),
		maxPageWidth, baseScale, m_customZoom, m_uniformScale);
#endif

	// ★★★ 第二步：使用统一的缩放比例计算所有页面的高度
	// ★★★ 修复：第一页从 PAGE_SPACING 开始，确保顶部有灰色间隔
	int currentY = PAGE_SPACING;
	for (int i = 0; i < m_totalPages; i++)
	{
		m_pageYPositions[i] = currentY;

		fz_page* page = nullptr;
		fz_try(m_ctx)
		{
			page = fz_load_page(m_ctx, m_doc, i);
			if (page)
			{
				fz_rect bounds = fz_bound_page(m_ctx, page);
				float pageWidth = bounds.x1 - bounds.x0;
				float pageHeight = bounds.y1 - bounds.y0;

				// 根据旋转角度调整
				int rotation = GetPageRotation(i);
				if (rotation == 90 || rotation == 270)
				{
					float temp = pageWidth;
					pageWidth = pageHeight;
					pageHeight = temp;
				}

				// ★★★ 使用统一的缩放比例计算高度
				int scaledHeight = (int)(pageHeight * m_uniformScale);
				m_pageHeights[i] = scaledHeight;

				currentY += scaledHeight + PAGE_SPACING;

				fz_drop_page(m_ctx, page);
			}
		}
		fz_catch(m_ctx)
		{
			if (page)
				fz_drop_page(m_ctx, page);
			m_pageHeights[i] = 0;
		}
	}

	// ★★★ 修复：保留最后一页的底部间距，确保底部有灰色间隔
	m_totalScrollHeight = currentY;

#ifdef _DEBUG
	TRACE(_T("连续滚动: 总高度=%d\n"), m_totalScrollHeight);
#endif
}

// 渲染可见的页面
void CXiaoGongPDFDlg::RenderVisiblePages()
{
	// 添加更严格的边界检查，防止切换PDF时vector越界
	if (!m_doc || m_totalPages <= 0 || m_pageYPositions.empty())
		return;

	// 确保vector大小与总页数匹配，避免切换文档时的时序问题导致越界
	if ((int)m_pageYPositions.size() < m_totalPages || (int)m_pageHeights.size() < m_totalPages)
	{
#ifdef _DEBUG
		TRACE(_T("RenderVisiblePages: vector大小不匹配，跳过渲染。m_totalPages=%d, m_pageYPositions.size()=%d, m_pageHeights.size()=%d\n"),
			m_totalPages, (int)m_pageYPositions.size(), (int)m_pageHeights.size());
#endif
		return;
	}

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	int viewHeight = viewRect.Height();
	int viewWidth = viewRect.Width();

	// 计算可见范围
	int visibleTop = m_scrollPosition;
	int visibleBottom = m_scrollPosition + viewHeight;

#ifdef _DEBUG
	TRACE(_T("RenderVisiblePages: viewSize=(%d,%d), visibleTop=%d\n"),
		viewWidth, viewHeight, visibleTop);
#endif

	// 创建内存DC和位图
	CDC* pDC = m_pdfView.GetDC();
	CDC memDC;
	memDC.CreateCompatibleDC(pDC);

	CBitmap bmp;
	bmp.CreateCompatibleBitmap(pDC, viewWidth, viewHeight);
	CBitmap* pOldBmp = memDC.SelectObject(&bmp);

	// 填充背景色（灰色）
	memDC.FillSolidRect(0, 0, viewWidth, viewHeight, RGB(128, 128, 128));

	// 渲染所有可见的页面
	for (int i = 0; i < m_totalPages; i++)
	{
		int pageY = m_pageYPositions[i];
		int pageHeight = m_pageHeights[i];

		// 检查页面是否可见
		if (pageY + pageHeight < visibleTop || pageY > visibleBottom)
			continue;

		// 渲染这个页面
		fz_page* page = nullptr;
		fz_pixmap* pixmap = nullptr;
		fz_device* dev = nullptr;

		fz_try(m_ctx)
		{
			page = fz_load_page(m_ctx, m_doc, i);
			if (!page)
				continue;

			fz_rect bounds = fz_bound_page(m_ctx, page);
			float pageWidth = bounds.x1 - bounds.x0;
			float pageHeightF = bounds.y1 - bounds.y0;

#ifdef _DEBUG
			if (i == 0)  // 只打印第0页
				TRACE(_T("  Page bounds: x0=%.1f, y0=%.1f, x1=%.1f, y1=%.1f\n"),
					bounds.x0, bounds.y0, bounds.x1, bounds.y1);
#endif

			// 根据旋转角度调整
			int rotation = GetPageRotation(i);
			if (rotation == 90 || rotation == 270)
			{
				float temp = pageWidth;
				pageWidth = pageHeightF;
				pageHeightF = temp;
			}

			// ★★★ 使用统一的缩放比例
			int width = (int)(pageWidth * m_uniformScale);
			int height = (int)(pageHeightF * m_uniformScale);

			// 创建pixmap
			pixmap = fz_new_pixmap(m_ctx, fz_device_rgb(m_ctx), width, height, nullptr, 1);
			fz_clear_pixmap_with_value(m_ctx, pixmap, 0xff);

			// 渲染页面
			fz_matrix ctm = fz_scale(m_uniformScale, m_uniformScale);

			// 应用旋转
			if (rotation != 0)
			{
				fz_matrix rotate = fz_rotate((float)rotation);
				if (rotation == 90)
					rotate = fz_pre_translate(rotate, 0, -(bounds.y1 - bounds.y0));
				else if (rotation == 180)
					rotate = fz_pre_translate(rotate, -(bounds.x1 - bounds.x0), -(bounds.y1 - bounds.y0));
				else if (rotation == 270)
					rotate = fz_pre_translate(rotate, -(bounds.x1 - bounds.x0), 0);
				ctm = fz_concat(rotate, ctm);
			}

			dev = fz_new_draw_device(m_ctx, ctm, pixmap);
			fz_run_page(m_ctx, page, dev, fz_identity, nullptr);
			fz_close_device(m_ctx, dev);
			fz_drop_device(m_ctx, dev);
			dev = nullptr;

			// 创建DIB位图
			BITMAPINFO bmi = { 0 };
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = width;
			bmi.bmiHeader.biHeight = -height;
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 24;
			bmi.bmiHeader.biCompression = BI_RGB;

			BYTE* pbBits = nullptr;
			HBITMAP hBitmap = CreateDIBSection(memDC.GetSafeHdc(), &bmi,
				DIB_RGB_COLORS, (void**)&pbBits, NULL, 0);

			if (hBitmap && pbBits)
			{
				// 复制像素数据
				unsigned char* samples = fz_pixmap_samples(m_ctx, pixmap);
				int stride = (width * 3 + 3) & ~3;
				int n = fz_pixmap_components(m_ctx, pixmap);

				for (int y = 0; y < height; y++) {
					for (int x = 0; x < width; x++) {
						pbBits[y * stride + x * 3 + 0] = samples[(y * width + x) * n + 2];
						pbBits[y * stride + x * 3 + 1] = samples[(y * width + x) * n + 1];
						pbBits[y * stride + x * 3 + 2] = samples[(y * width + x) * n + 0];
					}
				}

				// 在内存DC中绘制这个页面
				CDC pageDC;
				pageDC.CreateCompatibleDC(&memDC);
				HBITMAP oldPageBmp = (HBITMAP)pageDC.SelectObject(hBitmap);

				int destY = pageY - visibleTop + m_panOffset.y;  // 应用垂直拖拽偏移
				int destX = (viewWidth - width) / 2 + m_panOffset.x;  // 居中显示并应用水平拖拽偏移

#ifdef _DEBUG
				TRACE(_T("  Page %d: pageY=%d, size=(%d,%d), dest=(%d,%d)\n"),
					i, pageY, width, height, destX, destY);
#endif

				memDC.BitBlt(destX, destY, width, height, &pageDC, 0, 0, SRCCOPY);

				// 绘制搜索高亮（如果有搜索结果）
				if (!m_searchMatches.empty())
				{
					// 获取原始页面尺寸（未旋转）
					float origPageWidth = bounds.x1 - bounds.x0;
					float origPageHeight = bounds.y1 - bounds.y0;

					for (size_t matchIdx = 0; matchIdx < m_searchMatches.size(); matchIdx++)
					{
						if (m_searchMatches[matchIdx].pageNumber != i)
							continue;

						// 将quad坐标转换为屏幕坐标
						fz_quad quad = m_searchMatches[matchIdx].quad;

						// 计算高亮矩形（在原始PDF坐标系中）
						float minX = min(min(quad.ul.x, quad.ur.x), min(quad.ll.x, quad.lr.x));
						float maxX = max(max(quad.ul.x, quad.ur.x), max(quad.ll.x, quad.lr.x));
						float minY = min(min(quad.ul.y, quad.ur.y), min(quad.ll.y, quad.lr.y));
						float maxY = max(max(quad.ul.y, quad.ur.y), max(quad.ll.y, quad.lr.y));

#ifdef _DEBUG
						TRACE(_T("  quad corners: ul=(%.1f,%.1f) ur=(%.1f,%.1f) ll=(%.1f,%.1f) lr=(%.1f,%.1f)\n"),
							quad.ul.x, quad.ul.y, quad.ur.x, quad.ur.y,
							quad.ll.x, quad.ll.y, quad.lr.x, quad.lr.y);
#endif

						// 转换为屏幕坐标（PDF坐标系原点在左下，屏幕坐标系原点在左上）
						// ★★★ 修复：MuPDF返回的quad坐标是相对于页面bounds的绝对坐标，需要减去bounds.x0
						// 且Y轴变换应使用bounds.y1（PDF坐标系的顶部）而非origPageHeight
						int x1 = destX + (int)((minX - bounds.x0) * m_uniformScale);
						int y1 = destY + (int)(minY * m_uniformScale);
						int x2 = destX + (int)((maxX - bounds.x0) * m_uniformScale);
						int y2 = destY + (int)(maxY * m_uniformScale);

						CRect highlightRect(x1, y1, x2, y2);

#ifdef _DEBUG
						TRACE(_T("Page %d, Match %d: destX=%d, destY=%d, scale=%.3f, origH=%.1f\n"),
							i, (int)matchIdx, destX, destY, m_uniformScale, origPageHeight);
						TRACE(_T("  quad: minX=%.1f, maxX=%.1f, minY=%.1f, maxY=%.1f\n"),
							minX, maxX, minY, maxY);
						TRACE(_T("  rect: (%d,%d,%d,%d)\n"), x1, y1, x2, y2);
#endif

						// 选择颜色
						COLORREF fillColor;
						if ((int)matchIdx == m_currentMatchIndex)
							fillColor = RGB(255, 150, 0);  // 当前匹配项用橙色
						else
							fillColor = RGB(255, 200, 0);  // 其他匹配项用深黄色（更明显）

						// 创建临时DC用于AlphaBlend
					CDC tempDC;
					tempDC.CreateCompatibleDC(&memDC);
					CBitmap tempBmp;
					tempBmp.CreateCompatibleBitmap(&memDC, highlightRect.Width(), highlightRect.Height());
					CBitmap* pOldTempBmp = tempDC.SelectObject(&tempBmp);
					tempDC.FillSolidRect(0, 0, highlightRect.Width(), highlightRect.Height(), fillColor);

					// 使用AlphaBlend提高不透明度
					BLENDFUNCTION bf;
					bf.BlendOp = AC_SRC_OVER;
					bf.BlendFlags = 0;
					bf.SourceConstantAlpha = ((int)matchIdx == m_currentMatchIndex) ? 200 : 180;
					bf.AlphaFormat = 0;
					memDC.AlphaBlend(highlightRect.left, highlightRect.top,
						highlightRect.Width(), highlightRect.Height(),
						&tempDC, 0, 0, highlightRect.Width(), highlightRect.Height(), bf);
					tempDC.SelectObject(pOldTempBmp);

						// 绘制边框使其更明显
						CPen borderPen(PS_SOLID, 1, RGB(255, 140, 0));
						CPen* pOldPen = memDC.SelectObject(&borderPen);
						memDC.SelectStockObject(NULL_BRUSH);
						memDC.Rectangle(highlightRect);
						memDC.SelectObject(pOldPen);
					}
				}

				pageDC.SelectObject(oldPageBmp);
				DeleteObject(hBitmap);
			}

			fz_drop_pixmap(m_ctx, pixmap);
			fz_drop_page(m_ctx, page);
		}
		fz_catch(m_ctx)
		{
			if (dev) fz_drop_device(m_ctx, dev);
			if (pixmap) fz_drop_pixmap(m_ctx, pixmap);
			if (page) fz_drop_page(m_ctx, page);
		}
	}

	// ★★★ 保存位图并设置到控件（这样控件可以自动处理重绘）
	// 先清理旧的位图
	if (m_hContinuousViewBitmap)
	{
		DeleteObject(m_hContinuousViewBitmap);
		m_hContinuousViewBitmap = NULL;
	}

	// 将内存DC中的位图分离并保存
	memDC.SelectObject(pOldBmp);

	// 创建位图的副本
	m_hContinuousViewBitmap = (HBITMAP)CopyImage(bmp.GetSafeHandle(), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

	// 设置位图到控件（控件会自动处理重绘）
	m_pdfView.SetBitmap(m_hContinuousViewBitmap);

#ifdef _DEBUG
	CRect ctrlRect;
	m_pdfView.GetClientRect(&ctrlRect);
	TRACE(_T("m_pdfView control size: (%d,%d), bitmap size: (%d,%d)\n"),
		ctrlRect.Width(), ctrlRect.Height(), viewWidth, viewHeight);

	// 保存位图到文件用于调试
	if (!m_searchMatches.empty())
	{
		CDC tempDC;
		tempDC.CreateCompatibleDC(NULL);
		HBITMAP oldBmp = (HBITMAP)tempDC.SelectObject(m_hContinuousViewBitmap);

		BITMAP bmpInfo;
		GetObject(m_hContinuousViewBitmap, sizeof(BITMAP), &bmpInfo);

		CImage img;
		img.Attach(m_hContinuousViewBitmap);
		img.Save(_T("D:\\debug_highlight.png"));
		img.Detach();

		tempDC.SelectObject(oldBmp);
		TRACE(_T("Saved debug bitmap to D:\\debug_highlight.png\n"));
	}
#endif

	m_pdfView.ReleaseDC(pDC);

	// 更新当前页（根据滚动位置）
	int newCurrentPage = GetPageAtPosition(m_scrollPosition + viewHeight / 2);
	if (newCurrentPage != m_currentPage && newCurrentPage >= 0 && newCurrentPage < m_totalPages)
	{
		m_currentPage = newCurrentPage;
		UpdatePageControls();
		HighlightCurrentThumbnail();
	}
}

// 垂直滚动条消息处理
void CXiaoGongPDFDlg::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	if (!m_continuousScrollMode)
	{
		CDialogEx::OnVScroll(nSBCode, nPos, pScrollBar);
		return;
	}

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	int pageSize = viewRect.Height();

	int newPos = m_scrollPosition;

	switch (nSBCode)
	{
	case SB_LINEUP:
		newPos -= 20;
		break;
	case SB_LINEDOWN:
		newPos += 20;
		break;
	case SB_PAGEUP:
		newPos -= pageSize;
		break;
	case SB_PAGEDOWN:
		newPos += pageSize;
		break;
	case SB_THUMBPOSITION:
	case SB_THUMBTRACK:
		newPos = nPos;
		break;
	}

	// 限制范围
	int maxScroll = m_totalScrollHeight - viewRect.Height();
	if (maxScroll < 0) maxScroll = 0;
	if (newPos < 0) newPos = 0;
	if (newPos > maxScroll) newPos = maxScroll;

	if (newPos != m_scrollPosition)
	{
		m_scrollPosition = newPos;
		UpdateScrollBar();
		RenderVisiblePages();
	}

	CDialogEx::OnVScroll(nSBCode, nPos, pScrollBar);
}

// 更新滚动条
void CXiaoGongPDFDlg::UpdateScrollBar()
{
	if (!m_continuousScrollMode)
		return;

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);

	SCROLLINFO si = { 0 };
	si.cbSize = sizeof(SCROLLINFO);
	si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;  // ★★★ 添加 SIF_DISABLENOSCROLL 确保滚动条始终可见
	si.nMin = 0;
	si.nMax = m_totalScrollHeight;
	si.nPage = viewRect.Height();
	si.nPos = m_scrollPosition;

	::SetScrollInfo(m_pdfView.GetSafeHwnd(), SB_VERT, &si, TRUE);
}

// 获取指定位置的页面索引
int CXiaoGongPDFDlg::GetPageAtPosition(int yPos)
{
	// 添加边界检查，防止vector越界
	if ((int)m_pageYPositions.size() < m_totalPages || (int)m_pageHeights.size() < m_totalPages)
		return 0;

	for (int i = 0; i < m_totalPages; i++)
	{
		if (yPos >= m_pageYPositions[i] && yPos < m_pageYPositions[i] + m_pageHeights[i])
			return i;
	}
	return 0;
}

// ============================================================================
// 搜索功能实现
// ============================================================================

// 清除搜索结果
void CXiaoGongPDFDlg::ClearSearchResults()
{
	m_searchMatches.clear();
	m_currentMatchIndex = -1;
	m_searchKeyword.Empty();

	// 刷新当前页面以移除高亮
	if (m_doc && m_currentPage >= 0)
	{
		m_pdfView.Invalidate();
	}
}

// 搜索PDF文档
void CXiaoGongPDFDlg::SearchPDF(const CString& keyword, bool caseSensitive)
{
	if (!m_doc || m_totalPages <= 0 || keyword.IsEmpty())
	{
		MessageBox(_T("请输入搜索关键词"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	// 清除之前的搜索结果
	ClearSearchResults();

	// 保存搜索参数
	m_searchKeyword = keyword;
	m_searchCaseSensitive = caseSensitive;

	// 将CString转换为UTF-8
	int utf8Len = WideCharToMultiByte(CP_UTF8, 0, keyword, -1, NULL, 0, NULL, NULL);
	char* utf8Keyword = new char[utf8Len];
	WideCharToMultiByte(CP_UTF8, 0, keyword, -1, utf8Keyword, utf8Len, NULL, NULL);

	// 如果不区分大小写，转换为小写
	char* searchKeyword = utf8Keyword;
	if (!caseSensitive)
	{
		searchKeyword = new char[utf8Len];
		strcpy(searchKeyword, utf8Keyword);
		_strlwr_s(searchKeyword, utf8Len);  // 转换为小写
	}

	// 遍历所有页面
	for (int i = 0; i < m_totalPages; i++)
	{
		fz_page* page = nullptr;
		fz_stext_page* stext = nullptr;

		fz_try(m_ctx)
		{
			// 加载页面
			page = fz_load_page(m_ctx, m_doc, i);

			// 提取文本
			stext = fz_new_stext_page_from_page(m_ctx, page, nullptr);

			// 搜索关键词
			fz_quad quads[512];
			int hit_marks[512];  // 用于接收匹配标记
			int hit_count = fz_search_stext_page(m_ctx, stext, searchKeyword, hit_marks, quads, 512);

#ifdef _DEBUG
			if (hit_count > 0)
			{
				TRACE(_T("SearchPDF: 在第 %d 页找到 %d 个匹配项\n"), i, hit_count);
			}
#endif

			// 保存匹配项
			for (int j = 0; j < hit_count; j++)
			{
				SearchMatch match;
				match.pageNumber = i;
				match.quad = quads[j];
				match.context = _T("");  // 暂时不提取上下文
				m_searchMatches.push_back(match);

#ifdef _DEBUG
				TRACE(_T("  Match %d: quad=(%f,%f,%f,%f,%f,%f,%f,%f)\n"), j,
					quads[j].ul.x, quads[j].ul.y,
					quads[j].ur.x, quads[j].ur.y,
					quads[j].ll.x, quads[j].ll.y,
					quads[j].lr.x, quads[j].lr.y);
#endif
			}
		}
		fz_catch(m_ctx)
		{
			// 搜索出错，跳过当前页
#ifdef _DEBUG
			TRACE(_T("SearchPDF: 搜索第 %d 页时出错\n"), i);
#endif
		}

		// 清理资源
		if (stext)
			fz_drop_stext_page(m_ctx, stext);
		if (page)
			fz_drop_page(m_ctx, page);
	}

	// 清理内存
	if (!caseSensitive && searchKeyword != utf8Keyword)
		delete[] searchKeyword;
	delete[] utf8Keyword;

	// 显示搜索结果
	CString msg;
	if (!m_searchMatches.empty())
	{
		msg.Format(_T("找到 %d 个匹配项"), (int)m_searchMatches.size());
		m_statusBar.SetWindowText(msg);

		// 跳转到第一个匹配项
		m_currentMatchIndex = 0;
		GoToPage(m_searchMatches[0].pageNumber);
	}
	else
	{
		MessageBox(_T("未找到匹配项"), _T("搜索结果"), MB_OK | MB_ICONINFORMATION);
		m_statusBar.SetWindowText(_T("未找到匹配项"));
	}
}

// 跳转到下一个匹配项
void CXiaoGongPDFDlg::GoToNextMatch()
{
	if (m_searchMatches.empty())
	{
		MessageBox(_T("请先执行搜索"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	m_currentMatchIndex = (m_currentMatchIndex + 1) % m_searchMatches.size();
	int pageNum = m_searchMatches[m_currentMatchIndex].pageNumber;
	fz_quad quad = m_searchMatches[m_currentMatchIndex].quad;

	// 跳转到对应页面
	GoToPage(pageNum);

	// 如果是连续滚动模式，滚动到匹配项位置
	if (m_continuousScrollMode)
	{
		// 添加边界检查，防止vector越界
		if (pageNum < 0 || pageNum >= (int)m_pageYPositions.size() || pageNum >= (int)m_pageHeights.size())
			return;

		CRect viewRect;
		m_pdfView.GetClientRect(&viewRect);

		// 计算匹配项在页面内的Y坐标范围
		float minY = min(min(quad.ul.y, quad.ur.y), min(quad.ll.y, quad.lr.y));
		float maxY = max(max(quad.ul.y, quad.ur.y), max(quad.ll.y, quad.lr.y));

		// 转换为连续视图中的绝对Y坐标
		int pageYPos = m_pageYPositions[pageNum];
		int matchTop = pageYPos + (int)(minY * m_uniformScale);
		int matchBottom = pageYPos + (int)(maxY * m_uniformScale);

		// 计算当前可见区域
		int visibleTop = m_scrollPosition;
		int visibleBottom = m_scrollPosition + viewRect.Height();

		// 只有当匹配项不完全可见时才滚动
		int margin = 50;  // 留出一些边距
		bool needScroll = false;

		if (matchTop < visibleTop + margin)
		{
			// 匹配项在可见区域上方，滚动到使其在顶部可见
			m_scrollPosition = matchTop - margin;
			needScroll = true;
		}
		else if (matchBottom > visibleBottom - margin)
		{
			// 匹配项在可见区域下方，滚动到使其在底部可见
			m_scrollPosition = matchBottom - viewRect.Height() + margin;
			needScroll = true;
		}

		if (needScroll)
		{
			// 限制范围
			int maxScroll = m_totalScrollHeight - viewRect.Height();
			if (maxScroll < 0) maxScroll = 0;
			if (m_scrollPosition < 0) m_scrollPosition = 0;
			if (m_scrollPosition > maxScroll) m_scrollPosition = maxScroll;

			UpdateScrollBar();
			RenderVisiblePages();
		}
	}

	// 更新状态栏
	CString msg;
	msg.Format(_T("匹配项 %d / %d"), m_currentMatchIndex + 1, (int)m_searchMatches.size());
	m_statusBar.SetWindowText(msg);
}

// 跳转到上一个匹配项
void CXiaoGongPDFDlg::GoToPrevMatch()
{
	if (m_searchMatches.empty())
	{
		MessageBox(_T("请先执行搜索"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	m_currentMatchIndex = (m_currentMatchIndex - 1 + m_searchMatches.size()) % m_searchMatches.size();
	int pageNum = m_searchMatches[m_currentMatchIndex].pageNumber;
	fz_quad quad = m_searchMatches[m_currentMatchIndex].quad;

	// 跳转到对应页面
	GoToPage(pageNum);

	// 如果是连续滚动模式，滚动到匹配项位置
	if (m_continuousScrollMode)
	{
		// 添加边界检查，防止vector越界
		if (pageNum < 0 || pageNum >= (int)m_pageYPositions.size() || pageNum >= (int)m_pageHeights.size())
			return;

		CRect viewRect;
		m_pdfView.GetClientRect(&viewRect);

		// 计算匹配项在页面内的Y坐标范围
		float minY = min(min(quad.ul.y, quad.ur.y), min(quad.ll.y, quad.lr.y));
		float maxY = max(max(quad.ul.y, quad.ur.y), max(quad.ll.y, quad.lr.y));

		// 转换为连续视图中的绝对Y坐标
		int pageYPos = m_pageYPositions[pageNum];
		int matchTop = pageYPos + (int)(minY * m_uniformScale);
		int matchBottom = pageYPos + (int)(maxY * m_uniformScale);

		// 计算当前可见区域
		int visibleTop = m_scrollPosition;
		int visibleBottom = m_scrollPosition + viewRect.Height();

		// 只有当匹配项不完全可见时才滚动
		int margin = 50;  // 留出一些边距
		bool needScroll = false;

		if (matchTop < visibleTop + margin)
		{
			// 匹配项在可见区域上方，滚动到使其在顶部可见
			m_scrollPosition = matchTop - margin;
			needScroll = true;
		}
		else if (matchBottom > visibleBottom - margin)
		{
			// 匹配项在可见区域下方，滚动到使其在底部可见
			m_scrollPosition = matchBottom - viewRect.Height() + margin;
			needScroll = true;
		}

		if (needScroll)
		{
			// 限制范围
			int maxScroll = m_totalScrollHeight - viewRect.Height();
			if (maxScroll < 0) maxScroll = 0;
			if (m_scrollPosition < 0) m_scrollPosition = 0;
			if (m_scrollPosition > maxScroll) m_scrollPosition = maxScroll;

			UpdateScrollBar();
			RenderVisiblePages();
		}
	}

	// 更新状态栏
	CString msg;
	msg.Format(_T("匹配项 %d / %d"), m_currentMatchIndex + 1, (int)m_searchMatches.size());
	m_statusBar.SetWindowText(msg);
}

// 查找按钮点击事件
void CXiaoGongPDFDlg::OnBtnFind()
{
	CString keyword;
	m_editSearch.GetWindowText(keyword);

	// 默认区分大小写（MuPDF搜索API的限制）
	bool caseSensitive = true;

	SearchPDF(keyword, caseSensitive);
}

// 下一个匹配按钮点击事件
void CXiaoGongPDFDlg::OnBtnNextMatch()
{
	GoToNextMatch();
}

// 上一个匹配按钮点击事件
void CXiaoGongPDFDlg::OnBtnPrevMatch()
{
	GoToPrevMatch();
}

// 将PDF坐标转换为位图坐标
CRect CXiaoGongPDFDlg::TransformQuadToScreen(const fz_quad& quad, int pageNumber)
{
	if (!m_currentPageObj)
		return CRect(0, 0, 0, 0);

	// 获取页面大小
	fz_rect bounds = fz_bound_page(m_ctx, m_currentPageObj);
	float origPageWidth = bounds.x1 - bounds.x0;
	float origPageHeight = bounds.y1 - bounds.y0;

	// 获取旋转角度
	int rotation = GetPageRotation(pageNumber);

	// 计算有效页面尺寸（考虑旋转）
	float effectivePageWidth = origPageWidth;
	float effectivePageHeight = origPageHeight;
	if (rotation == 90 || rotation == 270)
	{
		effectivePageWidth = origPageHeight;
		effectivePageHeight = origPageWidth;
	}

	// 获取视图尺寸
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	int viewWidth = viewRect.Width();
	int viewHeight = viewRect.Height();

	// 计算缩放比例（与RenderPage中的逻辑一致）
	float scale;
	switch (m_zoomMode)
	{
	case ZOOM_FIT_WIDTH:
		scale = viewWidth / effectivePageWidth * 0.95f;
		break;
	case ZOOM_FIT_PAGE:
	{
		float scaleX = viewWidth / effectivePageWidth;
		float scaleY = viewHeight / effectivePageHeight;
		scale = min(scaleX, scaleY) * 0.95f;
	}
	break;
	case ZOOM_CUSTOM:
		scale = m_customZoom;
		break;
	default:
	{
		float scaleX = viewWidth / effectivePageWidth;
		float scaleY = viewHeight / effectivePageHeight;
		scale = min(scaleX, scaleY) * 0.95f;
	}
	}

	// 计算渲染后的页面尺寸（使用原始尺寸，因为MuPDF总是按原始方向渲染）
	int width = (int)(origPageWidth * scale);
	int height = (int)(origPageHeight * scale);

	// ★★★ 考虑旋转后的实际尺寸
	int actualWidth = width;
	int actualHeight = height;
	if (rotation == 90 || rotation == 270)
	{
		actualWidth = height;
		actualHeight = width;
	}

	// ★★★ 检查是否需要背景（与RenderPage中的逻辑一致）
	bool needsBackground = (m_zoomMode == ZOOM_CUSTOM && (m_panOffset.x != 0 || m_panOffset.y != 0));

	// 计算偏移量
	int offsetX, offsetY;
	if (needsBackground)
	{
		// ★★★ 有平移偏移时，使用视图大小的位图，页面在其中居中并偏移
		offsetX = (viewWidth - actualWidth) / 2 + m_panOffset.x;
		offsetY = (viewHeight - actualHeight) / 2 + m_panOffset.y;
	}
	else
	{
		// ★★★ 无平移偏移时，使用页面大小的位图，坐标从(0,0)开始
		offsetX = 0;
		offsetY = 0;
	}

	// 转换quad坐标（PDF坐标系 -> 位图坐标系）
	// PDF坐标原点在左下，Y轴向上；位图坐标原点在左上，Y轴向下
	// 使用quad的四个角来确定矩形范围
	float minX = min(min(quad.ul.x, quad.ur.x), min(quad.ll.x, quad.lr.x));
	float maxX = max(max(quad.ul.x, quad.ur.x), max(quad.ll.x, quad.lr.x));
	float minY = min(min(quad.ul.y, quad.ur.y), min(quad.ll.y, quad.lr.y));
	float maxY = max(max(quad.ul.y, quad.ur.y), max(quad.ll.y, quad.lr.y));

	// ★★★ 转换到位图坐标（在原始页面坐标系中）
	// ★★★ 修复：MuPDF返回的quad坐标是相对于页面bounds的绝对坐标，需要减去bounds.x0
	// 且Y轴变换应使用bounds.y1（PDF坐标系的顶部）而非origPageHeight
	int x1 = (int)((minX - bounds.x0) * scale);
	int y1 = (int)(minY * scale);
	int x2 = (int)((maxX - bounds.x0) * scale);
	int y2 = (int)(maxY * scale);

	// ★★★ TODO: 如果有旋转，需要对坐标应用旋转变换
	// 目前暂不支持旋转页面的搜索高亮，需要实现坐标旋转变换

	// ★★★ 添加偏移量
	x1 += offsetX;
	y1 += offsetY;
	x2 += offsetX;
	y2 += offsetY;

#ifdef _DEBUG
	TRACE(_T("TransformQuadToScreen: quad=(%f,%f,%f,%f) -> rect=(%d,%d,%d,%d), offset=(%d,%d), scale=%.2f\n"),
		minX, minY, maxX, maxY, x1, y1, x2, y2, offsetX, offsetY, scale);
#endif

	return CRect(x1, y1, x2, y2);
}

// 高亮显示搜索匹配项
void CXiaoGongPDFDlg::HighlightSearchMatches(CDC* pDC, int pageNumber)
{
	if (m_searchMatches.empty() || !pDC)
		return;

#ifdef _DEBUG
	TRACE(_T("HighlightSearchMatches: pageNumber=%d, matchCount=%d\n"), pageNumber, (int)m_searchMatches.size());
#endif

	// 遍历所有匹配项
	for (size_t i = 0; i < m_searchMatches.size(); i++)
	{
		if (m_searchMatches[i].pageNumber != pageNumber)
			continue;

		// 将PDF坐标转换为位图坐标
		CRect highlightRect = TransformQuadToScreen(m_searchMatches[i].quad, pageNumber);

#ifdef _DEBUG
		TRACE(_T("Match %d: rect=(%d,%d,%d,%d)\n"), (int)i,
			highlightRect.left, highlightRect.top, highlightRect.right, highlightRect.bottom);
#endif

		// 确保矩形有效且在合理范围内
		if (highlightRect.IsRectEmpty() || highlightRect.Width() <= 0 || highlightRect.Height() <= 0)
			continue;

		// 选择颜色
		COLORREF fillColor;
		if ((int)i == m_currentMatchIndex)
		{
			// 当前匹配项用亮橙色
			fillColor = RGB(255, 150, 0);
		}
		else
		{
			// 其他匹配项用亮黄色
			fillColor = RGB(255, 200, 0);
		}

		// 创建临时DC用于AlphaBlend
	CDC tempDC;
	tempDC.CreateCompatibleDC(pDC);
	CBitmap tempBmp;
	tempBmp.CreateCompatibleBitmap(pDC, highlightRect.Width(), highlightRect.Height());
	CBitmap* pOldTempBmp = tempDC.SelectObject(&tempBmp);
	tempDC.FillSolidRect(0, 0, highlightRect.Width(), highlightRect.Height(), fillColor);

	// 使用AlphaBlend提高不透明度
	BLENDFUNCTION bf;
	bf.BlendOp = AC_SRC_OVER;
	bf.BlendFlags = 0;
	bf.SourceConstantAlpha = ((int)i == m_currentMatchIndex) ? 200 : 180;
	bf.AlphaFormat = 0;
	pDC->AlphaBlend(highlightRect.left, highlightRect.top,
		highlightRect.Width(), highlightRect.Height(),
		&tempDC, 0, 0, highlightRect.Width(), highlightRect.Height(), bf);
	tempDC.SelectObject(pOldTempBmp);

		// 绘制边框使其更明显
		CPen borderPen(PS_SOLID, 1, RGB(255, 140, 0));  // 橙色边框
		CPen* pOldPen = pDC->SelectObject(&borderPen);
		pDC->SelectStockObject(NULL_BRUSH);
		pDC->Rectangle(highlightRect);
		pDC->SelectObject(pOldPen);
	}
}
