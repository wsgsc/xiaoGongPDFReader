#include "pch.h"
#include "CustomButton.h"

IMPLEMENT_DYNAMIC(CCustomButton, CButton)

CCustomButton::CCustomButton()
	: m_normalColor(RGB(70, 130, 180))      // 钢蓝色
	, m_hoverColor(RGB(100, 149, 237))      // 矢车菊蓝
	, m_pressedColor(RGB(65, 105, 225))     // 皇家蓝
	, m_textColor(RGB(255, 255, 255))       // 白色文字
	, m_cornerRadius(6)                      // 圆角半径6像素
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

	// 创建字体
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

	// 确定按钮状态和颜色
	COLORREF btnColor = m_normalColor;
	if (state & ODS_SELECTED)
		btnColor = m_pressedColor;
	else if (m_bHover)
		btnColor = m_hoverColor;

	// 禁用状态
	if (state & ODS_DISABLED)
		btnColor = RGB(180, 180, 180);

	// 绘制圆角矩形背景
	DrawRoundRect(pDC, rect, m_cornerRadius, btnColor);

	// 绘制文字
	CString strText;
	GetWindowText(strText);

	if (!strText.IsEmpty())
	{
		CFont* pOldFont = pDC->SelectObject(&m_buttonFont);
		pDC->SetBkMode(TRANSPARENT);

		// 禁用状态下文字颜色变灰
		if (state & ODS_DISABLED)
			pDC->SetTextColor(RGB(120, 120, 120));
		else
			pDC->SetTextColor(m_textColor);

		pDC->DrawText(strText, rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		pDC->SelectObject(pOldFont);
	}

	// 绘制焦点框（如果有焦点）
	if (state & ODS_FOCUS)
	{
		CRect focusRect = rect;
		focusRect.DeflateRect(3, 3);
		CPen pen(PS_DOT, 1, RGB(255, 255, 255));
		CPen* pOldPen = pDC->SelectObject(&pen);
		pDC->SelectStockObject(NULL_BRUSH);
		pDC->RoundRect(focusRect, CPoint(m_cornerRadius - 2, m_cornerRadius - 2));
		pDC->SelectObject(pOldPen);
	}
}

void CCustomButton::DrawRoundRect(CDC* pDC, CRect rect, int radius, COLORREF color)
{
	// 使用GDI+绘制圆角矩形以获得更平滑的效果
	Gdiplus::Graphics graphics(pDC->GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	// 创建圆角路径
	Gdiplus::GraphicsPath path;
	int diameter = radius * 2;

	path.AddArc(rect.left, rect.top, diameter, diameter, 180, 90);
	path.AddArc(rect.right - diameter, rect.top, diameter, diameter, 270, 90);
	path.AddArc(rect.right - diameter, rect.bottom - diameter, diameter, diameter, 0, 90);
	path.AddArc(rect.left, rect.bottom - diameter, diameter, diameter, 90, 90);
	path.CloseFigure();

	// 填充背景
	Gdiplus::SolidBrush brush(Gdiplus::Color(GetRValue(color), GetGValue(color), GetBValue(color)));
	graphics.FillPath(&brush, &path);

	// 绘制边框（稍微深一点的颜色）
	int r = max(0, GetRValue(color) - 30);
	int g = max(0, GetGValue(color) - 30);
	int b = max(0, GetBValue(color) - 30);
	Gdiplus::Pen pen(Gdiplus::Color(r, g, b), 1.0f);
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
	// 防止闪烁
	return TRUE;
}
