#include "pch.h"
#include "CustomButton.h"

IMPLEMENT_DYNAMIC(CCustomButton, CButton)

CCustomButton::CCustomButton()
	: m_normalColor(RGB(255, 255, 255))
	, m_hoverColor(RGB(232, 234, 237))
	, m_pressedColor(RGB(218, 220, 224))
	, m_textColor(RGB(32, 33, 36))
	, m_cornerRadius(5)
	, m_bHover(FALSE)
	, m_bTracking(FALSE)
{
}

CCustomButton::~CCustomButton()
{
	if (m_buttonFont.GetSafeHandle())
		m_buttonFont.DeleteObject();
}

BEGIN_MESSAGE_MAP(CCustomButton, CButton)
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSEHOVER()
	ON_WM_MOUSELEAVE()
	ON_WM_ERASEBKGND()
END_MESSAGE_MAP()

void CCustomButton::SetColors(COLORREF normalColor, COLORREF hoverColor, COLORREF pressedColor)
{
	m_normalColor = normalColor;
	m_hoverColor = hoverColor;
	m_pressedColor = pressedColor;
	if (GetSafeHwnd())
		Invalidate();
}

void CCustomButton::SetTextColor(COLORREF textColor)
{
	m_textColor = textColor;
	if (GetSafeHwnd())
		Invalidate();
}

void CCustomButton::SetCornerRadius(int radius)
{
	m_cornerRadius = radius;
	if (GetSafeHwnd())
		Invalidate();
}

void CCustomButton::PreSubclassWindow()
{
	CButton::PreSubclassWindow();
	ModifyStyle(0, BS_OWNERDRAW);

	if (!m_buttonFont.GetSafeHandle())
	{
		m_buttonFont.CreatePointFont(90, _T("Microsoft YaHei UI"));
	}
}

void CCustomButton::DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct)
{
	CDC* pDC = CDC::FromHandle(lpDrawItemStruct->hDC);
	CRect rect = lpDrawItemStruct->rcItem;
	UINT state = lpDrawItemStruct->itemState;

	// Fill entire area with toolbar white background to clear residuals
	CBrush bgBrush(RGB(255, 255, 255));
	pDC->FillRect(rect, &bgBrush);

	// Determine button state
	bool bDisabled = (state & ODS_DISABLED) != 0;
	bool bPressed  = (state & ODS_SELECTED) != 0;

	if (bDisabled)
	{
		// Disabled: white fill + light gray border
		DrawRoundRect(pDC, rect, m_cornerRadius, RGB(255, 255, 255), RGB(210, 210, 210));
	}
	else if (bPressed)
	{
		// Pressed: medium gray fill, no visible border
		DrawRoundRect(pDC, rect, m_cornerRadius, m_pressedColor, m_pressedColor);
	}
	else if (m_bHover)
	{
		// Hover: light gray fill, no visible border
		DrawRoundRect(pDC, rect, m_cornerRadius, m_hoverColor, m_hoverColor);
	}
	else
	{
		// Normal: white fill + gray border (makes button visible)
		DrawRoundRect(pDC, rect, m_cornerRadius, m_normalColor, RGB(218, 220, 224));
	}

	// Draw text
	CString strText;
	GetWindowText(strText);

	if (!strText.IsEmpty())
	{
		CFont* pOldFont = pDC->SelectObject(&m_buttonFont);
		pDC->SetBkMode(TRANSPARENT);

		if (bDisabled)
			pDC->SetTextColor(RGB(180, 180, 180));
		else
			pDC->SetTextColor(m_textColor);

		pDC->DrawText(strText, rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		pDC->SelectObject(pOldFont);
	}
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

	float diameter = (float)(radius * 2);

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
	if (!m_bTracking)
	{
		TRACKMOUSEEVENT tme;
		tme.cbSize = sizeof(TRACKMOUSEEVENT);
		tme.dwFlags = TME_HOVER | TME_LEAVE;
		tme.hwndTrack = m_hWnd;
		tme.dwHoverTime = HOVER_DEFAULT;

		if (TrackMouseEvent(&tme))
		{
			m_bTracking = TRUE;
		}
	}

	CButton::OnMouseMove(nFlags, point);
}

void CCustomButton::OnMouseHover(UINT nFlags, CPoint point)
{
	m_bHover = TRUE;
	Invalidate();

	CButton::OnMouseHover(nFlags, point);
}

void CCustomButton::OnMouseLeave()
{
	m_bHover = FALSE;
	m_bTracking = FALSE;
	Invalidate();

	CButton::OnMouseLeave();
}

BOOL CCustomButton::OnEraseBkgnd(CDC* pDC)
{
	return TRUE;
}
