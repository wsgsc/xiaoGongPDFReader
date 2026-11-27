// PDFSidebarPanel.cpp: 左侧导航侧边栏三合一面板实现
//

#include "pch.h"
#include "framework.h"
#include <algorithm>
#include <cmath>
#include "resource.h"
#include "PDFSidebarPanel.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{
	void FillRoundRectGdip(Gdiplus::Graphics& graphics, const CRect& rc, float radius,
		COLORREF fillCol, BYTE fillAlpha, COLORREF borderCol, bool drawBorder, float borderWidth = 1.0f)
	{
		if (rc.Width() <= 1 || rc.Height() <= 1)
			return;

		const float r = max(0.0f, min(radius, min(rc.Width(), rc.Height()) * 0.5f));
		const float d = r * 2.0f;
		Gdiplus::RectF rf(
			static_cast<Gdiplus::REAL>(rc.left) + 0.5f,
			static_cast<Gdiplus::REAL>(rc.top) + 0.5f,
			static_cast<Gdiplus::REAL>(rc.Width() - 1),
			static_cast<Gdiplus::REAL>(rc.Height() - 1));

		Gdiplus::SolidBrush brush(Gdiplus::Color(fillAlpha,
			GetRValue(fillCol), GetGValue(fillCol), GetBValue(fillCol)));

		if (d <= 0.5f)
		{
			if (fillAlpha > 0)
				graphics.FillRectangle(&brush, rf);
			if (drawBorder)
			{
				Gdiplus::Pen pen(Gdiplus::Color(255,
					GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)), borderWidth);
				graphics.DrawRectangle(&pen, rf);
			}
			return;
		}

		Gdiplus::GraphicsPath path;
		path.AddArc(rf.X, rf.Y, d, d, 180.0f, 90.0f);
		path.AddArc(rf.GetRight() - d, rf.Y, d, d, 270.0f, 90.0f);
		path.AddArc(rf.GetRight() - d, rf.GetBottom() - d, d, d, 0.0f, 90.0f);
		path.AddArc(rf.X, rf.GetBottom() - d, d, d, 90.0f, 90.0f);
		path.CloseFigure();

		if (fillAlpha > 0)
			graphics.FillPath(&brush, &path);
		if (drawBorder)
		{
			Gdiplus::Pen pen(Gdiplus::Color(255,
				GetRValue(borderCol), GetGValue(borderCol), GetBValue(borderCol)), borderWidth);
			graphics.DrawPath(&pen, &path);
		}
	}

	void DrawSlimScrollbar(Gdiplus::Graphics& graphics, const CRect& thumbRect, bool dark, bool dragging)
	{
		if (thumbRect.IsRectEmpty())
			return;
		const COLORREF thumbCol = dark ?
			(dragging ? RGB(136, 148, 162) : RGB(96, 105, 118)) :
			(dragging ? RGB(130, 144, 164) : RGB(178, 189, 204));
		FillRoundRectGdip(graphics, thumbRect, 3.0f, thumbCol, 230, 0, false);
	}
}

// ============================================================================
// CPDFSidebarHeaderWnd 实现
// ============================================================================

BEGIN_MESSAGE_MAP(CPDFSidebarHeaderWnd, CWnd)
	ON_WM_ERASEBKGND()
	ON_WM_PAINT()
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSELEAVE()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_SETCURSOR()
	ON_WM_CAPTURECHANGED()
END_MESSAGE_MAP()

CPDFSidebarHeaderWnd::CPDFSidebarHeaderWnd()
	: m_activeTab(SIDEBAR_TAB_OUTLINE)
	, m_hoverTab(-1)
	, m_pressedTab(-1)
	, m_annotCount(0)
	, m_dark(false)
	, m_mouseTracking(FALSE)
{
}

CPDFSidebarHeaderWnd::~CPDFSidebarHeaderWnd()
{
	if (m_normalFont.GetSafeHandle())
		m_normalFont.DeleteObject();
	if (m_boldFont.GetSafeHandle())
		m_boldFont.DeleteObject();
}

bool CPDFSidebarHeaderWnd::CreatePanel(CWnd* pParent, UINT nID)
{
	if (!pParent)
		return false;
	if (GetSafeHwnd())
		return true;

	CString wndClass = AfxRegisterWndClass(CS_DBLCLKS, ::LoadCursor(nullptr, IDC_ARROW));
	return CreateEx(0, wndClass, _T(""), WS_CHILD | WS_CLIPSIBLINGS,
		CRect(0, 0, 260, HEADER_HEIGHT), pParent, nID) != FALSE;
}

void CPDFSidebarHeaderWnd::ensureFonts()
{
	if (!m_normalFont.GetSafeHandle())
	{
		m_normalFont.CreateFont(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑"));
	}
	if (!m_boldFont.GetSafeHandle())
	{
		m_boldFont.CreateFont(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑"));
	}
}

void CPDFSidebarHeaderWnd::SetActiveTab(SidebarTabMode tab)
{
	if (tab < SIDEBAR_TAB_OUTLINE || tab > SIDEBAR_TAB_ANNOTATIONS)
		tab = SIDEBAR_TAB_OUTLINE;
	if (m_activeTab == tab)
		return;
	m_activeTab = tab;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CPDFSidebarHeaderWnd::SetAnnotationCount(int count)
{
	count = max(0, count);
	if (m_annotCount == count)
		return;
	m_annotCount = count;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CPDFSidebarHeaderWnd::SetDark(bool dark)
{
	if (m_dark == dark)
		return;
	m_dark = dark;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

CRect CPDFSidebarHeaderWnd::getSegmentRect(int index, const CRect& clientRect) const
{
	if (index < 0 || index >= 3 || clientRect.Width() <= 20)
		return CRect(0, 0, 0, 0);

	const int trackLeft = 7;
	const int trackRight = max(trackLeft + 9, clientRect.Width() - 8);
	const int trackWidth = trackRight - trackLeft;
	const int segW = trackWidth / 3;
	const int left = trackLeft + index * segW;
	const int right = (index == 2) ? trackRight : (left + segW);
	return CRect(left + 1, 5, right - 1, HEADER_HEIGHT - 5);
}

int CPDFSidebarHeaderWnd::hitTestTab(CPoint point) const
{
	CRect rc;
	GetClientRect(&rc);
	for (int i = 0; i < 3; ++i)
	{
		if (getSegmentRect(i, rc).PtInRect(point))
			return i;
	}
	return -1;
}

BOOL CPDFSidebarHeaderWnd::OnEraseBkgnd(CDC*)
{
	return TRUE;
}

void CPDFSidebarHeaderWnd::OnPaint()
{
	CPaintDC paintDC(this);
	CRect rc;
	GetClientRect(&rc);
	if (rc.IsRectEmpty())
		return;

	ensureFonts();

	CDC memDC;
	CBitmap memBmp;
	if (!memDC.CreateCompatibleDC(&paintDC) ||
		!memBmp.CreateCompatibleBitmap(&paintDC, rc.Width(), rc.Height()))
	{
		return;
	}
	CBitmap* pOldBmp = memDC.SelectObject(&memBmp);

	const COLORREF bgCol = m_dark ? RGB(39, 43, 47) : RGB(248, 250, 253);
	const COLORREF trackCol = m_dark ? RGB(30, 33, 37) : RGB(233, 239, 248);
	const COLORREF borderCol = m_dark ? RGB(58, 63, 70) : RGB(218, 227, 241);
	memDC.FillSolidRect(&rc, bgCol);

	Gdiplus::Graphics graphics(memDC.GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	CRect trackRect(6, 4, max(7, rc.Width() - 7), HEADER_HEIGHT - 4);
	FillRoundRectGdip(graphics, trackRect, 6.0f, trackCol, 255, 0, false);

	CString annotLabel(_T("批注"));
	if (m_annotCount > 0)
	{
		if (m_annotCount > 99)
			annotLabel = _T("批注 99+");
		else
			annotLabel.Format(_T("批注 %d"), m_annotCount);
	}
	const LPCTSTR labels[3] = { _T("目录"), _T("缩略图"), annotLabel.GetString() };

	memDC.SetBkMode(TRANSPARENT);
	CFont* pOldFont = memDC.SelectObject(&m_normalFont);

	for (int i = 0; i < 3; ++i)
	{
		CRect segRect = getSegmentRect(i, rc);
		const bool active = (static_cast<int>(m_activeTab) == i);
		const bool hovered = (m_hoverTab == i);
		const bool pressed = (m_pressedTab == i);

		if (active)
		{
			const COLORREF pillFill = m_dark ? RGB(54, 66, 82) : RGB(255, 255, 255);
			const COLORREF pillBorder = m_dark ? RGB(82, 108, 142) : RGB(204, 220, 244);
			FillRoundRectGdip(graphics, segRect, 5.0f, pillFill, 255, pillBorder, true);
		}
		else if (hovered || pressed)
		{
			const COLORREF hovFill = m_dark ?
				(pressed ? RGB(48, 54, 62) : RGB(42, 47, 54)) :
				(pressed ? RGB(220, 230, 245) : RGB(241, 246, 253));
			FillRoundRectGdip(graphics, segRect, 5.0f, hovFill, 220, 0, false);
		}

		memDC.SelectObject(active ? &m_boldFont : &m_normalFont);
		const COLORREF textCol = active ?
			(m_dark ? RGB(134, 188, 255) : RGB(25, 103, 210)) :
			(m_dark ? RGB(182, 190, 200) : RGB(82, 94, 110));
		memDC.SetTextColor(textCol);
		memDC.DrawText(labels[i], &segRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	memDC.SelectObject(pOldFont);

	// 底部与右侧 1px 分隔线
	memDC.FillSolidRect(0, rc.bottom - 1, rc.Width(), 1, borderCol);
	memDC.FillSolidRect(rc.right - 1, 0, 1, rc.Height(), borderCol);

	paintDC.BitBlt(0, 0, rc.Width(), rc.Height(), &memDC, 0, 0, SRCCOPY);
	memDC.SelectObject(pOldBmp);
}

void CPDFSidebarHeaderWnd::OnMouseMove(UINT nFlags, CPoint point)
{
	if (!m_mouseTracking)
	{
		TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, GetSafeHwnd(), 0 };
		if (::TrackMouseEvent(&tme))
			m_mouseTracking = TRUE;
	}

	const int hit = hitTestTab(point);
	if (hit != m_hoverTab)
	{
		m_hoverTab = hit;
		Invalidate(FALSE);
	}
	CWnd::OnMouseMove(nFlags, point);
}

void CPDFSidebarHeaderWnd::OnMouseLeave()
{
	m_mouseTracking = FALSE;
	if (m_hoverTab != -1)
	{
		m_hoverTab = -1;
		Invalidate(FALSE);
	}
	CWnd::OnMouseLeave();
}

void CPDFSidebarHeaderWnd::OnLButtonDown(UINT nFlags, CPoint point)
{
	const int hit = hitTestTab(point);
	if (hit >= 0)
	{
		m_pressedTab = hit;
		SetCapture();
		Invalidate(FALSE);
		return;
	}
	CWnd::OnLButtonDown(nFlags, point);
}

void CPDFSidebarHeaderWnd::OnLButtonUp(UINT nFlags, CPoint point)
{
	const int pressed = m_pressedTab;
	m_pressedTab = -1;
	if (::GetCapture() == GetSafeHwnd())
		::ReleaseCapture();

	if (pressed >= 0)
	{
		const int releaseHit = hitTestTab(point);
		Invalidate(FALSE);
		if (releaseHit == pressed)
		{
			const SidebarTabMode target = static_cast<SidebarTabMode>(releaseHit);
			SetActiveTab(target);
			if (m_onTabSelected)
				m_onTabSelected(target);
		}
		return;
	}
	CWnd::OnLButtonUp(nFlags, point);
}

BOOL CPDFSidebarHeaderWnd::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
	if (nHitTest == HTCLIENT)
	{
		CPoint pt;
		::GetCursorPos(&pt);
		ScreenToClient(&pt);
		::SetCursor(::LoadCursor(nullptr, hitTestTab(pt) >= 0 ? IDC_HAND : IDC_ARROW));
		return TRUE;
	}
	return CWnd::OnSetCursor(pWnd, nHitTest, message);
}

void CPDFSidebarHeaderWnd::OnCaptureChanged(CWnd* pWnd)
{
	if (pWnd != this && m_pressedTab != -1)
	{
		m_pressedTab = -1;
		Invalidate(FALSE);
	}
	CWnd::OnCaptureChanged(pWnd);
}

// ============================================================================
// CPDFThumbnailListWnd 实现
// ============================================================================

BEGIN_MESSAGE_MAP(CPDFThumbnailListWnd, CWnd)
	ON_WM_ERASEBKGND()
	ON_WM_PAINT()
	ON_WM_SIZE()
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSELEAVE()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_RBUTTONUP()
	ON_WM_MOUSEWHEEL()
	ON_WM_SETCURSOR()
	ON_WM_TIMER()
	ON_WM_CAPTURECHANGED()
	ON_WM_DESTROY()
END_MESSAGE_MAP()

CPDFThumbnailListWnd::CPDFThumbnailListWnd()
	: m_totalPages(0)
	, m_currentPage(0)
	, m_hoverPage(-1)
	, m_pressedPage(-1)
	, m_scrollY(0)
	, m_dark(false)
	, m_draggingScrollbar(false)
	, m_dragScrollOffset(0)
	, m_mouseTracking(FALSE)
{
}

CPDFThumbnailListWnd::~CPDFThumbnailListWnd()
{
	if (m_labelFont.GetSafeHandle())
		m_labelFont.DeleteObject();
	if (m_activeLabelFont.GetSafeHandle())
		m_activeLabelFont.DeleteObject();
}

bool CPDFThumbnailListWnd::CreatePanel(CWnd* pParent, UINT nID)
{
	if (!pParent)
		return false;
	if (GetSafeHwnd())
		return true;

	CString wndClass = AfxRegisterWndClass(CS_DBLCLKS, ::LoadCursor(nullptr, IDC_ARROW));
	return CreateEx(0, wndClass, _T(""), WS_CHILD | WS_CLIPSIBLINGS,
		CRect(0, 0, 260, 400), pParent, nID) != FALSE;
}

void CPDFThumbnailListWnd::OnDestroy()
{
	KillTimer(TIMER_ID_RENDER);
	CWnd::OnDestroy();
}

void CPDFThumbnailListWnd::ensureFonts()
{
	if (!m_labelFont.GetSafeHandle())
	{
		m_labelFont.CreateFont(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
	}
	if (!m_activeLabelFont.GetSafeHandle())
	{
		m_activeLabelFont.CreateFont(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
	}
}

int CPDFThumbnailListWnd::totalContentHeight() const
{
	if (m_totalPages <= 0)
		return 0;
	return TOP_PADDING * 2 + m_totalPages * ITEM_STRIDE_Y;
}

int CPDFThumbnailListWnd::maxScrollY(int viewHeight) const
{
	return max(0, totalContentHeight() - max(0, viewHeight));
}

void CPDFThumbnailListWnd::clampScroll()
{
	if (!GetSafeHwnd())
		return;
	CRect rc;
	GetClientRect(&rc);
	m_scrollY = max(0, min(m_scrollY, maxScrollY(rc.Height())));
}

void CPDFThumbnailListWnd::scrollToY(int newScrollY)
{
	if (!GetSafeHwnd())
		return;
	CRect rc;
	GetClientRect(&rc);
	newScrollY = max(0, min(newScrollY, maxScrollY(rc.Height())));
	if (newScrollY == m_scrollY)
		return;
	m_scrollY = newScrollY;
	Invalidate(FALSE);
	TriggerVisibleRender();
}

void CPDFThumbnailListWnd::SetDocumentPageCount(int totalPages, int currentPage)
{
	KillTimer(TIMER_ID_RENDER);
	m_totalPages = max(0, totalPages);
	m_currentPage = (m_totalPages > 0) ? max(0, min(currentPage, m_totalPages - 1)) : 0;
	m_hoverPage = -1;
	m_pressedPage = -1;
	m_scrollY = 0;
	if (GetSafeHwnd())
	{
		EnsurePageVisible(m_currentPage);
		Invalidate(FALSE);
		if (IsWindowVisible() && m_totalPages > 0)
			TriggerVisibleRender();
	}
}

void CPDFThumbnailListWnd::SetCurrentPage(int currentPage, bool ensureVisible)
{
	if (m_totalPages <= 0)
		return;
	currentPage = max(0, min(currentPage, m_totalPages - 1));
	const bool changed = (m_currentPage != currentPage);
	const int prevScrollY = m_scrollY;
	m_currentPage = currentPage;
	if (ensureVisible && GetSafeHwnd())
		EnsurePageVisible(m_currentPage);
	if (GetSafeHwnd() && (changed || m_scrollY != prevScrollY))
	{
		Invalidate(FALSE);
		if (m_scrollY != prevScrollY)
			TriggerVisibleRender();
	}
}

void CPDFThumbnailListWnd::EnsurePageVisible(int pageIndex)
{
	if (!GetSafeHwnd() || pageIndex < 0 || pageIndex >= m_totalPages)
		return;
	CRect rc;
	GetClientRect(&rc);
	if (rc.Height() <= 0)
		return;

	const int itemTop = TOP_PADDING + pageIndex * ITEM_STRIDE_Y;
	const int itemBottom = itemTop + ITEM_STRIDE_Y;
	if (itemTop < m_scrollY + 6)
		m_scrollY = max(0, itemTop - 6);
	else if (itemBottom > m_scrollY + rc.Height() - 6)
		m_scrollY = itemBottom - rc.Height() + 6;
	clampScroll();
}

void CPDFThumbnailListWnd::NotifyPageInvalidated(int /*pageIndex*/)
{
	if (!GetSafeHwnd())
		return;
	Invalidate(FALSE);
	if (IsWindowVisible() && m_totalPages > 0)
		TriggerVisibleRender();
}

void CPDFThumbnailListWnd::ClearThumbnails()
{
	if (GetSafeHwnd())
		KillTimer(TIMER_ID_RENDER);
	m_totalPages = 0;
	m_currentPage = 0;
	m_hoverPage = -1;
	m_pressedPage = -1;
	m_scrollY = 0;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CPDFThumbnailListWnd::SetDark(bool dark)
{
	if (m_dark == dark)
		return;
	m_dark = dark;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CPDFThumbnailListWnd::TriggerVisibleRender()
{
	if (GetSafeHwnd() && IsWindowVisible() && m_totalPages > 0)
		SetTimer(TIMER_ID_RENDER, 16, nullptr);
}

CRect CPDFThumbnailListWnd::getScrollbarThumbRect(const CRect& clientRect) const
{
	const int totalH = totalContentHeight();
	const int viewH = clientRect.Height();
	if (totalH <= viewH || viewH < 24)
		return CRect(0, 0, 0, 0);

	const int trackTop = 4;
	const int trackLen = max(12, viewH - 8);
	const int thumbLen = max(28, min(trackLen, static_cast<int>(
		static_cast<double>(trackLen) * viewH / totalH)));
	const int travel = max(0, trackLen - thumbLen);
	const int maxS = maxScrollY(viewH);
	const int thumbTop = trackTop + (maxS > 0 ?
		static_cast<int>(static_cast<double>(m_scrollY) * travel / maxS) : 0);

	return CRect(clientRect.right - SCROLLBAR_WIDTH + 1, thumbTop,
		clientRect.right - 3, thumbTop + thumbLen);
}

CRect CPDFThumbnailListWnd::getItemCardRect(int pageIndex, const CRect& clientRect) const
{
	const int availW = max(60, clientRect.Width() - SCROLLBAR_WIDTH - 2);
	const int cardW = min(availW - 16, MAX_THUMB_WIDTH + 28);
	const int left = (availW - cardW) / 2;
	const int top = TOP_PADDING + pageIndex * ITEM_STRIDE_Y - m_scrollY;
	return CRect(left, top, left + cardW, top + ITEM_STRIDE_Y - 6);
}

void CPDFThumbnailListWnd::getVisiblePageRange(const CRect& clientRect, int& outFirst, int& outLast) const
{
	if (m_totalPages <= 0 || clientRect.Height() <= 0)
	{
		outFirst = 0;
		outLast = -1;
		return;
	}
	outFirst = max(0, (m_scrollY - TOP_PADDING) / ITEM_STRIDE_Y);
	outLast = min(m_totalPages - 1, (m_scrollY + clientRect.Height() - TOP_PADDING) / ITEM_STRIDE_Y + 1);
}

int CPDFThumbnailListWnd::hitTestPageItem(CPoint point, const CRect& clientRect) const
{
	if (m_totalPages <= 0 || point.x >= clientRect.Width() - SCROLLBAR_WIDTH)
		return -1;
	int first = 0, last = -1;
	getVisiblePageRange(clientRect, first, last);
	for (int i = first; i <= last; ++i)
	{
		if (getItemCardRect(i, clientRect).PtInRect(point))
			return i;
	}
	return -1;
}

BOOL CPDFThumbnailListWnd::OnEraseBkgnd(CDC*)
{
	return TRUE;
}

void CPDFThumbnailListWnd::OnSize(UINT nType, int cx, int cy)
{
	CWnd::OnSize(nType, cx, cy);
	clampScroll();
	if (IsWindowVisible() && m_totalPages > 0)
		TriggerVisibleRender();
}

void CPDFThumbnailListWnd::OnPaint()
{
	CPaintDC paintDC(this);
	CRect rc;
	GetClientRect(&rc);
	if (rc.IsRectEmpty())
		return;

	ensureFonts();

	CDC memDC;
	CBitmap memBmp;
	if (!memDC.CreateCompatibleDC(&paintDC) ||
		!memBmp.CreateCompatibleBitmap(&paintDC, rc.Width(), rc.Height()))
	{
		return;
	}
	CBitmap* pOldBmp = memDC.SelectObject(&memBmp);

	const COLORREF bgCol = m_dark ? RGB(39, 43, 47) : RGB(248, 250, 253);
	const COLORREF dividerCol = m_dark ? RGB(58, 63, 70) : RGB(218, 227, 241);
	memDC.FillSolidRect(&rc, bgCol);

	Gdiplus::Graphics graphics(memDC.GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	bool hasUncachedVisible = false;
	int first = 0, last = -1;
	getVisiblePageRange(rc, first, last);

	memDC.SetBkMode(TRANSPARENT);
	CFont* pOldFont = memDC.SelectObject(&m_labelFont);

	for (int i = first; i <= last; ++i)
	{
		CRect cardRect = getItemCardRect(i, rc);
		if (cardRect.bottom < 0 || cardRect.top > rc.bottom)
			continue;

		const bool selected = (i == m_currentPage);
		const bool hovered = (i == m_hoverPage);

		if (selected)
		{
			const COLORREF selFill = m_dark ? RGB(47, 63, 84) : RGB(228, 239, 254);
			const COLORREF selBorder = m_dark ? RGB(94, 158, 249) : RGB(26, 115, 232);
			FillRoundRectGdip(graphics, cardRect, 7.0f, selFill, 255, selBorder, true, 1.6f);
		}
		else if (hovered)
		{
			const COLORREF hovFill = m_dark ? RGB(48, 53, 60) : RGB(237, 243, 252);
			FillRoundRectGdip(graphics, cardRect, 7.0f, hovFill, 255, 0, false);
		}

		HBITMAP hThumb = nullptr;
		CSize pagePx(108, 150);
		if (m_queryThumbnailFn)
		{
			m_queryThumbnailFn(i, MAX_THUMB_WIDTH, MAX_THUMB_HEIGHT, hThumb, pagePx, false);
		}
		if (!hThumb)
			hasUncachedVisible = true;

		const int boxAreaTop = cardRect.top + 8;
		const int boxAreaH = MAX_THUMB_HEIGHT;
		const int thumbW = max(16, min(MAX_THUMB_WIDTH, pagePx.cx));
		const int thumbH = max(16, min(MAX_THUMB_HEIGHT, pagePx.cy));
		const int drawX = cardRect.left + (cardRect.Width() - thumbW) / 2;
		const int drawY = boxAreaTop + (boxAreaH - thumbH) / 2;

		// 页面轻微阴影与纸张边框
		CRect shadowRect(drawX + 1, drawY + 2, drawX + thumbW + 1, drawY + thumbH + 2);
		memDC.FillSolidRect(&shadowRect, m_dark ? RGB(24, 27, 30) : RGB(222, 229, 239));

		if (hThumb)
		{
			CDC srcDC;
			if (srcDC.CreateCompatibleDC(&memDC))
			{
				HBITMAP hOldSrc = static_cast<HBITMAP>(srcDC.SelectObject(hThumb));
				BITMAP bm = {};
				if (::GetObject(hThumb, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight != 0)
				{
					const DWORD rop = m_dark ? NOTSRCCOPY : SRCCOPY;
					memDC.BitBlt(drawX, drawY, min(thumbW, (int)bm.bmWidth),
						min(thumbH, abs(bm.bmHeight)), &srcDC, 0, 0, rop);
				}
				srcDC.SelectObject(hOldSrc);
			}
		}
		else
		{
			// 尚未渲染完成的页面绘制同比例纸张骨架占位，保证快速拖拽零卡顿无抖动
			CRect paperRect(drawX, drawY, drawX + thumbW, drawY + thumbH);
			memDC.FillSolidRect(&paperRect, m_dark ? RGB(48, 52, 57) : RGB(255, 255, 255));
		}

		CRect pageBorder(drawX - 1, drawY - 1, drawX + thumbW + 1, drawY + thumbH + 1);
		CPen borderPen(PS_SOLID, 1, selected ?
			(m_dark ? RGB(94, 158, 249) : RGB(26, 115, 232)) :
			(m_dark ? RGB(72, 78, 86) : RGB(204, 214, 228)));
		CPen* pOldPen = memDC.SelectObject(&borderPen);
		HGDIOBJ pOldBrush = memDC.SelectStockObject(NULL_BRUSH);
		memDC.Rectangle(&pageBorder);
		memDC.SelectObject(pOldBrush);
		memDC.SelectObject(pOldPen);

		// 页码标签
		CString pageStr;
		pageStr.Format(_T("%d"), i + 1);
		CRect labelRect(cardRect.left + 4, cardRect.bottom - 22, cardRect.right - 4, cardRect.bottom - 3);
		memDC.SelectObject(selected ? &m_activeLabelFont : &m_labelFont);
		memDC.SetTextColor(selected ?
			(m_dark ? RGB(138, 190, 255) : RGB(24, 98, 206)) :
			(m_dark ? RGB(178, 186, 196) : RGB(90, 102, 118)));
		memDC.DrawText(pageStr, &labelRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
	}

	memDC.SelectObject(pOldFont);

	DrawSlimScrollbar(graphics, getScrollbarThumbRect(rc), m_dark, m_draggingScrollbar);
	memDC.FillSolidRect(rc.right - 1, 0, 1, rc.Height(), dividerCol);

	paintDC.BitBlt(0, 0, rc.Width(), rc.Height(), &memDC, 0, 0, SRCCOPY);
	memDC.SelectObject(pOldBmp);

	if (hasUncachedVisible)
		SetTimer(TIMER_ID_RENDER, 15, nullptr);
}

void CPDFThumbnailListWnd::OnTimer(UINT_PTR nIDEvent)
{
	if (nIDEvent != TIMER_ID_RENDER)
	{
		CWnd::OnTimer(nIDEvent);
		return;
	}

	if (!IsWindowVisible() || m_totalPages <= 0 || !m_queryThumbnailFn)
	{
		KillTimer(TIMER_ID_RENDER);
		return;
	}

	CRect rc;
	GetClientRect(&rc);
	int first = 0, last = -1;
	getVisiblePageRange(rc, first, last);
	if (last < first)
	{
		KillTimer(TIMER_ID_RENDER);
		return;
	}

	// 1. 优先渲染当前屏幕内缺失的缩略图（每次最多 2 张，确保 UI 响应丝滑）
	int renderedVisible = 0;
	bool moreVisibleMissing = false;
	for (int i = first; i <= last; ++i)
	{
		HBITMAP hBmp = nullptr;
		CSize sz(0, 0);
		m_queryThumbnailFn(i, MAX_THUMB_WIDTH, MAX_THUMB_HEIGHT, hBmp, sz, false);
		if (!hBmp)
		{
			if (renderedVisible < 2)
			{
				m_queryThumbnailFn(i, MAX_THUMB_WIDTH, MAX_THUMB_HEIGHT, hBmp, sz, true);
				++renderedVisible;
			}
			else
			{
				moreVisibleMissing = true;
				break;
			}
		}
	}

	if (renderedVisible > 0)
	{
		Invalidate(FALSE);
		if (!moreVisibleMissing)
			SetTimer(TIMER_ID_RENDER, 25, nullptr);
		return;
	}

	// 2. 可见区全部就绪后，后台静默预取前后邻近各 4 页
	const int preStart = max(0, first - 4);
	const int preEnd = min(m_totalPages - 1, last + 5);
	for (int i = preStart; i <= preEnd; ++i)
	{
		if (i >= first && i <= last)
			continue;
		HBITMAP hBmp = nullptr;
		CSize sz(0, 0);
		m_queryThumbnailFn(i, MAX_THUMB_WIDTH, MAX_THUMB_HEIGHT, hBmp, sz, false);
		if (!hBmp)
		{
			m_queryThumbnailFn(i, MAX_THUMB_WIDTH, MAX_THUMB_HEIGHT, hBmp, sz, true);
			SetTimer(TIMER_ID_RENDER, 30, nullptr);
			return;
		}
	}

	KillTimer(TIMER_ID_RENDER);
}

void CPDFThumbnailListWnd::OnMouseMove(UINT nFlags, CPoint point)
{
	if (!m_mouseTracking)
	{
		TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, GetSafeHwnd(), 0 };
		if (::TrackMouseEvent(&tme))
			m_mouseTracking = TRUE;
	}

	CRect rc;
	GetClientRect(&rc);

	if (m_draggingScrollbar)
	{
		if (!(nFlags & MK_LBUTTON))
		{
			m_draggingScrollbar = false;
			if (::GetCapture() == GetSafeHwnd())
				::ReleaseCapture();
			Invalidate(FALSE);
		}
		else
		{
			const CRect thumb = getScrollbarThumbRect(rc);
			const int trackLen = max(12, rc.Height() - 8);
			const int travel = max(1, trackLen - thumb.Height());
			const int maxS = maxScrollY(rc.Height());
			const int targetPos = max(0, min(travel, point.y - m_dragScrollOffset - 4));
			scrollToY(static_cast<int>(static_cast<double>(targetPos) * maxS / travel));
			return;
		}
	}

	const int hit = hitTestPageItem(point, rc);
	if (hit != m_hoverPage)
	{
		m_hoverPage = hit;
		Invalidate(FALSE);
	}
	CWnd::OnMouseMove(nFlags, point);
}

void CPDFThumbnailListWnd::OnMouseLeave()
{
	m_mouseTracking = FALSE;
	if (m_hoverPage != -1)
	{
		m_hoverPage = -1;
		Invalidate(FALSE);
	}
	CWnd::OnMouseLeave();
}

void CPDFThumbnailListWnd::OnLButtonDown(UINT nFlags, CPoint point)
{
	CRect rc;
	GetClientRect(&rc);

	if (point.x >= rc.Width() - SCROLLBAR_WIDTH - 2 && totalContentHeight() > rc.Height())
	{
		const CRect thumb = getScrollbarThumbRect(rc);
		if (thumb.PtInRect(point))
		{
			m_draggingScrollbar = true;
			m_dragScrollOffset = point.y - thumb.top;
			SetCapture();
			Invalidate(FALSE);
		}
		else
		{
			scrollToY(m_scrollY + (point.y < thumb.top ? -rc.Height() : rc.Height()));
		}
		return;
	}

	const int hit = hitTestPageItem(point, rc);
	if (hit >= 0)
	{
		m_pressedPage = hit;
		SetCapture();
		return;
	}
	CWnd::OnLButtonDown(nFlags, point);
}

void CPDFThumbnailListWnd::OnLButtonUp(UINT nFlags, CPoint point)
{
	if (m_draggingScrollbar)
	{
		m_draggingScrollbar = false;
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();
		Invalidate(FALSE);
		return;
	}

	const int pressed = m_pressedPage;
	m_pressedPage = -1;
	if (::GetCapture() == GetSafeHwnd())
		::ReleaseCapture();

	if (pressed >= 0)
	{
		CRect rc;
		GetClientRect(&rc);
		if (hitTestPageItem(point, rc) == pressed)
		{
			m_currentPage = pressed;
			Invalidate(FALSE);
			if (m_onPageSelected)
				m_onPageSelected(pressed);
		}
		return;
	}
	CWnd::OnLButtonUp(nFlags, point);
}

void CPDFThumbnailListWnd::OnRButtonUp(UINT /*nFlags*/, CPoint point)
{
	CRect rc;
	GetClientRect(&rc);
	const int hit = hitTestPageItem(point, rc);
	if (hit >= 0 && m_onPageContextMenu)
	{
		CPoint screenPt = point;
		ClientToScreen(&screenPt);
		m_onPageContextMenu(hit, screenPt);
	}
}

BOOL CPDFThumbnailListWnd::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
	CRect winRect;
	GetWindowRect(&winRect);
	if (!winRect.PtInRect(pt))
	{
		if (CWnd* pParent = GetParent())
			return static_cast<BOOL>(pParent->SendMessage(WM_MOUSEWHEEL,
				MAKEWPARAM(nFlags, zDelta), MAKELPARAM(pt.x, pt.y)));
		return FALSE;
	}

	if (zDelta != 0 && m_totalPages > 0)
	{
		const int deltaPx = -(zDelta * 120) / WHEEL_DELTA;
		scrollToY(m_scrollY + deltaPx);
	}
	return TRUE;
}

BOOL CPDFThumbnailListWnd::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
	if (nHitTest == HTCLIENT)
	{
		CPoint pt;
		::GetCursorPos(&pt);
		ScreenToClient(&pt);
		CRect rc;
		GetClientRect(&rc);
		::SetCursor(::LoadCursor(nullptr, hitTestPageItem(pt, rc) >= 0 ? IDC_HAND : IDC_ARROW));
		return TRUE;
	}
	return CWnd::OnSetCursor(pWnd, nHitTest, message);
}

void CPDFThumbnailListWnd::OnCaptureChanged(CWnd* pWnd)
{
	if (pWnd != this)
	{
		m_draggingScrollbar = false;
		m_pressedPage = -1;
		Invalidate(FALSE);
	}
	CWnd::OnCaptureChanged(pWnd);
}

// ============================================================================
// CPDFAnnotationListWnd 实现
// ============================================================================

BEGIN_MESSAGE_MAP(CPDFAnnotationListWnd, CWnd)
	ON_WM_ERASEBKGND()
	ON_WM_PAINT()
	ON_WM_SIZE()
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSELEAVE()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_LBUTTONDBLCLK()
	ON_WM_RBUTTONUP()
	ON_WM_MOUSEWHEEL()
	ON_WM_SETCURSOR()
	ON_WM_CAPTURECHANGED()
END_MESSAGE_MAP()

CPDFAnnotationListWnd::CPDFAnnotationListWnd()
	: m_selectedIndex(-1)
	, m_hoverIndex(-1)
	, m_hoverDeleteBtn(false)
	, m_pressedIndex(-1)
	, m_pressedDeleteBtn(false)
	, m_scrollY(0)
	, m_dark(false)
	, m_draggingScrollbar(false)
	, m_dragScrollOffset(0)
	, m_mouseTracking(FALSE)
{
}

CPDFAnnotationListWnd::~CPDFAnnotationListWnd()
{
	if (m_titleFont.GetSafeHandle())
		m_titleFont.DeleteObject();
	if (m_bodyFont.GetSafeHandle())
		m_bodyFont.DeleteObject();
	if (m_pagePillFont.GetSafeHandle())
		m_pagePillFont.DeleteObject();
}

bool CPDFAnnotationListWnd::CreatePanel(CWnd* pParent, UINT nID)
{
	if (!pParent)
		return false;
	if (GetSafeHwnd())
		return true;

	CString wndClass = AfxRegisterWndClass(CS_DBLCLKS, ::LoadCursor(nullptr, IDC_ARROW));
	return CreateEx(0, wndClass, _T(""), WS_CHILD | WS_CLIPSIBLINGS,
		CRect(0, 0, 260, 400), pParent, nID) != FALSE;
}

void CPDFAnnotationListWnd::ensureFonts()
{
	if (!m_titleFont.GetSafeHandle())
	{
		m_titleFont.CreateFont(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑"));
	}
	if (!m_bodyFont.GetSafeHandle())
	{
		m_bodyFont.CreateFont(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑"));
	}
	if (!m_pagePillFont.GetSafeHandle())
	{
		m_pagePillFont.CreateFont(-11, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑"));
	}
}

void CPDFAnnotationListWnd::SetAnnotations(std::vector<DocumentAnnotationItem> items)
{
	m_items = std::move(items);
	if (m_selectedIndex >= static_cast<int>(m_items.size()))
		m_selectedIndex = static_cast<int>(m_items.size()) - 1;
	m_hoverIndex = -1;
	m_hoverDeleteBtn = false;
	m_pressedIndex = -1;
	m_pressedDeleteBtn = false;
	clampScroll();
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CPDFAnnotationListWnd::ClearAnnotations()
{
	m_items.clear();
	m_selectedIndex = -1;
	m_hoverIndex = -1;
	m_hoverDeleteBtn = false;
	m_pressedIndex = -1;
	m_pressedDeleteBtn = false;
	m_scrollY = 0;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CPDFAnnotationListWnd::SetDark(bool dark)
{
	if (m_dark == dark)
		return;
	m_dark = dark;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

int CPDFAnnotationListWnd::totalContentHeight() const
{
	if (m_items.empty())
		return 0;
	return TOP_PADDING * 2 + static_cast<int>(m_items.size()) * ITEM_STRIDE_Y;
}

int CPDFAnnotationListWnd::maxScrollY(int viewHeight) const
{
	return max(0, totalContentHeight() - max(0, viewHeight));
}

void CPDFAnnotationListWnd::clampScroll()
{
	if (!GetSafeHwnd())
		return;
	CRect rc;
	GetClientRect(&rc);
	m_scrollY = max(0, min(m_scrollY, maxScrollY(rc.Height())));
}

void CPDFAnnotationListWnd::scrollToY(int newScrollY)
{
	if (!GetSafeHwnd())
		return;
	CRect rc;
	GetClientRect(&rc);
	newScrollY = max(0, min(newScrollY, maxScrollY(rc.Height())));
	if (newScrollY == m_scrollY)
		return;
	m_scrollY = newScrollY;
	Invalidate(FALSE);
}

CRect CPDFAnnotationListWnd::getScrollbarThumbRect(const CRect& clientRect) const
{
	const int totalH = totalContentHeight();
	const int viewH = clientRect.Height();
	if (totalH <= viewH || viewH < 24)
		return CRect(0, 0, 0, 0);

	const int trackTop = 4;
	const int trackLen = max(12, viewH - 8);
	const int thumbLen = max(28, min(trackLen, static_cast<int>(
		static_cast<double>(trackLen) * viewH / totalH)));
	const int travel = max(0, trackLen - thumbLen);
	const int maxS = maxScrollY(viewH);
	const int thumbTop = trackTop + (maxS > 0 ?
		static_cast<int>(static_cast<double>(m_scrollY) * travel / maxS) : 0);

	return CRect(clientRect.right - SCROLLBAR_WIDTH + 1, thumbTop,
		clientRect.right - 3, thumbTop + thumbLen);
}

CRect CPDFAnnotationListWnd::getItemCardRect(int index, const CRect& clientRect) const
{
	const int rightEdge = max(40, clientRect.Width() - SCROLLBAR_WIDTH - 4);
	const int top = TOP_PADDING + index * ITEM_STRIDE_Y - m_scrollY;
	return CRect(8, top, rightEdge, top + ITEM_STRIDE_Y - 6);
}

CRect CPDFAnnotationListWnd::getDeleteButtonRect(const CRect& cardRect) const
{
	return CRect(cardRect.right - 24, cardRect.top + 6, cardRect.right - 6, cardRect.top + 24);
}

int CPDFAnnotationListWnd::hitTestItem(CPoint point, const CRect& clientRect, bool& outOnDeleteBtn) const
{
	outOnDeleteBtn = false;
	if (m_items.empty() || point.x >= clientRect.Width() - SCROLLBAR_WIDTH)
		return -1;

	const int first = max(0, (m_scrollY - TOP_PADDING) / ITEM_STRIDE_Y);
	const int last = min(static_cast<int>(m_items.size()) - 1,
		(m_scrollY + clientRect.Height() - TOP_PADDING) / ITEM_STRIDE_Y + 1);
	for (int i = first; i <= last; ++i)
	{
		const CRect cardRect = getItemCardRect(i, clientRect);
		if (cardRect.PtInRect(point))
		{
			outOnDeleteBtn = getDeleteButtonRect(cardRect).PtInRect(point) != FALSE;
			return i;
		}
	}
	return -1;
}

CString CPDFAnnotationListWnd::buildItemDisplayText(const DocumentAnnotationItem& item) const
{
	CString notes = item.contents;
	notes.Replace(_T("\r\n"), _T(" "));
	notes.Replace(_T("\n"), _T(" "));
	notes.Replace(_T("\r"), _T(" "));

	CString quote = item.quotedText;
	quote.Replace(_T("\r\n"), _T(" "));
	quote.Replace(_T("\n"), _T(" "));
	quote.Replace(_T("\r"), _T(" "));

	if (!notes.IsEmpty() && !quote.IsEmpty())
		return notes + _T("  |  “") + quote + _T("”");
	if (!notes.IsEmpty())
		return notes;
	if (!quote.IsEmpty())
		return _T("“") + quote + _T("”");
	return _T("点击定位到该批注位置");
}

BOOL CPDFAnnotationListWnd::OnEraseBkgnd(CDC*)
{
	return TRUE;
}

void CPDFAnnotationListWnd::OnSize(UINT nType, int cx, int cy)
{
	CWnd::OnSize(nType, cx, cy);
	clampScroll();
}

void CPDFAnnotationListWnd::OnPaint()
{
	CPaintDC paintDC(this);
	CRect rc;
	GetClientRect(&rc);
	if (rc.IsRectEmpty())
		return;

	ensureFonts();

	CDC memDC;
	CBitmap memBmp;
	if (!memDC.CreateCompatibleDC(&paintDC) ||
		!memBmp.CreateCompatibleBitmap(&paintDC, rc.Width(), rc.Height()))
	{
		return;
	}
	CBitmap* pOldBmp = memDC.SelectObject(&memBmp);

	const COLORREF bgCol = m_dark ? RGB(39, 43, 47) : RGB(248, 250, 253);
	const COLORREF dividerCol = m_dark ? RGB(58, 63, 70) : RGB(218, 227, 241);
	memDC.FillSolidRect(&rc, bgCol);
	memDC.SetBkMode(TRANSPARENT);

	Gdiplus::Graphics graphics(memDC.GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	CFont* pOldFont = memDC.SelectObject(&m_titleFont);

	if (m_items.empty())
	{
		const int midY = max(40, rc.Height() / 2 - 36);
		CRect emptyTitleRect(16, midY, rc.Width() - 16, midY + 24);
		memDC.SelectObject(&m_titleFont);
		memDC.SetTextColor(m_dark ? RGB(178, 186, 196) : RGB(84, 96, 112));
		memDC.DrawText(_T("当前文档暂无批注"), &emptyTitleRect,
			DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

		CRect emptyHintRect(18, midY + 28, rc.Width() - 18, midY + 84);
		memDC.SelectObject(&m_bodyFont);
		memDC.SetTextColor(m_dark ? RGB(130, 139, 150) : RGB(128, 140, 156));
		memDC.DrawText(_T("选中页面文字可添加高亮 / 下划线，\n或按 Ctrl+T 添加文字批注"),
			&emptyHintRect, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
	}
	else
	{
		const int first = max(0, (m_scrollY - TOP_PADDING) / ITEM_STRIDE_Y);
		const int last = min(static_cast<int>(m_items.size()) - 1,
			(m_scrollY + rc.Height() - TOP_PADDING) / ITEM_STRIDE_Y + 1);

		for (int i = first; i <= last; ++i)
		{
			const DocumentAnnotationItem& item = m_items[i];
			CRect cardRect = getItemCardRect(i, rc);
			if (cardRect.bottom < 0 || cardRect.top > rc.bottom)
				continue;

			const bool selected = (i == m_selectedIndex);
			const bool hovered = (i == m_hoverIndex);

			COLORREF cardFill = m_dark ? RGB(45, 50, 56) : RGB(255, 255, 255);
			COLORREF cardBorder = m_dark ? RGB(62, 68, 76) : RGB(224, 232, 244);
			if (selected)
			{
				cardFill = m_dark ? RGB(48, 64, 86) : RGB(234, 243, 255);
				cardBorder = m_dark ? RGB(94, 158, 249) : RGB(26, 115, 232);
			}
			else if (hovered)
			{
				cardFill = m_dark ? RGB(52, 58, 66) : RGB(242, 247, 254);
				cardBorder = m_dark ? RGB(78, 86, 98) : RGB(196, 214, 240);
			}
			FillRoundRectGdip(graphics, cardRect, 6.0f, cardFill, 255, cardBorder, true);

			// 左侧批注颜色标识条
			CRect colorBar(cardRect.left + 4, cardRect.top + 7, cardRect.left + 8, cardRect.bottom - 7);
			FillRoundRectGdip(graphics, colorBar, 2.0f, item.color, 255, 0, false);

			int rightLimit = cardRect.right - 8;
			if (hovered)
			{
				CRect delRect = getDeleteButtonRect(cardRect);
				if (m_hoverDeleteBtn)
					FillRoundRectGdip(graphics, delRect, 4.0f, RGB(232, 38, 51), 255, 0, false);
				const COLORREF xCol = m_hoverDeleteBtn ? RGB(255, 255, 255) :
					(m_dark ? RGB(175, 183, 194) : RGB(120, 132, 146));
				Gdiplus::Pen xPen(Gdiplus::Color(255, GetRValue(xCol), GetGValue(xCol), GetBValue(xCol)), 1.5f);
				graphics.DrawLine(&xPen,
					static_cast<Gdiplus::REAL>(delRect.left + 5),
					static_cast<Gdiplus::REAL>(delRect.top + 5),
					static_cast<Gdiplus::REAL>(delRect.right - 5),
					static_cast<Gdiplus::REAL>(delRect.bottom - 5));
				graphics.DrawLine(&xPen,
					static_cast<Gdiplus::REAL>(delRect.right - 5),
					static_cast<Gdiplus::REAL>(delRect.top + 5),
					static_cast<Gdiplus::REAL>(delRect.left + 5),
					static_cast<Gdiplus::REAL>(delRect.bottom - 5));
				rightLimit = delRect.left - 5;
			}

			// 页码胶囊
			CString pageBadge;
			pageBadge.Format(_T("第 %d 页"), item.pageNumber + 1);
			memDC.SelectObject(&m_pagePillFont);
			const int pillW = max(44, memDC.GetTextExtent(pageBadge).cx + 12);
			CRect pillRect(rightLimit - pillW, cardRect.top + 6, rightLimit, cardRect.top + 24);
			FillRoundRectGdip(graphics, pillRect, 9.0f,
				m_dark ? RGB(38, 52, 70) : RGB(235, 243, 254), 255, 0, false);
			memDC.SetTextColor(m_dark ? RGB(134, 186, 255) : RGB(26, 115, 232));
			memDC.DrawText(pageBadge, &pillRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			rightLimit = pillRect.left - 6;

			// 批注类型标题
			CRect typeRect(cardRect.left + 14, cardRect.top + 5, max(cardRect.left + 20, rightLimit), cardRect.top + 25);
			memDC.SelectObject(&m_titleFont);
			memDC.SetTextColor(m_dark ? RGB(226, 232, 238) : RGB(32, 40, 52));
			memDC.DrawText(item.typeLabel, &typeRect,
				DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

			// 批注正文或被标记文本摘要（最多两行自动省略）
			CString bodyText = buildItemDisplayText(item);
			CRect bodyRect(cardRect.left + 14, cardRect.top + 27, cardRect.right - 8, cardRect.bottom - 5);
			memDC.SelectObject(&m_bodyFont);
			const bool placeholderOnly = item.contents.IsEmpty() && item.quotedText.IsEmpty();
			memDC.SetTextColor(placeholderOnly ?
				(m_dark ? RGB(126, 136, 148) : RGB(144, 154, 168)) :
				(m_dark ? RGB(178, 186, 196) : RGB(82, 94, 110)));
			memDC.DrawText(bodyText, &bodyRect,
				DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
		}
	}

	memDC.SelectObject(pOldFont);

	DrawSlimScrollbar(graphics, getScrollbarThumbRect(rc), m_dark, m_draggingScrollbar);
	memDC.FillSolidRect(rc.right - 1, 0, 1, rc.Height(), dividerCol);

	paintDC.BitBlt(0, 0, rc.Width(), rc.Height(), &memDC, 0, 0, SRCCOPY);
	memDC.SelectObject(pOldBmp);
}

void CPDFAnnotationListWnd::OnMouseMove(UINT nFlags, CPoint point)
{
	if (!m_mouseTracking)
	{
		TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, GetSafeHwnd(), 0 };
		if (::TrackMouseEvent(&tme))
			m_mouseTracking = TRUE;
	}

	CRect rc;
	GetClientRect(&rc);

	if (m_draggingScrollbar)
	{
		if (!(nFlags & MK_LBUTTON))
		{
			m_draggingScrollbar = false;
			if (::GetCapture() == GetSafeHwnd())
				::ReleaseCapture();
			Invalidate(FALSE);
		}
		else
		{
			const CRect thumb = getScrollbarThumbRect(rc);
			const int trackLen = max(12, rc.Height() - 8);
			const int travel = max(1, trackLen - thumb.Height());
			const int maxS = maxScrollY(rc.Height());
			const int targetPos = max(0, min(travel, point.y - m_dragScrollOffset - 4));
			scrollToY(static_cast<int>(static_cast<double>(targetPos) * maxS / travel));
			return;
		}
	}

	bool onDel = false;
	const int hit = hitTestItem(point, rc, onDel);
	if (hit != m_hoverIndex || onDel != m_hoverDeleteBtn)
	{
		m_hoverIndex = hit;
		m_hoverDeleteBtn = onDel;
		Invalidate(FALSE);
	}
	CWnd::OnMouseMove(nFlags, point);
}

void CPDFAnnotationListWnd::OnMouseLeave()
{
	m_mouseTracking = FALSE;
	if (m_hoverIndex != -1 || m_hoverDeleteBtn)
	{
		m_hoverIndex = -1;
		m_hoverDeleteBtn = false;
		Invalidate(FALSE);
	}
	CWnd::OnMouseLeave();
}

void CPDFAnnotationListWnd::OnLButtonDown(UINT nFlags, CPoint point)
{
	CRect rc;
	GetClientRect(&rc);

	if (point.x >= rc.Width() - SCROLLBAR_WIDTH - 2 && totalContentHeight() > rc.Height())
	{
		const CRect thumb = getScrollbarThumbRect(rc);
		if (thumb.PtInRect(point))
		{
			m_draggingScrollbar = true;
			m_dragScrollOffset = point.y - thumb.top;
			SetCapture();
			Invalidate(FALSE);
		}
		else
		{
			scrollToY(m_scrollY + (point.y < thumb.top ? -rc.Height() : rc.Height()));
		}
		return;
	}

	bool onDel = false;
	const int hit = hitTestItem(point, rc, onDel);
	if (hit >= 0)
	{
		m_pressedIndex = hit;
		m_pressedDeleteBtn = onDel;
		SetCapture();
		return;
	}
	CWnd::OnLButtonDown(nFlags, point);
}

void CPDFAnnotationListWnd::OnLButtonUp(UINT nFlags, CPoint point)
{
	if (m_draggingScrollbar)
	{
		m_draggingScrollbar = false;
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();
		Invalidate(FALSE);
		return;
	}

	const int pressed = m_pressedIndex;
	const bool pressedDel = m_pressedDeleteBtn;
	m_pressedIndex = -1;
	m_pressedDeleteBtn = false;
	if (::GetCapture() == GetSafeHwnd())
		::ReleaseCapture();

	if (pressed >= 0 && pressed < static_cast<int>(m_items.size()))
	{
		CRect rc;
		GetClientRect(&rc);
		bool releaseDel = false;
		const int releaseHit = hitTestItem(point, rc, releaseDel);
		if (releaseHit == pressed)
		{
			const DocumentAnnotationItem itemCopy = m_items[pressed];
			if (pressedDel && releaseDel)
			{
				if (m_onItemDelete)
					m_onItemDelete(itemCopy);
			}
			else if (!pressedDel)
			{
				m_selectedIndex = pressed;
				Invalidate(FALSE);
				if (m_onItemClicked)
					m_onItemClicked(itemCopy);
			}
		}
		return;
	}
	CWnd::OnLButtonUp(nFlags, point);
}

void CPDFAnnotationListWnd::OnLButtonDblClk(UINT nFlags, CPoint point)
{
	CRect rc;
	GetClientRect(&rc);
	bool onDel = false;
	const int hit = hitTestItem(point, rc, onDel);
	if (hit >= 0 && hit < static_cast<int>(m_items.size()) && !onDel)
	{
		m_selectedIndex = hit;
		Invalidate(FALSE);
		const DocumentAnnotationItem itemCopy = m_items[hit];
		if (m_onItemDoubleClicked)
			m_onItemDoubleClicked(itemCopy);
		return;
	}
	CWnd::OnLButtonDblClk(nFlags, point);
}

void CPDFAnnotationListWnd::OnRButtonUp(UINT /*nFlags*/, CPoint point)
{
	CRect rc;
	GetClientRect(&rc);
	bool onDel = false;
	const int hit = hitTestItem(point, rc, onDel);
	if (hit >= 0 && hit < static_cast<int>(m_items.size()) && m_onItemContextMenu)
	{
		m_selectedIndex = hit;
		Invalidate(FALSE);
		const DocumentAnnotationItem itemCopy = m_items[hit];
		CPoint screenPt = point;
		ClientToScreen(&screenPt);
		m_onItemContextMenu(itemCopy, screenPt);
	}
}

BOOL CPDFAnnotationListWnd::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
	CRect winRect;
	GetWindowRect(&winRect);
	if (!winRect.PtInRect(pt))
	{
		if (CWnd* pParent = GetParent())
			return static_cast<BOOL>(pParent->SendMessage(WM_MOUSEWHEEL,
				MAKEWPARAM(nFlags, zDelta), MAKELPARAM(pt.x, pt.y)));
		return FALSE;
	}

	if (zDelta != 0 && !m_items.empty())
	{
		const int deltaPx = -(zDelta * 108) / WHEEL_DELTA;
		scrollToY(m_scrollY + deltaPx);
	}
	return TRUE;
}

BOOL CPDFAnnotationListWnd::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
	if (nHitTest == HTCLIENT)
	{
		CPoint pt;
		::GetCursorPos(&pt);
		ScreenToClient(&pt);
		CRect rc;
		GetClientRect(&rc);
		bool onDel = false;
		::SetCursor(::LoadCursor(nullptr, hitTestItem(pt, rc, onDel) >= 0 ? IDC_HAND : IDC_ARROW));
		return TRUE;
	}
	return CWnd::OnSetCursor(pWnd, nHitTest, message);
}

void CPDFAnnotationListWnd::OnCaptureChanged(CWnd* pWnd)
{
	if (pWnd != this)
	{
		m_draggingScrollbar = false;
		m_pressedIndex = -1;
		m_pressedDeleteBtn = false;
		Invalidate(FALSE);
	}
	CWnd::OnCaptureChanged(pWnd);
}
