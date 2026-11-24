#include "pch.h"
#include "CustomButton.h"

IMPLEMENT_DYNAMIC(CCustomButton, CButton)

CCustomButton::CCustomButton()
	: m_iconGlyph(0)
	, m_iconAndText(false)
	, m_iconResourceId(0)
	, m_iconSize(18)
	, m_pIconBitmap(nullptr)
	, m_pIconStream(nullptr)
	, m_windowControlStyle(false)
	, m_closeWindowControl(false)
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
	clearIconBitmap();
	if (m_buttonFont.GetSafeHandle())
		m_buttonFont.DeleteObject();
	if (m_iconFont.GetSafeHandle())
		m_iconFont.DeleteObject();
	if (m_windowIconFont.GetSafeHandle())
		m_windowIconFont.DeleteObject();
}

BEGIN_MESSAGE_MAP(CCustomButton, CButton)
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSELEAVE()
	ON_WM_ENABLE()
	ON_WM_SHOWWINDOW()
	ON_WM_CANCELMODE()
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
	if (m_iconResourceId)
	{
		m_iconResourceId = 0;
		clearIconBitmap();
	}
	m_iconGlyph = glyph;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CCustomButton::SetIconAndText(bool enabled)
{
	if (m_iconAndText == enabled)
		return;
	m_iconAndText = enabled;
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

void CCustomButton::SetIconImage(UINT resourceId, int iconSize)
{
	if (m_iconResourceId != resourceId)
		clearIconBitmap();
	m_iconResourceId = resourceId;
	m_iconSize = iconSize > 0 ? iconSize : 18;
	m_iconGlyph = 0;
	if (m_iconResourceId)
		ensureIconBitmap();
	if (GetSafeHwnd())
		Invalidate(FALSE);
}

Gdiplus::Bitmap* CCustomButton::ensureIconBitmap()
{
	if (m_pIconBitmap || !m_iconResourceId)
		return m_pIconBitmap;

	HINSTANCE hInstance = AfxGetInstanceHandle();
	HRSRC hRes = ::FindResource(hInstance, MAKEINTRESOURCE(m_iconResourceId), RT_RCDATA);
	if (!hRes)
		return nullptr;

	DWORD size = ::SizeofResource(hInstance, hRes);
	HGLOBAL hMem = ::LoadResource(hInstance, hRes);
	if (!hMem || size == 0)
		return nullptr;

	const void* pData = ::LockResource(hMem);
	if (!pData)
		return nullptr;

	IStream* pStream = ::SHCreateMemStream(
		static_cast<const BYTE*>(pData), static_cast<UINT>(size));
	if (!pStream)
		return nullptr;

	Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream, FALSE);
	if (!pBitmap || pBitmap->GetLastStatus() != Gdiplus::Ok ||
		pBitmap->GetWidth() == 0 || pBitmap->GetHeight() == 0)
	{
		delete pBitmap;
		pStream->Release();
		return nullptr;
	}

	m_pIconBitmap = pBitmap;
	m_pIconStream = pStream;
	return m_pIconBitmap;
}

void CCustomButton::clearIconBitmap()
{
	if (m_pIconBitmap)
	{
		delete m_pIconBitmap;
		m_pIconBitmap = nullptr;
	}
	if (m_pIconStream)
	{
		m_pIconStream->Release();
		m_pIconStream = nullptr;
	}
}

bool CCustomButton::DrawIconImage(CDC* pDC, const CRect& rect, bool disabled, bool active)
{
	Gdiplus::Bitmap* pBitmap = ensureIconBitmap();
	if (!pBitmap)
		return false;

	int side = min(m_iconSize, min(rect.Width(), rect.Height()));
	if (side <= 0)
		return false;

	const bool darkTheme = (static_cast<int>(GetRValue(m_normalColor)) +
		static_cast<int>(GetGValue(m_normalColor)) +
		static_cast<int>(GetBValue(m_normalColor))) < 300;
	COLORREF iconColor = disabled ?
		(darkTheme ? RGB(105, 113, 122) : RGB(163, 173, 185)) : m_textColor;
	if (!disabled && active)
		iconColor = darkTheme ? RGB(130, 182, 255) : RGB(32, 100, 185);

	// 源图为纯黑透明图标，用颜色矩阵把它整体替换为目标颜色，保留 alpha 通道。
	Gdiplus::ColorMatrix matrix = {};
	matrix.m[3][3] = 1.0f;
	matrix.m[4][0] = GetRValue(iconColor) / 255.0f;
	matrix.m[4][1] = GetGValue(iconColor) / 255.0f;
	matrix.m[4][2] = GetBValue(iconColor) / 255.0f;
	matrix.m[4][4] = 1.0f;

	Gdiplus::ImageAttributes attributes;
	attributes.SetColorMatrix(&matrix, Gdiplus::ColorMatrixFlagsDefault,
		Gdiplus::ColorAdjustTypeBitmap);

	Gdiplus::RectF destRect(
		(Gdiplus::REAL)(rect.left + (rect.Width() - side) / 2),
		(Gdiplus::REAL)(rect.top + (rect.Height() - side) / 2),
		(Gdiplus::REAL)side,
		(Gdiplus::REAL)side);

	Gdiplus::Graphics graphics(pDC->GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
	graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
	graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
	graphics.DrawImage(pBitmap, destRect, 0, 0,
		(Gdiplus::REAL)pBitmap->GetWidth(), (Gdiplus::REAL)pBitmap->GetHeight(),
		Gdiplus::UnitPixel, &attributes);

	return true;
}

void CCustomButton::SetWindowControlStyle(bool closeButton)
{
	m_windowControlStyle = true;
	m_closeWindowControl = closeButton;
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
	if (!lpDrawItemStruct || !lpDrawItemStruct->hDC)
		return;

	CDC* pDC = CDC::FromHandle(lpDrawItemStruct->hDC);
	CRect rect = lpDrawItemStruct->rcItem;
	if (!pDC || rect.Width() <= 0 || rect.Height() <= 0)
		return;
	UINT state = lpDrawItemStruct->itemState;

	// Draw into a memory DC so the rounded edge and text update together.
	CDC bufferDC;
	CBitmap bufferBitmap;
	if (!bufferDC.CreateCompatibleDC(pDC) ||
		!bufferBitmap.CreateCompatibleBitmap(pDC, rect.Width(), rect.Height()))
		return;
	CBitmap* pOldBitmap = bufferDC.SelectObject(&bufferBitmap);
	CRect localRect(0, 0, rect.Width(), rect.Height());
	bufferDC.FillSolidRect(localRect, m_normalColor);
	if (!m_buttonFont.GetSafeHandle())
		m_buttonFont.CreatePointFont(90, _T("Microsoft YaHei UI"));
	if (!m_iconFont.GetSafeHandle())
		m_iconFont.CreatePointFont(140, _T("Segoe MDL2 Assets"));
	if (m_windowControlStyle && !m_windowIconFont.GetSafeHandle())
		m_windowIconFont.CreatePointFont(115, _T("Segoe MDL2 Assets"));

	const bool bDisabled = (state & ODS_DISABLED) != 0;
	const bool bPressed = (state & ODS_SELECTED) != 0;
	const bool darkTheme = (static_cast<int>(GetRValue(m_normalColor)) +
		static_cast<int>(GetGValue(m_normalColor)) +
		static_cast<int>(GetBValue(m_normalColor))) < 300;
	COLORREF fillColor = m_normalColor;
	COLORREF borderColor = m_normalColor;
	if (bDisabled)
	{
		fillColor = darkTheme ? RGB(45, 49, 54) : RGB(250, 251, 253);
		borderColor = darkTheme ? RGB(58, 63, 69) : RGB(236, 239, 243);
	}
	else if (bPressed)
	{
		fillColor = m_pressedColor;
		borderColor = darkTheme ? RGB(92, 130, 180) : RGB(158, 192, 248);
	}
	else if (m_bHover)
	{
		fillColor = m_hoverColor;
		borderColor = darkTheme ? RGB(82, 102, 128) : RGB(186, 209, 250);
	}
	else
		borderColor = m_normalColor;

	if (m_windowControlStyle)
		bufferDC.FillSolidRect(localRect, fillColor);
	else
	{
		CRect buttonRect = localRect;
		buttonRect.DeflateRect(1, 1);
		DrawRoundRect(&bufferDC, buttonRect, m_cornerRadius, fillColor, borderColor);
	}

	CString strText;
	GetWindowText(strText);

	// PNG 图标优先于字体图标；失败时自动回退到文字。
	CRect iconRect = localRect;
	if (m_iconAndText)
	{
		iconRect.left += 7;
		iconRect.right = iconRect.left + 24;
	}
	const bool useImageIcon = DrawIconImage(&bufferDC, iconRect, bDisabled, m_bHover || bPressed);

	CFont* pIconFont = m_windowControlStyle ? &m_windowIconFont : &m_iconFont;
	bool useIcon = m_iconGlyph && pIconFont->GetSafeHandle();
	if (useIcon)
	{
		CFont* pOldFont = bufferDC.SelectObject(pIconFont);
		CString face;
		bufferDC.GetTextFace(face);
		useIcon = face.CompareNoCase(_T("Segoe MDL2 Assets")) == 0;
		if (useIcon)
		{
			TCHAR glyph[] = { static_cast<TCHAR>(m_iconGlyph), 0 };
			bufferDC.SetBkMode(TRANSPARENT);
			COLORREF iconColor = bDisabled ?
				(darkTheme ? RGB(105, 113, 122) : RGB(163, 173, 185)) : m_textColor;
			if (!bDisabled && m_windowControlStyle && m_closeWindowControl &&
				(m_bHover || bPressed))
				iconColor = RGB(255, 255, 255);
			else if (!bDisabled && !m_windowControlStyle && (m_bHover || bPressed))
				iconColor = darkTheme ? RGB(130, 182, 255) : RGB(32, 100, 185);
			bufferDC.SetTextColor(iconColor);
			bufferDC.DrawText(glyph, iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
		bufferDC.SelectObject(pOldFont);
	}
	if ((m_iconAndText || (!useIcon && !useImageIcon)) && !strText.IsEmpty())
	{
		CFont* pFont = m_iconGlyph ? &m_buttonFont : GetFont();
		CFont* pOldFont = bufferDC.SelectObject(pFont ? pFont : &m_buttonFont);
		bufferDC.SetBkMode(TRANSPARENT);
		COLORREF textColor = bDisabled ?
			(darkTheme ? RGB(105, 113, 122) : RGB(163, 173, 185)) :
			(bPressed && !m_windowControlStyle ?
				(darkTheme ? RGB(130, 182, 255) : RGB(35, 84, 155)) : m_textColor);
		if (!bDisabled && m_windowControlStyle && m_closeWindowControl &&
			(m_bHover || bPressed))
			textColor = RGB(255, 255, 255);
		bufferDC.SetTextColor(textColor);
		CRect textRect = localRect;
		if (m_iconAndText)
			textRect.left += 32;
		bufferDC.DrawText(strText, textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
		bufferDC.SelectObject(pOldFont);
	}

	if ((state & ODS_FOCUS) && !bDisabled && !m_windowControlStyle)
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
	if (!pDC || rect.Width() <= 1 || rect.Height() <= 1)
		return;

	Gdiplus::Graphics graphics(pDC->GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

	Gdiplus::RectF rf(
		(Gdiplus::REAL)rect.left   + 0.5f,
		(Gdiplus::REAL)rect.top    + 0.5f,
		(Gdiplus::REAL)(rect.Width()  - 1),
		(Gdiplus::REAL)(rect.Height() - 1)
	);

	Gdiplus::SolidBrush brush(Gdiplus::Color(255,
		GetRValue(fillColor), GetGValue(fillColor), GetBValue(fillColor)));
	Gdiplus::Pen pen(Gdiplus::Color(255,
		GetRValue(borderColor), GetGValue(borderColor), GetBValue(borderColor)), 1.0f);

	int clampedRadius = max(0, min(radius, min(rect.Width(), rect.Height()) / 2));
	float diameter = (float)(clampedRadius * 2);
	if (diameter <= 0.0f)
	{
		graphics.FillRectangle(&brush, rf);
		graphics.DrawRectangle(&pen, rf);
		return;
	}

	Gdiplus::GraphicsPath path;
	path.AddArc(rf.X,                        rf.Y,                        diameter, diameter, 180, 90);
	path.AddArc(rf.X + rf.Width - diameter,  rf.Y,                        diameter, diameter, 270, 90);
	path.AddArc(rf.X + rf.Width - diameter,  rf.Y + rf.Height - diameter, diameter, diameter,   0, 90);
	path.AddArc(rf.X,                        rf.Y + rf.Height - diameter, diameter, diameter,  90, 90);
	path.CloseFigure();

	graphics.FillPath(&brush, &path);
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

void CCustomButton::OnEnable(BOOL bEnable)
{
	if (!bEnable && (m_bHover || m_bTracking))
	{
		m_bHover = FALSE;
		m_bTracking = FALSE;
		Invalidate(FALSE);
	}
	CButton::OnEnable(bEnable);
}

void CCustomButton::OnShowWindow(BOOL bShow, UINT nStatus)
{
	if (!bShow && (m_bHover || m_bTracking))
	{
		m_bHover = FALSE;
		m_bTracking = FALSE;
	}
	CButton::OnShowWindow(bShow, nStatus);
}

void CCustomButton::OnCancelMode()
{
	if (m_bHover || m_bTracking)
	{
		m_bHover = FALSE;
		m_bTracking = FALSE;
		Invalidate(FALSE);
	}
	CButton::OnCancelMode();
}

BOOL CCustomButton::OnEraseBkgnd(CDC* pDC)
{
	return TRUE;
}
