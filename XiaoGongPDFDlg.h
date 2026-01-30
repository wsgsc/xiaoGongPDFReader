// XiaoGongPDFDlg.h: 头文件
//

#pragma once
#include <mupdf/fitz.h>
#include <afxext.h>
#include <map>
#include <vector>
#include "ShortcutsDialog.h"  // 包含快捷键对话框
#include "PDFDocument.h"       // 包含PDF文档类
#include "ModernTabCtrl.h"     // 包含现代化Tab控件
#include "CustomButton.h"      // 包含自定义按钮类
#include <list>

// 应用程序最小尺寸常量
#define APP_MINWIDTH 800
#define APP_MINHEIGHT 600

// 最近文件数量（注意：需要与资源文件中的菜单项数量保持一致）
// 资源文件定义了 ID_MENU_RECENT_FILE_1 到 ID_MENU_RECENT_FILE_10，共 10 个
#define RECENT_FILE_NUMS 10

// 前向声明
class CXiaoGongPDFDlg;

// 搜索匹配项结构（从 SearchTypes.h 包含）
#include "SearchTypes.h"

// ★★★ 自定义PDF预览控件（支持滚动条消息）
class CPDFViewCtrl : public CStatic
{
public:
	CPDFViewCtrl() : m_pParentDlg(nullptr), m_oldWndProc(nullptr) {}
	void SetParentDlg(CXiaoGongPDFDlg* pDlg) { m_pParentDlg = pDlg; }
	void SubclassWindow();

protected:
	CXiaoGongPDFDlg* m_pParentDlg;
	WNDPROC m_oldWndProc;

	static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

	afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg void OnPaint();  // ★★★ 自定义绘制，支持滚动偏移
	DECLARE_MESSAGE_MAP()
};

// 自定义打印对话框类
class CCustomPrintDlg : public CDialogEx
{
public:
	CCustomPrintDlg(int totalPages, CWnd* pParent = nullptr);
	virtual ~CCustomPrintDlg();

	enum { IDD = IDD_PRINT_DIALOG };

	// 打印设置
	CString m_printerName;      // 选中的打印机名称
	BOOL m_bPrintAll;           // 打印全部
	int m_pageFrom;             // 起始页
	int m_pageTo;               // 结束页
	int m_copies;               // 份数
	BOOL m_bCollate;            // 自动分页
	BOOL m_bDuplex;             // 双面打印

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();

	afx_msg void OnPrinterChange();
	afx_msg void OnPrintAllRadio();
	afx_msg void OnPrintRangeRadio();
	afx_msg void OnPrinterProperties();

	DECLARE_MESSAGE_MAP()

private:
	int m_totalPages;           // 总页数
	CComboBox m_printerCombo;   // 打印机下拉列表
	CButton m_printAllRadio;    // 全部单选按钮
	CButton m_printRangeRadio;  // 页码范围单选按钮
	CEdit m_pageFromEdit;       // 起始页编辑框
	CEdit m_pageToEdit;         // 结束页编辑框
	CEdit m_copiesEdit;         // 份数编辑框
	CButton m_collateCheck;     // 自动分页复选框
	CButton m_duplexCheck;      // 双面打印复选框

	void EnumeratePrinters();   // 枚举打印机
	void UpdateControls();      // 更新控件状态
};

// CXiaoGongPDFDlg 对话框
class CXiaoGongPDFDlg : public CDialogEx
{
	// ★★★ 友元类声明，允许CPDFViewCtrl访问私有成员
	friend class CPDFViewCtrl;

// 构造
public:
	CXiaoGongPDFDlg(CWnd* pParent = nullptr);	// 标准构造函数
	virtual ~CXiaoGongPDFDlg();
	// START MENU
	afx_msg void onMenuOpen();
	afx_msg void onMenuExit();
	afx_msg void onMenuAbout();
	afx_msg void onMenuPrint();
	afx_msg void onMenuRecentFile(UINT nID);
	afx_msg void onMenuSetDefault();    // 设为默认PDF阅读器
	afx_msg void onMenuShortcuts();     // 快捷键设置
	// END

	// 初始文件路径（从命令行参数传入）
	CString m_initialFilePath;

// 对话框数据
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_MOUTAIPDF_DIALOG };
#endif

protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV 支持
	virtual BOOL PreTranslateMessage(MSG* pMsg); // 添加消息预处理函数

private:
	CModernTabCtrl m_tabCtrl;   // 现代化标签页控件
	CListCtrl m_thumbnailList;  // 缩略图列表控件
	CPDFViewCtrl m_pdfView;     // ★★★ 使用自定义控件支持滚动条消息
	CStatic m_toolbar;
	CCustomButton m_btnFirst;
	CCustomButton m_btnLast;
	CCustomButton m_btnFullscreen;    // 全屏按钮
	CCustomButton m_btnRotateLeft;    // 向左旋转按钮
	CCustomButton m_btnRotateRight;   // 向右旋转按钮
	CCustomButton m_btnEdit;          // 编辑按钮
	CButton m_checkThumbnail;         // 缩略图复选框
	CEdit m_editCurrent;
	CStatic m_statusBar;        // 状态栏

	// 搜索相关控件
	CEdit m_editSearch;              // 搜索输入框
	CCustomButton m_btnFind;         // 查找按钮
	CCustomButton m_btnPrevMatch;    // 上一个匹配按钮
	CCustomButton m_btnNextMatch;    // 下一个匹配按钮
	CFont m_buttonFont;      // 按钮字体
	CFont m_labelFont;       // 标签字体
	
	fz_context* m_ctx;
	fz_document* m_doc;
	int m_currentPage;
	float m_zoom;
	int m_totalPages;  // 添加总页数
	fz_page* m_currentPageObj;  // 当前页面对象
	HBITMAP m_hCurrentBitmap;  // 当前显示的位图句柄

	// 多文档管理
	std::vector<CPDFDocument*> m_documents;  // 所有打开的文档
	int m_activeDocIndex;  // 当前活动文档索引

	// 后台加载相关
	struct PDFLoadParams {
		CString filePath;
		std::vector<char> utf8Path;
		CXiaoGongPDFDlg* pDlg;
		CPDFDocument* pDoc;
		bool success;
		CString errorMsg;
	};
	static UINT PDFLoadThreadProc(LPVOID pParam);  // 后台加载线程函数
	CWinThread* m_pLoadThread;  // 加载线程指针
	PDFLoadParams* m_pLoadParams;  // 加载参数
	CDialog* m_pProgressDlg;  // 进度对话框指针

	// 缩放相关（ZoomMode 定义在 PDFDocument.h 中）
	ZoomMode m_zoomMode;
	float m_customZoom;
	float m_documentMaxPageWidth;  // ★★★ 文档最大页面宽度（原始PDF点单位）
	float m_documentUniformScale;  // ★★★ 文档统一缩放比例（基于最大页面宽度，确保所有页面宽度一致）

	// 最近文件列表
	std::vector<CString> m_recentFiles;

    // 缩略图相关
    std::map<int, ThumbnailInfo> m_thumbnailCache;  // 缩略图缓存
    static const float THUMBNAIL_WIDTH_RATIO;  // 缩略图区域宽度比例（30%）
	static const int THUMBNAIL_WIDTH = 220;
	static const int LIST_WIDTH = THUMBNAIL_WIDTH + 30;
	static const int MARGIN = 7;
    static const int THUMBNAIL_SPACING = 10;  // 缩略图间距
	CBrush m_highlightBrush;  // 高亮背景画刷
	CBrush m_whiteBrush;      // 白色背景画刷（用于标签页）
    int m_thumbnailPicHeight;  // 当前缩略图高度
	int m_thumbnailPicWidth;   // 当前缩略图宽度
	int m_scrollBarWidth; //系统垂直滚动条的宽度

    CCriticalSection m_renderLock;  // 添加互斥锁成员变量

	bool m_isFullscreen;        // 是否处于全屏状态
	CRect m_windowRect;         // 存储窗口在进入全屏前的位置和大小
	ZoomMode m_savedZoomMode;   // 进入全屏前的缩放模式
	float m_savedCustomZoom;    // 进入全屏前的自定义缩放值

	int m_minWidth;             // 窗口最小宽度
	int m_minHeight;            // 窗口最小高度

	bool m_thumbnailVisible;    // 缩略图面板是否可见

	CShortcutsDialog m_shortcutsDialog;  // 快捷键对话框

	// 旋转相关
	std::map<int, int> m_pageRotations;  // 存储每个页面的旋转角度 (0, 90, 180, 270)

	// 缩放相关 - 每页独立保存
	struct PageZoomState {
		ZoomMode zoomMode;   // 缩放模式
		float customZoom;    // 自定义缩放比例
		CPoint panOffset;    // 平移偏移量（拖拽位置）

		PageZoomState() : zoomMode(ZOOM_FIT_PAGE), customZoom(1.0f), panOffset(0, 0) {}
		PageZoomState(ZoomMode mode, float zoom, CPoint offset)
			: zoomMode(mode), customZoom(zoom), panOffset(offset) {}
	};
	std::map<int, PageZoomState> m_pageZoomStates;  // 存储每个页面的缩放状态和平移位置

	// 拖拽平移相关
	bool m_isDragging;          // 是否正在拖拽
	CPoint m_lastMousePos;      // 上次鼠标位置
	CPoint m_panOffset;         // 当前平移偏移量
	HCURSOR m_hHandCursor;      // 手形光标
	HCURSOR m_hHandCursorGrab;  // 抓取光标
	HBITMAP m_hPanPageBitmap;   // 拖拽时缓存的PDF页面位图（不含偏移）
	bool m_canDrag;             // ★★★ 缓存：当前页面是否可以拖动（避免频繁计算）

	// 滚动条拖拽相关
	bool m_isDraggingScrollbar;     // 是否正在拖拽滚动条
	int m_scrollbarDragStartY;      // 滚动条拖拽起始Y坐标
	int m_scrollbarDragStartPos;    // 滚动条拖拽起始滚动位置

	//增加页面渲染缓存机制 start
	// 按分辨率缓存，进行优化，避免大界面小图片缓存
	struct PageCacheKey {
		int pageNumber;
		int width;
		int height;
		int rotation;  // ★★★ 添加旋转角度，解决旋转后缓存混乱问题

		bool operator<(const PageCacheKey& other) const noexcept {
			if (pageNumber != other.pageNumber) return pageNumber < other.pageNumber;
			if (width != other.width) return width < other.width;
			if (height != other.height) return height < other.height;
			return rotation < other.rotation;  // ★★★ 包含旋转角度比较
		}

		bool operator==(const PageCacheKey& other) const noexcept {
			return pageNumber == other.pageNumber &&
				width == other.width &&
				height == other.height &&
				rotation == other.rotation;  // ★★★ 包含旋转角度比较
		}
	};

	// 缓存值：页面位图
	struct PageCacheItem {
		HBITMAP hBitmap;
	};

	// 页面缓存表
	std::map<PageCacheKey, PageCacheItem> m_pageCache;

	// 可选：控制最大缓存数量
	static const int CACHE_LIMIT = 50;  // 增加缓存大小以提升大文件性能
	std::list<PageCacheKey> m_cacheOrder;
	// end

	// 缩略图缓存限制（避免大文件内存爆炸）
	static const int THUMBNAIL_CACHE_LIMIT = 100;  // 最多缓存100个缩略图

	// 连续滚动模式相关
	bool m_continuousScrollMode;            // 是否启用连续滚动模式
	int m_scrollPosition;                    // 当前垂直滚动位置
	int m_scrollPositionH;                   // 当前横向滚动位置
	int m_totalScrollHeight;                 // 总滚动高度（所有页面高度之和）
	int m_totalScrollWidth;                  // 总滚动宽度（PDF渲染宽度）
	float m_uniformScale;                    // ★★★ 统一的缩放比例（确保所有页面宽度一致）
	static const int PAGE_SPACING = 10;      // 页面间距（像素）
	std::vector<int> m_pageYPositions;       // 每页在滚动区域中的Y坐标
	std::vector<int> m_pageHeights;          // 每页的高度
	std::map<int, HBITMAP> m_continuousPageBitmaps;  // 连续滚动模式下的页面位图缓存
	HBITMAP m_hContinuousViewBitmap;         // ★★★ 当前连续滚动视图的位图（用于处理重绘）

	// ★★★ 滚动条拖动优化相关成员变量
	UINT_PTR m_scrollThumbTimer;             // 滚动条拖动定时器ID（0表示无定时器）
	bool m_isThumbTracking;                  // 是否正在拖动滚动条
	static const int THUMB_RENDER_INTERVAL = 200;  // 拖动时的渲染间隔（毫秒）

	// 搜索相关成员变量
	std::vector<SearchMatch> m_searchMatches;  // 所有搜索匹配项
	int m_currentMatchIndex;                    // 当前匹配项索引 (-1表示无匹配)
	CString m_searchKeyword;                    // 当前搜索关键词
	bool m_searchCaseSensitive;                 // 是否区分大小写

// 实现
protected:
	HICON m_hIcon;

	// 生成的消息映射函数
	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
	afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);  // ★★★ 拦截背景擦除消息，防止闪烁
	DECLARE_MESSAGE_MAP()

    // 缩略图相关函数
    void InitializeThumbnailList();
    bool RenderThumbnail(int pageNumber);
    void UpdateThumbnails();
    void CleanupThumbnails();
    void HighlightCurrentThumbnail();
	void RenderVisibleThumbnails();  // 渲染可见区域的缩略图（按需加载）
    afx_msg void OnThumbnailItemChanged(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg LRESULT OnThumbnailScroll(WPARAM wParam, LPARAM lParam);  // 缩略图滚动事件
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg LRESULT OnRenderThumbnailsAsync(WPARAM wParam, LPARAM lParam);  // 异步渲染缩略图
    afx_msg LRESULT OnOpenInitialFile(WPARAM wParam, LPARAM lParam);  // 延迟打开初始文件
    afx_msg LRESULT OnUpdateThumbnailHighlight(WPARAM wParam, LPARAM lParam);  // 延迟更新缩略图高亮
    afx_msg LRESULT OnPDFLoadComplete(WPARAM wParam, LPARAM lParam);  // PDF后台加载完成
    afx_msg void OnTimer(UINT_PTR nIDEvent);  // 定时器消息处理（用于去抖动缩略图高亮更新）

    // 工具栏相关函数
    void InitializeToolbar();  // 新增初始化工具栏函数
    void UpdatePageControls();
    void UpdateStatusBar();    // 更新状态栏
    afx_msg void OnBtnFirst();
    afx_msg void OnBtnLast();
    afx_msg void OnEnChangeEditCurrent();
    afx_msg void OnEnChangeEditSearch();  // 搜索框文本变化事件
    afx_msg void OnEditCurrentKillFocus();
    afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags); // 添加键盘事件处理
    afx_msg void OnBtnFullscreen();  // 全屏按钮点击事件

    // 旋转相关函数
    afx_msg void OnBtnRotateLeft();   // 向左旋转按钮点击事件
    afx_msg void OnBtnRotateRight();  // 向右旋转按钮点击事件
    void RotatePage(int degrees);     // 旋转当前页面

    // 编辑相关函数
    afx_msg void OnBtnEdit();         // 编辑按钮点击事件

    // 缩略图复选框相关
    afx_msg void OnCheckThumbnail();  // 缩略图复选框点击事件
    int GetPageRotation(int pageNumber);  // 获取指定页面的旋转角度
    HBITMAP RotateBitmap(HBITMAP hSrcBitmap, int rotation);  // 安全地旋转位图

    // 缩放状态管理函数
    void SaveCurrentPageZoomState();  // 保存当前页面的缩放状态
    void RestorePageZoomState(int pageNumber);  // 恢复指定页面的缩放状态
    PageZoomState GetPageZoomState(int pageNumber);  // 获取页面缩放状态

    // 全屏相关
    void EnterFullscreen();  // 进入全屏
    void ExitFullscreen();   // 退出全屏

    // 缩略图面板相关
    void ToggleThumbnailPanel();  // 切换缩略图面板显示/隐藏
    afx_msg void OnBtnToggleThumbnail();  // 按钮点击事件

	void OnOK();
	void OnCancel();

	void OnDestroy();

	void PostNcDestroy();

	// 打印相关
	void PrintSinglePage(CDC& dcPrint, int pageNum, int horRes, int verRes, BOOL bDuplex);

	void UIRelease();

	void ResourceRelease();

	void ClearPageCache();

    // 缩放相关函数
    void SetZoom(float zoom, ZoomMode mode = ZOOM_CUSTOM);



    // 最近文件相关函数
    void LoadRecentFiles();
    void SaveRecentFiles();
    void AddRecentFile(const CString& filePath);
    void UpdateRecentFilesMenu();

    // 拖拽相关
    afx_msg void OnDropFiles(HDROP hDropInfo);

    // 进程间通信
    afx_msg BOOL OnCopyData(CWnd* pWnd, COPYDATASTRUCT* pCopyDataStruct);

    // 多文档标签页管理
    void InitializeTabControl();  // 初始化标签页控件
    bool OpenPDFInNewTab(const CString& filePath);  // 在新标签页中打开PDF
    void SwitchToDocument(int index);  // 切换到指定文档
    void CloseDocument(int index);  // 关闭指定文档
    void CloseCurrentDocument();  // 关闭当前文档
    void UpdateTabControl();  // 更新标签页显示
    CPDFDocument* GetActiveDocument();  // 获取当前活动文档
    afx_msg void OnTabSelChange(NMHDR* pNMHDR, LRESULT* pResult);  // 标签页切换事件
    afx_msg LRESULT OnTabCloseButton(WPARAM wParam, LPARAM lParam);  // 标签页关闭按钮事件

    // 鼠标拖拽平移相关
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
    void ResetPanOffset();  // 重置平移偏移量

    // 连续滚动模式相关函数
    void CalculatePagePositions();  // 计算所有页面的位置和高度
    void RenderVisiblePages();  // 渲染可见的页面
    afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);  // 垂直滚动条消息处理
    afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);  // 横向滚动条消息处理
    void UpdateScrollBar();  // 更新滚动条
    int GetPageAtPosition(int yPos);  // 获取指定位置的页面索引

    // 搜索相关函数
    void SearchPDF(const CString& keyword, bool caseSensitive);  // 搜索PDF文档
    void GoToNextMatch();  // 跳转到下一个匹配项
    void GoToPrevMatch();  // 跳转到上一个匹配项
    void ClearSearchResults();  // 清除搜索结果
    void HighlightSearchMatches(CDC* pDC, int pageNumber);  // 高亮显示搜索匹配项
    CRect TransformQuadToScreen(const fz_quad& quad, int pageNumber);  // 将PDF坐标转换为屏幕坐标
    afx_msg void OnBtnFind();  // 查找按钮点击事件
    afx_msg void OnBtnNextMatch();  // 下一个匹配按钮点击事件
    afx_msg void OnBtnPrevMatch();  // 上一个匹配按钮点击事件

public:
	void RenderPDF(const char* filename);
	bool RenderPage(int pageNumber);  // 新增：渲染指定页面
	void CleanupCurrentPage();  // 新增：清理当前页面资源
	void CleanupBitmap();  // 新增：清理位图资源
	void GoToPage(int pageNumber);  // 统一的页面跳转函数，支持分页和连续滚动模式

    // 在类声明中添加消息处理函数
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
};
