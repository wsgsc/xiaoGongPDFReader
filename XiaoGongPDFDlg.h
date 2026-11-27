// XiaoGongPDFDlg.h: 主对话框头文件
//

#pragma once

#include <afxext.h>
#include <afxmt.h>
#include <atomic>
#include <list>
#include <map>
#include <vector>
#include <mupdf/fitz.h>

#include "CustomButton.h"
#include "FileWatcher.h"
#include "ModernTabCtrl.h"
#include "PDFAnnotationHelper.h"
#include "PDFDocument.h"
#include "PDFImageExport.h"
#include "PDFPropertiesDialog.h"
#include "PDFScrollBar.h"
#include "PDFSearch.h"
#include "PDFSidebarPanel.h"
#include "PDFViewCtrl.h"
#include "SearchTypes.h"
#include "ShortcutsDialog.h"

// 应用程序最小尺寸常量
#define APP_MINWIDTH 600
#define APP_MINHEIGHT 600

// 最近文件数量（需要与资源文件中的菜单项数量保持一致）
#define RECENT_FILE_NUMS 10

// CXiaoGongPDFDlg 对话框
class CXiaoGongPDFDlg : public CDialogEx
{
	friend class CPDFViewCtrl;
	friend class CPDFScrollBar;

public:
	CXiaoGongPDFDlg(CWnd* pParent = nullptr);
	virtual ~CXiaoGongPDFDlg();

	afx_msg void onMenuOpen();
	afx_msg void onMenuExit();
	afx_msg void onMenuAbout();
	afx_msg void onMenuPrint();
	afx_msg void onMenuRecentFile(UINT nID);
	afx_msg void onMenuSetDefault();
	afx_msg void onMenuShortcuts();
	afx_msg void OnMenuRegionCapture();
	afx_msg void OnMenuExportImages();
	afx_msg void OnMenuDocProperties();
	afx_msg void OnMenuSavePdf();
	afx_msg void OnMenuSavePdfAs();
	afx_msg void OnMenuPageLayout(UINT nID);
	afx_msg void OnBtnPageLayout();
	afx_msg void OnBtnAnnotate();
	afx_msg void OnMenuAnnotHighlight();
	afx_msg void OnMenuAnnotUnderline();
	afx_msg void OnMenuAnnotStrikeout();
	afx_msg void OnMenuAnnotTextNote();
	afx_msg void OnMenuSidebarTab(UINT nID);

	// 初始文件路径（从命令行参数传入）
	CString m_initialFilePath;
	std::vector<CString> m_initialFilePaths;

#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_MOUTAIPDF_DIALOG };
#endif

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL PreTranslateMessage(MSG* pMsg);

private:
	CModernTabCtrl m_tabCtrl;
	CPDFSidebarHeaderWnd m_sidebarHeader;
	CTreeCtrl m_outlineTree;
	CPDFThumbnailListWnd m_thumbnailListWnd;
	CPDFAnnotationListWnd m_annotListWnd;
	SidebarTabMode m_sidebarTab = SIDEBAR_TAB_OUTLINE;
	bool m_currentDocHasOutline = false;
	CPDFDocument* m_sidebarSyncedDoc = nullptr;
	CListCtrl m_searchResultsList;
	CStatic m_searchResultsHeader;
	CCustomButton m_btnCloseSearchResults;
	CPDFViewCtrl m_pdfView;
	CPDFScrollBar m_pdfVerticalScrollBar;
	CPDFScrollBar m_pdfHorizontalScrollBar{ false };
	CStatic m_pdfScrollCorner;
	CStatic m_toolbar;
	CCustomButton m_btnOpen;
	CCustomButton m_btnZoomOut;
	CCustomButton m_btnFitWidth;
	CCustomButton m_btnFitPage;
	CCustomButton m_btnZoomIn;
	CCustomButton m_btnMore;
	CCustomButton m_btnMinimize;
	CCustomButton m_btnMaximize;
	CCustomButton m_btnCloseWindow;
	CCustomButton m_btnFirst;
	CCustomButton m_btnPrevPage;
	CCustomButton m_btnNextPage;
	CCustomButton m_btnLast;
	CCustomButton m_btnPrint;
	CCustomButton m_btnSnapshot;
	CCustomButton m_btnPageLayout;
	CCustomButton m_btnAnnotate;
	CStatic m_toolbarDividers[16];
	CCustomButton m_btnFullscreen;
	CCustomButton m_btnRotateLeft;
	CCustomButton m_btnRotateRight;
	CCustomButton m_btnEdit;
	CCustomButton m_btnOutline;
	CEdit m_editCurrent;
	CStatic m_pageTotal;
	CStatic m_statusBar;
	CStatic m_zoomStatus;

	// 搜索相关控件
	CEdit m_editSearch;
	CStatic m_searchPopup;
	CStatic m_searchMatchCount;
	CCustomButton m_btnCloseSearchPopup;
	CCustomButton m_btnFind;
	CCustomButton m_btnPrevMatch;
	CCustomButton m_btnNextMatch;
	bool m_searchToolsVisible = false;
	CFont m_buttonFont;

	// 当前活动文档的非拥有引用（实际生命周期由 CPDFDocument 管理）
	fz_context* m_ctx;
	fz_document* m_doc;
	int m_currentPage;
	float m_zoom;
	int m_totalPages;

	// 多文档管理
	std::vector<CPDFDocument*> m_documents;
	int m_activeDocIndex;
	std::vector<CString> m_sessionRestorePaths;
	size_t m_sessionRestoreIndex = 0;
	CString m_sessionActivePath;
	bool m_sessionRestoreStarted = false;

	// 后台加载相关
	struct PDFLoadParams
	{
		CString filePath;
		std::vector<char> utf8Path;
		std::vector<char> utf8Password;
		HWND notifyWnd;
		CPDFDocument* pDoc;
		bool success;
		PDFOpenResult openResult = PDFOpenResult::Failed;
		std::atomic<bool> cancelled{ false };
		CString errorMsg;
		CWinThread* pThread = nullptr;
		CDialog* pProgressDlg = nullptr;
		bool restoringSession = false;

		~PDFLoadParams()
		{
			if (!utf8Password.empty())
				SecureZeroMemory(utf8Password.data(), utf8Password.size());
		}
	};
	static UINT PDFLoadThreadProc(LPVOID pParam);
	std::vector<PDFLoadParams*> m_pendingLoads;
	bool m_closePending;

	// 缩放相关
	ZoomMode m_zoomMode;
	ZoomMode m_nextFitMode = ZOOM_FIT_PAGE;
	float m_customZoom;

	// 最近文件列表
	std::vector<CString> m_recentFiles;

	// 侧栏与界面画刷
	static const int LIST_WIDTH = 250;
	CBrush m_highlightBrush;
	CBrush m_whiteBrush;
	CBrush m_toolbarBrush;
	CBrush m_searchPopupBrush;
	ReadingMode m_readingMode = READING_NORMAL;
	int m_scrollBarWidth;

	CCriticalSection m_renderLock;

	// 全屏状态保存
	bool m_isFullscreen;
	CRect m_windowRect;
	bool m_wasMaximized;
	ZoomMode m_savedZoomMode;
	float m_savedCustomZoom;
	CPoint m_savedPanOffset;
	int m_savedScrollPosition;
	float m_savedUniformScale;
	int m_savedFullscreenPage = -1;
	bool m_isUpdatingSearchEdit = false;
	bool m_suppressResizeRender = false;

	// OnSize 防抖与页面预取定时器
	static const UINT_PTR TIMER_ID_RESIZE_DEBOUNCE = 1002;
	static const UINT_PTR TIMER_ID_PAGE_PREFETCH = 1003;
	static const UINT_PTR TIMER_ID_ANNOT_CARET = 1004;
	static const UINT RESIZE_DEBOUNCE_MS = 60;
	static const UINT RESIZE_THROTTLE_MS = 33;
	int m_pendingResizeCx;
	int m_pendingResizeCy;
	ULONGLONG m_lastResizeRenderTick;

	bool m_outlineVisible;
	bool m_searchResultsVisible;
	bool m_updatingSearchResultsList;
	bool m_isUpdatingOutline;

	CShortcutsDialog m_shortcutsDialog;

	// 旋转相关
	std::map<int, int> m_pageRotations;

	// 拖拽平移与窗口拖动相关
	enum class PdfLinkKind
	{
		None = 0,
		Internal,
		External
	};
	struct PdfLinkHit
	{
		PdfLinkKind kind = PdfLinkKind::None;
		int targetPage = -1;
		float targetX = -1.0f;
		float targetY = -1.0f;
		CString externalUri;
	};
	struct CachedPageLinkItem
	{
		fz_rect rect = { 0.0f, 0.0f, 0.0f, 0.0f };
		PdfLinkHit hit;
	};

	bool m_isDragging;
	bool m_isWindowDragging;
	CPoint m_windowDragStartScreen;
	CRect m_windowDragStartRect;
	CPoint m_lastMousePos;
	CPoint m_linkPressPoint;
	PdfLinkHit m_pendingLinkHit;
	fz_document* m_pendingLinkDocument;
	int m_cachedLinksPage = -1;
	fz_document* m_cachedLinksDoc = nullptr;
	std::vector<CachedPageLinkItem> m_cachedPageLinks;
	bool m_hasHoverLinkStatus = false;
	CString m_lastHoverLinkPreview;
	CPoint m_panOffset;
	HCURSOR m_hHandCursor;
	HCURSOR m_hHandCursorGrab;
	bool m_canDrag;
	bool m_spaceKeyDown = false;
	bool m_tempHandDrag = false;

	// 框选区域高清截图相关
	enum CaptureBarAction
	{
		CAPTURE_ACT_NONE = -1,
		CAPTURE_ACT_COPY = 0,
		CAPTURE_ACT_SAVE = 1,
		CAPTURE_ACT_CANCEL = 2
	};
	bool m_isAreaCaptureMode = false;
	bool m_isDraggingCaptureRect = false;
	bool m_hasCaptureRect = false;
	CPoint m_captureStartView{ 0, 0 };
	CPoint m_captureEndView{ 0, 0 };
	CRect m_captureActionRects[3];
	int m_hoverCaptureAction = CAPTURE_ACT_NONE;
	int m_pressedCaptureAction = CAPTURE_ACT_NONE;
	CFont m_captureBarFont;

	// 文本选择相关
	enum SelectionBarAction
	{
		SEL_BAR_NONE = -1,
		SEL_BAR_COPY = 0,
		SEL_BAR_HIGHLIGHT = 1,
		SEL_BAR_UNDERLINE = 2,
		SEL_BAR_NOTE = 3,
		SEL_BAR_COUNT = 4
	};
	bool m_isSelecting;
	bool m_hasSelection;
	CPoint m_selStartView;
	CPoint m_selEndView;
	int m_selPageNumber;
	fz_stext_page* m_selectionTextPage;
	std::vector<fz_quad> m_selectionQuads;
	CString m_selectedText;
	CRect m_selectionActionRects[SEL_BAR_COUNT];
	int m_hoverSelectionAction = SEL_BAR_NONE;
	int m_pressedSelectionAction = SEL_BAR_NONE;

	// 连续滚动与单双页并排布局相关
	PageLayoutMode m_layoutMode = LAYOUT_SINGLE_CONTINUOUS;
	static const int SPREAD_GAP = 14;
	int m_scrollPosition;
	int m_scrollPositionH;
	int m_totalScrollHeight;
	int m_totalScrollWidth;
	float m_uniformScale;
	static const int PAGE_SPACING = 10;
	std::vector<int> m_pageYPositions;
	std::vector<int> m_pageHeights;
	std::vector<int> m_pageRowTops;
	std::vector<int> m_pageRowHeights;
	HBITMAP m_hContinuousViewBitmap;

	// 搜索相关成员变量
	std::vector<SearchMatch> m_searchMatches;
	std::vector<std::vector<int>> m_searchMatchesByPage;
	int m_currentMatchIndex;
	CString m_searchKeyword;
	bool m_searchCaseSensitive;
	PDFSearchParams* m_pSearchParams;
	std::vector<PDFSearchParams*> m_retiredSearches;
	bool m_isSearching;
	UINT_PTR m_searchTaskId;

	void StopSearch();
	void ReleaseSearchThread(PDFSearchParams* pParams);
	void CleanupSearches();
	void FinishPendingClose();
	void RefreshSearchResultsList();
	void NavigateToSearchMatch(int index);
	afx_msg void OnSearchResultSelected(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnCloseSearchResults();
	afx_msg LRESULT OnSearchComplete(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnSearchProgress(WPARAM wParam, LPARAM lParam);

	// 文件变化监听
	CFileWatcher m_fileWatcher;
	int m_fileWatchSuspendCount = 0;
	std::vector<CString> m_deferredFileChanges;
	struct ScopedFileWatchSuspend
	{
		CXiaoGongPDFDlg* pDlg;
		explicit ScopedFileWatchSuspend(CXiaoGongPDFDlg* dlg) : pDlg(dlg)
		{
			if (pDlg)
				++pDlg->m_fileWatchSuspendCount;
		}
		~ScopedFileWatchSuspend()
		{
			if (pDlg && --pDlg->m_fileWatchSuspendCount == 0)
				pDlg->FlushDeferredFileChanges();
		}
	};
	void FlushDeferredFileChanges();
	afx_msg LRESULT OnFileChanged(WPARAM wParam, LPARAM lParam);
	void ReloadDocument(CPDFDocument* pDoc);

	bool m_isRenderingWheelZoom;

protected:
	HICON m_hIcon;
	HICON m_hTitleIcon;

	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor);
	void StartWindowDrag();
	afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	DECLARE_MESSAGE_MAP()

	void UpdateOutlineTree();
	void AppendOutlineItems(const fz_outline* pOutline, HTREEITEM parent);
	afx_msg void OnOutlineSelectionChanged(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnSize(UINT nType, int cx, int cy);
	afx_msg LRESULT OnOpenInitialFile(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnPDFLoadComplete(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnPDFLoadCancelled(WPARAM wParam, LPARAM lParam);
	afx_msg void OnTimer(UINT_PTR nIDEvent);

	// 工具栏相关函数
	void InitializeToolbar();
	void layoutToolbar(int cx);
	void ApplyReadingMode(ReadingMode mode);
	void UpdateOutlineButtonColors();
	void UpdateSnapshotButtonColors();
	void UpdatePageLayoutButton();
	void UpdateAnnotateButtonColors();
	void UpdateDocumentDirtyTitle();
	COLORREF ViewBgColor() const { return m_readingMode == READING_NIGHT ? DARK_VIEW_BACKGROUND : PDF_VIEW_BACKGROUND; }
	afx_msg void OnReadingMode(UINT nID);
	void UpdatePageControls();
	void UpdateStatusBar();
	void UpdateZoomStatus();
	void SetStatusBarMessage(LPCTSTR message);
	afx_msg void OnBtnFirst();
	afx_msg void OnBtnPrevPage();
	afx_msg void OnBtnNextPage();
	afx_msg void OnBtnLast();
	afx_msg void OnEnChangeEditCurrent();
	afx_msg void OnEnChangeEditSearch();
	afx_msg void OnEditCurrentKillFocus();
	afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg void OnBtnFullscreen();

	// 旋转相关函数
	afx_msg void OnBtnRotateLeft();
	afx_msg void OnBtnRotateRight();
	void RotatePage(int degrees);
	int GetPageRotation(int pageNumber);

	// 编辑与目录侧栏相关函数
	afx_msg void OnBtnEdit();
	afx_msg void OnCheckThumbnail();
	void ToggleThumbnailPanel();

	// 缩放状态管理函数
	void SaveCurrentPageZoomState();
	void RestorePageZoomState(int pageNumber);
	::PageZoomState GetPageZoomState(int pageNumber);

	// 全屏相关
	void EnterFullscreen();
	void ExitFullscreen();

	void OnOK();
	void OnCancel();
	afx_msg void OnClose();
	void OnDestroy();
	void PostNcDestroy();
	void UIRelease();
	void ResourceRelease();

	void SaveDialogStateToDocument(CPDFDocument* pDoc);
	void LoadDialogStateFromDocument(CPDFDocument* pDoc);
	CString ReadingStateKey(const CString& filePath) const;
	void SaveReadingPosition(CPDFDocument* pDoc);
	void RestoreReadingPosition(CPDFDocument* pDoc);
	void SaveOpenTabs();
	void RestoreOpenTabs();
	void OpenNextRestoredTab();
	void CloseProgressDialog(PDFLoadParams* pParams);
	void ReleaseLoadThread(PDFLoadParams* pParams);
	void CleanupPendingLoad();
	void CleanupPendingLoad(PDFLoadParams* pParams);

	// 内部辅助函数
	void RefreshCurrentView();
	void SetControlsVisible(bool visible);
	void SafeDeleteBitmap(HBITMAP& hBmp);
	void ScrollToMatchVisible(int pageNum, fz_quad quad);
	void PerformResizeRender();

	// 缩放相关函数
	float GetCurrentZoomBaseForStep() const;
	void SetZoom(float zoom, ZoomMode mode = ZOOM_CUSTOM);
	void OnBtnZoomOut();
	void OnBtnFitWidth();
	void OnBtnFitPage();
	void OnBtnToggleFit();
	void UpdateFitButton();
	void OnBtnZoomIn();
	void OnBtnMore();
	void OnBtnMinimize();
	void OnBtnMaximize();
	void OnBtnCloseWindow();

	// 最近文件相关函数
	void LoadRecentFiles();
	void SaveRecentFiles();
	void AddRecentFile(const CString& filePath);
	void OpenRecentFileAt(int index);
	void RemoveRecentFileAt(int index);
	void ClearRecentFiles();

	// 拖拽与进程间通信
	afx_msg void OnDropFiles(HDROP hDropInfo);
	afx_msg BOOL OnCopyData(CWnd* pWnd, COPYDATASTRUCT* pCopyDataStruct);
	void OpenMultiplePdfFiles(const std::vector<CString>& paths);

	// 多文档标签页管理
	void InitializeTabControl();
	void UpdateTabWidth();
	bool OpenPDFInNewTab(const CString& filePath, bool restoringSession = false);
	void SwitchToDocument(int index);
	bool CloseDocument(int index, bool promptUnsaved = true);
	void CloseCurrentDocument();
	void UpdateTabControl();
	void RefreshDocumentLayout();
	CPDFDocument* GetActiveDocument();
	afx_msg void OnTabSelChange(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg LRESULT OnTabCloseButton(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnTabReorder(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnTabContextMenu(WPARAM wParam, LPARAM lParam);

	// 鼠标拖拽平移相关
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnMButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnCaptureChanged(CWnd* pWnd);
	afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);
	afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
	void ResetPanOffset();
	bool IsEditControlFocused() const;
	bool IsHandPanModeActive() const;
	bool StartPanDrag(CPoint pdfViewPoint, bool temporaryPan);

	// 连续滚动与页面渲染相关函数
	void CalculatePagePositions(bool recalcScale = true);
	void RenderVisiblePages();
	HBITMAP RenderContinuousPageToCache(int pageNumber, HDC hDC);
	void PrefetchNearbyPages();
	afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	void UpdateScrollBar();
	void ScrollToVerticalPosition(int position);
	void ScrollToHorizontalPosition(int position);
	int GetPageAtPosition(int yPos);
	void GetLayoutRowPages(int pageNumber, int& outLeftPage, int& outRightPage) const;
	bool IsSameLayoutRow(int pageA, int pageB) const;
	int GetPrevSpreadPage(int pageNumber) const;
	int GetNextSpreadPage(int pageNumber) const;
	bool GetPageViewPlacement(int pageNumber, int viewWidth, int& outOffsetX, int& outOffsetY,
		int* pOutWidth = nullptr, int* pOutHeight = nullptr);
	int GetPageAtViewPoint(CPoint viewPt, bool allowNearestFallback = true);
	void ApplyPageLayoutMode(PageLayoutMode mode);

	// 搜索相关函数
	void SearchPDF(const CString& keyword, bool caseSensitive);
	void GoToNextMatch();
	void GoToPrevMatch();
	void ClearSearchResults(bool keepPanel = false);
	void UpdateSelectionHighlight(CDC* pDC, int pageNumber);
	void RebuildSearchMatchPageIndex();
	bool TryGetPageBaseBounds(int pageNumber, fz_rect& outBounds) const;
	CRect TransformQuadToBitmapRect(const fz_quad& quad, const fz_rect& bounds,
		int rotation, float scale) const;
	afx_msg void OnBtnFind();
	void ShowSearchTools();
	afx_msg void OnCloseSearchPopup();
	afx_msg void OnBtnNextMatch();
	afx_msg void OnBtnPrevMatch();

	// 文本选择、链接交互与区域截图相关函数
	CString ExtractSelectedText();
	bool CopyTextToClipboard(const CString& text);
	void CopySelectionToClipboard();
	bool ScreenPointToPdfCoords(CPoint viewPt, int pageNumber, float& pdfX, float& pdfY);
	void InvalidateLinkCache();
	void EnsurePageLinksCached(int pageNumber);
	bool HitTestLink(CPoint viewPt, PdfLinkHit& outHit);
	void UpdateHoverLinkFeedback(CPoint viewPt);
	void ClearHoverLinkFeedback();
	void OnPdfViewMouseLeave();
	void OpenExternalLinkWithConfirm(const CString& externalUri);
	void StartAreaCaptureMode();
	void CancelAreaCaptureMode(bool restoreStatusBar = true);
	void ClearActiveCaptureRect();
	bool IsAreaCaptureOverlayVisible() const { return m_isAreaCaptureMode && (m_isDraggingCaptureRect || m_hasCaptureRect); }
	CRect GetNormalizedCaptureViewRect() const;
	bool ComputeCapturePixelSizePreview(const CRect& viewSelRect, CSize& outPixelSize, int& outDpi) const;
	void DrawAreaCaptureOverlay(CDC* pDC, const CRect& clientRect);
	int HitTestCaptureActionBar(CPoint viewPt) const;
	bool RenderCapturedRegionBuffer(const CRect& viewSelRect, ExportBitmapBuffer& outBuffer, int& outDpi);
	void CopyCapturedRegionToClipboard();
	void SaveCapturedRegionAsPng();
	void CopyCurrentPageAsImage();
	bool RenderDocumentPageBuffer(int pageNumber, float targetScale, double maxPixels, ExportBitmapBuffer& outBuffer);
	bool GetSelectionPageTransform(int pageNumber, fz_rect& bounds, float& scale,
		int& offsetX, int& offsetY);
	void UpdateSelectedText();
	void ClearSelection();
	void InvalidateSelection();
	bool IsSelectionActionBarVisible() const
	{
		return m_hasSelection && !m_isSelecting && !m_isAreaCaptureMode &&
			!m_selectionQuads.empty();
	}
	void DrawSelectionActionBar(CDC* pDC, const CRect& clientRect);
	int HitTestSelectionActionBar(CPoint viewPt) const;

	// 批注与原文件保存辅助函数
	enum InlineAnnotBarAction
	{
		INLINE_ANNOT_BTN_NONE = -1,
		INLINE_ANNOT_BTN_COLOR = 0,
		INLINE_ANNOT_BTN_FONT_INC = 1,
		INLINE_ANNOT_BTN_FONT_DEC = 2,
		INLINE_ANNOT_BTN_CHAR_SPACE_INC = 3,
		INLINE_ANNOT_BTN_CHAR_SPACE_DEC = 4,
		INLINE_ANNOT_BTN_DELETE = 5,
		INLINE_ANNOT_BTN_EXIT = 6,
		INLINE_ANNOT_BTN_COUNT = 7
	};
	static const int INLINE_ANNOT_SWATCH_COUNT = 8;

	struct InlineTextAnnotState
	{
		bool active = false;
		int pageNumber = -1;
		int existingAnnotIndex = -1;
		float localXPt = 72.0f;
		float localYPt = 72.0f;
		float widthPt = 135.0f;
		bool userResizedWidth = false;
		COLORREF color = RGB(0, 114, 206);
		float fontSizePt = 12.0f;
		float charSpacingPt = 0.0f;
		bool colorPopupOpen = false;
		bool draggingMove = false;
		bool draggingResizeW = false;
		bool selectingText = false;
		int selectAnchorChar = 0;
		CPoint dragStartView{ 0, 0 };
		float dragStartLocalXPt = 0.0f;
		float dragStartLocalYPt = 0.0f;
		float dragStartWidthPt = 0.0f;
		bool justCreatedFromMenu = false;
		bool caretVisible = true;
		CRect boxViewRect{ 0, 0, 0, 0 };
		CRect moveHandleViewRect{ 0, 0, 0, 0 };
		CRect resizeHandleViewRect{ 0, 0, 0, 0 };
		CRect toolbarViewRect{ 0, 0, 0, 0 };
		CRect toolbarBtnRects[INLINE_ANNOT_BTN_COUNT];
		CRect colorPopupViewRect{ 0, 0, 0, 0 };
		CRect colorSwatchRects[INLINE_ANNOT_SWATCH_COUNT];
		int hoverToolbarBtn = INLINE_ANNOT_BTN_NONE;
		int pressedToolbarBtn = INLINE_ANNOT_BTN_NONE;
		int hoverColorSwatch = -1;
		int pressedColorSwatch = -1;
		bool hoverMoveHandle = false;
		bool hoverResizeHandle = false;
		bool hoverBorderMove = false;
	};

	COLORREF m_defaultAnnotTextColor = RGB(0, 114, 206);
	float m_defaultAnnotFontSizePt = 12.0f;
	float m_defaultAnnotCharSpacingPt = 0.0f;
	CInlineAnnotEdit m_inlineAnnotEdit;
	InlineTextAnnotState m_inlineAnnot;

	bool IsInlineTextAnnotVisible() const { return m_inlineAnnot.active && m_doc != nullptr; }
	void DrawInlineTextAnnotOverlay(CDC* pDC, const CRect& clientRect);
	void StartInlineTextAnnotAt(int pageNumber, float fitzX, float fitzY,
		const CString& initialText = CString(), bool fromToolbarMenu = false);
	void StartInlineTextAnnotFromExisting(int pageNumber, const PageAnnotHitInfo& hitInfo);
	void FinishInlineTextAnnot(bool discard = false);
	void DeleteActiveInlineTextAnnot();
	bool HitTestInlineAnnotMoveBorder(CPoint viewPt) const;
	int HitTestInlineAnnotToolbar(CPoint viewPt) const;
	int HitTestInlineAnnotColorSwatch(CPoint viewPt) const;
	int HitTestInlineAnnotCharIndex(CPoint viewPt);
	bool HandleInlineAnnotKeyNav(UINT vk, bool shift);
	afx_msg void OnEnChangeInlineAnnotEdit();

	void ApplySelectionMarkupAnnotation(enum pdf_annot_type annotType, COLORREF color, LPCTSTR actionName);
	void PromptAddTextNoteAt(int pageNumber, float fitzX, float fitzY,
		const CString& defaultText = CString(), bool fromToolbarMenu = false);
	bool HitTestAnnotationAtViewPoint(CPoint viewPt, int& outPageNumber, PageAnnotHitInfo& outHit);
	bool SaveDocumentEdits(CPDFDocument* pTargetDoc, bool saveAs);
	bool PromptSaveUnsavedAnnotations(CPDFDocument* pDoc);

	// 侧边栏（目录 / 缩略图 / 批注列表）相关函数
	void SwitchSidebarTab(SidebarTabMode tab, bool saveToDoc = true);
	void OpenSidebarTab(SidebarTabMode tab);
	void SyncSidebarForActiveDocument();
	void RefreshAnnotationsSidebar();
	void UpdateSidebarWindowsVisibility(bool allowVisible);
	bool QueryOrRenderPageThumbnail(int pageIndex, int maxWidth, int maxHeight,
		HBITMAP& outBitmap, CSize& outPagePxSize, bool renderIfMissing);
	void RotateSpecificPage(int pageIndex, int degrees);
	void CopySpecificPageAsImage(int pageIndex);
	void ShowThumbnailContextMenu(int pageIndex, CPoint screenPt);
	void NavigateToSidebarAnnotation(const DocumentAnnotationItem& item);
	void EditSidebarAnnotation(const DocumentAnnotationItem& item);
	void DeleteSidebarAnnotation(const DocumentAnnotationItem& item);
	void ShowSidebarAnnotationContextMenu(const DocumentAnnotationItem& item, CPoint screenPt);

public:
	void CleanupBitmap();
	void GoToPage(int pageNumber);
	afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
};
