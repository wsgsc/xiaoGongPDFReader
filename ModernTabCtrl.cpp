#include "pch.h"
#include <commctrl.h>
#include "ModernTabCtrl.h"
#include "PDFDocument.h"

IMPLEMENT_DYNAMIC(CModernTabCtrl, CTabCtrl)

BEGIN_MESSAGE_MAP(CModernTabCtrl, CTabCtrl)
	ON_WM_PAINT()
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSEWHEEL()
	ON_WM_MOUSELEAVE()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_LBUTTONDBLCLK()
	ON_WM_MBUTTONDOWN()
	ON_WM_MBUTTONUP()
	ON_WM_RBUTTONDOWN()
	ON_WM_RBUTTONUP()
	ON_WM_CAPTURECHANGED()
	ON_WM_CANCELMODE()
	ON_WM_ERASEBKGND()
	ON_WM_DESTROY()
	ON_MESSAGE(TCM_SETCURSEL, &CModernTabCtrl::OnSetCurSelMsg)
END_MESSAGE_MAP()

CModernTabCtrl::CModernTabCtrl()
	: m_hoverIndex(-1)
	, m_closeHoverIndex(-1)
	, m_bCloseButtonEnabled(TRUE)
	, m_bTracking(FALSE)
	, m_tabDragArmed(FALSE)
	, m_tabDragging(FALSE)
	, m_tabPressedIndex(-1)
	, m_middlePressedIndex(-1)
	, m_tabDragStartScreen(0, 0)
	, m_tabDragStartWindow(0, 0, 0, 0)
	, m_scrollOffset(0)
	, m_lastEnsuredSel(-1)
	, m_wheelRemainder(0)
	, m_tabHeight(46)
	, m_normalColor(RGB(240, 240, 240))      // 浅灰色背景
	, m_hoverColor(RGB(230, 230, 230))       // 稍深的灰色（悬停）
	, m_selectedColor(RGB(255, 255, 255))    // 白色（选中）
{
	// 使用负值字高指定实际字符高度（与下方工具栏字号协调），并开启 ClearType 抗锯齿
	VERIFY(m_font.CreateFont(
		-15,                       // 字符高度 15px
		0,
		0,
		0,
		FW_NORMAL,
		FALSE,
		FALSE,
		0,
		DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY,
		DEFAULT_PITCH | FF_SWISS,
		_T("微软雅黑")));
	VERIFY(m_selectedFont.CreateFont(
		-15, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, 0,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑")));
	VERIFY(m_compactFont.CreateFont(
		-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, 0,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑")));
	VERIFY(m_badgeFont.CreateFont(
		-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, 0,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI")));
}

CModernTabCtrl::~CModernTabCtrl()
{
	// 双重保护：如果OnDestroy没有清理，这里再清理一次
	// 如果已经删除了，GetSafeHandle()会返回NULL，不会重复删除
	if (m_font.GetSafeHandle())
	{
		m_font.DeleteObject();
	}
	if (m_selectedFont.GetSafeHandle())
	{
		m_selectedFont.DeleteObject();
	}
	if (m_compactFont.GetSafeHandle())
	{
		m_compactFont.DeleteObject();
	}
	if (m_badgeFont.GetSafeHandle())
	{
		m_badgeFont.DeleteObject();
	}
}

void CModernTabCtrl::OnDestroy()
{
	// 在窗口销毁时清理GDI资源（推荐做法）
	if (m_font.GetSafeHandle())
	{
		m_font.DeleteObject();
	}
	if (m_selectedFont.GetSafeHandle())
	{
		m_selectedFont.DeleteObject();
	}
	if (m_compactFont.GetSafeHandle())
	{
		m_compactFont.DeleteObject();
	}
	if (m_badgeFont.GetSafeHandle())
	{
		m_badgeFont.DeleteObject();
	}

	CTabCtrl::OnDestroy();
}

void CModernTabCtrl::EnableCloseButton(BOOL bEnable)
{
	m_bCloseButtonEnabled = bEnable;
	if (GetSafeHwnd())
		Invalidate();
}

void CModernTabCtrl::SetTabHeight(int height)
{
	m_tabHeight = height;
	if (GetSafeHwnd())
	{
		// 更新控件高度
		CWnd* pParent = GetParent();
		if (pParent)
		{
			CRect rect;
			GetWindowRect(&rect);
			pParent->ScreenToClient(&rect);
			rect.bottom = rect.top + m_tabHeight;
			MoveWindow(&rect);
		}
		Invalidate();
	}
}

void CModernTabCtrl::SetColors(COLORREF normal, COLORREF hover, COLORREF selected)
{
	m_normalColor = normal;
	m_hoverColor = hover;
	m_selectedColor = selected;
	if (GetSafeHwnd())
		Invalidate();
}

void CModernTabCtrl::HideOverflowButtons()
{
	if (!GetSafeHwnd())
		return;

	HWND hButtons = ::FindWindowEx(GetSafeHwnd(), nullptr, UPDOWN_CLASS, nullptr);
	if (hButtons && ::IsWindowVisible(hButtons))
		::ShowWindow(hButtons, SW_HIDE);
}

LRESULT CModernTabCtrl::OnSetCurSelMsg(WPARAM wParam, LPARAM lParam)
{
	// 原生 SysTabControl32 处理 TCM_SETCURSEL 时仅使内部按钮区域失效（不含底部 3px 选中蓝条），
	// 且多标签溢出时坐标与自绘滚动偏移不一致，因此统一同步可视位置并整条无闪烁重绘。
	const LRESULT prevSel = DefWindowProc(TCM_SETCURSEL, wParam, lParam);
	const int curSel = static_cast<int>(DefWindowProc(TCM_GETCURSEL, 0, 0));
	m_lastEnsuredSel = curSel;
	EnsureTabVisible(curSel);
	HideOverflowButtons();
	Invalidate(FALSE);
	return prevSel;
}

BOOL CModernTabCtrl::OnEraseBkgnd(CDC* pDC)
{
	// 自己处理背景绘制，避免闪烁
	return TRUE;
}

void CModernTabCtrl::OnPaint()
{
	HideOverflowButtons();
	CPaintDC dcPaint(this);

	CRect clientRect;
	GetClientRect(&clientRect);
	if (clientRect.Width() <= 0 || clientRect.Height() <= 0)
		return;

	// 双缓冲绘制，避免闪烁
	CDC memDC;
	if (!memDC.CreateCompatibleDC(&dcPaint))
		return;

	CBitmap memBitmap;
	if (!memBitmap.CreateCompatibleBitmap(&dcPaint, clientRect.Width(), clientRect.Height()))
	{
		memDC.DeleteDC();
		return;
	}

	CBitmap* pOldBitmap = memDC.SelectObject(&memBitmap);
	if (!pOldBitmap)
	{
		memBitmap.DeleteObject();
		memDC.DeleteDC();
		return;
	}

	// 绘制背景
	memDC.FillSolidRect(clientRect, m_normalColor);

	// 获取标签数量
	int tabCount = GetItemCount();
	int curSel = GetCurSel();
	if (curSel != m_lastEnsuredSel)
	{
		m_lastEnsuredSel = curSel;
		EnsureTabVisible(curSel);
	}
	else
	{
		ClampScrollOffset();
	}

	// 先绘制未选中的标签页
	for (int i = 0; i < tabCount; i++)
	{
		if (i != curSel)
		{
			CRect tabRect = GetTabRect(i);
			if (tabRect.right <= 0 || tabRect.left >= clientRect.Width())
				continue;
			BOOL isHovered = (i == m_hoverIndex);
			DrawTab(&memDC, i, tabRect, FALSE, isHovered);
		}
	}

	// 最后绘制选中的标签页（在最上层）
	if (curSel >= 0 && curSel < tabCount)
	{
		CRect tabRect = GetTabRect(curSel);
		if (tabRect.right > 0 && tabRect.left < clientRect.Width())
			DrawTab(&memDC, curSel, tabRect, TRUE, FALSE);
	}

	// 复制到屏幕
	dcPaint.BitBlt(0, 0, clientRect.Width(), clientRect.Height(), &memDC, 0, 0, SRCCOPY);

	// 清理：必须恢复原来的bitmap，然后显式删除GDI对象
	memDC.SelectObject(pOldBitmap);
	memBitmap.DeleteObject();
	memDC.DeleteDC();
}

void CModernTabCtrl::DrawTab(CDC* pDC, int index, CRect rect, BOOL isSelected, BOOL isHovered)
{
	const bool dark = GetRValue(m_normalColor) < 100;
	// 根据状态选择背景色
	COLORREF bgColor;
	if (isSelected)
		bgColor = m_selectedColor;
	else if (isHovered)
		bgColor = m_hoverColor;
	else
		bgColor = dark ? RGB(51, 55, 61) : RGB(244, 248, 254);

	// 绘制圆角矩形背景
	CPen pen;
	pen.CreatePen(PS_SOLID, 1, isSelected ?
		(dark ? RGB(73, 79, 85) : RGB(216, 228, 250)) :
		(dark ? RGB(78, 83, 91) : RGB(200, 215, 236)));
	CPen* pOldPen = pDC->SelectObject(&pen);

	CBrush brush;
	brush.CreateSolidBrush(bgColor);
	CBrush* pOldBrush = pDC->SelectObject(&brush);

	// 调整矩形以创建间隔
	rect.DeflateRect(3, 3, 3, 0);

	// 绘制圆角矩形（上方圆角）
	pDC->RoundRect(rect, CPoint(9, 9));

	// 恢复原来的GDI对象（让MFC自动清理pen和brush）
	pDC->SelectObject(pOldBrush);
	pDC->SelectObject(pOldPen);

	// Selected tabs use a blue underline.
	if (isSelected)
	{
		pDC->FillSolidRect(rect.left + 3, rect.bottom - 3, rect.Width() - 6, 3,
			dark ? RGB(94, 158, 249) : RGB(26, 119, 244));
	}

	// 获取标签文本
	TCHAR szText[256] = { 0 };
	TCITEM tci = {};
	tci.mask = TCIF_TEXT;
	tci.pszText = szText;
	tci.cchTextMax = _countof(szText);
	if (!GetItem(index, &tci))
		szText[0] = _T('\0');

	const bool compact = GetTabRect(index).Width() < 145;
	const BOOL showCloseBtn = IsCloseButtonVisible(index);

	// 绘制圆角蓝底 PDF 图标角标（20x20，紧凑模式 18x18）
	const int iconSize = compact ? 18 : 20;
	const int iconLeft = rect.left + (compact ? 7 : 10);
	CRect iconRect(iconLeft, rect.top + (rect.Height() - iconSize) / 2,
		iconLeft + iconSize, rect.top + (rect.Height() + iconSize) / 2);
	{
		Gdiplus::Graphics graphics(pDC->GetSafeHdc());
		graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		const Gdiplus::REAL r = 4.0f;
		const Gdiplus::REAL d = r * 2.0f;
		Gdiplus::RectF badgeRf(
			static_cast<Gdiplus::REAL>(iconRect.left),
			static_cast<Gdiplus::REAL>(iconRect.top),
			static_cast<Gdiplus::REAL>(iconRect.Width()),
			static_cast<Gdiplus::REAL>(iconRect.Height()));
		Gdiplus::GraphicsPath badgePath;
		badgePath.AddArc(badgeRf.X, badgeRf.Y, d, d, 180.0f, 90.0f);
		badgePath.AddArc(badgeRf.GetRight() - d, badgeRf.Y, d, d, 270.0f, 90.0f);
		badgePath.AddArc(badgeRf.GetRight() - d, badgeRf.GetBottom() - d, d, d, 0.0f, 90.0f);
		badgePath.AddArc(badgeRf.X, badgeRf.GetBottom() - d, d, d, 90.0f, 90.0f);
		badgePath.CloseFigure();
		Gdiplus::SolidBrush badgeBrush(Gdiplus::Color(255, 29, 124, 243));
		graphics.FillPath(&badgeBrush, &badgePath);
	}
	CFont* pOldFont = pDC->SelectObject(&m_badgeFont);
	pDC->SetBkMode(TRANSPARENT);
	pDC->SetTextColor(RGB(255, 255, 255));
	pDC->DrawText(_T("P"), iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

	// 计算文件名区域：非激活且未悬停的标签不预留关闭按钮空白，尽可能完整展示文件名
	CRect textRect = rect;
	textRect.left = iconRect.right + (compact ? 6 : 8);
	if (showCloseBtn)
		textRect.right = GetCloseButtonRect(index).left - 4;
	else
		textRect.right = rect.right - 8;

	pDC->SelectObject(compact ? &m_compactFont : (isSelected ? &m_selectedFont : &m_font));
	pDC->SetTextColor(dark ? RGB(226, 231, 235) :
		(isSelected ? RGB(24, 32, 44) : RGB(45, 55, 68)));
	pDC->SetBkMode(TRANSPARENT);

	CString displayText = szText;
	pDC->DrawText(displayText, textRect,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
	pDC->SelectObject(pOldFont);

	// 绘制关闭按钮
	if (showCloseBtn)
	{
		CRect closeRect = GetCloseButtonRect(index);
		BOOL isCloseHovered = (index == m_closeHoverIndex);
		DrawCloseButton(pDC, closeRect, isCloseHovered);
	}
}

void CModernTabCtrl::DrawCloseButton(CDC* pDC, CRect rect, BOOL isHovered)
{
	// 悬停时绘制圆角红色背景
	if (isHovered)
	{
		Gdiplus::Graphics graphics(pDC->GetSafeHdc());
		graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		const Gdiplus::REAL r = 4.0f;
		const Gdiplus::REAL d = r * 2.0f;
		Gdiplus::RectF rf(
			static_cast<Gdiplus::REAL>(rect.left),
			static_cast<Gdiplus::REAL>(rect.top),
			static_cast<Gdiplus::REAL>(rect.Width()),
			static_cast<Gdiplus::REAL>(rect.Height()));
		Gdiplus::GraphicsPath path;
		path.AddArc(rf.X, rf.Y, d, d, 180.0f, 90.0f);
		path.AddArc(rf.GetRight() - d, rf.Y, d, d, 270.0f, 90.0f);
		path.AddArc(rf.GetRight() - d, rf.GetBottom() - d, d, d, 0.0f, 90.0f);
		path.AddArc(rf.X, rf.GetBottom() - d, d, d, 90.0f, 90.0f);
		path.CloseFigure();
		Gdiplus::SolidBrush brush(Gdiplus::Color(255, 232, 17, 35));
		graphics.FillPath(&brush, &path);
	}

	// 绘制抗锯齿 × 符号
	Gdiplus::Graphics graphics(pDC->GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
	const COLORREF lineColor = isHovered ? RGB(255, 255, 255) :
		(GetRValue(m_normalColor) < 100 ? RGB(190, 198, 205) : RGB(96, 106, 118));
	Gdiplus::Pen pen(Gdiplus::Color(255,
		GetRValue(lineColor), GetGValue(lineColor), GetBValue(lineColor)), 1.6f);
	const Gdiplus::REAL margin = rect.Width() <= 16 ? 4.5f : 5.0f;
	graphics.DrawLine(&pen,
		static_cast<Gdiplus::REAL>(rect.left) + margin,
		static_cast<Gdiplus::REAL>(rect.top) + margin,
		static_cast<Gdiplus::REAL>(rect.right) - margin,
		static_cast<Gdiplus::REAL>(rect.bottom) - margin);
	graphics.DrawLine(&pen,
		static_cast<Gdiplus::REAL>(rect.right) - margin,
		static_cast<Gdiplus::REAL>(rect.top) + margin,
		static_cast<Gdiplus::REAL>(rect.left) + margin,
		static_cast<Gdiplus::REAL>(rect.bottom) - margin);
}

CRect CModernTabCtrl::GetCloseButtonRect(int index)
{
	CRect tabRect = GetTabRect(index);
	const bool compact = tabRect.Width() < 145;
	const int btnSize = compact ? 16 : 18;
	const int contentTop = tabRect.top + 3;
	const int contentHeight = max(btnSize, tabRect.Height() - 3);
	const int top = contentTop + (contentHeight - btnSize) / 2;
	const int rightMargin = compact ? 7 : 9;
	return CRect(tabRect.right - rightMargin - btnSize, top,
		tabRect.right - rightMargin, top + btnSize);
}

BOOL CModernTabCtrl::IsCloseButtonVisible(int index)
{
	return m_bCloseButtonEnabled && (index == GetCurSel() || index == m_hoverIndex);
}

int CModernTabCtrl::CalculateTabWidth(int availableWidth, int tabCount) const
{
	if (tabCount <= 0)
		return MAX_TAB_WIDTH;
	int tabWidth = availableWidth / tabCount;
	if (tabWidth > MAX_TAB_WIDTH)
		tabWidth = MAX_TAB_WIDTH;
	if (tabWidth < MIN_TAB_WIDTH)
		tabWidth = MIN_TAB_WIDTH;
	return tabWidth;
}

int CModernTabCtrl::GetMaxScrollOffset() const
{
	int tabCount = GetItemCount();
	if (tabCount <= 0 || !GetSafeHwnd())
		return 0;

	CRect clientRect;
	GetClientRect(&clientRect);
	int availableWidth = clientRect.Width();
	if (availableWidth <= 0)
		return 0;

	const int tabWidth = CalculateTabWidth(availableWidth, tabCount);
	const int totalWidth = tabCount * tabWidth;
	return max(0, totalWidth - availableWidth);
}

void CModernTabCtrl::ClampScrollOffset()
{
	const int maxOffset = GetMaxScrollOffset();
	if (m_scrollOffset < 0)
		m_scrollOffset = 0;
	else if (m_scrollOffset > maxOffset)
		m_scrollOffset = maxOffset;
}

void CModernTabCtrl::EnsureTabVisible(int index)
{
	int tabCount = GetItemCount();
	if (tabCount <= 0 || index < 0 || index >= tabCount || !GetSafeHwnd())
	{
		ClampScrollOffset();
		return;
	}

	CRect clientRect;
	GetClientRect(&clientRect);
	int availableWidth = clientRect.Width();
	if (availableWidth <= 0)
		return;

	const int tabWidth = CalculateTabWidth(availableWidth, tabCount);
	const int tabLeft = index * tabWidth;
	const int tabRight = tabLeft + tabWidth;
	if (tabLeft < m_scrollOffset)
		m_scrollOffset = tabLeft;
	else if (tabRight > m_scrollOffset + availableWidth)
		m_scrollOffset = tabRight - availableWidth;

	ClampScrollOffset();
}

void CModernTabCtrl::ResetDragState()
{
	m_tabDragArmed = FALSE;
	m_tabDragging = FALSE;
	m_tabPressedIndex = -1;
	m_middlePressedIndex = -1;
}

BOOL CModernTabCtrl::PreTranslateMessage(MSG* pMsg)
{
	if (m_toolTip.GetSafeHwnd())
		m_toolTip.RelayEvent(pMsg);
	return CTabCtrl::PreTranslateMessage(pMsg);
}

CString CModernTabCtrl::GetTabToolTipText(int index) const
{
	if (index < 0 || index >= GetItemCount())
		return CString();

	TCHAR szText[256] = { 0 };
	TCITEM tci = {};
	tci.mask = TCIF_PARAM | TCIF_TEXT;
	tci.pszText = szText;
	tci.cchTextMax = _countof(szText);
	if (!GetItem(index, &tci))
		return CString();

	if (tci.lParam != 0)
	{
		const CPDFDocument* pDoc = reinterpret_cast<const CPDFDocument*>(tci.lParam);
		if (pDoc && !pDoc->GetFilePath().IsEmpty())
			return pDoc->GetFilePath();
	}
	return CString(szText);
}

void CModernTabCtrl::UpdateHoverToolTip()
{
	if (!GetSafeHwnd())
		return;

	if (m_hoverIndex < 0)
	{
		if (m_toolTip.GetSafeHwnd())
		{
			m_toolTip.Pop();
			m_toolTip.Activate(FALSE);
		}
		m_lastToolTipText.Empty();
		return;
	}

	CString tipText = (m_closeHoverIndex >= 0) ?
		CString(_T("关闭标签页 (Ctrl+W / 鼠标中键)")) :
		GetTabToolTipText(m_hoverIndex);
	if (tipText.IsEmpty())
		return;

	if (!m_toolTip.GetSafeHwnd())
	{
		if (!m_toolTip.Create(this, TTS_ALWAYSTIP | TTS_NOPREFIX))
			return;
		m_toolTip.SetMaxTipWidth(640);
		m_toolTip.AddTool(this, tipText);
	}

	if (tipText != m_lastToolTipText)
	{
		m_lastToolTipText = tipText;
		m_toolTip.UpdateTipText(tipText, this);
	}
	m_toolTip.Activate(TRUE);
}

CRect CModernTabCtrl::GetTabRect(int index)
{
	int tabCount = GetItemCount();
	if (tabCount <= 0)
	{
		CRect rect(0, 0, 0, 0);
		GetItemRect(index, &rect);
		return rect;
	}

	CRect clientRect;
	GetClientRect(&clientRect);
	const int tabWidth = CalculateTabWidth(clientRect.Width(), tabCount);

	CRect rect;
	rect.left = index * tabWidth - m_scrollOffset;
	rect.right = rect.left + tabWidth;
	rect.top = 0;
	rect.bottom = m_tabHeight;

	return rect;
}

int CModernTabCtrl::HitTestTab(CPoint point)
{
	CRect clientRect;
	GetClientRect(&clientRect);
	if (!clientRect.PtInRect(point))
		return -1;

	int tabCount = GetItemCount();
	for (int i = 0; i < tabCount; i++)
	{
		CRect tabRect = GetTabRect(i);
		if (tabRect.PtInRect(point))
			return i;
	}
	return -1;
}

void CModernTabCtrl::OnMouseMove(UINT nFlags, CPoint point)
{
	if (m_tabDragArmed)
	{
		CWnd* pParent = GetParent();
		if (!(nFlags & MK_LBUTTON) || !pParent)
		{
			ResetDragState();
			if (::GetCapture() == GetSafeHwnd())
				::ReleaseCapture();
		}
		else
		{
			CPoint cursor;
			::GetCursorPos(&cursor);
			const int dx = cursor.x - m_tabDragStartScreen.x;
			const int dy = cursor.y - m_tabDragStartScreen.y;
			if (m_tabDragging || dx * dx + dy * dy >= 25)
			{
				if (!m_tabDragging)
				{
					m_tabDragging = TRUE;
					if (m_toolTip.GetSafeHwnd())
					{
						m_toolTip.Pop();
						m_toolTip.Activate(FALSE);
					}
				}
				if (pParent->IsZoomed())
				{
					const int startWidth = m_tabDragStartWindow.Width();
					const double xRatio = startWidth > 0 ?
						static_cast<double>(m_tabDragStartScreen.x - m_tabDragStartWindow.left) / startWidth :
						0.5;
					pParent->ShowWindow(SW_RESTORE);
					pParent->GetWindowRect(&m_tabDragStartWindow);
					m_tabDragStartWindow.OffsetRect(
						cursor.x - m_tabDragStartWindow.left -
						static_cast<int>(xRatio * m_tabDragStartWindow.Width()),
						cursor.y - m_tabDragStartWindow.top - m_tabHeight / 2);
					m_tabDragStartScreen = cursor;
				}
				pParent->SetWindowPos(nullptr,
					m_tabDragStartWindow.left + cursor.x - m_tabDragStartScreen.x,
					m_tabDragStartWindow.top + cursor.y - m_tabDragStartScreen.y,
					0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
				return;
			}
		}
	}
	// 开始跟踪鼠标离开事件
	if (!m_bTracking)
	{
		TRACKMOUSEEVENT tme;
		tme.cbSize = sizeof(tme);
		tme.dwFlags = TME_LEAVE;
		tme.hwndTrack = GetSafeHwnd();
		TrackMouseEvent(&tme);
		m_bTracking = TRUE;
	}

	// 检测鼠标在哪个标签页上
	int oldHoverIndex = m_hoverIndex;
	int oldCloseHoverIndex = m_closeHoverIndex;
	m_hoverIndex = -1;
	m_closeHoverIndex = -1;

	int tabCount = GetItemCount();
	for (int i = 0; i < tabCount; i++)
	{
		CRect tabRect = GetTabRect(i);
		if (tabRect.PtInRect(point))
		{
			m_hoverIndex = i;

			// 检查是否在关闭按钮上
			if (IsCloseButtonVisible(i))
			{
				CRect closeRect = GetCloseButtonRect(i);
				if (closeRect.PtInRect(point))
				{
					m_closeHoverIndex = i;
				}
			}
			break;
		}
	}

	// 如果悬停状态改变，更新提示并重绘
	if (oldHoverIndex != m_hoverIndex || oldCloseHoverIndex != m_closeHoverIndex)
	{
		UpdateHoverToolTip();
		Invalidate(FALSE);
	}

	CTabCtrl::OnMouseMove(nFlags, point);
}

BOOL CModernTabCtrl::OnMouseWheel(UINT nFlags, short zDelta, CPoint point)
{
	CPoint clientPoint = point;
	ScreenToClient(&clientPoint);
	CRect clientRect;
	GetClientRect(&clientRect);
	if (!clientRect.PtInRect(clientPoint))
		return CTabCtrl::OnMouseWheel(nFlags, zDelta, point);

	if (zDelta == 0)
		return TRUE;

	m_wheelRemainder += zDelta;
	const int steps = m_wheelRemainder / WHEEL_DELTA;
	if (steps == 0)
		return TRUE;
	m_wheelRemainder -= steps * WHEEL_DELTA;

	const int tabCount = GetItemCount();
	const int current = GetCurSel();
	if (current < 0 || tabCount <= 0)
		return TRUE;

	const int next = max(0, min(tabCount - 1, current - steps));
	if (next == current)
		return TRUE;

	SetCurSel(next);
	m_lastEnsuredSel = next;
	EnsureTabVisible(next);
	HideOverflowButtons();
	Invalidate(FALSE);

	CWnd* pParent = GetParent();
	if (pParent)
	{
		NMHDR notification = {};
		notification.hwndFrom = GetSafeHwnd();
		notification.idFrom = GetDlgCtrlID();
		notification.code = TCN_SELCHANGE;
		pParent->SendMessage(WM_NOTIFY, notification.idFrom,
			reinterpret_cast<LPARAM>(&notification));
	}
	return TRUE;
}

void CModernTabCtrl::OnMouseLeave()
{
	m_bTracking = FALSE;

	// 清除悬停状态
	if (m_hoverIndex != -1 || m_closeHoverIndex != -1)
	{
		m_hoverIndex = -1;
		m_closeHoverIndex = -1;
		UpdateHoverToolTip();
		Invalidate(FALSE);
	}

	CTabCtrl::OnMouseLeave();
}

void CModernTabCtrl::OnLButtonDown(UINT nFlags, CPoint point)
{
	ResetDragState();
	// 检查是否点击了关闭按钮
	int closeIndex = -1;
	if (m_bCloseButtonEnabled)
	{
		for (int i = 0; i < GetItemCount(); ++i)
		{
			if (IsCloseButtonVisible(i) && GetCloseButtonRect(i).PtInRect(point))
			{
				closeIndex = i;
				break;
			}
		}
	}
	if (closeIndex != -1)
	{
		m_hoverIndex = -1;
		m_closeHoverIndex = -1;
		UpdateHoverToolTip();
		if (CWnd* pParent = GetParent())
			pParent->SendMessage(WM_TAB_CLOSE_REQUEST, closeIndex, 0);
		return;
	}
	m_tabPressedIndex = HitTestTab(point);
	::GetCursorPos(&m_tabDragStartScreen);
	if (CWnd* pParent = GetParent())
		pParent->GetWindowRect(&m_tabDragStartWindow);
	else
		m_tabDragStartWindow.SetRectEmpty();
	m_tabDragArmed = TRUE;
	SetCapture();
}

void CModernTabCtrl::OnLButtonUp(UINT nFlags, CPoint point)
{
	const BOOL wasDragging = m_tabDragging;
	const int pressedIndex = m_tabPressedIndex;
	ResetDragState();
	if (::GetCapture() == GetSafeHwnd())
		::ReleaseCapture();
	if (!wasDragging && pressedIndex >= 0)
	{
		if (HitTestTab(point) == pressedIndex)
		{
			SetCurSel(pressedIndex);
			m_lastEnsuredSel = pressedIndex;
			EnsureTabVisible(pressedIndex);
			Invalidate(FALSE);
			if (CWnd* pParent = GetParent())
			{
				NMHDR notification = {};
				notification.hwndFrom = GetSafeHwnd();
				notification.idFrom = GetDlgCtrlID();
				notification.code = TCN_SELCHANGE;
				pParent->SendMessage(WM_NOTIFY, notification.idFrom,
					reinterpret_cast<LPARAM>(&notification));
			}
		}
	}
}

void CModernTabCtrl::OnMButtonDown(UINT /*nFlags*/, CPoint point)
{
	const int tabIndex = HitTestTab(point);
	if (tabIndex >= 0)
	{
		m_middlePressedIndex = tabIndex;
		SetCapture();
	}
}

void CModernTabCtrl::OnMButtonUp(UINT /*nFlags*/, CPoint point)
{
	const int pressedIndex = m_middlePressedIndex;
	m_middlePressedIndex = -1;
	if (::GetCapture() == GetSafeHwnd())
		::ReleaseCapture();
	if (pressedIndex >= 0 && HitTestTab(point) == pressedIndex)
	{
		m_hoverIndex = -1;
		m_closeHoverIndex = -1;
		UpdateHoverToolTip();
		if (CWnd* pParent = GetParent())
			pParent->SendMessage(WM_TAB_CLOSE_REQUEST, pressedIndex, 0);
	}
}

void CModernTabCtrl::OnRButtonDown(UINT /*nFlags*/, CPoint /*point*/)
{
	// 拦截默认右键按下行为，统一在右键抬起时弹出标签菜单
}

void CModernTabCtrl::OnRButtonUp(UINT /*nFlags*/, CPoint point)
{
	const int tabIndex = HitTestTab(point);
	if (tabIndex >= 0)
	{
		CPoint screenPt = point;
		ClientToScreen(&screenPt);
		if (CWnd* pParent = GetParent())
		{
			pParent->SendMessage(WM_TAB_CONTEXT_MENU, tabIndex,
				MAKELPARAM(screenPt.x, screenPt.y));
		}
	}
}

void CModernTabCtrl::OnLButtonDblClk(UINT nFlags, CPoint point)
{
	if (HitTestTab(point) < 0)
	{
		if (CWnd* pParent = GetParent())
		{
			pParent->SendMessage(WM_SYSCOMMAND,
				pParent->IsZoomed() ? SC_RESTORE : SC_MAXIMIZE);
		}
		return;
	}
	CTabCtrl::OnLButtonDblClk(nFlags, point);
}

void CModernTabCtrl::OnCaptureChanged(CWnd* pWnd)
{
	if (pWnd != this)
		ResetDragState();
	CTabCtrl::OnCaptureChanged(pWnd);
}

void CModernTabCtrl::OnCancelMode()
{
	ResetDragState();
	CTabCtrl::OnCancelMode();
}
