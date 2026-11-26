#include "pch.h"
#include "framework.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <WinUser.h>
#include <mupdf/fitz.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <vector>
#include "afxdialogex.h"
#include "XiaoGongPDF.h"
#include "XiaoGongPDFDlg.h"
#include "CustomPrintDlg.h"
#include "PDFEditDialog.h"
#include "ProgressDlg.h"

#pragma comment(lib, "dwmapi.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	constexpr int TITLE_BAR_HEIGHT = 46; 
	constexpr int TITLE_ICON_AREA_WIDTH = 56;
	constexpr int TITLE_ICON_SIZE = 38;

	HICON loadTitleIcon()
	{
		HINSTANCE hInstance = AfxGetInstanceHandle();
		HRSRC hResource = ::FindResource(hInstance, MAKEINTRESOURCE(IDR_WELCOME_PNG), RT_RCDATA);
		if (!hResource)
			return nullptr;

		HGLOBAL hData = ::LoadResource(hInstance, hResource);
		const DWORD dataSize = ::SizeofResource(hInstance, hResource);
		const BYTE* pData = hData ? static_cast<const BYTE*>(::LockResource(hData)) : nullptr;
		if (!pData || dataSize == 0)
			return nullptr;

		IStream* pStream = ::SHCreateMemStream(pData, dataSize);
		if (!pStream)
			return nullptr;

		HICON hIcon = nullptr;
		{
			Gdiplus::Bitmap bitmap(pStream, FALSE);
			if (bitmap.GetLastStatus() == Gdiplus::Ok && bitmap.GetWidth() > 0 && bitmap.GetHeight() > 0)
			{
				// 裁掉欢迎插画四周的大面积空白及底部提示文字，仅提取中央人物+PDF主体作为标题栏图标
				int srcX = 0;
				int srcY = 0;
				int srcW = static_cast<int>(bitmap.GetWidth());
				int srcH = static_cast<int>(bitmap.GetHeight());
				if (srcW >= 1200 && srcH >= 800)
				{
					srcX = 505;
					srcY = 66;
					srcW = 536;
					srcH = 588;
				}

				const double scale = min(static_cast<double>(TITLE_ICON_SIZE) / srcW,
					static_cast<double>(TITLE_ICON_SIZE) / srcH);
				const int width = max(1, static_cast<int>(srcW * scale + 0.5));
				const int height = max(1, static_cast<int>(srcH * scale + 0.5));
				Gdiplus::Bitmap iconBitmap(TITLE_ICON_SIZE, TITLE_ICON_SIZE, PixelFormat32bppARGB);
				if (iconBitmap.GetLastStatus() == Gdiplus::Ok)
				{
					Gdiplus::Status drawStatus;
					{
						Gdiplus::Graphics graphics(&iconBitmap);
						graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
						graphics.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
						graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
						graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
						Gdiplus::Rect destRect(
							(TITLE_ICON_SIZE - width) / 2,
							(TITLE_ICON_SIZE - height) / 2,
							width, height);
						drawStatus = graphics.DrawImage(&bitmap, destRect,
							srcX, srcY, srcW, srcH, Gdiplus::UnitPixel);
					}
					if (drawStatus == Gdiplus::Ok)
						iconBitmap.GetHICON(&hIcon);
				}
			}
		}
		pStream->Release();
		return hIcon;
	}

	class CPasswordDialog : public CDialogEx
	{
	public:
		CPasswordDialog(const CString& fileName, bool invalidPassword, CWnd* pParent)
			: CDialogEx(IDD_PASSWORD_DIALOG, pParent)
			, m_fileName(fileName)
			, m_invalidPassword(invalidPassword)
		{
		}

		~CPasswordDialog()
		{
			const int length = m_password.GetLength();
			if (length > 0)
			{
				SecureZeroMemory(m_password.GetBuffer(), length * sizeof(TCHAR));
				m_password.ReleaseBuffer(0);
			}
		}

		const CString& GetPassword() const { return m_password; }

	protected:
		BOOL OnInitDialog() override
		{
			CDialogEx::OnInitDialog();
			SetDlgItemText(IDC_PASSWORD_FILE, m_fileName);
			if (m_invalidPassword)
				SetDlgItemText(IDC_PASSWORD_PROMPT, _T("密码错误，请重新输入"));
			GetDlgItem(IDC_EDIT_PASSWORD)->SetFocus();
			return FALSE;
		}

		void OnOK() override
		{
			GetDlgItemText(IDC_EDIT_PASSWORD, m_password);
			CDialogEx::OnOK();
		}

	private:
		CString m_fileName;
		CString m_password;
		bool m_invalidPassword;
	};

	constexpr size_t PAGE_CACHE_BUDGET = 128u * 1024u * 1024u;
	constexpr int FIT_WIDTH_SIDE_PADDING = 12;
	constexpr float FIT_WIDTH_FACTOR = 0.99f;

	float FitWidthScale(int viewWidth, float pageWidth)
	{
		return pageWidth > 0.0f ?
			max(1, viewWidth - 2 * FIT_WIDTH_SIDE_PADDING) / pageWidth * FIT_WIDTH_FACTOR : 1.0f;
	}

	CString FullPathForComparison(const CString& path)
	{
		std::vector<TCHAR> buffer(MAX_PATH);
		for (;;)
		{
			DWORD length = ::GetFullPathName(path, static_cast<DWORD>(buffer.size()),
				buffer.data(), nullptr);
			if (length == 0)
				return path;
			if (length < buffer.size())
				return CString(buffer.data());
			buffer.resize(static_cast<size_t>(length) + 1);
		}
	}

	template<typename Key, typename Item>
	void TrimBitmapCache(std::map<Key, Item>& cache, std::list<Key>& order,
		bool oldestAtFront, size_t maxBytes, size_t maxEntries)
	{
		size_t bytes = 0;
		for (const auto& entry : cache)
		{
			BITMAP info = { 0 };
			if (entry.second.hBitmap &&
				::GetObject(entry.second.hBitmap, sizeof(info), &info))
				bytes += static_cast<size_t>(info.bmWidthBytes) *
					static_cast<size_t>(abs(info.bmHeight));
		}
		while (order.size() > 1 && (cache.size() > maxEntries || bytes > maxBytes))
		{
			Key oldest = oldestAtFront ? order.front() : order.back();
			if (oldestAtFront)
				order.pop_front();
			else
				order.pop_back();
			auto it = cache.find(oldest);
			if (it == cache.end())
				continue;
			BITMAP info = { 0 };
			if (it->second.hBitmap && ::GetObject(it->second.hBitmap, sizeof(info), &info))
				bytes -= static_cast<size_t>(info.bmWidthBytes) *
					static_cast<size_t>(abs(info.bmHeight));
			if (it->second.hBitmap)
				::DeleteObject(it->second.hBitmap);
			cache.erase(it);
		}
	}

	struct QuadPointF
	{
		float x;
		float y;
	};

	int ClampRenderDimension(float value)
	{
		const int dimension = static_cast<int>(value);
		return (dimension > 0) ? dimension : 1;
	}

	fz_matrix BuildPageRenderMatrix(const fz_rect& bounds, float scale, int rotation)
	{
		const int normRotation = ((rotation % 360) + 360) % 360;
		fz_matrix ctm = fz_scale(scale, scale);
		if (normRotation != 0)
		{
			fz_matrix rotate = fz_rotate(static_cast<float>(normRotation));
			if (normRotation == 90)
				rotate = fz_pre_translate(rotate, -bounds.x0, -bounds.y1);
			else if (normRotation == 180)
				rotate = fz_pre_translate(rotate, -bounds.x1, -bounds.y1);
			else if (normRotation == 270)
				rotate = fz_pre_translate(rotate, -bounds.x1, -bounds.y0);
			ctm = fz_concat(rotate, ctm);
		}
		else if (bounds.x0 != 0.0f || bounds.y0 != 0.0f)
		{
			ctm = fz_pre_translate(ctm, -bounds.x0, -bounds.y0);
		}
		return ctm;
	}

	QuadPointF TransformPdfPointToBitmap(float x, float y, const fz_rect& bounds,
		int rotation, float scale)
	{
		const float localX = x - bounds.x0;
		const float localY = y - bounds.y0;
		const float pageWidth = bounds.x1 - bounds.x0;
		const float pageHeight = bounds.y1 - bounds.y0;

		switch ((rotation % 360 + 360) % 360)
		{
		case 90:
			return { (pageHeight - localY) * scale, localX * scale };
		case 180:
			return { (pageWidth - localX) * scale, (pageHeight - localY) * scale };
		case 270:
			return { localY * scale, (pageWidth - localX) * scale };
		default:
			return { localX * scale, localY * scale };
		}
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
	, m_zoom(1.0f)
	, m_totalPages(0)
	, m_activeDocIndex(-1)
	, m_closePending(false)
	, m_zoomMode(ZOOM_CUSTOM)
	, m_customZoom(1.0f)
	, m_scrollBarWidth(0)
	, m_renderLock()
	, m_isFullscreen(false)
	, m_wasMaximized(false)
	, m_savedZoomMode(ZOOM_CUSTOM)
	, m_savedCustomZoom(1.0f)
	, m_savedPanOffset(0, 0)
	, m_savedScrollPosition(0)
	, m_savedUniformScale(0.0f)
	, m_pendingResizeCx(0)
	, m_pendingResizeCy(0)
	, m_lastResizeRenderTick(0)
	, m_outlineVisible(false)
	, m_searchResultsVisible(false)
	, m_updatingSearchResultsList(false)
	, m_isUpdatingOutline(false)
	, m_isDragging(false)
	, m_isWindowDragging(false)
	, m_windowDragStartScreen(0, 0)
	, m_linkPressPoint(0, 0)
	, m_pendingLinkDocument(nullptr)
	, m_panOffset(0, 0)
	, m_hHandCursor(nullptr)
	, m_hHandCursorGrab(nullptr)
	, m_canDrag(false)
	, m_isSelecting(false)
	, m_hasSelection(false)
	, m_selPageNumber(-1)
	, m_selectionTextPage(nullptr)
	, m_scrollPosition(0)
	, m_scrollPositionH(0)
	, m_totalScrollHeight(0)
	, m_totalScrollWidth(0)
	, m_uniformScale(1.0f)
	, m_hContinuousViewBitmap(nullptr)
	, m_currentMatchIndex(-1)
	, m_searchCaseSensitive(false)
	, m_pSearchParams(nullptr)
	, m_isSearching(false)
	, m_searchTaskId(0)
	, m_isRenderingWheelZoom(false)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	m_hTitleIcon = loadTitleIcon();
	m_highlightBrush.CreateSolidBrush(RGB(173, 216, 230));  // 浅蓝色
	m_whiteBrush.CreateSolidBrush(RGB(240, 244, 248));
	m_toolbarBrush.CreateSolidBrush(RGB(255, 255, 255));   // 白色（Chrome/Edge 风格工具栏背景）
	m_searchPopupBrush.CreateSolidBrush(RGB(255, 255, 255));
	LoadRecentFiles();

	// 加载手形光标
	m_hHandCursor = LoadCursor(NULL, IDC_HAND);
	// 注意：Windows没有内置的"抓取"光标，我们暂时使用手形光标
	m_hHandCursorGrab = LoadCursor(NULL, IDC_HAND);
}

CXiaoGongPDFDlg::~CXiaoGongPDFDlg()
{
	if (m_hTitleIcon)
		::DestroyIcon(m_hTitleIcon);

#ifdef _DEBUG
	TRACE(_T("~CXiaoGongPDFDlg() 析构函数开始\n"));
#endif

	// 析构仅做"纯资源"清理，不碰 UI
	CleanupPendingLoad();

	CleanupSearches();

	// 释放可能残留的文本选择页面（必须在 m_ctx 销毁之前）
	if (m_selectionTextPage && m_ctx)
	{
		fz_drop_stext_page(m_ctx, m_selectionTextPage);
	}
	m_selectionTextPage = nullptr;

	// 在删除文档对象之前，先将当前活动文档的状态保存回文档对象
	if (m_activeDocIndex >= 0 && m_activeDocIndex < (int)m_documents.size())
	{
#ifdef _DEBUG
		TRACE(_T("保存当前文档状态，索引: %d\n"), m_activeDocIndex);
#endif
		CPDFDocument* activeDoc = m_documents[m_activeDocIndex];
		if (activeDoc)
		{
			SaveDialogStateToDocument(activeDoc);
			SaveReadingPosition(activeDoc);
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

	m_ctx = nullptr;
	m_doc = nullptr;

	SafeDeleteBitmap(m_hContinuousViewBitmap);

	ResourceRelease();

	// 释放字体和画刷对象
	if (m_buttonFont.GetSafeHandle())
		m_buttonFont.DeleteObject();
	if (m_captureBarFont.GetSafeHandle())
		m_captureBarFont.DeleteObject();
	if (m_highlightBrush.GetSafeHandle())
		m_highlightBrush.DeleteObject();
	if (m_whiteBrush.GetSafeHandle())
		m_whiteBrush.DeleteObject();
	if (m_toolbarBrush.GetSafeHandle())
		m_toolbarBrush.DeleteObject();
	if (m_searchPopupBrush.GetSafeHandle())
		m_searchPopupBrush.DeleteObject();
}

void CXiaoGongPDFDlg::SaveDialogStateToDocument(CPDFDocument* pDoc)
{
	if (!pDoc)
		return;

	if (m_currentPage >= 0 && m_currentPage < m_totalPages)
	{
		pDoc->SetCurrentPage(m_currentPage);
		pDoc->SetZoom(m_customZoom, m_zoomMode);
		pDoc->SetPanOffset(CPoint(m_scrollPositionH, 0));
		pDoc->SetCanDrag(m_canDrag);
		pDoc->SaveCurrentPageZoomState();
	}

	pDoc->SetUniformScale(m_uniformScale);
	pDoc->SetScrollPosition(m_scrollPosition);
	pDoc->SetSearchMatches(m_searchMatches);
	pDoc->SetCurrentMatchIndex(m_currentMatchIndex);

	CString searchKeyword;
	if (m_editSearch.GetSafeHwnd())
		m_editSearch.GetWindowText(searchKeyword);
	else
		searchKeyword = m_searchKeyword;
	pDoc->SetSearchKeyword(searchKeyword);

	pDoc->GetPageRotations() = m_pageRotations;
	pDoc->SetLayoutMode(m_layoutMode);
}

void CXiaoGongPDFDlg::LoadDialogStateFromDocument(CPDFDocument* pDoc)
{
	if (!pDoc)
		return;

	ClearSelection();
	CancelAreaCaptureMode(false);
	InvalidateLinkCache();
	if (m_pdfView.GetSafeHwnd())
		m_pdfView.SetDisplayBitmap(NULL);
	m_pendingLinkHit = PdfLinkHit{};
	m_pendingLinkDocument = nullptr;

	m_ctx = pDoc->GetContext();
	m_doc = pDoc->GetDocument();
	m_totalPages = pDoc->GetTotalPages();
	m_currentPage = pDoc->GetCurrentPage();
	m_zoom = pDoc->GetZoom();
	m_zoomMode = pDoc->GetZoomMode();
	if (m_zoomMode == ZOOM_FIT_PAGE || m_zoomMode == ZOOM_FIT_WIDTH)
	{
		m_nextFitMode = m_zoomMode == ZOOM_FIT_PAGE ? ZOOM_FIT_WIDTH : ZOOM_FIT_PAGE;
		UpdateFitButton();
	}
	m_customZoom = pDoc->GetCustomZoom();
	const CPoint savedPan = pDoc->GetPanOffset();
	m_panOffset = CPoint(0, 0);
	m_canDrag = pDoc->GetCanDrag();
	m_scrollPosition = pDoc->GetScrollPosition();
	m_scrollPositionH = max(0, savedPan.x);
	m_searchMatches = pDoc->GetSearchMatches();
	m_currentMatchIndex = pDoc->GetCurrentMatchIndex();
	m_searchKeyword = pDoc->GetSearchKeyword();
	m_pageRotations = pDoc->GetPageRotations();
	m_layoutMode = pDoc->GetLayoutMode();
	UpdatePageLayoutButton();
	UpdateAnnotateButtonColors();
	RebuildSearchMatchPageIndex();
}

CString CXiaoGongPDFDlg::ReadingStateKey(const CString& filePath) const
{
	CString normalized(filePath);
	normalized.MakeLower();
	ULONGLONG hash = 14695981039346656037ULL;
	for (int i = 0; i < normalized.GetLength(); ++i)
	{
		hash ^= static_cast<ULONGLONG>(normalized[i]);
		hash *= 1099511628211ULL;
	}
	CString key;
	key.Format(_T("%016I64X"), hash);
	return key;
}

void CXiaoGongPDFDlg::SaveReadingPosition(CPDFDocument* pDoc)
{
	if (!pDoc || pDoc->GetFilePath().IsEmpty())
		return;
	CString key = ReadingStateKey(pDoc->GetFilePath());
	CWinApp* pApp = AfxGetApp();
	pApp->WriteProfileInt(_T("ReadingPosition"), key + _T("_Page"),
		pDoc->GetCurrentPage());
	pApp->WriteProfileInt(_T("ReadingPosition"), key + _T("_Scroll"),
		pDoc->GetScrollPosition());
	pApp->WriteProfileInt(_T("ReadingPosition"), key + _T("_Mode"),
		pDoc->GetZoomMode());
	pApp->WriteProfileInt(_T("ReadingPosition"), key + _T("_Zoom"),
		static_cast<int>(pDoc->GetCustomZoom() * 10000.0f));
	pApp->WriteProfileInt(_T("ReadingPosition"), key + _T("_Scale"),
		static_cast<int>(pDoc->GetUniformScale() * 10000.0f));
	pApp->WriteProfileInt(_T("ReadingPosition"), key + _T("_Layout"),
		static_cast<int>(pDoc->GetLayoutMode()));

	// 持久化单页旋转角度（格式："0:90,3:270"），下次打开自动恢复
	CString rotSerialized;
	for (const auto& rotPair : pDoc->GetPageRotations())
	{
		const int pageIdx = rotPair.first;
		const int normDeg = ((rotPair.second % 360) + 360) % 360;
		if (pageIdx >= 0 && normDeg != 0)
		{
			CString item;
			item.Format(rotSerialized.IsEmpty() ? _T("%d:%d") : _T(",%d:%d"), pageIdx, normDeg);
			rotSerialized += item;
		}
	}
	pApp->WriteProfileString(_T("ReadingPosition"), key + _T("_Rotations"), rotSerialized);
}

void CXiaoGongPDFDlg::RestoreReadingPosition(CPDFDocument* pDoc)
{
	if (!pDoc || pDoc->GetFilePath().IsEmpty())
		return;
	CString key = ReadingStateKey(pDoc->GetFilePath());
	CWinApp* pApp = AfxGetApp();

	// 恢复页面视图模式（单页连续 / 双页并排）
	int layoutMode = static_cast<int>(pApp->GetProfileInt(_T("ReadingPosition"),
		key + _T("_Layout"), LAYOUT_SINGLE_CONTINUOUS));
	if (layoutMode != LAYOUT_SINGLE_CONTINUOUS && layoutMode != LAYOUT_TWO_PAGE_SPREAD)
		layoutMode = LAYOUT_SINGLE_CONTINUOUS;
	pDoc->SetLayoutMode(static_cast<PageLayoutMode>(layoutMode));

	// 恢复单页旋转角度（须在计算最大页面宽度与滚动恢复前应用）
	CString rotSerialized = pApp->GetProfileString(_T("ReadingPosition"), key + _T("_Rotations"));
	if (!rotSerialized.IsEmpty())
	{
		int tokenPos = 0;
		CString token = rotSerialized.Tokenize(_T(","), tokenPos);
		while (!token.IsEmpty())
		{
			const int colon = token.Find(_T(':'));
			if (colon > 0)
			{
				const int pageIdx = _ttoi(token.Left(colon));
				const int deg = ((_ttoi(token.Mid(colon + 1)) % 360) + 360) % 360;
				if (pageIdx >= 0 && pageIdx < pDoc->GetTotalPages() &&
					(deg == 90 || deg == 180 || deg == 270))
				{
					pDoc->SetPageRotation(pageIdx, deg);
				}
			}
			token = rotSerialized.Tokenize(_T(","), tokenPos);
		}
	}

	int page = static_cast<int>(pApp->GetProfileInt(_T("ReadingPosition"),
		key + _T("_Page"), -1));
	if (page < 0 || page >= pDoc->GetTotalPages())
		return;
	pDoc->SetCurrentPage(page);
	pDoc->SetScrollPosition(static_cast<int>(pApp->GetProfileInt(
		_T("ReadingPosition"), key + _T("_Scroll"), 0)));
	int mode = static_cast<int>(pApp->GetProfileInt(_T("ReadingPosition"),
		key + _T("_Mode"), ZOOM_CUSTOM));
	if (mode < ZOOM_CUSTOM || mode > ZOOM_FIT_PAGE)
		mode = ZOOM_CUSTOM;
	float zoom = pApp->GetProfileInt(_T("ReadingPosition"),
		key + _T("_Zoom"), 10000) / 10000.0f;
	float scale = pApp->GetProfileInt(_T("ReadingPosition"),
		key + _T("_Scale"), 0) / 10000.0f;
	if (zoom > 0.0f && zoom <= 20.0f && scale >= 0.0f && scale <= 20.0f)
		pDoc->RestoreReadingZoom(static_cast<ZoomMode>(mode), zoom, scale);
}

void CXiaoGongPDFDlg::SaveOpenTabs()
{
	CWinApp* pApp = AfxGetApp();
	const int count = static_cast<int>(m_documents.size());
	for (int i = 0; i < count; ++i)
	{
		CString key;
		key.Format(_T("File%d"), i);
		pApp->WriteProfileString(_T("OpenTabs"), key, m_documents[i]->GetFilePath());
	}
	pApp->WriteProfileInt(_T("OpenTabs"), _T("Active"), m_activeDocIndex);
	pApp->WriteProfileInt(_T("OpenTabs"), _T("Count"), count);
}

void CXiaoGongPDFDlg::RestoreOpenTabs()
{
	CWinApp* pApp = AfxGetApp();
	const int count = min(1000, static_cast<int>(pApp->GetProfileInt(
		_T("OpenTabs"), _T("Count"), 0)));
	const int active = static_cast<int>(pApp->GetProfileInt(
		_T("OpenTabs"), _T("Active"), -1));
	for (int i = 0; i < count; ++i)
	{
		CString key;
		key.Format(_T("File%d"), i);
		CString path = pApp->GetProfileString(_T("OpenTabs"), key);
		if (i == active)
			m_sessionActivePath = path;
		if (!path.IsEmpty() && PathFileExists(path))
			m_sessionRestorePaths.push_back(path);
	}
	// 命令行/关联打开的文件：全部排入恢复队列，首个作为活动标签。
	if (m_initialFilePaths.empty() && !m_initialFilePath.IsEmpty())
		m_initialFilePaths.push_back(m_initialFilePath);
	if (!m_initialFilePaths.empty())
	{
		m_sessionActivePath = m_initialFilePaths.front();
		for (const CString& initialPath : m_initialFilePaths)
		{
			bool alreadyQueued = false;
			for (const CString& path : m_sessionRestorePaths)
			{
				if (FullPathForComparison(path).CompareNoCase(
					FullPathForComparison(initialPath)) == 0)
				{
					alreadyQueued = true;
					break;
				}
			}
			if (!alreadyQueued)
				m_sessionRestorePaths.push_back(initialPath);
		}
	}
}

void CXiaoGongPDFDlg::OpenNextRestoredTab()
{
	if (m_sessionRestorePaths.empty())
		return;
	while (m_sessionRestoreIndex < m_sessionRestorePaths.size() && m_pendingLoads.size() < 4)
	{
		const CString path = m_sessionRestorePaths[m_sessionRestoreIndex++];
		if (!PathFileExists(path) && path.CompareNoCase(m_initialFilePath) != 0)
			continue;
		bool alreadyOpen = false;
		for (const CPDFDocument* pDoc : m_documents)
		{
			if (FullPathForComparison(pDoc->GetFilePath()).CompareNoCase(
				FullPathForComparison(path)) == 0)
			{
				alreadyOpen = true;
				break;
			}
		}
		if (!alreadyOpen)
			OpenPDFInNewTab(path, true);
	}
	if (m_sessionRestoreIndex < m_sessionRestorePaths.size() || !m_pendingLoads.empty())
		return;
	for (int i = 0; i < static_cast<int>(m_documents.size()); ++i)
	{
		if (FullPathForComparison(m_documents[i]->GetFilePath()).CompareNoCase(
			FullPathForComparison(m_sessionActivePath)) == 0)
		{
			m_tabCtrl.SetCurSel(i);
			SwitchToDocument(i);
			break;
		}
	}
	m_sessionRestorePaths.clear();
	m_sessionRestoreIndex = 0;  // 重置游标，否则下一批入队路径会被跳过
	m_sessionActivePath.Empty();
}

void CXiaoGongPDFDlg::CloseProgressDialog(PDFLoadParams* pParams)
{
	if (!pParams || !pParams->pProgressDlg)
		return;

	if (pParams->pProgressDlg->GetSafeHwnd() && ::IsWindow(pParams->pProgressDlg->GetSafeHwnd()))
		pParams->pProgressDlg->DestroyWindow();

	delete pParams->pProgressDlg;
	pParams->pProgressDlg = nullptr;
}

void CXiaoGongPDFDlg::ReleaseLoadThread(PDFLoadParams* pParams)
{
	if (!pParams || !pParams->pThread)
		return;

	if (pParams->pThread->m_hThread)
		WaitForSingleObject(pParams->pThread->m_hThread, INFINITE);

	delete pParams->pThread;
	pParams->pThread = nullptr;
}

void CXiaoGongPDFDlg::CleanupPendingLoad()
{
	for (PDFLoadParams* pParams : m_pendingLoads)
		pParams->cancelled.store(true);
	while (!m_pendingLoads.empty())
		CleanupPendingLoad(m_pendingLoads.back());
}

void CXiaoGongPDFDlg::CleanupPendingLoad(PDFLoadParams* pParams)
{
	if (!pParams)
		return;
	pParams->cancelled.store(true);
	ReleaseLoadThread(pParams);
	CloseProgressDialog(pParams);
	auto it = std::find(m_pendingLoads.begin(), m_pendingLoads.end(), pParams);
	if (it != m_pendingLoads.end())
		m_pendingLoads.erase(it);
	delete pParams->pDoc;
	delete pParams;
}

void CXiaoGongPDFDlg::CleanupBitmap()
{
	// 先解除控件中的位图引用，再销毁位图句柄，避免重绘期间访问已释放的 HBITMAP
	if (m_pdfView.GetSafeHwnd())
		m_pdfView.SetDisplayBitmap(NULL);

	SafeDeleteBitmap(m_hContinuousViewBitmap);
}

void CXiaoGongPDFDlg::SafeDeleteBitmap(HBITMAP& hBmp)
{
	if (hBmp) {
		DeleteObject(hBmp);
		hBmp = NULL;
	}
}

void CXiaoGongPDFDlg::SetControlsVisible(bool visible)
{
	int nShow = visible ? SW_SHOW : SW_HIDE;
	CWnd* controls[] = {
		&m_toolbar, &m_tabCtrl, &m_btnOpen, &m_btnZoomOut, &m_btnFitWidth, &m_btnFitPage,
		&m_btnZoomIn, &m_btnMore, &m_btnMinimize, &m_btnMaximize,
		&m_btnCloseWindow, &m_btnFirst, &m_btnPrevPage, &m_btnNextPage,
		&m_btnLast, &m_btnPrint, &m_btnSnapshot, &m_btnPageLayout, &m_btnAnnotate,
		&m_btnFullscreen, &m_btnRotateLeft, &m_btnRotateRight,
		&m_btnEdit, &m_editCurrent, &m_pageTotal,
		&m_statusBar, &m_zoomStatus, &m_btnOutline,
		&m_searchPopup, &m_editSearch, &m_searchMatchCount,
		&m_btnFind, &m_btnPrevMatch, &m_btnNextMatch, &m_btnCloseSearchPopup,
		&m_outlineTree, &m_searchResultsList, &m_searchResultsHeader,
		&m_btnCloseSearchResults
	};
	for (auto* pCtrl : controls)
		if (pCtrl->GetSafeHwnd()) pCtrl->ShowWindow(nShow);
	for (auto& divider : m_toolbarDividers)
		if (divider.GetSafeHwnd()) divider.ShowWindow(nShow);
	if (visible)
	{
		CRect clientRect;
		GetClientRect(&clientRect);
		layoutToolbar(clientRect.Width());
		m_editSearch.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);
		m_searchPopup.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);
		m_searchMatchCount.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);
		m_btnPrevMatch.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);
		m_btnNextMatch.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);
		m_btnCloseSearchPopup.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);
	}
	if (m_outlineTree.GetSafeHwnd())
		m_outlineTree.ShowWindow(visible && m_outlineVisible && m_doc ? SW_SHOW : SW_HIDE);
	if (m_searchResultsList.GetSafeHwnd())
		m_searchResultsList.ShowWindow(visible && m_searchResultsVisible && m_doc ? SW_SHOW : SW_HIDE);
	if (m_searchResultsHeader.GetSafeHwnd())
		m_searchResultsHeader.ShowWindow(visible && m_searchResultsVisible && m_doc ? SW_SHOW : SW_HIDE);
	if (m_btnCloseSearchResults.GetSafeHwnd())
		m_btnCloseSearchResults.ShowWindow(visible && m_searchResultsVisible && m_doc ? SW_SHOW : SW_HIDE);
}

void CXiaoGongPDFDlg::RefreshCurrentView()
{
	m_pdfView.Invalidate(FALSE);
	m_pdfView.UpdateWindow();
}

void CXiaoGongPDFDlg::ScrollToMatchVisible(int pageNum, fz_quad quad)
{
	if (pageNum < 0 || pageNum >= (int)m_pageYPositions.size() || pageNum >= (int)m_pageHeights.size())
		return;

	fz_rect bounds;
	if (!TryGetPageBaseBounds(pageNum, bounds))
		return;

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	const int rotation = GetPageRotation(pageNum);
	CRect matchRect = TransformQuadToBitmapRect(quad, bounds, rotation, m_uniformScale);
	int pageYPos = m_pageYPositions[pageNum];
	int matchTop = pageYPos + matchRect.top;
	int matchBottom = pageYPos + matchRect.bottom;
	int visibleTop = m_scrollPosition;
	int visibleBottom = m_scrollPosition + viewRect.Height();
	int margin = 50;
	bool needScroll = false;
	if (matchTop < visibleTop + margin)
	{
		m_scrollPosition = matchTop - margin;
		needScroll = true;
	}
	else if (matchBottom > visibleBottom - margin)
	{
		m_scrollPosition = matchBottom - viewRect.Height() + margin;
		needScroll = true;
	}

	int curOffsetX = 0;
	int curOffsetY = 0;
	int pagePixelWidth = 0;
	if (GetPageViewPlacement(pageNum, viewRect.Width(), curOffsetX, curOffsetY, &pagePixelWidth, nullptr))
	{
		const int matchLeft = matchRect.left + curOffsetX;
		const int matchRight = matchRect.right + curOffsetX;
		if (m_totalScrollWidth > viewRect.Width() &&
			(matchLeft < margin || matchRight > viewRect.Width() - margin))
		{
			const int targetDeltaX = matchLeft - viewRect.Width() / 3;
			m_scrollPositionH = max(0, min(m_totalScrollWidth - viewRect.Width(),
				m_scrollPositionH + targetDeltaX));
			needScroll = true;
		}
	}

	if (needScroll)
	{
		int maxScroll = m_totalScrollHeight - viewRect.Height();
		if (maxScroll < 0) maxScroll = 0;
		if (m_scrollPosition < 0) m_scrollPosition = 0;
		if (m_scrollPosition > maxScroll) m_scrollPosition = maxScroll;
		UpdateScrollBar();
		RenderVisiblePages();
	}
}

void CXiaoGongPDFDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_TAB_CONTROL, m_tabCtrl);
	DDX_Control(pDX, IDC_PDF_VIEW, m_pdfView);
	DDX_Control(pDX, IDC_TOOLBAR, m_toolbar);
	DDX_Control(pDX, IDC_BTN_FIRST, m_btnFirst);
	DDX_Control(pDX, IDC_BTN_LAST, m_btnLast);
	DDX_Control(pDX, IDC_BTN_FULLSCREEN, m_btnFullscreen);
	DDX_Control(pDX, IDC_BTN_ROTATE_LEFT, m_btnRotateLeft);
	DDX_Control(pDX, IDC_BTN_ROTATE_RIGHT, m_btnRotateRight);
	DDX_Control(pDX, IDC_BTN_EDIT, m_btnEdit);
	DDX_Control(pDX, IDC_CHECK_THUMBNAIL, m_btnOutline);
	DDX_Control(pDX, IDC_EDIT_CURRENT, m_editCurrent);
	DDX_Control(pDX, IDC_PAGE_TOTAL, m_pageTotal);
	DDX_Control(pDX, IDC_STATUS_BAR, m_statusBar);

	// 搜索控件绑定
	DDX_Control(pDX, IDC_EDIT_SEARCH, m_editSearch);
	DDX_Control(pDX, IDC_BTN_FIND, m_btnFind);
	DDX_Control(pDX, IDC_BTN_PREV_MATCH, m_btnPrevMatch);
	DDX_Control(pDX, IDC_BTN_NEXT_MATCH, m_btnNextMatch);
}

BEGIN_MESSAGE_MAP(CXiaoGongPDFDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_CLOSE()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_WM_CTLCOLOR()
	ON_WM_ERASEBKGND()  // ★★★ 拦截背景擦除消息，防止窗口调整大小时的闪烁
	ON_COMMAND(ID_MENU_OPEN, &CXiaoGongPDFDlg::onMenuOpen)
	ON_COMMAND(ID_MENU_EXIT,&CXiaoGongPDFDlg::onMenuExit)
	ON_COMMAND(ID_MENU_ABOUT,&CXiaoGongPDFDlg::onMenuAbout)
	ON_COMMAND(ID_MENU_PRINT, &CXiaoGongPDFDlg::onMenuPrint)
	ON_COMMAND(ID_MENU_SET_DEFAULT, &CXiaoGongPDFDlg::onMenuSetDefault)
	ON_COMMAND(ID_MENU_SHORTCUTS, &CXiaoGongPDFDlg::onMenuShortcuts)
	ON_COMMAND(ID_MENU_REGION_CAPTURE, &CXiaoGongPDFDlg::OnMenuRegionCapture)
	ON_COMMAND(ID_MENU_EXPORT_IMAGES, &CXiaoGongPDFDlg::OnMenuExportImages)
	ON_COMMAND(ID_MENU_DOC_PROPERTIES, &CXiaoGongPDFDlg::OnMenuDocProperties)
	ON_COMMAND(ID_MENU_SAVE_PDF, &CXiaoGongPDFDlg::OnMenuSavePdf)
	ON_COMMAND(ID_MENU_SAVE_PDF_AS, &CXiaoGongPDFDlg::OnMenuSavePdfAs)
	ON_COMMAND_RANGE(ID_MENU_LAYOUT_SINGLE, ID_MENU_LAYOUT_TWO_PAGE, &CXiaoGongPDFDlg::OnMenuPageLayout)
	ON_COMMAND(ID_MENU_ANNOT_HIGHLIGHT, &CXiaoGongPDFDlg::OnMenuAnnotHighlight)
	ON_COMMAND(ID_MENU_ANNOT_UNDERLINE, &CXiaoGongPDFDlg::OnMenuAnnotUnderline)
	ON_COMMAND(ID_MENU_ANNOT_STRIKEOUT, &CXiaoGongPDFDlg::OnMenuAnnotStrikeout)
	ON_COMMAND(ID_MENU_ANNOT_TEXT_NOTE, &CXiaoGongPDFDlg::OnMenuAnnotTextNote)
	ON_COMMAND_RANGE(ID_MENU_READING_NORMAL, ID_MENU_READING_EYE, &CXiaoGongPDFDlg::OnReadingMode)
	ON_COMMAND_RANGE(ID_MENU_RECENT_FILE_1, ID_MENU_RECENT_FILE_10, &CXiaoGongPDFDlg::onMenuRecentFile)
	ON_NOTIFY(TVN_SELCHANGED, IDC_OUTLINE_TREE, &CXiaoGongPDFDlg::OnOutlineSelectionChanged)
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_SEARCH_RESULTS_LIST, &CXiaoGongPDFDlg::OnSearchResultSelected)
	ON_NOTIFY(TCN_SELCHANGE, IDC_TAB_CONTROL, &CXiaoGongPDFDlg::OnTabSelChange)
	ON_MESSAGE(WM_TAB_CLOSE_REQUEST, &CXiaoGongPDFDlg::OnTabCloseButton)  // 标签页关闭消息
	ON_MESSAGE(WM_TAB_REORDER_REQUEST, &CXiaoGongPDFDlg::OnTabReorder)    // 标签页拖拽重排消息
	ON_MESSAGE(WM_TAB_CONTEXT_MENU, &CXiaoGongPDFDlg::OnTabContextMenu)   // 标签页右键菜单消息
	ON_WM_SIZE()
	ON_MESSAGE(WM_USER + 101, &CXiaoGongPDFDlg::OnOpenInitialFile)  // 延迟打开初始文件
	ON_MESSAGE(WM_PDF_LOAD_COMPLETE, &CXiaoGongPDFDlg::OnPDFLoadComplete)  // PDF后台加载完成
	ON_MESSAGE(WM_PDF_LOAD_CANCELLED, &CXiaoGongPDFDlg::OnPDFLoadCancelled)
	ON_MESSAGE(WM_SEARCH_COMPLETE, &CXiaoGongPDFDlg::OnSearchComplete)  // 搜索后台完成
	ON_MESSAGE(WM_SEARCH_PROGRESS, &CXiaoGongPDFDlg::OnSearchProgress)
	ON_MESSAGE(WM_FILE_CHANGED, &CXiaoGongPDFDlg::OnFileChanged)  // 文件变化通知
	ON_WM_TIMER()
	ON_WM_MOUSEWHEEL()
	ON_WM_CAPTURECHANGED()
	ON_BN_CLICKED(IDC_BTN_FIRST, &CXiaoGongPDFDlg::OnBtnFirst)
	ON_BN_CLICKED(IDC_BTN_PREV, &CXiaoGongPDFDlg::OnBtnPrevPage)
	ON_BN_CLICKED(IDC_BTN_NEXT, &CXiaoGongPDFDlg::OnBtnNextPage)
	ON_BN_CLICKED(IDC_BTN_PRINT, &CXiaoGongPDFDlg::onMenuPrint)
	ON_BN_CLICKED(IDC_BTN_SNAPSHOT, &CXiaoGongPDFDlg::OnMenuRegionCapture)
	ON_BN_CLICKED(IDC_BTN_PAGE_LAYOUT, &CXiaoGongPDFDlg::OnBtnPageLayout)
	ON_BN_CLICKED(IDC_BTN_ANNOTATE, &CXiaoGongPDFDlg::OnBtnAnnotate)
	ON_BN_CLICKED(IDC_BTN_OPEN, &CXiaoGongPDFDlg::onMenuOpen)
	ON_BN_CLICKED(IDC_BTN_ZOOM_OUT, &CXiaoGongPDFDlg::OnBtnZoomOut)
	ON_BN_CLICKED(IDC_BTN_FIT_WIDTH, &CXiaoGongPDFDlg::OnBtnFitWidth)
	ON_BN_CLICKED(IDC_BTN_FIT_PAGE, &CXiaoGongPDFDlg::OnBtnToggleFit)
	ON_BN_CLICKED(IDC_BTN_ZOOM_IN, &CXiaoGongPDFDlg::OnBtnZoomIn)
	ON_BN_CLICKED(IDC_BTN_MORE, &CXiaoGongPDFDlg::OnBtnMore)
	ON_BN_CLICKED(IDC_BTN_CLOSE_SEARCH_RESULTS, &CXiaoGongPDFDlg::OnCloseSearchResults)
	ON_BN_CLICKED(IDC_BTN_CLOSE_SEARCH_POPUP, &CXiaoGongPDFDlg::OnCloseSearchPopup)
	ON_BN_CLICKED(IDC_BTN_MINIMIZE, &CXiaoGongPDFDlg::OnBtnMinimize)
	ON_BN_CLICKED(IDC_BTN_MAXIMIZE, &CXiaoGongPDFDlg::OnBtnMaximize)
	ON_BN_CLICKED(IDC_BTN_CLOSE_WINDOW, &CXiaoGongPDFDlg::OnBtnCloseWindow)
	ON_BN_CLICKED(IDC_BTN_LAST, &CXiaoGongPDFDlg::OnBtnLast)
	ON_BN_CLICKED(IDC_BTN_FULLSCREEN, &CXiaoGongPDFDlg::OnBtnFullscreen)
	ON_BN_CLICKED(IDC_BTN_ROTATE_LEFT, &CXiaoGongPDFDlg::OnBtnRotateLeft)
	ON_BN_CLICKED(IDC_BTN_ROTATE_RIGHT, &CXiaoGongPDFDlg::OnBtnRotateRight)
	ON_BN_CLICKED(IDC_BTN_EDIT, &CXiaoGongPDFDlg::OnBtnEdit)
	ON_BN_CLICKED(IDC_CHECK_THUMBNAIL, &CXiaoGongPDFDlg::OnCheckThumbnail)
	ON_EN_CHANGE(IDC_EDIT_CURRENT, &CXiaoGongPDFDlg::OnEnChangeEditCurrent)
	ON_EN_KILLFOCUS(IDC_EDIT_CURRENT, &CXiaoGongPDFDlg::OnEditCurrentKillFocus)
	ON_EN_CHANGE(IDC_INLINE_ANNOT_EDIT, &CXiaoGongPDFDlg::OnEnChangeInlineAnnotEdit)
	ON_EN_CHANGE(IDC_EDIT_SEARCH, &CXiaoGongPDFDlg::OnEnChangeEditSearch)
	ON_BN_CLICKED(IDC_BTN_FIND, &CXiaoGongPDFDlg::OnBtnFind)
	ON_BN_CLICKED(IDC_BTN_PREV_MATCH, &CXiaoGongPDFDlg::OnBtnPrevMatch)
	ON_BN_CLICKED(IDC_BTN_NEXT_MATCH, &CXiaoGongPDFDlg::OnBtnNextMatch)
	ON_WM_KEYDOWN()
	ON_WM_GETMINMAXINFO()
	ON_WM_DROPFILES()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_MBUTTONDOWN()
	ON_WM_MBUTTONUP()
	ON_WM_MOUSEMOVE()
	ON_WM_CONTEXTMENU()
	ON_WM_SETCURSOR()
	ON_WM_COPYDATA()
	ON_WM_DESTROY()
	ON_WM_VSCROLL()  // ★★★ 添加垂直滚动条消息处理（连续滚动模式）
	ON_WM_HSCROLL()  // ★★★ 添加横向滚动条消息处理
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
	DWORD pathLength = GetModuleFileName(NULL, szPath, MAX_PATH);
	if (pathLength == 0 || pathLength >= MAX_PATH)
	{
		MessageBox(_T("无法获取程序路径。"), _T("错误"), MB_OK | MB_ICONERROR);
		return;
	}

	CString exePath = szPath;
	CString commandLine;
	// 设置命令行格式，%1 会被 Windows 替换为文件路径
	commandLine.Format(_T("\"%s\" \"%%1\""), exePath);

	// 设置注册表项，将此程序设为PDF默认阅读器
	HKEY hKey;
	LONG result;
	DWORD dwDisposition;
	bool registered = true;

	// 注册程序标识符；系统默认应用由用户在系统设置中选择。
	result = RegCreateKeyEx(HKEY_CURRENT_USER, _T("Software\\Classes\\XiaoGongPDF.Document"), 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		registered &= RegSetValueEx(hKey, NULL, 0, REG_SZ, (BYTE*)_T("PDF Document"),
			(DWORD)((_tcslen(_T("PDF Document")) + 1) * sizeof(TCHAR))) == ERROR_SUCCESS;
		RegCloseKey(hKey);
	}
	else registered = false;

	// 2.5. 设置默认图标
	CString iconPath;
	iconPath.Format(_T("\"%s\",0"), exePath);  // 使用程序的第一个图标资源
	result = RegCreateKeyEx(HKEY_CURRENT_USER, _T("Software\\Classes\\XiaoGongPDF.Document\\DefaultIcon"),
		0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		registered &= RegSetValueEx(hKey, NULL, 0, REG_SZ, (BYTE*)(LPCTSTR)iconPath,
			(iconPath.GetLength() + 1) * sizeof(TCHAR)) == ERROR_SUCCESS;
		RegCloseKey(hKey);
	}
	else registered = false;

	// 3. 设置打开命令
	result = RegCreateKeyEx(HKEY_CURRENT_USER, _T("Software\\Classes\\XiaoGongPDF.Document\\shell\\open\\command"),
		0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		registered &= RegSetValueEx(hKey, NULL, 0, REG_SZ, (BYTE*)(LPCTSTR)commandLine,
			(commandLine.GetLength() + 1) * sizeof(TCHAR)) == ERROR_SUCCESS;
		RegCloseKey(hKey);
	}
	else registered = false;

	// 5. 注册应用程序能力
	CString appName = _T("XiaoGongPDF.exe");
	CString capabilitiesPath;
	capabilitiesPath.Format(_T("Software\\%s\\Capabilities"), appName);

	result = RegCreateKeyEx(HKEY_CURRENT_USER, capabilitiesPath, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		CString appDesc = _T("小龚PDF阅读器");
		registered &= RegSetValueEx(hKey, _T("ApplicationDescription"), 0, REG_SZ,
			(BYTE*)(LPCTSTR)appDesc, (appDesc.GetLength() + 1) * sizeof(TCHAR)) == ERROR_SUCCESS;
		registered &= RegSetValueEx(hKey, _T("ApplicationName"), 0, REG_SZ,
			(BYTE*)(LPCTSTR)appDesc, (appDesc.GetLength() + 1) * sizeof(TCHAR)) == ERROR_SUCCESS;
		RegCloseKey(hKey);
	}
	else registered = false;

	// 6. 注册文件关联
	CString fileAssocPath;
	fileAssocPath.Format(_T("%s\\FileAssociations"), capabilitiesPath);
	result = RegCreateKeyEx(HKEY_CURRENT_USER, fileAssocPath, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		registered &= RegSetValueEx(hKey, _T(".pdf"), 0, REG_SZ, (BYTE*)_T("XiaoGongPDF.Document"),
			(DWORD)((_tcslen(_T("XiaoGongPDF.Document")) + 1) * sizeof(TCHAR))) == ERROR_SUCCESS;
		RegCloseKey(hKey);
	}
	else registered = false;

	// 7. 注册到已注册应用程序列表
	CString regAppsPath = _T("Software\\RegisteredApplications");
	result = RegCreateKeyEx(HKEY_CURRENT_USER, regAppsPath, 0, NULL,
		REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, &dwDisposition);
	if (result == ERROR_SUCCESS)
	{
		CString capabilitiesRegPath;
		capabilitiesRegPath.Format(_T("Software\\%s\\Capabilities"), appName);
		registered &= RegSetValueEx(hKey, appName, 0, REG_SZ, (BYTE*)(LPCTSTR)capabilitiesRegPath,
			(capabilitiesRegPath.GetLength() + 1) * sizeof(TCHAR)) == ERROR_SUCCESS;
		RegCloseKey(hKey);
	}
	else registered = false;
	if (!registered)
	{
		MessageBox(_T("注册 PDF 阅读器失败，请检查当前用户的注册表权限。"),
			_T("错误"), MB_OK | MB_ICONERROR);
		return;
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
					// Windows 10 及以上，由用户在系统设置中完成默认应用选择。
					HINSTANCE result = ShellExecute(NULL, _T("open"),
						_T("ms-settings:defaultapps"), NULL, NULL, SW_SHOW);
					if ((INT_PTR)result <= 32)
						MessageBox(_T("已注册阅读器，但无法打开默认应用设置。请手动在系统设置中选择。"),
							_T("提示"), MB_OK | MB_ICONINFORMATION);
					return;
				}
			}
		}
	}

	MessageBox(_T("阅读器已注册。请在系统的“打开方式”中选择小龚PDF阅读器作为默认应用。"),
		_T("提示"), MB_OK | MB_ICONINFORMATION);
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
	::SetProp(GetSafeHwnd(), XIAOGONG_MAINWND_PROP, reinterpret_cast<HANDLE>(1));

	// 标签栏承担标题区，保留系统窗口的缩放边框。
	// SS_WHITERECT ignores WM_CTLCOLORSTATIC and always paints the toolbar white.
	m_toolbar.ModifyStyle(SS_TYPEMASK, SS_LEFT | WS_CLIPSIBLINGS, SWP_FRAMECHANGED);
	ModifyStyle(0, WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN);
	ModifyStyle(WS_CAPTION, 0, SWP_FRAMECHANGED);

	// ★★★ 禁用DWM窗口过渡动画，防止最大化→还原时DWM短暂显示旧大尺寸内容导致的边界溢出
	BOOL fDisableTransitions = TRUE;
	DwmSetWindowAttribute(GetSafeHwnd(), DWMWA_TRANSITIONS_FORCEDISABLED,
		&fDisableTransitions, sizeof(fDisableTransitions));

	SetIcon(m_hIcon, TRUE);
	SetIcon(m_hIcon, FALSE);

	// 显示PDF 需要设置控件类型如下
	// 设置 Picture Control 样式
	DWORD style = m_pdfView.GetStyle();
	style = (style & ~SS_TYPEMASK & ~WS_VSCROLL & ~WS_HSCROLL) |
		SS_BITMAP | SS_CENTERIMAGE | SS_NOTIFY;
	::SetWindowLong(m_pdfView.GetSafeHwnd(), GWL_STYLE, style);

	// 更新控件
	m_pdfView.ModifyStyle(WS_VSCROLL | WS_HSCROLL,
		SS_NOTIFY | SS_CENTERIMAGE | WS_CLIPSIBLINGS);

	// ★★★ 启用WS_EX_COMPOSITED扩展样式，使用桌面合成来减少高刷新率显示器上的闪烁
	// 这会让Windows使用双缓冲合成来渲染控件，避免直接绘制时的闪白问题
	m_pdfView.ModifyStyleEx(0, WS_EX_COMPOSITED);

	// ★★★ 使用FALSE参数避免擦除背景，减少闪烁
	m_pdfView.Invalidate(FALSE);
	m_pdfView.UpdateWindow();

	// ★★★ 初始化自定义PDF预览控件，使其能够转发滚动条消息
	m_pdfView.SetParentDlg(this);
	m_pdfVerticalScrollBar.SetParentDlg(this);
	m_pdfVerticalScrollBar.CreateEx(0, AfxRegisterWndClass(CS_DBLCLKS,
		::LoadCursor(nullptr, IDC_ARROW)), _T("PDF vertical scrollbar"),
		WS_CHILD, CRect(0, 0, 16, 100), this, 0);
	m_pdfHorizontalScrollBar.SetParentDlg(this);
	m_pdfHorizontalScrollBar.CreateEx(0, AfxRegisterWndClass(CS_DBLCLKS,
		::LoadCursor(nullptr, IDC_ARROW)), _T("PDF horizontal scrollbar"),
		WS_CHILD, CRect(0, 0, 100, 16), this, 0);
	m_pdfScrollCorner.Create(_T(""), WS_CHILD | SS_LEFT,
		CRect(0, 0, 16, 16), this, 0);

	// 初始状态隐藏滚动条（未加载PDF时不显示）
	m_pdfView.ShowScrollBar(SB_VERT, FALSE);
	m_pdfView.ShowScrollBar(SB_HORZ, FALSE);

	// 获取系统垂直滚动条的宽度
	m_scrollBarWidth = GetSystemMetrics(SM_CXVSCROLL);

	if (CWnd* pLegacyThumbList = GetDlgItem(IDC_THUMBNAIL_LIST))
		pLegacyThumbList->ShowWindow(SW_HIDE);
	if (CWnd* pPathStrip = GetDlgItem(IDC_EDIT_FILE_PATH))
		pPathStrip->ShowWindow(SW_HIDE);
	m_outlineTree.Create(WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
		TVS_LINESATROOT | TVS_SHOWSELALWAYS, CRect(0, 0, 250, 300), this,
		IDC_OUTLINE_TREE);
	m_searchResultsHeader.Create(_T("搜索结果"), WS_CHILD | SS_LEFT | SS_CENTERIMAGE,
		CRect(0, 0, 280, 34), this, IDC_SEARCH_RESULTS_HEADER);
	m_btnCloseSearchResults.Create(_T("关闭"), WS_CHILD | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_CLOSE_SEARCH_RESULTS);
	m_searchResultsList.Create(WS_CHILD | WS_TABSTOP | WS_BORDER | LVS_REPORT |
		LVS_SINGLESEL | LVS_SHOWSELALWAYS, CRect(0, 0, 300, 300), this,
		IDC_SEARCH_RESULTS_LIST);
	m_searchResultsList.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	m_searchResultsList.InsertColumn(0, _T("页码"), LVCFMT_LEFT, 58);
	m_searchResultsList.InsertColumn(1, _T("上下文"), LVCFMT_LEFT, 230);
	m_searchPopup.Create(_T(""), WS_CHILD | WS_CLIPSIBLINGS | WS_BORDER,
		CRect(0, 0, 430, 54), this, IDC_SEARCH_POPUP);
	m_searchMatchCount.Create(_T("0/0"), WS_CHILD | SS_CENTER | SS_CENTERIMAGE,
		CRect(0, 0, 58, 34), this, IDC_SEARCH_MATCH_COUNT);
	m_btnCloseSearchPopup.Create(_T("关闭"), WS_CHILD | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_CLOSE_SEARCH_POPUP);

	// 初始化页面内联透明文字批注输入代理（接管中文输入法 IME、剪贴板与光标导航）
	m_inlineAnnotEdit.CreateHiddenProxy(this, IDC_INLINE_ANNOT_EDIT);
	m_inlineAnnotEdit.m_onTextOrCaretChanged = [this]()
	{
		if (m_inlineAnnot.active)
		{
			m_inlineAnnot.caretVisible = true;
			InvalidateSelection();
		}
	};
	m_inlineAnnotEdit.m_onEscapePressed = [this]()
	{
		if (!m_inlineAnnot.active)
			return;
		if (m_inlineAnnot.colorPopupOpen)
		{
			m_inlineAnnot.colorPopupOpen = false;
			InvalidateSelection();
			return;
		}
		FinishInlineTextAnnot(false);
	};
	m_inlineAnnotEdit.m_onVerticalOrLineNav = [this](UINT vk, bool shift) -> bool
	{
		return HandleInlineAnnotKeyNav(vk, shift);
	};

	// 初始化状态栏
	m_statusBar.SetWindowText(_T("  就绪"));
	m_statusBar.ModifyStyle(SS_TYPEMASK | WS_BORDER, SS_LEFT | SS_CENTERIMAGE, SWP_FRAMECHANGED);
	m_statusBar.ShowWindow(SW_SHOW);
	m_zoomStatus.Create(_T(""), WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE | WS_BORDER,
		CRect(0, 0, 86, 25), this, IDC_STATUS_ZOOM);

	// MuPDF context 由每个 CPDFDocument 自己持有，主对话框只引用当前活动文档。

	// 初始化工具栏
	m_btnOpen.Create(_T("打开"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW,
		CRect(0, 0, 72, 34), this, IDC_BTN_OPEN);
	m_btnZoomOut.Create(_T("-"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_ZOOM_OUT);
	m_btnFitWidth.Create(_T("适合宽度"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
		CRect(0, 0, 76, 34), this, IDC_BTN_FIT_WIDTH);
	m_btnFitPage.Create(_T("适合页面"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
		CRect(0, 0, 76, 34), this, IDC_BTN_FIT_PAGE);
	m_btnZoomIn.Create(_T("+"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_ZOOM_IN);
	m_btnPrevPage.Create(_T("上一页"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_PREV);
	m_btnNextPage.Create(_T("下一页"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_NEXT);
	m_btnPrint.Create(_T("打印"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_PRINT);
	m_btnSnapshot.Create(_T("截图"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_SNAPSHOT);
	m_btnPageLayout.Create(_T("双页"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_PAGE_LAYOUT);
	m_btnAnnotate.Create(_T("批注"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_ANNOTATE);
	for (auto& divider : m_toolbarDividers)
		divider.Create(_T(""), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_ETCHEDVERT,
			CRect(0, 0, 2, 24), this);
	m_btnMore.Create(_T("更多"), WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_MORE);
	m_btnMinimize.Create(_T("最小化"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		CRect(0, 0, 46, TITLE_BAR_HEIGHT), this, IDC_BTN_MINIMIZE);
	m_btnMaximize.Create(_T("最大化"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		CRect(0, 0, 46, TITLE_BAR_HEIGHT), this, IDC_BTN_MAXIMIZE);
	m_btnCloseWindow.Create(_T("关闭"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		CRect(0, 0, 46, TITLE_BAR_HEIGHT), this, IDC_BTN_CLOSE_WINDOW);
	m_btnMinimize.SetWindowControlStyle();
	m_btnMaximize.SetWindowControlStyle();
	m_btnCloseWindow.SetWindowControlStyle(true);
	InitializeToolbar();
	m_editSearch.ShowWindow(SW_HIDE);
	m_searchPopup.ShowWindow(SW_HIDE);
	m_searchMatchCount.ShowWindow(SW_HIDE);
	m_btnPrevMatch.ShowWindow(SW_HIDE);
	m_btnNextMatch.ShowWindow(SW_HIDE);
	m_btnCloseSearchPopup.ShowWindow(SW_HIDE);
	m_btnFitWidth.ShowWindow(SW_HIDE);
	m_btnRotateLeft.ShowWindow(SW_HIDE);
	m_statusBar.SetFont(&m_buttonFont);
	m_zoomStatus.SetFont(&m_buttonFont);
	m_outlineTree.SetFont(&m_buttonFont);
	m_searchResultsHeader.SetFont(&m_buttonFont);
	m_searchResultsList.SetFont(&m_buttonFont);
	m_searchMatchCount.SetFont(&m_buttonFont);
	m_btnCloseSearchPopup.SetIconGlyph(0xE8BB);
	m_btnCloseSearchPopup.SetToolTipText(_T("关闭查询"));
	m_btnCloseSearchPopup.SetColors(RGB(255, 255, 255), RGB(232, 241, 252), RGB(211, 229, 250));
	m_btnCloseSearchResults.SetIconGlyph(0xE8BB);
	m_btnCloseSearchResults.SetToolTipText(_T("关闭搜索结果"));
	m_btnCloseSearchResults.SetColors(RGB(255, 255, 255), RGB(232, 241, 252), RGB(211, 229, 250));

	m_btnOutline.SetWindowText(_T("目录"));

	// 初始化标签页控件
	InitializeTabControl();

	// 设置对话框为可接收键盘消息的窗口
	SetFocus();

	// 启用拖放文件功能
	DragAcceptFiles(TRUE);

	// 窗口显示后再恢复上次的标签页和命令行文件。
	PostMessage(WM_USER + 101, 0, 0);


	// 工具栏图标采用无边框常态，交互时显示浅蓝反馈。
	const COLORREF normal = RGB(255, 255, 255);
	const COLORREF hover = RGB(232, 241, 252);
	const COLORREF pressed = RGB(211, 229, 250);
	m_btnOpen.SetColors(normal, hover, pressed);
	m_btnZoomOut.SetColors(normal, hover, pressed);
	m_btnFitWidth.SetColors(normal, hover, pressed);
	m_btnFitPage.SetColors(normal, hover, pressed);
	m_btnZoomIn.SetColors(normal, hover, pressed);
	m_btnMore.SetColors(RGB(240, 244, 248), RGB(224, 231, 238), RGB(207, 218, 228));
	m_btnOutline.SetColors(normal, hover, pressed);
	m_btnMinimize.SetColors(RGB(240, 244, 248), RGB(224, 231, 238), RGB(207, 218, 228));
	m_btnMaximize.SetColors(RGB(240, 244, 248), RGB(224, 231, 238), RGB(207, 218, 228));
	m_btnCloseWindow.SetColors(RGB(240, 244, 248), RGB(232, 38, 51), RGB(192, 25, 38));
	m_btnMinimize.SetTextColor(RGB(38, 45, 53));
	m_btnMaximize.SetTextColor(RGB(38, 45, 53));
	m_btnCloseWindow.SetTextColor(RGB(38, 45, 53));
	m_btnFirst.SetColors(normal, hover, pressed);
	m_btnLast.SetColors(normal, hover, pressed);
	m_btnFullscreen.SetColors(normal, hover, pressed);
	m_btnRotateLeft.SetColors(normal, hover, pressed);
	m_btnRotateRight.SetColors(normal, hover, pressed);
	m_btnEdit.SetColors(normal, hover, pressed);
	m_btnSnapshot.SetColors(normal, hover, pressed);
	m_btnPageLayout.SetColors(normal, hover, pressed);
	m_btnAnnotate.SetColors(normal, hover, pressed);
	m_btnFind.SetColors(normal, hover, pressed);
	m_btnPrevMatch.SetColors(normal, hover, pressed);
	m_btnNextMatch.SetColors(normal, hover, pressed);

	// 深色文字保持清晰对比度。
	m_btnFirst.SetTextColor     (RGB(32, 33, 36));
	m_btnOpen.SetTextColor(RGB(26, 45, 65));
	m_btnLast.SetTextColor      (RGB(32, 33, 36));
	m_btnFullscreen.SetTextColor(RGB(32, 33, 36));
	m_btnRotateLeft.SetTextColor(RGB(32, 33, 36));
	m_btnRotateRight.SetTextColor(RGB(32, 33, 36));
	m_btnEdit.SetTextColor      (RGB(32, 33, 36));
	m_btnSnapshot.SetTextColor  (RGB(32, 33, 36));
	m_btnPageLayout.SetTextColor(RGB(32, 33, 36));
	m_btnAnnotate.SetTextColor  (RGB(32, 33, 36));
	m_btnFind.SetTextColor      (RGB(32, 33, 36));
	m_btnPrevMatch.SetTextColor (RGB(32, 33, 36));
	m_btnNextMatch.SetTextColor (RGB(32, 33, 36));

	// ===== 颜色配置结束 =====
	const int savedMode = AfxGetApp()->GetProfileInt(_T("Appearance"), _T("ReadingMode"), 0);
	ApplyReadingMode(savedMode >= READING_NORMAL && savedMode <= READING_EYE ?
		static_cast<ReadingMode>(savedMode) : READING_NORMAL);
	CRect clientRect;
	GetClientRect(&clientRect);
	// OnSize 的基类处理依赖当前消息；初始化结束后再发送真正的 WM_SIZE。
	PostMessage(WM_SIZE, SIZE_RESTORED,
		MAKELPARAM(clientRect.Width(), clientRect.Height()));
	return TRUE;  // 除非将焦点设置到控件，否则返回 TRUE
} 

void CXiaoGongPDFDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	CDialogEx::OnSysCommand(nID, lParam);
}

void CXiaoGongPDFDlg::OnPaint()
{
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

		CDC* pTitleDC = GetDC();
		if (pTitleDC)
		{
			// 填充左上角应用图标区域背景并居中绘制放大后的高清图标
			pTitleDC->FillSolidRect(0, 0, TITLE_ICON_AREA_WIDTH, TITLE_BAR_HEIGHT,
				m_readingMode == READING_NIGHT ? RGB(36, 39, 43) : RGB(230, 238, 253));
			::DrawIconEx(pTitleDC->GetSafeHdc(),
				(TITLE_ICON_AREA_WIDTH - TITLE_ICON_SIZE) / 2,
				(TITLE_BAR_HEIGHT - TITLE_ICON_SIZE) / 2,
				m_hTitleIcon ? m_hTitleIcon : m_hIcon,
				TITLE_ICON_SIZE, TITLE_ICON_SIZE, 0, nullptr, DI_NORMAL);
			ReleaseDC(pTitleDC);
		}

		// 在工具栏底部绘制 1px 分隔线
		if (m_toolbar.GetSafeHwnd())
		{
			CRect toolbarRect;
			m_toolbar.GetWindowRect(&toolbarRect);
			ScreenToClient(&toolbarRect);

			CDC* pDC = GetDC();
			if (pDC)
			{
				CPen pen(PS_SOLID, 1, m_readingMode == READING_NIGHT ?
					RGB(65, 68, 72) : RGB(212, 224, 246));
				CPen* pOldPen = pDC->SelectObject(&pen);
				pDC->MoveTo(toolbarRect.left,  toolbarRect.bottom);
				pDC->LineTo(toolbarRect.right, toolbarRect.bottom);
				pDC->SelectObject(pOldPen);
				ReleaseDC(pDC);
			}
		}
	}
}

// ★★★ 拦截背景擦除消息，防止窗口调整大小时的闪烁（高刷新率显示器优化）
BOOL CXiaoGongPDFDlg::OnEraseBkgnd(CDC* pDC)
{
	// 不完全跳过背景擦除，而是用系统对话框背景色填充
	// 这样可以避免黑色区域，同时仍然减少闪烁
	CRect rect;
	GetClientRect(&rect);

	CBrush brush(ViewBgColor());
	pDC->FillRect(&rect, &brush);
	pDC->FillSolidRect(0, 0, rect.Width(), TITLE_BAR_HEIGHT,
		m_readingMode == READING_NIGHT ? RGB(36, 39, 43) : RGB(230, 238, 253));
	if (!m_isFullscreen)
	{
		pDC->FillSolidRect(0, TITLE_BAR_HEIGHT, rect.Width(), 52,
			m_readingMode == READING_NIGHT ? RGB(39, 43, 47) : RGB(255, 255, 255));
	}

	return TRUE;  // 返回TRUE表示我们已经处理了背景擦除
}

HBRUSH CXiaoGongPDFDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	HBRUSH hbr = CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);
	const bool dark = m_readingMode == READING_NIGHT;
	const COLORREF toolbar = dark ? RGB(39, 43, 47) : RGB(255, 255, 255);
	const COLORREF text = dark ? RGB(226, 231, 235) : RGB(32, 33, 36);

	// 标签区与工具栏分别使用柔和灰色和白色。
	if (pWnd->GetSafeHwnd() == m_tabCtrl.GetSafeHwnd())
	{
		pDC->SetBkColor(dark ? RGB(36, 39, 43) : RGB(230, 238, 253));
		return m_whiteBrush;
	}
	if (pWnd->GetSafeHwnd() == m_pdfScrollCorner.GetSafeHwnd())
	{
		pDC->SetBkColor(dark ? RGB(39, 43, 47) : RGB(255, 255, 255));
		return m_toolbarBrush;
	}
	if (pWnd->GetSafeHwnd() == m_statusBar.GetSafeHwnd() ||
		pWnd->GetSafeHwnd() == m_zoomStatus.GetSafeHwnd())
	{
		pDC->SetBkColor(toolbar);
		pDC->SetTextColor(dark ? text : RGB(75, 88, 102));
		return m_toolbarBrush;
	}
	if (pWnd->GetSafeHwnd() == m_searchPopup.GetSafeHwnd())
	{
		return m_searchPopupBrush;
	}
	if (pWnd->GetSafeHwnd() == m_searchMatchCount.GetSafeHwnd())
	{
		const COLORREF popupBg = dark ? RGB(50, 54, 58) : RGB(255, 255, 255);
		pDC->SetBkColor(popupBg);
		pDC->SetTextColor(dark ? RGB(180, 185, 190) : RGB(75, 88, 102));
		return m_searchPopupBrush;
	}
	if (pWnd->GetSafeHwnd() == m_searchResultsHeader.GetSafeHwnd())
	{
		pDC->SetBkColor(toolbar);
		pDC->SetTextColor(text);
		return m_toolbarBrush;
	}
	// 工具栏背景：白色（Chrome/Edge 风格）
	if (pWnd->GetSafeHwnd() == m_toolbar.GetSafeHwnd())
	{
		pDC->SetBkColor(toolbar);
		return m_toolbarBrush;
	}
	if (pWnd->GetSafeHwnd() == m_pageTotal.GetSafeHwnd())
	{
		pDC->SetBkColor(toolbar);
		pDC->SetTextColor(text);
		return m_toolbarBrush;
	}

	// 工具栏内的编辑框（页码输入、搜索框）
	if (nCtlColor == CTLCOLOR_EDIT &&
		(pWnd->GetSafeHwnd() == m_editCurrent.GetSafeHwnd() ||
		 pWnd->GetSafeHwnd() == m_editSearch.GetSafeHwnd()))
	{
		const bool searchEdit = pWnd->GetSafeHwnd() == m_editSearch.GetSafeHwnd();
		const COLORREF editBg = searchEdit ? (dark ? RGB(50, 54, 58) : RGB(255, 255, 255)) : toolbar;
		pDC->SetBkColor(editBg);
		pDC->SetTextColor(text);
		if (searchEdit)
			return m_searchPopupBrush;
		return m_toolbarBrush;
	}

	return hbr;
}

HCURSOR CXiaoGongPDFDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

// ============================================================================
// 统一的页面跳转函数
// ============================================================================
void CXiaoGongPDFDlg::GoToPage(int pageNumber)
{
	// 验证页码范围
	if (pageNumber < 0 || pageNumber >= m_totalPages)
		return;

	ClearActiveCaptureRect();
	m_currentPage = pageNumber;

	// 滚动到目标页面所在排版行的顶部
	if (!m_pageYPositions.empty() && pageNumber < (int)m_pageYPositions.size())
	{
		const int rowTop = (pageNumber < (int)m_pageRowTops.size()) ?
			m_pageRowTops[pageNumber] : m_pageYPositions[pageNumber];
		m_scrollPosition = rowTop;

		// 限制滚动范围
		CRect viewRect;
		m_pdfView.GetClientRect(&viewRect);
		int maxScroll = m_totalScrollHeight - viewRect.Height();
		if (maxScroll < 0) maxScroll = 0;
		if (m_scrollPosition > maxScroll) m_scrollPosition = maxScroll;

		// 更新滚动条和显示
		UpdateScrollBar();
		RenderVisiblePages();
		UpdatePageControls();
	}
}

// 逐个恢复标签页，等待每次后台加载完成后继续。
LRESULT CXiaoGongPDFDlg::OnOpenInitialFile(WPARAM wParam, LPARAM lParam)
{
	if (!m_sessionRestoreStarted)
	{
		m_sessionRestoreStarted = true;
		RestoreOpenTabs();
	}
	OpenNextRestoredTab();
	return 0;
}

// 定时器消息处理函数
void CXiaoGongPDFDlg::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent == TIMER_ID_RESIZE_DEBOUNCE)
	{
		KillTimer(TIMER_ID_RESIZE_DEBOUNCE);

#ifdef _DEBUG
		TRACE(_T("OnTimer: 执行窗口大小变化后的重渲染（防抖定时器触发，cx=%d, cy=%d）\n"),
			m_pendingResizeCx, m_pendingResizeCy);
#endif

		if (m_doc && m_currentPage >= 0)
		{
			PerformResizeRender();
		}
		return;
	}
	if (nIDEvent == TIMER_ID_PAGE_PREFETCH)
	{
		KillTimer(TIMER_ID_PAGE_PREFETCH);
		PrefetchNearbyPages();
		return;
	}
	if (nIDEvent == TIMER_ID_ANNOT_CARET)
	{
		if (!m_inlineAnnot.active || !m_doc)
		{
			KillTimer(TIMER_ID_ANNOT_CARET);
			return;
		}
		m_inlineAnnot.caretVisible = !m_inlineAnnot.caretVisible;
		InvalidateSelection();
		return;
	}

	CDialogEx::OnTimer(nIDEvent);
}

void CXiaoGongPDFDlg::PerformResizeRender()
{
	if (!m_doc || m_currentPage < 0)
	{
		return;
	}

	ClearActiveCaptureRect();

	const bool recalcScale = (m_zoomMode == ZOOM_FIT_WIDTH || m_zoomMode == ZOOM_FIT_PAGE);
	const float scrollRatio = (recalcScale && m_totalScrollHeight > 0) ?
		static_cast<float>(m_scrollPosition) / m_totalScrollHeight : -1.0f;
	CalculatePagePositions(recalcScale);
	if (scrollRatio >= 0.0f && m_totalScrollHeight > 0)
		m_scrollPosition = static_cast<int>(scrollRatio * m_totalScrollHeight);
	UpdateScrollBar();
	RenderVisiblePages();

	m_lastResizeRenderTick = GetTickCount64();
	UpdateZoomStatus();
}

void CXiaoGongPDFDlg::layoutToolbar(int cx)
{
	if (!m_toolbar.GetSafeHwnd() || !m_btnOpen.GetSafeHwnd() || cx <= 0)
		return;

	const int TOOLBAR_HEIGHT = 52;
	const int TAB_HEIGHT = TITLE_BAR_HEIGHT;
	const int BTN_HEIGHT = 34;
	const int btnY = TAB_HEIGHT + (TOOLBAR_HEIGHT - BTN_HEIGHT) / 2;

	m_toolbar.MoveWindow(0, TAB_HEIGHT, cx, TOOLBAR_HEIGHT);

	// 测量字体度量与总页数文本宽度，动态适配多位数页码并使输入框内数字垂直居中
	int textHeight = 20;
	int digitSampleWidth = 18;
	int totalTextWidth = 28;
	{
		CClientDC dc(this);
		CFont* pOldFont = m_buttonFont.GetSafeHandle() ? dc.SelectObject(&m_buttonFont) : nullptr;

		TEXTMETRIC tm = {};
		if (dc.GetTextMetrics(&tm) && tm.tmHeight > 0)
			textHeight = tm.tmHeight;

		int maxPages = max(1, m_totalPages);
		int digits = 0;
		while (maxPages > 0)
		{
			++digits;
			maxPages /= 10;
		}
		CString sampleDigits(_T('8'), max(2, digits));
		digitSampleWidth = dc.GetTextExtent(sampleDigits).cx;

		CString totalText;
		m_pageTotal.GetWindowText(totalText);
		if (totalText.IsEmpty())
			totalText = _T("/ 0");
		totalTextWidth = dc.GetTextExtent(totalText).cx;

		if (pOldFont)
			dc.SelectObject(pOldFont);
	}

	const DWORD editExStyle = m_editCurrent.GetSafeHwnd() ? m_editCurrent.GetExStyle() : 0;
	const int borderX = (editExStyle & WS_EX_CLIENTEDGE) ?
		2 * GetSystemMetrics(SM_CXEDGE) : 2 * GetSystemMetrics(SM_CXBORDER);
	const int borderY = (editExStyle & WS_EX_CLIENTEDGE) ?
		2 * GetSystemMetrics(SM_CYEDGE) : 2 * GetSystemMetrics(SM_CYBORDER);
	const int editHeight = min(BTN_HEIGHT, max(24, textHeight + borderY + 2));
	const int editY = btnY + (BTN_HEIGHT - editHeight) / 2;

	// 先评估标准尺寸下左右两侧是否能完整容纳全部按钮；若不能则切换为紧凑尺寸并按优先级隐藏
	const int normPageWidth = max(42, digitSampleWidth + borderX + 12);
	const int normTotalWidth = max(40, totalTextWidth + 8);
	const int normInnerGap = 6;
	const int normCenterWidth = 2 * 34 + 2 * 5 + normPageWidth + normInnerGap + normTotalWidth;
	const int normCenterLeft = (cx - normCenterWidth) / 2;
	const int normCenterRight = normCenterLeft + normCenterWidth;
	const int normLeftBudget = normCenterLeft - 16 - 16;
	const int normRightBudget = (cx - 16) - (normCenterRight + 16);
	const int normFullLeftWidth = 3 * 82 + 2 * 14;
	const int normFullRightWidth = (34 + 5 + 34 + 5 + 68) + 14 + 34 + 14 + 34 + 14 + 34 +
		14 + 34 + 14 + 34 + 14 + 34 + 14 + (34 + 5 + 34) + 14 + 34 + 14 + 76;
	const bool compact = (normLeftBudget < normFullLeftWidth || normRightBudget < normFullRightWidth);

	const int sideMargin = compact ? 12 : 16;
	const int minCenterGap = compact ? 12 : 16;
	const int iconWidth = compact ? 30 : 34;
	const int gap = compact ? 4 : 5;
	const int actionTextWidth = compact ? 72 : 82;
	const int fitWidth = compact ? 58 : 68;
	const int moreTextWidth = compact ? 68 : 76;
	const int pageWidth = compact ? max(38, digitSampleWidth + borderX + 10) : normPageWidth;
	const int totalWidth = compact ? max(36, totalTextWidth + 6) : normTotalWidth;
	const int innerPageGap = compact ? 4 : normInnerGap;

	int groupGap = compact ? 8 : 14;
	if (!compact && normRightBudget > normFullRightWidth)
	{
		groupGap = min(24, 14 + (normRightBudget - normFullRightWidth) / 12);
	}

	// 1. 中央区域：页码及总页码组永远相对工具栏水平与垂直居中绘制
	const int centerWidth = 2 * iconWidth + 2 * gap + pageWidth + innerPageGap + totalWidth;
	const int centerLeft = max(4, (cx - centerWidth) / 2);
	const int centerRight = centerLeft + centerWidth;

	const bool showCenter = (cx >= centerWidth + 8);
	m_btnPrevPage.ShowWindow(showCenter ? SW_SHOW : SW_HIDE);
	m_editCurrent.ShowWindow(showCenter ? SW_SHOW : SW_HIDE);
	m_pageTotal.ShowWindow(showCenter ? SW_SHOW : SW_HIDE);
	m_btnNextPage.ShowWindow(showCenter ? SW_SHOW : SW_HIDE);
	if (showCenter)
	{
		int cxPos = centerLeft;
		m_btnPrevPage.MoveWindow(cxPos, btnY, iconWidth, BTN_HEIGHT);
		cxPos += iconWidth + gap;
		m_editCurrent.MoveWindow(cxPos, editY, pageWidth, editHeight);
		cxPos += pageWidth + innerPageGap;
		m_pageTotal.MoveWindow(cxPos, btnY, totalWidth, BTN_HEIGHT);
		cxPos += totalWidth + gap;
		m_btnNextPage.MoveWindow(cxPos, btnY, iconWidth, BTN_HEIGHT);
	}

	const int leftBudget = max(0, centerLeft - sideMargin - minCenterGap);
	const int rightBudget = max(0, (cx - sideMargin) - (centerRight + minCenterGap));

	int dividerIndex = 0;
	auto placeDivider = [&](int divX)
	{
		if (dividerIndex < static_cast<int>(_countof(m_toolbarDividers)))
		{
			m_toolbarDividers[dividerIndex].MoveWindow(
				divX + (groupGap - 2) / 2, btnY + 5, 2, 24);
			m_toolbarDividers[dividerIndex].ShowWindow(SW_SHOW);
			++dividerIndex;
		}
	};

	// 2. 左侧区域（打开 | 目录 | 查找）：左对齐，空间压缩时先缩为纯图标再逐个隐藏
	bool showOpen = true;
	bool showOutline = true;
	bool showFind = true;
	bool leftIconAndText = true;
	auto calcLeftWidth = [&]() -> int
	{
		const int count = (showOpen ? 1 : 0) + (showOutline ? 1 : 0) + (showFind ? 1 : 0);
		if (count == 0)
			return 0;
		const int btnW = leftIconAndText ? actionTextWidth : iconWidth;
		return count * btnW + (count - 1) * groupGap;
	};
	if (calcLeftWidth() > leftBudget)
		leftIconAndText = false;
	if (calcLeftWidth() > leftBudget)
		showFind = false;
	if (calcLeftWidth() > leftBudget)
		showOutline = false;
	if (calcLeftWidth() > leftBudget)
		showOpen = false;

	m_btnOpen.SetIconAndText(leftIconAndText);
	m_btnOutline.SetIconAndText(leftIconAndText);
	m_btnFind.SetIconAndText(leftIconAndText);
	m_btnOpen.ShowWindow(showOpen ? SW_SHOW : SW_HIDE);
	m_btnOutline.ShowWindow(showOutline ? SW_SHOW : SW_HIDE);
	m_btnFind.ShowWindow(showFind ? SW_SHOW : SW_HIDE);

	const int leftBtnWidth = leftIconAndText ? actionTextWidth : iconWidth;
	int leftX = sideMargin;
	bool placedLeftGroup = false;
	auto placeLeftBtn = [&](CCustomButton& btn, bool visible)
	{
		if (!visible)
			return;
		if (placedLeftGroup)
		{
			placeDivider(leftX);
			leftX += groupGap;
		}
		btn.MoveWindow(leftX, btnY, leftBtnWidth, BTN_HEIGHT);
		leftX += leftBtnWidth;
		placedLeftGroup = true;
	};
	placeLeftBtn(m_btnOpen, showOpen);
	placeLeftBtn(m_btnOutline, showOutline);
	placeLeftBtn(m_btnFind, showFind);

	// 3. 右侧区域（缩放 | 页面视图 | 打印 | 右旋 | 批注 | 编辑 | 截图 | 首末页 | 全屏 | 更多）：右对齐，压缩时按优先级隐藏
	bool showZoomStep = true;
	bool showFitPage = true;
	bool showPageLayout = true;
	bool showPrint = true;
	bool showRotate = true;
	bool showAnnotate = true;
	bool showEdit = true;
	bool showSnapshot = true;
	bool showFirstLast = true;
	bool showFullscreen = true;
	bool showMore = true;
	bool moreIconAndText = true;

	auto calcRightWidth = [&]() -> int
	{
		int total = 0;
		int groups = 0;
		auto addGroup = [&](int w)
		{
			if (w <= 0)
				return;
			if (groups > 0)
				total += groupGap;
			total += w;
			++groups;
		};

		int zoomW = 0;
		if (showZoomStep)
			zoomW += 2 * iconWidth + gap;
		if (showFitPage)
			zoomW += (zoomW > 0 ? gap : 0) + fitWidth;
		addGroup(zoomW);

		if (showPageLayout)
			addGroup(iconWidth);
		if (showPrint)
			addGroup(iconWidth);
		if (showRotate)
			addGroup(iconWidth);
		if (showAnnotate)
			addGroup(iconWidth);
		if (showEdit)
			addGroup(iconWidth);
		if (showSnapshot)
			addGroup(iconWidth);
		if (showFirstLast)
			addGroup(2 * iconWidth + gap);
		if (showFullscreen)
			addGroup(iconWidth);
		if (showMore)
			addGroup(moreIconAndText ? moreTextWidth : iconWidth);

		return total;
	};

	if (calcRightWidth() > rightBudget)
		showFirstLast = false;
	if (calcRightWidth() > rightBudget)
		showSnapshot = false;
	if (calcRightWidth() > rightBudget)
		showPrint = false;
	if (calcRightWidth() > rightBudget)
		showEdit = false;
	if (calcRightWidth() > rightBudget)
		showFullscreen = false;
	if (calcRightWidth() > rightBudget)
		showRotate = false;
	if (calcRightWidth() > rightBudget)
		showPageLayout = false;
	if (calcRightWidth() > rightBudget)
		showAnnotate = false;
	if (calcRightWidth() > rightBudget)
		moreIconAndText = false;
	if (calcRightWidth() > rightBudget)
		showFitPage = false;
	if (calcRightWidth() > rightBudget)
		showZoomStep = false;
	if (calcRightWidth() > rightBudget)
		showMore = false;

	m_btnMore.SetIconAndText(moreIconAndText);
	m_btnZoomOut.ShowWindow(showZoomStep ? SW_SHOW : SW_HIDE);
	m_btnZoomIn.ShowWindow(showZoomStep ? SW_SHOW : SW_HIDE);
	m_btnFitPage.ShowWindow(showFitPage ? SW_SHOW : SW_HIDE);
	m_btnPageLayout.ShowWindow(showPageLayout ? SW_SHOW : SW_HIDE);
	m_btnPrint.ShowWindow(showPrint ? SW_SHOW : SW_HIDE);
	m_btnRotateRight.ShowWindow(showRotate ? SW_SHOW : SW_HIDE);
	m_btnAnnotate.ShowWindow(showAnnotate ? SW_SHOW : SW_HIDE);
	m_btnEdit.ShowWindow(showEdit ? SW_SHOW : SW_HIDE);
	m_btnSnapshot.ShowWindow(showSnapshot ? SW_SHOW : SW_HIDE);
	m_btnFirst.ShowWindow(showFirstLast ? SW_SHOW : SW_HIDE);
	m_btnLast.ShowWindow(showFirstLast ? SW_SHOW : SW_HIDE);
	m_btnFullscreen.ShowWindow(showFullscreen ? SW_SHOW : SW_HIDE);
	m_btnMore.ShowWindow(showMore ? SW_SHOW : SW_HIDE);
	m_btnFitWidth.ShowWindow(SW_HIDE);
	m_btnRotateLeft.ShowWindow(SW_HIDE);

	const int rightWidth = calcRightWidth();
	int rightX = max(centerRight + minCenterGap, cx - sideMargin - rightWidth);
	bool placedRightGroup = false;
	auto beginRightGroup = [&]()
	{
		if (placedRightGroup)
		{
			placeDivider(rightX);
			rightX += groupGap;
		}
		placedRightGroup = true;
	};

	if (showZoomStep || showFitPage)
	{
		beginRightGroup();
		if (showZoomStep)
		{
			m_btnZoomOut.MoveWindow(rightX, btnY, iconWidth, BTN_HEIGHT);
			rightX += iconWidth + gap;
			m_btnZoomIn.MoveWindow(rightX, btnY, iconWidth, BTN_HEIGHT);
			rightX += iconWidth;
			if (showFitPage)
				rightX += gap;
		}
		if (showFitPage)
		{
			m_btnFitPage.MoveWindow(rightX, btnY, fitWidth, BTN_HEIGHT);
			rightX += fitWidth;
		}
	}
	if (showPageLayout)
	{
		beginRightGroup();
		m_btnPageLayout.MoveWindow(rightX, btnY, iconWidth, BTN_HEIGHT);
		rightX += iconWidth;
	}
	if (showPrint)
	{
		beginRightGroup();
		m_btnPrint.MoveWindow(rightX, btnY, iconWidth, BTN_HEIGHT);
		rightX += iconWidth;
	}
	if (showRotate)
	{
		beginRightGroup();
		m_btnRotateRight.MoveWindow(rightX, btnY, iconWidth, BTN_HEIGHT);
		rightX += iconWidth;
	}
	if (showAnnotate)
	{
		beginRightGroup();
		m_btnAnnotate.MoveWindow(rightX, btnY, iconWidth, BTN_HEIGHT);
		rightX += iconWidth;
	}
	if (showEdit)
	{
		beginRightGroup();
		m_btnEdit.MoveWindow(rightX, btnY, iconWidth, BTN_HEIGHT);
		rightX += iconWidth;
	}
	if (showSnapshot)
	{
		beginRightGroup();
		m_btnSnapshot.MoveWindow(rightX, btnY, iconWidth, BTN_HEIGHT);
		rightX += iconWidth;
	}
	if (showFirstLast)
	{
		beginRightGroup();
		m_btnFirst.MoveWindow(rightX, btnY, iconWidth, BTN_HEIGHT);
		rightX += iconWidth + gap;
		m_btnLast.MoveWindow(rightX, btnY, iconWidth, BTN_HEIGHT);
		rightX += iconWidth;
	}
	if (showFullscreen)
	{
		beginRightGroup();
		m_btnFullscreen.MoveWindow(rightX, btnY, iconWidth, BTN_HEIGHT);
		rightX += iconWidth;
	}
	if (showMore)
	{
		beginRightGroup();
		const int moreW = moreIconAndText ? moreTextWidth : iconWidth;
		m_btnMore.MoveWindow(rightX, btnY, moreW, BTN_HEIGHT);
		rightX += moreW;
	}

	for (; dividerIndex < static_cast<int>(_countof(m_toolbarDividers)); ++dividerIndex)
		m_toolbarDividers[dividerIndex].ShowWindow(SW_HIDE);
}

void CXiaoGongPDFDlg::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);

	bool doImmediateResizeRender = false;

	// 全屏模式下不进行调整，已提前调整
	if (m_isFullscreen && m_pdfView.GetSafeHwnd())
	{
		return;
	}

	// 检查控件是否有效，以及窗口是否为最小化状态
	if (m_pdfView.GetSafeHwnd() && m_toolbar.GetSafeHwnd() &&
		m_btnOpen.GetSafeHwnd() && nType != SIZE_MINIMIZED)
	{
		// ★★★ 禁止重绘，避免布局调整过程中的闪烁（高刷新率显示器优化）
		SetRedraw(FALSE);
		const int TOOLBAR_HEIGHT = 52;
		const int TAB_HEIGHT = TITLE_BAR_HEIGHT;
		const int BTN_HEIGHT = 34;

		if (m_tabCtrl.GetSafeHwnd())
		{
			m_tabCtrl.MoveWindow(TITLE_ICON_AREA_WIDTH, 0,
				max(1, cx - TITLE_ICON_AREA_WIDTH - 138), TAB_HEIGHT);
			UpdateTabWidth();
		}
		m_btnMinimize.MoveWindow(cx - 138, 0, 46, TAB_HEIGHT);
		m_btnMaximize.MoveWindow(cx - 92, 0, 46, TAB_HEIGHT);
		m_btnCloseWindow.MoveWindow(cx - 46, 0, 46, TAB_HEIGHT);
		m_btnMaximize.SetIconGlyph(IsZoomed() ? 0xE923 : 0xE922);
		m_btnMaximize.SetToolTipText(IsZoomed() ? _T("还原") : _T("最大化"));
		layoutToolbar(cx);

		// 根据缩略图面板可见性调整布局
		int pdfX, pdfWidth;
		const int STATUS_BAR_HEIGHT = 25;  // 状态栏高度

		// 计算内容区域的起始Y坐标（工具栏 + 标签页 + 间距）
		int contentY = TAB_HEIGHT + TOOLBAR_HEIGHT + 1;
		int contentHeight = max(1, cy - contentY - STATUS_BAR_HEIGHT);
		const bool showSearchResults = m_searchResultsVisible && m_doc;
		m_searchResultsHeader.ShowWindow(showSearchResults ? SW_SHOW : SW_HIDE);
		m_btnCloseSearchResults.ShowWindow(showSearchResults ? SW_SHOW : SW_HIDE);
		m_searchResultsList.ShowWindow(showSearchResults ? SW_SHOW : SW_HIDE);
		m_outlineTree.ShowWindow(m_outlineVisible && m_doc && !showSearchResults ? SW_SHOW : SW_HIDE);
		if (showSearchResults)
		{
			const int panelWidth = min(320, max(220, cx / 3));
			m_searchResultsHeader.MoveWindow(12, contentY, panelWidth - 50, 36);
			m_btnCloseSearchResults.MoveWindow(panelWidth - 38, contentY + 2, 34, 32);
			m_searchResultsList.MoveWindow(0, contentY + 36, panelWidth,
				max(1, contentHeight - 36));
			m_searchResultsList.SetColumnWidth(1, max(80, panelWidth - 82));
			pdfX = panelWidth + 1;
			pdfWidth = cx - pdfX;
		}
		else if (m_outlineVisible && m_doc)
		{
			m_outlineTree.MoveWindow(0, contentY,
				LIST_WIDTH + m_scrollBarWidth, contentHeight);
			pdfX = LIST_WIDTH + m_scrollBarWidth + 1;
			pdfWidth = cx - pdfX;
		}
		else
		{
			// 缩略图隐藏时，PDF 预览占满整个宽度
			pdfX = 0;
			pdfWidth = cx;
		}
		const int popupWidth = min(430, max(220, cx - 16));
		const int popupX = (cx - popupWidth) / 2;
		const int popupY = contentY + 8;
		const int searchEditWidth = popupWidth - 172;
		m_searchPopup.MoveWindow(popupX, popupY, popupWidth, 54);
		m_editSearch.MoveWindow(popupX + 12, popupY + 10, searchEditWidth, BTN_HEIGHT);
		m_searchMatchCount.MoveWindow(popupX + popupWidth - 158, popupY + 10, 54, BTN_HEIGHT);
		m_btnPrevMatch.MoveWindow(popupX + popupWidth - 104, popupY + 10, 34, BTN_HEIGHT);
		m_btnNextMatch.MoveWindow(popupX + popupWidth - 70, popupY + 10, 34, BTN_HEIGHT);
		m_btnCloseSearchPopup.MoveWindow(popupX + popupWidth - 36, popupY + 10, 34, BTN_HEIGHT);
		m_searchPopup.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);
		m_editSearch.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);
		m_searchMatchCount.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);
		m_btnPrevMatch.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);
		m_btnNextMatch.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);
		m_btnCloseSearchPopup.ShowWindow(m_searchToolsVisible ? SW_SHOW : SW_HIDE);

		// PDF视图，为状态栏留出空间
		const int scrollBarSize = 16;
		const bool showHorizontalBar = m_doc && m_pdfHorizontalScrollBar.GetSafeHwnd() &&
			m_pdfHorizontalScrollBar.IsWindowVisible();
		const int horizontalBarHeight = showHorizontalBar ? scrollBarSize : 0;
		m_pdfView.MoveWindow(pdfX, contentY, max(1, pdfWidth - scrollBarSize),
			max(1, contentHeight - horizontalBarHeight));
		if (m_searchToolsVisible)
		{
			m_searchPopup.BringWindowToTop();
			m_editSearch.BringWindowToTop();
			m_searchMatchCount.BringWindowToTop();
			m_btnPrevMatch.BringWindowToTop();
			m_btnNextMatch.BringWindowToTop();
			m_btnCloseSearchPopup.BringWindowToTop();
		}
		if (m_pdfVerticalScrollBar.GetSafeHwnd())
		{
			m_pdfVerticalScrollBar.MoveWindow(pdfX + pdfWidth - scrollBarSize,
				contentY, scrollBarSize, max(1, contentHeight - horizontalBarHeight));
			if (!m_doc)
				m_pdfVerticalScrollBar.ShowWindow(SW_HIDE);
		}
		if (m_pdfHorizontalScrollBar.GetSafeHwnd())
		{
			m_pdfHorizontalScrollBar.MoveWindow(pdfX, contentY + contentHeight - scrollBarSize,
				max(1, pdfWidth - scrollBarSize), scrollBarSize);
			if (!m_doc)
				m_pdfHorizontalScrollBar.ShowWindow(SW_HIDE);
		}
		if (m_pdfScrollCorner.GetSafeHwnd())
		{
			m_pdfScrollCorner.MoveWindow(pdfX + pdfWidth - scrollBarSize,
				contentY + contentHeight - scrollBarSize, scrollBarSize, scrollBarSize);
			if (!m_doc)
				m_pdfScrollCorner.ShowWindow(SW_HIDE);
		}

		// ★★★ 不清除旧位图，让旧位图保留到防抖渲染完成后原子替换，避免 150ms 灰屏。
		// OnPaint 中的裁剪逻辑（drawW/drawH 限制在客户区范围内）保证旧位图不会溢出新边界。

		// ★★★ MoveWindow 会重置控件状态，需要立即根据模式设置滚动条
		m_pdfView.ShowScrollBar(SB_VERT, FALSE);
		m_pdfView.ShowScrollBar(SB_HORZ, FALSE);

		// 状态栏在最底部
		m_statusBar.MoveWindow(0, cy - STATUS_BAR_HEIGHT, max(1, cx - 86), STATUS_BAR_HEIGHT);
		m_zoomStatus.MoveWindow(max(0, cx - 86), cy - STATUS_BAR_HEIGHT, 86, STATUS_BAR_HEIGHT);

		// 更新工具栏控件状态 - 仅在文档已加载时更新
		if (m_doc)
		{
			// 仅在编辑框未聚焦且文本确实变化时更新页码，避免拖拽缩放时覆盖用户输入或高频抖动
			if (::GetFocus() != m_editCurrent.GetSafeHwnd())
			{
				CString currentPage;
				currentPage.Format(_T("%d"), m_currentPage + 1);
				CString existingText;
				m_editCurrent.GetWindowText(existingText);
				if (existingText != currentPage)
					m_editCurrent.SetWindowText(currentPage);
			}

			// 更新状态栏
			UpdateStatusBar();
		}

		// 无论是否有文档，都需要重绘状态栏（修复未打开PDF时横向伸缩显示异常）
		m_statusBar.Invalidate(FALSE);
		m_statusBar.UpdateWindow();
		m_zoomStatus.Invalidate(FALSE);

		// ★★★ P1优化：使用防抖定时器延迟重渲染，避免拖拽调整窗口时高频触发完整渲染
		// 拖拽过程中 WM_SIZE 以约 60fps 频率到来，每次都重渲染会严重阻塞 UI 线程
		if (!m_suppressResizeRender && m_doc && m_currentPage >= 0)
		{
			// 保存最新的窗口尺寸，定时器触发时使用
			m_pendingResizeCx = cx;
			m_pendingResizeCy = cy;

			ULONGLONG currentTick = GetTickCount64();
			if (nType == SIZE_MAXIMIZED ||
				m_lastResizeRenderTick == 0 ||
				currentTick - m_lastResizeRenderTick >= RESIZE_THROTTLE_MS)
			{
				// 本次立即重渲染，取消旧的防抖定时器避免 60ms 后重复渲染
				KillTimer(TIMER_ID_RESIZE_DEBOUNCE);
				doImmediateResizeRender = true;
			}
			else
			{
				// 节流窗口内仅刷新防抖定时器，拖拽停手后再执行一次完整渲染
				KillTimer(TIMER_ID_RESIZE_DEBOUNCE);
				SetTimer(TIMER_ID_RESIZE_DEBOUNCE, RESIZE_DEBOUNCE_MS, NULL);
			}
		}

		// ★★★ 重新启用重绘，一次性刷新整个窗口（高刷新率显示器优化）
		SetRedraw(TRUE);

		if (doImmediateResizeRender)
		{
			PerformResizeRender();
		}

		// ★★★ 两步刷新策略：
		// 第一步：只对主对话框自身触发 ERASE（OnEraseBkgnd 填 COLOR_3DFACE），
		//         不传播到子控件（无 RDW_ALLCHILDREN），避免标准控件白屏。
		//         这一步确保最大化→还原后新暴露的背景区域被正确填色。
		RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
		// 第二步：让所有子控件重绘自身内容及非客户区边框（如页码输入框、分隔线），但不触发 ERASEBKGND。
		RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);

		// ★★★ 确保PDF视图也立即重绘（应用居中等布局变化）
		if (m_doc && m_currentPage >= 0 && !doImmediateResizeRender)
		{
			// 使用RDW_UPDATENOW强制立即处理WM_PAINT消息
			m_pdfView.RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
		}
	}
}

BOOL CXiaoGongPDFDlg::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
	// 检查文档是否加载
	if (!m_doc || m_totalPages <= 0)
		return CDialogEx::OnMouseWheel(nFlags, zDelta, pt);

	// 使用成员变量防止重入
	if (m_isRenderingWheelZoom)
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
				m_isRenderingWheelZoom = true;  // 标记开始渲染

				// 从当前真实缩放比例起步步进，避免从适页/适宽切到自定义缩放时发生画面跳变
				const float baseZoom = GetCurrentZoomBaseForStep();
				float newZoom = (zDelta > 0) ? (baseZoom * 1.1f) : (baseZoom / 1.1f);

				// 限制缩放范围 0.25x ~ 4.0x
				if (newZoom < 0.25f) newZoom = 0.25f;
				if (newZoom > 4.0f) newZoom = 4.0f;

				// ★★★ 只有当缩放模式或缩放值真正改变时才调用 SetZoom
				if (m_zoomMode != ZOOM_CUSTOM || fabs(newZoom - m_customZoom) > 0.001f)
				{
					SetZoom(newZoom, ZOOM_CUSTOM);
				}

				m_isRenderingWheelZoom = false;  // 标记渲染完成
				return TRUE;
			}
			catch (...)
			{
				m_isRenderingWheelZoom = false;  // 确保发生异常时也重置标记
				MessageBox(_T("缩放页面时发生错误"), _T("错误"), MB_OK | MB_ICONERROR);
				return FALSE;
			}
		}
		else
		{
			// 滚轮滚动页面
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
				ClearActiveCaptureRect();
				m_scrollPosition = newPos;
				UpdateScrollBar();
				RenderVisiblePages();
			}

			return TRUE;
		}
	}

	m_isRenderingWheelZoom = false;  // 确保最后重置标记
	return CDialogEx::OnMouseWheel(nFlags, zDelta, pt);
}

// 工具栏功能实现
void CXiaoGongPDFDlg::UpdatePageControls()
{
	CString prevTotalText;
	if (m_pageTotal.GetSafeHwnd())
		m_pageTotal.GetWindowText(prevTotalText);

	if (!m_doc)
	{
		m_editCurrent.SetWindowText(_T("0"));
		const CString emptyTotal(_T("/ 0"));
		if (prevTotalText != emptyTotal)
		{
			m_pageTotal.SetWindowText(emptyTotal);
			if (!m_isFullscreen && GetSafeHwnd())
			{
				CRect clientRect;
				GetClientRect(&clientRect);
				layoutToolbar(clientRect.Width());
				CRect toolbarArea(0, TITLE_BAR_HEIGHT, clientRect.Width(), TITLE_BAR_HEIGHT + 52);
				RedrawWindow(&toolbarArea, NULL,
					RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
			}
		}
		return;
	}

	// 更新当前页码显示
	CString currentPage;
	currentPage.Format(_T("%d"), m_currentPage + 1);
	m_editCurrent.SetWindowText(currentPage);
	m_editCurrent.UpdateWindow();  // 强制立即重绘，确保快速滚动时也能实时显示

	CString totalPages;
	totalPages.Format(_T("/ %d"), m_totalPages);
	if (prevTotalText != totalPages)
	{
		m_pageTotal.SetWindowText(totalPages);
		if (!m_isFullscreen && GetSafeHwnd())
		{
			CRect clientRect;
			GetClientRect(&clientRect);
			layoutToolbar(clientRect.Width());
			CRect toolbarArea(0, TITLE_BAR_HEIGHT, clientRect.Width(), TITLE_BAR_HEIGHT + 52);
			RedrawWindow(&toolbarArea, NULL,
				RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
		}
	}

	// 更新状态栏
	UpdateStatusBar();

	// 立即重绘按钮，确保状态变化可见
	// ★★★ 使用FALSE参数避免擦除背景，减少高刷新率显示器上的闪烁
	m_btnFirst.Invalidate(FALSE);
	m_btnLast.Invalidate(FALSE);
	m_btnFullscreen.Invalidate(FALSE);
}

void CXiaoGongPDFDlg::UpdateStatusBar()
{
	SetStatusBarMessage(nullptr);
	m_statusBar.ShowWindow(m_isFullscreen ? SW_HIDE : SW_SHOW);
	m_zoomStatus.ShowWindow(m_isFullscreen ? SW_HIDE : SW_SHOW);
}

void CXiaoGongPDFDlg::UpdateZoomStatus()
{
	if (!m_zoomStatus.GetSafeHwnd())
		return;

	CString zoomText;
	if (m_doc && m_totalPages > 0)
	{
		if (m_uniformScale > 0.0f)
			zoomText.Format(_T("%d%%"), max(1, static_cast<int>(m_uniformScale * 100.0f + 0.5f)));
	}
	m_zoomStatus.SetWindowText(zoomText);
}

void CXiaoGongPDFDlg::SetStatusBarMessage(LPCTSTR message)
{
	CPDFDocument* pDoc = GetActiveDocument();
	CString status(_T("  "));
	if (pDoc)
	{
		status += pDoc->GetFilePath();
		if (m_currentPage >= 0 && m_currentPage < m_totalPages)
		{
			float pageW = 0.0f;
			float pageH = 0.0f;
			if (pDoc->GetPageBounds(m_currentPage, pageW, pageH) && pageW > 0.0f && pageH > 0.0f)
			{
				status += _T("    |    ");
				status += FormatPagePhysicalSize(pageW, pageH, false);
			}
			if (m_layoutMode == LAYOUT_TWO_PAGE_SPREAD)
				status += _T("  [双页并排]");
		}
	}
	else
	{
		status += _T("就绪");
	}
	if (message && *message)
	{
		status += _T("    |    ");
		status += message;
	}
	m_statusBar.SetWindowText(status);
	UpdateZoomStatus();
}

void CXiaoGongPDFDlg::OnBtnFirst()
{
	if (!m_doc || m_totalPages <= 0)
		return;

	GoToPage(0);
}

void CXiaoGongPDFDlg::OnBtnPrevPage()
{
	if (m_doc && m_currentPage > 0)
		GoToPage(GetPrevSpreadPage(m_currentPage));
}

void CXiaoGongPDFDlg::OnBtnNextPage()
{
	if (m_doc && m_currentPage + 1 < m_totalPages)
		GoToPage(GetNextSpreadPage(m_currentPage));
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

	// 停止并等待后台搜索线程退出，避免搜索线程持有同一PDF文件句柄阻塞覆盖保存
	FinishInlineTextAnnot(false);
	CancelAreaCaptureMode(false);
	CleanupSearches();

	// 模态编辑期间挂起外部文件变化通知，防止编辑框借用的 m_ctx/m_doc 被异步重载或关闭
	ScopedFileWatchSuspend suspendFileWatch(this);

	auto normalizeFilePath = [](const CString& path) -> CString
	{
		if (path.IsEmpty())
			return CString();
		TCHAR fullPath[MAX_PATH * 4] = { 0 };
		const DWORD len = ::GetFullPathName(path, static_cast<DWORD>(_countof(fullPath)), fullPath, nullptr);
		CString normalized = (len > 0 && len < _countof(fullPath)) ? CString(fullPath) : path;
		normalized.Replace(_T('/'), _T('\\'));
		return normalized;
	};

	struct LockedDocState
	{
		int index = -1;
		CPDFDocument* doc = nullptr;
		bool wasActive = false;
		int savedPage = 0;
		ZoomMode savedZoomMode = ZOOM_CUSTOM;
		float savedCustomZoom = 1.0f;
		float savedUniformScale = 0.0f;
		int savedScroll = 0;
		PageLayoutMode savedLayoutMode = LAYOUT_SINGLE_CONTINUOUS;
		std::map<int, int> savedRotations;
		CString filePath;
		std::vector<char> password;
	};

	std::vector<LockedDocState> lockedDocs;

	// 创建编辑对话框
	CPDFEditDialog editDlg(m_ctx, m_doc, pDoc->GetFilePath(), pDoc->GetPassword(), this);
	editDlg.SetTargetFileHandlers(
		[&](const CString& targetPath)
		{
			lockedDocs.clear();
			const CString normalizedTarget = normalizeFilePath(targetPath);
			if (normalizedTarget.IsEmpty())
				return;

			for (int i = 0; i < static_cast<int>(m_documents.size()); ++i)
			{
				CPDFDocument* openDoc = m_documents[i];
				if (!openDoc || !openDoc->IsOpen())
					continue;
				if (normalizeFilePath(openDoc->GetFilePath()).CompareNoCase(normalizedTarget) != 0)
					continue;

				const bool isActive = (i == m_activeDocIndex);
				if (isActive)
				{
					SaveDialogStateToDocument(openDoc);
					ClearSelection();
					CleanupBitmap();
				}

				LockedDocState state;
				state.index = i;
				state.doc = openDoc;
				state.wasActive = isActive;
				state.savedPage = openDoc->GetCurrentPage();
				state.savedZoomMode = openDoc->GetZoomMode();
				state.savedCustomZoom = openDoc->GetCustomZoom();
				state.savedUniformScale = openDoc->GetUniformScale();
				state.savedScroll = openDoc->GetScrollPosition();
				state.savedLayoutMode = openDoc->GetLayoutMode();
				state.savedRotations = openDoc->GetPageRotations();
				state.filePath = openDoc->GetFilePath();
				state.password = openDoc->GetPassword();
				lockedDocs.push_back(std::move(state));

				openDoc->CloseDocument();
				if (isActive)
				{
					m_doc = nullptr;
					editDlg.RebindBorrowedDocument(nullptr);
				}
			}
		},
		[&](const CString& /*targetPath*/, bool moved)
		{
			std::vector<CPDFDocument*> failedDocs;
			for (auto& state : lockedDocs)
			{
				if (!state.doc || state.filePath.IsEmpty())
					continue;

				const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, state.filePath, -1, NULL, 0, NULL, NULL);
				std::vector<char> utf8Path(max(1, utf8Len), 0);
				WideCharToMultiByte(CP_UTF8, 0, state.filePath, -1, utf8Path.data(), utf8Len, NULL, NULL);
				const char* pwd = state.password.empty() ? nullptr : state.password.data();

				const bool reopened = (state.doc->OpenDocument(utf8Path.data(), pwd) == PDFOpenResult::Success);
				if (reopened)
				{
					const int totalPages = state.doc->GetTotalPages();
					const int clampedPage = max(0, min(state.savedPage, totalPages - 1));
					state.doc->SetCurrentPage(clampedPage);
					state.doc->RestoreReadingZoom(state.savedZoomMode, state.savedCustomZoom, state.savedUniformScale);
					state.doc->SetScrollPosition(state.savedScroll);
					state.doc->SetLayoutMode(state.savedLayoutMode);
					if (!moved)
					{
						for (const auto& rotPair : state.savedRotations)
						{
							if (rotPair.first >= 0 && rotPair.first < totalPages)
								state.doc->SetPageRotation(rotPair.first, rotPair.second);
						}
					}
					else
					{
						AfxGetApp()->WriteProfileString(_T("ReadingPosition"),
							ReadingStateKey(state.filePath) + _T("_Rotations"), _T(""));
					}
				}
				else
				{
					failedDocs.push_back(state.doc);
				}

				if (!state.password.empty())
					SecureZeroMemory(state.password.data(), state.password.size());

				if (state.wasActive && state.doc->IsOpen())
				{
					LoadDialogStateFromDocument(state.doc);
					ClearSearchResults();
					editDlg.RebindBorrowedDocument(m_doc);
					if (m_uniformScale <= 0.0f)
						CalculatePagePositions(true);
					else
						CalculatePagePositions(m_zoomMode == ZOOM_FIT_WIDTH || m_zoomMode == ZOOM_FIT_PAGE);
					UpdateScrollBar();
					RenderVisiblePages();
					if (m_outlineVisible)
						UpdateOutlineTree();
					else if (m_outlineTree.GetSafeHwnd())
						m_outlineTree.DeleteAllItems();
					UpdatePageControls();
					UpdateStatusBar();
				}
			}
			lockedDocs.clear();

			// 若覆盖后无法重新打开文档，关闭对应的失效标签页避免遗留空指针僵尸标签
			if (!failedDocs.empty())
			{
				for (CPDFDocument* pFailedDoc : failedDocs)
				{
					auto it = std::find(m_documents.begin(), m_documents.end(), pFailedDoc);
					if (it != m_documents.end())
					{
						const int failedIdx = static_cast<int>(it - m_documents.begin());
						CloseDocument(failedIdx, false);
					}
				}
				MessageBox(_T("重新加载被覆盖的 PDF 文件失败，相关标签页已关闭。"),
					_T("提示"), MB_OK | MB_ICONWARNING);
			}
		});

	// 显示对话框
	if (editDlg.DoModal() == IDOK)
	{
		CString newFilePath = editDlg.GetSavedFilePath();
		if (!newFilePath.IsEmpty())
		{
			const CString normalizedNew = normalizeFilePath(newFilePath);
			int existingIndex = -1;
			for (int i = 0; i < static_cast<int>(m_documents.size()); ++i)
			{
				if (m_documents[i] &&
					normalizeFilePath(m_documents[i]->GetFilePath()).CompareNoCase(normalizedNew) == 0)
				{
					existingIndex = i;
					break;
				}
			}

			if (existingIndex >= 0)
			{
				if (existingIndex != m_activeDocIndex)
				{
					if (m_tabCtrl.GetSafeHwnd())
						m_tabCtrl.SetCurSel(existingIndex);
					SwitchToDocument(existingIndex);
				}
				SetStatusBarMessage(_T("已覆盖保存并重新加载当前PDF"));
			}
			else
			{
				OpenPDFInNewTab(newFilePath);
			}
		}
	}
}

// 目录侧栏开关
void CXiaoGongPDFDlg::OnCheckThumbnail()
{
	m_outlineVisible = !m_outlineVisible;
	if (m_outlineVisible)
		m_searchResultsVisible = false;
	UpdateOutlineButtonColors();
	m_btnOutline.SetToolTipText(m_outlineVisible ? _T("隐藏目录") : _T("显示目录"));
	if (m_outlineVisible && m_outlineTree.GetSafeHwnd() && !m_outlineTree.GetRootItem())
		UpdateOutlineTree();
	m_outlineTree.ShowWindow(m_outlineVisible && m_doc ? SW_SHOW : SW_HIDE);
	CRect rect;
	GetClientRect(&rect);
	SendMessage(WM_SIZE, SIZE_RESTORED, MAKELPARAM(rect.Width(), rect.Height()));
}

void CXiaoGongPDFDlg::RotatePage(int degrees)
{
#ifdef _DEBUG
	TRACE(_T("RotatePage() : degrees=%d\n"), degrees);
#endif

	if (!m_doc || m_totalPages <= 0 || m_currentPage < 0)
		return;

	CPDFDocument* pDoc = GetActiveDocument();
	if (!pDoc)
		return;

	FinishInlineTextAnnot(false);
	ClearSelection();
	ClearActiveCaptureRect();
	InvalidateLinkCache();

	// 记录旋转前相对当前排版行的滚动比例，避免旋转改变总高度后视口跳到其他页
	float relPageOffset = 0.0f;
	bool hasRelPageOffset = false;
	if (m_currentPage >= 0 &&
		m_currentPage < static_cast<int>(m_pageRowTops.size()) &&
		m_currentPage < static_cast<int>(m_pageRowHeights.size()) &&
		m_pageRowHeights[m_currentPage] > 0)
	{
		relPageOffset = static_cast<float>(m_scrollPosition - m_pageRowTops[m_currentPage]) /
			static_cast<float>(m_pageRowHeights[m_currentPage]);
		hasRelPageOffset = true;
	}

	// 获取当前页面的旋转角度
	int currentRotation = GetPageRotation(m_currentPage);
	// 计算新的旋转角度（保持在0-270范围内）
	int newRotation = (currentRotation + degrees + 360) % 360;
	// 更新旋转角度存储，并让文档对象同步失效页面尺寸缓存与位图缓存
	m_pageRotations[m_currentPage] = newRotation;
	pDoc->SetPageRotation(m_currentPage, newRotation);
	pDoc->InvalidatePageRenderCache(m_currentPage);

	// 旋转后重置平移偏移量（因为页面尺寸改变了）
	m_panOffset.x = 0;
	m_panOffset.y = 0;

#ifdef _DEBUG
	TRACE(_T("旋转前重置平移偏移量: (%d, %d)\n"), m_panOffset.x, m_panOffset.y);
#endif

	// 重新计算页面位置，并按原排版行相对位置锚定当前页
	CalculatePagePositions();
	if (hasRelPageOffset &&
		m_currentPage >= 0 &&
		m_currentPage < static_cast<int>(m_pageRowTops.size()) &&
		m_currentPage < static_cast<int>(m_pageRowHeights.size()))
	{
		CRect viewRect;
		m_pdfView.GetClientRect(&viewRect);
		const int maxScroll = max(0, m_totalScrollHeight - viewRect.Height());
		const int targetY = m_pageRowTops[m_currentPage] +
			static_cast<int>(relPageOffset * m_pageRowHeights[m_currentPage]);
		m_scrollPosition = max(0, min(targetY, maxScroll));
	}
	UpdateScrollBar();
	RenderVisiblePages();

	// 保存当前状态并立即持久化单页旋转记录，同时刷新底部页面物理尺寸显示
	SaveCurrentPageZoomState();
	SaveDialogStateToDocument(pDoc);
	SaveReadingPosition(pDoc);
	UpdateStatusBar();
}

int CXiaoGongPDFDlg::GetPageRotation(int pageNumber)
{
	auto it = m_pageRotations.find(pageNumber);
	if (it != m_pageRotations.end())
		return it->second;
	return 0;  // 默认无旋转
}

void CXiaoGongPDFDlg::OnEnChangeEditCurrent()
{
	// 实时验证在 OnEditCurrentKillFocus 中处理
	// 这里保持空实现，避免输入时频繁触发
}

void CXiaoGongPDFDlg::OnEnChangeEditSearch()
{
	if (!m_doc || m_isUpdatingSearchEdit)
		return;

	CString searchText;
	m_editSearch.GetWindowText(searchText);
	searchText.Trim();
	if (searchText == m_searchKeyword)
		return;
	if (!m_isSearching && m_searchMatches.empty() && !m_searchResultsVisible &&
		m_searchKeyword.IsEmpty())
		return;

	StopSearch();
	ClearSearchResults();
	UpdateStatusBar();
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
	m_toolbar.ModifyStyle(WS_BORDER, WS_CLIPSIBLINGS, SWP_FRAMECHANGED);
	m_toolbar.SetWindowPos(&CWnd::wndBottom, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	// 创建工具栏字体。
	LOGFONT lf;
	memset(&lf, 0, sizeof(LOGFONT));
	lf.lfHeight = -16;          // 工具栏按钮和输入框使用更易读的字号
	lf.lfWeight = FW_MEDIUM;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = CLEARTYPE_QUALITY;  // ClearType渲染，更锐利
	lf.lfPitchAndFamily = DEFAULT_PITCH | FF_SWISS;
	_tcscpy_s(lf.lfFaceName, _T("微软雅黑"));

	// 确保先删除可能存在的字体对象
	m_buttonFont.DeleteObject();

	// 创建新的字体对象
	m_buttonFont.CreateFontIndirect(&lf);

	// 为所有按钮设置新字体
	m_btnFirst.SetFont(&m_buttonFont);
	m_btnPrevPage.SetFont(&m_buttonFont);
	m_btnNextPage.SetFont(&m_buttonFont);
	m_btnPrint.SetFont(&m_buttonFont);
	m_btnSnapshot.SetFont(&m_buttonFont);
	m_btnPageLayout.SetFont(&m_buttonFont);
	m_btnAnnotate.SetFont(&m_buttonFont);
	m_btnOpen.SetFont(&m_buttonFont);
	m_btnZoomOut.SetFont(&m_buttonFont);
	m_btnFitWidth.SetFont(&m_buttonFont);
	m_btnFitPage.SetFont(&m_buttonFont);
	m_btnZoomIn.SetFont(&m_buttonFont);
	m_btnMore.SetFont(&m_buttonFont);
	m_btnMinimize.SetFont(&m_buttonFont);
	m_btnMaximize.SetFont(&m_buttonFont);
	m_btnCloseWindow.SetFont(&m_buttonFont);
	m_btnLast.SetFont(&m_buttonFont);
	m_btnFullscreen.SetFont(&m_buttonFont);
	m_btnRotateLeft.SetFont(&m_buttonFont);
	m_btnRotateRight.SetFont(&m_buttonFont);
	m_btnEdit.SetFont(&m_buttonFont);
	m_btnFind.SetFont(&m_buttonFont);
	m_btnPrevMatch.SetFont(&m_buttonFont);
	m_btnNextMatch.SetFont(&m_buttonFont);

	// 设置编辑框与总页码静态文本的字体、居中对齐及裁剪属性
	m_editCurrent.ModifyStyle(0, WS_CLIPSIBLINGS | ES_CENTER);
	m_pageTotal.ModifyStyle(SS_TYPEMASK,
		WS_CLIPSIBLINGS | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX);
	CWnd* rcToolbarButtons[] = {
		&m_btnFirst, &m_btnLast, &m_btnFullscreen, &m_btnRotateLeft,
		&m_btnRotateRight, &m_btnEdit, &m_btnOutline, &m_btnFind
	};
	for (CWnd* pBtn : rcToolbarButtons)
	{
		if (pBtn->GetSafeHwnd())
			pBtn->ModifyStyle(0, WS_CLIPSIBLINGS);
	}
	m_editCurrent.SetFont(&m_buttonFont);
	m_pageTotal.SetFont(&m_buttonFont);
	m_editSearch.SetFont(&m_buttonFont);

	// 大部分图标由 Windows 系统字体提供，左右旋转使用内置 PNG 图标。
	m_btnFirst.SetWindowText(_T("首页"));
	m_btnOpen.SetWindowText(_T("打开"));
	m_btnOpen.SetIconGlyph(0xE8E5);
	m_btnOpen.SetIconAndText(true);
	m_btnLast.SetWindowText(_T("末页"));
	m_btnFullscreen.SetWindowText(_T("全屏"));
	m_btnRotateLeft.SetWindowText(_T("左旋"));
	m_btnRotateRight.SetWindowText(_T("右旋"));
	m_btnEdit.SetWindowText(_T("编辑"));
	m_btnSnapshot.SetWindowText(_T("截图"));
	m_btnPageLayout.SetWindowText(_T("双页"));
	m_btnAnnotate.SetWindowText(_T("批注"));
	m_btnFind.SetWindowText(_T("查找"));
	m_btnFind.SetIconAndText(true);
	m_btnFitPage.SetWindowText(_T("适页"));
	m_btnOutline.SetWindowText(_T("目录"));
	m_btnOutline.SetIconAndText(true);
	m_btnMore.SetWindowText(_T("更多"));
	m_btnMore.SetIconAndText(true);
	m_btnPrevMatch.SetWindowText(_T("上个"));
	m_btnNextMatch.SetWindowText(_T("下个"));
	m_btnFirst.SetIconGlyph(0xE892);
	m_btnPrevPage.SetIconGlyph(0xE76B);
	m_btnNextPage.SetIconGlyph(0xE76C);
	m_btnPrint.SetIconGlyph(0xE749);
	m_btnLast.SetIconGlyph(0xE893);
	m_btnFullscreen.SetIconGlyph(0xE740);
	m_btnRotateLeft.SetIconImage(IDR_ROTATE_LEFT_PNG);
	m_btnRotateRight.SetIconImage(IDR_ROTATE_RIGHT_PNG);
	m_btnEdit.SetIconGlyph(0xE70F);
	m_btnSnapshot.SetIconGlyph(0xE7A8);
	m_btnPageLayout.SetIconGlyph(0xE89A);
	m_btnAnnotate.SetIconGlyph(0xE929);
	m_btnFind.SetIconGlyph(0xE721);
	m_btnPrevMatch.SetIconGlyph(0xE76B);
	m_btnNextMatch.SetIconGlyph(0xE76C);
	m_btnMore.SetIconGlyph(0xE712);
	m_btnOutline.SetIconGlyph(0xE8FD);
	m_btnMinimize.SetIconGlyph(0xE921);
	m_btnMaximize.SetIconGlyph(0xE922);
	m_btnCloseWindow.SetIconGlyph(0xE8BB);
	m_btnFirst.SetToolTipText(_T("首页"));
	m_btnPrevPage.SetToolTipText(_T("上一页"));
	m_btnNextPage.SetToolTipText(_T("下一页"));
	m_btnPrint.SetToolTipText(_T("打印"));
	m_btnOpen.SetToolTipText(_T("打开 PDF 文件"));
	m_btnZoomOut.SetToolTipText(_T("缩小"));
	m_btnFitWidth.SetToolTipText(_T("适合宽度"));
	m_btnFitPage.SetToolTipText(_T("适合页面"));
	UpdateFitButton();
	m_btnZoomIn.SetToolTipText(_T("放大"));
	m_btnMore.SetToolTipText(_T("更多操作"));
	m_btnOutline.SetToolTipText(_T("显示目录"));
	m_btnMinimize.SetToolTipText(_T("最小化"));
	m_btnMaximize.SetToolTipText(_T("最大化"));
	m_btnCloseWindow.SetToolTipText(_T("关闭"));
	m_btnLast.SetToolTipText(_T("末页"));
	m_btnFullscreen.SetToolTipText(_T("全屏"));
	m_btnRotateLeft.SetToolTipText(_T("向左旋转"));
	m_btnRotateRight.SetToolTipText(_T("向右旋转"));
	m_btnEdit.SetToolTipText(_T("编辑页面"));
	m_btnSnapshot.SetToolTipText(_T("区域截图 (Ctrl+Shift+S)"));
	m_btnPageLayout.SetToolTipText(_T("切换单页连续 / 双页并排视图"));
	m_btnAnnotate.SetToolTipText(_T("PDF 批注与保存 (Ctrl+S)"));
	UpdatePageLayoutButton();
	m_btnFind.SetToolTipText(_T("查找"));
	m_btnPrevMatch.SetToolTipText(_T("上一个匹配项"));
	m_btnNextMatch.SetToolTipText(_T("下一个匹配项"));
	m_editSearch.SetCueBanner(_T("搜索文档"));

	// 设置初始文本
	m_editCurrent.SetWindowText(_T("0"));
	m_pageTotal.SetWindowText(_T("/ 0"));

	// 确保所有按钮默认启用
	m_btnFirst.EnableWindow(TRUE);
	m_btnLast.EnableWindow(TRUE);
	m_btnFullscreen.EnableWindow(TRUE);
	m_btnRotateLeft.EnableWindow(TRUE);
	m_btnRotateRight.EnableWindow(TRUE);
	m_btnEdit.EnableWindow(TRUE);
	m_editCurrent.EnableWindow(TRUE);

	// 所有工具栏按钮使用相同圆角。
	m_btnFirst.SetCornerRadius(6);
	m_btnPrevPage.SetCornerRadius(6);
	m_btnNextPage.SetCornerRadius(6);
	m_btnPrint.SetCornerRadius(6);
	m_btnOpen.SetCornerRadius(5);
	m_btnZoomOut.SetCornerRadius(5);
	m_btnFitWidth.SetCornerRadius(5);
	m_btnFitPage.SetCornerRadius(5);
	m_btnZoomIn.SetCornerRadius(5);
	m_btnMore.SetCornerRadius(5);
	m_btnMinimize.SetCornerRadius(4);
	m_btnMaximize.SetCornerRadius(4);
	m_btnCloseWindow.SetCornerRadius(4);
	m_btnLast.SetCornerRadius(6);
	m_btnFullscreen.SetCornerRadius(6);
	m_btnRotateLeft.SetCornerRadius(6);
	m_btnRotateRight.SetCornerRadius(6);
	m_btnEdit.SetCornerRadius(6);
	m_btnSnapshot.SetCornerRadius(6);
	m_btnPageLayout.SetCornerRadius(6);
	m_btnAnnotate.SetCornerRadius(6);
	m_btnFind.SetCornerRadius(6);
	m_btnPrevMatch.SetCornerRadius(6);
	m_btnNextMatch.SetCornerRadius(6);
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
	case VK_LEFT:  // 左方向键 - 上一页/上一对页
		if (m_currentPage > 0)
			GoToPage(GetPrevSpreadPage(m_currentPage));
		break;

	case VK_RIGHT:  // 右方向键 - 下一页/下一对页
		if (m_currentPage < m_totalPages - 1)
			GoToPage(GetNextSpreadPage(m_currentPage));
		break;

	case VK_PRIOR:  // Page Up键 - 上一页/上一对页
		if (m_currentPage > 0)
			GoToPage(GetPrevSpreadPage(m_currentPage));
		break;

	case VK_NEXT:  // Page Down键 - 下一页/下一对页
		if (m_currentPage < m_totalPages - 1)
			GoToPage(GetNextSpreadPage(m_currentPage));
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
	if (pMsg->message == WM_LBUTTONDOWN && !m_isFullscreen &&
		m_tabCtrl.GetSafeHwnd() && m_tabCtrl.IsWindowVisible() &&
		(pMsg->hwnd == m_tabCtrl.GetSafeHwnd() || pMsg->hwnd == GetSafeHwnd()))
	{
		CPoint screenPoint(GET_X_LPARAM(pMsg->lParam), GET_Y_LPARAM(pMsg->lParam));
		::ClientToScreen(pMsg->hwnd, &screenPoint);
		CRect tabRect;
		m_tabCtrl.GetWindowRect(&tabRect);
		if (tabRect.PtInRect(screenPoint))
		{
			int lastTabRight = tabRect.left;
			bool foundTabOnRow = false;
			const int tabCount = m_tabCtrl.GetItemCount();
			for (int i = 0; i < tabCount; ++i)
			{
				CRect itemRect;
				if (m_tabCtrl.GetItemRect(i, &itemRect))
				{
					m_tabCtrl.ClientToScreen(&itemRect);
					if (screenPoint.y >= itemRect.top && screenPoint.y < itemRect.bottom)
					{
						lastTabRight = max(lastTabRight, itemRect.right);
						foundTabOnRow = true;
					}
				}
			}
			if ((tabCount == 0 || foundTabOnRow) && screenPoint.x >= lastTabRight)
			{
				StartWindowDrag();
				return TRUE;
			}
		}
	}
	if (pMsg->message == WM_KEYUP && pMsg->wParam == VK_SPACE)
	{
		if (m_spaceKeyDown)
		{
			m_spaceKeyDown = false;
			if (!m_isDragging && m_pdfView.GetSafeHwnd())
			{
				SendMessage(WM_SETCURSOR,
					reinterpret_cast<WPARAM>(m_pdfView.GetSafeHwnd()),
					MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
			}
		}
	}

	// 如果是键盘消息
	if (pMsg->message == WM_KEYDOWN || pMsg->message == WM_CHAR)
	{
		if (m_inlineAnnot.active && m_inlineAnnotEdit.GetSafeHwnd() &&
			pMsg->hwnd == m_inlineAnnotEdit.GetSafeHwnd() && pMsg->wParam == VK_RETURN &&
			!(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000))
		{
			if (pMsg->message == WM_KEYDOWN)
				::TranslateMessage(pMsg);
			::DispatchMessage(pMsg);
			return TRUE;
		}
	}

	if (pMsg->message == WM_KEYDOWN)
	{
		if (pMsg->wParam == VK_SPACE && !IsEditControlFocused() &&
			!(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000))
		{
			if (!m_spaceKeyDown)
			{
				m_spaceKeyDown = true;
				if (!m_isDragging && m_pdfView.GetSafeHwnd())
				{
					SendMessage(WM_SETCURSOR,
						reinterpret_cast<WPARAM>(m_pdfView.GetSafeHwnd()),
						MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
				}
			}
			return TRUE;
		}

		// 处理ESC键 - 关闭当前临时界面
		if (pMsg->wParam == VK_ESCAPE)
		{
			if (m_inlineAnnot.active)
			{
				if (m_inlineAnnot.colorPopupOpen)
				{
					m_inlineAnnot.colorPopupOpen = false;
					InvalidateSelection();
				}
				else
				{
					FinishInlineTextAnnot(false);
				}
				return TRUE;
			}
			if (m_isAreaCaptureMode)
			{
				CancelAreaCaptureMode(true);
				return TRUE;
			}
			if (m_isSearching)
			{
				StopSearch();
				RefreshSearchResultsList();
			}
			else if (m_searchToolsVisible)
			{
				OnCloseSearchPopup();
			}
			else if (m_isFullscreen)
				ExitFullscreen();
			else if (m_hasSelection)
			{
				ClearSelection();
				InvalidateSelection();
			}
			return TRUE;
		}

		const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
		const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;

		// 区域截图框选完成后的快捷操作：Enter / Ctrl+C 复制，Ctrl+S 另存为 PNG
		if (m_isAreaCaptureMode && m_hasCaptureRect && !IsEditControlFocused() && !altDown)
		{
			if ((!ctrlDown && !shiftDown && pMsg->wParam == VK_RETURN) ||
				(ctrlDown && !shiftDown && pMsg->wParam == 'C'))
			{
				CopyCapturedRegionToClipboard();
				return TRUE;
			}
			if (ctrlDown && !shiftDown && pMsg->wParam == 'S')
			{
				SaveCapturedRegionAsPng();
				return TRUE;
			}
		}

		// 处理 Ctrl + D - 打开文档属性与页面物理尺寸面板
		if (ctrlDown && !shiftDown && !altDown && pMsg->wParam == 'D')
		{
			OnMenuDocProperties();
			return TRUE;
		}

		// 处理 Ctrl + S - 将批注与页面旋转保存写回原 PDF
		if (ctrlDown && !shiftDown && !altDown && !m_isAreaCaptureMode && pMsg->wParam == 'S')
		{
			OnMenuSavePdf();
			return TRUE;
		}

		// 处理 Ctrl + H / Ctrl + U / Ctrl + T - 选中文本高亮 / 下划线 / 添加文字批注
		if (ctrlDown && !shiftDown && !altDown && !IsEditControlFocused())
		{
			if (pMsg->wParam == 'H')
			{
				OnMenuAnnotHighlight();
				return TRUE;
			}
			if (pMsg->wParam == 'U')
			{
				OnMenuAnnotUnderline();
				return TRUE;
			}
			if (pMsg->wParam == 'T')
			{
				OnMenuAnnotTextNote();
				return TRUE;
			}
		}

		// 处理 Ctrl + Shift + S - 框选区域高清截图
		if (ctrlDown && shiftDown && !altDown && pMsg->wParam == 'S')
		{
			OnMenuRegionCapture();
			return TRUE;
		}

		// 处理 Ctrl + Shift + E - 导出页面为图片
		if (ctrlDown && shiftDown && !altDown && pMsg->wParam == 'E')
		{
			OnMenuExportImages();
			return TRUE;
		}

		// 处理 Ctrl + Q - 退出程序
		if (ctrlDown && !altDown && !shiftDown && pMsg->wParam == 'Q')
		{
			onMenuExit();
			return TRUE;
		}

		// 处理 Ctrl + W / Ctrl + F4 - 关闭当前标签页
		if (ctrlDown && !altDown &&
			((!shiftDown && pMsg->wParam == 'W') || pMsg->wParam == VK_F4))
		{
			if (!m_documents.empty())
				CloseCurrentDocument();
			return TRUE;
		}

		// 处理 Ctrl + Tab / Ctrl + Shift + Tab - 循环切换标签页
		if (ctrlDown && !altDown && pMsg->wParam == VK_TAB)
		{
			const int docCount = static_cast<int>(m_documents.size());
			if (docCount > 1)
			{
				const int cur = (m_activeDocIndex >= 0 && m_activeDocIndex < docCount) ?
					m_activeDocIndex : 0;
				const int next = shiftDown ?
					((cur - 1 + docCount) % docCount) : ((cur + 1) % docCount);
				if (m_tabCtrl.GetSafeHwnd())
				{
					m_tabCtrl.SetCurSel(next);
					m_tabCtrl.UpdateWindow();
				}
				SwitchToDocument(next);
			}
			return TRUE;
		}

		// 处理 Ctrl + G - 聚焦顶部页码框快速跳页
		if (ctrlDown && !altDown && !shiftDown && pMsg->wParam == 'G')
		{
			if (m_isFullscreen)
				ExitFullscreen();
			if (m_editCurrent.GetSafeHwnd() && m_editCurrent.IsWindowVisible())
			{
				m_editCurrent.SetFocus();
				m_editCurrent.SetSel(0, -1);
			}
			return TRUE;
		}

		// 处理 Ctrl + (+ / - / 0) - 键盘放大、缩小、恢复适合宽度
		if (ctrlDown && !altDown && !IsEditControlFocused())
		{
			if (pMsg->wParam == VK_OEM_PLUS || pMsg->wParam == VK_ADD)
			{
				if (m_doc && m_totalPages > 0)
					OnBtnZoomIn();
				return TRUE;
			}
			if (pMsg->wParam == VK_OEM_MINUS || pMsg->wParam == VK_SUBTRACT)
			{
				if (m_doc && m_totalPages > 0)
					OnBtnZoomOut();
				return TRUE;
			}
			if (!shiftDown && (pMsg->wParam == '0' || pMsg->wParam == VK_NUMPAD0))
			{
				if (m_doc && m_totalPages > 0)
					OnBtnFitWidth();
				return TRUE;
			}
		}

		// 处理Ctrl+C - 复制选中文本
		if ((GetKeyState(VK_CONTROL) & 0x8000) && pMsg->wParam == 'C')
		{
			if (m_hasSelection && !IsEditControlFocused())
			{
				CopySelectionToClipboard();
				return TRUE;
			}
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

		// F9 切换目录侧栏
		if (pMsg->wParam == VK_F9)
		{
			ToggleThumbnailPanel();
			return TRUE;
		}

		// Ctrl+Shift+F 切换大小写匹配方式。
		if ((GetKeyState(VK_CONTROL) & 0x8000) &&
			(GetKeyState(VK_SHIFT) & 0x8000) && pMsg->wParam == 'F')
		{
			m_searchCaseSensitive = !m_searchCaseSensitive;
			if (!m_searchKeyword.IsEmpty())
				SearchPDF(m_searchKeyword, m_searchCaseSensitive);
			else
				SetStatusBarMessage(m_searchCaseSensitive ?
					_T("搜索：区分大小写") : _T("搜索：忽略大小写"));
			return TRUE;
		}

		// 处理Ctrl + F - 聚焦到搜索框
		if ((GetKeyState(VK_CONTROL) & 0x8000) && pMsg->wParam == 'F')
		{
			ShowSearchTools();
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
		// 焦点在搜索框或页码框等编辑框内时不拦截，保证输入光标移动与选字正常
		if (!IsEditControlFocused() &&
			(pMsg->wParam == VK_LEFT || pMsg->wParam == VK_RIGHT ||
				pMsg->wParam == VK_PRIOR || pMsg->wParam == VK_NEXT ||
				pMsg->wParam == VK_HOME || pMsg->wParam == VK_END))
		{
			OnKeyDown((UINT)pMsg->wParam, 1, 0);
			return TRUE;
		}
	}

	return CDialogEx::PreTranslateMessage(pMsg);
} 

void CXiaoGongPDFDlg::OnGetMinMaxInfo(MINMAXINFO* lpMMI)
{
	CDialogEx::OnGetMinMaxInfo(lpMMI);

	// 设置窗口的最小跟踪尺寸（这是用户通过拖动窗口边缘能够调整的最小大小）
	lpMMI->ptMinTrackSize.x = APP_MINWIDTH;
	lpMMI->ptMinTrackSize.y = APP_MINHEIGHT;

	if (m_isFullscreen || !GetSafeHwnd())
		return;

	MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
	HMONITOR hMonitor = ::MonitorFromWindow(GetSafeHwnd(), MONITOR_DEFAULTTONEAREST);
	if (::GetMonitorInfo(hMonitor, &monitorInfo))
	{
		const RECT& monitor = monitorInfo.rcMonitor;
		const RECT& work = monitorInfo.rcWork;
		RECT frame = { 0, 0, 0, 0 };
		::AdjustWindowRectEx(&frame, GetStyle(), FALSE, GetExStyle());
		lpMMI->ptMaxPosition.x = work.left - monitor.left + frame.left;
		lpMMI->ptMaxPosition.y = work.top - monitor.top + frame.top;
		lpMMI->ptMaxSize.x = work.right - work.left + frame.right - frame.left;
		lpMMI->ptMaxSize.y = work.bottom - work.top + frame.bottom - frame.top;
	}
}

// 进入全屏模式
void CXiaoGongPDFDlg::EnterFullscreen()
{
	if (m_isFullscreen)
		return;

#ifdef _DEBUG
	TRACE(_T("进入全屏\n"));
#endif

	// 保存进入全屏前的最大化状态与窗口位置
	m_wasMaximized = (IsZoomed() != FALSE);
	GetWindowRect(&m_windowRect);

	// ★★★ 保存当前缩放状态（含平移偏移、滚动位置、页码和实际缩放比例），用于退出全屏时恢复
	m_savedZoomMode = m_zoomMode;
	m_savedCustomZoom = m_customZoom;
	m_savedPanOffset = CPoint(m_scrollPositionH, m_scrollPosition);
	m_savedScrollPosition = m_scrollPosition;
	m_savedUniformScale = m_uniformScale;
	m_savedFullscreenPage = m_currentPage;

	// 获取当前窗口所在显示器的完整边界（支持多显示器）
	CRect monitorRect(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
	MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
	HMONITOR hMonitor = ::MonitorFromWindow(GetSafeHwnd(), MONITOR_DEFAULTTONEAREST);
	if (::GetMonitorInfo(hMonitor, &monitorInfo))
	{
		monitorRect = monitorInfo.rcMonitor;
	}

	// ★★★ 先设置全屏标志，避免 ShowWindow/SetWindowPos 触发 OnSize 时重新布局控件
	m_isFullscreen = true;
	KillTimer(TIMER_ID_RESIZE_DEBOUNCE);

	// 若进入全屏前处于最大化，先清除 WS_MAXIMIZE，避免退出全屏时 ShowWindow(SW_MAXIMIZE) 失效盖住任务栏
	if (m_wasMaximized)
		ShowWindow(SW_RESTORE);

	// 移除窗口边框和标题栏
	ModifyStyle(WS_CAPTION | WS_THICKFRAME, 0, SWP_FRAMECHANGED);

	// 隐藏工具栏、标签页、缩略图和其他控件
	SetControlsVisible(false);

	// 调整窗口大小为当前所在显示器全屏
	SetWindowPos(NULL, monitorRect.left, monitorRect.top,
		monitorRect.Width(), monitorRect.Height(), SWP_NOZORDER);

	// 在全屏模式下，只调整PDF预览控件大小为整个客户区
	// ★★★ 不隐藏 m_pdfView：让旧位图保持可见直到新位图渲染完成后原子替换，避免主窗口背景短暂裸露。
	if (m_pdfView.GetSafeHwnd())
	{
		m_pdfView.MoveWindow(0, 0, monitorRect.Width(), monitorRect.Height());
	}
	if (m_pdfVerticalScrollBar.GetSafeHwnd())
		m_pdfVerticalScrollBar.ShowWindow(SW_HIDE);
	if (m_pdfHorizontalScrollBar.GetSafeHwnd())
		m_pdfHorizontalScrollBar.ShowWindow(SW_HIDE);
	if (m_pdfScrollCorner.GetSafeHwnd())
		m_pdfScrollCorner.ShowWindow(SW_HIDE);

	// ★★★ 重新渲染当前页面以适应新的窗口尺寸
	if (m_doc && m_currentPage >= 0 && m_currentPage < m_totalPages)
	{
		// ★★★ 强制重置缩放模式，避免SetZoom的优化检查跳过渲染
		m_zoomMode = ZOOM_CUSTOM;
		m_customZoom = 0.5f;  // 设置一个不同的值

		// ★★★ 进入全屏时强制使用适应页面模式，确保充分利用全屏空间
		SetZoom(1.0f, ZOOM_FIT_PAGE);
	}
}

// 退出全屏模式
void CXiaoGongPDFDlg::ExitFullscreen()
{
	if (!m_isFullscreen)
		return;

#ifdef _DEBUG
	TRACE(_T("退出全屏\n"));
#endif

	const bool pageChangedInFullscreen = (m_savedFullscreenPage >= 0 && m_currentPage != m_savedFullscreenPage);

	// ★★★ 先恢复进入全屏前的缩放与滚动状态，避免随后布局触发的 OnSize 使用全屏的 ZOOM_FIT_PAGE 渲染
	if (m_doc && m_currentPage >= 0)
	{
		m_zoomMode = m_savedZoomMode;
		m_customZoom = m_savedCustomZoom;
		m_panOffset = CPoint(0, 0);
		if (m_savedUniformScale > 0.0f)
			m_uniformScale = m_savedUniformScale;
		if (!pageChangedInFullscreen)
		{
			m_scrollPositionH = max(0, m_savedPanOffset.x);
			m_scrollPosition = max(0, m_savedScrollPosition);
		}
		else
		{
			m_scrollPositionH = 0;
			m_scrollPosition = 0;
		}
	}

	// 恢复窗口样式
	ModifyStyle(0, WS_THICKFRAME, SWP_FRAMECHANGED);

	// ★★★ 重设全屏标志，让 ShowWindow/SetWindowPos 触发的 OnSize 能正确布局控件
	m_isFullscreen = false;

	// 显示隐藏的控件
	SetControlsVisible(true);

	// 恢复窗口位置和大小：若进入全屏前是最大化，直接还原最大化；否则还原普通窗口
	m_suppressResizeRender = true;
	if (m_wasMaximized)
	{
		ShowWindow(SW_MAXIMIZE);
		CRect clientRect;
		GetClientRect(&clientRect);
		SendMessage(WM_SIZE, SIZE_MAXIMIZED,
			MAKELPARAM(clientRect.Width(), clientRect.Height()));
	}
	else
	{
		SetWindowPos(NULL, m_windowRect.left, m_windowRect.top,
			m_windowRect.Width(), m_windowRect.Height(), SWP_NOZORDER);
	}
	m_suppressResizeRender = false;

	// 取消 OnSize 可能残留的防抖定时器，防止 60ms 后覆盖已恢复的滚动位置
	KillTimer(TIMER_ID_RESIZE_DEBOUNCE);

	if (m_doc && m_currentPage >= 0)
	{
		if (m_savedUniformScale > 0.0f)
			m_uniformScale = m_savedUniformScale;
		CalculatePagePositions(false);

		CRect viewRect;
		m_pdfView.GetClientRect(&viewRect);
		const int maxScroll = max(0, m_totalScrollHeight - viewRect.Height());
		if (pageChangedInFullscreen && m_currentPage < static_cast<int>(m_pageRowTops.size()))
			m_scrollPosition = max(0, min(m_pageRowTops[m_currentPage], maxScroll));
		else
			m_scrollPosition = max(0, min(m_savedScrollPosition, maxScroll));
		m_scrollPositionH = pageChangedInFullscreen ? 0 : max(0, m_savedPanOffset.x);

		UpdateScrollBar();
		RenderVisiblePages();
		SaveCurrentPageZoomState();
		UpdateZoomStatus();
	}

	m_savedFullscreenPage = -1;

	// 确保窗口和所有控件更新
	Invalidate(FALSE);
	UpdateWindow();
}

// ============ 缩放状态管理函数 ============

// 保存当前页面的缩放状态和平移位置
void CXiaoGongPDFDlg::SaveCurrentPageZoomState()
{
	CPDFDocument* pDoc = GetActiveDocument();
	if (!pDoc || m_currentPage < 0 || m_currentPage >= m_totalPages)
		return;

	pDoc->SetCurrentPage(m_currentPage);
	pDoc->SetZoom(m_customZoom, m_zoomMode);
	pDoc->SetPanOffset(CPoint(m_scrollPositionH, 0));
	pDoc->SetCanDrag(m_canDrag);
	pDoc->SaveCurrentPageZoomState();

#ifdef _DEBUG
	TRACE(_T("保存页面 %d 的状态: mode=%d, zoom=%.2f, scroll=(%d,%d)\n"),
		m_currentPage, m_zoomMode, m_customZoom, m_scrollPositionH, m_scrollPosition);
#endif
}

// 恢复指定页面的缩放状态和平移位置
void CXiaoGongPDFDlg::RestorePageZoomState(int pageNumber)
{
	if (pageNumber < 0 || pageNumber >= m_totalPages)
		return;

	CPDFDocument* pDoc = GetActiveDocument();
	if (!pDoc)
		return;

	pDoc->RestorePageZoomState(pageNumber);
	m_zoomMode = pDoc->GetZoomMode();
	if (m_zoomMode == ZOOM_FIT_PAGE || m_zoomMode == ZOOM_FIT_WIDTH)
	{
		m_nextFitMode = m_zoomMode == ZOOM_FIT_PAGE ? ZOOM_FIT_WIDTH : ZOOM_FIT_PAGE;
		UpdateFitButton();
	}
	m_customZoom = pDoc->GetCustomZoom();
	m_panOffset = CPoint(0, 0);
	m_canDrag = pDoc->GetCanDrag();
	m_zoom = (m_zoomMode == ZOOM_CUSTOM) ? m_customZoom : pDoc->GetZoom();

#ifdef _DEBUG
	TRACE(_T("恢复页面 %d 的状态: mode=%d, zoom=%.2f, scroll=(%d,%d)\n"),
		pageNumber, m_zoomMode, m_customZoom, m_scrollPositionH, m_scrollPosition);
#endif
}

// 获取页面缩放状态
::PageZoomState CXiaoGongPDFDlg::GetPageZoomState(int pageNumber)
{
	CPDFDocument* pDoc = GetActiveDocument();
	if (pDoc)
	{
		return pDoc->GetPageZoomState(pageNumber);
	}
	return ::PageZoomState();
}

// ============ 缩放功能实现 ============

float CXiaoGongPDFDlg::GetCurrentZoomBaseForStep() const
{
	if (m_zoomMode == ZOOM_CUSTOM)
		return m_customZoom;

	CPDFDocument* pDoc = const_cast<CXiaoGongPDFDlg*>(this)->GetActiveDocument();
	if (!pDoc || !m_pdfView.GetSafeHwnd())
		return m_customZoom;

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	if (viewRect.Width() <= 0 || viewRect.Height() <= 0)
		return m_customZoom;

	float maxRowPtWidth = pDoc->GetMaxPageWidth();
	int rowGapPx = 0;
	if (m_layoutMode == LAYOUT_TWO_PAGE_SPREAD && pDoc->HasPageBoundsCache())
	{
		maxRowPtWidth = 0.0f;
		int idx = 0;
		while (idx < m_totalPages)
		{
			int leftP = idx;
			int rightP = -1;
			GetLayoutRowPages(idx, leftP, rightP);
			float wL = 0.0f, hL = 0.0f;
			pDoc->GetPageBounds(leftP, wL, hL);
			float rowW = wL;
			if (rightP >= 0)
			{
				float wR = 0.0f, hR = 0.0f;
				pDoc->GetPageBounds(rightP, wR, hR);
				rowW += wR;
			}
			else
			{
				rowW += wL;
			}
			if (rowW > maxRowPtWidth)
				maxRowPtWidth = rowW;
			idx = (rightP >= 0) ? (rightP + 1) : (leftP + 1);
		}
		rowGapPx = SPREAD_GAP;
	}
	const int contentViewWidth = max(1, viewRect.Width() - 40 - rowGapPx);
	const float baseScale = (maxRowPtWidth > 0.0f) ?
		(static_cast<float>(contentViewWidth) / maxRowPtWidth * 0.95f) : 1.0f;
	if (baseScale > 0.0f && m_uniformScale > 0.0f)
		return m_uniformScale / baseScale;
	return m_customZoom;
}

void CXiaoGongPDFDlg::OnBtnZoomOut()
{
	const float baseZoom = GetCurrentZoomBaseForStep();
	SetZoom(max(0.25f, baseZoom / 1.1f), ZOOM_CUSTOM);
}

void CXiaoGongPDFDlg::OnBtnFitWidth()
{
	SetZoom(1.0f, ZOOM_FIT_WIDTH);
}

void CXiaoGongPDFDlg::OnBtnFitPage()
{
	SetZoom(1.0f, ZOOM_FIT_PAGE);
}

void CXiaoGongPDFDlg::OnBtnToggleFit()
{
	SetZoom(1.0f, m_nextFitMode);
}

void CXiaoGongPDFDlg::UpdateFitButton()
{
	if (!m_btnFitPage.GetSafeHwnd())
		return;
	const bool fitPageNext = m_nextFitMode == ZOOM_FIT_PAGE;
	m_btnFitPage.SetWindowText(fitPageNext ? _T("适页") : _T("适宽"));
	m_btnFitPage.SetToolTipText(fitPageNext ? _T("适合页面") : _T("适合宽度"));
}

void CXiaoGongPDFDlg::OnBtnZoomIn()
{
	const float baseZoom = GetCurrentZoomBaseForStep();
	SetZoom(min(4.0f, baseZoom * 1.1f), ZOOM_CUSTOM);
}

void CXiaoGongPDFDlg::OnBtnMore()
{
	CMenu menu;
	if (!menu.CreatePopupMenu())
		return;

	if (!m_recentFiles.empty())
	{
		CMenu recent;
		if (recent.CreatePopupMenu())
		{
			const int count = min(static_cast<int>(m_recentFiles.size()), RECENT_FILE_NUMS);
			for (int i = 0; i < count; ++i)
			{
				CString fileName = m_recentFiles[i];
				const int slash = max(fileName.ReverseFind(_T('\\')), fileName.ReverseFind(_T('/')));
				if (slash >= 0)
					fileName = fileName.Mid(slash + 1);
				fileName.Replace(_T("&"), _T("&&"));
				CString label;
				label.Format(_T("%d  %s"), i + 1, fileName.GetString());
				recent.AppendMenu(MF_STRING, ID_MENU_RECENT_FILE_1 + i, label);
			}
			if (menu.AppendMenu(MF_POPUP,
				reinterpret_cast<UINT_PTR>(recent.GetSafeHmenu()), _T("最近打开")))
			{
				recent.Detach();
				menu.AppendMenu(MF_SEPARATOR);
			}
		}
	}
	// 窗口压缩时自动隐藏的工具栏功能同步收纳到“更多”菜单中
	bool hasOverflowItems = false;
	auto appendOverflowItem = [&](UINT flags, UINT_PTR id, LPCTSTR label)
	{
		menu.AppendMenu(MF_STRING | flags, id, label);
		hasOverflowItems = true;
	};
	const UINT docFlag = (m_doc && m_totalPages > 0) ? 0 : MF_GRAYED;
	if (m_btnOpen.GetSafeHwnd() && !m_btnOpen.IsWindowVisible())
		appendOverflowItem(0, ID_MENU_OPEN, _T("打开"));
	if (m_btnOutline.GetSafeHwnd() && !m_btnOutline.IsWindowVisible())
		appendOverflowItem(docFlag | (m_outlineVisible ? MF_CHECKED : 0),
			IDC_CHECK_THUMBNAIL, _T("目录"));
	if (m_btnFind.GetSafeHwnd() && !m_btnFind.IsWindowVisible())
		appendOverflowItem(docFlag, IDC_BTN_FIND, _T("查找"));
	if (m_btnZoomOut.GetSafeHwnd() && !m_btnZoomOut.IsWindowVisible())
		appendOverflowItem(docFlag, IDC_BTN_ZOOM_OUT, _T("缩小"));
	if (m_btnZoomIn.GetSafeHwnd() && !m_btnZoomIn.IsWindowVisible())
		appendOverflowItem(docFlag, IDC_BTN_ZOOM_IN, _T("放大"));
	if (m_btnFitPage.GetSafeHwnd() && !m_btnFitPage.IsWindowVisible())
	{
		appendOverflowItem(docFlag, IDC_BTN_FIT_PAGE,
			m_nextFitMode == ZOOM_FIT_PAGE ? _T("适合页面") : _T("适合宽度"));
	}
	if (m_btnPrint.GetSafeHwnd() && !m_btnPrint.IsWindowVisible())
		appendOverflowItem(docFlag, ID_MENU_PRINT, _T("打印"));
	if (m_btnRotateRight.GetSafeHwnd() && !m_btnRotateRight.IsWindowVisible())
		appendOverflowItem(docFlag, IDC_BTN_ROTATE_RIGHT, _T("向右旋转"));
	if (m_btnEdit.GetSafeHwnd() && !m_btnEdit.IsWindowVisible())
		appendOverflowItem(docFlag, IDC_BTN_EDIT, _T("编辑页面"));
	if (m_btnFirst.GetSafeHwnd() && !m_btnFirst.IsWindowVisible())
		appendOverflowItem(docFlag, IDC_BTN_FIRST, _T("首页"));
	if (m_btnLast.GetSafeHwnd() && !m_btnLast.IsWindowVisible())
		appendOverflowItem(docFlag, IDC_BTN_LAST, _T("末页"));
	if (m_btnFullscreen.GetSafeHwnd() && !m_btnFullscreen.IsWindowVisible())
		appendOverflowItem(0, IDC_BTN_FULLSCREEN, _T("全屏"));
	if (hasOverflowItems)
		menu.AppendMenu(MF_SEPARATOR);

	// 页面视图模式子菜单（单页连续 / 双页并排）
	CMenu layoutMenu;
	if (layoutMenu.CreatePopupMenu())
	{
		layoutMenu.AppendMenu(MF_STRING | docFlag, ID_MENU_LAYOUT_SINGLE, _T("单页连续视图"));
		layoutMenu.AppendMenu(MF_STRING | docFlag, ID_MENU_LAYOUT_TWO_PAGE, _T("双页并排视图"));
		layoutMenu.CheckMenuRadioItem(ID_MENU_LAYOUT_SINGLE, ID_MENU_LAYOUT_TWO_PAGE,
			ID_MENU_LAYOUT_SINGLE + static_cast<int>(m_layoutMode), MF_BYCOMMAND);
		if (menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(layoutMenu.GetSafeHmenu()),
			_T("页面视图")))
		{
			layoutMenu.Detach();
		}
	}

	// PDF 批注与保存子菜单
	CMenu annotMenu;
	if (annotMenu.CreatePopupMenu())
	{
		const UINT selFlag = (m_doc && m_hasSelection && !m_selectionQuads.empty()) ? 0 : MF_GRAYED;
		annotMenu.AppendMenu(MF_STRING | selFlag, ID_MENU_ANNOT_HIGHLIGHT, _T("高亮选中文本\tCtrl+H"));
		annotMenu.AppendMenu(MF_STRING | selFlag, ID_MENU_ANNOT_UNDERLINE, _T("下划线选中文本\tCtrl+U"));
		annotMenu.AppendMenu(MF_STRING | selFlag, ID_MENU_ANNOT_STRIKEOUT, _T("删除线选中文本"));
		annotMenu.AppendMenu(MF_SEPARATOR);
		annotMenu.AppendMenu(MF_STRING | docFlag, ID_MENU_ANNOT_TEXT_NOTE, _T("添加文字批注\tCtrl+T"));
		annotMenu.AppendMenu(MF_SEPARATOR);
		annotMenu.AppendMenu(MF_STRING | docFlag, ID_MENU_SAVE_PDF, _T("保存批注与旋转回原 PDF\tCtrl+S"));
		annotMenu.AppendMenu(MF_STRING | docFlag, ID_MENU_SAVE_PDF_AS, _T("另存为新 PDF..."));
		if (menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(annotMenu.GetSafeHmenu()),
			_T("PDF 批注")))
		{
			annotMenu.Detach();
		}
	}
	menu.AppendMenu(MF_STRING | docFlag, ID_MENU_SAVE_PDF, _T("保存修改\tCtrl+S"));
	menu.AppendMenu(MF_STRING | docFlag, ID_MENU_DOC_PROPERTIES, _T("文档属性与幅面...\tCtrl+D"));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING | docFlag | (m_isAreaCaptureMode ? MF_CHECKED : 0),
		ID_MENU_REGION_CAPTURE, _T("区域截图\tCtrl+Shift+S"));
	menu.AppendMenu(MF_STRING | docFlag,
		ID_MENU_EXPORT_IMAGES, _T("导出为图片...\tCtrl+Shift+E"));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING, ID_MENU_SHORTCUTS, _T("快捷键"));
	CMenu readingMenu;
	if (readingMenu.CreatePopupMenu())
	{
		readingMenu.AppendMenu(MF_STRING, ID_MENU_READING_NORMAL, _T("正常"));
		readingMenu.AppendMenu(MF_STRING, ID_MENU_READING_NIGHT, _T("夜间"));
		readingMenu.AppendMenu(MF_STRING, ID_MENU_READING_EYE, _T("护眼"));
		readingMenu.CheckMenuRadioItem(ID_MENU_READING_NORMAL, ID_MENU_READING_EYE,
			ID_MENU_READING_NORMAL + m_readingMode, MF_BYCOMMAND);
		if (menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(readingMenu.GetSafeHmenu()),
			_T("阅读模式")))
			readingMenu.Detach();
	}
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING, ID_MENU_SET_DEFAULT, _T("设为默认 PDF 阅读器"));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING, ID_MENU_ABOUT, _T("关于"));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING, ID_MENU_EXIT, _T("退出"));

	CRect buttonRect;
	m_btnMore.GetWindowRect(&buttonRect);
	const UINT command = static_cast<UINT>(menu.TrackPopupMenu(
		TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
		buttonRect.right, buttonRect.bottom, this));
	if (command != 0)
		PostMessage(WM_COMMAND, MAKEWPARAM(command, 0));
}

void CXiaoGongPDFDlg::OnReadingMode(UINT nID)
{
	if (nID < ID_MENU_READING_NORMAL || nID > ID_MENU_READING_EYE)
		return;
	ApplyReadingMode(static_cast<ReadingMode>(nID - ID_MENU_READING_NORMAL));
	AfxGetApp()->WriteProfileInt(_T("Appearance"), _T("ReadingMode"), m_readingMode);
}

void CXiaoGongPDFDlg::ApplyReadingMode(ReadingMode mode)
{
	m_readingMode = mode;
	m_pdfView.SetReadingMode(mode);
	const bool dark = mode == READING_NIGHT;
	m_pdfVerticalScrollBar.SetDark(dark);
	m_pdfHorizontalScrollBar.SetDark(dark);
	const COLORREF tab = dark ? RGB(36, 39, 43) : RGB(230, 238, 253);
	const COLORREF toolbar = dark ? RGB(39, 43, 47) : RGB(255, 255, 255);
	const COLORREF hover = dark ? RGB(55, 65, 72) : RGB(236, 244, 255);
	const COLORREF pressed = dark ? RGB(69, 82, 91) : RGB(219, 235, 255);
	const COLORREF text = dark ? RGB(226, 231, 235) : RGB(32, 33, 36);
	const COLORREF searchPopupBg = dark ? RGB(50, 54, 58) : RGB(255, 255, 255);

	m_whiteBrush.DeleteObject();
	m_whiteBrush.CreateSolidBrush(tab);
	m_toolbarBrush.DeleteObject();
	m_toolbarBrush.CreateSolidBrush(toolbar);
	m_searchPopupBrush.DeleteObject();
	m_searchPopupBrush.CreateSolidBrush(searchPopupBg);
	m_tabCtrl.SetColors(tab, dark ? RGB(49, 54, 59) : RGB(240, 246, 255),
		dark ? toolbar : RGB(250, 252, 255));
	if (m_outlineTree.GetSafeHwnd())
	{
		m_outlineTree.SetBkColor(toolbar);
		m_outlineTree.SetTextColor(text);
		m_outlineTree.Invalidate(FALSE);
	}
	if (m_searchResultsList.GetSafeHwnd())
	{
		m_searchResultsList.SetBkColor(toolbar);
		m_searchResultsList.SetTextBkColor(toolbar);
		m_searchResultsList.SetTextColor(text);
		m_searchResultsList.Invalidate(FALSE);
	}
	CCustomButton* buttons[] = { &m_btnOpen, &m_btnZoomOut, &m_btnFitWidth,
		&m_btnFitPage, &m_btnZoomIn, &m_btnOutline, &m_btnFirst, &m_btnLast,
		&m_btnPrevPage, &m_btnNextPage, &m_btnPrint, &m_btnSnapshot,
		&m_btnPageLayout, &m_btnAnnotate,
		&m_btnFullscreen, &m_btnRotateLeft, &m_btnRotateRight, &m_btnEdit,
		&m_btnFind, &m_btnPrevMatch, &m_btnNextMatch, &m_btnMore,
		&m_btnCloseSearchResults };
	for (CCustomButton* pButton : buttons)
	{
		pButton->SetColors(toolbar, hover, pressed);
		pButton->SetTextColor(text);
	}
	CCustomButton* popupButtons[] = { &m_btnPrevMatch, &m_btnNextMatch, &m_btnCloseSearchPopup };
	for (CCustomButton* pButton : popupButtons)
	{
		pButton->SetColors(dark ? RGB(50, 54, 58) : RGB(255, 255, 255),
			dark ? RGB(65, 72, 80) : RGB(232, 241, 252),
			dark ? RGB(78, 86, 95) : RGB(211, 229, 250));
		pButton->SetTextColor(text);
	}
	UpdateOutlineButtonColors();
	UpdateSnapshotButtonColors();
	UpdatePageLayoutButton();
	UpdateAnnotateButtonColors();
	CCustomButton* titleButtons[] = { &m_btnMinimize, &m_btnMaximize };
	for (CCustomButton* pButton : titleButtons)
	{
		pButton->SetColors(tab, dark ? RGB(55, 60, 65) : RGB(213, 228, 251),
			dark ? RGB(72, 78, 84) : RGB(198, 220, 251));
		pButton->SetTextColor(text);
	}
	m_btnCloseWindow.SetColors(tab, RGB(232, 38, 51), RGB(192, 25, 38));
	m_btnCloseWindow.SetTextColor(text);

	// Windows 10 1809 used attribute 19; newer builds use 20.
	BOOL enabled = dark ? TRUE : FALSE;
	if (FAILED(DwmSetWindowAttribute(GetSafeHwnd(), 20, &enabled, sizeof(enabled))))
		DwmSetWindowAttribute(GetSafeHwnd(), 19, &enabled, sizeof(enabled));
	Invalidate(FALSE);
	if (m_toolbar.GetSafeHwnd()) m_toolbar.Invalidate(FALSE);
	if (m_statusBar.GetSafeHwnd()) m_statusBar.Invalidate(FALSE);
	if (m_zoomStatus.GetSafeHwnd()) m_zoomStatus.Invalidate(FALSE);
	if (m_pageTotal.GetSafeHwnd()) m_pageTotal.Invalidate(FALSE);
	if (m_searchResultsHeader.GetSafeHwnd()) m_searchResultsHeader.Invalidate(FALSE);
	if (m_editCurrent.GetSafeHwnd()) m_editCurrent.Invalidate(FALSE);
	if (m_editSearch.GetSafeHwnd()) m_editSearch.Invalidate(FALSE);
	if (m_searchPopup.GetSafeHwnd()) m_searchPopup.Invalidate(FALSE);
	if (m_searchMatchCount.GetSafeHwnd()) m_searchMatchCount.Invalidate(FALSE);
	if (m_pdfScrollCorner.GetSafeHwnd()) m_pdfScrollCorner.Invalidate(FALSE);

	// 夜间/护眼/普通模式切换后从缓存重新合成页面位图，避免背景二次反色或旧反色残留
	if (m_doc && m_totalPages > 0 && m_pdfView.GetSafeHwnd())
	{
		RenderVisiblePages();
	}
}

void CXiaoGongPDFDlg::UpdateOutlineButtonColors()
{
	const bool dark = m_readingMode == READING_NIGHT;
	m_btnOutline.SetColors(
		m_outlineVisible ? (dark ? RGB(65, 82, 88) : RGB(221, 235, 252)) :
			(dark ? RGB(39, 43, 47) : RGB(255, 255, 255)),
		dark ? RGB(55, 65, 72) : RGB(236, 244, 255),
		dark ? RGB(69, 82, 91) : RGB(219, 235, 255));
}

void CXiaoGongPDFDlg::UpdateSnapshotButtonColors()
{
	if (!m_btnSnapshot.GetSafeHwnd())
		return;
	const bool dark = m_readingMode == READING_NIGHT;
	m_btnSnapshot.SetColors(
		m_isAreaCaptureMode ? (dark ? RGB(65, 82, 88) : RGB(221, 235, 252)) :
			(dark ? RGB(39, 43, 47) : RGB(255, 255, 255)),
		dark ? RGB(55, 65, 72) : RGB(236, 244, 255),
		dark ? RGB(69, 82, 91) : RGB(219, 235, 255));
}

void CXiaoGongPDFDlg::UpdatePageLayoutButton()
{
	if (!m_btnPageLayout.GetSafeHwnd())
		return;
	const bool dark = m_readingMode == READING_NIGHT;
	const bool twoPageActive = (m_layoutMode == LAYOUT_TWO_PAGE_SPREAD);
	m_btnPageLayout.SetIconGlyph(twoPageActive ? 0xE89A : 0xE7C3);
	m_btnPageLayout.SetToolTipText(twoPageActive ?
		_T("当前：双页并排视图（点击切换为单页连续）") :
		_T("当前：单页连续视图（点击切换为双页并排）"));

	m_btnPageLayout.SetColors(
		twoPageActive ? (dark ? RGB(65, 82, 88) : RGB(221, 235, 252)) :
			(dark ? RGB(39, 43, 47) : RGB(255, 255, 255)),
		dark ? RGB(55, 65, 72) : RGB(236, 244, 255),
		dark ? RGB(69, 82, 91) : RGB(219, 235, 255));
	m_btnPageLayout.Invalidate(FALSE);
}

void CXiaoGongPDFDlg::UpdateAnnotateButtonColors()
{
	if (!m_btnAnnotate.GetSafeHwnd())
		return;
	const bool dark = m_readingMode == READING_NIGHT;
	CPDFDocument* pDoc = GetActiveDocument();
	const bool hasDirty = (pDoc && pDoc->HasUnsavedAnnotations());
	m_btnAnnotate.SetColors(
		hasDirty ? (dark ? RGB(65, 82, 88) : RGB(221, 235, 252)) :
			(dark ? RGB(39, 43, 47) : RGB(255, 255, 255)),
		dark ? RGB(55, 65, 72) : RGB(236, 244, 255),
		dark ? RGB(69, 82, 91) : RGB(219, 235, 255));
	m_btnAnnotate.Invalidate(FALSE);
}

void CXiaoGongPDFDlg::UpdateDocumentDirtyTitle()
{
	UpdateTabControl();
	if (m_tabCtrl.GetSafeHwnd())
		m_tabCtrl.Invalidate(FALSE);
	if (CPDFDocument* pDoc = GetActiveDocument())
	{
		CString title;
		title.Format(pDoc->HasUnsavedAnnotations() ?
			_T("小龚PDF阅读器 - * %s") : _T("小龚PDF阅读器 - %s"),
			pDoc->GetFileName().GetString());
		SetWindowText(title);
	}
	UpdateAnnotateButtonColors();
}

void CXiaoGongPDFDlg::OnBtnMinimize()
{
	ShowWindow(SW_MINIMIZE);
}

void CXiaoGongPDFDlg::OnBtnMaximize()
{
	ShowWindow(IsZoomed() ? SW_RESTORE : SW_MAXIMIZE);
}

void CXiaoGongPDFDlg::OnBtnCloseWindow()
{
	PostMessage(WM_CLOSE);
}

void CXiaoGongPDFDlg::SetZoom(float zoom, ZoomMode mode)
{
#ifdef _DEBUG
	TRACE(_T("SetZoom() : zoom=%.2f, mode=%d\n"), zoom, mode);
#endif

	if (!m_doc || m_totalPages <= 0)
		return;
	if (!m_isFullscreen && (mode == ZOOM_FIT_PAGE || mode == ZOOM_FIT_WIDTH))
	{
		m_nextFitMode = mode == ZOOM_FIT_PAGE ? ZOOM_FIT_WIDTH : ZOOM_FIT_PAGE;
		UpdateFitButton();
	}

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
	ClearActiveCaptureRect();

	// ★★★ 切换缩放模式时重置平移偏移量
	if (mode != ZOOM_CUSTOM)
	{
		ResetPanOffset();
	}

	// ★★★ 全屏模式下不覆盖用户原本保存的单页缩放状态
	if (!m_isFullscreen)
	{
		SaveCurrentPageZoomState();
	}

	// 保存当前的滚动位置比例（相对于总高度）
	float scrollRatio = 0.0f;
	if (m_totalScrollHeight > 0)
	{
		scrollRatio = (float)m_scrollPosition / m_totalScrollHeight;
	}

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);

	// 重新计算所有页面位置与统一缩放比例
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

	// 打印对话框与打印循环期间挂起外部文件变化通知，避免 m_doc/m_totalPages 在打印期间变化
	ScopedFileWatchSuspend suspendFileWatch(this);

	// 创建自定义打印对话框
	CCustomPrintDlg printDlg(m_totalPages, this);

	if (printDlg.DoModal() == IDOK)
	{
		if (!m_doc || m_totalPages <= 0)
		{
			MessageBox(_T("文档已关闭或不可用，打印已取消。"), _T("提示"), MB_OK | MB_ICONINFORMATION);
			return;
		}

		int startPage = max(0, printDlg.m_pageFrom - 1);
		int endPage = min(m_totalPages - 1, printDlg.m_pageTo - 1);
		if (startPage > endPage)
		{
			MessageBox(_T("打印页码范围无效，请重试。"), _T("提示"), MB_OK | MB_ICONWARNING);
			return;
		}

		// 份数与逐份/逐页顺序由应用层循环精确控制，强制驱动侧 dmCopies = 1 防止份数倍增
		DEVMODE* pDevMode = printDlg.GetDevMode();
		if (pDevMode)
		{
			pDevMode->dmFields |= DM_COPIES;
			pDevMode->dmCopies = 1;
		}

		// 获取打印机DC
		HDC hPrinterDC = CreateDC(NULL, printDlg.m_printerName, NULL, pDevMode);
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

		// 确定要打印的页面范围（已在上方重新校验并钳制到有效区间）
		CProgressDlg progress(this);
		progress.SetLoadCancelNotification(false);
		if (!progress.Create(IDD_PROGRESS_DIALOG, this))
		{
			dcPrint.AbortDoc();
			dcPrint.DeleteDC();
			MessageBox(_T("无法创建打印进度窗口"), _T("错误"), MB_OK | MB_ICONERROR);
			return;
		}
		progress.SetWindowText(_T("正在打印"));
		progress.SetIndeterminate(false);
		progress.ShowWindow(SW_SHOW);
		EnableWindow(FALSE);
		progress.EnableWindow(TRUE);
		long long completedPages = 0;
		const long long totalPagesToPrint =
			static_cast<long long>(endPage - startPage + 1) * printDlg.m_copies;
		auto printPage = [&](int pageNum) -> bool
		{
			CString status;
			status.Format(_T("正在打印第 %d 页 (%lld / %lld)"), pageNum + 1,
				completedPages + 1, totalPagesToPrint);
			progress.SetStatus(status);
			progress.UpdateWindow();
			if (!PrintPDFPageToDC(dcPrint, m_ctx, m_doc, pageNum,
				GetPageRotation(pageNum), m_renderLock, horRes, verRes))
				return false;
			++completedPages;
			progress.SetProgress(static_cast<int>(completedPages * 100LL / totalPagesToPrint));
			MSG message;
			while (::PeekMessage(&message, progress.GetSafeHwnd(), 0, 0, PM_REMOVE))
			{
				if (!progress.IsDialogMessage(&message))
				{
					::TranslateMessage(&message);
					::DispatchMessage(&message);
				}
			}
			return !progress.IsCancelled();
		};
		auto closeProgress = [&]()
		{
			EnableWindow(TRUE);
			progress.DestroyWindow();
			SetActiveWindow();
		};

		// 显示进度
		bool failed = false;
		int failedPage = -1;
		bool cancelled = false;

		try
		{
			// 如果需要自动分页，处理多份打印
			int totalCopies = printDlg.m_copies;

			if (printDlg.m_bCollate)
			{
				// 自动分页：逐份打印 (1,2,3) (1,2,3) (1,2,3)
				for (int copy = 0; copy < totalCopies && !failed; copy++)
				{
					for (int pageNum = startPage; pageNum <= endPage && !failed; pageNum++)
					{
						failed = !printPage(pageNum);
						if (failed) failedPage = pageNum;
					}
				}
			}
			else
			{
				// 不自动分页：按页打印 (1,1,1) (2,2,2) (3,3,3)
				for (int pageNum = startPage; pageNum <= endPage && !failed; pageNum++)
				{
					for (int copy = 0; copy < totalCopies && !failed; copy++)
					{
						failed = !printPage(pageNum);
						if (failed) failedPage = pageNum;
					}
				}
			}

			cancelled = progress.IsCancelled();
			closeProgress();
			if (failed || cancelled)
			{
				dcPrint.AbortDoc();
				if (!cancelled)
				{
					CString message;
					message.Format(_T("第 %d 页打印失败，任务已取消。"), failedPage + 1);
					MessageBox(message, _T("错误"), MB_OK | MB_ICONERROR);
				}
			}
			else if (dcPrint.EndDoc() < 0)
				MessageBox(_T("打印机未能完成任务。"), _T("错误"), MB_OK | MB_ICONERROR);
			else
				MessageBox(_T("打印任务已发送到打印机"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		}
		catch (...)
		{
			if (progress.GetSafeHwnd())
				closeProgress();
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

	OpenRecentFileAt(static_cast<int>(nID - ID_MENU_RECENT_FILE_1));
}

void CXiaoGongPDFDlg::OpenRecentFileAt(int index)
{
	if (index < 0 || index >= static_cast<int>(m_recentFiles.size()))
		return;

	const CString filePath = m_recentFiles[index];
	if (!PathFileExists(filePath))
	{
		MessageBox(_T("文件不存在或已被删除"), _T("错误"), MB_OK | MB_ICONERROR);
		m_recentFiles.erase(m_recentFiles.begin() + index);
		SaveRecentFiles();
		if (m_documents.empty() && m_pdfView.GetSafeHwnd())
			m_pdfView.Invalidate(FALSE);
		return;
	}

	OpenPDFInNewTab(filePath);
}

void CXiaoGongPDFDlg::RemoveRecentFileAt(int index)
{
	if (index < 0 || index >= static_cast<int>(m_recentFiles.size()))
		return;

	m_recentFiles.erase(m_recentFiles.begin() + index);
	SaveRecentFiles();
	if (m_documents.empty() && m_pdfView.GetSafeHwnd())
		m_pdfView.Invalidate(FALSE);
}

void CXiaoGongPDFDlg::ClearRecentFiles()
{
	if (m_recentFiles.empty())
		return;

	m_recentFiles.clear();
	SaveRecentFiles();
	if (m_documents.empty() && m_pdfView.GetSafeHwnd())
		m_pdfView.Invalidate(FALSE);
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
		bool skippedMissing = false;
		// 读取最多 RECENT_FILE_NUMS 个最近文件
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
				else
				{
					skippedMissing = true;
				}
			}
		}

		regKey.Close();
		if (skippedMissing)
			SaveRecentFiles();
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
		// 清理多余的旧尾部项，防止已删除记录重启后残留复活
		for (int i = count; i < RECENT_FILE_NUMS; i++)
		{
			CString valueName;
			valueName.Format(_T("File%d"), i);
			regKey.DeleteValue(valueName);
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
		std::vector<CString> pdfPaths;
		int rejectedCount = 0;
		for (UINT i = 0; i < fileCount; ++i)
		{
			TCHAR filePath[MAX_PATH] = { 0 };
			if (DragQueryFile(hDropInfo, i, filePath, MAX_PATH) == 0)
				continue;
			CString path(filePath);
			CString ext = path.Right(4);
			ext.MakeLower();
			if (ext == _T(".pdf"))
				pdfPaths.push_back(path);
			else
				++rejectedCount;
		}

		if (!pdfPaths.empty())
		{
			// 全部拖入的 PDF 都打开：首个正常加载，其余排队进入新标签页
			OpenMultiplePdfFiles(pdfPaths);
		}
		else if (rejectedCount > 0)
		{
			MessageBox(_T("请拖放PDF文件"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		}
	}

	DragFinish(hDropInfo);
}

// ============ 进程间通信：接收新打开的PDF文件路径 ============

BOOL CXiaoGongPDFDlg::OnCopyData(CWnd* pWnd, COPYDATASTRUCT* pCopyDataStruct)
{
	// dwData == 1：单个以 '\0' 结尾的路径（兼容旧实例）
	// dwData == 2：多字符串，每个路径以 '\0' 结尾，整体再以一个 '\0' 收尾
	if (pCopyDataStruct && pCopyDataStruct->lpData != nullptr &&
		pCopyDataStruct->cbData >= sizeof(TCHAR) &&
		(pCopyDataStruct->dwData == 1 || pCopyDataStruct->dwData == 2))
	{
		const TCHAR* rawChars = static_cast<const TCHAR*>(pCopyDataStruct->lpData);
		const size_t totalChars = pCopyDataStruct->cbData / sizeof(TCHAR);

		std::vector<CString> paths;
		size_t index = 0;
		while (index < totalChars)
		{
			size_t length = 0;
			while (index + length < totalChars && rawChars[index + length] != _T('\0'))
				++length;
			if (length == 0)
				break;  // 空字符串：多字符串列表结束

			CString filePath(rawChars + index, static_cast<int>(length));
			filePath.Trim();
			if (!filePath.IsEmpty() && PathFileExists(filePath))
				paths.push_back(filePath);

			index += length + 1;
			if (pCopyDataStruct->dwData == 1)
				break;  // 单路径格式只取第一个
		}

		if (!paths.empty())
		{
			OpenMultiplePdfFiles(paths);
			return TRUE;
		}
	}

	return CDialogEx::OnCopyData(pWnd, pCopyDataStruct);
}

// 批量打开：第一个文件走正常加载流程（显示进度窗口），
// 其余排入标签页恢复队列，由 OpenNextRestoredTab 在线程完成时逐个启动。
void CXiaoGongPDFDlg::OpenMultiplePdfFiles(const std::vector<CString>& paths)
{
	if (paths.empty() || m_closePending)
		return;

	OpenPDFInNewTab(paths.front());
	if (paths.size() == 1)
		return;

	// 上一轮恢复队列已消费完（游标停在末尾）时必须重置，
	// 否则新入队路径落在游标之前，永远不会被 OpenNextRestoredTab 取到。
	if (m_sessionRestoreIndex >= m_sessionRestorePaths.size())
	{
		m_sessionRestorePaths.clear();
		m_sessionRestoreIndex = 0;
		m_sessionActivePath.Empty();
	}

	// 规范化后再入队：OpenPDFInNewTab 内部按规范化路径去重，
	// 避免同一文件以不同写法被并发加载两次。
	for (size_t i = 1; i < paths.size(); ++i)
		m_sessionRestorePaths.push_back(FullPathForComparison(paths[i]));
	OpenNextRestoredTab();
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

	// 配置标签页控件
	m_tabCtrl.EnableCloseButton(TRUE);  // 启用关闭按钮
	m_tabCtrl.SetTabHeight(TITLE_BAR_HEIGHT);
	m_tabCtrl.SetColors(RGB(240, 244, 248), RGB(225, 234, 244), RGB(255, 255, 255));

	// 所有标签页使用相同宽度
	m_tabCtrl.ModifyStyle(0, TCS_SCROLLOPPOSITE | TCS_TOOLTIPS | TCS_FIXEDWIDTH);

	UpdateTabWidth();

#ifdef _DEBUG
	TRACE(_T("现代化标签页控件初始化完成\n"));
#endif

	// 确保标签页可见
	m_tabCtrl.ShowWindow(SW_SHOW);
}

void CXiaoGongPDFDlg::UpdateTabWidth()
{
	if (!m_tabCtrl.GetSafeHwnd())
		return;

	CRect rect;
	m_tabCtrl.GetClientRect(&rect);
	const int tabCount = max(1, m_tabCtrl.GetItemCount());
	const int availableWidth = rect.Width() / tabCount;
	const int tabWidth = min(CModernTabCtrl::MAX_TAB_WIDTH,
		max(CModernTabCtrl::MIN_TAB_WIDTH, availableWidth));
	m_tabCtrl.SendMessage(TCM_SETITEMSIZE, 0, MAKELPARAM(tabWidth, TITLE_BAR_HEIGHT));
	m_tabCtrl.HideOverflowButtons();
	m_tabCtrl.Invalidate(FALSE);
}

CPDFDocument* CXiaoGongPDFDlg::GetActiveDocument()
{
	if (m_activeDocIndex >= 0 && m_activeDocIndex < (int)m_documents.size())
	{
		return m_documents[m_activeDocIndex];
	}
	return nullptr;
}

bool CXiaoGongPDFDlg::OpenPDFInNewTab(const CString& filePath, bool restoringSession)
{
	if (m_closePending)
		return false;
#ifdef _DEBUG
	TRACE(_T("OpenPDFInNewTab: %s\n"), filePath);
	TRACE(_T("当前已打开文档数: %d\n"), (int)m_documents.size());
	TRACE(_T("标签页控件句柄: %p\n"), m_tabCtrl.GetSafeHwnd());
#endif

	// 检查文件是否已经打开
	// 规范化路径用于比较（转为小写、获取完整路径,win路径不区分大小写，但是字符比较区分）
	CString normalizedNewPath = FullPathForComparison(filePath);

#ifdef _DEBUG
	TRACE(_T("规范化路径: %s\n"), normalizedNewPath);
#endif

	// 遍历已打开的文档，检查是否重复
	for (int i = 0; i < (int)m_documents.size(); i++)
	{
		if (!m_documents[i])
			continue;
		CString existingPath = m_documents[i]->GetFilePath();

		// 规范化已打开的文件路径
		CString normalizedExistingPath = FullPathForComparison(existingPath);

		// 比较路径
		if (normalizedNewPath.CompareNoCase(normalizedExistingPath) == 0)
		{
#ifdef _DEBUG
			TRACE(_T("文件已打开，切换到标签页索引: %d\n"), i);
#endif
			// 文件已打开，切换到对应标签页
			if (m_tabCtrl.GetSafeHwnd())
				m_tabCtrl.SetCurSel(i);
			SwitchToDocument(i);

			// 使用非阻塞状态栏提示，避免在 WM_COPYDATA 转发或会话恢复期间阻塞发送进程
			if (!restoringSession)
				SetStatusBarMessage(_T("该文件已经打开，已切换到对应标签页"));

			return true;
		}
	}
	for (PDFLoadParams* pPending : m_pendingLoads)
	{
		if (normalizedNewPath.CompareNoCase(pPending->filePath) == 0)
			return true;
	}

	// 转换为UTF-8 mupdf需要转换成 UTF-8形式
	CStringW wPath(normalizedNewPath);
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

	//以上转化是标准的2次转化模式， 一次得出转换大小，一次实际转换

	// ★★★ 使用后台线程加载PDF，避免大文件加载时UI卡死
#ifdef _DEBUG
	TRACE(_T("准备后台加载PDF文件...\n"));
#endif

	// 创建新文档对象
	CPDFDocument* newDoc = new CPDFDocument();

	// 创建加载参数
	PDFLoadParams* pParams = new PDFLoadParams();
	pParams->filePath = normalizedNewPath;
	pParams->utf8Path = utf8Path;
	pParams->notifyWnd = GetSafeHwnd();
	pParams->pDoc = newDoc;
	pParams->success = false;
	pParams->restoringSession = restoringSession;
	if (!restoringSession)
	{
		CProgressDlg* pProgressDlg = new CProgressDlg(this);
		if (!pProgressDlg->Create(IDD_PROGRESS_DIALOG, this))
		{
			delete pProgressDlg;
			delete newDoc;
			delete pParams;
			return false;
		}
		pParams->pProgressDlg = pProgressDlg;
		pProgressDlg->ShowWindow(SW_SHOW);
		pProgressDlg->UpdateWindow();
	}

	// 启动后台加载线程
	pParams->pThread = AfxBeginThread(PDFLoadThreadProc, pParams, THREAD_PRIORITY_NORMAL, 0, CREATE_SUSPENDED);
	if (pParams->pThread)
	{
		pParams->pThread->m_bAutoDelete = FALSE;
		m_pendingLoads.push_back(pParams);
		pParams->pThread->ResumeThread();
	}
	else
	{
		// 线程创建失败 ,电脑资源极度紧张，概率很小
		DWORD dwError = GetLastError();
		delete newDoc;
		CloseProgressDialog(pParams);
		delete pParams;
		CString strMsg;
		strMsg.Format(_T("启动后台线程失败 (错误码: %d)\n\n")
			_T("可能原因：系统资源不足\n")
			_T("建议：保存当前工作，重启程序后重试"),
			dwError);
		AfxMessageBox(strMsg, MB_OK | MB_ICONERROR);
		return false;
	}

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
	if (index != m_activeDocIndex)
		StopSearch();

	// 切换文档时提交当前内联文字批注并清除文本选择与截图模式
	FinishInlineTextAnnot(false);
	ClearSelection();
	CancelAreaCaptureMode(false);
	InvalidateLinkCache();

	// 保存当前文档的状态
	if (m_activeDocIndex >= 0 && m_activeDocIndex < (int)m_documents.size())
	{
		CPDFDocument* oldDoc = m_documents[m_activeDocIndex];
		if (oldDoc)
		{
#ifdef _DEBUG
			TRACE(_T("保存旧文档 %d 的状态\n"), m_activeDocIndex);
#endif
			SaveDialogStateToDocument(oldDoc);
			SaveReadingPosition(oldDoc);

#ifdef _DEBUG
			TRACE(_T("已保存滚动位置: %d\n"), m_scrollPosition);
#endif
		}
	}

	// 切换到新文档
	const bool wasNoActiveDoc = (m_activeDocIndex < 0);
	CPDFDocument* newDoc = m_documents[index];
	if (!newDoc)
		return;
	m_activeDocIndex = index;

#ifdef _DEBUG
	TRACE(_T("切换到新文档 %d\n"), index);
	TRACE(_T("新文档指针: %p\n"), newDoc);
	TRACE(_T("新文档文件名: %s\n"), newDoc->GetFileName());
#endif

	// 更新对话框的成员变量（从CPDFDocument复制到对话框）
	LoadDialogStateFromDocument(newDoc);

	// ★★★ 立即清空页面位置缓存，防止在CalculatePagePositions()调用前触发RenderVisiblePages()导致越界
	m_pageYPositions.clear();
	m_pageHeights.clear();
	m_pageRowTops.clear();
	m_pageRowHeights.clear();

#ifdef _DEBUG
	TRACE(_T("m_doc: %p, 总页数: %d, 当前页: %d\n"), m_doc, m_totalPages, m_currentPage);
#endif

	// ★★★ 重置平移位置（连续滚动不使用拖拽平移）
	m_panOffset = CPoint(0, 0);
	m_canDrag = false;

#ifdef _DEBUG
	TRACE(_T("恢复旋转状态，旋转信息数量: %d\n"), (int)m_pageRotations.size());
	TRACE(_T("恢复文档缩放状态: zoom=%.2f, mode=%d, customZoom=%.2f\n"),
		m_zoom, m_zoomMode, m_customZoom);
#endif

	// 更新窗口标题与未保存星标
	UpdateDocumentDirtyTitle();

	// ★★★ 恢复搜索信息（每个文档的搜索结果应该独立），同步文本时屏蔽 EN_CHANGE 防止清空搜索结果
	const bool hadSearchPanel = m_searchResultsVisible;
	m_searchResultsVisible = false;
	if (m_editSearch.GetSafeHwnd())
	{
		m_isUpdatingSearchEdit = true;
		m_editSearch.SetWindowText(m_searchKeyword);
		m_isUpdatingSearchEdit = false;
	}
	RefreshSearchResultsList();
	if ((wasNoActiveDoc || hadSearchPanel) && GetSafeHwnd())
	{
		CRect clientRect;
		GetClientRect(&clientRect);
		m_suppressResizeRender = true;
		SendMessage(WM_SIZE, SIZE_RESTORED, MAKELPARAM(clientRect.Width(), clientRect.Height()));
		m_suppressResizeRender = false;
		KillTimer(TIMER_ID_RESIZE_DEBOUNCE);
	}

#ifdef _DEBUG
	TRACE(_T("已恢复搜索信息: %d 个匹配项, 当前索引=%d\n"),
		(int)m_searchMatches.size(), m_currentMatchIndex);
#endif

	// ★★★ 恢复该文档上次的实际渲染缩放比例
	float savedUniformScale = newDoc->GetUniformScale();
	if (savedUniformScale > 0.0f)
	{
		// 已保存过：直接恢复上次的缩放
		m_uniformScale = savedUniformScale;
		CalculatePagePositions(m_zoomMode == ZOOM_FIT_WIDTH || m_zoomMode == ZOOM_FIT_PAGE);
	}
	else if (m_zoomMode == ZOOM_FIT_WIDTH || m_zoomMode == ZOOM_FIT_PAGE)
	{
		m_customZoom = 1.0f;
		CalculatePagePositions(true);
	}
	else
	{
		// 首次打开：将目标实际缩放比例反推为基于 baseScale 的 m_customZoom
		const float desiredScale = (m_customZoom > 0.0f) ? m_customZoom : 1.0f;
		m_customZoom = 1.0f;
		CalculatePagePositions(true);
		if (m_uniformScale > 0.0f)
		{
			float baseScale = m_uniformScale;
			m_customZoom = desiredScale / baseScale;
			m_uniformScale = desiredScale;
		}
		CalculatePagePositions(false);
	}
	UpdateScrollBar();

	RenderVisiblePages();

	// 更新UI控件
	UpdatePageControls();
	UpdateStatusBar();

	if (m_outlineVisible)
		UpdateOutlineTree();
	else if (m_outlineTree.GetSafeHwnd())
		m_outlineTree.DeleteAllItems();
}

bool CXiaoGongPDFDlg::CloseDocument(int index, bool promptUnsaved)
{
#ifdef _DEBUG
	TRACE(_T("CloseDocument: %d\n"), index);
#endif

	if (index < 0 || index >= (int)m_documents.size())
	{
		return false;
	}

	if (index == m_activeDocIndex)
		FinishInlineTextAnnot(false);

	CPDFDocument* doc = m_documents[index];
	if (promptUnsaved && doc && doc->HasUnsavedAnnotations())
	{
		if (!PromptSaveUnsavedAnnotations(doc))
			return false;
		// PromptSaveUnsavedAnnotations 内部写回重开期间可能调整状态，重新确认索引
		auto curIt = std::find(m_documents.begin(), m_documents.end(), doc);
		if (curIt == m_documents.end())
			return true;
		index = static_cast<int>(curIt - m_documents.begin());
	}

	bool isActiveDocument = (index == m_activeDocIndex);
	const bool closingLastDocument = isActiveDocument && m_documents.size() == 1;

	// 取消文件变化监听
	if (doc && !doc->GetFilePath().IsEmpty())
		m_fileWatcher.RemoveWatch(doc->GetFilePath());
	if (m_pSearchParams && m_pSearchParams->pDoc == doc)
		StopSearch();
	// 最后一个文档关闭期间会多次更新布局，只在布局完成后绘制空视图。
	if (closingLastDocument)
		m_pdfView.SetRedraw(FALSE);

	if (isActiveDocument)
	{
		KillTimer(TIMER_ID_PAGE_PREFETCH);
		ClearSelection();
		CancelAreaCaptureMode(false);
		InvalidateLinkCache();
		m_pdfView.SetDisplayBitmap(NULL);
		SafeDeleteBitmap(m_hContinuousViewBitmap);

		if (doc)
		{
			SaveDialogStateToDocument(doc);
			SaveReadingPosition(doc);
		}

		m_ctx = nullptr;
		m_doc = nullptr;
		m_totalPages = 0;
		m_currentPage = 0;
		m_scrollPosition = 0;
		m_scrollPositionH = 0;
		m_panOffset = CPoint(0, 0);
		m_canDrag = false;
		m_pageRotations.clear();
		m_searchMatches.clear();
		m_searchMatchesByPage.clear();
		m_currentMatchIndex = -1;
		m_searchKeyword.Empty();
		m_searchResultsVisible = false;
		RefreshSearchResultsList();
	}

	// 删除文档对象
	if (doc)
	{
		delete doc;
	}

	// 从列表中移除
	m_documents.erase(m_documents.begin() + index);

	// 删除标签页
	m_tabCtrl.DeleteItem(index);
	UpdateTabWidth();

	// 如果关闭的是当前文档
	if (isActiveDocument)
	{
		if (m_documents.empty())
		{
			// 没有文档了，重置状态并释放布局残留
			m_activeDocIndex = -1;
			m_ctx = nullptr;
			m_doc = nullptr;
			m_totalPages = 0;
			m_currentPage = 0;
			m_pageYPositions.clear();
			m_pageHeights.clear();
			m_pageRowTops.clear();
			m_pageRowHeights.clear();
			m_layoutMode = LAYOUT_SINGLE_CONTINUOUS;
			UpdatePageLayoutButton();
			UpdateAnnotateButtonColors();
			m_totalScrollHeight = 0;
			m_totalScrollWidth = 0;

			if (m_editSearch.GetSafeHwnd())
			{
				m_isUpdatingSearchEdit = true;
				m_editSearch.SetWindowText(_T(""));
				m_isUpdatingSearchEdit = false;
			}
			SetWindowText(_T("小龚PDF阅读器"));
			if (m_outlineTree.GetSafeHwnd())
			{
				m_outlineTree.DeleteAllItems();
				m_outlineTree.ShowWindow(SW_HIDE);
			}

			// ★★★ 彻底清理并隐藏标签页控件
			if (m_tabCtrl.GetSafeHwnd())
			{
				// 删除所有标签页项
				m_tabCtrl.DeleteAllItems();
				// 隐藏标签页控件
				m_tabCtrl.ShowWindow(SW_HIDE);
			}

			// 清空显示
			CleanupBitmap();
			if (m_isFullscreen)
				ExitFullscreen();
			UpdatePageControls();
			UpdateStatusBar();

			// 刷新布局
			CRect rect;
			GetClientRect(&rect);
			SendMessage(WM_SIZE, SIZE_RESTORED, MAKELPARAM(rect.Width(), rect.Height()));

			// 无文档时隐藏滚动条（必须在 OnSize 之后，防止 OnSize 重新显示）
			m_pdfView.ShowScrollBar(SB_VERT, FALSE);
			m_pdfView.ShowScrollBar(SB_HORZ, FALSE);
			m_pdfView.SetRedraw(TRUE);
			m_pdfView.RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
		}
		else
		{
			// 切换到其他文档
			int newIndex = (index > 0) ? (index - 1) : 0;
			m_activeDocIndex = -1;
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
	return true;
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
	if (!m_tabCtrl.GetSafeHwnd())
		return;

	// 更新所有标签页的标题（去掉 .pdf 后缀）与文档指针关联
	for (int i = 0; i < (int)m_documents.size(); i++)
	{
		CPDFDocument* pDoc = m_documents[i];
		if (!pDoc)
			continue;

		CString tabTitle = pDoc->GetFileName();
		const int dotPos = tabTitle.ReverseFind(_T('.'));
		if (dotPos != -1)
		{
			CString ext = tabTitle.Mid(dotPos);
			ext.MakeLower();
			if (ext == _T(".pdf"))
				tabTitle = tabTitle.Left(dotPos);
		}

		if (pDoc->HasUnsavedAnnotations())
			tabTitle = _T("* ") + tabTitle;

		TCITEM tci = { 0 };
		tci.mask = TCIF_TEXT | TCIF_PARAM;
		tci.pszText = (LPTSTR)(LPCTSTR)tabTitle;
		tci.lParam = reinterpret_cast<LPARAM>(pDoc);
		m_tabCtrl.SetItem(i, &tci);
	}
}

void CXiaoGongPDFDlg::RefreshDocumentLayout()
{
	CRect rect;
	GetClientRect(&rect);
	PostMessage(WM_SIZE, SIZE_RESTORED, MAKELPARAM(rect.Width(), rect.Height()));
}

void CXiaoGongPDFDlg::AppendOutlineItems(const fz_outline* pOutline, HTREEITEM parent)
{
	for (const fz_outline* pItem = pOutline; pItem; pItem = pItem->next)
	{
		CString title;
		if (pItem->title && *pItem->title)
		{
			const int length = MultiByteToWideChar(CP_UTF8, 0, pItem->title, -1, nullptr, 0);
			if (length > 1)
			{
				LPWSTR pBuffer = title.GetBuffer(length);
				MultiByteToWideChar(CP_UTF8, 0, pItem->title, -1, pBuffer, length);
				title.ReleaseBuffer();
			}
		}
		if (title.IsEmpty())
			title = _T("未命名章节");

		HTREEITEM item = m_outlineTree.InsertItem(title, parent, TVI_LAST);
		if (!item)
			continue;
		if (pItem->page.chapter >= 0 && pItem->page.page >= 0)
		{
			int pageNumber = -1;
			fz_var(pageNumber);
			fz_try(m_ctx)
			{
				pageNumber = fz_page_number_from_location(m_ctx, m_doc, pItem->page);
			}
			fz_catch(m_ctx)
			{
				pageNumber = -1;
			}
			if (pageNumber >= 0 && pageNumber < m_totalPages)
				m_outlineTree.SetItemData(item, static_cast<DWORD_PTR>(pageNumber + 1));
		}
		if (pItem->down)
		{
			AppendOutlineItems(pItem->down, item);
			if (pItem->is_open)
				m_outlineTree.Expand(item, TVE_EXPAND);
		}
	}
}

void CXiaoGongPDFDlg::UpdateOutlineTree()
{
	if (!m_outlineTree.GetSafeHwnd())
		return;
	m_isUpdatingOutline = true;
	m_outlineTree.SetRedraw(FALSE);
	m_outlineTree.DeleteAllItems();
	if (m_ctx && m_doc)
	{
		fz_outline* pOutline = nullptr;
		fz_var(pOutline);
		bool failed = false;
		fz_try(m_ctx)
		{
			pOutline = fz_load_outline(m_ctx, m_doc);
		}
		fz_catch(m_ctx)
		{
			if (pOutline)
				fz_drop_outline(m_ctx, pOutline);
			pOutline = nullptr;
			failed = true;
		}
		if (pOutline)
		{
			AppendOutlineItems(pOutline, TVI_ROOT);
			fz_drop_outline(m_ctx, pOutline);
		}
		if (!m_outlineTree.GetRootItem())
			m_outlineTree.InsertItem(failed ? _T("目录读取失败") : _T("此文档没有目录"));
	}
	m_outlineTree.SetRedraw(TRUE);
	m_outlineTree.Invalidate(FALSE);
	m_isUpdatingOutline = false;
}

void CXiaoGongPDFDlg::OnOutlineSelectionChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	*pResult = 0;
	if (m_isUpdatingOutline || !m_doc)
		return;
	const NMTREEVIEW* pChange = reinterpret_cast<NMTREEVIEW*>(pNMHDR);
	if (!pChange->itemNew.hItem)
		return;
	const DWORD_PTR page = m_outlineTree.GetItemData(pChange->itemNew.hItem);
	if (page > 0 && page <= static_cast<DWORD_PTR>(m_totalPages))
		GoToPage(static_cast<int>(page - 1));
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
	UNREFERENCED_PARAMETER(lParam);
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

// 处理标签页左右拖拽重排事件
LRESULT CXiaoGongPDFDlg::OnTabReorder(WPARAM wParam, LPARAM lParam)
{
	const int fromIndex = static_cast<int>(wParam);
	const int toIndex = static_cast<int>(lParam);
	const int count = static_cast<int>(m_documents.size());
	if (fromIndex < 0 || fromIndex >= count ||
		toIndex < 0 || toIndex >= count || fromIndex == toIndex)
	{
		return 0;
	}

	CPDFDocument* pActiveDoc = GetActiveDocument();
	CPDFDocument* pMovedDoc = m_documents[fromIndex];
	m_documents.erase(m_documents.begin() + fromIndex);
	m_documents.insert(m_documents.begin() + toIndex, pMovedDoc);

	if (pActiveDoc)
	{
		auto it = std::find(m_documents.begin(), m_documents.end(), pActiveDoc);
		if (it != m_documents.end())
			m_activeDocIndex = static_cast<int>(it - m_documents.begin());
	}

	UpdateTabControl();
	if (m_tabCtrl.GetSafeHwnd() && m_activeDocIndex >= 0)
	{
		m_tabCtrl.SetCurSel(m_activeDocIndex);
		m_tabCtrl.Invalidate(FALSE);
	}
	SaveOpenTabs();
	return 0;
}

// 处理标签页右键菜单事件
LRESULT CXiaoGongPDFDlg::OnTabContextMenu(WPARAM wParam, LPARAM lParam)
{
	const int tabIndex = static_cast<int>(wParam);
	if (tabIndex < 0 || tabIndex >= static_cast<int>(m_documents.size()))
		return 0;

	CPDFDocument* pTargetDoc = m_documents[tabIndex];
	if (!pTargetDoc)
		return 0;

	enum TabMenuCmd
	{
		CMD_TAB_CLOSE = 1,
		CMD_TAB_CLOSE_OTHERS,
		CMD_TAB_CLOSE_RIGHT,
		CMD_TAB_SAVE_PDF,
		CMD_TAB_DOC_PROPS,
		CMD_TAB_COPY_PATH,
		CMD_TAB_OPEN_FOLDER
	};

	CMenu menu;
	if (!menu.CreatePopupMenu())
		return 0;

	const int docCount = static_cast<int>(m_documents.size());
	menu.AppendMenu(MF_STRING, CMD_TAB_CLOSE, _T("关闭标签页\tCtrl+W"));
	menu.AppendMenu(MF_STRING | (docCount > 1 ? 0 : MF_GRAYED),
		CMD_TAB_CLOSE_OTHERS, _T("关闭其他标签页"));
	menu.AppendMenu(MF_STRING | (tabIndex < docCount - 1 ? 0 : MF_GRAYED),
		CMD_TAB_CLOSE_RIGHT, _T("关闭右侧标签页"));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING, CMD_TAB_SAVE_PDF, _T("保存批注与旋转\tCtrl+S"));
	menu.AppendMenu(MF_STRING, CMD_TAB_DOC_PROPS, _T("文档属性与幅面...\tCtrl+D"));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING, CMD_TAB_COPY_PATH, _T("复制完整路径"));
	menu.AppendMenu(MF_STRING, CMD_TAB_OPEN_FOLDER, _T("打开文件所在位置"));

	const CPoint screenPt(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
	const UINT cmd = static_cast<UINT>(menu.TrackPopupMenu(
		TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
		screenPt.x, screenPt.y, this));

	// TrackPopupMenu 期间可能有外部事件改变标签列表，重新定位目标文档
	auto curIt = std::find(m_documents.begin(), m_documents.end(), pTargetDoc);
	if (curIt == m_documents.end())
		return 0;
	const int curTabIndex = static_cast<int>(curIt - m_documents.begin());

	switch (cmd)
	{
	case CMD_TAB_CLOSE:
		CloseDocument(curTabIndex);
		break;

	case CMD_TAB_CLOSE_OTHERS:
		if (m_activeDocIndex != curTabIndex)
		{
			if (m_tabCtrl.GetSafeHwnd())
				m_tabCtrl.SetCurSel(curTabIndex);
			SwitchToDocument(curTabIndex);
		}
		for (int i = static_cast<int>(m_documents.size()) - 1; i >= 0; --i)
		{
			if (m_documents[i] != pTargetDoc)
			{
				if (!CloseDocument(i))
					break;
			}
		}
		break;

	case CMD_TAB_CLOSE_RIGHT:
		if (m_activeDocIndex > curTabIndex)
		{
			if (m_tabCtrl.GetSafeHwnd())
				m_tabCtrl.SetCurSel(curTabIndex);
			SwitchToDocument(curTabIndex);
		}
		for (int i = static_cast<int>(m_documents.size()) - 1; i > curTabIndex; --i)
		{
			if (!CloseDocument(i))
				break;
		}
		break;

	case CMD_TAB_SAVE_PDF:
		SaveDocumentEdits(pTargetDoc, false);
		break;

	case CMD_TAB_DOC_PROPS:
		if (m_activeDocIndex != curTabIndex)
		{
			if (m_tabCtrl.GetSafeHwnd())
				m_tabCtrl.SetCurSel(curTabIndex);
			SwitchToDocument(curTabIndex);
		}
		OnMenuDocProperties();
		break;

	case CMD_TAB_COPY_PATH:
		if (CopyTextToClipboard(pTargetDoc->GetFilePath()))
			SetStatusBarMessage(_T("已复制文件完整路径"));
		break;

	case CMD_TAB_OPEN_FOLDER:
	{
		const CString filePath = pTargetDoc->GetFilePath();
		if (!filePath.IsEmpty())
		{
			PIDLIST_ABSOLUTE pidl = ::ILCreateFromPath(filePath);
			if (pidl)
			{
				::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
				::ILFree(pidl);
			}
			else
			{
				CString folder = filePath;
				const int slash = max(folder.ReverseFind(_T('\\')), folder.ReverseFind(_T('/')));
				if (slash >= 0)
					folder = folder.Left(slash);
				::ShellExecute(nullptr, _T("open"), folder, nullptr, nullptr, SW_SHOWNORMAL);
			}
		}
		break;
	}
	default:
		break;
	}

	return 0;
}

// ============ 目录侧栏切换 ============

void CXiaoGongPDFDlg::ToggleThumbnailPanel()
{
#ifdef _DEBUG
	TRACE(_T("ToggleThumbnailPanel()\n"));
#endif

	OnCheckThumbnail();
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
	FinishInlineTextAnnot(false);

	// 退出程序前检查所有已打开标签页是否有未保存的批注修改
	for (size_t i = 0; i < m_documents.size(); ++i)
	{
		CPDFDocument* pDoc = m_documents[i];
		if (pDoc && pDoc->HasUnsavedAnnotations())
		{
			if (!PromptSaveUnsavedAnnotations(pDoc))
				return;
		}
	}

	if (!m_pendingLoads.empty() || m_pSearchParams || !m_retiredSearches.empty())
	{
		m_closePending = true;
		for (PDFLoadParams* pParams : m_pendingLoads)
			pParams->cancelled.store(true);
		StopSearch();
		return;
	}
	CDialogEx::OnCancel();
}

void CXiaoGongPDFDlg::FinishPendingClose()
{
	if (m_closePending && m_pendingLoads.empty() &&
		!m_pSearchParams && m_retiredSearches.empty())
		CDialogEx::OnCancel();
}

void CXiaoGongPDFDlg::OnClose()
{
	OnCancel();
}

void CXiaoGongPDFDlg::OnDestroy()
{
#ifdef _DEBUG
	TRACE(_T("OnDestroy() called\n"));
#endif
	if (GetSafeHwnd())
		::RemoveProp(GetSafeHwnd(), XIAOGONG_MAINWND_PROP);
	m_fileWatcher.Stop();
	m_deferredFileChanges.clear();
	KillTimer(TIMER_ID_PAGE_PREFETCH);
	if (m_sessionRestoreStarted)
		SaveOpenTabs();
	FinishInlineTextAnnot(true);
	ClearSelection();
	CancelAreaCaptureMode(false);
	InvalidateLinkCache();
	CleanupSearches();
	CleanupPendingLoad();
	UIRelease();             // 先在 HWND 有效时解绑 UI
	CDialogEx::OnDestroy();
}

void CXiaoGongPDFDlg::PostNcDestroy()
{
#ifdef _DEBUG
	TRACE(_T("PostNcDestroy() called\n"));
#endif
	CDialogEx::PostNcDestroy();
}

// 只能在 HWND 仍有效时调用（OnDestroy 里）
void CXiaoGongPDFDlg::UIRelease()
{
	// 清理 PDF 视图控件
	HWND h = m_pdfView.GetSafeHwnd();
	if (h && ::IsWindow(h))
	{
		m_pdfView.SetDisplayBitmap(NULL);
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
	// 活动文档对象由 CPDFDocument 持有并释放，这里只清空非拥有引用
	m_doc = nullptr;
	m_ctx = nullptr;
}

// ============================================================================
// 鼠标拖拽平移功能实现
// ============================================================================

bool CXiaoGongPDFDlg::IsEditControlFocused() const
{
	HWND hFocus = ::GetFocus();
	if (!hFocus)
		return false;
	if (hFocus == m_editSearch.GetSafeHwnd() || hFocus == m_editCurrent.GetSafeHwnd())
		return true;
	TCHAR className[32] = { 0 };
	if (::GetClassName(hFocus, className, _countof(className)) > 0 &&
		_tcsicmp(className, _T("Edit")) == 0)
		return true;
	return false;
}

bool CXiaoGongPDFDlg::IsHandPanModeActive() const
{
	if (m_tempHandDrag)
		return true;
	if (m_spaceKeyDown && !IsEditControlFocused() && (::GetAsyncKeyState(VK_SPACE) & 0x8000) != 0)
		return true;
	return false;
}

bool CXiaoGongPDFDlg::StartPanDrag(CPoint pdfViewPoint, bool temporaryPan)
{
	if (!m_doc || m_totalPages <= 0)
		return false;

	SetFocus();
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	m_canDrag = (m_totalScrollHeight > viewRect.Height()) ||
		(m_totalScrollWidth > viewRect.Width());
	m_isDragging = true;
	m_tempHandDrag = temporaryPan;
	m_lastMousePos = pdfViewPoint;
	SetCapture();
	::SetCursor(m_hHandCursorGrab);
	return true;
}

void CXiaoGongPDFDlg::OnLButtonDown(UINT nFlags, CPoint point)
{
	m_pendingLinkHit = PdfLinkHit{};
	m_pendingLinkDocument = nullptr;
	CRect clientRect;
	GetClientRect(&clientRect);
	const int dragRight = m_tabCtrl.GetSafeHwnd() && m_tabCtrl.IsWindowVisible() ?
		TITLE_ICON_AREA_WIDTH : clientRect.Width() - 138;
	if (!m_isFullscreen && point.x >= 0 && point.x < dragRight &&
		point.y >= 0 && point.y < TITLE_BAR_HEIGHT)
	{
		StartWindowDrag();
		return;
	}
#ifdef _DEBUG
	TRACE(_T("OnLButtonDown: ENTRY point=(%d,%d) nFlags=0x%x m_doc=%p\n"), point.x, point.y, nFlags, m_doc);
#endif
	// 没有加载PDF时不处理鼠标事件
	if (m_doc == nullptr)
	{
		CDialogEx::OnLButtonDown(nFlags, point);
		return;
	}

	// 检查鼠标是否在PDF视图区域内
	CRect pdfRect;
	m_pdfView.GetWindowRect(&pdfRect);
	ScreenToClient(&pdfRect);

	if (pdfRect.PtInRect(point))
	{
		CPoint pdfViewPoint = point;
		pdfViewPoint.Offset(-pdfRect.left, -pdfRect.top);

		const bool spaceHeld = (::GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
		if (!spaceHeld)
			m_spaceKeyDown = false;
		const bool temporarySpacePan = m_spaceKeyDown && !IsEditControlFocused() && spaceHeld;

		// 框选区域截图模式优先处理（按住空格仍支持临时抓手平移页面）
		if (m_isAreaCaptureMode && !temporarySpacePan && m_totalPages > 0)
		{
			SetFocus();
			if (m_hasCaptureRect && !m_isDraggingCaptureRect)
			{
				const int hitAction = HitTestCaptureActionBar(pdfViewPoint);
				if (hitAction != CAPTURE_ACT_NONE)
				{
					m_pressedCaptureAction = hitAction;
					SetCapture();
					InvalidateSelection();
					return;
				}
			}

			m_pressedCaptureAction = CAPTURE_ACT_NONE;
			m_hoverCaptureAction = CAPTURE_ACT_NONE;
			m_isDraggingCaptureRect = true;
			m_hasCaptureRect = false;
			for (auto& r : m_captureActionRects)
				r.SetRectEmpty();
			m_captureStartView = pdfViewPoint;
			m_captureEndView = pdfViewPoint;
			SetCapture();
			::SetCursor(::LoadCursor(nullptr, IDC_CROSS));
			InvalidateSelection();
			return;
		}

		// 选中文本浮动快捷操作条点击检测
		if (!temporarySpacePan && IsSelectionActionBarVisible())
		{
			const int hitSelAct = HitTestSelectionActionBar(pdfViewPoint);
			if (hitSelAct != SEL_BAR_NONE)
			{
				SetFocus();
				m_pressedSelectionAction = hitSelAct;
				SetCapture();
				InvalidateSelection();
				return;
			}
		}

		// 页面内联文字批注编辑框与顶部悬浮样式栏交互处理
		if (!temporarySpacePan && IsInlineTextAnnotVisible())
		{
			if (m_inlineAnnot.colorPopupOpen)
			{
				const int hitSwatch = HitTestInlineAnnotColorSwatch(pdfViewPoint);
				if (hitSwatch >= 0)
				{
					m_inlineAnnot.pressedColorSwatch = hitSwatch;
					SetCapture();
					InvalidateSelection();
					return;
				}
				if (!m_inlineAnnot.colorPopupViewRect.IsRectEmpty() &&
					m_inlineAnnot.colorPopupViewRect.PtInRect(pdfViewPoint))
				{
					if (m_inlineAnnotEdit.GetSafeHwnd())
						m_inlineAnnotEdit.SetFocus();
					return;
				}
				if (HitTestInlineAnnotToolbar(pdfViewPoint) != INLINE_ANNOT_BTN_COLOR)
				{
					m_inlineAnnot.colorPopupOpen = false;
					InvalidateSelection();
				}
			}

			const int hitBarBtn = HitTestInlineAnnotToolbar(pdfViewPoint);
			if (hitBarBtn != INLINE_ANNOT_BTN_NONE)
			{
				m_inlineAnnot.pressedToolbarBtn = hitBarBtn;
				SetCapture();
				InvalidateSelection();
				return;
			}
			if (!m_inlineAnnot.toolbarViewRect.IsRectEmpty() &&
				m_inlineAnnot.toolbarViewRect.PtInRect(pdfViewPoint))
			{
				if (m_inlineAnnotEdit.GetSafeHwnd())
					m_inlineAnnotEdit.SetFocus();
				return;
			}

			CRect resizeHit = m_inlineAnnot.resizeHandleViewRect;
			if (!resizeHit.IsRectEmpty())
				resizeHit.InflateRect(4, 4);
			if (!resizeHit.IsRectEmpty() && resizeHit.PtInRect(pdfViewPoint))
			{
				m_inlineAnnot.draggingResizeW = true;
				m_inlineAnnot.justCreatedFromMenu = false;
				m_inlineAnnot.dragStartView = pdfViewPoint;
				m_inlineAnnot.dragStartWidthPt = m_inlineAnnot.widthPt;
				SetCapture();
				::SetCursor(::LoadCursor(nullptr, IDC_SIZEWE));
				return;
			}

			if ((!m_inlineAnnot.moveHandleViewRect.IsRectEmpty() &&
				m_inlineAnnot.moveHandleViewRect.PtInRect(pdfViewPoint)) ||
				HitTestInlineAnnotMoveBorder(pdfViewPoint))
			{
				m_inlineAnnot.draggingMove = true;
				m_inlineAnnot.justCreatedFromMenu = false;
				m_inlineAnnot.dragStartView = pdfViewPoint;
				m_inlineAnnot.dragStartLocalXPt = m_inlineAnnot.localXPt;
				m_inlineAnnot.dragStartLocalYPt = m_inlineAnnot.localYPt;
				SetCapture();
				::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
				return;
			}

			if (!m_inlineAnnot.boxViewRect.IsRectEmpty() &&
				m_inlineAnnot.boxViewRect.PtInRect(pdfViewPoint))
			{
				m_inlineAnnot.justCreatedFromMenu = false;
				const int charIdx = HitTestInlineAnnotCharIndex(pdfViewPoint);
				if (m_inlineAnnotEdit.GetSafeHwnd())
				{
					if (nFlags & MK_SHIFT)
					{
						int curStart = 0, curEnd = 0;
						m_inlineAnnotEdit.GetSel(curStart, curEnd);
						m_inlineAnnotEdit.SetSel(curStart, charIdx);
						m_inlineAnnot.selectAnchorChar = curStart;
					}
					else
					{
						m_inlineAnnotEdit.SetSel(charIdx, charIdx);
						m_inlineAnnot.selectAnchorChar = charIdx;
					}
					m_inlineAnnotEdit.SetFocus();
				}
				m_inlineAnnot.selectingText = true;
				m_inlineAnnot.caretVisible = true;
				SetCapture();
				InvalidateSelection();
				return;
			}

			// 若刚通过菜单新建且尚未输入文字，点击页面任意空白位置可直接将该输入框定位到点击处
			CString curAnnotText;
			if (m_inlineAnnotEdit.GetSafeHwnd())
				m_inlineAnnotEdit.GetWindowText(curAnnotText);
			int clickAnnotPage = -1;
			PageAnnotHitInfo clickAnnotHit;
			const bool hitOtherFreeText = HitTestAnnotationAtViewPoint(pdfViewPoint, clickAnnotPage, clickAnnotHit) &&
				clickAnnotHit.type == PDF_ANNOT_FREE_TEXT;

			if (m_inlineAnnot.justCreatedFromMenu && curAnnotText.IsEmpty() && !hitOtherFreeText)
			{
				const int targetPage = GetPageAtViewPoint(pdfViewPoint, false);
				float targetFitzX = 0.0f, targetFitzY = 0.0f;
				if (targetPage >= 0 && ScreenPointToPdfCoords(pdfViewPoint, targetPage, targetFitzX, targetFitzY))
				{
					StartInlineTextAnnotAt(targetPage, targetFitzX, targetFitzY, CString(), false);
					return;
				}
			}

			FinishInlineTextAnnot(false);
			if (hitOtherFreeText)
			{
				StartInlineTextAnnotFromExisting(clickAnnotPage, clickAnnotHit);
			}
			return;
		}

		if (!temporarySpacePan && m_totalPages > 0)
		{
			m_linkPressPoint = pdfViewPoint;
			if (HitTestLink(pdfViewPoint, m_pendingLinkHit))
				m_pendingLinkDocument = m_doc;
		}

		// 点击页面上已有的文字批注时直接进入内联编辑/移动状态
		if (!temporarySpacePan && m_pendingLinkHit.kind == PdfLinkKind::None && m_totalPages > 0)
		{
			int hitAnnotPage = -1;
			PageAnnotHitInfo hitAnnot;
			if (HitTestAnnotationAtViewPoint(pdfViewPoint, hitAnnotPage, hitAnnot) &&
				hitAnnot.type == PDF_ANNOT_FREE_TEXT)
			{
				ClearSelection();
				StartInlineTextAnnotFromExisting(hitAnnotPage, hitAnnot);
				return;
			}
		}

		// 未按住空格临时抓手时：开始文本选择
		if (!temporarySpacePan && m_doc != nullptr && m_totalPages > 0)
		{
			ClearSelection();
			m_isSelecting = true;
			SetFocus();
			m_selStartView = pdfViewPoint;
			m_selEndView = pdfViewPoint;

			m_selPageNumber = GetPageAtViewPoint(pdfViewPoint, true);

			if (m_selPageNumber < 0 || m_selPageNumber >= m_totalPages)
				m_selPageNumber = m_currentPage;

			if (m_ctx)
			{
				fz_page* page = nullptr;
				fz_var(page);
				CSingleLock lock(&m_renderLock, TRUE);
				fz_try(m_ctx)
				{
					page = fz_load_page(m_ctx, m_doc, m_selPageNumber);
					if (page)
						m_selectionTextPage = fz_new_stext_page_from_page(m_ctx, page, nullptr);
				}
				fz_always(m_ctx)
				{
					if (page)
						fz_drop_page(m_ctx, page);
				}
				fz_catch(m_ctx)
				{
					m_selectionTextPage = nullptr;
				}
			}

			SetCapture();
			return;
		}

		// 按住空格键触发临时抓手拖动
		if (StartPanDrag(pdfViewPoint, temporarySpacePan))
			return;
	}

	CDialogEx::OnLButtonDown(nFlags, point);
}

void CXiaoGongPDFDlg::StartWindowDrag()
{
	::GetCursorPos(&m_windowDragStartScreen);
	GetWindowRect(&m_windowDragStartRect);
	m_isWindowDragging = true;
	SetCapture();
}

void CXiaoGongPDFDlg::OnLButtonUp(UINT nFlags, CPoint point)
{
	const PdfLinkHit linkHit = m_pendingLinkHit;
	const fz_document* linkDocument = m_pendingLinkDocument;
	m_pendingLinkHit = PdfLinkHit{};
	m_pendingLinkDocument = nullptr;
	if (m_isWindowDragging)
	{
		m_isWindowDragging = false;
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();
		return;
	}
	// 没有加载PDF时先复位可能残留的捕获/拖拽状态
	if (m_doc == nullptr)
	{
		m_isDragging = false;
		m_tempHandDrag = false;
		if (m_isSelecting)
			ClearSelection();
		CancelAreaCaptureMode(false);
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();
		CDialogEx::OnLButtonUp(nFlags, point);
		return;
	}

#ifdef _DEBUG
	TRACE(_T("OnLButtonUp: 被调用! point=(%d,%d), isDragging=%d, isSelecting=%d\n"),
		point.x, point.y, m_isDragging, m_isSelecting);
#endif

	CRect linkViewRect;
	m_pdfView.GetWindowRect(&linkViewRect);
	ScreenToClient(&linkViewRect);
	CPoint linkReleasePoint = point;
	linkReleasePoint.Offset(-linkViewRect.left, -linkViewRect.top);

	// 处理页面内联文字批注浮动栏点击、调色板选择、手柄拖拽与框内选字释放
	if (IsInlineTextAnnotVisible())
	{
		if (m_inlineAnnot.pressedColorSwatch >= 0)
		{
			const int pressedIdx = m_inlineAnnot.pressedColorSwatch;
			m_inlineAnnot.pressedColorSwatch = -1;
			if (::GetCapture() == GetSafeHwnd())
				::ReleaseCapture();
			const int releaseIdx = HitTestInlineAnnotColorSwatch(linkReleasePoint);
			if (releaseIdx == pressedIdx)
			{
				static const COLORREF kAnnotPalette[INLINE_ANNOT_SWATCH_COUNT] = {
					RGB(0, 114, 206),
					RGB(216, 43, 43),
					RGB(32, 33, 36),
					RGB(22, 148, 72),
					RGB(230, 106, 18),
					RGB(124, 58, 202),
					RGB(198, 32, 104),
					RGB(95, 107, 122)
				};
				m_inlineAnnot.color = kAnnotPalette[releaseIdx];
				m_defaultAnnotTextColor = m_inlineAnnot.color;
				m_inlineAnnot.colorPopupOpen = false;
				SetStatusBarMessage(_T("文字批注：已更新字体颜色"));
			}
			if (m_inlineAnnotEdit.GetSafeHwnd())
				m_inlineAnnotEdit.SetFocus();
			InvalidateSelection();
			return;
		}

		if (m_inlineAnnot.pressedToolbarBtn != INLINE_ANNOT_BTN_NONE)
		{
			const int pressedBtn = m_inlineAnnot.pressedToolbarBtn;
			m_inlineAnnot.pressedToolbarBtn = INLINE_ANNOT_BTN_NONE;
			if (::GetCapture() == GetSafeHwnd())
				::ReleaseCapture();
			const int releaseBtn = HitTestInlineAnnotToolbar(linkReleasePoint);
			if (releaseBtn == pressedBtn)
			{
				if (releaseBtn == INLINE_ANNOT_BTN_COLOR)
				{
					m_inlineAnnot.colorPopupOpen = !m_inlineAnnot.colorPopupOpen;
				}
				else if (releaseBtn == INLINE_ANNOT_BTN_FONT_INC)
				{
					m_inlineAnnot.colorPopupOpen = false;
					m_inlineAnnot.fontSizePt = min(48.0f, m_inlineAnnot.fontSizePt + 2.0f);
					m_defaultAnnotFontSizePt = m_inlineAnnot.fontSizePt;
					CString tip;
					tip.Format(_T("文字批注：字号 %.0f pt"), m_inlineAnnot.fontSizePt);
					SetStatusBarMessage(tip);
				}
				else if (releaseBtn == INLINE_ANNOT_BTN_FONT_DEC)
				{
					m_inlineAnnot.colorPopupOpen = false;
					m_inlineAnnot.fontSizePt = max(8.0f, m_inlineAnnot.fontSizePt - 2.0f);
					m_defaultAnnotFontSizePt = m_inlineAnnot.fontSizePt;
					CString tip;
					tip.Format(_T("文字批注：字号 %.0f pt"), m_inlineAnnot.fontSizePt);
					SetStatusBarMessage(tip);
				}
				else if (releaseBtn == INLINE_ANNOT_BTN_CHAR_SPACE_INC)
				{
					m_inlineAnnot.colorPopupOpen = false;
					m_inlineAnnot.charSpacingPt = min(24.0f, m_inlineAnnot.charSpacingPt + 1.2f);
					m_defaultAnnotCharSpacingPt = m_inlineAnnot.charSpacingPt;
					CString tip;
					tip.Format(_T("文字批注：文字间距 +%.1f pt"), m_inlineAnnot.charSpacingPt);
					SetStatusBarMessage(tip);
				}
				else if (releaseBtn == INLINE_ANNOT_BTN_CHAR_SPACE_DEC)
				{
					m_inlineAnnot.colorPopupOpen = false;
					m_inlineAnnot.charSpacingPt = max(0.0f, m_inlineAnnot.charSpacingPt - 1.2f);
					m_defaultAnnotCharSpacingPt = m_inlineAnnot.charSpacingPt;
					CString tip;
					tip.Format(_T("文字批注：文字间距 +%.1f pt"), m_inlineAnnot.charSpacingPt);
					SetStatusBarMessage(tip);
				}
				else if (releaseBtn == INLINE_ANNOT_BTN_DELETE)
				{
					DeleteActiveInlineTextAnnot();
					return;
				}
				else if (releaseBtn == INLINE_ANNOT_BTN_EXIT)
				{
					FinishInlineTextAnnot(false);
					return;
				}
			}
			if (m_inlineAnnot.active && m_inlineAnnotEdit.GetSafeHwnd())
				m_inlineAnnotEdit.SetFocus();
			InvalidateSelection();
			return;
		}

		if (m_inlineAnnot.draggingMove || m_inlineAnnot.draggingResizeW || m_inlineAnnot.selectingText)
		{
			m_inlineAnnot.draggingMove = false;
			m_inlineAnnot.draggingResizeW = false;
			m_inlineAnnot.selectingText = false;
			if (::GetCapture() == GetSafeHwnd())
				::ReleaseCapture();
			if (m_inlineAnnotEdit.GetSafeHwnd())
				m_inlineAnnotEdit.SetFocus();
			InvalidateSelection();
			if (m_pdfView.GetSafeHwnd())
			{
				SendMessage(WM_SETCURSOR,
					reinterpret_cast<WPARAM>(m_pdfView.GetSafeHwnd()),
					MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
			}
			return;
		}
	}

	// 处理选中文本快捷操作条按钮释放
	if (m_pressedSelectionAction != SEL_BAR_NONE)
	{
		const int pressedSel = m_pressedSelectionAction;
		m_pressedSelectionAction = SEL_BAR_NONE;
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();
		const int releaseSel = HitTestSelectionActionBar(linkReleasePoint);
		InvalidateSelection();
		if (releaseSel == pressedSel)
		{
			if (releaseSel == SEL_BAR_COPY)
			{
				CopySelectionToClipboard();
				SetStatusBarMessage(_T("已复制选中文本"));
			}
			else if (releaseSel == SEL_BAR_HIGHLIGHT)
			{
				OnMenuAnnotHighlight();
			}
			else if (releaseSel == SEL_BAR_UNDERLINE)
			{
				OnMenuAnnotUnderline();
			}
			else if (releaseSel == SEL_BAR_NOTE)
			{
				OnMenuAnnotTextNote();
			}
		}
		return;
	}

	// 处理区域截图浮动工具栏点击或框选拖拽完成
	if (m_isAreaCaptureMode)
	{
		if (m_pressedCaptureAction != CAPTURE_ACT_NONE)
		{
			const int pressedAct = m_pressedCaptureAction;
			m_pressedCaptureAction = CAPTURE_ACT_NONE;
			if (::GetCapture() == GetSafeHwnd())
				::ReleaseCapture();
			const int releaseAct = HitTestCaptureActionBar(linkReleasePoint);
			InvalidateSelection();
			if (releaseAct == pressedAct)
			{
				if (releaseAct == CAPTURE_ACT_COPY)
					CopyCapturedRegionToClipboard();
				else if (releaseAct == CAPTURE_ACT_SAVE)
					SaveCapturedRegionAsPng();
				else if (releaseAct == CAPTURE_ACT_CANCEL)
					CancelAreaCaptureMode(true);
			}
			return;
		}

		if (m_isDraggingCaptureRect)
		{
			CRect viewClient;
			m_pdfView.GetClientRect(&viewClient);
			m_captureEndView.x = max(0L, min(viewClient.right, linkReleasePoint.x));
			m_captureEndView.y = max(0L, min(viewClient.bottom, linkReleasePoint.y));
			m_isDraggingCaptureRect = false;
			if (::GetCapture() == GetSafeHwnd())
				::ReleaseCapture();

			const CRect selRect = GetNormalizedCaptureViewRect();
			CSize previewSize(0, 0);
			int previewDpi = 300;
			if (selRect.Width() >= 6 && selRect.Height() >= 6 &&
				ComputeCapturePixelSizePreview(selRect, previewSize, previewDpi))
			{
				m_hasCaptureRect = true;
				CString tip;
				tip.Format(_T("已框选区域 (%d×%d px, %d DPI)：点击“复制截图”/按 Enter 复制，或点击“保存 PNG”"),
					previewSize.cx, previewSize.cy, previewDpi);
				SetStatusBarMessage(tip);
			}
			else
			{
				m_hasCaptureRect = false;
				SetStatusBarMessage(_T("区域截图模式：请在 PDF 页面有效内容区域内拖动鼠标框选，按 Esc 取消"));
			}
			InvalidateSelection();
			return;
		}
	}

	if (linkHit.kind != PdfLinkKind::None && linkDocument == m_doc && linkViewRect.PtInRect(point) &&
		abs(linkReleasePoint.x - m_linkPressPoint.x) <= 3 &&
		abs(linkReleasePoint.y - m_linkPressPoint.y) <= 3)
	{
		if (m_isSelecting)
			ClearSelection();
		if (m_isDragging)
		{
			m_isDragging = false;
			m_tempHandDrag = false;
			SaveCurrentPageZoomState();
		}
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();

		if (linkHit.kind == PdfLinkKind::Internal && linkHit.targetPage >= 0)
		{
			::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
			GoToPage(linkHit.targetPage);
			if (linkHit.targetY >= 0.0f || linkHit.targetX >= 0.0f)
			{
				fz_rect bounds;
				float scale;
				int offsetX, offsetY;
				if (GetSelectionPageTransform(linkHit.targetPage, bounds, scale, offsetX, offsetY))
				{
					const int rotation = GetPageRotation(linkHit.targetPage);
					float localY = -1.0f;
					if (linkHit.targetY >= 0.0f && rotation == 0)
						localY = linkHit.targetY - bounds.y0;
					else if (linkHit.targetY >= 0.0f && rotation == 180)
						localY = bounds.y1 - linkHit.targetY;
					else if (linkHit.targetX >= 0.0f && rotation == 90)
						localY = linkHit.targetX - bounds.x0;
					else if (linkHit.targetX >= 0.0f && rotation == 270)
						localY = bounds.x1 - linkHit.targetX;
					if (localY >= 0.0f)
					{
						ScrollToVerticalPosition(m_scrollPosition + offsetY + (int)(localY * scale) - 24);
					}
				}
			}
			return;
		}
		else if (linkHit.kind == PdfLinkKind::External)
		{
			::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
			OpenExternalLinkWithConfirm(linkHit.externalUri);
			return;
		}
	}

	// ★★★ 文本选择结束
	if (m_isSelecting)
	{
		CRect pdfRect;
		m_pdfView.GetWindowRect(&pdfRect);
		ScreenToClient(&pdfRect);
		m_selEndView = point;
		m_selEndView.Offset(-pdfRect.left, -pdfRect.top);
		m_isSelecting = false;
		UpdateSelectedText();
		if (m_selectionTextPage && m_ctx)
		{
			CSingleLock lock(&m_renderLock, TRUE);
			fz_drop_stext_page(m_ctx, m_selectionTextPage);
		}
		m_selectionTextPage = nullptr;
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();

		// 检查选择区域是否有效（至少有一定大小）
		int dx = abs(m_selEndView.x - m_selStartView.x);
		int dy = abs(m_selEndView.y - m_selStartView.y);
		if ((dx > 3 || dy > 3) && !m_selectionQuads.empty() && !m_selectedText.IsEmpty())
		{
			m_hasSelection = true;
#ifdef _DEBUG
			TRACE(_T("OnLButtonUp: 文本选择完成, start=(%d,%d), end=(%d,%d)\n"),
				m_selStartView.x, m_selStartView.y, m_selEndView.x, m_selEndView.y);
#endif
		}
		else
		{
#ifdef _DEBUG
			TRACE(_T("OnLButtonUp: 选择区域太小，清除选择 dx=%d dy=%d\n"), dx, dy);
#endif
			ClearSelection();
		}
		InvalidateSelection();
		if (m_pdfView.GetSafeHwnd())
		{
			SendMessage(WM_SETCURSOR,
				reinterpret_cast<WPARAM>(m_pdfView.GetSafeHwnd()),
				MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
		}
		return;
	}

	if (m_isDragging && !(nFlags & MK_MBUTTON))
	{
#ifdef _DEBUG
		TRACE(_T("OnLButtonUp: 结束PDF拖拽，恢复光标\n"));
#endif
		m_isDragging = false;
		m_tempHandDrag = false;
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();
		if (m_pdfView.GetSafeHwnd())
		{
			SendMessage(WM_SETCURSOR,
				reinterpret_cast<WPARAM>(m_pdfView.GetSafeHwnd()),
				MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
		}
		SaveCurrentPageZoomState();
	}

	CDialogEx::OnLButtonUp(nFlags, point);
}

void CXiaoGongPDFDlg::OnMButtonDown(UINT nFlags, CPoint point)
{
	if (m_doc == nullptr || m_totalPages <= 0)
	{
		CDialogEx::OnMButtonDown(nFlags, point);
		return;
	}

	CRect pdfRect;
	m_pdfView.GetWindowRect(&pdfRect);
	ScreenToClient(&pdfRect);
	if (pdfRect.PtInRect(point))
	{
		CPoint pdfViewPoint = point;
		pdfViewPoint.Offset(-pdfRect.left, -pdfRect.top);
		if (StartPanDrag(pdfViewPoint, true))
			return;
	}

	CDialogEx::OnMButtonDown(nFlags, point);
}

void CXiaoGongPDFDlg::OnMButtonUp(UINT nFlags, CPoint point)
{
	if (m_doc == nullptr)
	{
		m_isDragging = false;
		m_tempHandDrag = false;
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();
		CDialogEx::OnMButtonUp(nFlags, point);
		return;
	}

	if (m_isDragging && !(nFlags & MK_LBUTTON))
	{
		m_isDragging = false;
		m_tempHandDrag = false;
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();
		if (m_pdfView.GetSafeHwnd())
		{
			SendMessage(WM_SETCURSOR,
				reinterpret_cast<WPARAM>(m_pdfView.GetSafeHwnd()),
				MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
		}
		SaveCurrentPageZoomState();
		return;
	}

	CDialogEx::OnMButtonUp(nFlags, point);
}

void CXiaoGongPDFDlg::OnCaptureChanged(CWnd* pWnd)
{
	if (pWnd != this)
	{
		m_isWindowDragging = false;
		m_isDragging = false;
		m_tempHandDrag = false;
		m_pendingLinkHit = PdfLinkHit{};
		m_pendingLinkDocument = nullptr;
		m_pressedCaptureAction = CAPTURE_ACT_NONE;
		m_pressedSelectionAction = SEL_BAR_NONE;
		m_inlineAnnot.pressedToolbarBtn = INLINE_ANNOT_BTN_NONE;
		m_inlineAnnot.pressedColorSwatch = -1;
		m_inlineAnnot.draggingMove = false;
		m_inlineAnnot.draggingResizeW = false;
		m_inlineAnnot.selectingText = false;
		if (m_isDraggingCaptureRect)
		{
			m_isDraggingCaptureRect = false;
			InvalidateSelection();
		}
		if (m_isSelecting)
		{
			ClearSelection();
			InvalidateSelection();
		}
	}
	CDialogEx::OnCaptureChanged(pWnd);
}

void CXiaoGongPDFDlg::OnMouseMove(UINT nFlags, CPoint point)
{
	if (m_isWindowDragging)
	{
		if (!(nFlags & MK_LBUTTON))
		{
			m_isWindowDragging = false;
			if (::GetCapture() == GetSafeHwnd())
				::ReleaseCapture();
		}
		else
		{
			CPoint cursor;
			::GetCursorPos(&cursor);
			if (IsZoomed())
			{
				const int dx = cursor.x - m_windowDragStartScreen.x;
				const int dy = cursor.y - m_windowDragStartScreen.y;
				if (dx * dx + dy * dy < 25)
					return;
				const int startWidth = m_windowDragStartRect.Width();
				const double xRatio = startWidth > 0 ?
					static_cast<double>(m_windowDragStartScreen.x - m_windowDragStartRect.left) / startWidth :
					0.5;
				ShowWindow(SW_RESTORE);
				GetWindowRect(&m_windowDragStartRect);
				m_windowDragStartRect.OffsetRect(
					cursor.x - m_windowDragStartRect.left -
					static_cast<int>(xRatio * m_windowDragStartRect.Width()),
					cursor.y - m_windowDragStartRect.top - TITLE_BAR_HEIGHT / 2);
				m_windowDragStartScreen = cursor;
			}
			SetWindowPos(nullptr,
				m_windowDragStartRect.left + cursor.x - m_windowDragStartScreen.x,
				m_windowDragStartRect.top + cursor.y - m_windowDragStartScreen.y,
				0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			return;
		}
	}

	// ★★★ 页面内联文字批注拖拽移动、右侧圆柄调宽、框内拖选与浮动栏悬停
	if (IsInlineTextAnnotVisible() && m_pdfView.GetSafeHwnd())
	{
		CRect pdfRect;
		m_pdfView.GetWindowRect(&pdfRect);
		ScreenToClient(&pdfRect);
		CPoint pdfViewPoint = point;
		pdfViewPoint.Offset(-pdfRect.left, -pdfRect.top);

		if (m_inlineAnnot.draggingMove && (nFlags & MK_LBUTTON))
		{
			fz_rect bounds = {};
			float scale = 0.0f;
			int offsetX = 0, offsetY = 0;
			if (GetSelectionPageTransform(m_inlineAnnot.pageNumber, bounds, scale, offsetX, offsetY) &&
				scale > 0.01f)
			{
				const int rot = GetPageRotation(m_inlineAnnot.pageNumber);
				const float origW = bounds.x1 - bounds.x0;
				const float origH = bounds.y1 - bounds.y0;
				const float effW = (rot == 90 || rot == 270) ? origH : origW;
				const float effH = (rot == 90 || rot == 270) ? origW : origH;
				const float dxPt = static_cast<float>(pdfViewPoint.x - m_inlineAnnot.dragStartView.x) / scale;
				const float dyPt = static_cast<float>(pdfViewPoint.y - m_inlineAnnot.dragStartView.y) / scale;
				const float estH = max(m_inlineAnnot.fontSizePt * 1.5f,
					m_inlineAnnot.boxViewRect.Height() / scale);
				m_inlineAnnot.localXPt = max(2.0f, min(max(2.0f, effW - m_inlineAnnot.widthPt - 2.0f),
					m_inlineAnnot.dragStartLocalXPt + dxPt));
				m_inlineAnnot.localYPt = max(2.0f, min(max(2.0f, effH - estH - 2.0f),
					m_inlineAnnot.dragStartLocalYPt + dyPt));
				InvalidateSelection();
			}
			return;
		}

		if (m_inlineAnnot.draggingResizeW && (nFlags & MK_LBUTTON))
		{
			fz_rect bounds = {};
			float scale = 0.0f;
			int offsetX = 0, offsetY = 0;
			if (GetSelectionPageTransform(m_inlineAnnot.pageNumber, bounds, scale, offsetX, offsetY) &&
				scale > 0.01f)
			{
				const int rot = GetPageRotation(m_inlineAnnot.pageNumber);
				const float origW = bounds.x1 - bounds.x0;
				const float origH = bounds.y1 - bounds.y0;
				const float effW = (rot == 90 || rot == 270) ? origH : origW;
				const float dxPt = static_cast<float>(pdfViewPoint.x - m_inlineAnnot.dragStartView.x) / scale;
				const float minW = max(44.0f, m_inlineAnnot.fontSizePt * 2.5f + 10.0f);
				const float maxW = max(minW, effW - m_inlineAnnot.localXPt - 4.0f);
				m_inlineAnnot.widthPt = max(minW, min(maxW, m_inlineAnnot.dragStartWidthPt + dxPt));
				m_inlineAnnot.userResizedWidth = true;
				InvalidateSelection();
			}
			return;
		}

		if (m_inlineAnnot.selectingText && (nFlags & MK_LBUTTON))
		{
			const int charIdx = HitTestInlineAnnotCharIndex(pdfViewPoint);
			if (m_inlineAnnotEdit.GetSafeHwnd())
			{
				m_inlineAnnotEdit.SetSel(m_inlineAnnot.selectAnchorChar, charIdx);
				m_inlineAnnot.caretVisible = true;
			}
			InvalidateSelection();
			return;
		}

		if (!m_isDragging)
		{
			const int hoverBtn = HitTestInlineAnnotToolbar(pdfViewPoint);
			const int hoverSwatch = m_inlineAnnot.colorPopupOpen ?
				HitTestInlineAnnotColorSwatch(pdfViewPoint) : -1;
			CRect resizeHit = m_inlineAnnot.resizeHandleViewRect;
			if (!resizeHit.IsRectEmpty())
				resizeHit.InflateRect(4, 4);
			const bool hoverResize = !resizeHit.IsRectEmpty() && resizeHit.PtInRect(pdfViewPoint);
			const bool hoverMove = !hoverResize &&
				!m_inlineAnnot.moveHandleViewRect.IsRectEmpty() &&
				m_inlineAnnot.moveHandleViewRect.PtInRect(pdfViewPoint);
			const bool hoverBorder = !hoverResize && !hoverMove &&
				HitTestInlineAnnotMoveBorder(pdfViewPoint);

			if (hoverBtn != m_inlineAnnot.hoverToolbarBtn ||
				hoverSwatch != m_inlineAnnot.hoverColorSwatch ||
				hoverResize != m_inlineAnnot.hoverResizeHandle ||
				hoverMove != m_inlineAnnot.hoverMoveHandle ||
				hoverBorder != m_inlineAnnot.hoverBorderMove)
			{
				m_inlineAnnot.hoverToolbarBtn = hoverBtn;
				m_inlineAnnot.hoverColorSwatch = hoverSwatch;
				m_inlineAnnot.hoverResizeHandle = hoverResize;
				m_inlineAnnot.hoverMoveHandle = hoverMove;
				m_inlineAnnot.hoverBorderMove = hoverBorder;
				InvalidateSelection();
			}
		}
	}

	// ★★★ 框选区域高清截图模式：更新框选矩形或悬浮操作按钮高亮
	if (m_isAreaCaptureMode && m_pdfView.GetSafeHwnd())
	{
		CRect pdfRect;
		m_pdfView.GetWindowRect(&pdfRect);
		ScreenToClient(&pdfRect);
		CPoint pdfViewPoint = point;
		pdfViewPoint.Offset(-pdfRect.left, -pdfRect.top);

		if (m_isDraggingCaptureRect && (nFlags & MK_LBUTTON))
		{
			CRect viewClient;
			m_pdfView.GetClientRect(&viewClient);
			m_captureEndView.x = max(0L, min(viewClient.right, pdfViewPoint.x));
			m_captureEndView.y = max(0L, min(viewClient.bottom, pdfViewPoint.y));
			InvalidateSelection();
			return;
		}

		if (m_hasCaptureRect && !m_isDraggingCaptureRect)
		{
			const int hoverAct = HitTestCaptureActionBar(pdfViewPoint);
			if (hoverAct != m_hoverCaptureAction)
			{
				m_hoverCaptureAction = hoverAct;
				InvalidateSelection();
			}
		}
	}

	// ★★★ 文本选择模式：更新选择区域
	if (m_isSelecting && (nFlags & MK_LBUTTON))
	{
		CRect pdfRect;
		m_pdfView.GetWindowRect(&pdfRect);
		ScreenToClient(&pdfRect);

		CPoint pdfViewPoint = point;
		pdfViewPoint.Offset(-pdfRect.left, -pdfRect.top);

#ifdef _DEBUG
		TRACE(_T("OnMouseMove: 选择中 endView=(%d,%d)\n"), pdfViewPoint.x, pdfViewPoint.y);
#endif

		m_selEndView = pdfViewPoint;
		UpdateSelectedText();
		InvalidateSelection();
		return;
	}

	if (m_isDragging)
	{
		if (!((nFlags & MK_LBUTTON) || (nFlags & MK_MBUTTON)))
		{
			m_isDragging = false;
			m_tempHandDrag = false;
			if (::GetCapture() == GetSafeHwnd())
				::ReleaseCapture();
			SaveCurrentPageZoomState();
		}
		else if (m_pdfView.GetSafeHwnd())
		{
			CRect pdfRect;
			m_pdfView.GetWindowRect(&pdfRect);
			ScreenToClient(&pdfRect);

			CPoint pdfViewPoint = point;
			pdfViewPoint.Offset(-pdfRect.left, -pdfRect.top);

			CPoint delta = pdfViewPoint - m_lastMousePos;
			m_lastMousePos = pdfViewPoint;
			m_panOffset = CPoint(0, 0);

			CRect viewRect;
			m_pdfView.GetClientRect(&viewRect);
			const int maxScrollY = max(0, m_totalScrollHeight - viewRect.Height());
			const int maxScrollX = max(0, m_totalScrollWidth - viewRect.Width());

			const int oldScrollY = m_scrollPosition;
			const int oldScrollX = m_scrollPositionH;
			m_scrollPosition = max(0, min(maxScrollY, m_scrollPosition - delta.y));
			m_scrollPositionH = max(0, min(maxScrollX, m_scrollPositionH - delta.x));

			UpdateScrollBar();

			if (m_scrollPosition != oldScrollY || m_scrollPositionH != oldScrollX)
			{
				ClearActiveCaptureRect();
				RenderVisiblePages();
			}
			return;
		}
	}

	// 普通悬停移动：更新选中文本浮动条高亮，并检测链接/批注在状态栏实时预览
	if (m_doc && m_totalPages > 0 && !m_isAreaCaptureMode &&
		!m_isSelecting && !m_isDragging && m_pdfView.GetSafeHwnd())
	{
		CRect pdfRect;
		m_pdfView.GetWindowRect(&pdfRect);
		ScreenToClient(&pdfRect);
		if (pdfRect.PtInRect(point))
		{
			CPoint pdfViewPoint = point;
			pdfViewPoint.Offset(-pdfRect.left, -pdfRect.top);
			if (IsSelectionActionBarVisible())
			{
				const int hoverSel = HitTestSelectionActionBar(pdfViewPoint);
				if (hoverSel != m_hoverSelectionAction)
				{
					m_hoverSelectionAction = hoverSel;
					InvalidateSelection();
				}
			}
			UpdateHoverLinkFeedback(pdfViewPoint);
		}
		else
		{
			if (m_hoverSelectionAction != SEL_BAR_NONE)
			{
				m_hoverSelectionAction = SEL_BAR_NONE;
				InvalidateSelection();
			}
			ClearHoverLinkFeedback();
		}
	}

	CDialogEx::OnMouseMove(nFlags, point);
}

void CXiaoGongPDFDlg::OnContextMenu(CWnd* pWnd, CPoint point)
{
	if (!pWnd || pWnd->GetSafeHwnd() != m_pdfView.GetSafeHwnd())
	{
		CDialogEx::OnContextMenu(pWnd, point);
		return;
	}

	// 区域截图模式下右键即取消当前操作（符合主流交互直觉）
	if (m_isAreaCaptureMode)
	{
		CancelAreaCaptureMode(true);
		return;
	}

	if (!m_doc || m_totalPages <= 0)
		return;

	CPoint menuScreenPt = point;
	if (menuScreenPt.x == -1 && menuScreenPt.y == -1)
	{
		::GetCursorPos(&menuScreenPt);
	}

	CPoint viewPt = menuScreenPt;
	m_pdfView.ScreenToClient(&viewPt);

	// 在内联文字批注输入框内右键时弹出标准文本编辑菜单
	if (IsInlineTextAnnotVisible() &&
		!m_inlineAnnot.boxViewRect.IsRectEmpty() &&
		m_inlineAnnot.boxViewRect.PtInRect(viewPt) &&
		m_inlineAnnotEdit.GetSafeHwnd())
	{
		enum InlineEditMenuCmd
		{
			CMD_INLINE_CUT = 1,
			CMD_INLINE_COPY,
			CMD_INLINE_PASTE,
			CMD_INLINE_SELECT_ALL,
			CMD_INLINE_DELETE_ANNOT,
			CMD_INLINE_FINISH
		};
		int selS = 0, selE = 0;
		m_inlineAnnotEdit.GetSel(selS, selE);
		const UINT hasSelFlag = (selS != selE) ? 0 : MF_GRAYED;
		const UINT canPasteFlag = ::IsClipboardFormatAvailable(CF_UNICODETEXT) ? 0 : MF_GRAYED;
		CMenu editPopup;
		if (editPopup.CreatePopupMenu())
		{
			editPopup.AppendMenu(MF_STRING | hasSelFlag, CMD_INLINE_CUT, _T("剪切\tCtrl+X"));
			editPopup.AppendMenu(MF_STRING | hasSelFlag, CMD_INLINE_COPY, _T("复制\tCtrl+C"));
			editPopup.AppendMenu(MF_STRING | canPasteFlag, CMD_INLINE_PASTE, _T("粘贴\tCtrl+V"));
			editPopup.AppendMenu(MF_STRING, CMD_INLINE_SELECT_ALL, _T("全选\tCtrl+A"));
			editPopup.AppendMenu(MF_SEPARATOR);
			editPopup.AppendMenu(MF_STRING, CMD_INLINE_DELETE_ANNOT, _T("删除该文字批注"));
			editPopup.AppendMenu(MF_STRING, CMD_INLINE_FINISH, _T("完成并退出编辑\tEsc"));
			const UINT editCmd = static_cast<UINT>(editPopup.TrackPopupMenu(
				TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
				menuScreenPt.x, menuScreenPt.y, this));
			if (editCmd == CMD_INLINE_CUT)
				m_inlineAnnotEdit.SendMessage(WM_CUT);
			else if (editCmd == CMD_INLINE_COPY)
				m_inlineAnnotEdit.SendMessage(WM_COPY);
			else if (editCmd == CMD_INLINE_PASTE)
				m_inlineAnnotEdit.SendMessage(WM_PASTE);
			else if (editCmd == CMD_INLINE_SELECT_ALL)
			{
				m_inlineAnnotEdit.SetSel(0, -1);
				InvalidateSelection();
			}
			else if (editCmd == CMD_INLINE_DELETE_ANNOT)
			{
				DeleteActiveInlineTextAnnot();
				return;
			}
			else if (editCmd == CMD_INLINE_FINISH)
			{
				FinishInlineTextAnnot(false);
				return;
			}
			if (m_inlineAnnot.active && m_inlineAnnotEdit.GetSafeHwnd())
				m_inlineAnnotEdit.SetFocus();
		}
		return;
	}

	PdfLinkHit linkHit;
	HitTestLink(viewPt, linkHit);

	const int ctxPage = GetPageAtViewPoint(viewPt, true);
	float ctxFitzX = 0.0f;
	float ctxFitzY = 0.0f;
	const bool hasCtxPdfPt = (ctxPage >= 0) &&
		ScreenPointToPdfCoords(viewPt, ctxPage, ctxFitzX, ctxFitzY);

	int hitAnnotPage = -1;
	PageAnnotHitInfo hitAnnot;
	const bool hasHitAnnot = HitTestAnnotationAtViewPoint(viewPt, hitAnnotPage, hitAnnot);

	enum PageContextCmd
	{
		CMD_CTX_COPY_TEXT = 1,
		CMD_CTX_ANNOT_HIGHLIGHT,
		CMD_CTX_ANNOT_UNDERLINE,
		CMD_CTX_ANNOT_STRIKEOUT,
		CMD_CTX_ANNOT_SEL_NOTE,
		CMD_CTX_EDIT_HIT_ANNOT,
		CMD_CTX_DELETE_HIT_ANNOT,
		CMD_CTX_OPEN_LINK,
		CMD_CTX_COPY_LINK,
		CMD_CTX_ADD_NOTE_HERE,
		CMD_CTX_AREA_CAPTURE,
		CMD_CTX_COPY_PAGE_IMG
	};

	CMenu popup;
	if (!popup.CreatePopupMenu())
		return;

	bool hasTopGroup = false;
	if (m_hasSelection && !m_selectionQuads.empty())
	{
		if (!m_selectedText.IsEmpty())
			popup.AppendMenu(MF_STRING, CMD_CTX_COPY_TEXT, _T("复制选中文本\tCtrl+C"));
		popup.AppendMenu(MF_STRING, CMD_CTX_ANNOT_HIGHLIGHT, _T("高亮选中文本\tCtrl+H"));
		popup.AppendMenu(MF_STRING, CMD_CTX_ANNOT_UNDERLINE, _T("下划线选中文本\tCtrl+U"));
		popup.AppendMenu(MF_STRING, CMD_CTX_ANNOT_STRIKEOUT, _T("删除线选中文本"));
		popup.AppendMenu(MF_STRING, CMD_CTX_ANNOT_SEL_NOTE, _T("为选中内容添加批注..."));
		hasTopGroup = true;
	}
	if (hasHitAnnot)
	{
		if (hasTopGroup)
			popup.AppendMenu(MF_SEPARATOR);
		if (hitAnnot.type != PDF_ANNOT_STAMP)
		{
			CString editLabel;
			editLabel.Format(_T("编辑该%s内容..."), hitAnnot.typeLabel.GetString());
			popup.AppendMenu(MF_STRING, CMD_CTX_EDIT_HIT_ANNOT, editLabel);
		}
		CString delLabel;
		delLabel.Format(_T("删除该%s"), hitAnnot.typeLabel.GetString());
		popup.AppendMenu(MF_STRING, CMD_CTX_DELETE_HIT_ANNOT, delLabel);
		hasTopGroup = true;
	}
	if (linkHit.kind == PdfLinkKind::External && !linkHit.externalUri.IsEmpty())
	{
		if (hasTopGroup)
			popup.AppendMenu(MF_SEPARATOR);
		popup.AppendMenu(MF_STRING, CMD_CTX_OPEN_LINK, _T("打开外部链接..."));
		popup.AppendMenu(MF_STRING, CMD_CTX_COPY_LINK, _T("复制链接地址"));
		hasTopGroup = true;
	}
	if (hasTopGroup)
		popup.AppendMenu(MF_SEPARATOR);

	popup.AppendMenu(MF_STRING | (hasCtxPdfPt ? 0 : MF_GRAYED),
		CMD_CTX_ADD_NOTE_HERE, _T("在此处添加文字批注..."));
	popup.AppendMenu(MF_SEPARATOR);

	popup.AppendMenu(MF_STRING, CMD_CTX_AREA_CAPTURE, _T("区域截图\tCtrl+Shift+S"));
	popup.AppendMenu(MF_STRING, CMD_CTX_COPY_PAGE_IMG, _T("复制当前页为图片"));

	const UINT cmd = static_cast<UINT>(popup.TrackPopupMenu(
		TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
		menuScreenPt.x, menuScreenPt.y, this));

	switch (cmd)
	{
	case CMD_CTX_COPY_TEXT:
		CopySelectionToClipboard();
		break;
	case CMD_CTX_ANNOT_HIGHLIGHT:
		OnMenuAnnotHighlight();
		break;
	case CMD_CTX_ANNOT_UNDERLINE:
		OnMenuAnnotUnderline();
		break;
	case CMD_CTX_ANNOT_STRIKEOUT:
		OnMenuAnnotStrikeout();
		break;
	case CMD_CTX_ANNOT_SEL_NOTE:
		OnMenuAnnotTextNote();
		break;
	case CMD_CTX_EDIT_HIT_ANNOT:
		if (hitAnnot.type == PDF_ANNOT_FREE_TEXT)
		{
			StartInlineTextAnnotFromExisting(hitAnnotPage, hitAnnot);
		}
		else if (CPDFDocument* pDoc = GetActiveDocument())
		{
			ScopedFileWatchSuspend suspendWatch(this);
			CPDFAnnotNoteDlg noteDlg(true, hitAnnot.contents, this);
			if (noteDlg.DoModal() == IDOK)
			{
				CSingleLock lock(&m_renderLock, TRUE);
				if (UpdatePageAnnotationContents(pDoc, hitAnnotPage, hitAnnot.annotIndex, noteDlg.m_noteText))
				{
					lock.Unlock();
					UpdateDocumentDirtyTitle();
					RenderVisiblePages();
					SetStatusBarMessage(_T("已更新批注内容（按 Ctrl+S 保存写回原 PDF）"));
				}
			}
		}
		break;
	case CMD_CTX_DELETE_HIT_ANNOT:
		if (CPDFDocument* pDoc = GetActiveDocument())
		{
			CSingleLock lock(&m_renderLock, TRUE);
			if (DeletePageAnnotationAt(pDoc, hitAnnotPage, hitAnnot.annotIndex))
			{
				lock.Unlock();
				UpdateDocumentDirtyTitle();
				RenderVisiblePages();
				CString tip;
				tip.Format(_T("已删除%s（按 Ctrl+S 保存写回原 PDF）"), hitAnnot.typeLabel.GetString());
				SetStatusBarMessage(tip);
			}
		}
		break;
	case CMD_CTX_OPEN_LINK:
		OpenExternalLinkWithConfirm(linkHit.externalUri);
		break;
	case CMD_CTX_COPY_LINK:
		if (CopyTextToClipboard(linkHit.externalUri))
			SetStatusBarMessage(_T("已复制链接地址到剪贴板"));
		break;
	case CMD_CTX_ADD_NOTE_HERE:
		if (hasCtxPdfPt)
			PromptAddTextNoteAt(ctxPage, ctxFitzX, ctxFitzY);
		break;
	case CMD_CTX_AREA_CAPTURE:
		StartAreaCaptureMode();
		break;
	case CMD_CTX_COPY_PAGE_IMG:
		CopyCurrentPageAsImage();
		break;
	default:
		break;
	}
}

BOOL CXiaoGongPDFDlg::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
	if (!m_pdfView.GetSafeHwnd())
		return CDialogEx::OnSetCursor(pWnd, nHitTest, message);

	CPoint point;
	::GetCursorPos(&point);
	CRect pdfRect;
	m_pdfView.GetWindowRect(&pdfRect);
	if (!pdfRect.PtInRect(point))
		return CDialogEx::OnSetCursor(pWnd, nHitTest, message);

	// 滚动条属于预览窗口的非客户区；非客户区始终显示箭头。
	if (nHitTest != HTCLIENT)
	{
		::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
		return TRUE;
	}

	if (m_spaceKeyDown && (::GetAsyncKeyState(VK_SPACE) & 0x8000) == 0)
		m_spaceKeyDown = false;

	if (m_isDragging)
	{
		::SetCursor(m_hHandCursorGrab ? m_hHandCursorGrab : ::LoadCursor(nullptr, IDC_HAND));
		return TRUE;
	}

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	m_pdfView.ScreenToClient(&point);
	if (!viewRect.PtInRect(point))
	{
		::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
		return TRUE;
	}

	if (!m_doc && m_documents.empty() && m_pdfView.IsHoveringWelcomeAction())
	{
		::SetCursor(m_hHandCursor ? m_hHandCursor : ::LoadCursor(nullptr, IDC_HAND));
		return TRUE;
	}

	if (m_doc && m_totalPages > 0 && IsHandPanModeActive())
	{
		::SetCursor(m_hHandCursor ? m_hHandCursor : ::LoadCursor(nullptr, IDC_HAND));
		return TRUE;
	}

	// 区域截图模式下：悬浮操作栏按钮显示手型光标，其余位置显示十字准星
	if (m_doc && m_totalPages > 0 && m_isAreaCaptureMode)
	{
		if (m_hasCaptureRect && !m_isDraggingCaptureRect &&
			HitTestCaptureActionBar(point) != CAPTURE_ACT_NONE)
		{
			::SetCursor(m_hHandCursor ? m_hHandCursor : ::LoadCursor(nullptr, IDC_HAND));
		}
		else
		{
			::SetCursor(::LoadCursor(nullptr, IDC_CROSS));
		}
		return TRUE;
	}

	if (m_doc && m_totalPages > 0 && IsInlineTextAnnotVisible())
	{
		if (m_inlineAnnot.draggingMove)
		{
			::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
			return TRUE;
		}
		if (m_inlineAnnot.draggingResizeW)
		{
			::SetCursor(::LoadCursor(nullptr, IDC_SIZEWE));
			return TRUE;
		}
		if (m_inlineAnnot.colorPopupOpen && HitTestInlineAnnotColorSwatch(point) >= 0)
		{
			::SetCursor(m_hHandCursor ? m_hHandCursor : ::LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		const int hitAnnotBtn = HitTestInlineAnnotToolbar(point);
		if (hitAnnotBtn != INLINE_ANNOT_BTN_NONE)
		{
			const bool btnDisabled =
				(hitAnnotBtn == INLINE_ANNOT_BTN_FONT_INC && m_inlineAnnot.fontSizePt >= 47.9f) ||
				(hitAnnotBtn == INLINE_ANNOT_BTN_FONT_DEC && m_inlineAnnot.fontSizePt <= 8.1f) ||
				(hitAnnotBtn == INLINE_ANNOT_BTN_CHAR_SPACE_INC && m_inlineAnnot.charSpacingPt >= 23.9f) ||
				(hitAnnotBtn == INLINE_ANNOT_BTN_CHAR_SPACE_DEC && m_inlineAnnot.charSpacingPt <= 0.01f);
			::SetCursor(btnDisabled ? ::LoadCursor(nullptr, IDC_ARROW) :
				(m_hHandCursor ? m_hHandCursor : ::LoadCursor(nullptr, IDC_HAND)));
			return TRUE;
		}
		if (!m_inlineAnnot.toolbarViewRect.IsRectEmpty() && m_inlineAnnot.toolbarViewRect.PtInRect(point))
		{
			::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
			return TRUE;
		}
		CRect resizeHit = m_inlineAnnot.resizeHandleViewRect;
		if (!resizeHit.IsRectEmpty())
			resizeHit.InflateRect(4, 4);
		if (!resizeHit.IsRectEmpty() && resizeHit.PtInRect(point))
		{
			::SetCursor(::LoadCursor(nullptr, IDC_SIZEWE));
			return TRUE;
		}
		if ((!m_inlineAnnot.moveHandleViewRect.IsRectEmpty() && m_inlineAnnot.moveHandleViewRect.PtInRect(point)) ||
			HitTestInlineAnnotMoveBorder(point))
		{
			::SetCursor(::LoadCursor(nullptr, IDC_SIZEALL));
			return TRUE;
		}
		if (!m_inlineAnnot.boxViewRect.IsRectEmpty() && m_inlineAnnot.boxViewRect.PtInRect(point))
		{
			::SetCursor(::LoadCursor(nullptr, IDC_IBEAM));
			return TRUE;
		}
	}

	if (m_doc && m_totalPages > 0 && IsSelectionActionBarVisible() &&
		HitTestSelectionActionBar(point) != SEL_BAR_NONE)
	{
		::SetCursor(m_hHandCursor ? m_hHandCursor : ::LoadCursor(nullptr, IDC_HAND));
		return TRUE;
	}

	bool overPage = false;
	bool overLink = false;
	bool overFreeTextAnnot = false;
	if (m_doc && m_totalPages > 0)
	{
		const int pageNumber = GetPageAtViewPoint(point, false);
		if (pageNumber >= 0 && pageNumber < m_totalPages)
		{
			overPage = true;
			if (!m_isSelecting)
			{
				PdfLinkHit linkHit;
				overLink = HitTestLink(point, linkHit);
				if (!overLink && !IsInlineTextAnnotVisible())
				{
					int hitPage = -1;
					PageAnnotHitInfo hitInfo;
					if (HitTestAnnotationAtViewPoint(point, hitPage, hitInfo) &&
						hitInfo.type == PDF_ANNOT_FREE_TEXT)
					{
						overFreeTextAnnot = true;
					}
				}
			}
		}
	}

	if (overLink || overFreeTextAnnot)
		::SetCursor(m_hHandCursor ? m_hHandCursor : ::LoadCursor(nullptr, IDC_HAND));
	else if (overPage)
		::SetCursor(::LoadCursor(nullptr, IDC_IBEAM));
	else
		::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
	return TRUE;
}

void CXiaoGongPDFDlg::ResetPanOffset()
{
	m_panOffset.x = 0;
	m_panOffset.y = 0;
}

// ============================================================================
// 连续滚动模式实现
// ============================================================================

// 计算所有页面的位置和高度（支持单页连续与双页并排两种排版模式）
void CXiaoGongPDFDlg::CalculatePagePositions(bool recalcScale)
{
	if (!m_doc || m_totalPages <= 0)
		return;

	m_pageYPositions.assign(m_totalPages, 0);
	m_pageHeights.assign(m_totalPages, 0);
	m_pageRowTops.assign(m_totalPages, 0);
	m_pageRowHeights.assign(m_totalPages, 0);

	// 获取预览区域宽度
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	int viewWidth = max(1, viewRect.Width() - 40);  // 留出左右边距各20像素

	CPDFDocument* pDoc = GetActiveDocument();
	if (pDoc && !pDoc->HasPageBoundsCache())
	{
		pDoc->CachePageseBounds();
	}

	auto getEffectivePagePtSize = [&](int pageIdx, float& outWidth, float& outHeight) -> bool
	{
		outWidth = 0.0f;
		outHeight = 0.0f;
		if (pDoc && pDoc->GetPageBounds(pageIdx, outWidth, outHeight) &&
			outWidth > 0.0f && outHeight > 0.0f)
		{
			return true;
		}
		fz_page* page = nullptr;
		bool ok = false;
		fz_var(page);
		fz_var(ok);
		fz_try(m_ctx)
		{
			page = fz_load_page(m_ctx, m_doc, pageIdx);
			if (page)
			{
				fz_rect bounds = fz_bound_page(m_ctx, page);
				float w = bounds.x1 - bounds.x0;
				float h = bounds.y1 - bounds.y0;
				const int rotation = GetPageRotation(pageIdx);
				if (rotation == 90 || rotation == 270)
					std::swap(w, h);
				outWidth = w;
				outHeight = h;
				ok = (w > 0.0f && h > 0.0f);
			}
		}
		fz_always(m_ctx)
		{
			if (page)
				fz_drop_page(m_ctx, page);
		}
		fz_catch(m_ctx)
		{
			ok = false;
		}
		return ok;
	};

	// 第一步：扫描所有排版行，获取最大行物理点宽度（双页模式下尾页单张亦预留双栏总宽）
	float maxRowPtWidth = 0.0f;
	const bool isTwoPageSpread = (m_layoutMode == LAYOUT_TWO_PAGE_SPREAD);
	if (!isTwoPageSpread)
	{
		if (pDoc && pDoc->HasPageBoundsCache())
		{
			maxRowPtWidth = pDoc->GetMaxPageWidth();
		}
		else
		{
			for (int i = 0; i < m_totalPages; ++i)
			{
				float w = 0.0f, h = 0.0f;
				if (getEffectivePagePtSize(i, w, h) && w > maxRowPtWidth)
					maxRowPtWidth = w;
			}
		}
	}
	else
	{
		int idx = 0;
		while (idx < m_totalPages)
		{
			int leftP = idx;
			int rightP = -1;
			GetLayoutRowPages(idx, leftP, rightP);
			float wL = 0.0f, hL = 0.0f;
			getEffectivePagePtSize(leftP, wL, hL);
			float rowW = wL;
			if (rightP >= 0)
			{
				float wR = 0.0f, hR = 0.0f;
				getEffectivePagePtSize(rightP, wR, hR);
				rowW += wR;
			}
			else
			{
				rowW += wL;
			}
			if (rowW > maxRowPtWidth)
				maxRowPtWidth = rowW;
			idx = (rightP >= 0) ? (rightP + 1) : (leftP + 1);
		}
	}

	// 仅在需要时重新计算统一缩放比例
	if (recalcScale)
	{
		const int rowGapPx = isTwoPageSpread ? SPREAD_GAP : 0;
		const int availViewWidth = max(1, viewWidth - rowGapPx);
		float baseScale = (maxRowPtWidth > 0.0f) ? ((float)availViewWidth / maxRowPtWidth * 0.95f) : 1.0f;
		m_uniformScale = baseScale * m_customZoom;
		if (m_zoomMode == ZOOM_FIT_WIDTH)
		{
			m_uniformScale = FitWidthScale(max(1, viewRect.Width() - rowGapPx), maxRowPtWidth);
		}
		if (m_zoomMode == ZOOM_FIT_PAGE)
		{
			const int viewHeight = max(1, viewRect.Height() - 40);
			float fitScale = 0.0f;
			int idx = 0;
			while (idx < m_totalPages)
			{
				int leftP = idx;
				int rightP = -1;
				GetLayoutRowPages(idx, leftP, rightP);
				float wL = 0.0f, hL = 0.0f;
				const bool okL = getEffectivePagePtSize(leftP, wL, hL);
				float rowPtW = wL;
				float rowPtH = hL;
				if (rightP >= 0)
				{
					float wR = 0.0f, hR = 0.0f;
					if (getEffectivePagePtSize(rightP, wR, hR))
					{
						rowPtW += wR;
						rowPtH = max(rowPtH, hR);
					}
				}
				else if (isTwoPageSpread)
				{
					rowPtW += wL;
				}
				if (okL && rowPtW > 0.0f && rowPtH > 0.0f)
				{
					const int rowAvailW = max(1, viewWidth - (isTwoPageSpread ? SPREAD_GAP : 0));
					const float rowScale = min((float)rowAvailW / rowPtW, (float)viewHeight / rowPtH);
					if (fitScale == 0.0f || rowScale < fitScale)
						fitScale = rowScale;
				}
				idx = (rightP >= 0) ? (rightP + 1) : (leftP + 1);
			}
			if (fitScale > 0.0f)
				m_uniformScale = fitScale * 0.95f;
		}
	}

	// 第二步：按排版行计算每页与每行的纵坐标及高度，不同高度对页在行内垂直居中
	int currentY = PAGE_SPACING;
	int maxRowWidthPx = 0;
	int idx = 0;
	while (idx < m_totalPages)
	{
		int leftP = idx;
		int rightP = -1;
		GetLayoutRowPages(idx, leftP, rightP);

		float wL = 0.0f, hL = 0.0f;
		getEffectivePagePtSize(leftP, wL, hL);
		const int wLeftPx = ClampRenderDimension(wL * m_uniformScale);
		const int hLeftPx = max(0, static_cast<int>(hL * m_uniformScale));

		if (rightP >= 0)
		{
			float wR = 0.0f, hR = 0.0f;
			getEffectivePagePtSize(rightP, wR, hR);
			const int wRightPx = ClampRenderDimension(wR * m_uniformScale);
			const int hRightPx = max(0, static_cast<int>(hR * m_uniformScale));
			const int rowHeight = max(hLeftPx, hRightPx);
			const int rowWidthPx = wLeftPx + SPREAD_GAP + wRightPx;

			m_pageRowTops[leftP] = currentY;
			m_pageRowTops[rightP] = currentY;
			m_pageRowHeights[leftP] = rowHeight;
			m_pageRowHeights[rightP] = rowHeight;
			m_pageHeights[leftP] = hLeftPx;
			m_pageHeights[rightP] = hRightPx;
			m_pageYPositions[leftP] = currentY + (rowHeight - hLeftPx) / 2;
			m_pageYPositions[rightP] = currentY + (rowHeight - hRightPx) / 2;

			maxRowWidthPx = max(maxRowWidthPx, rowWidthPx);
			currentY += rowHeight + PAGE_SPACING;
			idx = rightP + 1;
		}
		else
		{
			m_pageRowTops[leftP] = currentY;
			m_pageRowHeights[leftP] = hLeftPx;
			m_pageHeights[leftP] = hLeftPx;
			m_pageYPositions[leftP] = currentY;

			const int soloRowWidthPx = isTwoPageSpread ?
				(wLeftPx + SPREAD_GAP + wLeftPx) : wLeftPx;
			maxRowWidthPx = max(maxRowWidthPx, soloRowWidthPx);
			currentY += hLeftPx + PAGE_SPACING;
			idx = leftP + 1;
		}
	}

	m_totalScrollHeight = currentY;
	m_totalScrollWidth = maxRowWidthPx;

#ifdef _DEBUG
	TRACE(_T("页面排版完成(layout=%d): 总高度=%d, 总宽度=%d\n"),
		static_cast<int>(m_layoutMode), m_totalScrollHeight, m_totalScrollWidth);
#endif
}

HBITMAP CXiaoGongPDFDlg::RenderContinuousPageToCache(int pageNumber, HDC hDC)
{
	CPDFDocument* pDoc = GetActiveDocument();
	if (!pDoc || !m_ctx || !m_doc || pageNumber < 0 || pageNumber >= m_totalPages)
		return nullptr;
	CSingleLock lock(&m_renderLock, TRUE);

	const int rotation = GetPageRotation(pageNumber);
	float pageWidth = 0.0f;
	float pageHeight = 0.0f;
	fz_rect bounds = { 0 };
	const bool gotBounds = pDoc->GetPageBounds(pageNumber, pageWidth, pageHeight);
	bool gotBaseBounds = pDoc->GetPageBaseBounds(pageNumber, bounds);
	if (!gotBounds || !gotBaseBounds)
	{
		fz_page* pPage = nullptr;
		bool loadFailed = false;
		fz_var(pPage);
		fz_var(bounds);
		fz_var(gotBaseBounds);
		fz_var(loadFailed);
		fz_try(m_ctx)
		{
			pPage = fz_load_page(m_ctx, m_doc, pageNumber);
			if (!pPage)
				fz_throw(m_ctx, FZ_ERROR_GENERIC, "Failed to load page");
			bounds = fz_bound_page(m_ctx, pPage);
			gotBaseBounds = true;
		}
		fz_always(m_ctx)
		{
			if (pPage)
				fz_drop_page(m_ctx, pPage);
		}
		fz_catch(m_ctx)
		{
			loadFailed = true;
		}
		if (loadFailed)
			return nullptr;

		if (!gotBounds)
		{
			pageWidth = bounds.x1 - bounds.x0;
			pageHeight = bounds.y1 - bounds.y0;
			if (rotation == 90 || rotation == 270)
				std::swap(pageWidth, pageHeight);
		}
	}

	const int width = ClampRenderDimension(pageWidth * m_uniformScale);
	const int height = ClampRenderDimension(pageHeight * m_uniformScale);
	const ::PageCacheKey key{ pageNumber, width, height, rotation };
	auto& cache = pDoc->GetPageCache();
	auto& order = pDoc->GetCacheOrder();
	auto it = cache.find(key);
	if (it != cache.end() && it->second.hBitmap)
	{
		order.remove(key);
		order.push_back(key);
		return it->second.hBitmap;
	}

	// 直接创建 32-bit BGRA DIBSection，并交给 MuPDF 的 fz_device_bgr 零拷贝渲染
	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = width;
	bmi.bmiHeader.biHeight = -height;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	BYTE* pBits = nullptr;
	HBITMAP hBitmap = ::CreateDIBSection(hDC, &bmi, DIB_RGB_COLORS,
		reinterpret_cast<void**>(&pBits), nullptr, 0);
	if (!hBitmap || !pBits)
	{
		if (hBitmap)
			::DeleteObject(hBitmap);
		return nullptr;
	}

	fz_pixmap* pPixmap = nullptr;
	fz_device* pDev = nullptr;
	bool rendered = false;
	fz_var(pPixmap);
	fz_var(pDev);
	fz_var(rendered);
	fz_try(m_ctx)
	{
		fz_display_list* pList = pDoc->GetDisplayList(pageNumber);
		if (!pList)
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "Failed to create display list");
		pPixmap = fz_new_pixmap_with_data(m_ctx, fz_device_bgr(m_ctx),
			width, height, nullptr, 1, width * 4, pBits);
		fz_clear_pixmap_with_value(m_ctx, pPixmap, 0xff);
		fz_matrix ctm = BuildPageRenderMatrix(bounds, m_uniformScale, rotation);
		pDev = fz_new_draw_device(m_ctx, ctm, pPixmap);
		fz_run_display_list(m_ctx, pList, pDev, fz_identity, fz_infinite_rect, nullptr);
		fz_close_device(m_ctx, pDev);
		rendered = true;
	}
	fz_always(m_ctx)
	{
		if (pDev)
		{
			fz_drop_device(m_ctx, pDev);
			pDev = nullptr;
		}
		if (pPixmap)
		{
			fz_drop_pixmap(m_ctx, pPixmap);
			pPixmap = nullptr;
		}
	}
	fz_catch(m_ctx)
	{
		rendered = false;
	}

	if (!rendered)
	{
		::DeleteObject(hBitmap);
		return nullptr;
	}

	auto existingIt = cache.find(key);
	if (existingIt != cache.end())
	{
		if (existingIt->second.hBitmap)
			::DeleteObject(existingIt->second.hBitmap);
		existingIt->second.hBitmap = hBitmap;
		order.remove(key);
	}
	else
	{
		cache[key] = ::PageCacheItem{ hBitmap };
	}
	order.push_back(key);
	TrimBitmapCache(cache, order, true, PAGE_CACHE_BUDGET, 50);
	return hBitmap;
}

void CXiaoGongPDFDlg::PrefetchNearbyPages()
{
	if (!m_doc || m_totalPages <= 0 ||
		(int)m_pageYPositions.size() != m_totalPages ||
		(int)m_pageHeights.size() != m_totalPages)
		return;
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	int screenHeight = viewRect.Height();
	if (screenHeight <= 0)
		return;
	int top = max(0, m_scrollPosition - 2 * screenHeight);
	int bottom = m_scrollPosition + 3 * screenHeight;
	int center = m_scrollPosition + screenHeight / 2;
	int bestPage = -1;
	int bestDistance = INT_MAX;
	CPDFDocument* pDoc = GetActiveDocument();
	if (!pDoc)
		return;
	for (int i = 0; i < m_totalPages; ++i)
	{
		int pageTop = m_pageYPositions[i];
		int pageBottom = pageTop + m_pageHeights[i];
		if (pageBottom < top || pageTop > bottom ||
			(pageBottom >= m_scrollPosition && pageTop <= m_scrollPosition + screenHeight))
			continue;
		float width = 0.0f, height = 0.0f;
		if (!pDoc->GetPageBounds(i, width, height))
			continue;
		::PageCacheKey key{ i, ClampRenderDimension(width * m_uniformScale),
			ClampRenderDimension(height * m_uniformScale), GetPageRotation(i) };
		if (pDoc->GetPageCache().find(key) != pDoc->GetPageCache().end())
			continue;
		int distance = abs((pageTop + pageBottom) / 2 - center);
		if (distance < bestDistance)
		{
			bestPage = i;
			bestDistance = distance;
		}
	}
	if (bestPage < 0)
		return;
	CDC* pDC = m_pdfView.GetDC();
	if (!pDC)
		return;
	const bool rendered = RenderContinuousPageToCache(bestPage, pDC->GetSafeHdc()) != nullptr;
	m_pdfView.ReleaseDC(pDC);
	if (rendered)
		SetTimer(TIMER_ID_PAGE_PREFETCH, 30, nullptr);
}

// 渲染可见的页面
void CXiaoGongPDFDlg::RenderVisiblePages()
{
	// 添加更严格的边界检查，防止切换PDF时vector越界
	if (!m_doc || m_totalPages <= 0 || m_pageYPositions.empty())
		return;

	// 确保vector大小与总页数匹配，避免切换文档时的时序问题导致越界
	if ((int)m_pageYPositions.size() < m_totalPages ||
		(int)m_pageHeights.size() < m_totalPages ||
		(int)m_pageRowTops.size() < m_totalPages)
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
	if (viewWidth <= 0 || viewHeight <= 0)
		return;

	// 计算可见范围
	int visibleTop = m_scrollPosition;
	int visibleBottom = m_scrollPosition + viewHeight;

#ifdef _DEBUG
	TRACE(_T("RenderVisiblePages: viewSize=(%d,%d), visibleTop=%d\n"),
		viewWidth, viewHeight, visibleTop);
#endif

	// 创建内存DC和位图
	CSingleLock lock(&m_renderLock, TRUE);

	CPDFDocument* pActiveDoc = GetActiveDocument();
	if (!pActiveDoc)
		return;

	CDC* pDC = m_pdfView.GetDC();
	if (!pDC)
		return;
	CDC memDC;
	CBitmap bmp;
	if (!memDC.CreateCompatibleDC(pDC) ||
		!bmp.CreateCompatibleBitmap(pDC, viewWidth, viewHeight))
	{
		m_pdfView.ReleaseDC(pDC);
		return;
	}
	CBitmap* pOldBmp = memDC.SelectObject(&bmp);

	// 填充背景色
	memDC.FillSolidRect(0, 0, viewWidth, viewHeight, ViewBgColor());

	auto firstVisibleIt = std::lower_bound(m_pageRowTops.begin(),
		m_pageRowTops.begin() + m_totalPages, visibleTop);
	int firstVisibleIndex = (int)(firstVisibleIt - m_pageRowTops.begin());
	if (firstVisibleIndex >= m_totalPages)
		firstVisibleIndex = m_totalPages - 1;
	if (firstVisibleIndex > 0)
		--firstVisibleIndex;
	while (firstVisibleIndex > 0 &&
		m_pageRowTops[firstVisibleIndex - 1] == m_pageRowTops[firstVisibleIndex])
	{
		--firstVisibleIndex;
	}

	auto lastVisibleIt = std::upper_bound(m_pageRowTops.begin(),
		m_pageRowTops.begin() + m_totalPages, visibleBottom + PAGE_SPACING);
	int lastVisibleIndex = (int)(lastVisibleIt - m_pageRowTops.begin());
	if (lastVisibleIndex < m_totalPages - 1)
		++lastVisibleIndex;
	while (lastVisibleIndex + 1 < m_totalPages &&
		m_pageRowTops[lastVisibleIndex + 1] == m_pageRowTops[lastVisibleIndex])
	{
		++lastVisibleIndex;
	}

	// 渲染所有可见的页面
	for (int i = firstVisibleIndex; i < m_totalPages && i <= lastVisibleIndex; ++i)
	{
		int pageY = m_pageYPositions[i];
		int pageHeight = m_pageHeights[i];

		// 检查页面是否可见
		if (pageY + pageHeight < visibleTop || pageY > visibleBottom)
			continue;

		HBITMAP hBitmap = RenderContinuousPageToCache(i, memDC.GetSafeHdc());
		if (!hBitmap)
			continue;

		BITMAP bmpInfo = { 0 };
		if (!::GetObject(hBitmap, sizeof(bmpInfo), &bmpInfo) ||
			bmpInfo.bmWidth <= 0 || bmpInfo.bmHeight == 0)
			continue;
		const int width = bmpInfo.bmWidth;
		const int height = abs(bmpInfo.bmHeight);
		const int rotation = GetPageRotation(i);

		const std::vector<int>* pMatchIndices =
			(i >= 0 && i < (int)m_searchMatchesByPage.size()) ? &m_searchMatchesByPage[i] : nullptr;
		const bool pageHasHighlights = pMatchIndices && !pMatchIndices->empty();
		fz_rect bounds = { 0 };
		const bool gotBaseBounds = pageHasHighlights && TryGetPageBaseBounds(i, bounds);

		CDC pageDC;
		pageDC.CreateCompatibleDC(&memDC);
		HBITMAP oldPageBmp = (HBITMAP)pageDC.SelectObject(hBitmap);

		int destX = 0;
		int destY = 0;
		if (!GetPageViewPlacement(i, viewWidth, destX, destY))
		{
			destY = pageY - visibleTop + m_panOffset.y;
			destX = (width < viewWidth ? (viewWidth - width) / 2 :
				-min(m_scrollPositionH, width - viewWidth)) + m_panOffset.x;
		}

#ifdef _DEBUG
		TRACE(_T("  Page %d: pageY=%d, size=(%d,%d), dest=(%d,%d)\n"),
			i, pageY, width, height, destX, destY);
#endif

		const DWORD pageRop = m_readingMode == READING_NIGHT ? NOTSRCCOPY : SRCCOPY;
		memDC.BitBlt(destX, destY, width, height, &pageDC, 0, 0, pageRop);

		if (pageHasHighlights && pMatchIndices && gotBaseBounds)
		{
			for (int matchIdx : *pMatchIndices)
			{
				CRect highlightRect = TransformQuadToBitmapRect(
					m_searchMatches[matchIdx].quad, bounds, rotation, m_uniformScale);
				highlightRect.OffsetRect(destX, destY);
				if (highlightRect.IsRectEmpty() || highlightRect.Width() <= 0 || highlightRect.Height() <= 0)
				{
					continue;
				}

#ifdef _DEBUG
				TRACE(_T("Page %d, Match %d: rect=(%d,%d,%d,%d), rotation=%d, scale=%.3f\n"),
					i, matchIdx, highlightRect.left, highlightRect.top,
					highlightRect.right, highlightRect.bottom, rotation, m_uniformScale);
#endif

				COLORREF fillColor = (matchIdx == m_currentMatchIndex) ? RGB(255, 150, 0) : RGB(255, 200, 0);

				CDC tempDC;
				tempDC.CreateCompatibleDC(&memDC);
				CBitmap tempBmp;
				tempBmp.CreateCompatibleBitmap(&memDC, highlightRect.Width(), highlightRect.Height());
				CBitmap* pOldTempBmp = tempDC.SelectObject(&tempBmp);
				tempDC.FillSolidRect(0, 0, highlightRect.Width(), highlightRect.Height(), fillColor);

				BLENDFUNCTION bf;
				bf.BlendOp = AC_SRC_OVER;
				bf.BlendFlags = 0;
				bf.SourceConstantAlpha = (matchIdx == m_currentMatchIndex) ? 200 : 180;
				bf.AlphaFormat = 0;
				memDC.AlphaBlend(highlightRect.left, highlightRect.top,
					highlightRect.Width(), highlightRect.Height(),
					&tempDC, 0, 0, highlightRect.Width(), highlightRect.Height(), bf);
				tempDC.SelectObject(pOldTempBmp);

				CPen borderPen(PS_SOLID, 1, RGB(255, 140, 0));
				CPen* pOldPen = memDC.SelectObject(&borderPen);
				HGDIOBJ pOldBrush = memDC.SelectStockObject(NULL_BRUSH);
				memDC.Rectangle(highlightRect);
				memDC.SelectObject(pOldBrush);
				memDC.SelectObject(pOldPen);
			}
		}

		pageDC.SelectObject(oldPageBmp);
	}

	// ★★★ 保存位图并设置到控件（这样控件可以自动处理重绘）
	// 将内存DC中的位图分离并保存
	memDC.SelectObject(pOldBmp);

	// 直接接管离屏位图句柄，避免整屏 CopyImage 带来的额外拷贝。
	HBITMAP hNewBitmap = (HBITMAP)bmp.Detach();

	// ★★★ 先设置新位图到控件，再删除旧位图，避免快速拖动时出现闪白
	if (hNewBitmap)
	{
#ifdef _DEBUG
		TRACE(_T("RenderVisiblePages: 开始更新位图...\n"));
#endif
		// ★★★ 先调用 SetDisplayBitmap 再删除旧位图：
		// SetDisplayBitmap 只存句柄 + InvalidateRect(FALSE)，完全不走 CStatic 内部逻辑，无白闪风险
		m_pdfView.SetDisplayBitmap(hNewBitmap);

		// 在设置新位图之后再删除旧位图，确保控件始终有有效的位图可以显示
		SafeDeleteBitmap(m_hContinuousViewBitmap);

#ifdef _DEBUG
		TRACE(_T("RenderVisiblePages: 位图更新完成，准备刷新显示...\n"));
#endif

		// 保存新位图句柄
		m_hContinuousViewBitmap = hNewBitmap;

		// ★★★ 手动触发一次重绘，使用 RedrawWindow 确保立即显示
		m_pdfView.RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_NOERASE);

#ifdef _DEBUG
		TRACE(_T("RenderVisiblePages: 已触发重绘\n"));
#endif
	}
	else
	{
		// 创建失败，保持原有位图
#ifdef _DEBUG
		TRACE(_T("RenderVisiblePages: detach bitmap failed\n"));
#endif
	}

#ifdef _DEBUG
	CRect ctrlRect;
	m_pdfView.GetClientRect(&ctrlRect);
	TRACE(_T("m_pdfView control size: (%d,%d), bitmap size: (%d,%d)\n"),
		ctrlRect.Width(), ctrlRect.Height(), viewWidth, viewHeight);
#endif

	m_pdfView.ReleaseDC(pDC);
	KillTimer(TIMER_ID_PAGE_PREFETCH);
	SetTimer(TIMER_ID_PAGE_PREFETCH, 180, nullptr);

	// 更新当前页（双页并排同行内若用户主动跳到右侧页则保留其页码，跨行滚动时自动更新）
	int newCurrentPage = GetPageAtPosition(m_scrollPosition + viewHeight / 2);
	if (newCurrentPage != m_currentPage && newCurrentPage >= 0 && newCurrentPage < m_totalPages &&
		!IsSameLayoutRow(m_currentPage, newCurrentPage))
	{
		m_currentPage = newCurrentPage;
		UpdatePageControls();
	}
}

// 垂直滚动条消息处理
void CXiaoGongPDFDlg::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	if (!m_doc)
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
	{
		// ★★★ 修复：将滚动条位置映射回内容坐标系
		// 获取当前滚动条信息
		SCROLLINFO si = { 0 };
		si.cbSize = sizeof(SCROLLINFO);
		si.fMask = SIF_ALL;
		::GetScrollInfo(m_pdfView.GetSafeHwnd(), SB_VERT, &si);

		// 滚动条坐标系：nPos ∈ [0, si.nMax - si.nPage + 1]
		// 内容坐标系：newPos ∈ [0, m_totalScrollHeight - viewRect.Height()]
		int scrollbarMaxPos = max(0, si.nMax - si.nPage + 1);
		int contentMaxScroll = max(0, m_totalScrollHeight - viewRect.Height());

		if (scrollbarMaxPos > 0 && contentMaxScroll > 0)
		{
			// 线性映射：将滚动条位置映射到内容位置
			newPos = (int)((float)nPos / scrollbarMaxPos * contentMaxScroll);
		}
		else
		{
			newPos = 0;
		}
		break;
	}
	}

	// 限制范围
	int maxScroll = m_totalScrollHeight - viewRect.Height();
	if (maxScroll < 0) maxScroll = 0;
	if (newPos < 0) newPos = 0;
	if (newPos > maxScroll) newPos = maxScroll;

	if (newPos != m_scrollPosition)
	{
		ClearActiveCaptureRect();
		m_scrollPosition = newPos;
		UpdateScrollBar();
		RenderVisiblePages();
	}

	CDialogEx::OnVScroll(nSBCode, nPos, pScrollBar);
}

void CXiaoGongPDFDlg::ScrollToVerticalPosition(int position)
{
	if (!m_doc)
		return;
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	position = max(0, min(position, max(0, m_totalScrollHeight - viewRect.Height())));
	if (position == m_scrollPosition)
		return;
	ClearActiveCaptureRect();
	m_scrollPosition = position;
	UpdateScrollBar();
	RenderVisiblePages();
}

void CXiaoGongPDFDlg::ScrollToHorizontalPosition(int position)
{
	if (!m_doc)
		return;
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	position = max(0, min(position, max(0, m_totalScrollWidth - viewRect.Width())));
	if (position == m_scrollPositionH)
		return;
	ClearActiveCaptureRect();
	m_scrollPositionH = position;
	UpdateScrollBar();
	RenderVisiblePages();
}

// ★★★ 横向滚动条消息处理
void CXiaoGongPDFDlg::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	int pageSize = viewRect.Width();

	int newPos = m_scrollPositionH;

	switch (nSBCode)
	{
	case SB_LINELEFT:
		newPos -= 20;
		break;
	case SB_LINERIGHT:
		newPos += 20;
		break;
	case SB_PAGELEFT:
		newPos -= pageSize;
		break;
	case SB_PAGERIGHT:
		newPos += pageSize;
		break;
	case SB_THUMBPOSITION:
	case SB_THUMBTRACK:
	{
		// ★★★ 修复：将滚动条位置映射回内容坐标系
		// 获取当前滚动条信息
		SCROLLINFO siH = { 0 };
		siH.cbSize = sizeof(SCROLLINFO);
		siH.fMask = SIF_ALL;
		::GetScrollInfo(m_pdfView.GetSafeHwnd(), SB_HORZ, &siH);
		
		// 滚动条坐标系：nPos ∈ [0, siH.nMax - siH.nPage + 1]
		// 内容坐标系：newPos ∈ [0, m_totalScrollWidth - viewRect.Width()]
		int scrollbarMaxPosH = max(0, siH.nMax - siH.nPage + 1);
		int contentMaxScrollH = max(0, m_totalScrollWidth - viewRect.Width());
		
		if (scrollbarMaxPosH > 0 && contentMaxScrollH > 0)
		{
			// 线性映射：将滚动条位置映射到内容位置
			newPos = (int)((float)nPos / scrollbarMaxPosH * contentMaxScrollH);
		}
		else
		{
			newPos = 0;
		}
		break;
	}
	}

	// 限制范围
	int maxScroll = m_totalScrollWidth - viewRect.Width();
	if (maxScroll < 0) maxScroll = 0;
	if (newPos < 0) newPos = 0;
	if (newPos > maxScroll) newPos = maxScroll;

	if (newPos != m_scrollPositionH)
	{
		ClearActiveCaptureRect();
		m_scrollPositionH = newPos;
		UpdateScrollBar();
		RenderVisiblePages();
	}

	CDialogEx::OnHScroll(nSBCode, nPos, pScrollBar);
}

// 更新滚动条
void CXiaoGongPDFDlg::UpdateScrollBar()
{
	// 全屏模式下隐藏所有滚动条并返回
	if (m_isFullscreen)
	{
		m_pdfView.ShowScrollBar(SB_VERT, FALSE);
		m_pdfView.ShowScrollBar(SB_HORZ, FALSE);
		if (m_pdfVerticalScrollBar.GetSafeHwnd())
			m_pdfVerticalScrollBar.ShowWindow(SW_HIDE);
		if (m_pdfHorizontalScrollBar.GetSafeHwnd())
			m_pdfHorizontalScrollBar.ShowWindow(SW_HIDE);
		if (m_pdfScrollCorner.GetSafeHwnd())
			m_pdfScrollCorner.ShowWindow(SW_HIDE);
		return;
	}

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);

	// The custom bars use document coordinates directly, without the 16-bit scroll message limit.
	m_pdfView.ShowScrollBar(SB_VERT, FALSE);
	m_pdfView.ShowScrollBar(SB_HORZ, FALSE);
	m_pdfView.GetClientRect(&viewRect);
	m_scrollPosition = max(0, min(m_scrollPosition,
		max(0, m_totalScrollHeight - viewRect.Height())));
	int horizontalContentWidth = m_totalScrollWidth;
	if (m_doc &&
		m_pageRowTops.size() == static_cast<size_t>(m_totalPages) &&
		m_pageRowHeights.size() == static_cast<size_t>(m_totalPages))
	{
		CPDFDocument* pDoc = GetActiveDocument();
		if (pDoc)
		{
			const int visibleTop = m_scrollPosition;
			const int visibleBottom = visibleTop + viewRect.Height();
			auto first = std::lower_bound(m_pageRowTops.begin(),
				m_pageRowTops.end(), visibleTop);
			int firstIndex = static_cast<int>(first - m_pageRowTops.begin());
			if (firstIndex >= m_totalPages)
				firstIndex = m_totalPages - 1;
			if (firstIndex > 0)
				--firstIndex;
			while (firstIndex > 0 && m_pageRowTops[firstIndex - 1] == m_pageRowTops[firstIndex])
				--firstIndex;

			int visibleWidth = 0;
			bool boundsMissing = false;
			int idx = max(0, firstIndex);
			while (idx < m_totalPages && m_pageRowTops[idx] <= visibleBottom)
			{
				int leftP = idx;
				int rightP = -1;
				GetLayoutRowPages(idx, leftP, rightP);
				if (m_pageRowTops[leftP] + m_pageRowHeights[leftP] >= visibleTop)
				{
					float wL = 0.0f, hL = 0.0f;
					if (!pDoc->GetPageBounds(leftP, wL, hL))
					{
						boundsMissing = true;
						break;
					}
					const int leftWidthPx = ClampRenderDimension(wL * m_uniformScale);
					int rowW = leftWidthPx;
					if (rightP >= 0)
					{
						float wR = 0.0f, hR = 0.0f;
						if (!pDoc->GetPageBounds(rightP, wR, hR))
						{
							boundsMissing = true;
							break;
						}
						rowW += SPREAD_GAP + ClampRenderDimension(wR * m_uniformScale);
					}
					else if (m_layoutMode == LAYOUT_TWO_PAGE_SPREAD)
					{
						rowW += SPREAD_GAP + leftWidthPx;
					}
					visibleWidth = max(visibleWidth, rowW);
				}
				idx = (rightP >= 0) ? (rightP + 1) : (leftP + 1);
			}
			if (!boundsMissing)
				horizontalContentWidth = visibleWidth;
		}
	}
	m_scrollPositionH = max(0, min(m_scrollPositionH,
		max(0, horizontalContentWidth - viewRect.Width())));
	const bool showHorizontalBar = m_doc && horizontalContentWidth > viewRect.Width();
	if (m_pdfHorizontalScrollBar.GetSafeHwnd() &&
		m_pdfHorizontalScrollBar.IsWindowVisible() != showHorizontalBar &&
		m_statusBar.GetSafeHwnd())
	{
		CRect pdfRect, statusRect;
		m_pdfView.GetWindowRect(&pdfRect);
		m_statusBar.GetWindowRect(&statusRect);
		ScreenToClient(&pdfRect);
		ScreenToClient(&statusRect);
		const int viewHeight = max(1, statusRect.top - pdfRect.top - (showHorizontalBar ? 16 : 0));
		m_pdfView.SetWindowPos(nullptr, 0, 0, pdfRect.Width(), viewHeight,
			SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		if (m_pdfVerticalScrollBar.GetSafeHwnd())
		{
			m_pdfVerticalScrollBar.SetWindowPos(nullptr, 0, 0, 16, viewHeight,
				SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
		m_pdfView.GetClientRect(&viewRect);
		m_scrollPosition = max(0, min(m_scrollPosition,
			max(0, m_totalScrollHeight - viewRect.Height())));
	}
	if (m_pdfVerticalScrollBar.GetSafeHwnd())
	{
		m_pdfVerticalScrollBar.SetRange(m_totalScrollHeight, viewRect.Height(), m_scrollPosition);
		m_pdfVerticalScrollBar.ShowWindow(m_doc && m_totalScrollHeight > viewRect.Height() ?
			SW_SHOW : SW_HIDE);
	}
	if (m_pdfHorizontalScrollBar.GetSafeHwnd())
	{
		m_pdfHorizontalScrollBar.SetRange(horizontalContentWidth, viewRect.Width(), m_scrollPositionH);
		m_pdfHorizontalScrollBar.ShowWindow(showHorizontalBar ?
			SW_SHOW : SW_HIDE);
	}
	if (m_pdfScrollCorner.GetSafeHwnd())
		m_pdfScrollCorner.ShowWindow(showHorizontalBar ? SW_SHOW : SW_HIDE);
}

// 获取指定纵向文档位置的排版行主页面索引（二分查找单调 m_pageRowTops）
int CXiaoGongPDFDlg::GetPageAtPosition(int yPos)
{
	if (m_totalPages <= 0 ||
		(int)m_pageRowTops.size() < m_totalPages ||
		(int)m_pageHeights.size() < m_totalPages)
	{
		return 0;
	}

	auto it = std::upper_bound(m_pageRowTops.begin(),
		m_pageRowTops.begin() + m_totalPages, yPos);
	int idx = static_cast<int>(it - m_pageRowTops.begin()) - 1;
	if (idx < 0)
		idx = 0;
	if (idx >= m_totalPages)
		idx = m_totalPages - 1;

	int leftPage = idx;
	int rightPage = -1;
	GetLayoutRowPages(idx, leftPage, rightPage);
	return leftPage;
}

void CXiaoGongPDFDlg::GetLayoutRowPages(int pageNumber, int& outLeftPage, int& outRightPage) const
{
	outLeftPage = max(0, min(pageNumber, max(0, m_totalPages - 1)));
	outRightPage = -1;
	if (m_totalPages <= 0 || m_layoutMode == LAYOUT_SINGLE_CONTINUOUS)
		return;

	if (m_layoutMode == LAYOUT_TWO_PAGE_SPREAD)
	{
		outLeftPage = (outLeftPage / 2) * 2;
		outRightPage = (outLeftPage + 1 < m_totalPages) ? (outLeftPage + 1) : -1;
	}
}

bool CXiaoGongPDFDlg::IsSameLayoutRow(int pageA, int pageB) const
{
	if (pageA < 0 || pageB < 0 || pageA >= m_totalPages || pageB >= m_totalPages)
		return false;
	if (pageA == pageB)
		return true;
	int leftP = -1;
	int rightP = -1;
	GetLayoutRowPages(pageA, leftP, rightP);
	return (pageB == leftP || pageB == rightP);
}

int CXiaoGongPDFDlg::GetPrevSpreadPage(int pageNumber) const
{
	if (m_totalPages <= 0)
		return 0;
	if (m_layoutMode == LAYOUT_SINGLE_CONTINUOUS || m_totalPages <= 1)
		return max(0, pageNumber - 1);

	int leftP = 0;
	int rightP = -1;
	GetLayoutRowPages(pageNumber, leftP, rightP);
	if (leftP <= 0)
		return 0;

	int prevLeft = 0;
	int prevRight = -1;
	GetLayoutRowPages(leftP - 1, prevLeft, prevRight);
	return prevLeft;
}

int CXiaoGongPDFDlg::GetNextSpreadPage(int pageNumber) const
{
	if (m_totalPages <= 0)
		return 0;
	if (m_layoutMode == LAYOUT_SINGLE_CONTINUOUS || m_totalPages <= 1)
		return min(m_totalPages - 1, pageNumber + 1);

	int leftP = 0;
	int rightP = -1;
	GetLayoutRowPages(pageNumber, leftP, rightP);
	const int nextCandidate = (rightP >= 0) ? (rightP + 1) : (leftP + 1);
	return (nextCandidate < m_totalPages) ? nextCandidate : pageNumber;
}

bool CXiaoGongPDFDlg::GetPageViewPlacement(int pageNumber, int viewWidth,
	int& outOffsetX, int& outOffsetY, int* pOutWidth, int* pOutHeight)
{
	if (!m_doc || pageNumber < 0 || pageNumber >= m_totalPages ||
		pageNumber >= static_cast<int>(m_pageYPositions.size()) || m_uniformScale <= 0.0f)
	{
		return false;
	}

	CPDFDocument* pDoc = GetActiveDocument();
	auto getPixelSize = [&](int pIdx, int& outW, int& outH) -> bool
	{
		float wPt = 0.0f;
		float hPt = 0.0f;
		if (!pDoc || !pDoc->GetPageBounds(pIdx, wPt, hPt))
		{
			fz_rect base = {};
			if (!TryGetPageBaseBounds(pIdx, base))
				return false;
			wPt = base.x1 - base.x0;
			hPt = base.y1 - base.y0;
			const int rot = GetPageRotation(pIdx);
			if (rot == 90 || rot == 270)
				std::swap(wPt, hPt);
		}
		outW = ClampRenderDimension(wPt * m_uniformScale);
		outH = ClampRenderDimension(hPt * m_uniformScale);
		return true;
	};

	int pageW = 0;
	int pageH = 0;
	if (!getPixelSize(pageNumber, pageW, pageH))
		return false;

	int leftP = pageNumber;
	int rightP = -1;
	GetLayoutRowPages(pageNumber, leftP, rightP);

	int rowWidth = pageW;
	int offsetInRow = 0;
	if (rightP >= 0)
	{
		int wLeft = pageW;
		int hLeft = pageH;
		int wRight = pageW;
		int hRight = pageH;
		if (pageNumber == leftP)
		{
			if (!getPixelSize(rightP, wRight, hRight))
				wRight = pageW;
		}
		else
		{
			if (!getPixelSize(leftP, wLeft, hLeft))
				wLeft = pageW;
		}
		rowWidth = wLeft + SPREAD_GAP + wRight;
		offsetInRow = (pageNumber == rightP) ? (wLeft + SPREAD_GAP) : 0;
	}
	else if (m_layoutMode == LAYOUT_TWO_PAGE_SPREAD)
	{
		// 双页并排下若末尾只剩单张页面，为其保留右侧空白槽位宽度，使该页始终排在左侧栏而非居中
		rowWidth = pageW + SPREAD_GAP + pageW;
		offsetInRow = 0;
	}

	const int rowStartX = (rowWidth < viewWidth ? (viewWidth - rowWidth) / 2 :
		-min(m_scrollPositionH, rowWidth - viewWidth)) + m_panOffset.x;
	outOffsetX = rowStartX + offsetInRow;
	outOffsetY = m_pageYPositions[pageNumber] - m_scrollPosition + m_panOffset.y;
	if (pOutWidth)
		*pOutWidth = pageW;
	if (pOutHeight)
		*pOutHeight = pageH;
	return true;
}

int CXiaoGongPDFDlg::GetPageAtViewPoint(CPoint viewPt, bool allowNearestFallback)
{
	if (!m_doc || m_totalPages <= 0 || !m_pdfView.GetSafeHwnd() ||
		static_cast<int>(m_pageRowTops.size()) < m_totalPages)
	{
		return -1;
	}

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	const int viewWidth = viewRect.Width();

	const int docY = viewPt.y + m_scrollPosition - m_panOffset.y;
	const int basePage = GetPageAtPosition(docY);
	int leftP = basePage;
	int rightP = -1;
	GetLayoutRowPages(basePage, leftP, rightP);

	int lx = 0, ly = 0, lw = 0, lh = 0;
	const bool okLeft = GetPageViewPlacement(leftP, viewWidth, lx, ly, &lw, &lh);
	if (okLeft && CRect(lx, ly, lx + lw, ly + lh).PtInRect(viewPt))
		return leftP;

	int rx = 0, ry = 0, rw = 0, rh = 0;
	const bool okRight = (rightP >= 0) && GetPageViewPlacement(rightP, viewWidth, rx, ry, &rw, &rh);
	if (okRight && CRect(rx, ry, rx + rw, ry + rh).PtInRect(viewPt))
		return rightP;

	if (!allowNearestFallback)
		return -1;

	if (okLeft && okRight)
	{
		const int midX = (lx + lw + rx) / 2;
		return (viewPt.x >= midX) ? rightP : leftP;
	}
	return leftP;
}

void CXiaoGongPDFDlg::ApplyPageLayoutMode(PageLayoutMode mode)
{
	if (m_layoutMode == mode)
		return;

	ClearSelection();
	ClearActiveCaptureRect();

	const int anchorPage = max(0, min(m_currentPage, max(0, m_totalPages - 1)));
	m_layoutMode = mode;
	UpdatePageLayoutButton();

	CPDFDocument* pDoc = GetActiveDocument();
	if (pDoc)
		pDoc->SetLayoutMode(mode);

	if (m_doc && m_totalPages > 0)
	{
		if (m_zoomMode == ZOOM_CUSTOM)
			m_customZoom = 1.0f;
		CalculatePagePositions(true);

		CRect viewRect;
		m_pdfView.GetClientRect(&viewRect);
		const int maxScroll = max(0, m_totalScrollHeight - viewRect.Height());
		if (anchorPage < static_cast<int>(m_pageRowTops.size()))
			m_scrollPosition = max(0, min(m_pageRowTops[anchorPage], maxScroll));
		m_scrollPositionH = 0;
		m_currentPage = anchorPage;

		UpdateScrollBar();
		RenderVisiblePages();
		UpdatePageControls();
		if (pDoc)
		{
			SaveDialogStateToDocument(pDoc);
			SaveReadingPosition(pDoc);
		}
		UpdateStatusBar();
	}
}

// ============================================================================
// 搜索功能实现
// ============================================================================

// 后台搜索线程管理
void CXiaoGongPDFDlg::StopSearch()
{
	if (m_pSearchParams)
	{
		m_pSearchParams->cancelled.store(true);
		m_retiredSearches.push_back(m_pSearchParams);
		m_pSearchParams = nullptr;
	}
	m_isSearching = false;
}

void CXiaoGongPDFDlg::ReleaseSearchThread(PDFSearchParams* pParams)
{
	if (!pParams || !pParams->pThread)
		return;

	if (pParams->pThread->m_hThread)
		WaitForSingleObject(pParams->pThread->m_hThread, INFINITE);

	delete pParams->pThread;
	pParams->pThread = nullptr;
}

void CXiaoGongPDFDlg::CleanupSearches()
{
	StopSearch();
	while (!m_retiredSearches.empty())
	{
		PDFSearchParams* pParams = m_retiredSearches.back();
		m_retiredSearches.pop_back();
		if (pParams)
		{
			pParams->cancelled.store(true);
			ReleaseSearchThread(pParams);
			delete pParams;
		}
	}
}

void CXiaoGongPDFDlg::RefreshSearchResultsList()
{
	if (!m_searchResultsList.GetSafeHwnd())
		return;
	CString title;
	if (m_isSearching)
		title = _T("正在搜索");
	else
		title.Format(_T("搜索结果 (%d)"), static_cast<int>(m_searchMatches.size()));
	m_searchResultsHeader.SetWindowText(title);
	m_updatingSearchResultsList = true;
	m_searchResultsList.SetRedraw(FALSE);
	m_searchResultsList.DeleteAllItems();
	for (size_t i = 0; i < m_searchMatches.size(); ++i)
	{
		CString page;
		page.Format(_T("%d"), m_searchMatches[i].pageNumber + 1);
		const int row = m_searchResultsList.InsertItem(static_cast<int>(i), page);
		if (row >= 0)
			m_searchResultsList.SetItemText(row, 1,
				m_searchMatches[i].context.IsEmpty() ? _T("匹配内容") :
				m_searchMatches[i].context.GetString());
	}
	if (m_currentMatchIndex >= 0 && m_currentMatchIndex < m_searchResultsList.GetItemCount())
	{
		m_searchResultsList.SetItemState(m_currentMatchIndex,
			LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		m_searchResultsList.EnsureVisible(m_currentMatchIndex, FALSE);
	}
	m_searchResultsList.SetRedraw(TRUE);
	m_searchResultsList.Invalidate(FALSE);
	m_updatingSearchResultsList = false;
	CString count;
	count.Format(_T("%d/%d"), m_currentMatchIndex >= 0 ? m_currentMatchIndex + 1 : 0,
		static_cast<int>(m_searchMatches.size()));
	m_searchMatchCount.SetWindowText(count);
	const BOOL canNavigate = !m_isSearching && !m_searchMatches.empty();
	m_btnPrevMatch.EnableWindow(canNavigate);
	m_btnNextMatch.EnableWindow(canNavigate);
}

void CXiaoGongPDFDlg::NavigateToSearchMatch(int index)
{
	if (m_isSearching || !m_doc || index < 0 || index >= static_cast<int>(m_searchMatches.size()))
		return;
	m_currentMatchIndex = index;
	CString count;
	count.Format(_T("%d/%d"), index + 1, static_cast<int>(m_searchMatches.size()));
	m_searchMatchCount.SetWindowText(count);
	if (m_searchResultsList.GetSafeHwnd() && index < m_searchResultsList.GetItemCount())
	{
		m_updatingSearchResultsList = true;
		m_searchResultsList.SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);
		m_searchResultsList.SetItemState(index,
			LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		m_searchResultsList.EnsureVisible(index, FALSE);
		m_updatingSearchResultsList = false;
	}
	const int targetPage = m_searchMatches[index].pageNumber;
	const int prevScrollY = m_scrollPosition;
	const int prevScrollX = m_scrollPositionH;
	m_currentPage = targetPage;
	UpdatePageControls();
	ScrollToMatchVisible(targetPage, m_searchMatches[index].quad);
	// 若匹配项已在可视区域内未触发滚动，仍需重绘一次以移动当前橙色高亮框
	if (m_scrollPosition == prevScrollY && m_scrollPositionH == prevScrollX)
		RenderVisiblePages();
}

void CXiaoGongPDFDlg::OnSearchResultSelected(NMHDR* pNMHDR, LRESULT* pResult)
{
	NMLISTVIEW* pChange = reinterpret_cast<NMLISTVIEW*>(pNMHDR);
	if (!m_updatingSearchResultsList && pChange->iItem >= 0 &&
		(pChange->uNewState & LVIS_SELECTED) && !(pChange->uOldState & LVIS_SELECTED))
		NavigateToSearchMatch(pChange->iItem);
	*pResult = 0;
}

void CXiaoGongPDFDlg::OnCloseSearchResults()
{
	m_searchResultsVisible = false;
	CRect rect;
	GetClientRect(&rect);
	SendMessage(WM_SIZE, SIZE_RESTORED, MAKELPARAM(rect.Width(), rect.Height()));
}

void CXiaoGongPDFDlg::OnCloseSearchPopup()
{
	// 关闭搜索框视为退出搜索：先停掉进行中的搜索，再清除匹配项，
	// 否则关闭后页面上的文字仍保持标黄，且后台结果返回时还会重新高亮。
	StopSearch();
	ClearSearchResults();

	m_searchToolsVisible = false;
	m_searchPopup.ShowWindow(SW_HIDE);
	m_editSearch.ShowWindow(SW_HIDE);
	m_searchMatchCount.ShowWindow(SW_HIDE);
	m_btnPrevMatch.ShowWindow(SW_HIDE);
	m_btnNextMatch.ShowWindow(SW_HIDE);
	m_btnCloseSearchPopup.ShowWindow(SW_HIDE);
	m_pdfView.RedrawWindow(nullptr, nullptr,
		RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
	m_btnFind.SetFocus();
}

LRESULT CXiaoGongPDFDlg::OnSearchProgress(WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(wParam);
	UNREFERENCED_PARAMETER(lParam);
	return 0;
}

// 搜索完成消息处理
LRESULT CXiaoGongPDFDlg::OnSearchComplete(WPARAM wParam, LPARAM lParam)
{
	PDFSearchParams* pParams = (PDFSearchParams*)lParam;
	auto retired = std::find(m_retiredSearches.begin(), m_retiredSearches.end(), pParams);
	if (retired != m_retiredSearches.end())
	{
		m_retiredSearches.erase(retired);
		ReleaseSearchThread(pParams);
		delete pParams;
		FinishPendingClose();
		return 0;
	}
	if (!pParams || pParams != m_pSearchParams || wParam != m_searchTaskId)
		return 0;

	m_isSearching = false;

	// 如果搜索已被取消、窗口待关闭或用户关闭/切换了文档，忽略结果
	if (pParams->cancelled.load() || m_closePending || pParams->pDoc != GetActiveDocument() ||
		!m_doc || m_totalPages <= 0)
	{
		m_pSearchParams = nullptr;
		ReleaseSearchThread(pParams);
		delete pParams;
		FinishPendingClose();
		return 0;
	}

	// 将搜索结果转移到主对话框
	if (!pParams->failed)
		m_searchMatches = std::move(pParams->results);
	else
		m_searchMatches.clear();
	if (m_searchMatches.empty() && m_searchResultsVisible)
	{
		m_searchResultsVisible = false;
		RefreshDocumentLayout();
	}

	RebuildSearchMatchPageIndex();
	m_currentMatchIndex = m_searchMatches.empty() ? -1 : 0;
	RefreshSearchResultsList();

	if (pParams->failed)
	{
		MessageBox(_T("无法读取文档文本，请检查文件是否仍可访问。"),
			_T("搜索失败"), MB_OK | MB_ICONERROR);
	}
	else if (!m_searchMatches.empty())
	{
		NavigateToSearchMatch(0);
	}

	// 清理
	m_pSearchParams = nullptr;
	ReleaseSearchThread(pParams);
	delete pParams;

	// 刷新当前视图以移除搜索高亮（如果有的话，新结果已应用）
	RefreshCurrentView();
	FinishPendingClose();

	return 0;
}

// 清除搜索结果
void CXiaoGongPDFDlg::ClearSearchResults(bool keepPanel)
{
	const bool hadMatches = !m_searchMatches.empty();
	const bool hadPanel = m_searchResultsVisible;
	m_searchMatches.clear();
	m_searchMatchesByPage.clear();
	m_currentMatchIndex = -1;
	m_searchKeyword.Empty();
	if (!keepPanel)
		m_searchResultsVisible = false;
	RefreshSearchResultsList();
	if (!keepPanel && hadPanel && m_searchResultsList.GetSafeHwnd())
		RefreshDocumentLayout();

	if (hadMatches && m_doc && m_currentPage >= 0)
	{
		RenderVisiblePages();
	}
}

void CXiaoGongPDFDlg::RebuildSearchMatchPageIndex()
{
	m_searchMatchesByPage.clear();
	if (m_totalPages <= 0)
	{
		return;
	}

	m_searchMatchesByPage.resize(m_totalPages);
	for (size_t i = 0; i < m_searchMatches.size(); ++i)
	{
		int pageNumber = m_searchMatches[i].pageNumber;
		if (pageNumber >= 0 && pageNumber < m_totalPages)
		{
			m_searchMatchesByPage[pageNumber].push_back((int)i);
		}
	}
}

bool CXiaoGongPDFDlg::TryGetPageBaseBounds(int pageNumber, fz_rect& outBounds) const
{
	CPDFDocument* pDoc = const_cast<CXiaoGongPDFDlg*>(this)->GetActiveDocument();
	if (pDoc && pDoc->GetPageBaseBounds(pageNumber, outBounds))
	{
		return true;
	}

	if (!m_ctx || !m_doc || pageNumber < 0 || pageNumber >= m_totalPages)
	{
		return false;
	}

	fz_page* pPage = nullptr;
	bool gotBounds = false;
	fz_var(pPage);
	fz_var(gotBounds);
	CSingleLock lock(&const_cast<CXiaoGongPDFDlg*>(this)->m_renderLock, TRUE);
	fz_try(m_ctx)
	{
		pPage = fz_load_page(m_ctx, m_doc, pageNumber);
		if (pPage)
		{
			outBounds = fz_bound_page(m_ctx, pPage);
			gotBounds = true;
		}
	}
	fz_always(m_ctx)
	{
		if (pPage)
			fz_drop_page(m_ctx, pPage);
	}
	fz_catch(m_ctx)
	{
		gotBounds = false;
	}
	return gotBounds;
}

CRect CXiaoGongPDFDlg::TransformQuadToBitmapRect(const fz_quad& quad, const fz_rect& bounds,
	int rotation, float scale) const
{
	const QuadPointF points[] = {
		TransformPdfPointToBitmap(quad.ul.x, quad.ul.y, bounds, rotation, scale),
		TransformPdfPointToBitmap(quad.ur.x, quad.ur.y, bounds, rotation, scale),
		TransformPdfPointToBitmap(quad.ll.x, quad.ll.y, bounds, rotation, scale),
		TransformPdfPointToBitmap(quad.lr.x, quad.lr.y, bounds, rotation, scale)
	};

	float minX = points[0].x;
	float maxX = points[0].x;
	float minY = points[0].y;
	float maxY = points[0].y;
	for (int i = 1; i < _countof(points); ++i)
	{
		minX = min(minX, points[i].x);
		maxX = max(maxX, points[i].x);
		minY = min(minY, points[i].y);
		maxY = max(maxY, points[i].y);
	}

	return CRect((int)std::floor(minX), (int)std::floor(minY),
		(int)std::ceil(maxX), (int)std::ceil(maxY));
}

// 搜索PDF文档
void CXiaoGongPDFDlg::SearchPDF(const CString& keyword, bool caseSensitive)
{
	if (m_closePending)
		return;
	CString query(keyword);
	query.Trim();
	if (!m_doc || m_totalPages <= 0)
	{
		SetStatusBarMessage(_T("请先打开 PDF 文档"));
		return;
	}
	if (query.IsEmpty())
	{
		m_editSearch.SetFocus();
		return;
	}

	StopSearch();

	// 清除之前的搜索结果和旧高亮
	ClearSearchResults(true);

	// 保存搜索参数
	m_searchKeyword = query;
	m_searchCaseSensitive = caseSensitive;

	// 将CString转换为UTF-8
	int utf8Len = WideCharToMultiByte(CP_UTF8, 0, query, -1, NULL, 0, NULL, NULL);
	if (utf8Len <= 0)
		return;
	std::vector<char> utf8Keyword(utf8Len);
	WideCharToMultiByte(CP_UTF8, 0, query, -1, utf8Keyword.data(), utf8Len, NULL, NULL);

	// 启动后台搜索线程
	CPDFDocument* pActiveDoc = GetActiveDocument();
	if (!pActiveDoc)
	{
		MessageBox(_T("无法获取当前文档"), _T("错误"), MB_OK | MB_ICONERROR);
		return;
	}
	CStringW filePath(pActiveDoc->GetFilePath());
	int pathLen = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, NULL, 0, NULL, NULL);
	if (pathLen <= 0)
		return;
	std::vector<char> utf8Path(pathLen);
	WideCharToMultiByte(CP_UTF8, 0, filePath, -1, utf8Path.data(), pathLen, NULL, NULL);

	PDFSearchParams* pParams = new PDFSearchParams();
	pParams->notifyWnd = GetSafeHwnd();
	pParams->pDoc = pActiveDoc;
	pParams->taskId = ++m_searchTaskId;
	pParams->utf8Keyword = std::move(utf8Keyword);
	pParams->utf8Path = std::move(utf8Path);
	pParams->utf8Password = pActiveDoc->GetPassword();
	pParams->caseSensitive = caseSensitive;

	m_pSearchParams = pParams;
	m_isSearching = true;
	RefreshSearchResultsList();

	CWinThread* pSearchThread = AfxBeginThread(PDFSearchThreadProc, pParams,
		THREAD_PRIORITY_BELOW_NORMAL, 0, CREATE_SUSPENDED);
	if (pSearchThread)
	{
		pSearchThread->m_bAutoDelete = FALSE;
		pParams->pThread = pSearchThread;
		pSearchThread->ResumeThread();
	}
	else
	{
		m_isSearching = false;
		delete pParams;
		m_pSearchParams = nullptr;
		RefreshSearchResultsList();
		MessageBox(_T("无法启动搜索线程"), _T("错误"), MB_OK | MB_ICONERROR);
	}
}
// 跳转到下一个匹配项
void CXiaoGongPDFDlg::GoToNextMatch()
{
	if (m_isSearching) return;  // 搜索进行中
	if (m_searchMatches.empty())
	{
		ShowSearchTools();
		return;
	}

	NavigateToSearchMatch((m_currentMatchIndex + 1) % static_cast<int>(m_searchMatches.size()));
}

// 跳转到上一个匹配项
void CXiaoGongPDFDlg::GoToPrevMatch()
{
	if (m_isSearching) return;  // 搜索进行中
	if (m_searchMatches.empty())
	{
		ShowSearchTools();
		return;
	}

	NavigateToSearchMatch((m_currentMatchIndex - 1 + static_cast<int>(m_searchMatches.size())) %
		static_cast<int>(m_searchMatches.size()));
}

// Reveal search controls only when search is requested.
void CXiaoGongPDFDlg::ShowSearchTools()
{
	if (m_isFullscreen)
		ExitFullscreen();
	m_searchToolsVisible = true;
	m_searchPopup.ShowWindow(SW_SHOW);
	m_editSearch.ShowWindow(SW_SHOW);
	m_searchMatchCount.ShowWindow(SW_SHOW);
	m_btnPrevMatch.ShowWindow(SW_SHOW);
	m_btnNextMatch.ShowWindow(SW_SHOW);
	m_btnCloseSearchPopup.ShowWindow(SW_SHOW);
	m_searchPopup.BringWindowToTop();
	m_editSearch.BringWindowToTop();
	m_searchMatchCount.BringWindowToTop();
	m_btnPrevMatch.BringWindowToTop();
	m_btnNextMatch.BringWindowToTop();
	m_btnCloseSearchPopup.BringWindowToTop();
	m_editSearch.SetFocus();
	m_editSearch.SetSel(0, -1);
	m_searchPopup.RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
	m_searchMatchCount.RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
	m_btnPrevMatch.RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
	m_btnNextMatch.RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
	m_btnCloseSearchPopup.RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
}

// 查找按钮点击事件
void CXiaoGongPDFDlg::OnBtnFind()
{
	if (!m_searchToolsVisible)
	{
		ShowSearchTools();
		return;
	}
	if (m_isSearching) return;  // 搜索进行中，忽略重复点击

	CString keyword;
	m_editSearch.GetWindowText(keyword);
	keyword.Trim();
	if (keyword.IsEmpty())
	{
		m_editSearch.SetFocus();
		return;
	}
	if (!m_isSearching && keyword == m_searchKeyword && !m_searchMatches.empty())
	{
		GoToNextMatch();
		return;
	}

	SearchPDF(keyword, m_searchCaseSensitive);
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

// ============================================================================
// 文本选择相关实现
// ============================================================================

// 将屏幕（PDF视图客户区）坐标转换为PDF用户空间坐标
bool CXiaoGongPDFDlg::ScreenPointToPdfCoords(CPoint viewPt, int pageNumber,
	float& pdfX, float& pdfY)
{
	fz_rect bounds;
	float scale;
	int offsetX, offsetY;
	if (!GetSelectionPageTransform(pageNumber, bounds, scale, offsetX, offsetY))
		return false;

	const float pageWidth = bounds.x1 - bounds.x0;
	const float pageHeight = bounds.y1 - bounds.y0;
	const int rotation = GetPageRotation(pageNumber);
	const float effectiveWidth = (rotation == 90 || rotation == 270) ? pageHeight : pageWidth;
	const float effectiveHeight = (rotation == 90 || rotation == 270) ? pageWidth : pageHeight;
	const float localX = max(0.0f, min(effectiveWidth, (viewPt.x - offsetX) / scale));
	const float localY = max(0.0f, min(effectiveHeight, (viewPt.y - offsetY) / scale));

	switch ((rotation % 360 + 360) % 360)
	{
	case 90:
		pdfX = bounds.x0 + localY;
		pdfY = bounds.y0 + pageHeight - localX;
		break;
	case 180:
		pdfX = bounds.x0 + pageWidth - localX;
		pdfY = bounds.y0 + pageHeight - localY;
		break;
	case 270:
		pdfX = bounds.x0 + pageWidth - localY;
		pdfY = bounds.y0 + localX;
		break;
	default:
		pdfX = bounds.x0 + localX;
		pdfY = bounds.y0 + localY;
		break;
	}
	return true;
}

void CXiaoGongPDFDlg::InvalidateLinkCache()
{
	m_cachedLinksPage = -1;
	m_cachedLinksDoc = nullptr;
	m_cachedPageLinks.clear();
	ClearHoverLinkFeedback();
}

void CXiaoGongPDFDlg::EnsurePageLinksCached(int pageNumber)
{
	if (!m_ctx || !m_doc || pageNumber < 0 || pageNumber >= m_totalPages)
	{
		m_cachedLinksPage = -1;
		m_cachedLinksDoc = nullptr;
		m_cachedPageLinks.clear();
		return;
	}
	if (m_cachedLinksDoc == m_doc && m_cachedLinksPage == pageNumber)
		return;

	m_cachedLinksDoc = m_doc;
	m_cachedLinksPage = pageNumber;
	m_cachedPageLinks.clear();

	struct RawLinkEntry
	{
		fz_rect rect;
		bool isExternal;
		int destPage;
		float destX;
		float destY;
		std::string utf8Uri;
	};
	std::vector<RawLinkEntry> rawLinks;

	{
		fz_page* pPage = nullptr;
		fz_link* pLinks = nullptr;
		fz_var(pPage);
		fz_var(pLinks);
		CSingleLock lock(&m_renderLock, TRUE);
		fz_try(m_ctx)
		{
			pPage = fz_load_page(m_ctx, m_doc, pageNumber);
			pLinks = fz_load_links(m_ctx, pPage);
			for (fz_link* pLink = pLinks; pLink; pLink = pLink->next)
			{
				if (!pLink->uri || !*pLink->uri)
					continue;

				RawLinkEntry entry = {};
				entry.rect = pLink->rect;
				entry.destPage = -1;
				entry.destX = -1.0f;
				entry.destY = -1.0f;
				entry.isExternal = fz_is_external_link(m_ctx, pLink->uri) != 0;

				if (entry.isExternal)
				{
					entry.utf8Uri.assign(pLink->uri);
					rawLinks.push_back(std::move(entry));
				}
				else
				{
					int resolvedPage = -1;
					float resolvedX = -1.0f;
					float resolvedY = -1.0f;
					fz_var(resolvedPage);
					fz_var(resolvedX);
					fz_var(resolvedY);
					fz_try(m_ctx)
					{
						const fz_link_dest dest = fz_resolve_link_dest(m_ctx, m_doc, pLink->uri);
						if (dest.loc.chapter >= 0 && dest.loc.page >= 0)
						{
							const int pg = fz_page_number_from_location(m_ctx, m_doc, dest.loc);
							if (pg >= 0 && pg < m_totalPages)
							{
								resolvedPage = pg;
								if (dest.type == FZ_LINK_DEST_XYZ)
									resolvedX = dest.x;
								if (dest.type == FZ_LINK_DEST_XYZ ||
									dest.type == FZ_LINK_DEST_FIT_H ||
									dest.type == FZ_LINK_DEST_FIT_BH)
								{
									resolvedY = dest.y;
								}
							}
						}
					}
					fz_catch(m_ctx)
					{
						resolvedPage = -1;
					}
					if (resolvedPage >= 0)
					{
						entry.destPage = resolvedPage;
						entry.destX = resolvedX;
						entry.destY = resolvedY;
						rawLinks.push_back(std::move(entry));
					}
				}
			}
		}
		fz_always(m_ctx)
		{
			if (pLinks)
				fz_drop_link(m_ctx, pLinks);
			if (pPage)
				fz_drop_page(m_ctx, pPage);
		}
		fz_catch(m_ctx)
		{
			rawLinks.clear();
		}
	}

	m_cachedPageLinks.reserve(rawLinks.size());
	for (const RawLinkEntry& raw : rawLinks)
	{
		CachedPageLinkItem item;
		item.rect = raw.rect;
		if (raw.isExternal)
		{
			item.hit.kind = PdfLinkKind::External;
			const int wideLen = ::MultiByteToWideChar(CP_UTF8, 0, raw.utf8Uri.c_str(), -1, nullptr, 0);
			if (wideLen > 1)
			{
				CString wideUri;
				LPWSTR pBuf = wideUri.GetBuffer(wideLen);
				::MultiByteToWideChar(CP_UTF8, 0, raw.utf8Uri.c_str(), -1, pBuf, wideLen);
				wideUri.ReleaseBuffer();
				wideUri.Trim();
				item.hit.externalUri = wideUri;
			}
			if (!item.hit.externalUri.IsEmpty())
				m_cachedPageLinks.push_back(std::move(item));
		}
		else if (raw.destPage >= 0)
		{
			item.hit.kind = PdfLinkKind::Internal;
			item.hit.targetPage = raw.destPage;
			item.hit.targetX = raw.destX;
			item.hit.targetY = raw.destY;
			m_cachedPageLinks.push_back(std::move(item));
		}
	}
}

bool CXiaoGongPDFDlg::HitTestLink(CPoint viewPt, PdfLinkHit& outHit)
{
	outHit = PdfLinkHit{};
	if (!m_ctx || !m_doc || !m_pdfView.GetSafeHwnd() || m_totalPages <= 0)
		return false;

	const int sourcePage = GetPageAtViewPoint(viewPt, false);
	if (sourcePage < 0 || sourcePage >= m_totalPages)
		return false;

	float pdfX = 0.0f;
	float pdfY = 0.0f;
	if (!ScreenPointToPdfCoords(viewPt, sourcePage, pdfX, pdfY))
		return false;

	EnsurePageLinksCached(sourcePage);
	const fz_point pdfPoint = { pdfX, pdfY };
	for (const CachedPageLinkItem& item : m_cachedPageLinks)
	{
		if (fz_is_point_inside_rect(pdfPoint, item.rect))
		{
			outHit = item.hit;
			return outHit.kind != PdfLinkKind::None;
		}
	}
	return false;
}

void CXiaoGongPDFDlg::UpdateHoverLinkFeedback(CPoint viewPt)
{
	CString preview;
	PdfLinkHit hit;
	if (HitTestLink(viewPt, hit))
	{
		if (hit.kind == PdfLinkKind::Internal && hit.targetPage >= 0)
		{
			preview.Format(_T("链接：跳转到第 %d 页"), hit.targetPage + 1);
		}
		else if (hit.kind == PdfLinkKind::External && !hit.externalUri.IsEmpty())
		{
			CString displayUri = hit.externalUri;
			if (displayUri.GetLength() > 140)
				displayUri = displayUri.Left(137) + _T("...");
			CString lowerPrefix = hit.externalUri.Left(7);
			lowerPrefix.MakeLower();
			if (lowerPrefix == _T("mailto:"))
				preview.Format(_T("邮件链接：%s"), displayUri.GetString());
			else
				preview.Format(_T("外部链接：%s"), displayUri.GetString());
		}
	}
	else
	{
		int annotPage = -1;
		PageAnnotHitInfo annotHit;
		if (HitTestAnnotationAtViewPoint(viewPt, annotPage, annotHit))
		{
			if (!annotHit.contents.IsEmpty())
			{
				CString snippet = annotHit.contents;
				snippet.Replace(_T("\r\n"), _T(" "));
				snippet.Replace(_T("\n"), _T(" "));
				if (snippet.GetLength() > 100)
					snippet = snippet.Left(97) + _T("...");
				if (annotHit.type == PDF_ANNOT_FREE_TEXT)
				{
					preview.Format(_T("%s：%s（左键点击可直接编辑/移动/调色/调字号与字距）"),
						annotHit.typeLabel.GetString(), snippet.GetString());
				}
				else
				{
					preview.Format(_T("%s：%s（右键可编辑或删除）"),
						annotHit.typeLabel.GetString(), snippet.GetString());
				}
			}
			else
			{
				preview.Format(_T("%s（右键可删除）"), annotHit.typeLabel.GetString());
			}
		}
	}

	if (preview.IsEmpty())
	{
		ClearHoverLinkFeedback();
		return;
	}

	if (!m_hasHoverLinkStatus || m_lastHoverLinkPreview != preview)
	{
		m_hasHoverLinkStatus = true;
		m_lastHoverLinkPreview = preview;
		SetStatusBarMessage(preview);
	}
}

void CXiaoGongPDFDlg::ClearHoverLinkFeedback()
{
	if (!m_hasHoverLinkStatus)
		return;
	m_hasHoverLinkStatus = false;
	m_lastHoverLinkPreview.Empty();
	if (!m_isAreaCaptureMode && m_statusBar.GetSafeHwnd())
		SetStatusBarMessage(nullptr);
}

void CXiaoGongPDFDlg::OnPdfViewMouseLeave()
{
	ClearHoverLinkFeedback();
	bool needRedraw = false;
	if (m_hoverCaptureAction != CAPTURE_ACT_NONE)
	{
		m_hoverCaptureAction = CAPTURE_ACT_NONE;
		needRedraw = true;
	}
	if (m_hoverSelectionAction != SEL_BAR_NONE)
	{
		m_hoverSelectionAction = SEL_BAR_NONE;
		needRedraw = true;
	}
	if (m_inlineAnnot.hoverToolbarBtn != INLINE_ANNOT_BTN_NONE ||
		m_inlineAnnot.hoverColorSwatch != -1 ||
		m_inlineAnnot.hoverMoveHandle ||
		m_inlineAnnot.hoverResizeHandle ||
		m_inlineAnnot.hoverBorderMove)
	{
		m_inlineAnnot.hoverToolbarBtn = INLINE_ANNOT_BTN_NONE;
		m_inlineAnnot.hoverColorSwatch = -1;
		m_inlineAnnot.hoverMoveHandle = false;
		m_inlineAnnot.hoverResizeHandle = false;
		m_inlineAnnot.hoverBorderMove = false;
		needRedraw = true;
	}
	if (needRedraw)
		InvalidateSelection();
}

void CXiaoGongPDFDlg::OpenExternalLinkWithConfirm(const CString& externalUri)
{
	CString safeUri(externalUri);
	safeUri.Trim();
	if (safeUri.IsEmpty())
		return;

	// 过滤控制字符，防止命令行/协议注入
	for (int i = 0; i < safeUri.GetLength(); ++i)
	{
		const WCHAR ch = safeUri[i];
		if (ch < 0x20 || ch == 0x7F)
		{
			MessageBox(_T("检测到链接包含非法控制字符，已阻止打开。"),
				_T("安全提示"), MB_OK | MB_ICONWARNING);
			return;
		}
	}

	CString lowerUri(safeUri);
	lowerUri.MakeLower();
	const bool isHttp = (lowerUri.Left(7) == _T("http://"));
	const bool isHttps = (lowerUri.Left(8) == _T("https://"));
	const bool isMailto = (lowerUri.Left(7) == _T("mailto:"));

	CString displayUri = safeUri;
	if (displayUri.GetLength() > 500)
		displayUri = displayUri.Left(497) + _T("...");

	if (!isHttp && !isHttps && !isMailto)
	{
		CString blockMsg;
		blockMsg.Format(
			_T("出于安全考虑，小龚PDF阅读器仅允许打开 http / https / mailto 外部链接，已阻止以下协议地址：\n\n%s"),
			displayUri.GetString());
		MessageBox(blockMsg, _T("安全提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	CString confirmMsg;
	confirmMsg.Format(
		_T("即将调用系统默认%s打开以下%s：\n\n%s\n\n小龚PDF阅读器自身保持零联网。请确认该目标地址安全可信，是否继续打开？"),
		isMailto ? _T("邮件客户端") : _T("浏览器"),
		isMailto ? _T("邮件地址") : _T("外部链接"),
		displayUri.GetString());

	if (MessageBox(confirmMsg, _T("打开外部链接确认"),
		MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
	{
		return;
	}

	HINSTANCE hResult = ::ShellExecute(GetSafeHwnd(), _T("open"),
		safeUri, nullptr, nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(hResult) <= 32)
	{
		MessageBox(_T("无法调用系统默认程序打开该链接。"),
			_T("提示"), MB_OK | MB_ICONWARNING);
	}
}

// ============================================================================
// 框选区域高清截图与页面导出图片实现
// ============================================================================

void CXiaoGongPDFDlg::OnMenuRegionCapture()
{
	if (!m_doc || m_totalPages <= 0)
	{
		SetStatusBarMessage(_T("请先打开 PDF 文档后再使用区域截图"));
		return;
	}

	if (m_isAreaCaptureMode)
		CancelAreaCaptureMode(true);
	else
		StartAreaCaptureMode();
}

void CXiaoGongPDFDlg::StartAreaCaptureMode()
{
	if (!m_doc || m_totalPages <= 0)
		return;

	FinishInlineTextAnnot(false);
	ClearSelection();
	ClearHoverLinkFeedback();
	m_isAreaCaptureMode = true;
	m_isDraggingCaptureRect = false;
	m_hasCaptureRect = false;
	m_hoverCaptureAction = CAPTURE_ACT_NONE;
	m_pressedCaptureAction = CAPTURE_ACT_NONE;
	for (auto& r : m_captureActionRects)
		r.SetRectEmpty();

	UpdateSnapshotButtonColors();
	SetStatusBarMessage(_T("区域截图模式：在页面上拖动鼠标框选区域（空格+左键可平移页面，右键或 Esc 取消）"));
	InvalidateSelection();

	if (m_pdfView.GetSafeHwnd())
	{
		SendMessage(WM_SETCURSOR,
			reinterpret_cast<WPARAM>(m_pdfView.GetSafeHwnd()),
			MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
	}
}

void CXiaoGongPDFDlg::CancelAreaCaptureMode(bool restoreStatusBar)
{
	const bool wasActive = m_isAreaCaptureMode || m_isDraggingCaptureRect || m_hasCaptureRect;
	m_isAreaCaptureMode = false;
	m_isDraggingCaptureRect = false;
	m_hasCaptureRect = false;
	m_hoverCaptureAction = CAPTURE_ACT_NONE;
	m_pressedCaptureAction = CAPTURE_ACT_NONE;
	for (auto& r : m_captureActionRects)
		r.SetRectEmpty();

	if (wasActive && ::GetCapture() == GetSafeHwnd() && !m_isDragging && !m_isSelecting && !m_isWindowDragging)
		::ReleaseCapture();

	UpdateSnapshotButtonColors();
	if (restoreStatusBar && m_statusBar.GetSafeHwnd())
		UpdateStatusBar();
	if (wasActive)
	{
		InvalidateSelection();
		if (m_pdfView.GetSafeHwnd())
		{
			SendMessage(WM_SETCURSOR,
				reinterpret_cast<WPARAM>(m_pdfView.GetSafeHwnd()),
				MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
		}
	}
}

void CXiaoGongPDFDlg::ClearActiveCaptureRect()
{
	if (!m_isDraggingCaptureRect && !m_hasCaptureRect)
		return;
	m_isDraggingCaptureRect = false;
	m_hasCaptureRect = false;
	m_hoverCaptureAction = CAPTURE_ACT_NONE;
	m_pressedCaptureAction = CAPTURE_ACT_NONE;
	for (auto& r : m_captureActionRects)
		r.SetRectEmpty();
	if (m_isAreaCaptureMode)
		SetStatusBarMessage(_T("区域截图模式：在页面上拖动鼠标框选区域（右键或 Esc 取消）"));
	InvalidateSelection();
}

CRect CXiaoGongPDFDlg::GetNormalizedCaptureViewRect() const
{
	CRect r(
		min(m_captureStartView.x, m_captureEndView.x),
		min(m_captureStartView.y, m_captureEndView.y),
		max(m_captureStartView.x, m_captureEndView.x),
		max(m_captureStartView.y, m_captureEndView.y));
	if (m_pdfView.GetSafeHwnd())
	{
		CRect clientRect;
		m_pdfView.GetClientRect(&clientRect);
		r.IntersectRect(&r, &clientRect);
	}
	return r;
}

bool CXiaoGongPDFDlg::ComputeCapturePixelSizePreview(const CRect& viewSelRect,
	CSize& outPixelSize, int& outDpi) const
{
	outPixelSize = CSize(0, 0);
	outDpi = 300;
	if (!m_doc || m_totalPages <= 0 || viewSelRect.Width() < 2 || viewSelRect.Height() < 2)
		return false;

	auto* pSelf = const_cast<CXiaoGongPDFDlg*>(this);
	CRect unionClipped(0, 0, 0, 0);
	float sampleViewScale = m_uniformScale;
	int hitCount = 0;

	for (int p = 0; p < m_totalPages; ++p)
	{
		if (p < static_cast<int>(m_pageYPositions.size()) &&
			p < static_cast<int>(m_pageHeights.size()))
		{
			const int pageTopView = m_pageYPositions[p] - m_scrollPosition + m_panOffset.y;
			const int pageBottomView = pageTopView + m_pageHeights[p];
			if (pageBottomView < viewSelRect.top || pageTopView > viewSelRect.bottom)
				continue;
		}

		fz_rect bounds = {};
		float viewScale = 0.0f;
		int offsetX = 0;
		int offsetY = 0;
		if (!pSelf->GetSelectionPageTransform(p, bounds, viewScale, offsetX, offsetY))
			continue;

		const int rotation = pSelf->GetPageRotation(p);
		const float origW = bounds.x1 - bounds.x0;
		const float origH = bounds.y1 - bounds.y0;
		const float effW = (rotation == 90 || rotation == 270) ? origH : origW;
		const float effH = (rotation == 90 || rotation == 270) ? origW : origH;
		CRect pageRect(offsetX, offsetY,
			offsetX + ClampRenderDimension(effW * viewScale),
			offsetY + ClampRenderDimension(effH * viewScale));

		CRect inter;
		if (inter.IntersectRect(&viewSelRect, &pageRect) && !inter.IsRectEmpty())
		{
			if (hitCount == 0)
				unionClipped = inter;
			else
				unionClipped.UnionRect(&unionClipped, &inter);
			sampleViewScale = viewScale;
			++hitCount;
		}
	}

	if (hitCount == 0 || unionClipped.Width() < 2 || unionClipped.Height() < 2 || sampleViewScale <= 0.0f)
		return false;

	constexpr float MIN_SCALE = 300.0f / 72.0f;
	constexpr float MAX_SCALE = 600.0f / 72.0f;
	constexpr double MAX_CAPTURE_PIXELS = 36.0 * 1024.0 * 1024.0;

	float renderScale = min(MAX_SCALE, max(MIN_SCALE, sampleViewScale * 1.5f));
	float ratio = renderScale / sampleViewScale;
	const double estPixels = static_cast<double>(unionClipped.Width()) *
		static_cast<double>(unionClipped.Height()) * ratio * ratio;
	if (estPixels > MAX_CAPTURE_PIXELS)
	{
		ratio *= static_cast<float>(std::sqrt(MAX_CAPTURE_PIXELS / estPixels));
		renderScale = sampleViewScale * ratio;
	}

	outPixelSize.cx = max(1, static_cast<int>(std::round(unionClipped.Width() * ratio)));
	outPixelSize.cy = max(1, static_cast<int>(std::round(unionClipped.Height() * ratio)));
	outDpi = max(72, static_cast<int>(renderScale * 72.0f + 0.5f));
	return true;
}

int CXiaoGongPDFDlg::HitTestCaptureActionBar(CPoint viewPt) const
{
	if (!m_isAreaCaptureMode || !m_hasCaptureRect || m_isDraggingCaptureRect)
		return CAPTURE_ACT_NONE;
	for (int i = 0; i < 3; ++i)
	{
		if (!m_captureActionRects[i].IsRectEmpty() && m_captureActionRects[i].PtInRect(viewPt))
			return i;
	}
	return CAPTURE_ACT_NONE;
}

void CXiaoGongPDFDlg::DrawAreaCaptureOverlay(CDC* pDC, const CRect& clientRect)
{
	if (!pDC || !IsAreaCaptureOverlayVisible() || clientRect.IsRectEmpty())
		return;

	CRect selRect = GetNormalizedCaptureViewRect();
	if (selRect.IsRectEmpty())
		return;

	// 1. 绘制选区四周的半透明暗角遮罩，突出框选区域
	CDC maskDC;
	CBitmap maskBmp;
	if (maskDC.CreateCompatibleDC(pDC) && maskBmp.CreateCompatibleBitmap(pDC, 1, 1))
	{
		CBitmap* pOldMask = maskDC.SelectObject(&maskBmp);
		maskDC.FillSolidRect(0, 0, 1, 1, RGB(0, 0, 0));
		BLENDFUNCTION bf = { AC_SRC_OVER, 0, 88, 0 };

		auto dimRect = [&](int l, int t, int r, int b)
		{
			if (r > l && b > t)
				pDC->AlphaBlend(l, t, r - l, b - t, &maskDC, 0, 0, 1, 1, bf);
		};
		dimRect(clientRect.left, clientRect.top, clientRect.right, selRect.top);
		dimRect(clientRect.left, selRect.bottom, clientRect.right, clientRect.bottom);
		dimRect(clientRect.left, selRect.top, selRect.left, selRect.bottom);
		dimRect(selRect.right, selRect.top, clientRect.right, selRect.bottom);

		maskDC.SelectObject(pOldMask);
	}

	Gdiplus::Graphics graphics(pDC->GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	// 2. 绘制蓝色高亮选框及 8 个控制点锚点
	Gdiplus::Pen borderPen(Gdiplus::Color(255, 26, 115, 232), 1.8f);
	graphics.DrawRectangle(&borderPen,
		selRect.left, selRect.top, max(1, selRect.Width() - 1), max(1, selRect.Height() - 1));

	const CPoint anchors[8] = {
		{ selRect.left, selRect.top },
		{ (selRect.left + selRect.right) / 2, selRect.top },
		{ selRect.right - 1, selRect.top },
		{ selRect.right - 1, (selRect.top + selRect.bottom) / 2 },
		{ selRect.right - 1, selRect.bottom - 1 },
		{ (selRect.left + selRect.right) / 2, selRect.bottom - 1 },
		{ selRect.left, selRect.bottom - 1 },
		{ selRect.left, (selRect.top + selRect.bottom) / 2 }
	};
	Gdiplus::SolidBrush handleFill(Gdiplus::Color(255, 255, 255, 255));
	Gdiplus::Pen handlePen(Gdiplus::Color(255, 26, 115, 232), 1.2f);
	for (const CPoint& pt : anchors)
	{
		Gdiplus::Rect hr(pt.x - 3, pt.y - 3, 6, 6);
		graphics.FillRectangle(&handleFill, hr);
		graphics.DrawRectangle(&handlePen, hr);
	}

	if (!m_captureBarFont.GetSafeHandle())
	{
		m_captureBarFont.CreateFont(-13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑"));
	}

	auto drawRoundPill = [&](const CRect& rc, float radius, Gdiplus::Color fill,
		Gdiplus::Color border, bool withBorder)
	{
		if (rc.Width() <= 2 || rc.Height() <= 2)
			return;
		const float r = max(1.0f, min(radius, min(rc.Width(), rc.Height()) * 0.5f));
		const float d = r * 2.0f;
		Gdiplus::RectF rf(static_cast<Gdiplus::REAL>(rc.left) + 0.5f,
			static_cast<Gdiplus::REAL>(rc.top) + 0.5f,
			static_cast<Gdiplus::REAL>(rc.Width() - 1),
			static_cast<Gdiplus::REAL>(rc.Height() - 1));
		Gdiplus::GraphicsPath path;
		path.AddArc(rf.X, rf.Y, d, d, 180.0f, 90.0f);
		path.AddArc(rf.GetRight() - d, rf.Y, d, d, 270.0f, 90.0f);
		path.AddArc(rf.GetRight() - d, rf.GetBottom() - d, d, d, 0.0f, 90.0f);
		path.AddArc(rf.X, rf.GetBottom() - d, d, d, 90.0f, 90.0f);
		path.CloseFigure();
		Gdiplus::SolidBrush b(fill);
		graphics.FillPath(&b, &path);
		if (withBorder)
		{
			Gdiplus::Pen p(border, 1.0f);
			graphics.DrawPath(&p, &path);
		}
	};

	pDC->SetBkMode(TRANSPARENT);
	CFont* pOldFont = pDC->SelectObject(&m_captureBarFont);

	// 3. 左上角输出像素尺寸提示胶囊
	CSize pxSize(0, 0);
	int outDpi = 300;
	if (ComputeCapturePixelSizePreview(selRect, pxSize, outDpi))
	{
		CString badgeText;
		badgeText.Format(_T("%d × %d px (%d DPI)"), pxSize.cx, pxSize.cy, outDpi);
		const CSize ext = pDC->GetTextExtent(badgeText);
		const int badgeW = ext.cx + 18;
		const int badgeH = 24;
		int badgeX = max(clientRect.left + 6, min(selRect.left, clientRect.right - badgeW - 6));
		int badgeY = (selRect.top >= badgeH + 8) ? (selRect.top - badgeH - 6) : (selRect.top + 6);
		CRect badgeRect(badgeX, badgeY, badgeX + badgeW, badgeY + badgeH);
		drawRoundPill(badgeRect, 5.0f, Gdiplus::Color(220, 28, 33, 40),
			Gdiplus::Color(180, 70, 80, 95), false);
		pDC->SetTextColor(RGB(245, 248, 252));
		pDC->DrawText(badgeText, &badgeRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}

	// 4. 释放鼠标后在选框右下角显示浮动操作条：[复制截图] [保存 PNG] [取消]
	for (auto& r : m_captureActionRects)
		r.SetRectEmpty();

	if (m_hasCaptureRect && !m_isDraggingCaptureRect)
	{
		const int barH = 36;
		const int pad = 5;
		const int btnGap = 6;
		const int btnCopyW = 86;
		const int btnSaveW = 86;
		const int btnCancelW = 56;
		const int barW = pad * 2 + btnCopyW + btnSaveW + btnCancelW + btnGap * 2;

		int barX = selRect.right - barW;
		barX = max(clientRect.left + 8, min(barX, clientRect.right - barW - 8));
		int barY = selRect.bottom + 8;
		if (barY + barH + 6 > clientRect.bottom)
		{
			barY = (selRect.top >= barH + 14) ? (selRect.top - barH - 8) : (selRect.bottom - barH - 8);
		}
		barY = max(clientRect.top + 6, min(barY, clientRect.bottom - barH - 6));

		CRect barRect(barX, barY, barX + barW, barY + barH);
		const bool dark = (m_readingMode == READING_NIGHT);
		drawRoundPill(barRect, 8.0f,
			dark ? Gdiplus::Color(245, 40, 44, 50) : Gdiplus::Color(248, 255, 255, 255),
			dark ? Gdiplus::Color(255, 72, 79, 88) : Gdiplus::Color(255, 206, 218, 235),
			true);

		const int btnTop = barY + pad;
		const int btnBottom = barY + barH - pad;
		m_captureActionRects[CAPTURE_ACT_COPY].SetRect(barX + pad, btnTop,
			barX + pad + btnCopyW, btnBottom);
		m_captureActionRects[CAPTURE_ACT_SAVE].SetRect(
			m_captureActionRects[CAPTURE_ACT_COPY].right + btnGap, btnTop,
			m_captureActionRects[CAPTURE_ACT_COPY].right + btnGap + btnSaveW, btnBottom);
		m_captureActionRects[CAPTURE_ACT_CANCEL].SetRect(
			m_captureActionRects[CAPTURE_ACT_SAVE].right + btnGap, btnTop,
			m_captureActionRects[CAPTURE_ACT_SAVE].right + btnGap + btnCancelW, btnBottom);

		const LPCTSTR labels[3] = { _T("复制截图"), _T("保存 PNG"), _T("取消") };
		for (int i = 0; i < 3; ++i)
		{
			const bool hovered = (m_hoverCaptureAction == i);
			const bool pressed = (m_pressedCaptureAction == i);
			if (i == CAPTURE_ACT_COPY)
			{
				Gdiplus::Color fill = pressed ? Gdiplus::Color(255, 18, 92, 192) :
					(hovered ? Gdiplus::Color(255, 22, 105, 215) : Gdiplus::Color(255, 29, 124, 243));
				drawRoundPill(m_captureActionRects[i], 5.0f, fill, fill, false);
				pDC->SetTextColor(RGB(255, 255, 255));
			}
			else
			{
				if (hovered || pressed)
				{
					Gdiplus::Color fill = dark ?
						(pressed ? Gdiplus::Color(255, 68, 75, 85) : Gdiplus::Color(255, 56, 62, 70)) :
						(pressed ? Gdiplus::Color(255, 216, 230, 248) : Gdiplus::Color(255, 234, 242, 253));
					drawRoundPill(m_captureActionRects[i], 5.0f, fill, fill, false);
				}
				pDC->SetTextColor(dark ? RGB(228, 233, 238) : RGB(36, 44, 54));
			}
			pDC->DrawText(labels[i], &m_captureActionRects[i],
				DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
	}

	pDC->SelectObject(pOldFont);
}

bool CXiaoGongPDFDlg::RenderCapturedRegionBuffer(const CRect& viewSelRect,
	ExportBitmapBuffer& outBuffer, int& outDpi)
{
	outBuffer = ExportBitmapBuffer{};
	outDpi = 300;
	if (!m_ctx || !m_doc || m_totalPages <= 0 || viewSelRect.Width() < 2 || viewSelRect.Height() < 2)
		return false;

	struct PageSliceHit
	{
		int pageNumber;
		fz_rect bounds;
		int rotation;
		float effW;
		float effH;
		float viewScale;
		int offsetX;
		int offsetY;
		CRect inter;
	};
	std::vector<PageSliceHit> hits;
	CRect unionClipped(0, 0, 0, 0);

	for (int p = 0; p < m_totalPages; ++p)
	{
		if (p < static_cast<int>(m_pageYPositions.size()) &&
			p < static_cast<int>(m_pageHeights.size()))
		{
			const int pageTopView = m_pageYPositions[p] - m_scrollPosition + m_panOffset.y;
			const int pageBottomView = pageTopView + m_pageHeights[p];
			if (pageBottomView < viewSelRect.top || pageTopView > viewSelRect.bottom)
				continue;
		}

		fz_rect bounds = {};
		float viewScale = 0.0f;
		int offsetX = 0;
		int offsetY = 0;
		if (!GetSelectionPageTransform(p, bounds, viewScale, offsetX, offsetY))
			continue;

		const int rotation = GetPageRotation(p);
		const float origW = bounds.x1 - bounds.x0;
		const float origH = bounds.y1 - bounds.y0;
		const float effW = (rotation == 90 || rotation == 270) ? origH : origW;
		const float effH = (rotation == 90 || rotation == 270) ? origW : origH;
		CRect pageRect(offsetX, offsetY,
			offsetX + ClampRenderDimension(effW * viewScale),
			offsetY + ClampRenderDimension(effH * viewScale));

		CRect inter;
		if (inter.IntersectRect(&viewSelRect, &pageRect) && !inter.IsRectEmpty())
		{
			if (hits.empty())
				unionClipped = inter;
			else
				unionClipped.UnionRect(&unionClipped, &inter);
			hits.push_back({ p, bounds, rotation, effW, effH, viewScale, offsetX, offsetY, inter });
		}
	}

	if (hits.empty() || unionClipped.Width() < 2 || unionClipped.Height() < 2)
		return false;

	constexpr float MIN_SCALE = 300.0f / 72.0f;
	constexpr float MAX_SCALE = 600.0f / 72.0f;
	constexpr double MAX_CAPTURE_PIXELS = 36.0 * 1024.0 * 1024.0;

	const float baseViewScale = max(0.01f, hits.front().viewScale);
	float renderScale = min(MAX_SCALE, max(MIN_SCALE, baseViewScale * 1.5f));
	float ratio = renderScale / baseViewScale;
	const double estPixels = static_cast<double>(unionClipped.Width()) *
		static_cast<double>(unionClipped.Height()) * ratio * ratio;
	if (estPixels > MAX_CAPTURE_PIXELS)
	{
		ratio *= static_cast<float>(std::sqrt(MAX_CAPTURE_PIXELS / estPixels));
		renderScale = baseViewScale * ratio;
	}
	outDpi = max(72, static_cast<int>(renderScale * 72.0f + 0.5f));

	auto renderSubPage = [&](const PageSliceHit& h, ExportBitmapBuffer& sliceBuf) -> bool
	{
		const float ptLeft = max(0.0f, min(h.effW, (h.inter.left - h.offsetX) / h.viewScale));
		const float ptTop = max(0.0f, min(h.effH, (h.inter.top - h.offsetY) / h.viewScale));
		const float ptRight = max(ptLeft, min(h.effW, (h.inter.right - h.offsetX) / h.viewScale));
		const float ptBottom = max(ptTop, min(h.effH, (h.inter.bottom - h.offsetY) / h.viewScale));

		const int cropX = static_cast<int>(std::floor(ptLeft * renderScale));
		const int cropY = static_cast<int>(std::floor(ptTop * renderScale));
		const int cropW = max(1, static_cast<int>(std::ceil(ptRight * renderScale)) - cropX);
		const int cropH = max(1, static_cast<int>(std::ceil(ptBottom * renderScale)) - cropY);
		const int stride = cropW * 4;

		try
		{
			sliceBuf.bgra.assign(static_cast<size_t>(stride) * static_cast<size_t>(cropH), 0xFF);
		}
		catch (...)
		{
			return false;
		}
		sliceBuf.width = cropW;
		sliceBuf.height = cropH;
		sliceBuf.stride = stride;

		BYTE* pSamples = sliceBuf.bgra.data();
		bool rendered = false;
		fz_var(rendered);

		CSingleLock lock(&m_renderLock, TRUE);
		fz_page* pPage = nullptr;
		fz_pixmap* pPixmap = nullptr;
		fz_device* pDev = nullptr;
		fz_var(pPage);
		fz_var(pPixmap);
		fz_var(pDev);
		fz_try(m_ctx)
		{
			pPage = fz_load_page(m_ctx, m_doc, h.pageNumber);
			if (!pPage)
				fz_throw(m_ctx, FZ_ERROR_GENERIC, "Failed to load page for region capture");
			pPixmap = fz_new_pixmap_with_data(m_ctx, fz_device_bgr(m_ctx),
				cropW, cropH, nullptr, 1, stride, pSamples);
			fz_clear_pixmap_with_value(m_ctx, pPixmap, 0xff);
			fz_matrix ctm = BuildPageRenderMatrix(h.bounds, renderScale, h.rotation);
			ctm = fz_concat(ctm, fz_translate(static_cast<float>(-cropX), static_cast<float>(-cropY)));
			pDev = fz_new_draw_device(m_ctx, ctm, pPixmap);
			fz_run_page(m_ctx, pPage, pDev, fz_identity, nullptr);
			fz_close_device(m_ctx, pDev);
			rendered = true;
		}
		fz_always(m_ctx)
		{
			if (pDev)
				fz_drop_device(m_ctx, pDev);
			if (pPixmap)
				fz_drop_pixmap(m_ctx, pPixmap);
			if (pPage)
				fz_drop_page(m_ctx, pPage);
		}
		fz_catch(m_ctx)
		{
			rendered = false;
		}
		return rendered;
	};

	// 单页框选（绝大多数场景）：直接输出零拷贝切片缓冲区
	if (hits.size() == 1)
	{
		return renderSubPage(hits.front(), outBuffer);
	}

	// 跨页框选：合成所有命中页的高清局部渲染切片
	const int canvasW = max(1, static_cast<int>(std::round(unionClipped.Width() * ratio)));
	const int canvasH = max(1, static_cast<int>(std::round(unionClipped.Height() * ratio)));
	const int canvasStride = canvasW * 4;
	try
	{
		outBuffer.bgra.assign(static_cast<size_t>(canvasStride) * static_cast<size_t>(canvasH), 0xFF);
	}
	catch (...)
	{
		return false;
	}
	outBuffer.width = canvasW;
	outBuffer.height = canvasH;
	outBuffer.stride = canvasStride;

	for (const PageSliceHit& h : hits)
	{
		ExportBitmapBuffer slice;
		if (!renderSubPage(h, slice))
			return false;

		const int dstX = max(0, static_cast<int>(std::round((h.inter.left - unionClipped.left) * ratio)));
		const int dstY = max(0, static_cast<int>(std::round((h.inter.top - unionClipped.top) * ratio)));
		const int copyW = min(slice.width, canvasW - dstX);
		const int copyH = min(slice.height, canvasH - dstY);
		for (int row = 0; row < copyH; ++row)
		{
			BYTE* pDst = outBuffer.bgra.data() +
				static_cast<size_t>(dstY + row) * canvasStride +
				static_cast<size_t>(dstX) * 4;
			const BYTE* pSrc = slice.bgra.data() + static_cast<size_t>(row) * slice.stride;
			std::memcpy(pDst, pSrc, static_cast<size_t>(copyW) * 4);
		}
	}
	return true;
}

void CXiaoGongPDFDlg::CopyCapturedRegionToClipboard()
{
	if (!m_isAreaCaptureMode || !m_hasCaptureRect)
		return;

	const CRect selRect = GetNormalizedCaptureViewRect();
	ExportBitmapBuffer buffer;
	int outDpi = 300;
	if (!RenderCapturedRegionBuffer(selRect, buffer, outDpi))
	{
		MessageBox(_T("渲染所选区域失败，请重试。"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	if (!CopyBgrxBufferToClipboard(GetSafeHwnd(), buffer.bgra.data(),
		buffer.width, buffer.height, buffer.stride))
	{
		MessageBox(_T("无法将截图写入剪贴板。"), _T("错误"), MB_OK | MB_ICONERROR);
		return;
	}

	CancelAreaCaptureMode(false);
	CString msg;
	msg.Format(_T("已复制高清区域截图到剪贴板 (%d×%d px, %d DPI)"),
		buffer.width, buffer.height, outDpi);
	SetStatusBarMessage(msg);
}

void CXiaoGongPDFDlg::SaveCapturedRegionAsPng()
{
	if (!m_isAreaCaptureMode || !m_hasCaptureRect)
		return;

	const CRect selRect = GetNormalizedCaptureViewRect();
	ExportBitmapBuffer buffer;
	int outDpi = 300;
	if (!RenderCapturedRegionBuffer(selRect, buffer, outDpi))
	{
		MessageBox(_T("渲染所选区域失败，请重试。"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}

	CString baseName(_T("PDF截图"));
	CString initialDir;
	if (CPDFDocument* pDoc = GetActiveDocument())
	{
		const CString fullPath = pDoc->GetFilePath();
		const int slash = max(fullPath.ReverseFind(_T('\\')), fullPath.ReverseFind(_T('/')));
		if (slash > 0)
			initialDir = fullPath.Left(slash);
		CString fileName = pDoc->GetFileName();
		const int dot = fileName.ReverseFind(_T('.'));
		if (dot > 0)
			fileName = fileName.Left(dot);
		if (!fileName.IsEmpty())
			baseName = fileName;
	}

	CString defaultName;
	defaultName.Format(_T("%s_第%d页_区域截图.png"), baseName.GetString(), m_currentPage + 1);
	if (!initialDir.IsEmpty())
		defaultName = initialDir + _T("\\") + defaultName;

	ScopedFileWatchSuspend suspendWatch(this);
	CFileDialog saveDlg(FALSE, _T("png"), defaultName,
		OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
		_T("PNG 高清无损图片 (*.png)|*.png||"), this);
	if (saveDlg.DoModal() != IDOK)
		return;

	const CString savePath = saveDlg.GetPathName();
	if (!SaveBgrxBufferToImageFile(buffer.bgra.data(), buffer.width, buffer.height,
		buffer.stride, savePath, ExportImageFormat::Png, outDpi))
	{
		MessageBox(_T("保存 PNG 图片失败，请检查目标路径权限与磁盘空间。"),
			_T("保存失败"), MB_OK | MB_ICONERROR);
		return;
	}

	CancelAreaCaptureMode(false);
	CString msg;
	msg.Format(_T("高清区域截图已保存：%s (%d×%d px)"),
		savePath.GetString(), buffer.width, buffer.height);
	SetStatusBarMessage(msg);
}

bool CXiaoGongPDFDlg::RenderDocumentPageBuffer(int pageNumber, float targetScale,
	double maxPixels, ExportBitmapBuffer& outBuffer)
{
	outBuffer = ExportBitmapBuffer{};
	if (!m_ctx || !m_doc || pageNumber < 0 || pageNumber >= m_totalPages || targetScale <= 0.0f)
		return false;

	fz_rect bounds = {};
	if (!TryGetPageBaseBounds(pageNumber, bounds))
		return false;

	const int rotation = GetPageRotation(pageNumber);
	const float origW = bounds.x1 - bounds.x0;
	const float origH = bounds.y1 - bounds.y0;
	const float effW = max(1.0f, (rotation == 90 || rotation == 270) ? origH : origW);
	const float effH = max(1.0f, (rotation == 90 || rotation == 270) ? origW : origH);

	float scale = targetScale;
	const double estPixels = static_cast<double>(effW) * static_cast<double>(effH) * scale * scale;
	if (maxPixels > 0.0 && estPixels > maxPixels)
		scale *= static_cast<float>(std::sqrt(maxPixels / estPixels));

	const int width = ClampRenderDimension(effW * scale);
	const int height = ClampRenderDimension(effH * scale);
	const int stride = width * 4;

	try
	{
		outBuffer.bgra.assign(static_cast<size_t>(stride) * static_cast<size_t>(height), 0xFF);
	}
	catch (...)
	{
		return false;
	}
	outBuffer.width = width;
	outBuffer.height = height;
	outBuffer.stride = stride;

	BYTE* pSamples = outBuffer.bgra.data();
	bool rendered = false;
	fz_var(rendered);

	CSingleLock lock(&m_renderLock, TRUE);
	fz_page* pPage = nullptr;
	fz_pixmap* pPixmap = nullptr;
	fz_device* pDev = nullptr;
	fz_var(pPage);
	fz_var(pPixmap);
	fz_var(pDev);
	fz_try(m_ctx)
	{
		pPage = fz_load_page(m_ctx, m_doc, pageNumber);
		if (!pPage)
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "Failed to load page for image export");
		pPixmap = fz_new_pixmap_with_data(m_ctx, fz_device_bgr(m_ctx),
			width, height, nullptr, 1, stride, pSamples);
		fz_clear_pixmap_with_value(m_ctx, pPixmap, 0xff);
		const fz_matrix ctm = BuildPageRenderMatrix(bounds, scale, rotation);
		pDev = fz_new_draw_device(m_ctx, ctm, pPixmap);
		fz_run_page(m_ctx, pPage, pDev, fz_identity, nullptr);
		fz_close_device(m_ctx, pDev);
		rendered = true;
	}
	fz_always(m_ctx)
	{
		if (pDev)
			fz_drop_device(m_ctx, pDev);
		if (pPixmap)
			fz_drop_pixmap(m_ctx, pPixmap);
		if (pPage)
			fz_drop_page(m_ctx, pPage);
	}
	fz_catch(m_ctx)
	{
		rendered = false;
	}
	return rendered;
}

void CXiaoGongPDFDlg::CopyCurrentPageAsImage()
{
	if (!m_doc || m_totalPages <= 0 || m_currentPage < 0 || m_currentPage >= m_totalPages)
		return;

	ExportBitmapBuffer buffer;
	constexpr float SCALE_300_DPI = 300.0f / 72.0f;
	constexpr double MAX_PAGE_PIXELS = 32.0 * 1024.0 * 1024.0;
	if (!RenderDocumentPageBuffer(m_currentPage, SCALE_300_DPI, MAX_PAGE_PIXELS, buffer))
	{
		MessageBox(_T("渲染当前页面图片失败。"), _T("错误"), MB_OK | MB_ICONERROR);
		return;
	}

	if (CopyBgrxBufferToClipboard(GetSafeHwnd(), buffer.bgra.data(),
		buffer.width, buffer.height, buffer.stride))
	{
		CString msg;
		msg.Format(_T("已将第 %d 页高清图片复制到剪贴板 (%d×%d px)"),
			m_currentPage + 1, buffer.width, buffer.height);
		SetStatusBarMessage(msg);
	}
	else
	{
		MessageBox(_T("无法将页面图片复制到剪贴板。"), _T("错误"), MB_OK | MB_ICONERROR);
	}
}

void CXiaoGongPDFDlg::OnMenuExportImages()
{
	if (!m_doc || m_totalPages <= 0)
	{
		MessageBox(_T("请先打开 PDF 文档"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	CancelAreaCaptureMode(false);
	ScopedFileWatchSuspend suspendFileWatch(this);

	CPDFDocument* pActiveDoc = GetActiveDocument();
	const CString docPath = pActiveDoc ? pActiveDoc->GetFilePath() : CString();

	CString summary;
	const bool exported = RunExportPagesToImages(
		this,
		m_totalPages,
		max(0, min(m_currentPage, m_totalPages - 1)),
		docPath,
		[this, pActiveDoc](int pageIndex, float& outWidthPt, float& outHeightPt) -> bool
		{
			if (pActiveDoc && pActiveDoc->GetPageBounds(pageIndex, outWidthPt, outHeightPt))
				return true;
			fz_rect bounds = {};
			if (!TryGetPageBaseBounds(pageIndex, bounds))
				return false;
			outWidthPt = bounds.x1 - bounds.x0;
			outHeightPt = bounds.y1 - bounds.y0;
			const int rot = GetPageRotation(pageIndex);
			if (rot == 90 || rot == 270)
				std::swap(outWidthPt, outHeightPt);
			return true;
		},
		[this](int pageIndex, float scale, double maxPixels, ExportBitmapBuffer& outBuffer) -> bool
		{
			return RenderDocumentPageBuffer(pageIndex, scale, maxPixels, outBuffer);
		},
		&summary);

	if (exported && !summary.IsEmpty())
		SetStatusBarMessage(summary);
}

// 清除当前选择
void CXiaoGongPDFDlg::ClearSelection()
{
	if (m_selectionTextPage && m_ctx)
	{
		CSingleLock lock(&m_renderLock, TRUE);
		fz_drop_stext_page(m_ctx, m_selectionTextPage);
	}
	m_selectionTextPage = nullptr;
	m_selectionQuads.clear();
	m_selectedText.Empty();
	m_hasSelection = false;
	m_isSelecting = false;
	m_selPageNumber = -1;
	m_selStartView = CPoint(0, 0);
	m_selEndView = CPoint(0, 0);
	m_hoverSelectionAction = SEL_BAR_NONE;
	m_pressedSelectionAction = SEL_BAR_NONE;
	for (auto& rc : m_selectionActionRects)
		rc.SetRectEmpty();
}

// 重绘视图以更新选择高亮
void CXiaoGongPDFDlg::InvalidateSelection()
{
	if (m_pdfView.GetSafeHwnd())
		m_pdfView.Invalidate(FALSE);
}

// 计算页面在视图中的实际缩放与偏移（与 RenderVisiblePages 单页/双页/封面布局完全同源）
bool CXiaoGongPDFDlg::GetSelectionPageTransform(int pageNumber, fz_rect& bounds,
	float& scale, int& offsetX, int& offsetY)
{
	if (!TryGetPageBaseBounds(pageNumber, bounds))
		return false;
	if (pageNumber < 0 || pageNumber >= (int)m_pageYPositions.size())
		return false;

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	scale = m_uniformScale;
	if (!GetPageViewPlacement(pageNumber, viewRect.Width(), offsetX, offsetY))
		return false;
	return scale > 0.0f;
}

void CXiaoGongPDFDlg::UpdateSelectedText()
{
	m_selectionQuads.clear();
	m_selectedText.Empty();
	if (!m_selectionTextPage || !m_ctx || m_selPageNumber < 0)
		return;

	float startX, startY, endX, endY;
	if (!ScreenPointToPdfCoords(m_selStartView, m_selPageNumber, startX, startY) ||
		!ScreenPointToPdfCoords(m_selEndView, m_selPageNumber, endX, endY))
		return;

	const fz_point start = { startX, startY };
	const fz_point end = { endX, endY };
	m_selectionQuads.resize(4096);
	int count = 0;
	char* utf8 = nullptr;
	bool succeeded = false;
	fz_var(count);
	fz_var(utf8);
	fz_var(succeeded);

	{
		CSingleLock lock(&m_renderLock, TRUE);
		fz_try(m_ctx)
		{
			count = fz_highlight_selection(m_ctx, m_selectionTextPage,
				start, end, m_selectionQuads.data(), (int)m_selectionQuads.size());
			if (!m_isSelecting && count > 0)
			{
				utf8 = fz_copy_selection(m_ctx, m_selectionTextPage, start, end, 1);
			}
			succeeded = true;
		}
		fz_catch(m_ctx)
		{
			if (utf8)
			{
				fz_free(m_ctx, utf8);
				utf8 = nullptr;
			}
			succeeded = false;
		}
	}

	if (!succeeded)
	{
		m_selectionQuads.clear();
		m_selectedText.Empty();
		return;
	}

	m_selectionQuads.resize(max(0, min(count, (int)m_selectionQuads.size())));
	if (utf8)
	{
		m_selectedText = CA2W(utf8, CP_UTF8);
		CSingleLock lock(&m_renderLock, TRUE);
		fz_free(m_ctx, utf8);
	}
}

// 绘制与文字行贴合的选择高亮
void CXiaoGongPDFDlg::UpdateSelectionHighlight(CDC* pDC, int pageNumber)
{
	if ((!m_hasSelection && !m_isSelecting) || pageNumber != m_selPageNumber ||
		m_selectionQuads.empty())
		return;

	fz_rect bounds;
	float scale;
	int offsetX, offsetY;
	if (!GetSelectionPageTransform(pageNumber, bounds, scale, offsetX, offsetY))
		return;

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	CDC colorDC;
	CBitmap colorBitmap;
	if (!colorDC.CreateCompatibleDC(pDC) || !colorBitmap.CreateCompatibleBitmap(pDC, 1, 1))
		return;
	CBitmap* oldBitmap = colorDC.SelectObject(&colorBitmap);
	colorDC.FillSolidRect(0, 0, 1, 1, RGB(51, 153, 255));
	BLENDFUNCTION blend = { AC_SRC_OVER, 0, 110, 0 };

	for (const fz_quad& quad : m_selectionQuads)
	{
		CRect rect = TransformQuadToBitmapRect(quad, bounds, GetPageRotation(pageNumber), scale);
		rect.OffsetRect(offsetX, offsetY);
		CRect clipped;
		if (clipped.IntersectRect(&rect, &viewRect) && !clipped.IsRectEmpty())
			pDC->AlphaBlend(clipped.left, clipped.top, clipped.Width(), clipped.Height(),
				&colorDC, 0, 0, 1, 1, blend);
	}
	colorDC.SelectObject(oldBitmap);
}

// 提取选中文字
CString CXiaoGongPDFDlg::ExtractSelectedText()
{
	return m_hasSelection ? m_selectedText : CString();
}

bool CXiaoGongPDFDlg::CopyTextToClipboard(const CString& text)
{
	if (text.IsEmpty())
		return false;

	if (!::OpenClipboard(GetSafeHwnd()))
		return false;
	::EmptyClipboard();

	bool copied = false;
	const size_t byteSize = (text.GetLength() + 1) * sizeof(WCHAR);
	HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE, byteSize);
	if (hGlobal)
	{
		void* pData = ::GlobalLock(hGlobal);
		if (pData)
		{
			memcpy(pData, (LPCTSTR)text, byteSize);
			::GlobalUnlock(hGlobal);
			if (::SetClipboardData(CF_UNICODETEXT, hGlobal))
			{
				hGlobal = nullptr;
				copied = true;
			}
		}
		if (hGlobal)
			::GlobalFree(hGlobal);
	}
	::CloseClipboard();
	return copied;
}

// 复制选中文本到剪贴板
void CXiaoGongPDFDlg::CopySelectionToClipboard()
{
	CopyTextToClipboard(ExtractSelectedText());
}

// ============================================================================
// PDF后台加载相关实现
// ============================================================================

// 后台加载线程函数
UINT CXiaoGongPDFDlg::PDFLoadThreadProc(LPVOID pParam)
{
	PDFLoadParams* pParams = (PDFLoadParams*)pParam;
	if (!pParams)
		return 1;

	// 执行PDF加载
	if (!pParams->cancelled.load())
		pParams->openResult = pParams->pDoc->OpenDocument(
			pParams->utf8Path.data(),
			pParams->utf8Password.empty() ? nullptr : pParams->utf8Password.data(),
			&pParams->cancelled);
	pParams->success = pParams->openResult == PDFOpenResult::Success;
	if (!pParams->success)
	{
		pParams->errorMsg = _T("无法打开PDF文件");
	}

	// 通知主线程加载完成
	if (pParams->notifyWnd)
		::PostMessage(pParams->notifyWnd, WM_PDF_LOAD_COMPLETE, 0, (LPARAM)pParams);

	return 0;
}

LRESULT CXiaoGongPDFDlg::OnPDFLoadCancelled(WPARAM wParam, LPARAM)
{
	for (PDFLoadParams* pParams : m_pendingLoads)
	{
		if (pParams->pProgressDlg &&
			pParams->pProgressDlg->GetSafeHwnd() == reinterpret_cast<HWND>(wParam))
		{
			pParams->cancelled.store(true);
			break;
		}
	}
	return 0;
}

// PDF加载完成消息处理
LRESULT CXiaoGongPDFDlg::OnPDFLoadComplete(WPARAM wParam, LPARAM lParam)
{
	PDFLoadParams* pParams = (PDFLoadParams*)lParam;
	if (!pParams || std::find(m_pendingLoads.begin(), m_pendingLoads.end(), pParams) == m_pendingLoads.end())
	{
		return 0;
	}
	if (m_closePending)
	{
		CleanupPendingLoad(pParams);
		FinishPendingClose();
		return 0;
	}
	if (pParams->pProgressDlg && static_cast<CProgressDlg*>(pParams->pProgressDlg)->IsCancelled())
		pParams->cancelled.store(true);
	if (pParams->cancelled.load())
	{
		CleanupPendingLoad(pParams);
		UpdateStatusBar();
		OpenNextRestoredTab();
		return 0;
	}

	// 检查加载结果
	if (!pParams->success)
	{
		if (pParams->openResult == PDFOpenResult::PasswordRequired ||
			pParams->openResult == PDFOpenResult::InvalidPassword)
		{
			const bool invalidPassword = pParams->openResult == PDFOpenResult::InvalidPassword;
			ReleaseLoadThread(pParams);
			CloseProgressDialog(pParams);
			if (!pParams->utf8Password.empty())
			{
				SecureZeroMemory(pParams->utf8Password.data(), pParams->utf8Password.size());
				pParams->utf8Password.clear();
			}

			CString fileName = pParams->filePath.Mid(
				max(pParams->filePath.ReverseFind(_T('\\')),
					pParams->filePath.ReverseFind(_T('/'))) + 1);
			CPasswordDialog passwordDlg(fileName, invalidPassword, this);
			if (passwordDlg.DoModal() != IDOK)
			{
				CleanupPendingLoad(pParams);
				UpdateStatusBar();
				OpenNextRestoredTab();
				return 0;
			}
			if (m_closePending)
			{
				CleanupPendingLoad(pParams);
				FinishPendingClose();
				return 0;
			}

			const CString& password = passwordDlg.GetPassword();
			const int length = WideCharToMultiByte(CP_UTF8, 0, password, -1,
				nullptr, 0, nullptr, nullptr);
			if (length <= 0)
			{
				CleanupPendingLoad(pParams);
				MessageBox(_T("无法读取密码"), _T("错误"), MB_OK | MB_ICONERROR);
				OpenNextRestoredTab();
				return 0;
			}
			pParams->utf8Password.resize(length);
			if (!WideCharToMultiByte(CP_UTF8, 0, password, -1,
				pParams->utf8Password.data(), length, nullptr, nullptr))
			{
				CleanupPendingLoad(pParams);
				MessageBox(_T("无法读取密码"), _T("错误"), MB_OK | MB_ICONERROR);
				OpenNextRestoredTab();
				return 0;
			}

			CProgressDlg* pProgressDlg = new CProgressDlg(this);
			if (!pProgressDlg->Create(IDD_PROGRESS_DIALOG, this))
			{
				delete pProgressDlg;
				CleanupPendingLoad(pParams);
				MessageBox(_T("无法创建加载窗口"), _T("错误"), MB_OK | MB_ICONERROR);
				OpenNextRestoredTab();
				return 0;
			}
			pParams->pProgressDlg = pProgressDlg;
			pProgressDlg->ShowWindow(SW_SHOW);
			pProgressDlg->UpdateWindow();
			pParams->openResult = PDFOpenResult::Failed;
			pParams->success = false;
			pParams->pThread = AfxBeginThread(PDFLoadThreadProc, pParams,
				THREAD_PRIORITY_NORMAL, 0, CREATE_SUSPENDED);
			if (!pParams->pThread)
			{
				CleanupPendingLoad(pParams);
				MessageBox(_T("无法启动加载线程"), _T("错误"), MB_OK | MB_ICONERROR);
				OpenNextRestoredTab();
				return 0;
			}
			pParams->pThread->m_bAutoDelete = FALSE;
			pParams->pThread->ResumeThread();
			return 0;
		}
		CString errMsg = pParams->errorMsg;
		CleanupPendingLoad(pParams);

		MessageBox(errMsg, _T("错误"), MB_OK | MB_ICONERROR);
		UpdateStatusBar();
		OpenNextRestoredTab();
		return 0;
	}

	// 加载成功，恢复此文件上次的阅读位置。
	RestoreReadingPosition(pParams->pDoc);
	// Keep restored tabs in saved order even when workers finish out of order.
	int newIndex = (int)m_documents.size();
	if (pParams->restoringSession)
	{
		auto requested = std::find_if(m_sessionRestorePaths.begin(), m_sessionRestorePaths.end(),
			[pParams](const CString& path) {
				return FullPathForComparison(path).CompareNoCase(pParams->filePath) == 0;
			});
		for (int i = 0; i < (int)m_documents.size(); ++i)
		{
			auto existing = std::find_if(m_sessionRestorePaths.begin(), m_sessionRestorePaths.end(),
				[this, i](const CString& path) {
					return FullPathForComparison(path).CompareNoCase(m_documents[i]->GetFilePath()) == 0;
				});
			if (existing > requested)
			{
				newIndex = i;
				break;
			}
		}
	}
	m_documents.insert(m_documents.begin() + newIndex, pParams->pDoc);
	if (m_activeDocIndex >= newIndex)
		++m_activeDocIndex;

	// 添加标签页
	if (m_tabCtrl.GetSafeHwnd())
	{
		// ★★★ 如果是第一个文档，确保标签页控件可见
		if (newIndex == 0 && m_documents.size() == 1)
		{
			m_tabCtrl.ShowWindow(SW_SHOW);
		}

		TCITEM tci = {0};
		tci.mask = TCIF_TEXT | TCIF_PARAM;
		tci.lParam = reinterpret_cast<LPARAM>(pParams->pDoc);
		CString fullFileName = pParams->pDoc->GetFileName();
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

		tci.pszText = (LPTSTR)(LPCTSTR)tabTitle;
		m_tabCtrl.InsertItem(newIndex, &tci);
		UpdateTabWidth();
		if (!pParams->restoringSession || m_activeDocIndex < 0)
			m_tabCtrl.SetCurSel(newIndex);
		else
			m_tabCtrl.SetCurSel(m_activeDocIndex);

		// ★★★ 强制刷新标签页显示，避免显示异常
		m_tabCtrl.RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN);
	}

	if (!pParams->restoringSession || m_activeDocIndex < 0)
		SwitchToDocument(newIndex);

	// 更新状态栏
	UpdateStatusBar();

	// ★★★ 添加到最近文件列表
	AddRecentFile(pParams->filePath);

	// 注册文件变化监听
	m_fileWatcher.AddWatch(pParams->filePath, GetSafeHwnd());
	if (!m_fileWatcher.IsRunning())
		m_fileWatcher.Start();

	// ★★★ 强制刷新显示，确保PDF页面立即显示
	// 使用FALSE参数避免擦除背景，减少高刷新率显示器上的闪烁
	Invalidate(FALSE);
	UpdateWindow();

	// 清理
	pParams->pDoc = nullptr;
	ReleaseLoadThread(pParams);
	CloseProgressDialog(pParams);
	auto pendingIt = std::find(m_pendingLoads.begin(), m_pendingLoads.end(), pParams);
	if (pendingIt != m_pendingLoads.end())
		m_pendingLoads.erase(pendingIt);
	delete pParams;
	OpenNextRestoredTab();

	return 0;
}

void CXiaoGongPDFDlg::FlushDeferredFileChanges()
{
	if (m_deferredFileChanges.empty() || m_closePending || !GetSafeHwnd())
	{
		m_deferredFileChanges.clear();
		return;
	}

	std::vector<CString> pending = std::move(m_deferredFileChanges);
	m_deferredFileChanges.clear();
	for (const CString& path : pending)
	{
		CString* pPath = new CString(path);
		if (!PostMessage(WM_FILE_CHANGED, 0, reinterpret_cast<LPARAM>(pPath)))
			delete pPath;
	}
}

LRESULT CXiaoGongPDFDlg::OnFileChanged(WPARAM wParam, LPARAM lParam)
{
	CString* pPath = reinterpret_cast<CString*>(lParam);
	if (!pPath)
		return 0;

	CString changedPath = FullPathForComparison(*pPath);
	delete pPath;

	if (m_closePending)
		return 0;

	if (m_fileWatchSuspendCount > 0)
	{
		bool alreadyQueued = false;
		for (const CString& queued : m_deferredFileChanges)
		{
			if (queued.CompareNoCase(changedPath) == 0)
			{
				alreadyQueued = true;
				break;
			}
		}
		if (!alreadyQueued)
			m_deferredFileChanges.push_back(changedPath);
		return 0;
	}

	for (int i = 0; i < (int)m_documents.size(); ++i)
	{
		CPDFDocument* pDoc = m_documents[i];
		if (!pDoc || pDoc->GetFilePath().IsEmpty())
			continue;

		if (FullPathForComparison(pDoc->GetFilePath()).CompareNoCase(changedPath) == 0 &&
			pDoc->IsFileModified())
		{
			// 先刷新记录的修改时间，防止模态 MessageBox 期间重入弹出重复提示框
			pDoc->UpdateLastWriteTime();

			CString msg;
			msg.Format(_T("文件 \"%s\" 已被外部程序修改，是否重新加载？"),
				pDoc->GetFileName().GetString());
			int ret = MessageBox(msg, _T("文件已修改"), MB_YESNO | MB_ICONQUESTION);
			if (ret == IDYES)
			{
				if (std::find(m_documents.begin(), m_documents.end(), pDoc) != m_documents.end())
					ReloadDocument(pDoc);
			}
			break;
		}
	}
	return 0;
}

void CXiaoGongPDFDlg::ReloadDocument(CPDFDocument* pDoc)
{
	if (!pDoc || pDoc->GetFilePath().IsEmpty())
		return;

	int docIndex = -1;
	for (int i = 0; i < (int)m_documents.size(); ++i)
	{
		if (m_documents[i] == pDoc)
		{
			docIndex = i;
			break;
		}
	}
	if (docIndex < 0)
		return;

	const bool isActive = (docIndex == m_activeDocIndex);

	if (isActive || (m_pSearchParams && m_pSearchParams->pDoc == pDoc))
		StopSearch();

	if (isActive)
	{
		FinishInlineTextAnnot(true);
		SaveDialogStateToDocument(pDoc);
		ClearSelection();
		CancelAreaCaptureMode(false);
		InvalidateLinkCache();
		m_pdfView.SetDisplayBitmap(NULL);
		SafeDeleteBitmap(m_hContinuousViewBitmap);
		CleanupBitmap();
		m_pageYPositions.clear();
		m_pageHeights.clear();
		m_pageRowTops.clear();
		m_pageRowHeights.clear();
		m_doc = nullptr;
	}

	// 保存阅读状态
	int savedPage = pDoc->GetCurrentPage();
	ZoomMode savedZoomMode = pDoc->GetZoomMode();
	float savedCustomZoom = pDoc->GetCustomZoom();
	float savedUniformScale = pDoc->GetUniformScale();
	int savedScroll = pDoc->GetScrollPosition();
	CPoint savedPan = pDoc->GetPanOffset();
	PageLayoutMode savedLayoutMode = pDoc->GetLayoutMode();
	std::map<int, int> savedRotations = pDoc->GetPageRotations();

	CString filePath = pDoc->GetFilePath();
	std::vector<char> password = pDoc->GetPassword();
	const char* pwd = password.empty() ? nullptr : password.data();

	int utf8Len = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, NULL, 0, NULL, NULL);
	std::vector<char> utf8Path(max(1, utf8Len), 0);
	WideCharToMultiByte(CP_UTF8, 0, filePath, -1, utf8Path.data(), utf8Len, NULL, NULL);

	pDoc->CloseDocument();
	PDFOpenResult result = pDoc->OpenDocument(utf8Path.data(), pwd);
	if (!password.empty())
		SecureZeroMemory(password.data(), password.size());

	if (result != PDFOpenResult::Success)
	{
#ifdef _DEBUG
		TRACE(_T("ReloadDocument: 重新打开文档失败\n"));
#endif
		CloseDocument(docIndex, false);
		MessageBox(_T("重新加载PDF文件失败，文件可能已被占用、加密更改或损坏。"),
			_T("重新加载失败"), MB_OK | MB_ICONERROR);
		return;
	}

	// 恢复阅读状态
	int totalPages = pDoc->GetTotalPages();
	if (savedPage >= totalPages)
		savedPage = totalPages - 1;
	if (savedPage < 0)
		savedPage = 0;

	pDoc->SetCurrentPage(savedPage);
	pDoc->RestoreReadingZoom(savedZoomMode, savedCustomZoom, savedUniformScale);
	pDoc->SetScrollPosition(savedScroll);
	pDoc->SetPanOffset(savedPan);
	pDoc->SetLayoutMode(savedLayoutMode);
	pDoc->SetSearchMatches(std::vector<SearchMatch>());
	pDoc->SetCurrentMatchIndex(-1);

	for (auto& pair : savedRotations)
	{
		if (pair.first >= 0 && pair.first < totalPages)
			pDoc->SetPageRotation(pair.first, pair.second);
	}

	if (isActive)
	{
		LoadDialogStateFromDocument(pDoc);
		UpdateDocumentDirtyTitle();
		ClearSearchResults();
		if (m_uniformScale > 0.0f)
			CalculatePagePositions(m_zoomMode == ZOOM_FIT_WIDTH || m_zoomMode == ZOOM_FIT_PAGE);
		else
			CalculatePagePositions(true);
		UpdateScrollBar();
		RenderVisiblePages();

		if (m_outlineVisible)
			UpdateOutlineTree();
		else if (m_outlineTree.GetSafeHwnd())
			m_outlineTree.DeleteAllItems();
		UpdatePageControls();
		UpdateZoomStatus();
		UpdateStatusBar();
	}
}

// ============================================================================
// 文档属性面板、单双页排版切换、轻量级 PDF 批注与原文件保存
// ============================================================================

void CXiaoGongPDFDlg::OnMenuDocProperties()
{
	CPDFDocument* pDoc = GetActiveDocument();
	if (!m_doc || !pDoc || m_totalPages <= 0)
	{
		MessageBox(_T("请先打开 PDF 文档"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	SaveDialogStateToDocument(pDoc);
	ScopedFileWatchSuspend suspendWatch(this);
	CPDFPropertiesDialog dlg(pDoc, max(0, min(m_currentPage, m_totalPages - 1)), &m_renderLock, this);
	dlg.DoModal();
}

void CXiaoGongPDFDlg::OnMenuPageLayout(UINT nID)
{
	if (nID < ID_MENU_LAYOUT_SINGLE || nID > ID_MENU_LAYOUT_TWO_PAGE)
		return;
	ApplyPageLayoutMode(static_cast<PageLayoutMode>(nID - ID_MENU_LAYOUT_SINGLE));
}

void CXiaoGongPDFDlg::OnBtnPageLayout()
{
	if (!m_doc || m_totalPages <= 0)
		return;

	const PageLayoutMode nextMode = (m_layoutMode == LAYOUT_TWO_PAGE_SPREAD) ?
		LAYOUT_SINGLE_CONTINUOUS : LAYOUT_TWO_PAGE_SPREAD;
	ApplyPageLayoutMode(nextMode);
}

void CXiaoGongPDFDlg::OnBtnAnnotate()
{
	CMenu menu;
	if (!menu.CreatePopupMenu())
		return;

	const UINT docFlag = (m_doc && m_totalPages > 0) ? 0 : MF_GRAYED;
	const UINT selFlag = (m_doc && m_hasSelection && !m_selectionQuads.empty()) ? 0 : MF_GRAYED;
	menu.AppendMenu(MF_STRING | selFlag, ID_MENU_ANNOT_HIGHLIGHT, _T("高亮选中文本\tCtrl+H"));
	menu.AppendMenu(MF_STRING | selFlag, ID_MENU_ANNOT_UNDERLINE, _T("下划线选中文本\tCtrl+U"));
	menu.AppendMenu(MF_STRING | selFlag, ID_MENU_ANNOT_STRIKEOUT, _T("删除线选中文本"));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING | docFlag, ID_MENU_ANNOT_TEXT_NOTE, _T("添加文字批注\tCtrl+T"));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING | docFlag, ID_MENU_SAVE_PDF, _T("保存批注与旋转回原 PDF\tCtrl+S"));
	menu.AppendMenu(MF_STRING | docFlag, ID_MENU_SAVE_PDF_AS, _T("另存为新 PDF..."));

	CRect btnRect;
	m_btnAnnotate.GetWindowRect(&btnRect);
	const UINT cmd = static_cast<UINT>(menu.TrackPopupMenu(
		TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
		btnRect.left, btnRect.bottom, this));
	if (cmd != 0)
		PostMessage(WM_COMMAND, MAKEWPARAM(cmd, 0));
}

void CXiaoGongPDFDlg::ApplySelectionMarkupAnnotation(
	enum pdf_annot_type annotType, COLORREF color, LPCTSTR actionName)
{
	CPDFDocument* pDoc = GetActiveDocument();
	if (!m_doc || !pDoc || m_totalPages <= 0)
	{
		SetStatusBarMessage(_T("请先打开 PDF 文档"));
		return;
	}
	if (!m_hasSelection || m_selectionQuads.empty() ||
		m_selPageNumber < 0 || m_selPageNumber >= m_totalPages)
	{
		CString msg;
		msg.Format(_T("请先在页面上用鼠标拖动选中文本，再执行“%s”"), actionName);
		SetStatusBarMessage(msg);
		return;
	}

	const int pageNumber = m_selPageNumber;
	const std::vector<fz_quad> quads = m_selectionQuads;
	bool ok = false;
	{
		CSingleLock lock(&m_renderLock, TRUE);
		ok = AddQuadMarkupAnnotation(pDoc, pageNumber, annotType, quads, color);
	}
	if (!ok)
	{
		MessageBox(_T("无法在当前文档中写入批注，请确认该文件为可编辑的 PDF 格式。"),
			_T("批注失败"), MB_OK | MB_ICONWARNING);
		return;
	}

	ClearSelection();
	UpdateDocumentDirtyTitle();
	RenderVisiblePages();

	CString tip;
	tip.Format(_T("已添加%s批注（按 Ctrl+S 保存写回原 PDF，右键点击批注可删除）"), actionName);
	SetStatusBarMessage(tip);
}

void CXiaoGongPDFDlg::OnMenuAnnotHighlight()
{
	ApplySelectionMarkupAnnotation(PDF_ANNOT_HIGHLIGHT, RGB(255, 226, 64), _T("高亮"));
}

void CXiaoGongPDFDlg::OnMenuAnnotUnderline()
{
	ApplySelectionMarkupAnnotation(PDF_ANNOT_UNDERLINE, RGB(26, 115, 232), _T("下划线"));
}

void CXiaoGongPDFDlg::OnMenuAnnotStrikeout()
{
	ApplySelectionMarkupAnnotation(PDF_ANNOT_STRIKE_OUT, RGB(220, 45, 45), _T("删除线"));
}

void CXiaoGongPDFDlg::PromptAddTextNoteAt(int pageNumber, float fitzX, float fitzY,
	const CString& defaultText, bool fromToolbarMenu)
{
	StartInlineTextAnnotAt(pageNumber, fitzX, fitzY, defaultText, fromToolbarMenu);
}

void CXiaoGongPDFDlg::OnMenuAnnotTextNote()
{
	CPDFDocument* pDoc = GetActiveDocument();
	if (!m_doc || !pDoc || m_totalPages <= 0)
	{
		MessageBox(_T("请先打开 PDF 文档"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	// 若当前选中了文本，默认锚定在选区首行左上方；否则默认放置在当前页可视区域居中偏上位置
	if (m_hasSelection && !m_selectionQuads.empty() &&
		m_selPageNumber >= 0 && m_selPageNumber < m_totalPages)
	{
		const fz_quad& q = m_selectionQuads.front();
		const int selPage = m_selPageNumber;
		ClearSelection();
		PromptAddTextNoteAt(selPage, q.ul.x, max(4.0f, q.ul.y - 22.0f), CString(), false);
		return;
	}

	int targetPage = max(0, min(m_currentPage, m_totalPages - 1));
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	CPoint defaultViewPt(max(80, viewRect.Width() * 2 / 5), max(80, viewRect.Height() / 3));
	const int pageAtPt = GetPageAtViewPoint(defaultViewPt, true);
	if (pageAtPt >= 0 && pageAtPt < m_totalPages)
		targetPage = pageAtPt;

	float fitzX = 72.0f;
	float fitzY = 72.0f;
	if (!ScreenPointToPdfCoords(defaultViewPt, targetPage, fitzX, fitzY))
	{
		fz_rect bounds = {};
		if (TryGetPageBaseBounds(targetPage, bounds))
		{
			fitzX = bounds.x0 + (bounds.x1 - bounds.x0) * 0.25f;
			fitzY = bounds.y0 + (bounds.y1 - bounds.y0) * 0.22f;
		}
	}
	PromptAddTextNoteAt(targetPage, fitzX, fitzY, CString(), true);
}

void CXiaoGongPDFDlg::OnEnChangeInlineAnnotEdit()
{
	if (!m_inlineAnnot.active)
		return;
	m_inlineAnnot.justCreatedFromMenu = false;
	m_inlineAnnot.caretVisible = true;
	InvalidateSelection();
}

void CXiaoGongPDFDlg::StartInlineTextAnnotAt(int pageNumber, float fitzX, float fitzY,
	const CString& initialText, bool fromToolbarMenu)
{
	CPDFDocument* pDoc = GetActiveDocument();
	if (!m_doc || !pDoc || pageNumber < 0 || pageNumber >= m_totalPages)
		return;

	if (m_inlineAnnot.active)
		FinishInlineTextAnnot(false);

	CancelAreaCaptureMode(false);
	ClearSelection();

	fz_rect bounds = {};
	if (!TryGetPageBaseBounds(pageNumber, bounds))
		return;

	const int rot = GetPageRotation(pageNumber);
	const float origW = bounds.x1 - bounds.x0;
	const float origH = bounds.y1 - bounds.y0;
	const float effW = max(60.0f, (rot == 90 || rot == 270) ? origH : origW);
	const float effH = max(60.0f, (rot == 90 || rot == 270) ? origW : origH);

	const QuadPointF rotPt = TransformPdfPointToBitmap(fitzX, fitzY, bounds, rot, 1.0f);

	m_inlineAnnot = InlineTextAnnotState{};
	m_inlineAnnot.active = true;
	m_inlineAnnot.pageNumber = pageNumber;
	m_inlineAnnot.existingAnnotIndex = -1;
	m_inlineAnnot.color = m_defaultAnnotTextColor;
	m_inlineAnnot.fontSizePt = m_defaultAnnotFontSizePt;
	m_inlineAnnot.charSpacingPt = m_defaultAnnotCharSpacingPt;
	m_inlineAnnot.widthPt = min(effW - 8.0f, max(125.0f, m_inlineAnnot.fontSizePt * 9.5f));
	m_inlineAnnot.userResizedWidth = false;
	m_inlineAnnot.localXPt = max(4.0f, min(rotPt.x, max(4.0f, effW - m_inlineAnnot.widthPt - 4.0f)));
	m_inlineAnnot.localYPt = max(4.0f, min(rotPt.y, max(4.0f, effH - m_inlineAnnot.fontSizePt * 2.0f - 4.0f)));
	m_inlineAnnot.justCreatedFromMenu = fromToolbarMenu;
	m_inlineAnnot.caretVisible = true;

	if (m_inlineAnnotEdit.GetSafeHwnd())
	{
		m_inlineAnnotEdit.SetWindowText(initialText);
		m_inlineAnnotEdit.ShowWindow(SW_SHOWNOACTIVATE);
		m_inlineAnnotEdit.SetFocus();
		const int len = initialText.GetLength();
		m_inlineAnnotEdit.SetSel(len, len);
	}

	SetTimer(TIMER_ID_ANNOT_CARET, 530, nullptr);
	SetStatusBarMessage(_T("文字批注编辑中：直接输入文字，拖左侧蓝条移动，拖右侧圆点调宽，点 ✕ 或按 Esc 完成"));
	InvalidateSelection();
}

void CXiaoGongPDFDlg::StartInlineTextAnnotFromExisting(int pageNumber, const PageAnnotHitInfo& hitInfo)
{
	CPDFDocument* pDoc = GetActiveDocument();
	if (!m_doc || !pDoc || pageNumber < 0 || pageNumber >= m_totalPages ||
		hitInfo.annotIndex < 0 || hitInfo.type != PDF_ANNOT_FREE_TEXT)
	{
		return;
	}

	if (m_inlineAnnot.active)
		FinishInlineTextAnnot(false);

	CancelAreaCaptureMode(false);
	ClearSelection();

	fz_rect bounds = {};
	if (!TryGetPageBaseBounds(pageNumber, bounds))
		return;

	const int rot = GetPageRotation(pageNumber);
	const fz_quad annotQuad = fz_quad_from_rect(hitInfo.fitzBounds);
	const QuadPointF pts[4] = {
		TransformPdfPointToBitmap(annotQuad.ul.x, annotQuad.ul.y, bounds, rot, 1.0f),
		TransformPdfPointToBitmap(annotQuad.ur.x, annotQuad.ur.y, bounds, rot, 1.0f),
		TransformPdfPointToBitmap(annotQuad.ll.x, annotQuad.ll.y, bounds, rot, 1.0f),
		TransformPdfPointToBitmap(annotQuad.lr.x, annotQuad.lr.y, bounds, rot, 1.0f)
	};
	float minX = pts[0].x, maxX = pts[0].x;
	float minY = pts[0].y, maxY = pts[0].y;
	for (int i = 1; i < 4; ++i)
	{
		minX = min(minX, pts[i].x);
		maxX = max(maxX, pts[i].x);
		minY = min(minY, pts[i].y);
		maxY = max(maxY, pts[i].y);
	}

	{
		CSingleLock lock(&m_renderLock, TRUE);
		SetPageAnnotationEditingHidden(pDoc, pageNumber, hitInfo.annotIndex, true);
	}
	RenderVisiblePages();

	m_inlineAnnot = InlineTextAnnotState{};
	m_inlineAnnot.active = true;
	m_inlineAnnot.pageNumber = pageNumber;
	m_inlineAnnot.existingAnnotIndex = hitInfo.annotIndex;
	m_inlineAnnot.color = hitInfo.textColor;
	m_inlineAnnot.fontSizePt = hitInfo.fontSizePt;
	m_inlineAnnot.charSpacingPt = hitInfo.charSpacingPt;
	m_inlineAnnot.localXPt = max(1.0f, minX);
	m_inlineAnnot.localYPt = max(1.0f, minY);
	m_inlineAnnot.widthPt = max(56.0f, maxX - minX);
	m_inlineAnnot.userResizedWidth = true;
	m_inlineAnnot.caretVisible = true;

	m_defaultAnnotTextColor = m_inlineAnnot.color;
	m_defaultAnnotFontSizePt = m_inlineAnnot.fontSizePt;
	m_defaultAnnotCharSpacingPt = m_inlineAnnot.charSpacingPt;

	if (m_inlineAnnotEdit.GetSafeHwnd())
	{
		m_inlineAnnotEdit.SetWindowText(hitInfo.contents);
		m_inlineAnnotEdit.ShowWindow(SW_SHOWNOACTIVATE);
		m_inlineAnnotEdit.SetFocus();
		const int len = hitInfo.contents.GetLength();
		m_inlineAnnotEdit.SetSel(len, len);
	}

	SetTimer(TIMER_ID_ANNOT_CARET, 530, nullptr);
	SetStatusBarMessage(_T("编辑文字批注：可移动位置、修改文字/颜色/字号/文字间距、删除或点 ✕ 完成退出"));
	InvalidateSelection();
}

void CXiaoGongPDFDlg::FinishInlineTextAnnot(bool discard)
{
	if (!m_inlineAnnot.active)
		return;

	const InlineTextAnnotState st = m_inlineAnnot;
	m_inlineAnnot = InlineTextAnnotState{};
	KillTimer(TIMER_ID_ANNOT_CARET);

	CString finalText;
	if (m_inlineAnnotEdit.GetSafeHwnd())
	{
		m_inlineAnnotEdit.GetWindowText(finalText);
		m_inlineAnnotEdit.SetWindowText(_T(""));
		m_inlineAnnotEdit.ShowWindow(SW_HIDE);
	}
	if (GetSafeHwnd() && !IsEditControlFocused())
		SetFocus();

	CPDFDocument* pDoc = GetActiveDocument();
	if (!pDoc || !pDoc->IsOpen() || st.pageNumber < 0 || st.pageNumber >= pDoc->GetTotalPages())
	{
		InvalidateSelection();
		return;
	}

	CString trimmed = finalText;
	trimmed.Trim();

	if (discard)
	{
		if (st.existingAnnotIndex >= 0)
		{
			CSingleLock lock(&m_renderLock, TRUE);
			SetPageAnnotationEditingHidden(pDoc, st.pageNumber, st.existingAnnotIndex, false);
		}
		if (m_doc && m_pdfView.GetSafeHwnd())
			RenderVisiblePages();
		return;
	}

	if (trimmed.IsEmpty())
	{
		if (st.existingAnnotIndex >= 0)
		{
			CSingleLock lock(&m_renderLock, TRUE);
			DeletePageAnnotationAt(pDoc, st.pageNumber, st.existingAnnotIndex);
			lock.Unlock();
			UpdateDocumentDirtyTitle();
			RenderVisiblePages();
			SetStatusBarMessage(_T("文字为空，已移除该文字批注（按 Ctrl+S 保存写回原 PDF）"));
		}
		else
		{
			UpdateStatusBar();
			InvalidateSelection();
		}
		return;
	}

	fz_rect bounds = {};
	if (!pDoc->GetPageBaseBounds(st.pageNumber, bounds) && !TryGetPageBaseBounds(st.pageNumber, bounds))
	{
		if (st.existingAnnotIndex >= 0)
		{
			CSingleLock lock(&m_renderLock, TRUE);
			SetPageAnnotationEditingHidden(pDoc, st.pageNumber, st.existingAnnotIndex, false);
		}
		RenderVisiblePages();
		return;
	}

	const int rot = GetPageRotation(st.pageNumber);
	const float origW = bounds.x1 - bounds.x0;
	const float origH = bounds.y1 - bounds.y0;
	const float effW = max(20.0f, (rot == 90 || rot == 270) ? origH : origW);
	const float effH = max(20.0f, (rot == 90 || rot == 270) ? origW : origH);

	std::vector<FreeTextVisualLine> wrappedLines;
	LayoutFreeTextVisualLines(nullptr, nullptr, 1.0f, finalText, st.fontSizePt, st.charSpacingPt,
		max(24.0f, st.widthPt - 8.0f), wrappedLines);
	const float boxW = max(32.0f, min(st.widthPt + 4.0f, effW - st.localXPt));
	const float boxH = max(st.fontSizePt * 1.45f + 6.0f,
		min(effH - st.localYPt,
			static_cast<float>(wrappedLines.size()) * st.fontSizePt * 1.32f + 8.0f));

	auto rotPtToFitz = [&](float rx, float ry, float& outFx, float& outFy)
	{
		switch ((rot % 360 + 360) % 360)
		{
		case 90:
			outFx = bounds.x0 + ry;
			outFy = bounds.y0 + origH - rx;
			break;
		case 180:
			outFx = bounds.x0 + origW - rx;
			outFy = bounds.y0 + origH - ry;
			break;
		case 270:
			outFx = bounds.x0 + origW - ry;
			outFy = bounds.y0 + rx;
			break;
		default:
			outFx = bounds.x0 + rx;
			outFy = bounds.y0 + ry;
			break;
		}
	};

	float fx[4] = {}, fy[4] = {};
	rotPtToFitz(st.localXPt, st.localYPt, fx[0], fy[0]);
	rotPtToFitz(st.localXPt + boxW, st.localYPt, fx[1], fy[1]);
	rotPtToFitz(st.localXPt, st.localYPt + boxH, fx[2], fy[2]);
	rotPtToFitz(st.localXPt + boxW, st.localYPt + boxH, fx[3], fy[3]);

	float minFx = fx[0], maxFx = fx[0], minFy = fy[0], maxFy = fy[0];
	for (int i = 1; i < 4; ++i)
	{
		minFx = min(minFx, fx[i]);
		maxFx = max(maxFx, fx[i]);
		minFy = min(minFy, fy[i]);
		maxFy = max(maxFy, fy[i]);
	}
	const fz_rect fitzRect = fz_make_rect(
		max(bounds.x0, minFx), max(bounds.y0, minFy),
		min(bounds.x1, maxFx), min(bounds.y1, maxFy));

	bool ok = false;
	{
		CSingleLock lock(&m_renderLock, TRUE);
		ok = UpsertFreeTextAnnotation(pDoc, st.pageNumber, st.existingAnnotIndex, fitzRect,
			finalText, st.color, st.fontSizePt, st.charSpacingPt);
		if (!ok && st.existingAnnotIndex >= 0)
			SetPageAnnotationEditingHidden(pDoc, st.pageNumber, st.existingAnnotIndex, false);
	}

	if (ok)
	{
		UpdateDocumentDirtyTitle();
		RenderVisiblePages();
		SetStatusBarMessage(_T("已保存文字批注到页面（点击文字可再次移动或修改，按 Ctrl+S 写回原 PDF）"));
	}
	else
	{
		RenderVisiblePages();
		MessageBox(_T("无法在当前文档中写入文字批注，请确认该文件为可编辑的 PDF 格式。"),
			_T("批注失败"), MB_OK | MB_ICONWARNING);
	}
}

void CXiaoGongPDFDlg::DeleteActiveInlineTextAnnot()
{
	if (!m_inlineAnnot.active)
		return;

	const int pageNum = m_inlineAnnot.pageNumber;
	const int existingIdx = m_inlineAnnot.existingAnnotIndex;
	m_inlineAnnot = InlineTextAnnotState{};
	KillTimer(TIMER_ID_ANNOT_CARET);

	if (m_inlineAnnotEdit.GetSafeHwnd())
	{
		m_inlineAnnotEdit.SetWindowText(_T(""));
		m_inlineAnnotEdit.ShowWindow(SW_HIDE);
	}
	if (GetSafeHwnd())
		SetFocus();

	CPDFDocument* pDoc = GetActiveDocument();
	if (pDoc && pDoc->IsOpen() && existingIdx >= 0 && pageNum >= 0 && pageNum < pDoc->GetTotalPages())
	{
		bool deleted = false;
		{
			CSingleLock lock(&m_renderLock, TRUE);
			deleted = DeletePageAnnotationAt(pDoc, pageNum, existingIdx);
		}
		if (deleted)
		{
			UpdateDocumentDirtyTitle();
			RenderVisiblePages();
			SetStatusBarMessage(_T("已删除文字批注（按 Ctrl+S 保存写回原 PDF）"));
			return;
		}
	}

	UpdateStatusBar();
	InvalidateSelection();
}

bool CXiaoGongPDFDlg::HitTestInlineAnnotMoveBorder(CPoint viewPt) const
{
	if (!IsInlineTextAnnotVisible() || m_inlineAnnot.boxViewRect.IsRectEmpty())
		return false;

	CRect outer = m_inlineAnnot.boxViewRect;
	outer.InflateRect(3, 3);
	if (!outer.PtInRect(viewPt))
		return false;

	CRect resizeHit = m_inlineAnnot.resizeHandleViewRect;
	if (!resizeHit.IsRectEmpty())
	{
		resizeHit.InflateRect(5, 5);
		if (resizeHit.PtInRect(viewPt))
			return false;
	}

	CRect inner = m_inlineAnnot.boxViewRect;
	inner.DeflateRect(4, 4);
	return !inner.PtInRect(viewPt);
}

int CXiaoGongPDFDlg::HitTestInlineAnnotToolbar(CPoint viewPt) const
{
	if (!IsInlineTextAnnotVisible())
		return INLINE_ANNOT_BTN_NONE;
	for (int i = 0; i < INLINE_ANNOT_BTN_COUNT; ++i)
	{
		if (!m_inlineAnnot.toolbarBtnRects[i].IsRectEmpty() &&
			m_inlineAnnot.toolbarBtnRects[i].PtInRect(viewPt))
		{
			return i;
		}
	}
	return INLINE_ANNOT_BTN_NONE;
}

int CXiaoGongPDFDlg::HitTestInlineAnnotColorSwatch(CPoint viewPt) const
{
	if (!IsInlineTextAnnotVisible() || !m_inlineAnnot.colorPopupOpen)
		return -1;
	for (int i = 0; i < INLINE_ANNOT_SWATCH_COUNT; ++i)
	{
		if (!m_inlineAnnot.colorSwatchRects[i].IsRectEmpty() &&
			m_inlineAnnot.colorSwatchRects[i].PtInRect(viewPt))
		{
			return i;
		}
	}
	return -1;
}

int CXiaoGongPDFDlg::HitTestInlineAnnotCharIndex(CPoint viewPt)
{
	if (!IsInlineTextAnnotVisible() || !m_inlineAnnotEdit.GetSafeHwnd())
		return 0;

	CString fullText;
	m_inlineAnnotEdit.GetWindowText(fullText);
	if (fullText.IsEmpty())
		return 0;

	fz_rect bounds = {};
	float scale = 0.0f;
	int offsetX = 0, offsetY = 0;
	if (!GetSelectionPageTransform(m_inlineAnnot.pageNumber, bounds, scale, offsetX, offsetY) ||
		scale <= 0.01f)
	{
		return fullText.GetLength();
	}

	CClientDC dc(&m_pdfView);
	const int fontPx = max(8, static_cast<int>(std::round(m_inlineAnnot.fontSizePt * scale)));
	CFont font;
	font.CreateFont(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, 0,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑"));

	std::vector<FreeTextVisualLine> lines;
	LayoutFreeTextVisualLines(dc.GetSafeHdc(), static_cast<HFONT>(font.GetSafeHandle()), scale,
		fullText, m_inlineAnnot.fontSizePt, m_inlineAnnot.charSpacingPt,
		max(20.0f, m_inlineAnnot.widthPt - 8.0f), lines);
	if (lines.empty())
		return 0;

	const float padXPt = 4.0f;
	const float padYPt = 3.0f;
	const float charSpacingPx = m_inlineAnnot.charSpacingPt * scale;
	const float lineStepPx = max(10.0f, m_inlineAnnot.fontSizePt * 1.32f * scale);
	const int textTopPx = offsetY + static_cast<int>(std::round((m_inlineAnnot.localYPt + padYPt) * scale));
	const int textLeftPx = offsetX + static_cast<int>(std::round((m_inlineAnnot.localXPt + padXPt) * scale));

	int lineIdx = static_cast<int>(std::floor((viewPt.y - textTopPx) / lineStepPx));
	lineIdx = max(0, min(static_cast<int>(lines.size()) - 1, lineIdx));

	const FreeTextVisualLine& ln = lines[lineIdx];
	const int relX = viewPt.x - textLeftPx;
	if (relX <= 0 || ln.text.IsEmpty())
		return ln.startChar;

	HFONT hFont = static_cast<HFONT>(font.GetSafeHandle());
	int bestOffset = ln.text.GetLength();
	int prevW = 0;
	for (int i = 1; i <= ln.text.GetLength(); ++i)
	{
		if (i < ln.text.GetLength() && ln.text[i - 1] >= 0xD800 && ln.text[i - 1] <= 0xDBFF)
			continue;
		const int w = MeasureTextWithCharSpacingPx(dc.GetSafeHdc(), hFont, ln.text, i, charSpacingPx);
		if (relX <= (prevW + w) / 2)
		{
			bestOffset = i - ((i >= 2 && ln.text[i - 2] >= 0xD800 && ln.text[i - 2] <= 0xDBFF) ? 2 : 1);
			break;
		}
		prevW = w;
	}
	return ln.startChar + bestOffset;
}

bool CXiaoGongPDFDlg::HandleInlineAnnotKeyNav(UINT vk, bool shift)
{
	if (!IsInlineTextAnnotVisible() || !m_inlineAnnotEdit.GetSafeHwnd())
		return false;

	CString fullText;
	m_inlineAnnotEdit.GetWindowText(fullText);
	int selStart = 0, selEnd = 0;
	m_inlineAnnotEdit.GetSel(selStart, selEnd);

	fz_rect bounds = {};
	float scale = 0.0f;
	int offsetX = 0, offsetY = 0;
	if (!GetSelectionPageTransform(m_inlineAnnot.pageNumber, bounds, scale, offsetX, offsetY) ||
		scale <= 0.01f)
	{
		return false;
	}

	CClientDC dc(&m_pdfView);
	const int fontPx = max(8, static_cast<int>(std::round(m_inlineAnnot.fontSizePt * scale)));
	CFont font;
	font.CreateFont(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, 0,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑"));

	HFONT hFont = static_cast<HFONT>(font.GetSafeHandle());
	const float charSpacingPx = m_inlineAnnot.charSpacingPt * scale;

	std::vector<FreeTextVisualLine> lines;
	LayoutFreeTextVisualLines(dc.GetSafeHdc(), hFont, scale,
		fullText, m_inlineAnnot.fontSizePt, m_inlineAnnot.charSpacingPt,
		max(20.0f, m_inlineAnnot.widthPt - 8.0f), lines);
	if (lines.empty())
		return false;

	int curLine = 0;
	for (int i = 0; i < static_cast<int>(lines.size()); ++i)
	{
		if (selEnd >= lines[i].startChar && selEnd <= lines[i].endChar)
		{
			curLine = i;
			const bool softWrappedNext = (i + 1 < static_cast<int>(lines.size())) &&
				(lines[i + 1].startChar == lines[i].endChar) &&
				(selEnd == lines[i].endChar);
			if (!softWrappedNext)
				break;
		}
	}

	int newCaret = selEnd;
	if (vk == VK_HOME)
	{
		newCaret = lines[curLine].startChar;
	}
	else if (vk == VK_END)
	{
		newCaret = lines[curLine].endChar;
	}
	else if (vk == VK_UP || vk == VK_DOWN)
	{
		const int targetLine = (vk == VK_UP) ? max(0, curLine - 1) :
			min(static_cast<int>(lines.size()) - 1, curLine + 1);
		if (targetLine == curLine)
			return true;

		const int colChars = max(0, min(lines[curLine].text.GetLength(), selEnd - lines[curLine].startChar));
		const int curX = (colChars > 0) ?
			MeasureTextWithCharSpacingPx(dc.GetSafeHdc(), hFont, lines[curLine].text, colChars, charSpacingPx) : 0;

		const FreeTextVisualLine& dst = lines[targetLine];
		int bestOff = dst.text.GetLength();
		int prevW = 0;
		for (int i = 1; i <= dst.text.GetLength(); ++i)
		{
			if (i < dst.text.GetLength() && dst.text[i - 1] >= 0xD800 && dst.text[i - 1] <= 0xDBFF)
				continue;
			const int w = MeasureTextWithCharSpacingPx(dc.GetSafeHdc(), hFont, dst.text, i, charSpacingPx);
			if (curX <= (prevW + w) / 2)
			{
				bestOff = i - ((i >= 2 && dst.text[i - 2] >= 0xD800 && dst.text[i - 2] <= 0xDBFF) ? 2 : 1);
				break;
			}
			prevW = w;
		}
		newCaret = dst.startChar + bestOff;
	}
	else
	{
		return false;
	}

	if (shift)
		m_inlineAnnotEdit.SetSel(selStart, newCaret);
	else
		m_inlineAnnotEdit.SetSel(newCaret, newCaret);
	return true;
}

void CXiaoGongPDFDlg::DrawInlineTextAnnotOverlay(CDC* pDC, const CRect& clientRect)
{
	m_inlineAnnot.boxViewRect.SetRectEmpty();
	m_inlineAnnot.moveHandleViewRect.SetRectEmpty();
	m_inlineAnnot.resizeHandleViewRect.SetRectEmpty();
	m_inlineAnnot.toolbarViewRect.SetRectEmpty();
	m_inlineAnnot.colorPopupViewRect.SetRectEmpty();
	for (auto& rc : m_inlineAnnot.toolbarBtnRects)
		rc.SetRectEmpty();
	for (auto& rc : m_inlineAnnot.colorSwatchRects)
		rc.SetRectEmpty();

	if (!pDC || !IsInlineTextAnnotVisible() || clientRect.IsRectEmpty())
		return;

	fz_rect bounds = {};
	float scale = 0.0f;
	int offsetX = 0, offsetY = 0;
	if (!GetSelectionPageTransform(m_inlineAnnot.pageNumber, bounds, scale, offsetX, offsetY) ||
		scale <= 0.01f)
	{
		return;
	}

	const int rot = GetPageRotation(m_inlineAnnot.pageNumber);
	const float origW = bounds.x1 - bounds.x0;
	const float origH = bounds.y1 - bounds.y0;
	const float effW = max(40.0f, (rot == 90 || rot == 270) ? origH : origW);

	CString fullText;
	int selStart = 0, selEnd = 0;
	if (m_inlineAnnotEdit.GetSafeHwnd())
	{
		m_inlineAnnotEdit.GetWindowText(fullText);
		m_inlineAnnotEdit.GetSel(selStart, selEnd);
	}

	const int fontPx = max(8, static_cast<int>(std::round(m_inlineAnnot.fontSizePt * scale)));
	CFont textFont;
	textFont.CreateFont(-fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, 0,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑"));

	const float padXPt = 4.0f;
	const float padYPt = 3.0f;
	const float charSpacingPx = m_inlineAnnot.charSpacingPt * scale;
	HFONT hTextFont = static_cast<HFONT>(textFont.GetSafeHandle());

	// 未手动拖拽宽度手柄时，单行输入（及增大文字间距时）自动适度向右延展至上限后再自动折行
	if (!m_inlineAnnot.userResizedWidth && !fullText.IsEmpty())
	{
		int firstBreak = fullText.FindOneOf(_T("\r\n"));
		CString firstPara = (firstBreak >= 0) ? fullText.Left(firstBreak) : fullText;
		const int paraPx = MeasureTextWithCharSpacingPx(
			pDC->GetSafeHdc(), hTextFont, firstPara, firstPara.GetLength(), charSpacingPx);
		const float textW = static_cast<float>(paraPx) / scale + padXPt * 2.0f + 10.0f;
		const float autoMaxW = min(300.0f, max(80.0f, effW - m_inlineAnnot.localXPt - 4.0f));
		if (textW > m_inlineAnnot.widthPt)
			m_inlineAnnot.widthPt = min(autoMaxW, textW);
	}

	std::vector<FreeTextVisualLine> lines;
	LayoutFreeTextVisualLines(pDC->GetSafeHdc(), hTextFont, scale,
		fullText, m_inlineAnnot.fontSizePt, m_inlineAnnot.charSpacingPt,
		max(20.0f, m_inlineAnnot.widthPt - padXPt * 2.0f), lines);

	const float lineStepPt = m_inlineAnnot.fontSizePt * 1.32f;
	const float boxHeightPt = max(m_inlineAnnot.fontSizePt * 1.45f + padYPt * 2.0f,
		static_cast<float>(lines.size()) * lineStepPt + padYPt * 2.0f);

	const int boxLeft = offsetX + static_cast<int>(std::round(m_inlineAnnot.localXPt * scale));
	const int boxTop = offsetY + static_cast<int>(std::round(m_inlineAnnot.localYPt * scale));
	const int boxRight = boxLeft + max(44, static_cast<int>(std::round(m_inlineAnnot.widthPt * scale)));
	const int boxBottom = boxTop + max(22, static_cast<int>(std::round(boxHeightPt * scale)));

	CRect boxRect(boxLeft, boxTop, boxRight, boxBottom);
	const int handleBarW = 12;
	CRect handleRect(boxRect.left - handleBarW, boxRect.top, boxRect.left, boxRect.bottom);
	const int resizeCenterY = (boxRect.top + boxRect.bottom) / 2;
	CRect resizeRect(boxRect.right - 6, resizeCenterY - 6, boxRect.right + 6, resizeCenterY + 6);

	CRect visTest;
	CRect combinedBox = boxRect;
	combinedBox.UnionRect(&combinedBox, &handleRect);
	if (!visTest.IntersectRect(&combinedBox, &clientRect))
		return;

	m_inlineAnnot.boxViewRect = boxRect;
	m_inlineAnnot.moveHandleViewRect = handleRect;
	m_inlineAnnot.resizeHandleViewRect = resizeRect;

	Gdiplus::Graphics graphics(pDC->GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	auto fillRoundPath = [&](const Gdiplus::RectF& rf, float radius,
		Gdiplus::Color fill, Gdiplus::Color border, float borderW)
	{
		if (rf.Width <= 1.0f || rf.Height <= 1.0f)
			return;
		const float r = max(0.5f, min(radius, min(rf.Width, rf.Height) * 0.5f));
		const float d = r * 2.0f;
		Gdiplus::GraphicsPath path;
		path.AddArc(rf.X, rf.Y, d, d, 180.0f, 90.0f);
		path.AddArc(rf.GetRight() - d, rf.Y, d, d, 270.0f, 90.0f);
		path.AddArc(rf.GetRight() - d, rf.GetBottom() - d, d, d, 0.0f, 90.0f);
		path.AddArc(rf.X, rf.GetBottom() - d, d, d, 90.0f, 90.0f);
		path.CloseFigure();
		if (fill.GetAlpha() > 0)
		{
			Gdiplus::SolidBrush b(fill);
			graphics.FillPath(&b, &path);
		}
		if (borderW > 0.0f && border.GetAlpha() > 0)
		{
			Gdiplus::Pen p(border, borderW);
			graphics.DrawPath(&p, &path);
		}
	};

	// 1. 绘制蓝色虚线外框（对应 3.png 文本框样式，透明无填充）
	const Gdiplus::Color edgeBlue(255, 0, 120, 212);
	{
		Gdiplus::Pen dashPen(edgeBlue, 1.5f);
		const Gdiplus::REAL dashPattern[2] = { 4.0f, 3.0f };
		dashPen.SetDashPattern(dashPattern, 2);
		Gdiplus::RectF boxRf(
			static_cast<Gdiplus::REAL>(boxRect.left) + 0.5f,
			static_cast<Gdiplus::REAL>(boxRect.top) + 0.5f,
			static_cast<Gdiplus::REAL>(max(1, boxRect.Width() - 1)),
			static_cast<Gdiplus::REAL>(max(1, boxRect.Height() - 1)));
		graphics.DrawRectangle(&dashPen, boxRf);
	}

	// 2. 绘制左侧蓝色拖拽手柄条及 2×3 白色抓握圆点（对应 3.png 左侧移动手柄）
	{
		const Gdiplus::Color gripColor = (m_inlineAnnot.draggingMove || m_inlineAnnot.hoverMoveHandle) ?
			Gdiplus::Color(255, 0, 96, 176) : edgeBlue;
		Gdiplus::GraphicsPath gripPath;
		const float gx = static_cast<Gdiplus::REAL>(handleRect.left);
		const float gy = static_cast<Gdiplus::REAL>(handleRect.top);
		const float gw = static_cast<Gdiplus::REAL>(handleRect.Width());
		const float gh = static_cast<Gdiplus::REAL>(max(8, handleRect.Height()));
		const float gr = min(3.5f, gh * 0.25f);
		gripPath.AddArc(gx, gy, gr * 2.0f, gr * 2.0f, 180.0f, 90.0f);
		gripPath.AddLine(gx + gr, gy, gx + gw + 0.5f, gy);
		gripPath.AddLine(gx + gw + 0.5f, gy, gx + gw + 0.5f, gy + gh);
		gripPath.AddLine(gx + gw + 0.5f, gy + gh, gx + gr, gy + gh);
		gripPath.AddArc(gx, gy + gh - gr * 2.0f, gr * 2.0f, gr * 2.0f, 90.0f, 90.0f);
		gripPath.CloseFigure();
		Gdiplus::SolidBrush gripBrush(gripColor);
		graphics.FillPath(&gripBrush, &gripPath);

		// 绘制 6 个白色抓握小圆点 (2 列 × 3 行)
		Gdiplus::SolidBrush dotBrush(Gdiplus::Color(245, 255, 255, 255));
		const float cxLeft = gx + gw * 0.5f - 2.0f;
		const float cxRight = gx + gw * 0.5f + 2.0f;
		const float cyMid = gy + gh * 0.5f;
		for (int row = -1; row <= 1; ++row)
		{
			const float dy = cyMid + row * 4.2f;
			graphics.FillEllipse(&dotBrush, cxLeft - 1.0f, dy - 1.0f, 2.0f, 2.0f);
			graphics.FillEllipse(&dotBrush, cxRight - 1.0f, dy - 1.0f, 2.0f, 2.0f);
		}
	}

	// 3. 绘制右侧居中白底蓝圈缩放手柄（对应 3.png 右侧圆形调节锚点）
	{
		Gdiplus::SolidBrush circleFill(
			(m_inlineAnnot.draggingResizeW || m_inlineAnnot.hoverResizeHandle) ?
			Gdiplus::Color(255, 224, 240, 255) : Gdiplus::Color(255, 255, 255, 255));
		Gdiplus::Pen circlePen(edgeBlue, 2.0f);
		const Gdiplus::RectF circleRf(
			static_cast<Gdiplus::REAL>(boxRect.right) - 5.2f,
			static_cast<Gdiplus::REAL>(resizeCenterY) - 5.2f,
			10.4f, 10.4f);
		graphics.FillEllipse(&circleFill, circleRf);
		graphics.DrawEllipse(&circlePen, circleRf);
	}

	// 4. 绘制框内选区高亮、多行透明文字与闪烁输入光标
	pDC->SetBkMode(TRANSPARENT);
	CFont* pOldFont = pDC->SelectObject(&textFont);

	const int textLeftPx = boxLeft + static_cast<int>(std::round(padXPt * scale));
	const int textTopBasePx = boxTop + static_cast<int>(std::round(padYPt * scale));
	const float lineStepPx = lineStepPt * scale;
	const int glyphHeightPx = max(12, static_cast<int>(std::round(m_inlineAnnot.fontSizePt * 1.32f * scale)));

	COLORREF drawTextColor = m_inlineAnnot.color;
	if (m_readingMode == READING_NIGHT)
	{
		const int lum = (GetRValue(drawTextColor) * 299 +
			GetGValue(drawTextColor) * 587 + GetBValue(drawTextColor) * 114) / 1000;
		if (lum < 55)
			drawTextColor = RGB(232, 236, 242);
	}

	const int minSel = min(selStart, selEnd);
	const int maxSel = max(selStart, selEnd);
	if (minSel < maxSel && !fullText.IsEmpty())
	{
		Gdiplus::SolidBrush selBrush(Gdiplus::Color(115, 51, 153, 255));
		for (size_t i = 0; i < lines.size(); ++i)
		{
			const FreeTextVisualLine& ln = lines[i];
			const int ovStart = max(minSel, ln.startChar);
			const int ovEnd = min(maxSel, ln.endChar);
			if (ovStart < ovEnd)
			{
				const int preLen = ovStart - ln.startChar;
				const int selLen = ovEnd - ovStart;
				const int x1 = textLeftPx + (preLen > 0 ?
					MeasureTextWithCharSpacingPx(pDC->GetSafeHdc(), hTextFont, ln.text, preLen, charSpacingPx) : 0);
				const int x2 = textLeftPx +
					MeasureTextWithCharSpacingPx(pDC->GetSafeHdc(), hTextFont, ln.text, preLen + selLen, charSpacingPx);
				const int y1 = textTopBasePx + static_cast<int>(std::round(i * lineStepPx));
				graphics.FillRectangle(&selBrush, x1, y1, max(2, x2 - x1), glyphHeightPx);
			}
		}
	}

	const int savedDc = pDC->SaveDC();
	pDC->IntersectClipRect(boxRect.left + 1, boxRect.top + 1, boxRect.right - 1, boxRect.bottom - 1);
	pDC->SelectObject(&textFont);
	pDC->SetBkMode(TRANSPARENT);
	if (fullText.IsEmpty())
	{
		pDC->SetTextColor(m_readingMode == READING_NIGHT ? RGB(130, 140, 152) : RGB(158, 168, 180));
		CRect phRect(textLeftPx, textTopBasePx, boxRect.right - 4, boxRect.bottom - 2);
		pDC->DrawText(_T("输入文字..."), &phRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
	}
	else
	{
		pDC->SetTextColor(drawTextColor);
		for (size_t i = 0; i < lines.size(); ++i)
		{
			if (lines[i].text.IsEmpty())
				continue;
			const int yPx = textTopBasePx + static_cast<int>(std::round(i * lineStepPx));
			DrawTextWithCharSpacingPx(pDC->GetSafeHdc(), textLeftPx, yPx, lines[i].text, charSpacingPx);
		}
	}
	pDC->RestoreDC(savedDc);

	// 计算当前光标所在视觉行与像素坐标，并同步中文输入法候选窗锚点
	int caretLineIdx = 0;
	for (int i = 0; i < static_cast<int>(lines.size()); ++i)
	{
		if (selEnd >= lines[i].startChar && selEnd <= lines[i].endChar)
		{
			caretLineIdx = i;
			const bool softWrappedNext = (i + 1 < static_cast<int>(lines.size())) &&
				(lines[i + 1].startChar == lines[i].endChar) &&
				(selEnd == lines[i].endChar);
			if (!softWrappedNext)
				break;
		}
	}
	const FreeTextVisualLine& caretLine = lines[caretLineIdx];
	const int caretCharsInLine = max(0, min(caretLine.text.GetLength(), selEnd - caretLine.startChar));
	const int caretViewX = textLeftPx + (caretCharsInLine > 0 ?
		MeasureTextWithCharSpacingPx(pDC->GetSafeHdc(), hTextFont, caretLine.text, caretCharsInLine, charSpacingPx) : 0);
	const int caretViewY = textTopBasePx + static_cast<int>(std::round(caretLineIdx * lineStepPx));

	if (m_inlineAnnotEdit.GetSafeHwnd())
	{
		CRect pdfViewInDlg;
		m_pdfView.GetWindowRect(&pdfViewInDlg);
		ScreenToClient(&pdfViewInDlg);
		m_inlineAnnotEdit.SetWindowPos(nullptr,
			pdfViewInDlg.left + boxRect.left,
			pdfViewInDlg.top + boxRect.top,
			max(20, boxRect.Width()),
			max(20, boxRect.Height()),
			SWP_NOZORDER | SWP_NOACTIVATE);
		m_inlineAnnotEdit.SetCaretAnchorInEdit(
			CPoint(max(0, caretViewX - boxRect.left), max(0, caretViewY - boxRect.top)),
			glyphHeightPx);
	}

	if (m_inlineAnnot.caretVisible && m_inlineAnnotEdit.GetSafeHwnd() &&
		::GetFocus() == m_inlineAnnotEdit.GetSafeHwnd())
	{
		Gdiplus::SolidBrush caretBrush(Gdiplus::Color(245,
			GetRValue(drawTextColor), GetGValue(drawTextColor), GetBValue(drawTextColor)));
		graphics.FillRectangle(&caretBrush, caretViewX, caretViewY + 1, 2, max(10, glyphHeightPx - 2));
	}

	// 5. 绘制顶部白色圆角悬浮工具栏（字体颜色 | 字号增大/减小 | 文字间距增大/减小 | 删除 | 退出）
	const bool darkUi = (m_readingMode == READING_NIGHT);
	const int barH = 38;
	const int barPadX = 6;
	const int btnH = 28;
	const int btnWidths[INLINE_ANNOT_BTN_COUNT] = { 42, 32, 32, 32, 32, 32, 32 };
	const int divSpace = 9; // 两侧间距 + 1px 分隔竖线
	int barW = barPadX * 2;
	for (int w : btnWidths)
		barW += w;
	barW += divSpace * 2 + 2 * 4; // 与 3.png 一致：颜色组 | 字号与字距四按钮组 | 删除与退出组

	int barX = handleRect.left;
	barX = max(clientRect.left + 8, min(barX, clientRect.right - barW - 8));
	int barY = boxRect.top - barH - 10;
	bool toolbarPlacedBelow = false;
	if (barY < clientRect.top + 8)
	{
		barY = boxRect.bottom + 10;
		toolbarPlacedBelow = true;
	}
	barY = max(clientRect.top + 6, min(barY, clientRect.bottom - barH - 6));

	CRect barRect(barX, barY, barX + barW, barY + barH);
	m_inlineAnnot.toolbarViewRect = barRect;

	// 多层柔和投影与白色卡片底板（还原 3.png 悬浮工具条质感）
	for (int s = 3; s >= 1; --s)
	{
		Gdiplus::RectF shRf(
			static_cast<Gdiplus::REAL>(barRect.left - s),
			static_cast<Gdiplus::REAL>(barRect.top - s + 2),
			static_cast<Gdiplus::REAL>(barRect.Width() + s * 2),
			static_cast<Gdiplus::REAL>(barRect.Height() + s * 2));
		fillRoundPath(shRf, 6.0f + s, Gdiplus::Color(darkUi ? 28 : 14, 15, 23, 42),
			Gdiplus::Color(0, 0, 0, 0), 0.0f);
	}
	fillRoundPath(
		Gdiplus::RectF(static_cast<Gdiplus::REAL>(barRect.left) + 0.5f,
			static_cast<Gdiplus::REAL>(barRect.top) + 0.5f,
			static_cast<Gdiplus::REAL>(barRect.Width() - 1),
			static_cast<Gdiplus::REAL>(barRect.Height() - 1)),
		6.0f,
		darkUi ? Gdiplus::Color(250, 42, 46, 52) : Gdiplus::Color(252, 255, 255, 255),
		darkUi ? Gdiplus::Color(255, 72, 79, 88) : Gdiplus::Color(255, 222, 227, 235),
		1.0f);

	const Gdiplus::Color divColor = darkUi ?
		Gdiplus::Color(255, 68, 74, 82) : Gdiplus::Color(255, 224, 228, 234);
	Gdiplus::Pen divPen(divColor, 1.0f);
	auto drawBarDivider = [&](int xPos)
	{
		graphics.DrawLine(&divPen, xPos, barY + 9, xPos, barY + barH - 9);
	};

	const int btnTop = barY + (barH - btnH) / 2;
	int curX = barX + barPadX;
	m_inlineAnnot.toolbarBtnRects[INLINE_ANNOT_BTN_COLOR].SetRect(
		curX, btnTop, curX + btnWidths[0], btnTop + btnH);
	curX += btnWidths[0];
	drawBarDivider(curX + divSpace / 2);
	curX += divSpace;

	m_inlineAnnot.toolbarBtnRects[INLINE_ANNOT_BTN_FONT_INC].SetRect(
		curX, btnTop, curX + btnWidths[1], btnTop + btnH);
	curX += btnWidths[1] + 2;
	m_inlineAnnot.toolbarBtnRects[INLINE_ANNOT_BTN_FONT_DEC].SetRect(
		curX, btnTop, curX + btnWidths[2], btnTop + btnH);
	curX += btnWidths[2] + 2;
	m_inlineAnnot.toolbarBtnRects[INLINE_ANNOT_BTN_CHAR_SPACE_INC].SetRect(
		curX, btnTop, curX + btnWidths[3], btnTop + btnH);
	curX += btnWidths[3] + 2;
	m_inlineAnnot.toolbarBtnRects[INLINE_ANNOT_BTN_CHAR_SPACE_DEC].SetRect(
		curX, btnTop, curX + btnWidths[4], btnTop + btnH);
	curX += btnWidths[4];
	drawBarDivider(curX + divSpace / 2);
	curX += divSpace;

	m_inlineAnnot.toolbarBtnRects[INLINE_ANNOT_BTN_DELETE].SetRect(
		curX, btnTop, curX + btnWidths[5], btnTop + btnH);
	curX += btnWidths[5] + 2;
	m_inlineAnnot.toolbarBtnRects[INLINE_ANNOT_BTN_EXIT].SetRect(
		curX, btnTop, curX + btnWidths[6], btnTop + btnH);

	if (!m_captureBarFont.GetSafeHandle())
	{
		m_captureBarFont.CreateFont(-13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑"));
	}

	CFont iconLetterFont;
	iconLetterFont.CreateFont(-15, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, 0,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

	CFont avSmallFont;
	avSmallFont.CreateFont(-11, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, 0,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

	for (int i = 0; i < INLINE_ANNOT_BTN_COUNT; ++i)
	{
		const CRect& rc = m_inlineAnnot.toolbarBtnRects[i];
		const bool disabled =
			(i == INLINE_ANNOT_BTN_FONT_INC && m_inlineAnnot.fontSizePt >= 47.9f) ||
			(i == INLINE_ANNOT_BTN_FONT_DEC && m_inlineAnnot.fontSizePt <= 8.1f) ||
			(i == INLINE_ANNOT_BTN_CHAR_SPACE_INC && m_inlineAnnot.charSpacingPt >= 23.9f) ||
			(i == INLINE_ANNOT_BTN_CHAR_SPACE_DEC && m_inlineAnnot.charSpacingPt <= 0.01f);
		const bool hovered = !disabled && (m_inlineAnnot.hoverToolbarBtn == i);
		const bool pressed = !disabled && (m_inlineAnnot.pressedToolbarBtn == i);
		const bool toggled = (i == INLINE_ANNOT_BTN_COLOR && m_inlineAnnot.colorPopupOpen);

		if (hovered || pressed || toggled)
		{
			Gdiplus::Color btnBg;
			if (i == INLINE_ANNOT_BTN_DELETE && (hovered || pressed))
			{
				btnBg = pressed ? Gdiplus::Color(255, 254, 215, 215) :
					Gdiplus::Color(255, 254, 232, 232);
			}
			else if (darkUi)
			{
				btnBg = pressed ? Gdiplus::Color(255, 70, 77, 88) : Gdiplus::Color(255, 58, 64, 73);
			}
			else
			{
				btnBg = pressed ? Gdiplus::Color(255, 218, 230, 247) : Gdiplus::Color(255, 236, 242, 251);
			}
			fillRoundPath(Gdiplus::RectF(
				static_cast<Gdiplus::REAL>(rc.left),
				static_cast<Gdiplus::REAL>(rc.top),
				static_cast<Gdiplus::REAL>(rc.Width()),
				static_cast<Gdiplus::REAL>(rc.Height())),
				4.5f, btnBg, Gdiplus::Color(0, 0, 0, 0), 0.0f);
		}

		const COLORREF fgCol = disabled ?
			(darkUi ? RGB(105, 112, 122) : RGB(188, 195, 205)) :
			((i == INLINE_ANNOT_BTN_DELETE && hovered) ? RGB(210, 38, 48) :
				(darkUi ? RGB(228, 233, 240) : RGB(52, 58, 68)));
		const Gdiplus::Color gdipFg(255, GetRValue(fgCol), GetGValue(fgCol), GetBValue(fgCol));
		Gdiplus::Pen iconPen(gdipFg, 1.35f);
		iconPen.SetStartCap(Gdiplus::LineCapRound);
		iconPen.SetEndCap(Gdiplus::LineCapRound);
		iconPen.SetLineJoin(Gdiplus::LineJoinRound);

		const float cx = (rc.left + rc.right) * 0.5f;
		const float cy = (rc.top + rc.bottom) * 0.5f;

		if (i == INLINE_ANNOT_BTN_COLOR)
		{
			// A + 底部当前颜色条 + 右侧下拉小箭头（与 3.png 一致）
			pDC->SelectObject(&iconLetterFont);
			pDC->SetTextColor(fgCol);
			CRect aRect(rc.left + 3, rc.top + 1, rc.left + 23, rc.bottom - 6);
			pDC->DrawText(_T("A"), &aRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

			fillRoundPath(Gdiplus::RectF(
				static_cast<Gdiplus::REAL>(rc.left + 6),
				static_cast<Gdiplus::REAL>(rc.bottom - 7),
				14.0f, 3.6f),
				1.5f,
				Gdiplus::Color(255, GetRValue(m_inlineAnnot.color),
					GetGValue(m_inlineAnnot.color), GetBValue(m_inlineAnnot.color)),
				Gdiplus::Color(0, 0, 0, 0), 0.0f);

			const float vx = static_cast<Gdiplus::REAL>(rc.right - 11);
			const float vy = cy + 0.5f;
			graphics.DrawLine(&iconPen, vx - 3.0f, vy - 1.5f, vx, vy + 1.8f);
			graphics.DrawLine(&iconPen, vx, vy + 1.8f, vx + 3.0f, vy - 1.5f);
		}
		else if (i == INLINE_ANNOT_BTN_FONT_INC || i == INLINE_ANNOT_BTN_FONT_DEC)
		{
			// A^ 增大字号 / Aˇ 减小字号（与 3.png 一致）
			pDC->SelectObject(&iconLetterFont);
			pDC->SetTextColor(fgCol);
			CRect aRect(rc.left + 2, rc.top + 2, rc.right - 7, rc.bottom - 1);
			pDC->DrawText(_T("A"), &aRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

			const float ax = static_cast<Gdiplus::REAL>(rc.right - 8);
			const float ay = static_cast<Gdiplus::REAL>(rc.top + 9);
			if (i == INLINE_ANNOT_BTN_FONT_INC)
			{
				graphics.DrawLine(&iconPen, ax - 2.6f, ay + 1.4f, ax, ay - 1.5f);
				graphics.DrawLine(&iconPen, ax, ay - 1.5f, ax + 2.6f, ay + 1.4f);
			}
			else
			{
				graphics.DrawLine(&iconPen, ax - 2.6f, ay - 1.4f, ax, ay + 1.5f);
				graphics.DrawLine(&iconPen, ax, ay + 1.5f, ax + 2.6f, ay - 1.4f);
			}
		}
		else if (i == INLINE_ANNOT_BTN_CHAR_SPACE_INC || i == INLINE_ANNOT_BTN_CHAR_SPACE_DEC)
		{
			// 还原 3.png 中的 AV + 底部 <-> / >-< 文字间距图标
			pDC->SelectObject(&avSmallFont);
			pDC->SetTextColor(fgCol);
			CRect avRect(rc.left, rc.top + 2, rc.right, rc.bottom - 8);
			pDC->DrawText(_T("AV"), &avRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

			Gdiplus::Pen arrowPen(gdipFg, 1.15f);
			arrowPen.SetStartCap(Gdiplus::LineCapRound);
			arrowPen.SetEndCap(Gdiplus::LineCapRound);
			const float ay = static_cast<Gdiplus::REAL>(rc.bottom - 6);
			if (i == INLINE_ANNOT_BTN_CHAR_SPACE_INC)
			{
				graphics.DrawLine(&arrowPen, cx - 6.8f, ay, cx + 6.8f, ay);
				graphics.DrawLine(&arrowPen, cx - 4.4f, ay - 2.0f, cx - 6.8f, ay);
				graphics.DrawLine(&arrowPen, cx - 4.4f, ay + 2.0f, cx - 6.8f, ay);
				graphics.DrawLine(&arrowPen, cx + 4.4f, ay - 2.0f, cx + 6.8f, ay);
				graphics.DrawLine(&arrowPen, cx + 4.4f, ay + 2.0f, cx + 6.8f, ay);
			}
			else
			{
				graphics.DrawLine(&arrowPen, cx - 7.5f, ay, cx - 1.5f, ay);
				graphics.DrawLine(&arrowPen, cx - 3.6f, ay - 1.9f, cx - 1.5f, ay);
				graphics.DrawLine(&arrowPen, cx - 3.6f, ay + 1.9f, cx - 1.5f, ay);

				graphics.DrawLine(&arrowPen, cx + 7.5f, ay, cx + 1.5f, ay);
				graphics.DrawLine(&arrowPen, cx + 3.6f, ay - 1.9f, cx + 1.5f, ay);
				graphics.DrawLine(&arrowPen, cx + 3.6f, ay + 1.9f, cx + 1.5f, ay);
			}
		}
		else if (i == INLINE_ANNOT_BTN_DELETE)
		{
			// 垃圾桶删除图标（与 3.png 一致）
			graphics.DrawLine(&iconPen, cx - 6.5f, cy - 4.5f, cx + 6.5f, cy - 4.5f);
			graphics.DrawLine(&iconPen, cx - 2.5f, cy - 6.5f, cx + 2.5f, cy - 6.5f);
			graphics.DrawLine(&iconPen, cx - 2.5f, cy - 6.5f, cx - 2.5f, cy - 4.5f);
			graphics.DrawLine(&iconPen, cx + 2.5f, cy - 6.5f, cx + 2.5f, cy - 4.5f);

			graphics.DrawLine(&iconPen, cx - 4.8f, cy - 4.5f, cx - 4.0f, cy + 6.5f);
			graphics.DrawLine(&iconPen, cx + 4.8f, cy - 4.5f, cx + 4.0f, cy + 6.5f);
			graphics.DrawLine(&iconPen, cx - 4.0f, cy + 6.5f, cx + 4.0f, cy + 6.5f);

			graphics.DrawLine(&iconPen, cx - 1.6f, cy - 1.8f, cx - 1.4f, cy + 3.8f);
			graphics.DrawLine(&iconPen, cx + 1.6f, cy - 1.8f, cx + 1.4f, cy + 3.8f);
		}
		else if (i == INLINE_ANNOT_BTN_EXIT)
		{
			// 退出编辑按钮 (✕)
			graphics.DrawLine(&iconPen, cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f);
			graphics.DrawLine(&iconPen, cx + 4.5f, cy - 4.5f, cx - 4.5f, cy + 4.5f);
		}
	}

	// 6. 点击颜色按钮展开 8 色调色板浮层
	if (m_inlineAnnot.colorPopupOpen)
	{
		static const COLORREF kAnnotPalette[INLINE_ANNOT_SWATCH_COUNT] = {
			RGB(0, 114, 206),
			RGB(216, 43, 43),
			RGB(32, 33, 36),
			RGB(22, 148, 72),
			RGB(230, 106, 18),
			RGB(124, 58, 202),
			RGB(198, 32, 104),
			RGB(95, 107, 122)
		};

		const int swSize = 20;
		const int swGap = 6;
		const int palPad = 8;
		const int palW = palPad * 2 + INLINE_ANNOT_SWATCH_COUNT * swSize +
			(INLINE_ANNOT_SWATCH_COUNT - 1) * swGap;
		const int palH = 36;
		int palX = max(clientRect.left + 8, min(barRect.left, clientRect.right - palW - 8));
		int palY = toolbarPlacedBelow ? (barRect.bottom + 6) : (barRect.top - palH - 6);
		if (palY < clientRect.top + 6)
			palY = barRect.bottom + 6;
		if (palY + palH > clientRect.bottom - 6)
			palY = max(clientRect.top + 6, barRect.top - palH - 6);

		CRect palRect(palX, palY, palX + palW, palY + palH);
		m_inlineAnnot.colorPopupViewRect = palRect;

		fillRoundPath(
			Gdiplus::RectF(static_cast<Gdiplus::REAL>(palRect.left - 1),
				static_cast<Gdiplus::REAL>(palRect.top + 1),
				static_cast<Gdiplus::REAL>(palRect.Width() + 2),
				static_cast<Gdiplus::REAL>(palRect.Height() + 2)),
			7.0f, Gdiplus::Color(24, 15, 23, 42), Gdiplus::Color(0, 0, 0, 0), 0.0f);
		fillRoundPath(
			Gdiplus::RectF(static_cast<Gdiplus::REAL>(palRect.left) + 0.5f,
				static_cast<Gdiplus::REAL>(palRect.top) + 0.5f,
				static_cast<Gdiplus::REAL>(palRect.Width() - 1),
				static_cast<Gdiplus::REAL>(palRect.Height() - 1)),
			6.0f,
			darkUi ? Gdiplus::Color(252, 42, 46, 52) : Gdiplus::Color(254, 255, 255, 255),
			darkUi ? Gdiplus::Color(255, 72, 79, 88) : Gdiplus::Color(255, 220, 226, 235),
			1.0f);

		const int swTop = palY + (palH - swSize) / 2;
		for (int i = 0; i < INLINE_ANNOT_SWATCH_COUNT; ++i)
		{
			const int swLeft = palX + palPad + i * (swSize + swGap);
			CRect swRect(swLeft, swTop, swLeft + swSize, swTop + swSize);
			m_inlineAnnot.colorSwatchRects[i] = swRect;

			const COLORREF c = kAnnotPalette[i];
			Gdiplus::SolidBrush swBrush(Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)));
			Gdiplus::RectF swRf(
				static_cast<Gdiplus::REAL>(swRect.left) + 1.0f,
				static_cast<Gdiplus::REAL>(swRect.top) + 1.0f,
				static_cast<Gdiplus::REAL>(swSize - 2),
				static_cast<Gdiplus::REAL>(swSize - 2));
			graphics.FillEllipse(&swBrush, swRf);

			if (m_inlineAnnot.color == c)
			{
				Gdiplus::Pen selRingPen(edgeBlue, 1.8f);
				Gdiplus::RectF ringRf(swRf.X - 2.2f, swRf.Y - 2.2f,
					swRf.Width + 4.4f, swRf.Height + 4.4f);
				graphics.DrawEllipse(&selRingPen, ringRf);

				Gdiplus::Pen checkPen(Gdiplus::Color(255, 255, 255, 255), 1.6f);
				checkPen.SetStartCap(Gdiplus::LineCapRound);
				checkPen.SetEndCap(Gdiplus::LineCapRound);
				const float scx = (swRect.left + swRect.right) * 0.5f;
				const float scy = (swRect.top + swRect.bottom) * 0.5f;
				graphics.DrawLine(&checkPen, scx - 3.5f, scy, scx - 1.0f, scy + 2.5f);
				graphics.DrawLine(&checkPen, scx - 1.0f, scy + 2.5f, scx + 4.0f, scy - 2.8f);
			}
			else if (m_inlineAnnot.hoverColorSwatch == i)
			{
				Gdiplus::Pen hovPen(Gdiplus::Color(180, 0, 120, 212), 1.4f);
				Gdiplus::RectF ringRf(swRf.X - 1.6f, swRf.Y - 1.6f,
					swRf.Width + 3.2f, swRf.Height + 3.2f);
				graphics.DrawEllipse(&hovPen, ringRf);
			}
		}
	}
	else if (m_inlineAnnot.hoverToolbarBtn >= 0 && m_inlineAnnot.hoverToolbarBtn < INLINE_ANNOT_BTN_COUNT)
	{
		// 悬停工具栏按钮时显示简洁提示胶囊（含实时字号/行距数值）
		CString tipText;
		switch (m_inlineAnnot.hoverToolbarBtn)
		{
		case INLINE_ANNOT_BTN_COLOR:
			tipText = _T("字体颜色");
			break;
		case INLINE_ANNOT_BTN_FONT_INC:
			tipText.Format(_T("增大字号 (%.0f pt)"), m_inlineAnnot.fontSizePt);
			break;
		case INLINE_ANNOT_BTN_FONT_DEC:
			tipText.Format(_T("减小字号 (%.0f pt)"), m_inlineAnnot.fontSizePt);
			break;
		case INLINE_ANNOT_BTN_CHAR_SPACE_INC:
			tipText.Format(_T("增大文字间距 (+%.1f pt)"), m_inlineAnnot.charSpacingPt);
			break;
		case INLINE_ANNOT_BTN_CHAR_SPACE_DEC:
			tipText.Format(_T("减小文字间距 (+%.1f pt)"), m_inlineAnnot.charSpacingPt);
			break;
		case INLINE_ANNOT_BTN_DELETE:
			tipText = _T("删除批注");
			break;
		case INLINE_ANNOT_BTN_EXIT:
			tipText = _T("退出编辑 (Esc)");
			break;
		default:
			break;
		}
		if (!tipText.IsEmpty())
		{
			pDC->SelectObject(&m_captureBarFont);
			const CSize tipExt = pDC->GetTextExtent(tipText);
			const int tipW = tipExt.cx + 16;
			const int tipH = 22;
			const CRect& btnRc = m_inlineAnnot.toolbarBtnRects[m_inlineAnnot.hoverToolbarBtn];
			int tipX = max(clientRect.left + 6,
				min((btnRc.left + btnRc.right - tipW) / 2, clientRect.right - tipW - 6));
			int tipY = (barRect.top >= tipH + 8) ? (barRect.top - tipH - 5) : (barRect.bottom + 5);
			CRect tipRect(tipX, tipY, tipX + tipW, tipY + tipH);
			fillRoundPath(Gdiplus::RectF(
				static_cast<Gdiplus::REAL>(tipRect.left),
				static_cast<Gdiplus::REAL>(tipRect.top),
				static_cast<Gdiplus::REAL>(tipRect.Width()),
				static_cast<Gdiplus::REAL>(tipRect.Height())),
				4.5f, Gdiplus::Color(225, 30, 35, 42), Gdiplus::Color(0, 0, 0, 0), 0.0f);
			pDC->SetTextColor(RGB(246, 248, 252));
			pDC->DrawText(tipText, &tipRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
	}

	pDC->SelectObject(pOldFont);
}

bool CXiaoGongPDFDlg::HitTestAnnotationAtViewPoint(
	CPoint viewPt, int& outPageNumber, PageAnnotHitInfo& outHit)
{
	outPageNumber = -1;
	outHit = PageAnnotHitInfo{};
	CPDFDocument* pDoc = GetActiveDocument();
	if (!m_doc || !pDoc || m_totalPages <= 0)
		return false;

	const int pageNumber = GetPageAtViewPoint(viewPt, false);
	if (pageNumber < 0 || pageNumber >= m_totalPages)
		return false;

	float fitzX = 0.0f;
	float fitzY = 0.0f;
	if (!ScreenPointToPdfCoords(viewPt, pageNumber, fitzX, fitzY))
		return false;

	CSingleLock lock(&m_renderLock, TRUE);
	if (HitTestPageAnnotation(pDoc, pageNumber, fitzX, fitzY, outHit))
	{
		outPageNumber = pageNumber;
		return true;
	}
	return false;
}

int CXiaoGongPDFDlg::HitTestSelectionActionBar(CPoint viewPt) const
{
	if (!IsSelectionActionBarVisible())
		return SEL_BAR_NONE;
	for (int i = 0; i < SEL_BAR_COUNT; ++i)
	{
		if (!m_selectionActionRects[i].IsRectEmpty() && m_selectionActionRects[i].PtInRect(viewPt))
			return i;
	}
	return SEL_BAR_NONE;
}

void CXiaoGongPDFDlg::DrawSelectionActionBar(CDC* pDC, const CRect& clientRect)
{
	for (auto& rc : m_selectionActionRects)
		rc.SetRectEmpty();

	if (!pDC || !IsSelectionActionBarVisible() || clientRect.IsRectEmpty())
		return;

	fz_rect bounds = {};
	float scale = 0.0f;
	int offsetX = 0;
	int offsetY = 0;
	if (!GetSelectionPageTransform(m_selPageNumber, bounds, scale, offsetX, offsetY))
		return;

	const int rotation = GetPageRotation(m_selPageNumber);
	CRect unionRect(0, 0, 0, 0);
	bool hasAny = false;
	for (const fz_quad& quad : m_selectionQuads)
	{
		CRect qr = TransformQuadToBitmapRect(quad, bounds, rotation, scale);
		qr.OffsetRect(offsetX, offsetY);
		if (!qr.IsRectEmpty())
		{
			if (!hasAny)
			{
				unionRect = qr;
				hasAny = true;
			}
			else
			{
				unionRect.UnionRect(&unionRect, &qr);
			}
		}
	}
	CRect visibleCheck;
	if (!hasAny || !visibleCheck.IntersectRect(&unionRect, &clientRect))
		return;

	if (!m_captureBarFont.GetSafeHandle())
	{
		m_captureBarFont.CreateFont(-13, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑"));
	}

	Gdiplus::Graphics graphics(pDC->GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	auto drawPill = [&](const CRect& rc, float radius, Gdiplus::Color fill,
		Gdiplus::Color border, bool withBorder)
	{
		if (rc.Width() <= 2 || rc.Height() <= 2)
			return;
		const float r = max(1.0f, min(radius, min(rc.Width(), rc.Height()) * 0.5f));
		const float d = r * 2.0f;
		Gdiplus::RectF rf(static_cast<Gdiplus::REAL>(rc.left) + 0.5f,
			static_cast<Gdiplus::REAL>(rc.top) + 0.5f,
			static_cast<Gdiplus::REAL>(rc.Width() - 1),
			static_cast<Gdiplus::REAL>(rc.Height() - 1));
		Gdiplus::GraphicsPath path;
		path.AddArc(rf.X, rf.Y, d, d, 180.0f, 90.0f);
		path.AddArc(rf.GetRight() - d, rf.Y, d, d, 270.0f, 90.0f);
		path.AddArc(rf.GetRight() - d, rf.GetBottom() - d, d, d, 0.0f, 90.0f);
		path.AddArc(rf.X, rf.GetBottom() - d, d, d, 90.0f, 90.0f);
		path.CloseFigure();
		Gdiplus::SolidBrush b(fill);
		graphics.FillPath(&b, &path);
		if (withBorder)
		{
			Gdiplus::Pen p(border, 1.0f);
			graphics.DrawPath(&p, &path);
		}
	};

	const int barH = 34;
	const int pad = 4;
	const int btnGap = 4;
	const int btnWidths[SEL_BAR_COUNT] = { 54, 58, 66, 54 };
	int barW = pad * 2 + btnGap * (SEL_BAR_COUNT - 1);
	for (int w : btnWidths)
		barW += w;

	int barX = (unionRect.left + unionRect.right - barW) / 2;
	barX = max(clientRect.left + 8, min(barX, clientRect.right - barW - 8));
	int barY = unionRect.bottom + 8;
	if (barY + barH + 6 > clientRect.bottom)
		barY = (unionRect.top >= barH + 12) ? (unionRect.top - barH - 8) : (clientRect.bottom - barH - 8);
	barY = max(clientRect.top + 6, min(barY, clientRect.bottom - barH - 6));

	CRect barRect(barX, barY, barX + barW, barY + barH);
	const bool dark = (m_readingMode == READING_NIGHT);
	drawPill(barRect, 8.0f,
		dark ? Gdiplus::Color(246, 40, 44, 50) : Gdiplus::Color(250, 255, 255, 255),
		dark ? Gdiplus::Color(255, 72, 79, 88) : Gdiplus::Color(255, 206, 218, 235),
		true);

	pDC->SetBkMode(TRANSPARENT);
	CFont* pOldFont = pDC->SelectObject(&m_captureBarFont);

	const LPCTSTR labels[SEL_BAR_COUNT] = { _T("复制"), _T("高亮"), _T("下划线"), _T("批注") };
	int curX = barX + pad;
	for (int i = 0; i < SEL_BAR_COUNT; ++i)
	{
		m_selectionActionRects[i].SetRect(curX, barY + pad, curX + btnWidths[i], barY + barH - pad);
		curX += btnWidths[i] + btnGap;

		const bool hovered = (m_hoverSelectionAction == i);
		const bool pressed = (m_pressedSelectionAction == i);
		if (hovered || pressed)
		{
			Gdiplus::Color fill = dark ?
				(pressed ? Gdiplus::Color(255, 68, 75, 85) : Gdiplus::Color(255, 56, 62, 70)) :
				(pressed ? Gdiplus::Color(255, 216, 230, 248) : Gdiplus::Color(255, 234, 242, 253));
			drawPill(m_selectionActionRects[i], 5.0f, fill, fill, false);
		}
		if (i == SEL_BAR_HIGHLIGHT)
		{
			// 底部黄色色带提示“荧光高亮”
			CRect accent = m_selectionActionRects[i];
			Gdiplus::SolidBrush hlBrush(Gdiplus::Color(210, 250, 204, 21));
			graphics.FillRectangle(&hlBrush, accent.left + 12, accent.bottom - 4,
				max(8, accent.Width() - 24), 3);
		}
		else if (i == SEL_BAR_UNDERLINE)
		{
			CRect accent = m_selectionActionRects[i];
			Gdiplus::SolidBrush ulBrush(Gdiplus::Color(220, 29, 124, 243));
			graphics.FillRectangle(&ulBrush, accent.left + 10, accent.bottom - 4,
				max(8, accent.Width() - 20), 2);
		}
		pDC->SetTextColor(dark ? RGB(228, 233, 238) : RGB(36, 44, 54));
		pDC->DrawText(labels[i], &m_selectionActionRects[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}

	pDC->SelectObject(pOldFont);
}

void CXiaoGongPDFDlg::OnMenuSavePdf()
{
	CPDFDocument* pDoc = GetActiveDocument();
	if (!m_doc || !pDoc || m_totalPages <= 0)
	{
		SetStatusBarMessage(_T("请先打开 PDF 文档"));
		return;
	}
	SaveDocumentEdits(pDoc, false);
}

void CXiaoGongPDFDlg::OnMenuSavePdfAs()
{
	CPDFDocument* pDoc = GetActiveDocument();
	if (!m_doc || !pDoc || m_totalPages <= 0)
	{
		MessageBox(_T("请先打开 PDF 文档"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}
	SaveDocumentEdits(pDoc, true);
}

bool CXiaoGongPDFDlg::SaveDocumentEdits(CPDFDocument* pTargetDoc, bool saveAs)
{
	if (!pTargetDoc || !pTargetDoc->IsOpen())
		return false;

	if (pTargetDoc == GetActiveDocument())
	{
		FinishInlineTextAnnot(false);
		SaveDialogStateToDocument(pTargetDoc);
	}

	CancelAreaCaptureMode(false);

	CString targetPath = pTargetDoc->GetFilePath();
	ScopedFileWatchSuspend suspendWatch(this);

	if (saveAs || targetPath.IsEmpty())
	{
		CString defaultName = targetPath;
		if (!defaultName.IsEmpty())
		{
			const int dot = defaultName.ReverseFind(_T('.'));
			if (dot > 0)
				defaultName = defaultName.Left(dot) + _T("_已批注.pdf");
		}
		CFileDialog saveDlg(FALSE, _T("pdf"), defaultName.IsEmpty() ? _T("已批注文档.pdf") : defaultName.GetString(),
			OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST,
			_T("PDF 文档 (*.pdf)|*.pdf||"), this);
		if (saveDlg.DoModal() != IDOK)
			return false;
		targetPath = saveDlg.GetPathName();
	}
	else if (!pTargetDoc->HasUnsavedAnnotations() && !pTargetDoc->HasNonZeroPageRotations())
	{
		SetStatusBarMessage(_T("当前文档没有待写回的批注或单页旋转修改"));
		return true;
	}

	CleanupSearches();

	auto normalizeFilePath = [](const CString& path) -> CString
	{
		if (path.IsEmpty())
			return CString();
		TCHAR fullPath[MAX_PATH * 4] = { 0 };
		const DWORD len = ::GetFullPathName(path, static_cast<DWORD>(_countof(fullPath)), fullPath, nullptr);
		CString normalized = (len > 0 && len < _countof(fullPath)) ? CString(fullPath) : path;
		normalized.Replace(_T('/'), _T('\\'));
		return normalized;
	};

	struct LockedSaveDocState
	{
		CPDFDocument* doc = nullptr;
		bool wasActive = false;
		int savedPage = 0;
		ZoomMode savedZoomMode = ZOOM_CUSTOM;
		float savedCustomZoom = 1.0f;
		float savedUniformScale = 0.0f;
		int savedScroll = 0;
		PageLayoutMode savedLayoutMode = LAYOUT_SINGLE_CONTINUOUS;
		std::map<int, int> savedRotations;
		bool savedUnsavedAnnots = false;
		CString filePath;
		std::vector<char> password;
	};
	std::vector<LockedSaveDocState> lockedDocs;

	const std::map<int, int> rotationsToWrite = pTargetDoc->GetPageRotations();
	CString errorMsg;

	const bool savedOk = SaveDocumentWithAnnotationsAndRotations(
		pTargetDoc,
		targetPath,
		rotationsToWrite,
		[&](const CString& replacePath)
		{
			lockedDocs.clear();
			const CString normReplace = normalizeFilePath(replacePath);
			if (normReplace.IsEmpty())
				return;

			for (int i = 0; i < static_cast<int>(m_documents.size()); ++i)
			{
				CPDFDocument* openDoc = m_documents[i];
				if (!openDoc || !openDoc->IsOpen())
					continue;
				if (normalizeFilePath(openDoc->GetFilePath()).CompareNoCase(normReplace) != 0)
					continue;

				const bool isActive = (i == m_activeDocIndex);
				if (isActive)
				{
					SaveDialogStateToDocument(openDoc);
					ClearSelection();
					CleanupBitmap();
				}

				LockedSaveDocState st;
				st.doc = openDoc;
				st.wasActive = isActive;
				st.savedPage = openDoc->GetCurrentPage();
				st.savedZoomMode = openDoc->GetZoomMode();
				st.savedCustomZoom = openDoc->GetCustomZoom();
				st.savedUniformScale = openDoc->GetUniformScale();
				st.savedScroll = openDoc->GetScrollPosition();
				st.savedLayoutMode = openDoc->GetLayoutMode();
				st.savedRotations = openDoc->GetPageRotations();
				st.savedUnsavedAnnots = openDoc->HasUnsavedAnnotations();
				st.filePath = openDoc->GetFilePath();
				st.password = openDoc->GetPassword();
				lockedDocs.push_back(std::move(st));

				openDoc->CloseDocument();
				if (isActive)
				{
					m_doc = nullptr;
					m_ctx = nullptr;
				}
			}
		},
		[&](const CString& /*replacePath*/, bool moved)
		{
			for (auto& st : lockedDocs)
			{
				if (!st.doc || st.filePath.IsEmpty())
					continue;

				const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, st.filePath, -1, NULL, 0, NULL, NULL);
				std::vector<char> utf8Path(max(1, utf8Len), 0);
				WideCharToMultiByte(CP_UTF8, 0, st.filePath, -1, utf8Path.data(), utf8Len, NULL, NULL);
				const char* pwd = st.password.empty() ? nullptr : st.password.data();

				const bool reopened = (st.doc->OpenDocument(utf8Path.data(), pwd) == PDFOpenResult::Success);
				if (!st.password.empty())
					SecureZeroMemory(st.password.data(), st.password.size());

				if (reopened)
				{
					const int totalPages = st.doc->GetTotalPages();
					const int clampedPage = max(0, min(st.savedPage, totalPages - 1));
					st.doc->SetCurrentPage(clampedPage);
					st.doc->RestoreReadingZoom(st.savedZoomMode, st.savedCustomZoom, st.savedUniformScale);
					st.doc->SetScrollPosition(st.savedScroll);
					st.doc->SetLayoutMode(st.savedLayoutMode);

					if (moved)
					{
						// 旋转角度已永久写回 PDF 的 /Rotate，清空阅读器附加旋转记录防止二次叠加
						st.doc->SetUnsavedAnnotations(false);
						AfxGetApp()->WriteProfileString(_T("ReadingPosition"),
							ReadingStateKey(st.filePath) + _T("_Rotations"), _T(""));
						SaveReadingPosition(st.doc);
					}
					else
					{
						st.doc->SetUnsavedAnnotations(st.savedUnsavedAnnots);
						for (const auto& rotPair : st.savedRotations)
						{
							if (rotPair.first >= 0 && rotPair.first < totalPages)
								st.doc->SetPageRotation(rotPair.first, rotPair.second);
						}
					}
				}

				if (st.wasActive && st.doc->IsOpen())
				{
					LoadDialogStateFromDocument(st.doc);
					ClearSearchResults();
					if (m_uniformScale <= 0.0f)
						CalculatePagePositions(true);
					else
						CalculatePagePositions(m_zoomMode == ZOOM_FIT_WIDTH || m_zoomMode == ZOOM_FIT_PAGE);
					UpdateScrollBar();
					RenderVisiblePages();
					UpdatePageControls();
					UpdateStatusBar();
				}
			}
			lockedDocs.clear();
		},
		errorMsg);

	if (!savedOk)
	{
		MessageBox(errorMsg.IsEmpty() ? _T("保存 PDF 文件失败。") : errorMsg.GetString(),
			_T("保存失败"), MB_OK | MB_ICONERROR);
		UpdateDocumentDirtyTitle();
		return false;
	}

	const bool savedInPlace = (normalizeFilePath(targetPath).CompareNoCase(
		normalizeFilePath(pTargetDoc->GetFilePath())) == 0);
	if (savedInPlace)
	{
		pTargetDoc->SetUnsavedAnnotations(false);
		UpdateDocumentDirtyTitle();
		SetStatusBarMessage(_T("已将批注与页面旋转成功写回原 PDF 文件"));
	}
	else
	{
		UpdateDocumentDirtyTitle();
		CString msg;
		msg.Format(_T("已另存批注与旋转至：%s"), targetPath.GetString());
		SetStatusBarMessage(msg);
		OpenPDFInNewTab(targetPath);
	}
	return true;
}

bool CXiaoGongPDFDlg::PromptSaveUnsavedAnnotations(CPDFDocument* pDoc)
{
	if (!pDoc || !pDoc->HasUnsavedAnnotations())
		return true;

	CString prompt;
	prompt.Format(_T("文档 \"%s\" 含有未写回文件的 PDF 批注修改。\n\n是否在关闭前将批注（及当前页面旋转）保存回原 PDF 文件？"),
		pDoc->GetFileName().GetString());
	const int choice = MessageBox(prompt, _T("保存批注确认"),
		MB_YESNOCANCEL | MB_ICONQUESTION);
	if (choice == IDCANCEL)
		return false;
	if (choice == IDNO)
	{
		pDoc->SetUnsavedAnnotations(false);
		return true;
	}
	return SaveDocumentEdits(pDoc, false);
}
