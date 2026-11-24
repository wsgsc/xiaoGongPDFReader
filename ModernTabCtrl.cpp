#include "pch.h"
#include <commctrl.h>
#include "ModernTabCtrl.h"

IMPLEMENT_DYNAMIC(CModernTabCtrl, CTabCtrl)

BEGIN_MESSAGE_MAP(CModernTabCtrl, CTabCtrl)
	ON_WM_PAINT()
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSEWHEEL()
	ON_WM_MOUSELEAVE()
	ON_WM_LBUTTONDOWN()
	ON_WM_LBUTTONUP()
	ON_WM_LBUTTONDBLCLK()
	ON_WM_CAPTURECHANGED()
	ON_WM_CANCELMODE()
	ON_WM_ERASEBKGND()
	ON_WM_DESTROY()
END_MESSAGE_MAP()

CModernTabCtrl::CModernTabCtrl()
	: m_hoverIndex(-1)
	, m_closeHoverIndex(-1)
	, m_bCloseButtonEnabled(TRUE)
	, m_bTracking(FALSE)
	, m_tabDragArmed(FALSE)
	, m_tabDragging(FALSE)
	, m_tabPressedIndex(-1)
	, m_tabDragStartScreen(0, 0)
	, m_tabDragStartWindow(0, 0, 0, 0)
	, m_scrollOffset(0)
	, m_lastEnsuredSel(-1)
	, m_wheelRemainder(0)
	, m_tabHeight(36)
	, m_normalColor(RGB(240, 240, 240))      // 浅灰色背景
	, m_hoverColor(RGB(230, 230, 230))       // 稍深的灰色（悬停）
	, m_selectedColor(RGB(255, 255, 255))    // 白色（选中）
{
	// 创建标准字体
	VERIFY(m_font.CreateFont(
		16,                        // 字体高度
		0,                         // 字体宽度
		0,                         // 文本倾斜角度
		0,                         // 基线倾斜角度
		FW_NORMAL,                 // 字体粗细
		FALSE,                     // 斜体
		FALSE,                     // 下划线
		0,                         // 删除线
		DEFAULT_CHARSET,           // 字符集
		OUT_DEFAULT_PRECIS,        // 输出精度
		CLIP_DEFAULT_PRECIS,       // 裁剪精度
		DEFAULT_QUALITY,           // 输出质量
		DEFAULT_PITCH | FF_SWISS,  // 字体间距和族
		_T("微软雅黑")));            // 字体名称
	VERIFY(m_compactFont.CreateFont(
		14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, 0,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("微软雅黑")));

}

CModernTabCtrl::~CModernTabCtrl()
{
	// 双重保护：如果OnDestroy没有清理，这里再清理一次
	// 如果已经删除了，GetSafeHandle()会返回NULL，不会重复删除
	if (m_font.GetSafeHandle())
	{
		m_font.DeleteObject();
	}
	if (m_compactFont.GetSafeHandle())
	{
		m_compactFont.DeleteObject();
	}
}

void CModernTabCtrl::OnDestroy()
{
	// 在窗口销毁时清理GDI资源（推荐做法）
	if (m_font.GetSafeHandle())
	{
		m_font.DeleteObject();
	}
	if (m_compactFont.GetSafeHandle())
	{
		m_compactFont.DeleteObject();
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

	const bool compact = GetTabRect(index).Width() < 124;
	// 计算关闭按钮区域（如果启用）
	CRect textRect = rect;
	textRect.left += compact ? 14 : 32;
	if (m_bCloseButtonEnabled)
	{
		textRect.right = GetCloseButtonRect(index).left - (compact ? 1 : 5);
	}

	// 绘制文本
	const int iconSize = compact ? 10 : 16;
	const int iconLeft = rect.left + (compact ? 2 : 8);
	CRect iconRect(iconLeft, rect.top + (rect.Height() - iconSize) / 2,
		iconLeft + iconSize, rect.top + (rect.Height() + iconSize) / 2);
	pDC->FillSolidRect(iconRect, RGB(29, 124, 243));
	CFont* pOldFont = pDC->SelectObject(compact ? &m_compactFont : &m_font);
	pDC->SetBkMode(TRANSPARENT);
	pDC->SetTextColor(RGB(255, 255, 255));
	pDC->DrawText(_T("P"), iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	pDC->SetTextColor(dark ? RGB(226, 231, 235) : RGB(35, 45, 57));
	pDC->SetBkMode(TRANSPARENT);

	CString displayText = szText;
	pDC->DrawText(displayText, textRect,
		DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
	pDC->SelectObject(pOldFont);

	// 绘制关闭按钮
	if (IsCloseButtonVisible(index))
	{
		CRect closeRect = GetCloseButtonRect(index);
		BOOL isCloseHovered = (index == m_closeHoverIndex);
		DrawCloseButton(pDC, closeRect, isCloseHovered);
	}
}

void CModernTabCtrl::DrawCloseButton(CDC* pDC, CRect rect, BOOL isHovered)
{
	// 绘制关闭按钮背景
	if (isHovered)
	{
		CBrush brush;
		brush.CreateSolidBrush(RGB(232, 17, 35)); // 红色背景（悬停时）
		pDC->FillRect(rect, &brush);
		// 让CBrush析构函数自动清理
	}

	// 绘制 × 符号
	CPen pen;
	pen.CreatePen(PS_SOLID, 2, isHovered ? RGB(255, 255, 255) :
		(GetRValue(m_normalColor) < 100 ? RGB(190, 198, 205) : RGB(100, 100, 100)));
	CPen* pOldPen = pDC->SelectObject(&pen);

	int margin = rect.Width() <= 10 ? 2 : 4;
	pDC->MoveTo(rect.left + margin, rect.top + margin);
	pDC->LineTo(rect.right - margin, rect.bottom - margin);
	pDC->MoveTo(rect.right - margin, rect.top + margin);
	pDC->LineTo(rect.left + margin, rect.bottom - margin);

	pDC->SelectObject(pOldPen);
	// 让CPen析构函数自动清理
}

CRect CModernTabCtrl::GetCloseButtonRect(int index)
{
	CRect tabRect = GetTabRect(index);
	if (tabRect.Width() < 124)
	{
		const int top = tabRect.top + (tabRect.Height() - 10) / 2;
		return CRect(tabRect.right - 14, top, tabRect.right - 4, top + 10);
	}

	// 关闭按钮位于标签右侧
	CRect closeRect;
	closeRect.left = tabRect.right - 22;
	closeRect.right = tabRect.right - 6;
	closeRect.top = tabRect.top + (tabRect.Height() - 16) / 2;
	closeRect.bottom = closeRect.top + 16;

	return closeRect;
}

BOOL CModernTabCtrl::IsCloseButtonVisible(int index)
{
	return m_bCloseButtonEnabled && (index == GetCurSel() || index == m_hoverIndex);
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

	const int minTabWidth = 60;
	const int totalWidth = tabCount * minTabWidth;
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

	const int maxTabWidth = 220;
	const int minTabWidth = 60;
	int tabWidth = availableWidth / tabCount;
	if (tabWidth > maxTabWidth)
		tabWidth = maxTabWidth;
	if (tabWidth < minTabWidth)
		tabWidth = minTabWidth;

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
	int availableWidth = clientRect.Width();

	const int maxTabWidth = 220;
	const int minTabWidth = 60;

	int tabWidth = availableWidth / tabCount;
	if (tabWidth > maxTabWidth)
		tabWidth = maxTabWidth;
	if (tabWidth < minTabWidth)
		tabWidth = minTabWidth;

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
				m_tabDragging = TRUE;
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
						cursor.y - m_tabDragStartWindow.top - 21);
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

	// 如果悬停状态改变，重绘
	if (oldHoverIndex != m_hoverIndex || oldCloseHoverIndex != m_closeHoverIndex)
	{
		Invalidate();
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
		Invalidate();
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
		// 发送自定义消息通知父窗口关闭标签页
		// 使用WM_APP + 自定义偏移
		if (CWnd* pParent = GetParent())
			pParent->SendMessage(WM_APP + 100, closeIndex, 0);
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
