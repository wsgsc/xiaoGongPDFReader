#include "pch.h"
#include "framework.h"
#include "XiaoGongPDF.h"
#include "PDFScrollBar.h"
#include "XiaoGongPDFDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

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
	: m_pParentDlg(nullptr)
	, m_vertical(vertical)
	, m_contentLength(0)
	, m_pageLength(0)
	, m_position(0)
	, m_dragOffset(0)
	, m_dragging(false)
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
	dc.FillSolidRect(&rc, m_dark ? RGB(39, 43, 47) : RGB(250, 250, 250));
	if ((m_vertical ? rc.Height() : rc.Width()) < 32)
		return;
	const COLORREF grey = m_dark ? RGB(158, 169, 178) : RGB(133, 133, 133);
	CPen pen(PS_SOLID, 1, grey);
	CPen* pOldPen = dc.SelectObject(&pen);
	CBrush arrowBrush(grey);
	CBrush* pOldBrush = dc.SelectObject(&arrowBrush);
	if (m_vertical)
	{
		const int cx = rc.Width() / 2;
		POINT first[] = { { cx, 6 }, { cx - 5, 12 }, { cx + 5, 12 } };
		POINT last[] = { { cx - 5, rc.bottom - 12 }, { cx + 5, rc.bottom - 12 }, { cx, rc.bottom - 6 } };
		dc.Polygon(first, 3);
		dc.Polygon(last, 3);
	}
	else
	{
		const int cy = rc.Height() / 2;
		POINT first[] = { { 6, cy }, { 12, cy - 5 }, { 12, cy + 5 } };
		POINT last[] = { { rc.right - 12, cy - 5 }, { rc.right - 12, cy + 5 }, { rc.right - 6, cy } };
		dc.Polygon(first, 3);
		dc.Polygon(last, 3);
	}
	if (m_contentLength > m_pageLength)
	{
		CRect thumb = ThumbRect();
		dc.RoundRect(&thumb, CPoint(min(10, thumb.Width()), min(10, thumb.Height())));
	}
	dc.SelectObject(pOldBrush);
	dc.SelectObject(pOldPen);
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
		{
			ScrollTo(m_position + (coordinate < (m_vertical ? thumb.top : thumb.left) ?
				-m_pageLength : m_pageLength));
		}
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
	{
		ScrollTo(static_cast<int>(static_cast<double>(
			max(0, min(travel, coordinate - m_dragOffset - 16))) * maxScroll / travel));
	}
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
