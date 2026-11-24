#include "pch.h"
#include "framework.h"
#include <shlwapi.h>
#include "XiaoGongPDF.h"
#include "PDFViewCtrl.h"
#include "XiaoGongPDFDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CPDFViewCtrl, CStatic)
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_MBUTTONDOWN()
	ON_WM_MBUTTONUP()
	ON_WM_MOUSEMOVE()
	ON_WM_RBUTTONUP()
	ON_WM_ERASEBKGND()
	ON_WM_VSCROLL()
	ON_WM_HSCROLL()
	ON_WM_PAINT()
END_MESSAGE_MAP()

CPDFViewCtrl::~CPDFViewCtrl()
{
	delete m_pWelcomeBitmap;
	if (m_pWelcomeStream)
		m_pWelcomeStream->Release();
}

void CPDFViewCtrl::DrawWelcomeImage(CDC& dc, const CRect& clientRect)
{
	if (!m_pWelcomeBitmap)
	{
		HINSTANCE hInstance = AfxGetInstanceHandle();
		HRSRC hResource = ::FindResource(hInstance, MAKEINTRESOURCE(IDR_WELCOME_PNG), RT_RCDATA);
		if (!hResource)
			return;
		HGLOBAL hData = ::LoadResource(hInstance, hResource);
		DWORD dataSize = ::SizeofResource(hInstance, hResource);
		const BYTE* pData = hData ? static_cast<const BYTE*>(::LockResource(hData)) : nullptr;
		if (!pData || dataSize == 0)
			return;
		IStream* pStream = ::SHCreateMemStream(pData, dataSize);
		if (!pStream)
			return;
		Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream, FALSE);
		if (!pBitmap || pBitmap->GetLastStatus() != Gdiplus::Ok ||
			pBitmap->GetWidth() == 0 || pBitmap->GetHeight() == 0)
		{
			delete pBitmap;
			pStream->Release();
			return;
		}
		m_pWelcomeBitmap = pBitmap;
		m_pWelcomeStream = pStream;
	}

	const int imageWidth = static_cast<int>(m_pWelcomeBitmap->GetWidth());
	const int imageHeight = static_cast<int>(m_pWelcomeBitmap->GetHeight());
	const double scale = 0.75 * min(1.0, min(clientRect.Width() * 0.9 / imageWidth,
		clientRect.Height() * 0.9 / imageHeight));
	const int drawWidth = static_cast<int>(imageWidth * scale);
	const int drawHeight = static_cast<int>(imageHeight * scale);
	if (drawWidth <= 0 || drawHeight <= 0)
		return;

	Gdiplus::Graphics graphics(dc.GetSafeHdc());
	graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
	graphics.DrawImage(m_pWelcomeBitmap,
		Gdiplus::Rect((clientRect.Width() - drawWidth) / 2,
			(clientRect.Height() - drawHeight) / 2, drawWidth, drawHeight));
}

void CPDFViewCtrl::SetDisplayBitmap(HBITMAP hBitmap)
{
	m_hDisplayBitmap = hBitmap;
	HWND hwnd = GetSafeHwnd();
	if (hwnd && ::IsWindow(hwnd))
	{
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

void CPDFViewCtrl::OnMButtonDown(UINT nFlags, CPoint point)
{
	ForwardMouseMessage(WM_MBUTTONDOWN, nFlags, point);
}

void CPDFViewCtrl::OnMButtonUp(UINT nFlags, CPoint point)
{
	ForwardMouseMessage(WM_MBUTTONUP, nFlags, point);
}

void CPDFViewCtrl::OnMouseMove(UINT nFlags, CPoint point)
{
	ForwardMouseMessage(WM_MOUSEMOVE, nFlags, point);
}

void CPDFViewCtrl::OnRButtonUp(UINT, CPoint point)
{
	if (!m_pParentDlg || !m_pParentDlg->GetSafeHwnd())
		return;
	ClientToScreen(&point);
	m_pParentDlg->SendMessage(WM_CONTEXTMENU,
		reinterpret_cast<WPARAM>(GetSafeHwnd()), MAKELPARAM(point.x, point.y));
}

BOOL CPDFViewCtrl::OnEraseBkgnd(CDC*)
{
	return TRUE;
}

void CPDFViewCtrl::OnVScroll(UINT nSBCode, UINT nPos, CScrollBar*)
{
	if (m_pParentDlg)
	{
		m_pParentDlg->SendMessage(WM_VSCROLL, MAKEWPARAM(nSBCode, nPos), (LPARAM)GetSafeHwnd());
	}
}

void CPDFViewCtrl::OnHScroll(UINT nSBCode, UINT nPos, CScrollBar*)
{
	if (m_pParentDlg)
	{
		m_pParentDlg->SendMessage(WM_HSCROLL, MAKEWPARAM(nSBCode, nPos), (LPARAM)GetSafeHwnd());
	}
}

void CPDFViewCtrl::OnPaint()
{
	CPaintDC dc(this);

	CRect clientRect;
	GetClientRect(&clientRect);
	if (clientRect.IsRectEmpty())
		return;

	HBITMAP hBitmap = m_hDisplayBitmap;
	if (!hBitmap)
	{
		CDC backDC;
		backDC.CreateCompatibleDC(&dc);
		CBitmap backBmp;
		backBmp.CreateCompatibleBitmap(&dc, clientRect.Width(), clientRect.Height());
		CBitmap* pOldBackBmp = backDC.SelectObject(&backBmp);
		backDC.FillSolidRect(&clientRect, m_readingMode == READING_NIGHT ?
			DARK_VIEW_BACKGROUND : PDF_VIEW_BACKGROUND);
		if (m_pParentDlg && m_pParentDlg->m_documents.empty())
			DrawWelcomeImage(backDC, clientRect);
		dc.BitBlt(0, 0, clientRect.Width(), clientRect.Height(), &backDC, 0, 0, SRCCOPY);
		backDC.SelectObject(pOldBackBmp);
		return;
	}

	BITMAP bm = { 0 };
	if (!::GetObject(hBitmap, sizeof(BITMAP), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
		return;

	CDC backDC;
	backDC.CreateCompatibleDC(&dc);
	CBitmap backBmp;
	backBmp.CreateCompatibleBitmap(&dc, clientRect.Width(), clientRect.Height());
	CBitmap* pOldBackBmp = backDC.SelectObject(&backBmp);

	CDC srcDC;
	srcDC.CreateCompatibleDC(&dc);
	HBITMAP hOldSrcBmp = (HBITMAP)srcDC.SelectObject(hBitmap);

	backDC.FillSolidRect(&clientRect, m_readingMode == READING_NIGHT ?
		DARK_VIEW_BACKGROUND : PDF_VIEW_BACKGROUND);

	int destX = 0;
	int destY = 0;
	if (bm.bmWidth < clientRect.Width())
		destX = (clientRect.Width() - bm.bmWidth) / 2;
	if (bm.bmHeight < clientRect.Height())
		destY = (clientRect.Height() - bm.bmHeight) / 2;

	int srcX = max(0, -destX);
	int srcY = max(0, -destY);
	int dstX = max(0, destX);
	int dstY = max(0, destY);
	int drawW = min(bm.bmWidth - srcX, clientRect.Width() - dstX);
	int drawH = min(bm.bmHeight - srcY, clientRect.Height() - dstY);
	if (drawW > 0 && drawH > 0)
		backDC.BitBlt(dstX, dstY, drawW, drawH, &srcDC, srcX, srcY, SRCCOPY);

	if (m_pParentDlg && (m_pParentDlg->m_hasSelection || m_pParentDlg->m_isSelecting))
		m_pParentDlg->UpdateSelectionHighlight(&backDC, m_pParentDlg->m_selPageNumber);

	if (m_readingMode == READING_EYE)
	{
		CDC tintDC;
		CBitmap tintBitmap;
		tintDC.CreateCompatibleDC(&dc);
		tintBitmap.CreateCompatibleBitmap(&dc, 1, 1);
		CBitmap* pOldTint = tintDC.SelectObject(&tintBitmap);
		tintDC.FillSolidRect(0, 0, 1, 1, RGB(92, 76, 50));
		BLENDFUNCTION blend = { AC_SRC_OVER, 0, 64, 0 };
		backDC.AlphaBlend(0, 0, clientRect.Width(), clientRect.Height(),
			&tintDC, 0, 0, 1, 1, blend);
		tintDC.SelectObject(pOldTint);
	}

	dc.BitBlt(0, 0, clientRect.Width(), clientRect.Height(), &backDC, 0, 0, SRCCOPY);

	srcDC.SelectObject(hOldSrcBmp);
	backDC.SelectObject(pOldBackBmp);
}
