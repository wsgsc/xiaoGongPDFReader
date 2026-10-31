#include "pch.h"
#include "CustomButton.h"

IMPLEMENT_DYNAMIC(CCustomButton, CButton)

CCustomButton::CCustomButton()
	: m_iconGlyph(0)
	, m_normalColor(RGB(255, 255, 255))
	, m_hoverColor(RGB(235, 243, 255))
	, m_pressedColor(RGB(219, 234, 255))
	, m_textColor(RGB(45, 58, 75))
	, m_cornerRadius(6)
	, m_bHover(FALSE)
	, m_bTracking(FALSE)
{
}

CCustomButton::~CCustomButton()
{
	if (m_buttonFont.GetSafeHandle())
		m_buttonFont.DeleteObject();
	if (m_iconFont.GetSafeHandle())
		m_iconFont.DeleteObject();
}

BEGIN_MESSAGE_MAP(CCustomButton, CButton)
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSELEAVE()
	ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

void CCustomButton::SetColors(COLORREF normalColor, COLORREF hoverColor, COLORREF pressedColor)
{
	m_normalColor = normalColor;
	m_hoverColor = hoverColor;
	m_pressedColor = pressedColor;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CCustomButton::SetTextColor(COLORREF textColor)
{
	m_textColor = textColor;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CCustomButton::SetCornerRadius(int radius)
{
	m_cornerRadius = radius;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CCustomButton::SetIconGlyph(WCHAR glyph)
{
	m_iconGlyph = glyph;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CCustomButton::SetToolTipText(LPCTSTR text)
{
	if (!GetSafeHwnd())
		return;
	if (!m_toolTip.GetSafeHwnd())
	{
		if (!m_toolTip.Create(this, TTS_ALWAYSTIP | TTS_NOPREFIX))
			return;
		m_toolTip.AddTool(this, text);
		m_toolTip.Activate(TRUE);
	}
	else
	{
		m_toolTip.UpdateTipText(text, this);
	}
}

BOOL CCustomButton::PreTranslateMessage(MSG* pMsg)
{
	if (m_toolTip.GetSafeHwnd())
		m_toolTip.RelayEvent(pMsg);
	return CButton::PreTranslateMessage(pMsg);
}

void CCustomButton::PreSubclassWindow()
{
	CButton::PreSubclassWindow();
	ModifyStyle(0, BS_OWNERDRAW);

	if (!m_buttonFont.GetSafeHandle())
	{
		m_buttonFont.CreatePointFont(90, _T("Microsoft YaHei UI"));
	}
	if (!m_iconFont.GetSafeHandle())
		m_iconFont.CreatePointFont(140, _T("Segoe MDL2 Assets"));
}

void CCustomButton::DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct)
{
	CDC* pDC = CDC::FromHandle(lpDrawItemStruct->hDC);
	CRect rect = lpDrawItemStruct->rcItem;
	UINT state = lpDrawItemStruct->itemState;

	// Draw into a memory DC so the rounded edge and text update together.
	CDC bufferDC;
	CBitmap bufferBitmap;
	bufferDC.CreateCompatibleDC(pDC);
	bufferBitmap.CreateCompatibleBitmap(pDC, rect.Width(), rect.Height());
	CBitmap* pOldBitmap = bufferDC.SelectObject(&bufferBitmap);
	CRect localRect(0, 0, rect.Width(), rect.Height());
	bufferDC.FillSolidRect(localRect, RGB(255, 255, 255));

	const bool bDisabled = (state & ODS_DISABLED) != 0;
	const bool bPressed = (state & ODS_SELECTED) != 0;
	COLORREF fillColor = m_normalColor;
	COLORREF borderColor = RGB(224, 230, 238);
	if (bDisabled)
	{
		fillColor = RGB(250, 251, 253);
		borderColor = RGB(236, 239, 243);
	}
	else if (bPressed)
	{
		fillColor = m_pressedColor;
		borderColor = RGB(117, 165, 230);
	}
	else if (m_bHover)
	{
		fillColor = m_hoverColor;
		borderColor = RGB(151, 188, 239);
	}
	else if (m_iconGlyph)
	{
		borderColor = RGB(255, 255, 255);
	}

	CRect buttonRect = localRect;
	buttonRect.DeflateRect(1, 1);
	DrawRoundRect(&bufferDC, buttonRect, m_cornerRadius, fillColor, borderColor);

	CString strText;
	GetWindowText(strText);

	bool useIcon = m_iconGlyph && m_iconFont.GetSafeHandle();
	if (useIcon)
	{
		CFont* pOldFont = bufferDC.SelectObject(&m_iconFont);
		CString face;
		bufferDC.GetTextFace(face);
		useIcon = face.CompareNoCase(_T("Segoe MDL2 Assets")) == 0;
		if (useIcon)
		{
			TCHAR glyph[] = { static_cast<TCHAR>(m_iconGlyph), 0 };
			bufferDC.SetBkMode(TRANSPARENT);
			bufferDC.SetTextColor(bDisabled ? RGB(163, 173, 185) :
				(m_bHover || bPressed ? RGB(32, 100, 185) : m_textColor));
			bufferDC.DrawText(glyph, localRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
		bufferDC.SelectObject(pOldFont);
	}
	if (!useIcon && !strText.IsEmpty())
	{
		CFont* pFont = m_iconGlyph ? &m_buttonFont : GetFont();
		CFont* pOldFont = bufferDC.SelectObject(pFont ? pFont : &m_buttonFont);
		bufferDC.SetBkMode(TRANSPARENT);
		bufferDC.SetTextColor(bDisabled ? RGB(163, 173, 185) :
			(bPressed ? RGB(35, 84, 155) : m_textColor));
		bufferDC.DrawText(strText, localRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		bufferDC.SelectObject(pOldFont);
	}

	if ((state & ODS_FOCUS) && !bDisabled)
	{
		CRect focusRect = localRect;
		focusRect.DeflateRect(4, 4);
		bufferDC.DrawFocusRect(focusRect);
	}

	pDC->BitBlt(rect.left, rect.top, rect.Width(), rect.Height(), &bufferDC, 0, 0, SRCCOPY);
	bufferDC.SelectObject(pOldBitmap);
}

void CCustomButton::DrawRoundRect(CDC* pDC, CRect rect, int radius, COLORREF fillColor, COLORREF borderColor)
{
	Gdiplus::Graphics graphics(pDC->GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	Gdiplus::RectF rf(
		(Gdiplus::REAL)rect.left   + 0.5f,
		(Gdiplus::REAL)rect.top    + 0.5f,
		(Gdiplus::REAL)(rect.Width()  - 1),
		(Gdiplus::REAL)(rect.Height() - 1)
	);

	float diameter = (float)(min(radius, min(rect.Width(), rect.Height()) / 2) * 2);

	Gdiplus::GraphicsPath path;
	path.AddArc(rf.X,                        rf.Y,                        diameter, diameter, 180, 90);
	path.AddArc(rf.X + rf.Width - diameter,  rf.Y,                        diameter, diameter, 270, 90);
	path.AddArc(rf.X + rf.Width - diameter,  rf.Y + rf.Height - diameter, diameter, diameter,   0, 90);
	path.AddArc(rf.X,                        rf.Y + rf.Height - diameter, diameter, diameter,  90, 90);
	path.CloseFigure();

	// Fill
	Gdiplus::SolidBrush brush(Gdiplus::Color(255,
		GetRValue(fillColor), GetGValue(fillColor), GetBValue(fillColor)));
	graphics.FillPath(&brush, &path);

	// Border
	Gdiplus::Pen pen(Gdiplus::Color(255,
		GetRValue(borderColor), GetGValue(borderColor), GetBValue(borderColor)), 1.0f);
	graphics.DrawPath(&pen, &path);
}

void CCustomButton::OnMouseMove(UINT nFlags, CPoint point)
{
	if (!m_bHover)
	{
		m_bHover = TRUE;
		Invalidate(FALSE);
	}

	if (!m_bTracking)
	{
		TRACKMOUSEEVENT tme;
		tme.cbSize = sizeof(TRACKMOUSEEVENT);
		tme.dwFlags = TME_LEAVE;
		tme.hwndTrack = m_hWnd;

		if (TrackMouseEvent(&tme))
		{
			m_bTracking = TRUE;
		}
	}

	CButton::OnMouseMove(nFlags, point);
}

void CCustomButton::OnMouseLeave()
{
	m_bHover = FALSE;
	m_bTracking = FALSE;
	Invalidate(FALSE);

	CButton::OnMouseLeave();
}

BOOL CCustomButton::OnEraseBkgnd(CDC* pDC)
{
	return TRUE;
}
