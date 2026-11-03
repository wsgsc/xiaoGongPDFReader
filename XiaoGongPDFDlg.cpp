#include "pch.h"
#include "framework.h"
#include <commctrl.h>
#include "XiaoGongPDF.h"
#include "XiaoGongPDFDlg.h"
#include "PDFEditDialog.h"
#include "ProgressDlg.h"
#include "afxdialogex.h"
#include <mupdf/fitz.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <WinUser.h>  // 添加这个头文件来获取 HTSCROLLBAR 定义
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
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
	constexpr COLORREF PDF_VIEW_BACKGROUND = RGB(225, 230, 235);
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

	struct SearchHitCollector
	{
		int pageNumber;
		std::vector<SearchMatch>* results;
	};

	int CollectSearchHit(fz_context*, void* opaque, int count, fz_quad* quads)
	{
		if (count <= 0)
			return 0;
		SearchHitCollector* collector = static_cast<SearchHitCollector*>(opaque);
		fz_rect bounds = fz_rect_from_quad(quads[0]);
		for (int i = 1; i < count; ++i)
			bounds = fz_union_rect(bounds, fz_rect_from_quad(quads[i]));
		SearchMatch match;
		match.pageNumber = collector->pageNumber;
		match.quad = fz_quad_from_rect(bounds);
		collector->results->push_back(match);
		return 0;
	}

	CString BuildSearchContext(fz_stext_page* textPage, const fz_quad& quad)
	{
		const fz_rect hit = fz_rect_from_quad(quad);
		for (fz_stext_block* block = textPage->first_block; block; block = block->next)
		{
			if (block->type != FZ_STEXT_BLOCK_TEXT)
				continue;
			for (fz_stext_line* line = block->u.t.first_line; line; line = line->next)
			{
				if (line->bbox.x1 < hit.x0 || line->bbox.x0 > hit.x1 ||
					line->bbox.y1 < hit.y0 || line->bbox.y0 > hit.y1)
					continue;
				std::vector<int> runes;
				int hitIndex = -1;
				for (fz_stext_char* ch = line->first_char; ch; ch = ch->next)
				{
					const fz_rect charRect = fz_rect_from_quad(ch->quad);
					if (hitIndex < 0 && charRect.x1 >= hit.x0 && charRect.x0 <= hit.x1 &&
						charRect.y1 >= hit.y0 && charRect.y0 <= hit.y1)
						hitIndex = static_cast<int>(runes.size());
					runes.push_back(ch->c);
				}
				if (runes.empty())
					continue;
				const int start = max(0, (hitIndex < 0 ? 0 : hitIndex) - 24);
				const int end = min(static_cast<int>(runes.size()),
					(hitIndex < 0 ? 0 : hitIndex) + 56);
				std::string utf8;
				if (start > 0)
					utf8 = "...";
				for (int i = start; i < end; ++i)
				{
				char encoded[8] = {};
				const int length = fz_runetochar(encoded, runes[i]);
				if (length > 0)
					utf8.append(encoded, length);
				}
				if (end < static_cast<int>(runes.size()))
					utf8 += "...";
				CString context(CA2W(utf8.c_str(), CP_UTF8));
				context.Replace(_T("\t"), _T(" "));
				context.Trim();
				return context;
			}
		}
		return CString();
	}

	void FindCaseSensitiveHits(fz_stext_page* textPage, const std::vector<int>& needle,
		int pageNumber, std::vector<SearchMatch>& results)
	{
		if (needle.empty())
			return;
		for (fz_stext_block* block = textPage->first_block; block; block = block->next)
		{
			if (block->type != FZ_STEXT_BLOCK_TEXT)
				continue;
			for (fz_stext_line* line = block->u.t.first_line; line; line = line->next)
			{
				for (fz_stext_char* start = line->first_char; start; start = start->next)
				{
					fz_stext_char* current = start;
					fz_rect bounds = fz_rect_from_quad(start->quad);
					size_t matched = 0;
					while (current && matched < needle.size() && current->c == needle[matched])
					{
						bounds = fz_union_rect(bounds, fz_rect_from_quad(current->quad));
						current = current->next;
						++matched;
					}
					if (matched == needle.size())
					{
						SearchMatch match;
						match.pageNumber = pageNumber;
						match.quad = fz_quad_from_rect(bounds);
						results.push_back(match);
					}
				}
			}
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

BEGIN_MESSAGE_MAP(CPDFScrollBar, CWnd)
	ON_WM_PAINT()
	ON_WM_ERASEBKGND()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSEWHEEL()
	ON_WM_CAPTURECHANGED()
END_MESSAGE_MAP()

CPDFScrollBar::CPDFScrollBar(bool vertical)
	: m_pParentDlg(nullptr), m_vertical(vertical), m_contentLength(0), m_pageLength(0),
	m_position(0), m_dragOffset(0), m_dragging(false)
{
}

void CPDFScrollBar::SetRange(int contentLength, int pageLength, int position)
{
	contentLength = max(0, contentLength);
	pageLength = max(0, pageLength);
	position = max(0, min(position, max(0, contentLength - pageLength)));
	if (m_contentLength == contentLength && m_pageLength == pageLength && m_position == position)
		return;
	m_contentLength = contentLength;
	m_pageLength = pageLength;
	m_position = position;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

CRect CPDFScrollBar::ThumbRect() const
{
	CRect rc;
	GetClientRect(&rc);
	const int length = m_vertical ? rc.Height() : rc.Width();
	const int crossLength = m_vertical ? rc.Width() : rc.Height();
	const int buttonLength = min(16, length / 2);
	const int trackLength = max(0, length - buttonLength * 2);
	if (trackLength == 0)
		return CRect(0, 0, 0, 0);
	const int proportionalLength = m_contentLength > 0 ?
		static_cast<int>(static_cast<double>(trackLength) * m_pageLength / m_contentLength) : trackLength;
	const int horizontalLimit = max(1, trackLength / 20);
	const int thumbLength = m_vertical ?
		min(trackLength, max(24, min(44, proportionalLength))) :
		min(horizontalLimit, max(1, proportionalLength));
	const int maxScroll = max(0, m_contentLength - m_pageLength);
	const int travel = trackLength - thumbLength;
	const int start = buttonLength + (maxScroll > 0 ?
		static_cast<int>(static_cast<double>(m_position) * travel / maxScroll) : 0);
	const int crossSize = min(12, max(0, crossLength - 4));
	const int crossStart = (crossLength - crossSize) / 2;
	return m_vertical ?
		CRect(crossStart, start, crossStart + crossSize, start + thumbLength) :
		CRect(start, crossStart, start + thumbLength, crossStart + crossSize);
}

BOOL CPDFScrollBar::OnEraseBkgnd(CDC*)
{
	return TRUE;
}

void CPDFScrollBar::OnPaint()
{
	CPaintDC dc(this);
	CRect rc;
	GetClientRect(&rc);
	dc.FillSolidRect(&rc, RGB(250, 250, 250));
	if ((m_vertical ? rc.Height() : rc.Width()) < 32)
		return;
	const COLORREF grey = RGB(133, 133, 133);
	CPen pen(PS_SOLID, 1, grey);
	CPen* oldPen = dc.SelectObject(&pen);
	CBrush arrowBrush(grey);
	CBrush* oldBrush = dc.SelectObject(&arrowBrush);
	if (m_vertical)
	{
		const int cx = rc.Width() / 2;
		POINT first[] = { {cx, 6}, {cx - 5, 12}, {cx + 5, 12} };
		POINT last[] = { {cx - 5, rc.bottom - 12}, {cx + 5, rc.bottom - 12}, {cx, rc.bottom - 6} };
		dc.Polygon(first, 3);
		dc.Polygon(last, 3);
	}
	else
	{
		const int cy = rc.Height() / 2;
		POINT first[] = { {6, cy}, {12, cy - 5}, {12, cy + 5} };
		POINT last[] = { {rc.right - 12, cy - 5}, {rc.right - 12, cy + 5}, {rc.right - 6, cy} };
		dc.Polygon(first, 3);
		dc.Polygon(last, 3);
	}
	if (m_contentLength > m_pageLength)
	{
		CRect thumb = ThumbRect();
		dc.RoundRect(&thumb, CPoint(min(10, thumb.Width()), min(10, thumb.Height())));
	}
	dc.SelectObject(oldBrush);
	dc.SelectObject(oldPen);
}

void CPDFScrollBar::ScrollTo(int position)
{
	if (!m_pParentDlg || !m_pParentDlg->GetSafeHwnd())
		return;
	if (m_vertical)
		m_pParentDlg->ScrollToVerticalPosition(position);
	else
		m_pParentDlg->ScrollToHorizontalPosition(position);
}

void CPDFScrollBar::OnLButtonDown(UINT, CPoint point)
{
	CRect rc;
	GetClientRect(&rc);
	const int coordinate = m_vertical ? point.y : point.x;
	const int length = m_vertical ? rc.Height() : rc.Width();
	if (coordinate < 16)
		ScrollTo(m_position - 20);
	else if (coordinate >= length - 16)
		ScrollTo(m_position + 20);
	else if (m_contentLength > m_pageLength)
	{
		CRect thumb = ThumbRect();
		if (thumb.PtInRect(point))
		{
			m_dragging = true;
			m_dragOffset = coordinate - (m_vertical ? thumb.top : thumb.left);
			SetCapture();
		}
		else
			ScrollTo(m_position + (coordinate < (m_vertical ? thumb.top : thumb.left) ?
				-m_pageLength : m_pageLength));
	}
}

void CPDFScrollBar::OnMouseMove(UINT nFlags, CPoint point)
{
	if (!m_dragging || !(nFlags & MK_LBUTTON))
		return;
	CRect rc;
	GetClientRect(&rc);
	const int length = m_vertical ? rc.Height() : rc.Width();
	const CRect thumb = ThumbRect();
	const int thumbLength = m_vertical ? thumb.Height() : thumb.Width();
	const int travel = max(0, length - 32 - thumbLength);
	const int maxScroll = max(0, m_contentLength - m_pageLength);
	const int coordinate = m_vertical ? point.y : point.x;
	if (travel > 0)
		ScrollTo(static_cast<int>(static_cast<double>(
			max(0, min(travel, coordinate - m_dragOffset - 16))) * maxScroll / travel));
}

BOOL CPDFScrollBar::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
	if (!m_pParentDlg || !m_pParentDlg->GetSafeHwnd())
		return FALSE;
	CRect viewRect;
	m_pParentDlg->m_pdfView.GetWindowRect(&viewRect);
	if (viewRect.IsRectEmpty())
		return FALSE;
	pt.x = max(viewRect.left, min(pt.x, viewRect.right - 1));
	pt.y = max(viewRect.top, min(pt.y, viewRect.bottom - 1));
	return m_pParentDlg->OnMouseWheel(nFlags, zDelta, pt);
}

void CPDFScrollBar::OnLButtonUp(UINT, CPoint)
{
	if (m_dragging)
	{
		m_dragging = false;
		ReleaseCapture();
	}
}

void CPDFScrollBar::OnCaptureChanged(CWnd*)
{
	m_dragging = false;
}

// ============================================================================
// CPDFViewCtrl 实现（自定义PDF预览控件，支持滚动条）
// ============================================================================

BEGIN_MESSAGE_MAP(CPDFViewCtrl, CStatic)
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_MOUSEMOVE()
	ON_WM_RBUTTONUP()
	ON_WM_ERASEBKGND()
	ON_WM_VSCROLL()
	ON_WM_HSCROLL()
	ON_WM_PAINT()
END_MESSAGE_MAP()

// ★★★ SetDisplayBitmap：只存句柄 + 触发无擦除重绘，完全绕开 CStatic::STM_SETIMAGE 内部的白色背景
void CPDFViewCtrl::SetDisplayBitmap(HBITMAP hBitmap)
{
	m_hDisplayBitmap = hBitmap;
	HWND hwnd = GetSafeHwnd();
	if (hwnd && ::IsWindow(hwnd))
	{
		// FALSE = 不标记背景需要擦除，不会触发 WM_ERASEBKGND
		::InvalidateRect(hwnd, NULL, FALSE);
	}
}

void CPDFViewCtrl::ForwardMouseMessage(UINT message, UINT nFlags, CPoint point)
{
	if (!m_pParentDlg || !m_pParentDlg->GetSafeHwnd())
		return;
	if (message == WM_MOUSEMOVE && !m_pParentDlg->m_doc)
		return;
	ClientToScreen(&point);
	m_pParentDlg->ScreenToClient(&point);
	m_pParentDlg->SendMessage(message, nFlags, MAKELPARAM(point.x, point.y));
}

void CPDFViewCtrl::OnLButtonDown(UINT nFlags, CPoint point)
{
	ForwardMouseMessage(WM_LBUTTONDOWN, nFlags, point);
}

void CPDFViewCtrl::OnLButtonUp(UINT nFlags, CPoint point)
{
	ForwardMouseMessage(WM_LBUTTONUP, nFlags, point);
}

void CPDFViewCtrl::OnMouseMove(UINT nFlags, CPoint point)
{
	ForwardMouseMessage(WM_MOUSEMOVE, nFlags, point);
}

void CPDFViewCtrl::OnRButtonUp(UINT nFlags, CPoint point)
{
	if (!m_pParentDlg || !m_pParentDlg->GetSafeHwnd())
		return;
	ClientToScreen(&point);
	m_pParentDlg->SendMessage(WM_CONTEXTMENU,
		reinterpret_cast<WPARAM>(GetSafeHwnd()), MAKELPARAM(point.x, point.y));
}

BOOL CPDFViewCtrl::OnEraseBkgnd(CDC* pDC)
{
	return TRUE;
}

void CPDFViewCtrl::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	// 将滚动条消息转发给父对话框处理
	if (m_pParentDlg)
	{
		m_pParentDlg->SendMessage(WM_VSCROLL, MAKEWPARAM(nSBCode, nPos), (LPARAM)GetSafeHwnd());
	}
}

void CPDFViewCtrl::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar)
{
	// 将横向滚动条消息转发给父对话框处理
	if (m_pParentDlg)
	{
		m_pParentDlg->SendMessage(WM_HSCROLL, MAKEWPARAM(nSBCode, nPos), (LPARAM)GetSafeHwnd());
	}
}

// ★★★ 自定义绘制，支持滚动偏移
void CPDFViewCtrl::OnPaint()
{
	CPaintDC dc(this);  // 用于绘制的设备上下文

	// 获取客户区矩形
	CRect clientRect;
	GetClientRect(&clientRect);

	// 获取当前位图句柄（通过 SetDisplayBitmap 设置的）
	HBITMAP hBitmap = m_hDisplayBitmap;

	if (!hBitmap)
	{
		// 没有位图，填充背景色
		dc.FillSolidRect(&clientRect, PDF_VIEW_BACKGROUND);
		return;
	}

	// 获取位图信息
	BITMAP bm;
	::GetObject(hBitmap, sizeof(BITMAP), &bm);

	// ★★★ 判断是否为连续滚动模式
	bool isContinuousMode = false;
	if (m_pParentDlg)
	{
		isContinuousMode = m_pParentDlg->m_continuousScrollMode;
	}

	// ★★★ 双缓冲输出：在离屏 backDC 上先填背景再贴位图，最后一次性 BitBlt 到屏幕。
	// 屏幕只有一次写入，彻底消除"先灰/白后图"的中间态闪烁。
	CDC backDC;
	backDC.CreateCompatibleDC(&dc);
	CBitmap backBmp;
	backBmp.CreateCompatibleBitmap(&dc, clientRect.Width(), clientRect.Height());
	CBitmap* pOldBackBmp = backDC.SelectObject(&backBmp);

	// 将源位图选入临时 srcDC，用于后续 BitBlt
	CDC srcDC;
	srcDC.CreateCompatibleDC(&dc);
	HBITMAP hOldSrcBmp = (HBITMAP)srcDC.SelectObject(hBitmap);

	if (isContinuousMode)
	{
		// ★★★ 连续滚动模式：位图已经是可见区域，填灰色背景后居中贴图
		backDC.FillSolidRect(&clientRect, PDF_VIEW_BACKGROUND);

		int destX = 0;
		int destY = 0;

		if (bm.bmWidth < clientRect.Width())
			destX = (clientRect.Width() - bm.bmWidth) / 2;

		if (bm.bmHeight < clientRect.Height())
			destY = (clientRect.Height() - bm.bmHeight) / 2;

		int srcX  = max(0, -destX);
		int srcY  = max(0, -destY);
		int dstX  = max(0,  destX);
		int dstY  = max(0,  destY);
		int drawW = min(bm.bmWidth  - srcX, clientRect.Width()  - dstX);
		int drawH = min(bm.bmHeight - srcY, clientRect.Height() - dstY);
		if (drawW > 0 && drawH > 0)
			backDC.BitBlt(dstX, dstY, drawW, drawH, &srcDC, srcX, srcY, SRCCOPY);
	}
	else
	{
		// ★★★ 分页模式：位图是完整的 PDF 页面，根据滚动位置偏移后居中贴图
		int scrollX = 0;
		int scrollY = 0;
		if (m_pParentDlg)
		{
			scrollX = m_pParentDlg->m_scrollPositionH;
			scrollY = m_pParentDlg->m_scrollPosition;
		}

		backDC.FillSolidRect(&clientRect, PDF_VIEW_BACKGROUND);

		int destX = 0;
		int destY = 0;

		if (bm.bmWidth < clientRect.Width())
		{
			destX = (clientRect.Width() - bm.bmWidth) / 2;
		}
		else
		{
			destX = -scrollX;
		}

		if (bm.bmHeight < clientRect.Height())
			destY = (clientRect.Height() - bm.bmHeight) / 2;
		else
			destY = -scrollY;

		int srcX  = max(0, -destX);
		int srcY  = max(0, -destY);
		int dstX  = max(0,  destX);
		int dstY  = max(0,  destY);
		int drawW = min(bm.bmWidth  - srcX, clientRect.Width()  - dstX);
		int drawH = min(bm.bmHeight - srcY, clientRect.Height() - dstY);
		if (drawW > 0 && drawH > 0)
			backDC.BitBlt(dstX, dstY, drawW, drawH, &srcDC, srcX, srcY, SRCCOPY);
	}

	if (m_pParentDlg && (m_pParentDlg->m_hasSelection || m_pParentDlg->m_isSelecting))
		m_pParentDlg->UpdateSelectionHighlight(&backDC, m_pParentDlg->m_selPageNumber);

	// ★★★ 离屏合成完成，一次性输出到屏幕 DC
	dc.BitBlt(0, 0, clientRect.Width(), clientRect.Height(), &backDC, 0, 0, SRCCOPY);

	srcDC.SelectObject(hOldSrcBmp);
	backDC.SelectObject(pOldBackBmp);
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
	, m_wasMaximized(false)              // 初始化最大化状态标志
	, m_savedZoomMode(ZOOM_CUSTOM)       // 初始化保存的缩放模式
	, m_savedCustomZoom(1.0f)            // 初始化保存的自定义缩放值
	, m_savedPanOffset(0, 0)             // 初始化保存的平移偏移量
	, m_savedScrollPosition(0)           // 初始化保存的滚动位置
	, m_savedUniformScale(0.0f)          // 初始化保存的实际渲染缩放比例
	, m_zoomMode(ZOOM_CUSTOM)
	, m_customZoom(1.0f)
	, m_documentMaxPageWidth(0.0f)  // ★★★ 初始化文档最大页面宽度
	, m_documentUniformScale(1.0f)  // ★★★ 初始化文档统一缩放比例
	, m_thumbnailVisible(false)
	, m_outlineVisible(false)
	, m_searchResultsVisible(false)
	, m_updatingSearchResultsList(false)
	, m_isUpdatingOutline(false)
	, m_isUpdatingThumbnails(false)  // 初始化缩略图批量更新标记
	, m_isDragging(false)       // 初始化拖拽状态
	, m_isWindowDragging(false)
	, m_windowDragStartScreen(0, 0)
	, m_panOffset(0, 0)         // 初始化平移偏移量
	, m_hHandCursor(nullptr)    // 初始化手形光标
	, m_hHandCursorGrab(nullptr) // 初始化抓取光标
	, m_hPanPageBitmap(nullptr) // 初始化拖拽缓存位图
	, m_canDrag(false)          // ★★★ 初始化拖拽判断缓存
	, m_selectionMode(true)     // 默认文本选择模式
	, m_isSelecting(false)      // 未在选择中
	, m_hasSelection(false)     // 无选中区域
	, m_selPageNumber(-1)       // 未选择页码
	, m_selectionTextPage(nullptr)
	, m_activeDocIndex(-1)      // 初始化活动文档索引（-1表示无文档）
	, m_pLoadThread(nullptr)    // 初始化加载线程指针
	, m_pLoadParams(nullptr)    // 初始化加载参数
	, m_pProgressDlg(nullptr)   // 初始化进度对话框指针
	, m_closePending(false)
	, m_continuousScrollMode(true)  // ★★★ 默认启用连续滚动模式
	, m_scrollPosition(0)        // 初始化垂直滚动位置
	, m_scrollPositionH(0)       // 初始化横向滚动位置
	, m_totalScrollHeight(0)     // 初始化总滚动高度
	, m_totalScrollWidth(0)      // 初始化总滚动宽度
	, m_uniformScale(1.0f)       // ★★★ 初始化统一缩放比例（连续滚动模式用）
	, m_hContinuousViewBitmap(nullptr)  // ★★★ 初始化连续视图位图
	, m_isDraggingScrollbar(false)  // 初始化滚动条拖拽状态
	, m_scrollbarDragStartY(0)      // 初始化滚动条拖拽起始Y
	, m_scrollbarDragStartPos(0)    // 初始化滚动条拖拽起始位置
	, m_currentMatchIndex(-1)       // 初始化当前匹配项索引
	, m_searchCaseSensitive(false)  // 默认不区分大小写
	, m_pSearchThread(nullptr)      // 初始化搜索线程指针
	, m_pSearchParams(nullptr)      // 初始化搜索参数
	, m_isSearching(false)          // 初始化搜索状态
	, m_searchTaskId(0)
	, m_isThumbnailSubclassed(false) // 初始化缩略图子类化状态
	, m_lastThumbnailScrollTime(0)  // 初始化缩略图滚动时间
	, m_lastThumbnailHighlightTime(0) // 初始化缩略图高亮时间
	, m_isRenderingWheelZoom(false) // 初始化滚轮缩放重入标志
	, m_pendingResizeCx(0)          // ★★★ P1优化：防抖待处理宽度
	, m_pendingResizeCy(0)          // ★★★ P1优化：防抖待处理高度
	, m_lastResizeRenderTick(0)     // ★★★ 窗口拉伸过程中的节流重渲染时间戳
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	m_highlightBrush.CreateSolidBrush(RGB(173, 216, 230));  // 浅蓝色
	m_whiteBrush.CreateSolidBrush(RGB(240, 244, 248));
	m_toolbarBrush.CreateSolidBrush(RGB(255, 255, 255));   // 白色（Chrome/Edge 风格工具栏背景）
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
	CleanupPendingLoad();

	StopSearch();

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

	// ★★★ 重要：清空对话框中的所有资源指针，防止 ResourceRelease() 重复释放
	// 因为 CPDFDocument 的析构函数已经释放了这些资源
	m_ctx = nullptr;
	m_doc = nullptr;
	m_currentPageObj = nullptr;
	m_hCurrentBitmap = NULL;
	m_hPanPageBitmap = NULL;
	m_thumbnailCache.clear();  // 对话框侧不再持有缩略图位图所有权
	// 注意：m_pageCache 不在这里清空，让 ResourceRelease() 来清理
	// 否则位图不会被释放，导致内存泄漏

	// ★★★ 清理连续滚动视图位图
	SafeDeleteBitmap(m_hContinuousViewBitmap);

	ResourceRelease();

	// 在清理其他资源前先释放字体和画刷对象
	if (m_buttonFont.GetSafeHandle())
		m_buttonFont.DeleteObject();
	if (m_highlightBrush.GetSafeHandle())
		m_highlightBrush.DeleteObject();
	if (m_whiteBrush.GetSafeHandle())
		m_whiteBrush.DeleteObject();
}

void CXiaoGongPDFDlg::SaveDialogStateToDocument(CPDFDocument* pDoc, bool transferBitmaps)
{
	if (!pDoc)
		return;

	if (m_currentPage >= 0 && m_currentPage < m_totalPages)
	{
		pDoc->SetCurrentPage(m_currentPage);
		pDoc->SetZoom(m_customZoom, m_zoomMode);
		pDoc->SetPanOffset(m_panOffset);
		pDoc->SetCanDrag(m_canDrag);
		pDoc->SaveCurrentPageZoomState();
	}

	pDoc->SetUniformScale(m_uniformScale);
	pDoc->SetScrollPosition(m_scrollPosition);
	pDoc->SetThumbnailPicWidth(m_thumbnailPicWidth);
	pDoc->SetThumbnailPicHeight(m_thumbnailPicHeight);
	pDoc->SetSearchMatches(m_searchMatches);
	pDoc->SetCurrentMatchIndex(m_currentMatchIndex);

	CString searchKeyword;
	if (m_editSearch.GetSafeHwnd())
		m_editSearch.GetWindowText(searchKeyword);
	else
		searchKeyword = m_searchKeyword;
	pDoc->SetSearchKeyword(searchKeyword);

	pDoc->GetPageRotations() = m_pageRotations;

	if (!transferBitmaps)
		return;

	if (m_hCurrentBitmap)
	{
		BITMAP bm;
		if (::GetObject(m_hCurrentBitmap, sizeof(BITMAP), &bm) != 0)
			pDoc->SetCurrentBitmap(m_hCurrentBitmap);
		m_hCurrentBitmap = NULL;
	}

	if (m_hPanPageBitmap)
	{
		BITMAP bm;
		if (::GetObject(m_hPanPageBitmap, sizeof(BITMAP), &bm) != 0)
			pDoc->SetPanPageBitmap(m_hPanPageBitmap);
		m_hPanPageBitmap = NULL;
	}
}

void CXiaoGongPDFDlg::LoadDialogStateFromDocument(CPDFDocument* pDoc)
{
	if (!pDoc)
		return;

	ClearSelection();
	m_ctx = pDoc->GetContext();
	m_doc = pDoc->GetDocument();
	m_totalPages = pDoc->GetTotalPages();
	m_currentPage = pDoc->GetCurrentPage();
	m_zoom = pDoc->GetZoom();
	m_zoomMode = pDoc->GetZoomMode();
	m_customZoom = pDoc->GetCustomZoom();
	m_panOffset = pDoc->GetPanOffset();
	m_canDrag = pDoc->GetCanDrag();
	m_scrollPosition = pDoc->GetScrollPosition();
	m_thumbnailPicWidth = pDoc->GetThumbnailPicWidth();
	m_thumbnailPicHeight = pDoc->GetThumbnailPicHeight();
	m_searchMatches = pDoc->GetSearchMatches();
	m_currentMatchIndex = pDoc->GetCurrentMatchIndex();
	m_searchKeyword = pDoc->GetSearchKeyword();
	m_pageRotations = pDoc->GetPageRotations();
	m_hCurrentBitmap = pDoc->TransferCurrentBitmap();
	m_hPanPageBitmap = pDoc->TransferPanPageBitmap();
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
}

void CXiaoGongPDFDlg::RestoreReadingPosition(CPDFDocument* pDoc)
{
	if (!pDoc || pDoc->GetFilePath().IsEmpty())
		return;
	CString key = ReadingStateKey(pDoc->GetFilePath());
	CWinApp* pApp = AfxGetApp();
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

void CXiaoGongPDFDlg::CloseProgressDialog()
{
	if (!m_pProgressDlg)
		return;

	if (m_pProgressDlg->GetSafeHwnd() && ::IsWindow(m_pProgressDlg->GetSafeHwnd()))
		m_pProgressDlg->DestroyWindow();

	delete m_pProgressDlg;
	m_pProgressDlg = nullptr;
}

void CXiaoGongPDFDlg::ReleaseLoadThread()
{
	if (!m_pLoadThread)
		return;

	if (m_pLoadThread->m_hThread)
		WaitForSingleObject(m_pLoadThread->m_hThread, INFINITE);

	delete m_pLoadThread;
	m_pLoadThread = nullptr;
}

void CXiaoGongPDFDlg::CleanupPendingLoad()
{
	if (m_pLoadParams)
		m_pLoadParams->cancelled.store(true);
	CloseProgressDialog();
	ReleaseLoadThread();

	if (m_pLoadParams)
	{
		if (m_pLoadParams->pDoc)
		{
			delete m_pLoadParams->pDoc;
			m_pLoadParams->pDoc = nullptr;
		}

		delete m_pLoadParams;
		m_pLoadParams = nullptr;
	}
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
	SafeDeleteBitmap(m_hContinuousViewBitmap);

	// 清理控件中的位图引用
	m_pdfView.SetDisplayBitmap(NULL);
}

void CXiaoGongPDFDlg::SafeDeleteBitmap(HBITMAP& hBmp)
{
	if (hBmp) {
		DeleteObject(hBmp);
		hBmp = NULL;
	}
}

void CXiaoGongPDFDlg::RemoveFromCacheOrder(const PageCacheKey& key)
{
	auto it = std::find(m_cacheOrder.begin(), m_cacheOrder.end(), key);
	if (it != m_cacheOrder.end())
		m_cacheOrder.erase(it);
}

HBITMAP CXiaoGongPDFDlg::CreatePageDIBSection(int w, int h, HDC hDC, BYTE** ppBits)
{
	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = w;
	bmi.bmiHeader.biHeight = -h;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 24;
	bmi.bmiHeader.biCompression = BI_RGB;
	return CreateDIBSection(hDC, &bmi, DIB_RGB_COLORS, (void**)ppBits, NULL, 0);
}

void CXiaoGongPDFDlg::CopyMuPDFPixelsToBuffer(fz_pixmap* pixmap, BYTE* pbBits, int width, int height)
{
	unsigned char* samples = fz_pixmap_samples(m_ctx, pixmap);
	int stride = (width * 3 + 3) & ~3;
	int n = fz_pixmap_components(m_ctx, pixmap);
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			pbBits[y * stride + x * 3 + 0] = samples[(y * width + x) * n + 2];
			pbBits[y * stride + x * 3 + 1] = samples[(y * width + x) * n + 1];
			pbBits[y * stride + x * 3 + 2] = samples[(y * width + x) * n + 0];
		}
}

// 共用渲染函数：将 MuPDF 页面渲染为 HBITMAP，消除多处重复的渲染流水线
HBITMAP CXiaoGongPDFDlg::RenderMuPDFPageToBitmap(fz_context* ctx, fz_page* page, float scale,
	int rotation, HDC hDC, int& outWidth, int& outHeight)
{
	if (!ctx || !page || scale <= 0.0f)
		return nullptr;

	fz_pixmap* pixmap = nullptr;
	fz_device* dev = nullptr;
	HBITMAP hBitmap = nullptr;

	fz_try(ctx)
	{
		fz_rect bounds = fz_bound_page(ctx, page);
		float pageWidth = bounds.x1 - bounds.x0;
		float pageHeight = bounds.y1 - bounds.y0;

		int width = ClampRenderDimension(pageWidth * scale);
		int height = ClampRenderDimension(pageHeight * scale);

		pixmap = fz_new_pixmap(ctx, fz_device_rgb(ctx), width, height, nullptr, 1);
		fz_clear_pixmap_with_value(ctx, pixmap, 0xff);

		fz_matrix ctm = fz_scale(scale, scale);
		dev = fz_new_draw_device(ctx, ctm, pixmap);
		fz_run_page(ctx, page, dev, fz_identity, nullptr);
		fz_close_device(ctx, dev);
		fz_drop_device(ctx, dev);
		dev = nullptr;

		BITMAPINFO bmi = { 0 };
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = width;
		bmi.bmiHeader.biHeight = -height;
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 24;
		bmi.bmiHeader.biCompression = BI_RGB;

		BYTE* pbBits = nullptr;
		hBitmap = CreateDIBSection(hDC, &bmi, DIB_RGB_COLORS, (void**)&pbBits, NULL, 0);

		if (hBitmap && pbBits)
		{
			unsigned char* samples = fz_pixmap_samples(ctx, pixmap);
			int stride = (width * 3 + 3) & ~3;
			int n = fz_pixmap_components(ctx, pixmap);
			for (int y = 0; y < height; y++)
				for (int x = 0; x < width; x++) {
					pbBits[y * stride + x * 3 + 0] = samples[(y * width + x) * n + 2];
					pbBits[y * stride + x * 3 + 1] = samples[(y * width + x) * n + 1];
					pbBits[y * stride + x * 3 + 2] = samples[(y * width + x) * n + 0];
				}
		}

		outWidth = width;
		outHeight = height;
	}
	fz_always(ctx)
	{
		if (pixmap) fz_drop_pixmap(ctx, pixmap);
		if (dev) fz_drop_device(ctx, dev);
	}
	fz_catch(ctx)
	{
		if (hBitmap) { DeleteObject(hBitmap); hBitmap = nullptr; }
		outWidth = 0;
		outHeight = 0;
	}

	return hBitmap;
}

void CXiaoGongPDFDlg::SetControlsVisible(bool visible)
{
	int nShow = visible ? SW_SHOW : SW_HIDE;
	CWnd* controls[] = {
		&m_toolbar, &m_tabCtrl, &m_btnOpen, &m_btnZoomOut, &m_btnFitWidth, &m_btnFitPage,
		&m_btnZoomIn, &m_btnMore, &m_btnMinimize, &m_btnMaximize,
		&m_btnCloseWindow, &m_btnFirst, &m_btnLast,
		&m_btnFullscreen, &m_btnRotateLeft, &m_btnRotateRight,
		&m_btnEdit, &m_btnSelectText, &m_editCurrent, &m_pageTotal,
		&m_statusBar, &m_btnOutline,
		&m_editSearch, &m_btnFind, &m_btnPrevMatch, &m_btnNextMatch,
		&m_outlineTree, &m_searchResultsList, &m_searchResultsHeader,
		&m_btnCloseSearchResults
	};
	for (auto* pCtrl : controls)
		if (pCtrl->GetSafeHwnd()) pCtrl->ShowWindow(nShow);
	if (visible)
	{
		CRect clientRect;
		GetClientRect(&clientRect);
		if (clientRect.Width() < 980)
		{
			m_btnZoomOut.ShowWindow(SW_HIDE);
			m_btnFitWidth.ShowWindow(SW_HIDE);
			m_btnFitPage.ShowWindow(SW_HIDE);
			m_btnZoomIn.ShowWindow(SW_HIDE);
		}
	}
	if (m_thumbnailList.GetSafeHwnd())
		m_thumbnailList.ShowWindow(SW_HIDE);
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
	if (!m_continuousScrollMode)
	{
		fz_rect bounds;
		float scale = 0.0f;
		int offsetX = 0;
		int offsetY = 0;
		if (!GetSelectionPageTransform(pageNum, bounds, scale, offsetX, offsetY))
			return;
		const CRect matchRect = TransformQuadToBitmapRect(quad, bounds,
			GetPageRotation(pageNum), scale);
		CRect viewRect;
		m_pdfView.GetClientRect(&viewRect);
		const int matchY = matchRect.top + offsetY;
		if (matchY < 0 || matchY >= viewRect.Height())
			ScrollToVerticalPosition(m_scrollPosition + matchY - viewRect.Height() / 3);
		const int matchX = matchRect.left + offsetX;
		if (matchX < 0 || matchX >= viewRect.Width())
			ScrollToHorizontalPosition(m_scrollPositionH + matchX - viewRect.Width() / 3);
		return;
	}
	if (pageNum < 0 || pageNum >= (int)m_pageYPositions.size() || pageNum >= (int)m_pageHeights.size())
		return;

	fz_rect bounds;
	if (!TryGetPageBaseBounds(pageNum, bounds))
		return;

	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	CRect matchRect = TransformQuadToBitmapRect(quad, bounds,
		GetPageRotation(pageNum), m_uniformScale);
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
	DDX_Control(pDX, IDC_THUMBNAIL_LIST, m_thumbnailList);
	DDX_Control(pDX, IDC_TOOLBAR, m_toolbar);
	DDX_Control(pDX, IDC_BTN_FIRST, m_btnFirst);
	DDX_Control(pDX, IDC_BTN_LAST, m_btnLast);
	DDX_Control(pDX, IDC_BTN_FULLSCREEN, m_btnFullscreen);
	DDX_Control(pDX, IDC_BTN_ROTATE_LEFT, m_btnRotateLeft);
	DDX_Control(pDX, IDC_BTN_ROTATE_RIGHT, m_btnRotateRight);
	DDX_Control(pDX, IDC_BTN_EDIT, m_btnEdit);
	DDX_Control(pDX, IDC_BTN_SELECT_TEXT, m_btnSelectText);
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
	ON_COMMAND(ID_MENU_FIT_WIDTH, &CXiaoGongPDFDlg::OnBtnFitWidth)
	ON_COMMAND(ID_MENU_FIT_PAGE, &CXiaoGongPDFDlg::OnBtnFitPage)
	ON_COMMAND(ID_MENU_SEARCH_RESULTS, &CXiaoGongPDFDlg::OnMenuSearchResults)
	ON_COMMAND_RANGE(ID_MENU_RECENT_FILE_1, ID_MENU_RECENT_FILE_10, &CXiaoGongPDFDlg::onMenuRecentFile)
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_THUMBNAIL_LIST, &CXiaoGongPDFDlg::OnThumbnailItemChanged)
	ON_NOTIFY(TVN_SELCHANGED, IDC_OUTLINE_TREE, &CXiaoGongPDFDlg::OnOutlineSelectionChanged)
	ON_NOTIFY(LVN_ITEMCHANGED, IDC_SEARCH_RESULTS_LIST, &CXiaoGongPDFDlg::OnSearchResultSelected)
	ON_NOTIFY(TCN_SELCHANGE, IDC_TAB_CONTROL, &CXiaoGongPDFDlg::OnTabSelChange)
	ON_MESSAGE(WM_APP + 100, &CXiaoGongPDFDlg::OnTabCloseButton)  // 标签页关闭按钮消息
	ON_WM_SIZE()
	ON_MESSAGE(WM_USER + 100, &CXiaoGongPDFDlg::OnRenderThumbnailsAsync)
	ON_MESSAGE(WM_USER + 101, &CXiaoGongPDFDlg::OnOpenInitialFile)  // 延迟打开初始文件
	ON_MESSAGE(WM_USER + 102, &CXiaoGongPDFDlg::OnThumbnailScroll)  // 缩略图滚动事件
	ON_MESSAGE(WM_USER + 103, &CXiaoGongPDFDlg::OnUpdateThumbnailHighlight)  // 延迟更新缩略图高亮
	ON_MESSAGE(WM_PDF_LOAD_COMPLETE, &CXiaoGongPDFDlg::OnPDFLoadComplete)  // PDF后台加载完成
	ON_MESSAGE(WM_PDF_LOAD_CANCELLED, &CXiaoGongPDFDlg::OnPDFLoadCancelled)
	ON_MESSAGE(WM_SEARCH_COMPLETE, &CXiaoGongPDFDlg::OnSearchComplete)  // 搜索后台完成
	ON_MESSAGE(WM_SEARCH_PROGRESS, &CXiaoGongPDFDlg::OnSearchProgress)
	ON_WM_TIMER()  // 定时器消息处理（用于去抖动缩略图高亮更新）
	ON_WM_MOUSEWHEEL()
	ON_BN_CLICKED(IDC_BTN_FIRST, &CXiaoGongPDFDlg::OnBtnFirst)
	ON_BN_CLICKED(IDC_BTN_OPEN, &CXiaoGongPDFDlg::onMenuOpen)
	ON_BN_CLICKED(IDC_BTN_ZOOM_OUT, &CXiaoGongPDFDlg::OnBtnZoomOut)
	ON_BN_CLICKED(IDC_BTN_FIT_WIDTH, &CXiaoGongPDFDlg::OnBtnFitWidth)
	ON_BN_CLICKED(IDC_BTN_FIT_PAGE, &CXiaoGongPDFDlg::OnBtnFitPage)
	ON_BN_CLICKED(IDC_BTN_ZOOM_IN, &CXiaoGongPDFDlg::OnBtnZoomIn)
	ON_BN_CLICKED(IDC_BTN_MORE, &CXiaoGongPDFDlg::OnBtnMore)
	ON_BN_CLICKED(IDC_BTN_CLOSE_SEARCH_RESULTS, &CXiaoGongPDFDlg::OnCloseSearchResults)
	ON_BN_CLICKED(IDC_BTN_MINIMIZE, &CXiaoGongPDFDlg::OnBtnMinimize)
	ON_BN_CLICKED(IDC_BTN_MAXIMIZE, &CXiaoGongPDFDlg::OnBtnMaximize)
	ON_BN_CLICKED(IDC_BTN_CLOSE_WINDOW, &CXiaoGongPDFDlg::OnBtnCloseWindow)
	ON_BN_CLICKED(IDC_BTN_LAST, &CXiaoGongPDFDlg::OnBtnLast)
	ON_BN_CLICKED(IDC_BTN_FULLSCREEN, &CXiaoGongPDFDlg::OnBtnFullscreen)
	ON_BN_CLICKED(IDC_BTN_ROTATE_LEFT, &CXiaoGongPDFDlg::OnBtnRotateLeft)
	ON_BN_CLICKED(IDC_BTN_ROTATE_RIGHT, &CXiaoGongPDFDlg::OnBtnRotateRight)
	ON_BN_CLICKED(IDC_BTN_EDIT, &CXiaoGongPDFDlg::OnBtnEdit)
	ON_BN_CLICKED(IDC_BTN_SELECT_TEXT, &CXiaoGongPDFDlg::OnBtnSelectText)
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

	// 标签栏承担标题区，保留系统窗口的缩放边框。
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
	m_pdfView.ModifyStyle(WS_VSCROLL | WS_HSCROLL, SS_NOTIFY | SS_CENTERIMAGE);

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
	m_pdfScrollCorner.Create(_T(""), WS_CHILD | SS_WHITERECT,
		CRect(0, 0, 16, 16), this, 0);

	// 初始状态隐藏滚动条（未加载PDF时不显示）
	m_pdfView.ShowScrollBar(SB_VERT, FALSE);
	m_pdfView.ShowScrollBar(SB_HORZ, FALSE);

	// 获取系统垂直滚动条的宽度
	m_scrollBarWidth = GetSystemMetrics(SM_CXVSCROLL);

	m_thumbnailList.ModifyStyle(WS_VISIBLE, 0);
	m_thumbnailList.ModifyStyle(WS_BORDER, 0, SWP_FRAMECHANGED);
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

	// 初始化状态栏
	m_statusBar.SetWindowText(_T("  就绪"));
	m_statusBar.ModifyStyle(SS_TYPEMASK | WS_BORDER, SS_LEFT | SS_CENTERIMAGE, SWP_FRAMECHANGED);
	m_statusBar.ShowWindow(SW_SHOW);

	// MuPDF context 由每个 CPDFDocument 自己持有，主对话框只引用当前活动文档。

	// 初始化工具栏
	m_btnOpen.Create(_T("打开"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
		CRect(0, 0, 72, 34), this, IDC_BTN_OPEN);
	m_btnZoomOut.Create(_T("-"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_ZOOM_OUT);
	m_btnFitWidth.Create(_T("适合宽度"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		CRect(0, 0, 76, 34), this, IDC_BTN_FIT_WIDTH);
	m_btnFitPage.Create(_T("适合页面"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		CRect(0, 0, 76, 34), this, IDC_BTN_FIT_PAGE);
	m_btnZoomIn.Create(_T("+"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_ZOOM_IN);
	m_btnMore.Create(_T("更多"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		CRect(0, 0, 34, 34), this, IDC_BTN_MORE);
	m_btnMinimize.Create(_T("最小化"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		CRect(0, 0, 46, 42), this, IDC_BTN_MINIMIZE);
	m_btnMaximize.Create(_T("最大化"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		CRect(0, 0, 46, 42), this, IDC_BTN_MAXIMIZE);
	m_btnCloseWindow.Create(_T("关闭"), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
		CRect(0, 0, 46, 42), this, IDC_BTN_CLOSE_WINDOW);
	m_btnMinimize.SetWindowControlStyle();
	m_btnMaximize.SetWindowControlStyle();
	m_btnCloseWindow.SetWindowControlStyle(true);
	InitializeToolbar();
	m_statusBar.SetFont(&m_buttonFont);
	m_outlineTree.SetFont(&m_buttonFont);
	m_searchResultsHeader.SetFont(&m_buttonFont);
	m_searchResultsList.SetFont(&m_buttonFont);
	m_btnCloseSearchResults.SetIconGlyph(0xE8BB);
	m_btnCloseSearchResults.SetToolTipText(_T("关闭搜索结果"));
	m_btnCloseSearchResults.SetColors(RGB(255, 255, 255), RGB(232, 241, 252), RGB(211, 229, 250));

	m_btnOutline.SetWindowText(_T(""));

	// 初始化标签页控件
	InitializeTabControl();

	// 设置对话框为可接收键盘消息的窗口
	SetFocus();

	// 启用拖放文件功能
	DragAcceptFiles(TRUE);

	// 如果有从命令行传入的初始文件路径，延迟打开该文件
	// 使用PostMessage延迟打开，避免阻塞窗口显示
	if (!m_initialFilePath.IsEmpty())
	{
		// 使用PostMessage延迟打开，让窗口先显示
		PostMessage(WM_USER + 101, 0, 0);
	}


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
	m_btnSelectText.SetColors(normal, hover, pressed);
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
	m_btnSelectText.SetTextColor(RGB(32, 33, 36));
	m_btnFind.SetTextColor      (RGB(32, 33, 36));
	m_btnPrevMatch.SetTextColor (RGB(32, 33, 36));
	m_btnNextMatch.SetTextColor (RGB(32, 33, 36));

	// ===== 颜色配置结束 =====
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

		// 在工具栏底部绘制 1px 分隔线（Chrome/Edge 风格）
		if (m_toolbar.GetSafeHwnd())
		{
			CRect toolbarRect;
			m_toolbar.GetWindowRect(&toolbarRect);
			ScreenToClient(&toolbarRect);

			CDC* pDC = GetDC();
			if (pDC)
			{
				CPen pen(PS_SOLID, 1, RGB(212, 220, 228));
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

	CBrush brush(PDF_VIEW_BACKGROUND);
	pDC->FillRect(&rect, &brush);
	pDC->FillSolidRect(0, 0, rect.Width(), 42, RGB(240, 244, 248));

	return TRUE;  // 返回TRUE表示我们已经处理了背景擦除
}

HBRUSH CXiaoGongPDFDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT nCtlColor)
{
	HBRUSH hbr = CDialogEx::OnCtlColor(pDC, pWnd, nCtlColor);

	// 标签区与工具栏分别使用柔和灰色和白色。
	if (pWnd->GetSafeHwnd() == m_tabCtrl.GetSafeHwnd())
	{
		pDC->SetBkColor(RGB(240, 244, 248));
		return m_whiteBrush;
	}
	if (pWnd->GetSafeHwnd() == m_statusBar.GetSafeHwnd())
	{
		pDC->SetBkColor(RGB(255, 255, 255));
		pDC->SetTextColor(RGB(75, 88, 102));
		return m_toolbarBrush;
	}
	if (pWnd->GetSafeHwnd() == m_searchResultsHeader.GetSafeHwnd())
	{
		pDC->SetBkColor(RGB(255, 255, 255));
		pDC->SetTextColor(RGB(38, 45, 53));
		return m_toolbarBrush;
	}
	// 工具栏背景：白色（Chrome/Edge 风格）
	if (pWnd->GetSafeHwnd() == m_toolbar.GetSafeHwnd())
	{
		pDC->SetBkColor(RGB(255, 255, 255));
		return m_toolbarBrush;
	}
	if (pWnd->GetSafeHwnd() == m_pageTotal.GetSafeHwnd())
	{
		pDC->SetBkColor(RGB(255, 255, 255));
		pDC->SetTextColor(RGB(32, 33, 36));
		return m_toolbarBrush;
	}

	// 工具栏内的编辑框（页码输入、搜索框）：白色背景，深色文字
	if (nCtlColor == CTLCOLOR_EDIT &&
		(pWnd->GetSafeHwnd() == m_editCurrent.GetSafeHwnd() ||
		 pWnd->GetSafeHwnd() == m_editSearch.GetSafeHwnd()))
	{
		pDC->SetBkColor(RGB(255, 255, 255));
		pDC->SetTextColor(RGB(32, 33, 36));
		return m_toolbarBrush;
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
			m_pdfView.SetDisplayBitmap(it->second.hBitmap);
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
			m_pdfView.SetDisplayBitmap(m_hCurrentBitmap);
		}

		// 强制刷新控件显示
		RefreshCurrentView();

		m_currentPage = pageNumber;
		UpdatePageControls();
		HighlightCurrentThumbnail();

		// 更新使用顺序（LRU）
		RemoveFromCacheOrder(key);
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
	CDC* pDC = nullptr;  // 在 fz_try 前声明，fz_catch 中需要释放

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
			scale = FitWidthScale(viewWidth, effectivePageWidth);
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
		int width = ClampRenderDimension(origPageWidth * scale);
		int height = ClampRenderDimension(origPageHeight * scale);
		pixmap = fz_new_pixmap(m_ctx, fz_device_rgb(m_ctx), width, height, nullptr, 1);
		fz_clear_pixmap_with_value(m_ctx, pixmap, 0xff);

		// 渲染页面
		fz_matrix ctm = fz_scale(scale, scale);
		dev = fz_new_draw_device(m_ctx, ctm, pixmap);
		fz_run_page(m_ctx, m_currentPageObj, dev, fz_identity, nullptr);
		fz_close_device(m_ctx, dev);
		fz_drop_device(m_ctx, dev);
		dev = nullptr;

		// 创建DIB位图
		pDC = m_pdfView.GetDC();
		BYTE* pbBits = nullptr;
		m_hCurrentBitmap = CreatePageDIBSection(width, height, pDC->GetSafeHdc(), &pbBits);

		if (m_hCurrentBitmap && pbBits)
		{
			// 复制像素数据（RGB→BGR）
			CopyMuPDFPixelsToBuffer(pixmap, pbBits, width, height);

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
				BYTE* pNewBits = nullptr;
				HBITMAP hNewBitmap = CreatePageDIBSection(viewWidth, viewHeight, pDC->GetSafeHdc(), &pNewBits);

				if (hNewBitmap && pNewBits)
				{
					// 创建内存DC
					CDC memDC, targetDC;
					memDC.CreateCompatibleDC(pDC);
					targetDC.CreateCompatibleDC(pDC);

					HBITMAP hOldBmp1 = (HBITMAP)memDC.SelectObject(m_hCurrentBitmap);
					HBITMAP hOldBmp2 = (HBITMAP)targetDC.SelectObject(hNewBitmap);

					// 填充浅灰色背景（让白色PDF页面更明显）
					targetDC.FillSolidRect(0, 0, viewWidth, viewHeight, PDF_VIEW_BACKGROUND);

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
			m_pdfView.SetDisplayBitmap(m_hCurrentBitmap);

			// 强制刷新控件显示
			RefreshCurrentView();
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
			// 拷贝当前页的位图（防止后续被释放）
			HBITMAP hCopy = (HBITMAP)CopyImage(
				m_hCurrentBitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

			if (hCopy)
			{
				PageCacheItem item{ hCopy };
				m_pageCache[key] = item;
				m_cacheOrder.push_front(key);
				TrimBitmapCache(m_pageCache, m_cacheOrder, false,
					PAGE_CACHE_BUDGET, CACHE_LIMIT);

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
		if (pDC) { m_pdfView.ReleaseDC(pDC); pDC = nullptr; }
		CleanupCurrentPage();
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

		// ★★★ 更新滚动条范围（用于横向滚动条）
		m_totalScrollWidth = bm.bmWidth;
		m_totalScrollHeight = bm.bmHeight;
		UpdateScrollBar();
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

	// ★★★ 延迟更新缩略图高亮，避免频繁调用阻塞
	if (m_thumbnailVisible && m_thumbnailList.GetSafeHwnd())
	{
		PostMessage(WM_USER + 103, 0, 0);  // 延迟更新缩略图
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

	// ★★★ 子类化缩略图列表控件以捕获滚动事件
	if (!m_isThumbnailSubclassed && m_thumbnailList.GetSafeHwnd()) {
		::SetWindowLongPtr(m_thumbnailList.GetSafeHwnd(), GWLP_USERDATA, (LONG_PTR)this);
		m_isThumbnailSubclassed = true;
	}
}

bool CXiaoGongPDFDlg::RenderThumbnail(int pageNumber)
{
#ifdef _DEBUG
	if (pageNumber < 10 || (pageNumber % 100) == 0)
	{
		TRACE(_T("RenderThumbnail() :  %d\n"), pageNumber);
	}
#endif

	if (!m_ctx || !m_doc || pageNumber < 0 || pageNumber >= m_totalPages)
		return false;

	// ★★★ 获取当前活动文档，统一使用文档对象的缓存
	CPDFDocument* pDoc = GetActiveDocument();
	if (!pDoc) {
#ifdef _DEBUG
		TRACE(_T("RenderThumbnail: 无法获取活动文档\n"));
#endif
		return false;
	}

	// ★★★ 统一使用文档对象的缩略图缓存（避免缓存不同步）
	auto& docThumbnailCache = pDoc->GetThumbnailCache();
	auto it = docThumbnailCache.find(pageNumber);
	bool useCache = (it != docThumbnailCache.end());

	HBITMAP hBitmap = NULL;
	int thumbWidth = THUMBNAIL_WIDTH;
	int thumbHeight = 0;

	if (useCache)
	{
		// 使用缓存的位图
		hBitmap = it->second.hBitmap;

		// ★★★ 更新LRU顺序：将当前页面移到最前面（表示最近使用）
		pDoc->UpdateThumbnailLRU(pageNumber);

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
			float safePageWidth = (pageWidth > 0.0f) ? pageWidth : 1.0f;
			float scale = thumbWidth / safePageWidth;
			int pageThumbHeight = ClampRenderDimension(pageHeight * scale);

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
			thumbHeight = max(1, thumbHeight);

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

				// ★★★ 添加到文档对象的缓存（统一缓存管理）
				auto oldThumbIt = docThumbnailCache.find(pageNumber);
				if (oldThumbIt != docThumbnailCache.end() && oldThumbIt->second.hBitmap &&
					oldThumbIt->second.hBitmap != hBitmap)
				{
					DeleteObject(oldThumbIt->second.hBitmap);
				}

				ThumbnailInfo info = { hBitmap, pageNumber, pageNumber == m_currentPage };
				docThumbnailCache[pageNumber] = info;

				// ★★★ 更新LRU顺序：新渲染的缩略图添加到最前面
				pDoc->UpdateThumbnailLRU(pageNumber);

#ifdef _DEBUG
				if (pageNumber < 10 || (pageNumber % 100) == 0)
				{
					TRACE(_T("新渲染缩略图: 页面 %d, 尺寸 %d x %d\n"), pageNumber, thumbWidth, thumbHeight);
				}
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
		int itemIndex = -1;
		for (int i = 0; i < m_thumbnailList.GetItemCount(); i++)
		{
			if (m_thumbnailList.GetItemData(i) == (DWORD_PTR)pageNumber)
			{
				itemIndex = i;
				break;
			}
		}

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
			if (pImageList->Create(thumbWidth, thumbHeight, ILC_COLOR32, m_totalPages, 1))
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
			int imageIndex = -1;
			auto imageIndexIt = m_thumbnailImageIndices.find(pageNumber);
			if (imageIndexIt != m_thumbnailImageIndices.end() && imageIndexIt->second >= 0)
			{
				CBitmap* pBitmap = CBitmap::FromHandle(hBitmap);
				if (pBitmap && pImageList->Replace(imageIndexIt->second, pBitmap, nullptr))
				{
					imageIndex = imageIndexIt->second;
				}
			}

			if (imageIndex == -1)
			{
				imageIndex = pImageList->Add(CBitmap::FromHandle(hBitmap), (CBitmap*)nullptr);
			}

			if (imageIndex != -1)
			{
				m_thumbnailImageIndices[pageNumber] = imageIndex;

				CString strPageNum;
				strPageNum.Format(_T("第 %d 页"), pageNumber + 1);

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
			if (pageNumber < 10 || (pageNumber % 100) == 0)
			{
				TRACE(_T("添加缩略图到ImageList: 页面 %d, 索引 %d\n"), pageNumber, imageIndex);
			}
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
	CPDFDocument* pDebugDoc = GetActiveDocument();
	TRACE(_T("缩略图缓存数量: %d\n"), pDebugDoc ? (int)pDebugDoc->GetThumbnailCache().size() : 0);
#endif

	if (!m_doc || m_totalPages <= 0)
	{
#ifdef _DEBUG
		TRACE(_T("UpdateThumbnails() 跳过（无文档或无页面）\n"));
#endif
		return;
	}

	m_isUpdatingThumbnails = true;

	if (m_thumbnailList.GetSafeHwnd())
	{
		m_thumbnailList.SetRedraw(FALSE);
	}

	m_thumbnailImageIndices.clear();

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

	// 先为所有页面建立占位项，后续再按需填充缩略图图像。
	// 否则大于异步预渲染上限的页面不会出现在列表中，用户无法滚动到后续页面。
	for (int i = 0; i < m_totalPages; i++)
	{
		CString strPageNum;
		strPageNum.Format(_T("第 %d 页"), i + 1);
		LVITEM item = { 0 };
		item.mask = LVIF_TEXT;
		item.iItem = i;
		item.iSubItem = 0;
		item.pszText = const_cast<LPTSTR>((LPCTSTR)strPageNum);
		int itemIndex = m_thumbnailList.InsertItem(&item);
		if (itemIndex != -1)
		{
			m_thumbnailList.SetItemData(itemIndex, (DWORD_PTR)i);
		}
	}

	// ★★★ 优化：先只渲染前面部分缩略图，让界面快速响应
	// 对于页数多的PDF，避免阻塞UI线程和内存爆炸
	const int INITIAL_RENDER_COUNT = 10;  // 先渲染前10页，让预览框快速显示
	const int MAX_ASYNC_RENDER = 50;  // 异步最多再渲染50页，避免大文件内存爆炸

	int renderCount = min(INITIAL_RENDER_COUNT, m_totalPages);

#ifdef _DEBUG
	TRACE(_T("先渲染前 %d 页缩略图\n"), renderCount);
#endif

	// 渲染初始缩略图（会使用缓存，如果存在）
	for (int i = 0; i < renderCount; i++)
	{
		RenderThumbnail(i);
	}

	// 当前页不在首批范围内时，优先补一张当前页缩略图，避免首次打开/切换后滚到陌生位置时开始重刷远端页面。
	if (m_currentPage >= 0 && m_currentPage < m_totalPages && m_currentPage >= renderCount)
	{
		RenderThumbnail(m_currentPage);
	}

	// ★★★ 对于大文件（超过100页），只异步渲染有限数量的缩略图
	// 其余的缩略图将在用户滚动到可见范围时按需加载
	int maxAsyncPage = min(renderCount + MAX_ASYNC_RENDER, m_totalPages);

	if (maxAsyncPage > renderCount)
	{
#ifdef _DEBUG
		if (m_totalPages > maxAsyncPage) {
			TRACE(_T("异步渲染 %d 页（剩余 %d 页将按需加载）\n"),
				maxAsyncPage - renderCount, m_totalPages - maxAsyncPage);
		} else {
			TRACE(_T("异步渲染剩余 %d 页\n"), m_totalPages - renderCount);
		}
#endif
		// 使用PostMessage异步渲染部分缩略图
		PostMessage(WM_USER + 100, renderCount, maxAsyncPage);
	}

#ifdef _DEBUG
	TRACE(_T("UpdateThumbnails() 完成，已渲染: %d/%d\n"), renderCount, m_totalPages);
#endif

	HighlightCurrentThumbnail();

	if (m_thumbnailList.GetSafeHwnd())
	{
		m_thumbnailList.SetRedraw(TRUE);
		m_thumbnailList.RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
	}

	m_isUpdatingThumbnails = false;
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

	// 分批渲染，每次渲染10页，避免一次性阻塞太久
	const int BATCH_SIZE = 10;
	int batchEnd = min(startPage + BATCH_SIZE, endPage);

	for (int i = startPage; i < batchEnd; i++)
	{
		// 再次检查文档是否有效（可能在渲染过程中被关闭）
		if (!m_doc || i >= m_totalPages)
			break;

		RenderThumbnail(i);
	}

	// ★★★ 每批次渲染后，限制缩略图缓存大小
	CPDFDocument* pDoc = GetActiveDocument();
	if (pDoc) {
		pDoc->LimitThumbnailCache(THUMBNAIL_CACHE_LIMIT);
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

// ★★★ 渲染可见区域的缩略图（按需加载）
void CXiaoGongPDFDlg::RenderVisibleThumbnails()
{
	if (!m_thumbnailList.GetSafeHwnd() || !m_doc || m_totalPages <= 0)
		return;

	// 获取可见区域
	int topIndex = m_thumbnailList.GetTopIndex();
	int bottomIndex = topIndex + m_thumbnailList.GetCountPerPage();

	// 确保范围有效
	bottomIndex = min(bottomIndex, m_thumbnailList.GetItemCount() - 1);

#ifdef _DEBUG
	TRACE(_T("RenderVisibleThumbnails: 可见范围 %d - %d\n"), topIndex, bottomIndex);
#endif

	// ★★★ 预加载：向前向后各扩展10页
	const int PRELOAD_COUNT = 10;
	int startPage = max(0, topIndex - PRELOAD_COUNT);
	int endPage = min(m_totalPages - 1, bottomIndex + PRELOAD_COUNT);

	// 渲染可见区域和预加载区域的缩略图
	for (int i = startPage; i <= endPage; i++) {
		// 检查是否已经渲染
		CPDFDocument* pDoc = GetActiveDocument();
		if (pDoc) {
			auto& cache = pDoc->GetThumbnailCache();
			if (cache.find(i) == cache.end()) {
				// 未渲染，执行渲染
				RenderThumbnail(i);
			}
		}
	}

	// ★★★ 限制缓存大小
	CPDFDocument* pDoc = GetActiveDocument();
	if (pDoc) {
		pDoc->LimitThumbnailCache(THUMBNAIL_CACHE_LIMIT);
	}
}

// ★★★ 缩略图滚动事件处理
LRESULT CXiaoGongPDFDlg::OnThumbnailScroll(WPARAM wParam, LPARAM lParam)
{
#ifdef _DEBUG
	TRACE(_T("OnThumbnailScroll: 检测到缩略图列表滚动\n"));
#endif

	if (m_isUpdatingThumbnails)
	{
		return 0;
	}

	// 延迟渲染，避免频繁滚动时过度渲染
	DWORD currentTime = GetTickCount();

	// 如果距离上次滚动不到200ms，则跳过
	if (currentTime - m_lastThumbnailScrollTime < 200) {
		return 0;
	}
	m_lastThumbnailScrollTime = currentTime;

	// 渲染可见区域的缩略图
	RenderVisibleThumbnails();

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

// 延迟更新缩略图高亮的消息处理函数
LRESULT CXiaoGongPDFDlg::OnUpdateThumbnailHighlight(WPARAM wParam, LPARAM lParam)
{
#ifdef _DEBUG
	TRACE(_T("OnUpdateThumbnailHighlight: 延迟更新缩略图高亮\n"));
#endif

	// ★★★ 修复：使用节流+去抖动结合，既能在拖动中实时更新，又能确保最后一次更新不会被丢失
	const UINT_PTR TIMER_ID_THUMBNAIL_HIGHLIGHT = 1001;
	const UINT THROTTLE_INTERVAL_MS = 100;  // 节流间隔：拖动过程中最多每100ms更新一次
	const UINT DEBOUNCE_DELAY_MS = 150;     // 去抖动延迟：确保拖动停止后150ms内会执行最后一次更新

	DWORD currentTime = GetTickCount();

	// 取消之前的去抖动定时器（如果有），设置新的定时器
	// 这样确保拖动停止后，最后一次更新一定会在150ms内执行
	KillTimer(TIMER_ID_THUMBNAIL_HIGHLIGHT);
	SetTimer(TIMER_ID_THUMBNAIL_HIGHLIGHT, DEBOUNCE_DELAY_MS, NULL);

	// 节流：如果距离上次更新不到100ms，跳过立即更新（但去抖动定时器会确保最终更新）
	if (currentTime - m_lastThumbnailHighlightTime < THROTTLE_INTERVAL_MS) {
		return 0;
	}

	// 执行立即更新（拖动过程中实时反馈）
	m_lastThumbnailHighlightTime = currentTime;
	HighlightCurrentThumbnail();

	return 0;
}

// 定时器消息处理函数
void CXiaoGongPDFDlg::OnTimer(UINT_PTR nIDEvent)
{
	const UINT_PTR TIMER_ID_THUMBNAIL_HIGHLIGHT = 1001;

	if (nIDEvent == TIMER_ID_THUMBNAIL_HIGHLIGHT)
	{
#ifdef _DEBUG
		TRACE(_T("OnTimer: 执行缩略图高亮更新（去抖动定时器触发）\n"));
#endif
		// 执行缩略图高亮更新
		HighlightCurrentThumbnail();

		// 销毁一次性定时器
		KillTimer(TIMER_ID_THUMBNAIL_HIGHLIGHT);
		return;
	}

	// ★★★ P1优化：OnSize 防抖定时器触发，执行真正的重渲染
	if (nIDEvent == TIMER_ID_RESIZE_DEBOUNCE)
	{
		// 销毁一次性定时器
		KillTimer(TIMER_ID_RESIZE_DEBOUNCE);

#ifdef _DEBUG
		TRACE(_T("OnTimer: 执行窗口大小变化后的重渲染（防抖定时器触发，cx=%d, cy=%d）\n"),
			m_pendingResizeCx, m_pendingResizeCy);
#endif

		// 确认文档仍然有效
		if (m_doc && m_currentPage >= 0)
		{
			PerformResizeRender();
		}
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

	if (m_continuousScrollMode)
	{
		CalculatePagePositions(m_zoomMode == ZOOM_FIT_WIDTH || m_zoomMode == ZOOM_FIT_PAGE);
		UpdateScrollBar();
		RenderVisiblePages();
	}
	else
	{
		UpdateScrollBar();
		m_scrollPosition = 0;
		m_scrollPositionH = 0;
		RenderPage(m_currentPage);
	}

	m_lastResizeRenderTick = GetTickCount64();
}

void CXiaoGongPDFDlg::CleanupThumbnails()
{
#ifdef _DEBUG
	TRACE(_T("CleanupThumbnails() "));
#endif

	// 清理列表控件
	if (m_thumbnailList.GetSafeHwnd())
	{
		CImageList* pImageList = m_thumbnailList.GetImageList(LVSIL_NORMAL);
		if (pImageList)
		{
			HIMAGELIST hImageList = pImageList->Detach();
			if (hImageList)
			{
				::ImageList_Destroy(hImageList);
			}
		}

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
	m_thumbnailImageIndices.clear();
	m_thumbnailPicWidth = 0;
	m_thumbnailPicHeight = 0;
}

void CXiaoGongPDFDlg::HighlightCurrentThumbnail()
{
#ifdef _DEBUG
	TRACE(_T("HighlightCurrentThumbnail() - 当前页: %d\n"), m_currentPage);
#endif

	if (!m_thumbnailList.GetSafeHwnd() || m_currentPage < 0 || m_currentPage >= m_totalPages)
		return;

	// 清除所有项的选中状态
	for (int i = 0; i < m_thumbnailList.GetItemCount(); i++)
	{
		m_thumbnailList.SetItemState(i, 0, LVIS_SELECTED);
	}

	// 查找对应当前页面的列表项索引
	int itemIndex = -1;
	for (int i = 0; i < m_thumbnailList.GetItemCount(); i++)
	{
		if ((int)m_thumbnailList.GetItemData(i) == m_currentPage)
		{
			itemIndex = i;
			break;
		}
	}

	// ★★★ 修复：如果缩略图未渲染，主动渲染当前页面的缩略图
	// 这样可以确保快速拖动时缩略图能够跟上
	if (itemIndex == -1)
	{
#ifdef _DEBUG
		TRACE(_T("页面 %d 的缩略图还没渲染，立即渲染\n"), m_currentPage);
#endif
		// 渲染当前页面的缩略图
		if (RenderThumbnail(m_currentPage))
		{
			// 渲染成功后，重新查找列表项索引
			for (int i = 0; i < m_thumbnailList.GetItemCount(); i++)
			{
				if ((int)m_thumbnailList.GetItemData(i) == m_currentPage)
				{
					itemIndex = i;
					break;
				}
			}
		}

		// 如果还是找不到，直接返回
		if (itemIndex == -1)
		{
#ifdef _DEBUG
			TRACE(_T("渲染失败或未找到页面 %d 的缩略图\n"), m_currentPage);
#endif
			return;
		}
	}

	// 设置找到的项为选中状态并滚动到可见
	if (itemIndex != -1)
	{
#ifdef _DEBUG
		TRACE(_T("高亮页面 %d 对应的列表项: %d\n"), m_currentPage, itemIndex);
#endif
		m_thumbnailList.SetItemState(itemIndex, LVIS_SELECTED, LVIS_SELECTED);
		// ★★★ 确保缩略图滚动到可见区域（居中显示）
		m_thumbnailList.EnsureVisible(itemIndex, FALSE);

		// 主 PDF 跳到很后面的页面时，缩略图列表会被滚动到新的可见区域。
		// 这里主动按新可见范围补渲染，避免后段可见项仍显示旧的/占位图。
		if (!m_isUpdatingThumbnails)
		{
			RenderVisibleThumbnails();
		}
	}
}

void CXiaoGongPDFDlg::OnThumbnailItemChanged(NMHDR* pNMHDR, LRESULT* pResult)
{
	LPNMLISTVIEW pNMLV = reinterpret_cast<LPNMLISTVIEW>(pNMHDR);

#ifdef _DEBUG
	if (pNMLV->iItem >= 0 && ((pNMLV->uNewState & LVIS_SELECTED) || (pNMLV->uOldState & LVIS_SELECTED)))
	{
		TRACE(_T("OnThumbnailItemChanged() - iItem: %d, uChanged: %u, uNewState: %u, uOldState: %u, m_currentPage: %d\n"),
			pNMLV->iItem, pNMLV->uChanged, pNMLV->uNewState, pNMLV->uOldState, m_currentPage);
	}
#endif

	if (m_isUpdatingThumbnails)
	{
		*pResult = 0;
		return;
	}

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
	}

	*pResult = 0;
}

void CXiaoGongPDFDlg::OnSize(UINT nType, int cx, int cy)
{
	CDialogEx::OnSize(nType, cx, cy);

	bool doImmediateResizeRender = false;

	// 全屏模式下不进行调整，已提前調整
	if (m_isFullscreen && m_pdfView.GetSafeHwnd())
	{
		return;
	}

	// 检查控件是否有效，以及窗口是否为最小化状态
	if (m_thumbnailList.GetSafeHwnd() && m_pdfView.GetSafeHwnd() &&
		m_toolbar.GetSafeHwnd() && m_btnOpen.GetSafeHwnd() && nType != SIZE_MINIMIZED)
	{
		// ★★★ 禁止重绘，避免布局调整过程中的闪烁（高刷新率显示器优化）
		SetRedraw(FALSE);
		const int TOOLBAR_HEIGHT = 52;
		const int TAB_HEIGHT = 42;
		const int BTN_HEIGHT = 34;
		const bool compactToolbar = cx < 1080;
		const int ICON_WIDTH = compactToolbar ? 30 : 34;
		const int GAP = compactToolbar ? 2 : 5;
		const int GROUP_GAP = compactToolbar ? 5 : 14;
		const int OUTLINE_BUTTON_WIDTH = 34;
		const int btnY = MARGIN + TAB_HEIGHT + (TOOLBAR_HEIGHT - BTN_HEIGHT) / 2;

		m_btnMore.MoveWindow(0, 0, 46, TAB_HEIGHT);
		if (m_tabCtrl.GetSafeHwnd())
			m_tabCtrl.MoveWindow(46, 0, max(1, cx - 46 - 138), TAB_HEIGHT);
		m_btnMinimize.MoveWindow(cx - 138, 0, 46, TAB_HEIGHT);
		m_btnMaximize.MoveWindow(cx - 92, 0, 46, TAB_HEIGHT);
		m_btnCloseWindow.MoveWindow(cx - 46, 0, 46, TAB_HEIGHT);
		m_btnMaximize.SetIconGlyph(IsZoomed() ? 0xE923 : 0xE922);
		m_btnMaximize.SetToolTipText(IsZoomed() ? _T("还原") : _T("最大化"));
		m_toolbar.MoveWindow(0, TAB_HEIGHT, cx, TOOLBAR_HEIGHT);
		int x = 14;
		m_btnOpen.MoveWindow(x, btnY, compactToolbar ? 62 : 76, BTN_HEIGHT);
		x += (compactToolbar ? 62 : 76) + GROUP_GAP;
		m_btnOutline.MoveWindow(x, btnY, OUTLINE_BUTTON_WIDTH, BTN_HEIGHT);
		x += OUTLINE_BUTTON_WIDTH + GROUP_GAP;
		const int searchWidth = compactToolbar ? 100 : min(210, 100 + (cx - 1080) / 5);
		m_editSearch.MoveWindow(x, btnY, searchWidth, BTN_HEIGHT);
		x += searchWidth + GAP;
		m_btnFind.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);
		x += ICON_WIDTH + GAP;
		m_btnPrevMatch.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);
		x += ICON_WIDTH + GAP;
		m_btnNextMatch.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);
		x += ICON_WIDTH + GROUP_GAP;
		m_editCurrent.MoveWindow(x, btnY, 43, BTN_HEIGHT);
		x += 43 + GAP;
		m_pageTotal.MoveWindow(x, btnY + 5, 55, BTN_HEIGHT - 10);
		x += 55 + GROUP_GAP;
		m_btnFirst.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);
		x += ICON_WIDTH + GAP;
		m_btnLast.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);
		x += ICON_WIDTH + GROUP_GAP;
		const bool showZoomControls = cx >= 980;
		m_btnZoomOut.ShowWindow(showZoomControls ? SW_SHOW : SW_HIDE);
		m_btnFitWidth.ShowWindow(showZoomControls ? SW_SHOW : SW_HIDE);
		m_btnFitPage.ShowWindow(showZoomControls ? SW_SHOW : SW_HIDE);
		m_btnZoomIn.ShowWindow(showZoomControls ? SW_SHOW : SW_HIDE);
		if (showZoomControls)
		{
			m_btnZoomOut.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);
			x += ICON_WIDTH + GAP;
			m_btnFitWidth.MoveWindow(x, btnY, compactToolbar ? 72 : 80, BTN_HEIGHT);
			x += (compactToolbar ? 72 : 80) + GAP;
			m_btnFitPage.MoveWindow(x, btnY, compactToolbar ? 72 : 80, BTN_HEIGHT);
			x += (compactToolbar ? 72 : 80) + GAP;
			m_btnZoomIn.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);
			x += ICON_WIDTH + GROUP_GAP;
		}
		m_btnRotateLeft.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);
		x += ICON_WIDTH + GAP;
		m_btnRotateRight.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);
		x += ICON_WIDTH + GROUP_GAP;
		m_btnSelectText.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);
		x += ICON_WIDTH + GAP;
		m_btnEdit.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);
		x += ICON_WIDTH + GAP;
		m_btnFullscreen.MoveWindow(x, btnY, ICON_WIDTH, BTN_HEIGHT);

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

		// PDF视图，为状态栏留出空间
		const int scrollBarSize = 16;
		m_pdfView.MoveWindow(pdfX, contentY, max(1, pdfWidth - scrollBarSize),
			max(1, contentHeight - scrollBarSize));
		if (m_pdfVerticalScrollBar.GetSafeHwnd())
		{
			m_pdfVerticalScrollBar.MoveWindow(pdfX + pdfWidth - scrollBarSize,
				contentY, scrollBarSize, max(1, contentHeight - scrollBarSize));
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
		m_statusBar.MoveWindow(0, cy - STATUS_BAR_HEIGHT, cx, STATUS_BAR_HEIGHT);

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
			// ★★★ 使用FALSE参数避免擦除背景，减少高刷新率显示器上的闪烁
			m_btnFirst.Invalidate(FALSE);
			m_btnLast.Invalidate(FALSE);
			m_btnFullscreen.Invalidate(FALSE);
			m_btnRotateLeft.Invalidate(FALSE);
			m_btnRotateRight.Invalidate(FALSE);
			m_editCurrent.Invalidate(FALSE);
		}

		// 无论是否有文档，都需要重绘状态栏（修复未打开PDF时横向伸缩显示异常）
		m_statusBar.Invalidate(FALSE);
		m_statusBar.UpdateWindow();

		// ★★★ P1优化：使用防抖定时器延迟重渲染，避免拖拽调整窗口时高频触发完整渲染
		// 拖拽过程中 WM_SIZE 以约 60fps 频率到来，每次都重渲染会严重阻塞 UI 线程
		if (m_doc && m_currentPage >= 0)
		{
			// 保存最新的窗口尺寸，定时器触发时使用
			m_pendingResizeCx = cx;
			m_pendingResizeCy = cy;

			// 重置防抖定时器（每次 OnSize 都推迟 RESIZE_DEBOUNCE_MS 毫秒）
			KillTimer(TIMER_ID_RESIZE_DEBOUNCE);
			SetTimer(TIMER_ID_RESIZE_DEBOUNCE, RESIZE_DEBOUNCE_MS, NULL);

			ULONGLONG currentTick = GetTickCount64();
			if (nType == SIZE_MAXIMIZED ||
				m_lastResizeRenderTick == 0 ||
				currentTick - m_lastResizeRenderTick >= RESIZE_THROTTLE_MS)
			{
				doImmediateResizeRender = true;
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
		// 第二步：让所有子控件重绘自身内容，但不触发它们的 ERASEBKGND。
		RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);

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
					m_isRenderingWheelZoom = true;  // 标记开始渲染

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

					m_isRenderingWheelZoom = false;  // 标记渲染完成
					return TRUE;
				}
				catch (...)
				{
					m_isRenderingWheelZoom = false;  // 确保发生异常时也重置标记
					MessageBox(_T("渲染页面时发生错误"), _T("错误"), MB_OK | MB_ICONERROR);
					return FALSE;
				}
			}
		}
	}

	m_isRenderingWheelZoom = false;  // 确保最后重置标记
	return CDialogEx::OnMouseWheel(nFlags, zDelta, pt);
}

// 工具栏功能实现
void CXiaoGongPDFDlg::UpdatePageControls()
{
	if (!m_doc)
	{
		m_editCurrent.SetWindowText(_T("0"));
		m_pageTotal.SetWindowText(_T("/ 0"));
		return;
	}

	// 更新当前页码显示
	CString currentPage;
	currentPage.Format(_T("%d"), m_currentPage + 1);
	m_editCurrent.SetWindowText(currentPage);
	m_editCurrent.UpdateWindow();  // 强制立即重绘，确保快速滚动时也能实时显示
	CString totalPages;
	totalPages.Format(_T("/ %d"), m_totalPages);
	m_pageTotal.SetWindowText(totalPages);

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
}

void CXiaoGongPDFDlg::SetStatusBarMessage(LPCTSTR message)
{
	CPDFDocument* pDoc = GetActiveDocument();
	CString status(_T("  "));
	if (pDoc)
		status += pDoc->GetFilePath();
	else
		status += _T("就绪");
	if (message && *message)
	{
		status += _T("    |    ");
		status += message;
	}
	m_statusBar.SetWindowText(status);
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

// 目录侧栏开关
void CXiaoGongPDFDlg::OnCheckThumbnail()
{
	m_outlineVisible = !m_outlineVisible;
	if (m_outlineVisible)
		m_searchResultsVisible = false;
	m_btnOutline.SetColors(m_outlineVisible ? RGB(221, 235, 252) : RGB(255, 255, 255),
		RGB(232, 241, 252), RGB(211, 229, 250));
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

	// 获取当前页面的旋转角度
	int currentRotation = GetPageRotation(m_currentPage);
	// 计算新的旋转角度（保持在0-270范围内）
	int newRotation = (currentRotation + degrees + 360) % 360;
	// 更新旋转角度存储，并让文档对象同步失效页面尺寸缓存
	m_pageRotations[m_currentPage] = newRotation;
	pDoc->SetPageRotation(m_currentPage, newRotation);

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
			RemoveFromCacheOrder(it->first);

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

	// 清除当前页的连续滚动缓存（因为旋转角度变了）
	auto& docPageCache = pDoc->GetPageCache();
	auto& docCacheOrder = pDoc->GetCacheOrder();
	for (auto it = docPageCache.begin(); it != docPageCache.end(); )
	{
		if (it->first.pageNumber == m_currentPage)
		{
			if (it->second.hBitmap)
				DeleteObject(it->second.hBitmap);

			docCacheOrder.remove(it->first);
			it = docPageCache.erase(it);
		}
		else
		{
			++it;
		}
	}

	// 清除当前页的缩略图缓存（因为旋转角度变了）
	auto& docThumbnailCache = pDoc->GetThumbnailCache();
	auto thumbIt = docThumbnailCache.find(m_currentPage);
	if (thumbIt != docThumbnailCache.end())
	{
		if (thumbIt->second.hBitmap)
		{
			DeleteObject(thumbIt->second.hBitmap);
#ifdef _DEBUG
			TRACE(_T("清除旋转页面的缩略图缓存: 页面 %d\n"), m_currentPage);
#endif
		}
		docThumbnailCache.erase(thumbIt);
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
	if (m_thumbnailVisible && RenderThumbnail(m_currentPage))
	{
		// 刷新缩略图列表显示
		// ★★★ 使用FALSE参数避免擦除背景，减少高刷新率显示器上的闪烁
		m_thumbnailList.Invalidate(FALSE);
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
		if (m_isSearching)
			StopSearch();
		// 清除搜索结果
		m_searchMatches.clear();
		m_searchMatchesByPage.clear();
		m_currentMatchIndex = -1;
		m_searchKeyword.Empty();
		m_searchResultsVisible = false;
		RefreshSearchResultsList();
		if (m_searchResultsList.GetSafeHwnd())
			RefreshDocumentLayout();
		// 清除搜索提示并恢复文件路径
		UpdateStatusBar();

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
	m_toolbar.ModifyStyle(WS_BORDER, 0, SWP_FRAMECHANGED);
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
	m_btnSelectText.SetFont(&m_buttonFont);
	m_btnFind.SetFont(&m_buttonFont);
	m_btnPrevMatch.SetFont(&m_buttonFont);
	m_btnNextMatch.SetFont(&m_buttonFont);

	// 设置编辑框的字体和外观
	m_editCurrent.SetFont(&m_buttonFont);
	m_pageTotal.SetFont(&m_buttonFont);
	m_editSearch.SetFont(&m_buttonFont);

	// 图标由 Windows 系统字体提供，名称在悬停时显示。
	m_btnFirst.SetWindowText(_T("首页"));
	m_btnOpen.SetWindowText(_T("打开"));
	m_btnLast.SetWindowText(_T("末页"));
	m_btnFullscreen.SetWindowText(_T("全屏"));
	m_btnRotateLeft.SetWindowText(_T("左旋"));
	m_btnRotateRight.SetWindowText(_T("右旋"));
	m_btnEdit.SetWindowText(_T("编辑"));
	m_btnSelectText.SetWindowText(_T("选择"));
	m_btnFind.SetWindowText(_T("查找"));
	m_btnPrevMatch.SetWindowText(_T("上个"));
	m_btnNextMatch.SetWindowText(_T("下个"));
	m_btnFirst.SetIconGlyph(0xE892);
	m_btnLast.SetIconGlyph(0xE893);
	m_btnFullscreen.SetIconGlyph(0xE740);
	m_btnRotateLeft.SetIconGlyph(0xE7A7);
	m_btnRotateRight.SetIconGlyph(0xE7A6);
	m_btnEdit.SetIconGlyph(0xE70F);
	m_btnSelectText.SetIconGlyph(0xE933);
	m_btnFind.SetIconGlyph(0xE721);
	m_btnPrevMatch.SetIconGlyph(0xE76B);
	m_btnNextMatch.SetIconGlyph(0xE76C);
	m_btnMore.SetIconGlyph(0xE712);
	m_btnOutline.SetIconGlyph(0xE8FD);
	m_btnMinimize.SetIconGlyph(0xE921);
	m_btnMaximize.SetIconGlyph(0xE922);
	m_btnCloseWindow.SetIconGlyph(0xE8BB);
	m_btnFirst.SetToolTipText(_T("首页"));
	m_btnOpen.SetToolTipText(_T("打开 PDF 文件"));
	m_btnZoomOut.SetToolTipText(_T("缩小"));
	m_btnFitWidth.SetToolTipText(_T("适合宽度"));
	m_btnFitPage.SetToolTipText(_T("适合页面"));
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
	m_btnSelectText.SetToolTipText(_T("文字选择，点击切换为拖动"));
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
	m_btnSelectText.EnableWindow(TRUE);
	m_editCurrent.EnableWindow(TRUE);

	// 所有工具栏按钮使用相同圆角。
	m_btnFirst.SetCornerRadius(6);
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
	m_btnSelectText.SetCornerRadius(6);
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
	// 如果是键盘消息
	if (pMsg->message == WM_KEYDOWN)
	{
		// 处理ESC键 - 在全屏模式下退出全屏，有选择时清除选择，否则忽略
		if (pMsg->wParam == VK_ESCAPE)
		{
			if (m_isSearching)
			{
				StopSearch();
				RefreshSearchResultsList();
				SetStatusBarMessage(_T("搜索已取消"));
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

		// 处理Ctrl+C - 复制选中文本
		if ((GetKeyState(VK_CONTROL) & 0x8000) && pMsg->wParam == 'C')
		{
			HWND focusWnd = ::GetFocus();
			bool editingText = focusWnd == m_editSearch.GetSafeHwnd() ||
				focusWnd == m_editCurrent.GetSafeHwnd();
			if (m_hasSelection && !editingText)
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
			SetStatusBarMessage(m_searchCaseSensitive ?
				_T("搜索：区分大小写") : _T("搜索：忽略大小写"));
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

	// ★★★ 监听缩略图列表的鼠标滚轮事件，按需加载可见缩略图
	if (pMsg->message == WM_MOUSEWHEEL && m_thumbnailList.GetSafeHwnd())
	{
		CPoint pt(GET_X_LPARAM(pMsg->lParam), GET_Y_LPARAM(pMsg->lParam));
		CRect listRect;
		m_thumbnailList.GetWindowRect(&listRect);

		// 检查鼠标是否在缩略图列表区域
		if (listRect.PtInRect(pt))
		{
			// 触发按需加载（使用PostMessage避免阻塞滚动）
			PostMessage(WM_USER + 102, 0, 0);
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
		lpMMI->ptMaxPosition.x = work.left - monitor.left;
		lpMMI->ptMaxPosition.y = work.top - monitor.top;
		lpMMI->ptMaxSize.x = work.right - work.left;
		lpMMI->ptMaxSize.y = work.bottom - work.top;
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

	// 保存进入全屏前的最大化状态
	m_wasMaximized = (IsZoomed() != FALSE);

	// 保存当前窗口位置和大小用于恢复（仅在非最大化时有效，最大化时用 ShowWindow 恢复）
	GetWindowRect(&m_windowRect);

	// ★★★ 保存当前缩放状态（含平移偏移、滚动位置和实际缩放比例），用于退出全屏时恢复
	m_savedZoomMode = m_zoomMode;
	m_savedCustomZoom = m_customZoom;
	m_savedPanOffset = m_panOffset;
	m_savedScrollPosition = m_scrollPosition;
	m_savedUniformScale = m_uniformScale;

	// ★★★ 先设置全屏标志，避免SetWindowPos触发OnSize时重新布局控件
	m_isFullscreen = true;

	// 移除窗口边框和标题栏
	ModifyStyle(WS_CAPTION | WS_THICKFRAME, 0, SWP_FRAMECHANGED);

	// 隐藏工具栏、标签页、缩略图和其他控件
	SetControlsVisible(false);

	// 先隐藏控件在改变
	// 获取屏幕尺寸
	int screenWidth = GetSystemMetrics(SM_CXSCREEN);
	int screenHeight = GetSystemMetrics(SM_CYSCREEN);

	// 调整窗口大小为全屏
	SetWindowPos(NULL, 0, 0, screenWidth, screenHeight, SWP_NOZORDER);

	// 在全屏模式下，只调整PDF预览控件大小为整个客户区
	// ★★★ 不隐藏 m_pdfView：让旧位图保持可见直到新位图渲染完成后原子替换，避免主窗口背景短暂裸露。
	if (m_pdfView.GetSafeHwnd())
	{
		m_pdfView.MoveWindow(0, 0, screenWidth, screenHeight);
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

	// 恢复窗口样式
	ModifyStyle(0, WS_THICKFRAME, SWP_FRAMECHANGED);

	// ★★★ 先重设全屏标志，让 ShowWindow/SetWindowPos 触发的 OnSize 能正确布局控件
	m_isFullscreen = false;

	// 恢复窗口位置和大小：若进入全屏前是最大化，直接还原最大化；否则还原普通窗口
	if (m_wasMaximized)
	{
		ShowWindow(SW_MAXIMIZE);
		// ★★★ 全屏尺寸与最大化尺寸相同时，ShowWindow(SW_MAXIMIZE) 不会触发 OnSize。
		// 强制发送 WM_SIZE 确保控件布局被重新计算。
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

	// 显示隐藏的控件
	SetControlsVisible(true);

	// ★★★ SetWindowPos 已触发 OnSize，OnSize 里已用正确的视图尺寸重新计算并渲染。
	// 这里只需要恢复进入全屏前的缩放模式/值/位置，再触发一次渲染。
	if (m_doc && m_currentPage >= 0)
	{
		// ★★★ 恢复进入全屏前的缩放模式和缩放值（直接赋值，不通过SetZoom）
		m_zoomMode = m_savedZoomMode;
		m_customZoom = m_savedCustomZoom;
		// ★★★ 恢复进入全屏前保存的平移偏移量
		m_panOffset = m_savedPanOffset;

		if (m_continuousScrollMode)
		{
			// ★★★ 直接恢复进入全屏前的实际缩放比例，不重算（避免因视图尺寸变化导致缩放改变）
			if (m_savedUniformScale > 0.0f)
				m_uniformScale = m_savedUniformScale;
			CalculatePagePositions(false);

			// ★★★ 恢复进入全屏前的滚动位置，限制在有效范围内
			m_scrollPosition = m_savedScrollPosition;
			CRect viewRect;
			m_pdfView.GetClientRect(&viewRect);
			int maxScroll = max(0, m_totalScrollHeight - viewRect.Height());
			if (m_scrollPosition > maxScroll) m_scrollPosition = maxScroll;
			if (m_scrollPosition < 0) m_scrollPosition = 0;

			UpdateScrollBar();
			RenderVisiblePages();
		}
		else
		{
			// ★★★ 分页模式：重新渲染当前页面
			CleanupBitmap();
			RenderPage(m_currentPage);
		}
	}

	// 确保窗口和所有控件更新
	// ★★★ 使用FALSE参数避免擦除背景，减少高刷新率显示器上的闪烁
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
	pDoc->SetPanOffset(m_panOffset);
	pDoc->SetCanDrag(m_canDrag);
	pDoc->SaveCurrentPageZoomState();

#ifdef _DEBUG
	TRACE(_T("保存页面 %d 的状态: mode=%d, zoom=%.2f, offset=(%d,%d)\n"),
		m_currentPage, m_zoomMode, m_customZoom, m_panOffset.x, m_panOffset.y);
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
	m_customZoom = pDoc->GetCustomZoom();
	m_panOffset = pDoc->GetPanOffset();
	m_canDrag = pDoc->GetCanDrag();
	m_zoom = (m_zoomMode == ZOOM_CUSTOM) ? m_customZoom : pDoc->GetZoom();

#ifdef _DEBUG
	TRACE(_T("恢复页面 %d 的状态: mode=%d, zoom=%.2f, offset=(%d,%d)\n"),
		pageNumber, m_zoomMode, m_customZoom, m_panOffset.x, m_panOffset.y);
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

void CXiaoGongPDFDlg::OnBtnZoomOut()
{
	SetZoom(max(0.25f, m_customZoom / 1.1f), ZOOM_CUSTOM);
}

void CXiaoGongPDFDlg::OnBtnFitWidth()
{
	SetZoom(1.0f, ZOOM_FIT_WIDTH);
}

void CXiaoGongPDFDlg::OnBtnFitPage()
{
	SetZoom(1.0f, ZOOM_FIT_PAGE);
}

void CXiaoGongPDFDlg::OnBtnZoomIn()
{
	SetZoom(min(4.0f, m_customZoom * 1.1f), ZOOM_CUSTOM);
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
	menu.AppendMenu(MF_STRING | (m_doc ? 0 : MF_GRAYED), ID_MENU_PRINT, _T("打印..."));
	menu.AppendMenu(MF_STRING, ID_MENU_SHORTCUTS, _T("快捷键"));
	CMenu zoomMenu;
	if (zoomMenu.CreatePopupMenu())
	{
		zoomMenu.AppendMenu(MF_STRING | (m_doc ? 0 : MF_GRAYED) |
			(m_zoomMode == ZOOM_FIT_WIDTH ? MF_CHECKED : 0), ID_MENU_FIT_WIDTH, _T("适合宽度"));
		zoomMenu.AppendMenu(MF_STRING | (m_doc ? 0 : MF_GRAYED) |
			(m_zoomMode == ZOOM_FIT_PAGE ? MF_CHECKED : 0), ID_MENU_FIT_PAGE, _T("适合页面"));
		if (menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(zoomMenu.GetSafeHmenu()), _T("缩放")))
			zoomMenu.Detach();
	}
	menu.AppendMenu(MF_STRING | (m_searchMatches.empty() ? MF_GRAYED : 0),
		ID_MENU_SEARCH_RESULTS, _T("搜索结果"));
	menu.AppendMenu(MF_STRING, ID_MENU_SET_DEFAULT, _T("设为默认 PDF 阅读器"));
	menu.AppendMenu(MF_STRING, ID_MENU_ABOUT, _T("关于"));
	menu.AppendMenu(MF_SEPARATOR);
	menu.AppendMenu(MF_STRING, ID_MENU_EXIT, _T("退出"));

	CRect buttonRect;
	m_btnMore.GetWindowRect(&buttonRect);
	const UINT command = static_cast<UINT>(menu.TrackPopupMenu(
		TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
		buttonRect.left, buttonRect.bottom, this));
	if (command != 0)
		PostMessage(WM_COMMAND, MAKEWPARAM(command, 0));
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
			RemoveFromCacheOrder(key);
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
		int viewWidth = max(1, viewRect.Width() - 40);  // 留出左右边距各20像素

		// ★★★ 在连续滚动模式下，使用 m_customZoom 调整 m_uniformScale
		// m_uniformScale 的基准是适应宽度的缩放，m_customZoom 是在此基础上的倍数
		if (m_documentMaxPageWidth > 0)
		{
			float baseScale = m_zoomMode == ZOOM_FIT_WIDTH ?
				FitWidthScale(viewRect.Width(), m_documentMaxPageWidth) :
				(float)viewWidth / m_documentMaxPageWidth * 0.95f;
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
		HDC hPrinterDC = CreateDC(NULL, printDlg.m_printerName, NULL, printDlg.GetDevMode());
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
		bool failed = false;
		int failedPage = -1;

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
						failed = !PrintSinglePage(dcPrint, pageNum, horRes, verRes);
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
						failed = !PrintSinglePage(dcPrint, pageNum, horRes, verRes);
						if (failed) failedPage = pageNum;
					}
				}
			}

			if (failed)
			{
				dcPrint.AbortDoc();
				CString message;
				message.Format(_T("第 %d 页打印失败，任务已取消。"), failedPage + 1);
				MessageBox(message, _T("错误"), MB_OK | MB_ICONERROR);
			}
			else if (dcPrint.EndDoc() < 0)
				MessageBox(_T("打印机未能完成任务。"), _T("错误"), MB_OK | MB_ICONERROR);
			else
				MessageBox(_T("打印任务已发送到打印机"), _T("提示"), MB_OK | MB_ICONINFORMATION);
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

	// 配置标签页控件
	m_tabCtrl.EnableCloseButton(TRUE);  // 启用关闭按钮
	m_tabCtrl.SetTabHeight(42);
	m_tabCtrl.SetColors(RGB(240, 244, 248), RGB(225, 234, 244), RGB(255, 255, 255));

	// 添加滚动按钮样式和工具提示（ModernTabCtrl也支持这些）
	// 添加TCS_FIXEDWIDTH样式使所有标签页宽度一致
	m_tabCtrl.ModifyStyle(0, TCS_SCROLLOPPOSITE | TCS_TOOLTIPS | TCS_FIXEDWIDTH);

	// 文件名在较宽的标签中保持可读
	m_tabCtrl.SendMessage(TCM_SETITEMSIZE, 0, MAKELPARAM(218, 42));

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

	if (m_pLoadThread || m_pLoadParams)
	{
		MessageBox(_T("请等待当前PDF加载完成后再打开新文件。"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return false;
	}

	// 检查文件是否已经打开
	// 规范化路径用于比较（转为小写、获取完整路径,win路径不区分大小写，但是字符比较区分）
	CString normalizedNewPath = FullPathForComparison(filePath);

#ifdef _DEBUG
	TRACE(_T("规范化路径: %s\n"), normalizedNewPath);
#endif

	// 遍历已打开的文档，检查是否重复
	for (int i = 0; i < (int)m_documents.size(); i++)
	{
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
			m_tabCtrl.SetCurSel(i);
			SwitchToDocument(i);

			// 显示提示消息
			CString msg;
			msg.Format(_T("该文件已经打开，已切换到对应标签页"));
			MessageBox(msg, _T("提示"), MB_OK | MB_ICONINFORMATION);

			return true;
		}
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

	// 创建进度对话框
	CProgressDlg* pProgressDlg = new CProgressDlg(this);
	pProgressDlg->Create(IDD_PROGRESS_DIALOG, this);
	pProgressDlg->ShowWindow(SW_SHOW);
	pProgressDlg->UpdateWindow();
	m_pProgressDlg = pProgressDlg;  // 保存指针，供后续使用

	// 创建新文档对象
	CPDFDocument* newDoc = new CPDFDocument();

	// 创建加载参数
	m_pLoadParams = new PDFLoadParams();
	m_pLoadParams->filePath = normalizedNewPath;
	m_pLoadParams->utf8Path = utf8Path;
	m_pLoadParams->notifyWnd = GetSafeHwnd();
	m_pLoadParams->pDoc = newDoc;
	m_pLoadParams->success = false;

	// 启动后台加载线程
	m_pLoadThread = AfxBeginThread(PDFLoadThreadProc, m_pLoadParams, THREAD_PRIORITY_NORMAL, 0, CREATE_SUSPENDED);
	if (m_pLoadThread)
	{
		m_pLoadThread->m_bAutoDelete = FALSE;  // 手动管理线程生命周期
		m_pLoadThread->ResumeThread();

	}
	else
	{
		// 线程创建失败 ,电脑资源极度紧张，概率很小
		DWORD dwError = GetLastError();
		delete newDoc;
		delete m_pLoadParams;
		m_pLoadParams = nullptr;
		CloseProgressDialog();
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

	// 切换文档时清除文本选择
	ClearSelection();

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

	CleanupCurrentPage();

	// 切换到新文档
	m_activeDocIndex = index;
	CPDFDocument* newDoc = m_documents[index];
	RefreshDocumentLayout();

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

#ifdef _DEBUG
	TRACE(_T("m_doc: %p, 总页数: %d, 当前页: %d\n"), m_doc, m_totalPages, m_currentPage);
#endif

	// ★★★ 在连续滚动模式下，重置平移位置（连续滚动不使用拖拽平移）
	if (m_continuousScrollMode)
	{
		m_panOffset = CPoint(0, 0);
		m_canDrag = false;
	}

#ifdef _DEBUG
	TRACE(_T("恢复旋转状态，旋转信息数量: %d\n"), (int)m_pageRotations.size());
	TRACE(_T("恢复文档缩放状态: zoom=%.2f, mode=%d, customZoom=%.2f\n"),
		m_zoom, m_zoomMode, m_customZoom);
	TRACE(_T("位图句柄: %p\n"), m_hCurrentBitmap);
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
	m_editSearch.SetWindowText(m_searchKeyword);
	m_searchResultsVisible = !m_searchMatches.empty();
	if (m_searchResultsVisible)
	{
		m_outlineVisible = false;
		m_btnOutline.SetColors(RGB(255, 255, 255), RGB(232, 241, 252), RGB(211, 229, 250));
		m_btnOutline.SetToolTipText(_T("显示目录"));
	}
	RefreshSearchResultsList();

#ifdef _DEBUG
	TRACE(_T("已恢复搜索信息: %d 个匹配项, 当前索引=%d\n"),
		(int)m_searchMatches.size(), m_currentMatchIndex);
#endif

	// ★★★ 连续滚动模式：恢复该文档上次的实际渲染缩放比例
	float savedUniformScale = newDoc->GetUniformScale();
	if (savedUniformScale > 0.0f)
	{
		// 已打开过：直接恢复上次的缩放，不重算
		m_uniformScale = savedUniformScale;
		CalculatePagePositions(m_zoomMode == ZOOM_FIT_WIDTH || m_zoomMode == ZOOM_FIT_PAGE);
	}
	else
	{
		// 首次打开：先用 recalcScale=true 算出 baseScale（基于视图宽度的适应宽度值），
		// 再把 m_customZoom 反推为 1.0f/baseScale，使 m_uniformScale = 1.0f（1:1 显示）
		// 这样后续 Ctrl+滚轮 的倍率计算（newZoom = m_customZoom * 1.1）仍然正确
		CalculatePagePositions(true);  // 此时 m_uniformScale = baseScale * 1.0f = baseScale
		if (m_uniformScale > 0.0f)
		{
			float baseScale = m_uniformScale;          // baseScale = viewWidth/maxPageWidth*0.95
			m_customZoom = 1.0f / baseScale;           // 反推：使 baseScale * m_customZoom = 1.0f
			m_uniformScale = 1.0f;                     // 1:1 原始尺寸显示
		}
		CalculatePagePositions(false);  // 用新的 m_uniformScale=1.0f 重排页面位置/高度
	}
	UpdateScrollBar();

	RenderVisiblePages();

	// 更新UI控件
	UpdatePageControls();
	UpdateStatusBar();

	if (m_outlineVisible)
		UpdateOutlineTree();
	else
		m_outlineTree.DeleteAllItems();
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

	bool isActiveDocument = (index == m_activeDocIndex);
	CPDFDocument* doc = m_documents[index];
	if (m_pSearchParams && m_pSearchParams->pDoc == doc)
		StopSearch();

	if (isActiveDocument)
	{
		ClearSelection();
		m_pdfView.SetDisplayBitmap(NULL);
		SafeDeleteBitmap(m_hContinuousViewBitmap);
		CleanupCurrentPage();

		if (doc)
		{
			SaveDialogStateToDocument(doc);
			SaveReadingPosition(doc);
		}

		m_ctx = nullptr;
		m_doc = nullptr;
		m_totalPages = 0;
		m_currentPage = 0;
		m_hCurrentBitmap = NULL;
		m_hPanPageBitmap = NULL;
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

	// 如果关闭的是当前文档
	if (isActiveDocument)
	{
		if (m_documents.empty())
		{
			// 没有文档了，重置状态
			m_activeDocIndex = -1;
			m_ctx = nullptr;
			m_doc = nullptr;
			m_totalPages = 0;
			m_currentPage = 0;
			if (m_editSearch.GetSafeHwnd())
				m_editSearch.SetWindowText(_T(""));
			SetWindowText(_T("小龚PDF阅读器"));
			RefreshDocumentLayout();
			m_outlineTree.DeleteAllItems();
			m_outlineTree.ShowWindow(SW_HIDE);

			// ★★★ 彻底清理并隐藏标签页控件
			if (m_tabCtrl.GetSafeHwnd())
			{
				// 删除所有标签页项
				m_tabCtrl.DeleteAllItems();
				// 隐藏标签页控件
				m_tabCtrl.ShowWindow(SW_HIDE);
				// 强制刷新
				m_tabCtrl.RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN);
			}

			// 清空显示
			CleanupBitmap();
			CleanupThumbnails();
			UpdatePageControls();
			UpdateStatusBar();

			// 刷新布局
			CRect rect;
			GetClientRect(&rect);
			SendMessage(WM_SIZE, SIZE_RESTORED, MAKELPARAM(rect.Width(), rect.Height()));

			// 无文档时隐藏滚动条（必须在 OnSize 之后，防止 OnSize 重新显示）
			m_pdfView.ShowScrollBar(SB_VERT, FALSE);
			m_pdfView.ShowScrollBar(SB_HORZ, FALSE);
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

// ============ 目录侧栏切换 ============

void CXiaoGongPDFDlg::ToggleThumbnailPanel()
{
#ifdef _DEBUG
	TRACE(_T("ToggleThumbnailPanel()\n"));
#endif

	OnCheckThumbnail();
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
	if (m_pLoadParams)
	{
		m_closePending = true;
		m_pLoadParams->cancelled.store(true);
		return;
	}
	CDialogEx::OnCancel();
}

void CXiaoGongPDFDlg::OnClose()
{
	OnCancel();
}

void CXiaoGongPDFDlg::OnDestroy()
{
	TRACE(_T("OnDestroy() called\n"));
	ClearSelection();
	StopSearch();
	CleanupPendingLoad();
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
		m_pdfView.SetDisplayBitmap(NULL);
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

	// 活动文档对象由 CPDFDocument 持有并释放，这里只清空非 owning 引用。
	m_currentPageObj = nullptr;
	m_doc = nullptr;
	m_ctx = nullptr;

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
	CRect clientRect;
	GetClientRect(&clientRect);
	const int dragRight = m_tabCtrl.GetSafeHwnd() && m_tabCtrl.IsWindowVisible() ?
		46 : clientRect.Width() - 138;
	if (point.x >= 0 && point.x < dragRight && point.y >= 0 && point.y < 42)
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
		// ★★★ 将对话框客户区坐标转换为PDF视图客户区坐标（修复拖拽偏移问题）
		CPoint pdfViewPoint = point;
		pdfViewPoint.Offset(-pdfRect.left, -pdfRect.top);

#ifdef _DEBUG
		TRACE(_T("OnLButtonDown: point=(%d,%d), pdfRect=(%d,%d,%d,%d), pdfViewPoint=(%d,%d)\n"),
			point.x, point.y, pdfRect.left, pdfRect.top, pdfRect.right, pdfRect.bottom,
			pdfViewPoint.x, pdfViewPoint.y);
#endif

		// ★★★ 文本选择模式：不启动拖拽，开始文本选择
		if (m_selectionMode && m_doc != nullptr && m_totalPages > 0)
		{
			TRACE(_T(">>> 进入文本选择模式: m_selectionMode=%d, m_doc=%p, m_totalPages=%d\n"),
				m_selectionMode, m_doc, m_totalPages);
			ClearSelection();
			m_isSelecting = true;
			SetFocus();
			m_selStartView = pdfViewPoint;
			m_selEndView = pdfViewPoint;

			if (m_continuousScrollMode)
				m_selPageNumber = GetPageAtPosition(pdfViewPoint.y + m_scrollPosition - m_panOffset.y);
			else
				m_selPageNumber = m_currentPage;

			if (m_selPageNumber < 0 || m_selPageNumber >= m_totalPages)
				m_selPageNumber = m_currentPage;

			fz_page* page = nullptr;
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

			TRACE(_T(">>> 选择开始: page=%d, startView=(%d,%d)\n"),
				m_selPageNumber, m_selStartView.x, m_selStartView.y);

			SetCapture();
			return;
		}
		else
		{
			TRACE(_T(">>> 未进入选择模式: sel=%d, doc=%p, pages=%d\n"),
				m_selectionMode, m_doc, m_totalPages);
		}

		// ★★★ 在连续滚动模式下，支持自由拖拽（当页面放大后超出视图时）
		if (m_continuousScrollMode && m_doc != nullptr && m_totalPages > 0)
		{
			// 获取视图尺寸
			CRect viewRect;
			m_pdfView.GetClientRect(&viewRect);
			int viewWidth = max(1, viewRect.Width() - 40);  // 减去边距
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
					scale = FitWidthScale(viewRect.Width(), pageWidth);
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
				fz_device* dev = nullptr;

				fz_try(m_ctx)
				{
					// 获取页面大小
					fz_rect bounds = fz_bound_page(m_ctx, m_currentPageObj);
					float pageWidth = bounds.x1 - bounds.x0;
					float pageHeight = bounds.y1 - bounds.y0;

					// ★★★ 修复：使用前面计算的正确 scale，而不是硬编码 m_customZoom
					// 这样在 ZOOM_FIT_WIDTH/ZOOM_FIT_PAGE 模式下也能正确渲染

					// 计算渲染尺寸
					int width = ClampRenderDimension(pageWidth * scale);
					int height = ClampRenderDimension(pageHeight * scale);

					// 创建pixmap
					pixmap = fz_new_pixmap(m_ctx, fz_device_rgb(m_ctx), width, height, nullptr, 1);
					fz_clear_pixmap_with_value(m_ctx, pixmap, 0xff);

					// 渲染页面
					fz_matrix ctm = fz_scale(scale, scale);
					dev = fz_new_draw_device(m_ctx, ctm, pixmap);
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
					pixmap = nullptr;
				}
				fz_catch(m_ctx)
				{
					if (dev)
					{
						fz_drop_device(m_ctx, dev);
						dev = nullptr;
					}

					// ★★★ 修复：渲染失败时也要释放 pixmap
					if (pixmap)
					{
						fz_drop_pixmap(m_ctx, pixmap);
						pixmap = nullptr;
					}

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

void CXiaoGongPDFDlg::StartWindowDrag()
{
	::GetCursorPos(&m_windowDragStartScreen);
	GetWindowRect(&m_windowDragStartRect);
	m_isWindowDragging = true;
	SetCapture();
}

void CXiaoGongPDFDlg::OnLButtonUp(UINT nFlags, CPoint point)
{
	if (m_isWindowDragging)
	{
		m_isWindowDragging = false;
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();
		return;
	}
	// 没有加载PDF时不处理鼠标事件
	if (m_doc == nullptr)
	{
		CDialogEx::OnLButtonUp(nFlags, point);
		return;
	}

#ifdef _DEBUG
	TRACE(_T("OnLButtonUp: 被调用! point=(%d,%d), isDragging=%d, isScrollbar=%d, isSelecting=%d\n"),
		point.x, point.y, m_isDragging, m_isDraggingScrollbar, m_isSelecting);
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
		if (m_selectionTextPage)
		{
			CSingleLock lock(&m_renderLock, TRUE);
			fz_drop_stext_page(m_ctx, m_selectionTextPage);
			m_selectionTextPage = nullptr;
		}
		ReleaseCapture();

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
				const double xRatio = static_cast<double>(m_windowDragStartScreen.x -
					m_windowDragStartRect.left) / m_windowDragStartRect.Width();
				ShowWindow(SW_RESTORE);
				GetWindowRect(&m_windowDragStartRect);
				m_windowDragStartRect.OffsetRect(
					cursor.x - m_windowDragStartRect.left -
					static_cast<int>(xRatio * m_windowDragStartRect.Width()),
					cursor.y - m_windowDragStartRect.top - 21);
				m_windowDragStartScreen = cursor;
			}
			SetWindowPos(nullptr,
				m_windowDragStartRect.left + cursor.x - m_windowDragStartScreen.x,
				m_windowDragStartRect.top + cursor.y - m_windowDragStartScreen.y,
				0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
			return;
		}
	}
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

			// ★★★ P1优化：使用页面尺寸缓存获取渲染宽度，避免每次鼠标移动都 fz_load_page
			float pageRenderWidth = 0;
			CPDFDocument* pDoc = GetActiveDocument();
			if (pDoc && pDoc->HasPageBoundsCache())
			{
				// 直接从缓存读取第0页宽度，O(1) 操作
				float cachedW = 0.0f, cachedH = 0.0f;
				if (pDoc->GetPageBounds(0, cachedW, cachedH))
					pageRenderWidth = cachedW * m_uniformScale;
			}

			if (pageRenderWidth <= 0)
			{
				// 回退：缓存不可用时使用 m_totalScrollWidth（已在 CalculatePagePositions 中算好）
				pageRenderWidth = (float)m_totalScrollWidth;
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
				targetDC.FillSolidRect(0, 0, viewWidth, viewHeight, PDF_VIEW_BACKGROUND);

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
				m_pdfView.SetDisplayBitmap(m_hCurrentBitmap);
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

void CXiaoGongPDFDlg::OnContextMenu(CWnd* pWnd, CPoint point)
{
	if (!pWnd || pWnd->GetSafeHwnd() != m_pdfView.GetSafeHwnd())
	{
		CDialogEx::OnContextMenu(pWnd, point);
		return;
	}

	if (!m_hasSelection || m_selectedText.IsEmpty())
		return;

	CMenu popup;
	if (!popup.CreatePopupMenu())
		return;
	const UINT copyCommand = 1;
	popup.AppendMenu(MF_STRING, copyCommand, _T("复制"));
	if (popup.TrackPopupMenu(TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON |
		TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, this) == copyCommand)
		CopySelectionToClipboard();
}

BOOL CXiaoGongPDFDlg::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
	if (!m_pdfView.GetSafeHwnd())
		return CDialogEx::OnSetCursor(pWnd, nHitTest, message);

	CPoint point;
	::GetCursorPos(&point);
	CRect pdfRect;
	m_pdfView.GetWindowRect(&pdfRect);
	if (!pdfRect.PtInRect(point) && !m_isDraggingScrollbar)
		return CDialogEx::OnSetCursor(pWnd, nHitTest, message);

	// 滚动条属于预览窗口的非客户区；捕获鼠标拖动时也始终显示箭头。
	if (nHitTest != HTCLIENT || m_isDraggingScrollbar)
	{
		::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
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

	bool overPage = false;
	if (m_doc && m_totalPages > 0)
	{
		CPDFDocument* pDoc = GetActiveDocument();
		if (pDoc)
		{
			int pageNumber = m_currentPage;
			if (m_continuousScrollMode)
				pageNumber = GetPageAtPosition(point.y + m_scrollPosition - m_panOffset.y);

			float pageWidth = 0.0f;
			float pageHeight = 0.0f;
			if (pageNumber >= 0 && pageNumber < m_totalPages &&
				pDoc->GetPageBounds(pageNumber, pageWidth, pageHeight))
			{
				int width = 0;
				int height = 0;
				int x = 0;
				int y = 0;
				if (m_continuousScrollMode && pageNumber < (int)m_pageYPositions.size())
				{
					width = ClampRenderDimension(pageWidth * m_uniformScale);
					height = ClampRenderDimension(pageHeight * m_uniformScale);
					x = (viewRect.Width() - width) / 2 + m_panOffset.x;
					y = m_pageYPositions[pageNumber] - m_scrollPosition + m_panOffset.y;
				}
				else if (!m_continuousScrollMode && pageWidth > 0.0f && pageHeight > 0.0f)
				{
					float scale = m_customZoom;
					if (m_zoomMode == ZOOM_FIT_WIDTH)
						scale = FitWidthScale(viewRect.Width(), pageWidth);
					else if (m_zoomMode == ZOOM_FIT_PAGE)
						scale = min(viewRect.Width() / pageWidth,
						viewRect.Height() / pageHeight) * 0.95f;
					width = ClampRenderDimension(pageWidth * scale);
					height = ClampRenderDimension(pageHeight * scale);
					if (m_zoomMode == ZOOM_CUSTOM &&
						(m_panOffset.x != 0 || m_panOffset.y != 0))
					{
						x = (viewRect.Width() - width) / 2 + m_panOffset.x;
						y = (viewRect.Height() - height) / 2 + m_panOffset.y;
					}
					else
					{
						x = width < viewRect.Width() ? (viewRect.Width() - width) / 2 : -m_scrollPositionH;
						y = height < viewRect.Height() ? (viewRect.Height() - height) / 2 : -m_scrollPosition;
					}
				}
				CRect pageRect(x, y, x + width, y + height);
				overPage = pageRect.PtInRect(point);
			}
		}
	}

	if (m_selectionMode && overPage)
		::SetCursor(::LoadCursor(nullptr, IDC_IBEAM));
	else if (m_isDragging && overPage)
		::SetCursor(m_hHandCursorGrab);
	else
		::SetCursor(::LoadCursor(nullptr, IDC_ARROW));
	return TRUE;
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
	m_devMode.clear();
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
		LONG result = DocumentProperties(m_hWnd, hPrinter, (LPTSTR)(LPCTSTR)printerName, NULL, NULL, 0);
		if (result > 0)
		{
			std::vector<DWORD> devMode((result + sizeof(DWORD) - 1) / sizeof(DWORD));
			LPDEVMODE pDevMode = reinterpret_cast<LPDEVMODE>(devMode.data());
			if (DocumentProperties(m_hWnd, hPrinter, (LPTSTR)(LPCTSTR)printerName,
				pDevMode, NULL, DM_OUT_BUFFER) == IDOK)
			{
				if (DocumentProperties(m_hWnd, hPrinter, (LPTSTR)(LPCTSTR)printerName,
					pDevMode, pDevMode, DM_IN_BUFFER | DM_PROMPT) == IDOK)
				{
					m_devMode = std::move(devMode);
					if (GetDevMode()->dmFields & DM_DUPLEX)
						m_duplexCheck.SetCheck(GetDevMode()->dmDuplex == DMDUP_SIMPLEX ?
							BST_UNCHECKED : BST_CHECKED);
				}
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
	if (m_bDuplex && DeviceCapabilities(m_printerName, NULL, DC_DUPLEX, NULL, NULL) <= 0)
	{
		MessageBox(_T("所选打印机不支持双面打印。"), _T("提示"), MB_OK | MB_ICONWARNING);
		return;
	}
	if (m_bDuplex)
	{
		if (m_devMode.empty())
		{
			HANDLE hPrinter = NULL;
			if (OpenPrinter((LPTSTR)(LPCTSTR)m_printerName, &hPrinter, NULL))
			{
				LONG bytes = DocumentProperties(m_hWnd, hPrinter,
					(LPTSTR)(LPCTSTR)m_printerName, NULL, NULL, 0);
				if (bytes > 0)
				{
					m_devMode.resize((bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
					if (DocumentProperties(m_hWnd, hPrinter, (LPTSTR)(LPCTSTR)m_printerName,
						reinterpret_cast<LPDEVMODE>(m_devMode.data()), NULL,
						DM_OUT_BUFFER) != IDOK)
						m_devMode.clear();
				}
				ClosePrinter(hPrinter);
			}
		}
		if (m_devMode.empty())
		{
			MessageBox(_T("无法读取打印机双面设置。"), _T("错误"), MB_OK | MB_ICONERROR);
			return;
		}
		GetDevMode()->dmFields |= DM_DUPLEX;
		GetDevMode()->dmDuplex = DMDUP_VERTICAL;
	}
	else if (GetDevMode() && (GetDevMode()->dmFields & DM_DUPLEX))
		GetDevMode()->dmDuplex = DMDUP_SIMPLEX;

	CDialogEx::OnOK();
}

// ============================================================================
// 打印单个页面的辅助函数
// ============================================================================
bool CXiaoGongPDFDlg::PrintSinglePage(CDC& dcPrint, int pageNum, int horRes, int verRes)
{
	// 开始新页
	if (dcPrint.StartPage() <= 0)
		return false;
	volatile bool success = false;

	// 初始化资源为 NULL
	fz_page* page = nullptr;
	fz_pixmap* pixmap = nullptr;
	fz_device* dev = nullptr;
	HBITMAP hBitmap = nullptr;

	CSingleLock lock(&m_renderLock, TRUE);

	fz_try(m_ctx)
	{
		// 加载PDF页面
		page = fz_load_page(m_ctx, m_doc, pageNum);
		if (!page)
			fz_throw(m_ctx, FZ_ERROR_GENERIC, "Failed to load page %d", pageNum);

		// 获取页面大小
		fz_rect bounds = fz_bound_page(m_ctx, page);
		float pageWidth = bounds.x1 - bounds.x0;
		float pageHeight = bounds.y1 - bounds.y0;

		// 计算缩放比例以适应打印机页面
		float safePageWidth = (pageWidth > 0.0f) ? pageWidth : 1.0f;
		float safePageHeight = (pageHeight > 0.0f) ? pageHeight : 1.0f;
		float scaleX = (float)horRes / safePageWidth;
		float scaleY = (float)verRes / safePageHeight;
		float scale = min(scaleX, scaleY);

		// 计算渲染尺寸
		int renderWidth = ClampRenderDimension(pageWidth * scale);
		int renderHeight = ClampRenderDimension(pageHeight * scale);

		// 创建pixmap
		pixmap = fz_new_pixmap(m_ctx, fz_device_rgb(m_ctx), renderWidth, renderHeight, nullptr, 1);
		fz_clear_pixmap_with_value(m_ctx, pixmap, 0xff);

		// 创建渲染设备
		fz_matrix ctm = fz_scale(scale, scale);
		dev = fz_new_draw_device(m_ctx, ctm, pixmap);
		fz_run_page(m_ctx, page, dev, fz_identity, nullptr);
		fz_close_device(m_ctx, dev);

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
		hBitmap = CreateDIBSection(dcPrint.GetSafeHdc(), &bmi, DIB_RGB_COLORS, (void**)&pbBits, NULL, 0);

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

			success = dcPrint.StretchBlt(xOffset, yOffset, renderWidth, renderHeight,
				&memDC, 0, 0, renderWidth, renderHeight, SRCCOPY) != FALSE;

			memDC.SelectObject(oldBitmap);
		}
	}

	fz_catch(m_ctx)
	{
		// 先获取错误消息
		const char* msg = fz_caught_message(m_ctx);
		CString strMsg;
		if (msg)
			strMsg = CString(msg);  // 直接构造，自动转换编码

#ifdef _DEBUG
		TRACE(_T("PrintSinglePage error for page %d: %s\n"), pageNum, strMsg);
#endif
		success = false;
		// 或者用 TRACE 的宽字符版本
		// TRACE(L"PrintSinglePage error for page %d: %S\n", pageNum, msg);
	}

	// 统一清理资源（无论成功还是失败都会执行）
	if (dev)
		fz_drop_device(m_ctx, dev);
	if (pixmap)
		fz_drop_pixmap(m_ctx, pixmap);
	if (page)
		fz_drop_page(m_ctx, page);
	if (hBitmap)
		DeleteObject(hBitmap);

	// 结束当前页（即使出错也要结束，否则打印机状态错乱）
	if (dcPrint.EndPage() <= 0)
		success = false;
	return success;
}

// ============================================================================
// 连续滚动模式实现
// ============================================================================

// 计算所有页面的位置和高度（使用统一缩放比例确保宽度一致）
void CXiaoGongPDFDlg::CalculatePagePositions(bool recalcScale)
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
	int viewWidth = max(1, viewRect.Width() - 40);  // 留出左右边距各20像素

	// ★★★ P1优化：优先使用 CPDFDocument 中的页面尺寸缓存，避免每次全量 fz_load_page
	// 若缓存失效（首次加载、旋转后），则重建缓存（仅需一次全量加载）
	CPDFDocument* pDoc = GetActiveDocument();
	if (pDoc && !pDoc->HasPageBoundsCache())
	{
		pDoc->CachePageseBounds();
	}

	// ★★★ 第一步：从缓存中获取最大页面宽度（O(1)，不再调用 MuPDF）
	float maxPageWidth = 0.0f;
	if (pDoc && pDoc->HasPageBoundsCache())
	{
		// 直接使用缓存中已计算好的最大宽度
		maxPageWidth = pDoc->GetMaxPageWidth();
	}
	else
	{
		// 回退：缓存不可用时仍遍历（兼容 m_doc 与 pDoc 不一致的极端情况）
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
	}

	// ★★★ 仅在需要时重新计算缩放比例（用户主动缩放或首次加载时）
	// 窗口大小改变时传 recalcScale=false，保持用户设定的缩放不变
	if (recalcScale)
	{
		float baseScale = (maxPageWidth > 0) ? ((float)viewWidth / maxPageWidth * 0.95f) : 1.0f;
		m_uniformScale = baseScale * m_customZoom;
		if (m_zoomMode == ZOOM_FIT_WIDTH)
			m_uniformScale = FitWidthScale(viewRect.Width(), maxPageWidth);
		if (m_zoomMode == ZOOM_FIT_PAGE && pDoc && pDoc->HasPageBoundsCache())
		{
			const int viewHeight = max(1, viewRect.Height() - 40);
			float fitScale = 0.0f;
			for (int i = 0; i < m_totalPages; ++i)
			{
				float pageWidth = 0.0f;
				float pageHeight = 0.0f;
				if (!pDoc->GetPageBounds(i, pageWidth, pageHeight) ||
					pageWidth <= 0.0f || pageHeight <= 0.0f)
					continue;
				const float pageScale = min((float)viewWidth / pageWidth,
					(float)viewHeight / pageHeight);
				if (fitScale == 0.0f || pageScale < fitScale)
					fitScale = pageScale;
			}
			if (fitScale > 0.0f)
				m_uniformScale = fitScale * 0.95f;
		}
#ifdef _DEBUG
		TRACE(_T("连续滚动模式: 最大页面宽度=%.2f, 基准缩放=%.4f, customZoom=%.2f, 统一缩放比例=%.4f\n"),
			maxPageWidth, baseScale, m_customZoom, m_uniformScale);
#endif
	}
#ifdef _DEBUG
	else
	{
		TRACE(_T("连续滚动模式(保持缩放): 最大页面宽度=%.2f, customZoom=%.2f, 统一缩放比例=%.4f\n"),
			maxPageWidth, m_customZoom, m_uniformScale);
	}
#endif

	// ★★★ 第二步：使用统一的缩放比例计算所有页面的高度
	// 优先从缓存读取，无需再次调用 fz_load_page
	// ★★★ 修复：第一页从 PAGE_SPACING 开始，确保顶部有灰色间隔
	int currentY = PAGE_SPACING;
	for (int i = 0; i < m_totalPages; i++)
	{
		m_pageYPositions[i] = currentY;

		float pageHeight = 0.0f;
		float pageWidth  = 0.0f;
		bool gotBounds   = false;

		// 尝试从缓存获取
		if (pDoc)
			gotBounds = pDoc->GetPageBounds(i, pageWidth, pageHeight);

		if (gotBounds)
		{
			// ★★★ 缓存命中：直接计算，零 MuPDF 调用
			int scaledHeight = (int)(pageHeight * m_uniformScale);
			m_pageHeights[i] = scaledHeight;
			currentY += scaledHeight + PAGE_SPACING;
		}
		else
		{
			// 回退：缓存未命中时走原有 fz_load_page 路径
			fz_page* page = nullptr;
			fz_try(m_ctx)
			{
				page = fz_load_page(m_ctx, m_doc, i);
				if (page)
				{
					fz_rect bounds = fz_bound_page(m_ctx, page);
					float w = bounds.x1 - bounds.x0;
					float h = bounds.y1 - bounds.y0;

					int rotation = GetPageRotation(i);
					if (rotation == 90 || rotation == 270)
					{
						float temp = w;
						w = h;
						h = temp;
					}

					int scaledHeight = (int)(h * m_uniformScale);
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
	}

	// ★★★ 修复：保留最后一页的底部间距，确保底部有灰色间隔
	m_totalScrollHeight = currentY;

	// ★★★ 设置横向滚动宽度（使用最大页面宽度 × 统一缩放比例）
	m_totalScrollWidth = (int)(maxPageWidth * m_uniformScale);

#ifdef _DEBUG
	TRACE(_T("连续滚动: 总高度=%d, 总宽度=%d\n"), m_totalScrollHeight, m_totalScrollWidth);
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
	CPDFDocument* pActiveDoc = GetActiveDocument();
	if (!pActiveDoc)
	{
		m_pdfView.ReleaseDC(pDC);
		return;
	}

	CBitmap bmp;
	bmp.CreateCompatibleBitmap(pDC, viewWidth, viewHeight);
	CBitmap* pOldBmp = memDC.SelectObject(&bmp);

	// 填充背景色（灰色）
	memDC.FillSolidRect(0, 0, viewWidth, viewHeight, PDF_VIEW_BACKGROUND);

	auto firstVisibleIt = std::lower_bound(m_pageYPositions.begin(), m_pageYPositions.end(), visibleTop);
	int firstVisibleIndex = (int)(firstVisibleIt - m_pageYPositions.begin());
	if (firstVisibleIndex > 0)
	{
		--firstVisibleIndex;
	}

	auto lastVisibleIt = std::lower_bound(m_pageYPositions.begin(), m_pageYPositions.end(),
		visibleBottom + PAGE_SPACING);
	int lastVisibleIndex = (int)(lastVisibleIt - m_pageYPositions.begin());
	if (lastVisibleIndex < m_totalPages - 1)
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

		int rotation = GetPageRotation(i);
		float pageWidthF = 0.0f;
		float pageHeightF = 0.0f;
		bool gotBounds = pActiveDoc->GetPageBounds(i, pageWidthF, pageHeightF);
		fz_rect baseBounds = { 0 };
		bool gotBaseBounds = pActiveDoc->GetPageBaseBounds(i, baseBounds);
		int width = gotBounds ? ClampRenderDimension(pageWidthF * m_uniformScale) : 0;
		int height = gotBounds ? ClampRenderDimension(pageHeightF * m_uniformScale) : 0;

		::PageCacheKey cacheKey{ i, width, height, rotation };
		auto& pageCache = pActiveDoc->GetPageCache();
		auto& cacheOrder = pActiveDoc->GetCacheOrder();
		HBITMAP hBitmap = nullptr;
		const std::vector<int>* pMatchIndices =
			(i >= 0 && i < (int)m_searchMatchesByPage.size()) ? &m_searchMatchesByPage[i] : nullptr;
		bool pageHasHighlights = pMatchIndices && !pMatchIndices->empty();

		if (gotBounds)
		{
			auto it = pageCache.find(cacheKey);
			if (it != pageCache.end())
			{
				hBitmap = it->second.hBitmap;
				cacheOrder.remove(cacheKey);
				cacheOrder.push_back(cacheKey);

#ifdef _DEBUG
				TRACE(_T("RenderVisiblePages: 缓存命中 Page %d\n"), i);
#endif
			}
		}

		fz_page* page = nullptr;
		fz_pixmap* pixmap = nullptr;
		fz_device* dev = nullptr;
		fz_rect bounds = baseBounds;
		bool renderFailed = false;

		if (!hBitmap || !gotBounds || !gotBaseBounds)
		{
			fz_try(m_ctx)
			{
				page = fz_load_page(m_ctx, m_doc, i);
				if (!page)
					fz_throw(m_ctx, FZ_ERROR_GENERIC, "Failed to load page");

				bounds = fz_bound_page(m_ctx, page);
				gotBaseBounds = true;

				if (!gotBounds)
				{
					pageWidthF = bounds.x1 - bounds.x0;
					pageHeightF = bounds.y1 - bounds.y0;
					if (rotation == 90 || rotation == 270)
					{
						float temp = pageWidthF;
						pageWidthF = pageHeightF;
						pageHeightF = temp;
					}

					width = ClampRenderDimension(pageWidthF * m_uniformScale);
					height = ClampRenderDimension(pageHeightF * m_uniformScale);
					cacheKey.pageNumber = i;
					cacheKey.width = width;
					cacheKey.height = height;
					cacheKey.rotation = rotation;

					auto it = pageCache.find(cacheKey);
					if (it != pageCache.end())
					{
						hBitmap = it->second.hBitmap;
						cacheOrder.remove(cacheKey);
						cacheOrder.push_back(cacheKey);
					}
				}

				if (!hBitmap)
				{
#ifdef _DEBUG
					TRACE(_T("RenderVisiblePages: 缓存未命中，渲染 Page %d\n"), i);
#endif

					pixmap = fz_new_pixmap(m_ctx, fz_device_rgb(m_ctx), width, height, nullptr, 1);
					fz_clear_pixmap_with_value(m_ctx, pixmap, 0xff);

					fz_matrix ctm = fz_scale(m_uniformScale, m_uniformScale);
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

					BITMAPINFO bmi = { 0 };
					bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
					bmi.bmiHeader.biWidth = width;
					bmi.bmiHeader.biHeight = -height;
					bmi.bmiHeader.biPlanes = 1;
					bmi.bmiHeader.biBitCount = 24;
					bmi.bmiHeader.biCompression = BI_RGB;

					BYTE* pbBits = nullptr;
					hBitmap = CreateDIBSection(memDC.GetSafeHdc(), &bmi,
						DIB_RGB_COLORS, (void**)&pbBits, NULL, 0);

					if (hBitmap && pbBits)
					{
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

						::PageCacheItem cacheItem;
						cacheItem.hBitmap = hBitmap;
						pageCache[cacheKey] = cacheItem;
						cacheOrder.push_back(cacheKey);

						TrimBitmapCache(pageCache, cacheOrder, true,
							PAGE_CACHE_BUDGET, 50);
					}

					if (pixmap)
					{
						fz_drop_pixmap(m_ctx, pixmap);
						pixmap = nullptr;
					}
				}
			}
			fz_catch(m_ctx)
			{
				if (dev)
				{
					fz_drop_device(m_ctx, dev);
					dev = nullptr;
				}
				if (pixmap)
				{
					fz_drop_pixmap(m_ctx, pixmap);
					pixmap = nullptr;
				}
				if (page)
				{
					fz_drop_page(m_ctx, page);
					page = nullptr;
				}
				renderFailed = true;
			}
		}

		if (renderFailed || !hBitmap)
			continue;

		CDC pageDC;
		pageDC.CreateCompatibleDC(&memDC);
		HBITMAP oldPageBmp = (HBITMAP)pageDC.SelectObject(hBitmap);

		int destY = pageY - visibleTop + m_panOffset.y;
		int destX = (width < viewWidth ? (viewWidth - width) / 2 :
			-min(m_scrollPositionH, width - viewWidth)) + m_panOffset.x;

#ifdef _DEBUG
		TRACE(_T("  Page %d: pageY=%d, size=(%d,%d), dest=(%d,%d)\n"),
			i, pageY, width, height, destX, destY);
#endif

		memDC.BitBlt(destX, destY, width, height, &pageDC, 0, 0, SRCCOPY);

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
				memDC.SelectStockObject(NULL_BRUSH);
				memDC.Rectangle(highlightRect);
				memDC.SelectObject(pOldPen);
			}
		}

		pageDC.SelectObject(oldPageBmp);
		if (page)
			fz_drop_page(m_ctx, page);
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
		TRACE(_T("RenderVisiblePages: detach bitmap failed\n"));
	}

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

		// ★★★ 使用异步延迟更新缩略图高亮，避免阻塞滚动
		// 使用PostMessage延迟执行，不阻塞当前渲染
		if (m_thumbnailVisible && m_thumbnailList.GetSafeHwnd())
		{
			PostMessage(WM_USER + 103, 0, 0);  // 延迟更新缩略图
		}
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
		m_scrollPosition = newPos;
		UpdateScrollBar();

		// ★★★ 优化：使用页面缓存后，可以立即渲染，无需定时器节流
		RenderVisiblePages();
	}

	CDialogEx::OnVScroll(nSBCode, nPos, pScrollBar);
}

void CXiaoGongPDFDlg::ScrollToVerticalPosition(int position)
{
	if (!m_doc || m_isFullscreen)
		return;
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	position = max(0, min(position, max(0, m_totalScrollHeight - viewRect.Height())));
	if (position == m_scrollPosition)
		return;
	m_scrollPosition = position;
	UpdateScrollBar();
	if (m_continuousScrollMode)
		RenderVisiblePages();
	else
		RefreshCurrentView();
}

void CXiaoGongPDFDlg::ScrollToHorizontalPosition(int position)
{
	if (!m_doc || m_isFullscreen)
		return;
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	position = max(0, min(position, max(0, m_totalScrollWidth - viewRect.Width())));
	if (position == m_scrollPositionH)
		return;
	m_scrollPositionH = position;
	UpdateScrollBar();
	if (m_continuousScrollMode)
	{
		RenderVisiblePages();
		RefreshCurrentView();
	}
	else
		RefreshCurrentView();
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
		m_scrollPositionH = newPos;
		UpdateScrollBar();

		// ★★★ 触发重绘（不重新渲染PDF，只是用新的滚动位置重新绘制）
		RefreshCurrentView();

		// ★★★ 连续滚动模式需要重新渲染可见页面
		if (m_continuousScrollMode)
		{
			RenderVisiblePages();
		}
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
	if (m_continuousScrollMode && m_doc &&
		m_pageYPositions.size() == static_cast<size_t>(m_totalPages) &&
		m_pageHeights.size() == static_cast<size_t>(m_totalPages))
	{
		CPDFDocument* pDoc = GetActiveDocument();
		if (pDoc)
		{
			const int visibleTop = m_scrollPosition;
			const int visibleBottom = visibleTop + viewRect.Height();
			auto first = std::lower_bound(m_pageYPositions.begin(),
				m_pageYPositions.end(), visibleTop);
			int firstIndex = static_cast<int>(first - m_pageYPositions.begin());
			if (firstIndex > 0)
				--firstIndex;
			int visibleWidth = 0;
			bool boundsMissing = false;
			for (int i = firstIndex; i < m_totalPages &&
				m_pageYPositions[i] <= visibleBottom; ++i)
			{
				if (m_pageYPositions[i] + m_pageHeights[i] < visibleTop)
					continue;
				float pageWidth = 0.0f;
				float pageHeight = 0.0f;
				if (!pDoc->GetPageBounds(i, pageWidth, pageHeight))
				{
					boundsMissing = true;
					break;
				}
				visibleWidth = max(visibleWidth,
					ClampRenderDimension(pageWidth * m_uniformScale));
			}
			if (!boundsMissing)
				horizontalContentWidth = visibleWidth;
		}
	}
	m_scrollPositionH = max(0, min(m_scrollPositionH,
		max(0, horizontalContentWidth - viewRect.Width())));
	if (m_pdfVerticalScrollBar.GetSafeHwnd())
	{
		m_pdfVerticalScrollBar.SetRange(m_totalScrollHeight, viewRect.Height(), m_scrollPosition);
		m_pdfVerticalScrollBar.ShowWindow(m_doc && m_totalScrollHeight > viewRect.Height() ?
			SW_SHOW : SW_HIDE);
	}
	if (m_pdfHorizontalScrollBar.GetSafeHwnd())
	{
		m_pdfHorizontalScrollBar.SetRange(horizontalContentWidth, viewRect.Width(), m_scrollPositionH);
		m_pdfHorizontalScrollBar.ShowWindow(m_doc && horizontalContentWidth > viewRect.Width() ?
			SW_SHOW : SW_HIDE);
	}
	if (m_pdfScrollCorner.GetSafeHwnd())
		m_pdfScrollCorner.ShowWindow(m_doc &&
			(m_totalScrollHeight > viewRect.Height() || horizontalContentWidth > viewRect.Width()) ?
			SW_SHOW : SW_HIDE);
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

// 后台搜索线程函数
void CXiaoGongPDFDlg::StopSearch()
{
	if (m_pSearchParams)
		m_pSearchParams->cancelled.store(true);
	if (m_pSearchThread)
	{
		// 文档必须在工作线程结束后才能释放。不能超时后删除参数或强制终止线程。
		::WaitForSingleObject(m_pSearchThread->m_hThread, INFINITE);
		delete m_pSearchThread;
		m_pSearchThread = nullptr;
	}
	delete m_pSearchParams;
	m_pSearchParams = nullptr;
	m_isSearching = false;
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
}

void CXiaoGongPDFDlg::NavigateToSearchMatch(int index)
{
	if (m_isSearching || !m_doc || index < 0 || index >= static_cast<int>(m_searchMatches.size()))
		return;
	m_currentMatchIndex = index;
	if (m_searchResultsList.GetSafeHwnd() && index < m_searchResultsList.GetItemCount())
	{
		m_updatingSearchResultsList = true;
		m_searchResultsList.SetItemState(-1, 0, LVIS_SELECTED | LVIS_FOCUSED);
		m_searchResultsList.SetItemState(index,
			LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		m_searchResultsList.EnsureVisible(index, FALSE);
		m_updatingSearchResultsList = false;
	}
	GoToPage(m_searchMatches[index].pageNumber);
	ScrollToMatchVisible(m_searchMatches[index].pageNumber, m_searchMatches[index].quad);
	CString status;
	status.Format(_T("匹配项 %d / %d"), index + 1, static_cast<int>(m_searchMatches.size()));
	SetStatusBarMessage(status);
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

void CXiaoGongPDFDlg::OnMenuSearchResults()
{
	if (m_searchMatches.empty())
		return;
	m_searchResultsVisible = true;
	m_outlineVisible = false;
	m_btnOutline.SetColors(RGB(255, 255, 255), RGB(232, 241, 252), RGB(211, 229, 250));
	m_btnOutline.SetToolTipText(_T("显示目录"));
	CRect rect;
	GetClientRect(&rect);
	SendMessage(WM_SIZE, SIZE_RESTORED, MAKELPARAM(rect.Width(), rect.Height()));
}

UINT CXiaoGongPDFDlg::SearchThreadProc(LPVOID pParam)
{
	SearchParams* pParams = (SearchParams*)pParam;
	if (!pParams)
		return 1;
	fz_context* ctx = fz_new_context(nullptr, nullptr, 128u * 1024u * 1024u);
	if (!ctx)
		pParams->failed = true;
	fz_document* doc = nullptr;
	fz_var(doc);
	const char* keyword = pParams->utf8Keyword.data();
	std::vector<int> exactNeedle;
	if (pParams->caseSensitive)
	{
		for (const char* p = keyword; *p; )
		{
			int rune = 0;
			p += fz_chartorune(&rune, p);
			exactNeedle.push_back(rune);
		}
	}
	if (ctx)
	{
		fz_try(ctx)
		{
			fz_register_document_handlers(ctx);
			if (!pParams->cancelled.load())
				doc = fz_open_document(ctx, pParams->utf8Path.data());
			if (doc && fz_needs_password(ctx, doc) &&
				(pParams->utf8Password.empty() ||
				 !fz_authenticate_password(ctx, doc, pParams->utf8Password.data())))
				pParams->failed = true;
		}
		fz_catch(ctx)
		{
			pParams->failed = true;
		}
	}
	if (!doc && !pParams->cancelled.load())
		pParams->failed = true;
	if (doc && !pParams->failed)
	{
		int totalPages = 0;
		fz_try(ctx)
		{
			totalPages = fz_count_pages(ctx, doc);
		}
		fz_catch(ctx)
		{
			totalPages = 0;
			pParams->failed = true;
		}
		for (int i = 0; i < totalPages && !pParams->cancelled.load(); ++i)
		{
			fz_page* page = nullptr;
			fz_stext_page* stext = nullptr;
			const size_t firstResult = pParams->results.size();
			fz_try(ctx)
			{
				page = fz_load_page(ctx, doc, i);
				stext = fz_new_stext_page_from_page(ctx, page, nullptr);
				if (pParams->caseSensitive)
					FindCaseSensitiveHits(stext, exactNeedle, i, pParams->results);
				else
				{
					SearchHitCollector collector{ i, &pParams->results };
					fz_search_stext_page_cb(ctx, stext, keyword, CollectSearchHit, &collector);
				}
				for (size_t resultIndex = firstResult; resultIndex < pParams->results.size(); ++resultIndex)
					pParams->results[resultIndex].context =
						BuildSearchContext(stext, pParams->results[resultIndex].quad);
			}
			fz_catch(ctx)
			{
				// 单页损坏不影响其他页面的搜索。
			}
			if (stext) fz_drop_stext_page(ctx, stext);
			if (page) fz_drop_page(ctx, page);
			if (i % 10 == 0 && pParams->notifyWnd)
				::PostMessage(pParams->notifyWnd, WM_SEARCH_PROGRESS,
					pParams->taskId, static_cast<LPARAM>(i + 1));
		}
	}
	if (doc)
		fz_drop_document(ctx, doc);
	if (ctx)
		fz_drop_context(ctx);

	// 通知主线程搜索完成
	if (pParams->notifyWnd)
		::PostMessage(pParams->notifyWnd, WM_SEARCH_COMPLETE, pParams->taskId, (LPARAM)pParams);

	return 0;
}

LRESULT CXiaoGongPDFDlg::OnSearchProgress(WPARAM wParam, LPARAM lParam)
{
	if (m_isSearching && wParam == m_searchTaskId && m_pSearchParams &&
		m_pSearchParams->pDoc == GetActiveDocument())
	{
		CString status;
		status.Format(_T("正在搜索：%d / %d 页"), static_cast<int>(lParam), m_totalPages);
		SetStatusBarMessage(status);
	}
	return 0;
}

// 搜索完成消息处理
LRESULT CXiaoGongPDFDlg::OnSearchComplete(WPARAM wParam, LPARAM lParam)
{
	SearchParams* pParams = (SearchParams*)lParam;
	// StopSearch 可能已经释放了旧任务；旧完成消息只携带悬空地址，不能解引用。
	if (!pParams || pParams != m_pSearchParams || wParam != m_searchTaskId)
		return 0;

	m_isSearching = false;
	if (m_pSearchThread)
	{
		::WaitForSingleObject(m_pSearchThread->m_hThread, INFINITE);
		delete m_pSearchThread;
		m_pSearchThread = nullptr;
	}

	// 如果搜索已被取消或用户关闭了文档，忽略结果
	if (pParams->cancelled.load() || pParams->pDoc != GetActiveDocument() ||
		!m_doc || m_totalPages <= 0)
	{
		delete pParams;
		m_pSearchParams = nullptr;
		return 0;
	}

	// 将搜索结果转移到主对话框
	m_searchMatches = std::move(pParams->results);

	RebuildSearchMatchPageIndex();
	m_currentMatchIndex = m_searchMatches.empty() ? -1 : 0;
	RefreshSearchResultsList();

	if (pParams->failed)
	{
		SetStatusBarMessage(_T("搜索失败"));
		MessageBox(_T("无法读取文档文本，请检查文件是否仍可访问。"),
			_T("搜索失败"), MB_OK | MB_ICONERROR);
	}
	else if (!m_searchMatches.empty())
	{
		CString msg;
		msg.Format(_T("找到 %d 个匹配项"), (int)m_searchMatches.size());
		SetStatusBarMessage(msg);

		NavigateToSearchMatch(0);
	}
	else
	{
		SetStatusBarMessage(_T("未找到匹配项"));
	}

	// 清理
	delete pParams;
	m_pSearchParams = nullptr;

	// 刷新当前视图以移除搜索高亮（如果有的话，新结果已应用）
	RefreshCurrentView();

	return 0;
}

// 清除搜索结果
void CXiaoGongPDFDlg::ClearSearchResults()
{
	m_searchMatches.clear();
	m_searchMatchesByPage.clear();
	m_currentMatchIndex = -1;
	m_searchKeyword.Empty();
	m_searchResultsVisible = false;
	RefreshSearchResultsList();
	if (m_searchResultsList.GetSafeHwnd())
		RefreshDocumentLayout();

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

	if (pageNumber == m_currentPage && m_currentPageObj && m_ctx)
	{
		outBounds = fz_bound_page(m_ctx, m_currentPageObj);
		return true;
	}

	return false;
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
	if (!m_doc || m_totalPages <= 0 || keyword.IsEmpty())
	{
		MessageBox(_T("请输入搜索关键词"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	StopSearch();

	// 清除之前的搜索结果并刷新视图（移除旧高亮）
	ClearSearchResults();
	RefreshCurrentView();

	// 保存搜索参数
	m_searchKeyword = keyword;
	m_searchCaseSensitive = caseSensitive;

	// 将CString转换为UTF-8
	int utf8Len = WideCharToMultiByte(CP_UTF8, 0, keyword, -1, NULL, 0, NULL, NULL);
	if (utf8Len <= 0)
		return;
	std::vector<char> utf8Keyword(utf8Len);
	WideCharToMultiByte(CP_UTF8, 0, keyword, -1, utf8Keyword.data(), utf8Len, NULL, NULL);

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

	SearchParams* pParams = new SearchParams();
	pParams->notifyWnd = GetSafeHwnd();
	pParams->pDoc = pActiveDoc;
	pParams->taskId = ++m_searchTaskId;
	pParams->utf8Keyword = std::move(utf8Keyword);
	pParams->utf8Path = std::move(utf8Path);
	pParams->utf8Password = pActiveDoc->GetPassword();
	pParams->caseSensitive = caseSensitive;

	m_pSearchParams = pParams;
	m_isSearching = true;
	m_searchResultsVisible = true;
	m_outlineVisible = false;
	m_btnOutline.SetColors(RGB(255, 255, 255), RGB(232, 241, 252), RGB(211, 229, 250));
	m_btnOutline.SetToolTipText(_T("显示目录"));
	RefreshSearchResultsList();
	RefreshDocumentLayout();

	SetStatusBarMessage(_T("正在搜索..."));

	m_pSearchThread = AfxBeginThread(SearchThreadProc, pParams,
		THREAD_PRIORITY_BELOW_NORMAL, 0, CREATE_SUSPENDED);
	if (m_pSearchThread)
	{
		m_pSearchThread->m_bAutoDelete = FALSE;
		m_pSearchThread->ResumeThread();
	}
	else
	{
		m_isSearching = false;
		delete pParams;
		m_pSearchParams = nullptr;
		RefreshSearchResultsList();
		SetStatusBarMessage(_T("搜索启动失败"));
		MessageBox(_T("无法启动搜索线程"), _T("错误"), MB_OK | MB_ICONERROR);
	}
}
// 跳转到下一个匹配项
void CXiaoGongPDFDlg::GoToNextMatch()
{
	if (m_isSearching) return;  // 搜索进行中
	if (m_searchMatches.empty())
	{
		MessageBox(_T("请先执行搜索"), _T("提示"), MB_OK | MB_ICONINFORMATION);
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
		MessageBox(_T("请先执行搜索"), _T("提示"), MB_OK | MB_ICONINFORMATION);
		return;
	}

	NavigateToSearchMatch((m_currentMatchIndex - 1 + static_cast<int>(m_searchMatches.size())) %
		static_cast<int>(m_searchMatches.size()));
}

// 查找按钮点击事件
void CXiaoGongPDFDlg::OnBtnFind()
{
	if (m_isSearching) return;  // 搜索进行中，忽略重复点击

	CString keyword;
	m_editSearch.GetWindowText(keyword);

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

// 将PDF坐标转换为位图坐标
CRect CXiaoGongPDFDlg::TransformQuadToScreen(const fz_quad& quad, int pageNumber)
{
	fz_rect bounds;
	if (!TryGetPageBaseBounds(pageNumber, bounds))
		return CRect(0, 0, 0, 0);

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
		scale = FitWidthScale(viewWidth, effectivePageWidth);
		break;
	case ZOOM_FIT_PAGE:
	{
		float scaleX = effectivePageWidth > 0.0f ? viewWidth / effectivePageWidth : 1.0f;
		float scaleY = effectivePageHeight > 0.0f ? viewHeight / effectivePageHeight : 1.0f;
		scale = min(scaleX, scaleY) * 0.95f;
	}
	break;
	case ZOOM_CUSTOM:
		scale = m_customZoom;
		break;
	default:
	{
		float scaleX = effectivePageWidth > 0.0f ? viewWidth / effectivePageWidth : 1.0f;
		float scaleY = effectivePageHeight > 0.0f ? viewHeight / effectivePageHeight : 1.0f;
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

	CRect rect = TransformQuadToBitmapRect(quad, bounds, rotation, scale);
	rect.OffsetRect(offsetX, offsetY);

#ifdef _DEBUG
	TRACE(_T("TransformQuadToScreen: rect=(%d,%d,%d,%d), offset=(%d,%d), scale=%.2f, rotation=%d\n"),
		rect.left, rect.top, rect.right, rect.bottom, offsetX, offsetY, scale, rotation);
#endif

	return rect;
}

// 高亮显示搜索匹配项
void CXiaoGongPDFDlg::HighlightSearchMatches(CDC* pDC, int pageNumber)
{
	if (m_searchMatches.empty() || !pDC ||
		pageNumber < 0 || pageNumber >= (int)m_searchMatchesByPage.size())
		return;

#ifdef _DEBUG
	TRACE(_T("HighlightSearchMatches: pageNumber=%d, matchCount=%d\n"),
		pageNumber, (int)m_searchMatchesByPage[pageNumber].size());
#endif

	for (int matchIndex : m_searchMatchesByPage[pageNumber])
	{
		// 将PDF坐标转换为位图坐标
		CRect highlightRect = TransformQuadToScreen(m_searchMatches[matchIndex].quad, pageNumber);

#ifdef _DEBUG
		TRACE(_T("Match %d: rect=(%d,%d,%d,%d)\n"), matchIndex,
			highlightRect.left, highlightRect.top, highlightRect.right, highlightRect.bottom);
#endif

		// 确保矩形有效且在合理范围内
		if (highlightRect.IsRectEmpty() || highlightRect.Width() <= 0 || highlightRect.Height() <= 0)
			continue;

		// 选择颜色
		COLORREF fillColor;
		if (matchIndex == m_currentMatchIndex)
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
		bf.SourceConstantAlpha = (matchIndex == m_currentMatchIndex) ? 200 : 180;
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
}

// 重绘视图以更新选择高亮
void CXiaoGongPDFDlg::InvalidateSelection()
{
	if (m_pdfView.GetSafeHwnd())
		m_pdfView.Invalidate(FALSE);
}

// 计算页面在视图中的实际缩放与偏移
bool CXiaoGongPDFDlg::GetSelectionPageTransform(int pageNumber, fz_rect& bounds,
	float& scale, int& offsetX, int& offsetY)
{
	if (!TryGetPageBaseBounds(pageNumber, bounds))
		return false;

	const float pageWidth = bounds.x1 - bounds.x0;
	const float pageHeight = bounds.y1 - bounds.y0;
	const int rotation = GetPageRotation(pageNumber);
	const float effectiveWidth = (rotation == 90 || rotation == 270) ? pageHeight : pageWidth;
	const float effectiveHeight = (rotation == 90 || rotation == 270) ? pageWidth : pageHeight;
	CRect viewRect;
	m_pdfView.GetClientRect(&viewRect);
	const int viewWidth = viewRect.Width();
	const int viewHeight = viewRect.Height();

	if (m_continuousScrollMode)
	{
		if (pageNumber >= (int)m_pageYPositions.size())
			return false;
		scale = m_uniformScale;
		const int width = ClampRenderDimension(effectiveWidth * scale);
		offsetX = (viewWidth - width) / 2 + m_panOffset.x;
		offsetY = m_pageYPositions[pageNumber] - m_scrollPosition + m_panOffset.y;
	}
	else
	{
		switch (m_zoomMode)
		{
		case ZOOM_FIT_WIDTH:
			scale = FitWidthScale(viewWidth, effectiveWidth);
			break;
		case ZOOM_FIT_PAGE:
			scale = min(viewWidth / effectiveWidth, viewHeight / effectiveHeight) * 0.95f;
			break;
		default:
			scale = m_customZoom;
			break;
		}
		BITMAP bitmap = { 0 };
		if (!m_hCurrentBitmap || !::GetObject(m_hCurrentBitmap, sizeof(bitmap), &bitmap))
			return false;
		offsetX = bitmap.bmWidth < viewWidth ? (viewWidth - bitmap.bmWidth) / 2 : -m_scrollPositionH;
		offsetY = bitmap.bmHeight < viewHeight ? (viewHeight - bitmap.bmHeight) / 2 : -m_scrollPosition;
		if (m_zoomMode == ZOOM_CUSTOM && (m_panOffset.x != 0 || m_panOffset.y != 0))
		{
			offsetX += (bitmap.bmWidth - ClampRenderDimension(effectiveWidth * scale)) / 2 + m_panOffset.x;
			offsetY += (bitmap.bmHeight - ClampRenderDimension(effectiveHeight * scale)) / 2 + m_panOffset.y;
		}
	}
	return scale > 0;
}

void CXiaoGongPDFDlg::UpdateSelectedText()
{
	m_selectionQuads.clear();
	m_selectedText.Empty();
	if (!m_selectionTextPage || m_selPageNumber < 0)
		return;

	float startX, startY, endX, endY;
	if (!ScreenPointToPdfCoords(m_selStartView, m_selPageNumber, startX, startY) ||
		!ScreenPointToPdfCoords(m_selEndView, m_selPageNumber, endX, endY))
		return;

	const fz_point start = { startX, startY };
	const fz_point end = { endX, endY };
	CSingleLock lock(&m_renderLock, TRUE);
	fz_try(m_ctx)
	{
		m_selectionQuads.resize(4096);
		const int count = fz_highlight_selection(m_ctx, m_selectionTextPage,
			start, end, m_selectionQuads.data(), (int)m_selectionQuads.size());
		m_selectionQuads.resize(max(0, min(count, (int)m_selectionQuads.size())));
		if (!m_isSelecting && !m_selectionQuads.empty())
		{
			char* utf8 = fz_copy_selection(m_ctx, m_selectionTextPage, start, end, 1);
			if (utf8)
			{
				m_selectedText = CA2W(utf8, CP_UTF8);
				fz_free(m_ctx, utf8);
			}
		}
	}
	fz_catch(m_ctx)
	{
		m_selectionQuads.clear();
		m_selectedText.Empty();
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

// 复制选中文本到剪贴板
void CXiaoGongPDFDlg::CopySelectionToClipboard()
{
	CString text = ExtractSelectedText();
	if (text.IsEmpty())
		return;

	if (!::OpenClipboard(GetSafeHwnd()))
		return;
	::EmptyClipboard();

	size_t byteSize = (text.GetLength() + 1) * sizeof(WCHAR);
	HGLOBAL hGlobal = ::GlobalAlloc(GMEM_MOVEABLE, byteSize);
	if (hGlobal)
	{
		void* pData = ::GlobalLock(hGlobal);
		if (pData)
		{
			memcpy(pData, (LPCTSTR)text, byteSize);
			::GlobalUnlock(hGlobal);
			if (::SetClipboardData(CF_UNICODETEXT, hGlobal))
				hGlobal = nullptr;
		}
		if (hGlobal)
			::GlobalFree(hGlobal);
	}
	::CloseClipboard();
}

// 文本选择按钮点击事件
void CXiaoGongPDFDlg::OnBtnSelectText()
{
	m_selectionMode = !m_selectionMode;
	m_btnSelectText.SetWindowText(m_selectionMode ? _T("选择") : _T("拖动"));
	m_btnSelectText.SetIconGlyph(m_selectionMode ? 0xE933 : 0xE7C9);
	m_btnSelectText.SetToolTipText(m_selectionMode ?
		_T("文字选择，点击切换为拖动") : _T("拖动页面，点击切换为选择"));
	if (!m_selectionMode)
	{
		ClearSelection();
		InvalidateSelection();
	}

#ifdef _DEBUG
	TRACE(_T("OnBtnSelectText: mode=%d\n"), m_selectionMode);
#endif
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

LRESULT CXiaoGongPDFDlg::OnPDFLoadCancelled(WPARAM, LPARAM)
{
	if (m_pLoadParams)
		m_pLoadParams->cancelled.store(true);
	return 0;
}

// PDF加载完成消息处理
LRESULT CXiaoGongPDFDlg::OnPDFLoadComplete(WPARAM wParam, LPARAM lParam)
{
	PDFLoadParams* pParams = (PDFLoadParams*)lParam;
	if (!pParams || pParams != m_pLoadParams)
	{
		return 0;
	}
	if (m_closePending)
	{
		CleanupPendingLoad();
		CDialogEx::OnCancel();
		return 0;
	}
	if (m_pProgressDlg && static_cast<CProgressDlg*>(m_pProgressDlg)->IsCancelled())
		pParams->cancelled.store(true);
	if (pParams->cancelled.load())
	{
		delete pParams->pDoc;
		delete pParams;
		m_pLoadParams = nullptr;
		ReleaseLoadThread();
		CloseProgressDialog();
		UpdateStatusBar();
		return 0;
	}

	// 检查加载结果
	if (!pParams->success)
	{
		if (pParams->openResult == PDFOpenResult::PasswordRequired ||
			pParams->openResult == PDFOpenResult::InvalidPassword)
		{
			const bool invalidPassword = pParams->openResult == PDFOpenResult::InvalidPassword;
			ReleaseLoadThread();
			CloseProgressDialog();
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
				CleanupPendingLoad();
				UpdateStatusBar();
				return 0;
			}

			const CString& password = passwordDlg.GetPassword();
			const int length = WideCharToMultiByte(CP_UTF8, 0, password, -1,
				nullptr, 0, nullptr, nullptr);
			if (length <= 0)
			{
				CleanupPendingLoad();
				MessageBox(_T("无法读取密码"), _T("错误"), MB_OK | MB_ICONERROR);
				return 0;
			}
			pParams->utf8Password.resize(length);
			if (!WideCharToMultiByte(CP_UTF8, 0, password, -1,
				pParams->utf8Password.data(), length, nullptr, nullptr))
			{
				CleanupPendingLoad();
				MessageBox(_T("无法读取密码"), _T("错误"), MB_OK | MB_ICONERROR);
				return 0;
			}

			CProgressDlg* pProgressDlg = new CProgressDlg(this);
			if (!pProgressDlg->Create(IDD_PROGRESS_DIALOG, this))
			{
				delete pProgressDlg;
				CleanupPendingLoad();
				MessageBox(_T("无法创建加载窗口"), _T("错误"), MB_OK | MB_ICONERROR);
				return 0;
			}
			m_pProgressDlg = pProgressDlg;
			pProgressDlg->ShowWindow(SW_SHOW);
			pProgressDlg->UpdateWindow();
			pParams->openResult = PDFOpenResult::Failed;
			pParams->success = false;
			m_pLoadThread = AfxBeginThread(PDFLoadThreadProc, pParams,
				THREAD_PRIORITY_NORMAL, 0, CREATE_SUSPENDED);
			if (!m_pLoadThread)
			{
				CleanupPendingLoad();
				MessageBox(_T("无法启动加载线程"), _T("错误"), MB_OK | MB_ICONERROR);
				return 0;
			}
			m_pLoadThread->m_bAutoDelete = FALSE;
			m_pLoadThread->ResumeThread();
			return 0;
		}
		CString errMsg = pParams->errorMsg;
		delete pParams->pDoc;
		delete pParams;
		m_pLoadParams = nullptr;
		ReleaseLoadThread();

		CloseProgressDialog();

		MessageBox(errMsg, _T("错误"), MB_OK | MB_ICONERROR);
		UpdateStatusBar();
		return 0;
	}

	// 加载成功，恢复此文件上次的阅读位置。
	RestoreReadingPosition(pParams->pDoc);
	// 加载成功，添加到文档列表
	m_documents.push_back(pParams->pDoc);
	int newIndex = (int)m_documents.size() - 1;

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

		// 截断过长的文件名
		const int MAX_TAB_LENGTH = 12;
		if (tabTitle.GetLength() > MAX_TAB_LENGTH)
		{
			tabTitle = tabTitle.Left(MAX_TAB_LENGTH - 3) + _T("...");
		}

		tci.pszText = (LPTSTR)(LPCTSTR)tabTitle;
		m_tabCtrl.InsertItem(newIndex, &tci);
		m_tabCtrl.SetCurSel(newIndex);

		// ★★★ 强制刷新标签页显示，避免显示异常
		m_tabCtrl.RedrawWindow(NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE | RDW_ALLCHILDREN);
	}

	// 切换到新文档（这会触发文档渲染）
	SwitchToDocument(newIndex);

	// 更新状态栏
	UpdateStatusBar();

	// ★★★ 添加到最近文件列表
	AddRecentFile(pParams->filePath);

	// ★★★ 强制刷新显示，确保PDF页面立即显示
	// 使用FALSE参数避免擦除背景，减少高刷新率显示器上的闪烁
	Invalidate(FALSE);
	UpdateWindow();

	// 清理
	delete pParams;
	m_pLoadParams = nullptr;
	ReleaseLoadThread();

	// ★★★ 关键：在所有UI更新完成后才关闭进度对话框
	// 这样用户看到进度条消失时，PDF页面已经显示出来了
	CloseProgressDialog();

	return 0;
}
