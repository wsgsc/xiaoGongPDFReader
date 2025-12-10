#include "pch.h"
#include "ModernTabCtrl.h"

IMPLEMENT_DYNAMIC(CModernTabCtrl, CTabCtrl)

BEGIN_MESSAGE_MAP(CModernTabCtrl, CTabCtrl)
	ON_WM_PAINT()
	ON_WM_MOUSEMOVE()
	ON_WM_MOUSELEAVE()
	ON_WM_LBUTTONDOWN()
	ON_WM_ERASEBKGND()
	ON_WM_DESTROY()
END_MESSAGE_MAP()

CModernTabCtrl::CModernTabCtrl()
	: m_hoverIndex(-1)
	, m_closeHoverIndex(-1)
	, m_bCloseButtonEnabled(TRUE)
	, m_bTracking(FALSE)
	, m_tabHeight(36)
	, m_normalColor(RGB(240, 240, 240))      // 浅灰色背景
	, m_hoverColor(RGB(230, 230, 230))       // 稍深的灰色（悬停）
	, m_selectedColor(RGB(255, 255, 255))    // 白色（选中）
{
	// 创建标准字体
	VERIFY(m_font.CreateFont(
		15,                        // 字体高度
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

	// 创建粗体字体（选中时使用）
	VERIFY(m_boldFont.CreateFont(
		15,                        // 字体高度
		0,                         // 字体宽度
		0,                         // 文本倾斜角度
		0,                         // 基线倾斜角度
		FW_BOLD,                   // 字体粗细（加粗）
		FALSE,                     // 斜体
		FALSE,                     // 下划线
		0,                         // 删除线
		DEFAULT_CHARSET,           // 字符集
		OUT_DEFAULT_PRECIS,        // 输出精度
		CLIP_DEFAULT_PRECIS,       // 裁剪精度
		DEFAULT_QUALITY,           // 输出质量
		DEFAULT_PITCH | FF_SWISS,  // 字体间距和族
		_T("微软雅黑")));            // 字体名称
}

CModernTabCtrl::~CModernTabCtrl()
{
	// 双重保护：如果OnDestroy没有清理，这里再清理一次
	// 如果已经删除了，GetSafeHandle()会返回NULL，不会重复删除
	if (m_font.GetSafeHandle())
	{
		m_font.DeleteObject();
	}
	if (m_boldFont.GetSafeHandle())
	{
		m_boldFont.DeleteObject();
	}
}

void CModernTabCtrl::OnDestroy()
{
	// 在窗口销毁时清理GDI资源（推荐做法）
	if (m_font.GetSafeHandle())
	{
		m_font.DeleteObject();
	}
	if (m_boldFont.GetSafeHandle())
	{
		m_boldFont.DeleteObject();
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
		CRect rect;
		GetWindowRect(&rect);
		GetParent()->ScreenToClient(&rect);
		rect.bottom = rect.top + m_tabHeight;
		MoveWindow(&rect);
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

BOOL CModernTabCtrl::OnEraseBkgnd(CDC* pDC)
{
	// 自己处理背景绘制，避免闪烁
	return TRUE;
}

void CModernTabCtrl::OnPaint()
{
	CPaintDC dcPaint(this);

	CRect clientRect;
	GetClientRect(&clientRect);

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
	memDC.FillSolidRect(clientRect, RGB(245, 245, 245));

	// 获取标签数量
	int tabCount = GetItemCount();
	int curSel = GetCurSel();

	// 先绘制未选中的标签页
	for (int i = 0; i < tabCount; i++)
	{
		if (i != curSel)
		{
			CRect tabRect = GetTabRect(i);
			BOOL isHovered = (i == m_hoverIndex);
			DrawTab(&memDC, i, tabRect, FALSE, isHovered);
		}
	}

	// 最后绘制选中的标签页（在最上层）
	if (curSel >= 0 && curSel < tabCount)
	{
		CRect tabRect = GetTabRect(curSel);
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
	// 根据状态选择背景色
	COLORREF bgColor;
	if (isSelected)
		bgColor = m_selectedColor;
	else if (isHovered)
		bgColor = m_hoverColor;
	else
		bgColor = m_normalColor;

	// 绘制圆角矩形背景
	CPen pen;
	pen.CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
	CPen* pOldPen = pDC->SelectObject(&pen);

	CBrush brush;
	brush.CreateSolidBrush(bgColor);
	CBrush* pOldBrush = pDC->SelectObject(&brush);

	// 调整矩形以创建间隔
	rect.DeflateRect(2, 2, 2, 0);

	// 绘制圆角矩形（上方圆角）
	pDC->RoundRect(rect, CPoint(6, 6));

	// 恢复原来的GDI对象（让MFC自动清理pen和brush）
	pDC->SelectObject(pOldBrush);
	pDC->SelectObject(pOldPen);

	// 如果是选中状态，绘制底部边框覆盖
	if (isSelected)
	{
		CPen whitePen;
		whitePen.CreatePen(PS_SOLID, 2, m_selectedColor);
		CPen* pOldWhitePen = pDC->SelectObject(&whitePen);
		pDC->MoveTo(rect.left, rect.bottom - 1);
		pDC->LineTo(rect.right, rect.bottom - 1);
		pDC->SelectObject(pOldWhitePen);
		// 让CPen析构函数自动清理
	}

	// 获取标签文本
	TCHAR szText[256];
	TCITEM tci;
	tci.mask = TCIF_TEXT;
	tci.pszText = szText;
	tci.cchTextMax = 256;
	GetItem(index, &tci);

	// 计算关闭按钮区域（如果启用）
	CRect textRect = rect;
	if (m_bCloseButtonEnabled)
	{
		textRect.right -= 20; // 为关闭按钮留出空间
	}

	// 绘制文本
	CFont* pOldFont = pDC->SelectObject(isSelected ? &m_boldFont : &m_font);
	pDC->SetTextColor(RGB(50, 50, 50));
	pDC->SetBkMode(TRANSPARENT);

	// 截断过长的文本
	CString displayText = szText;
	CSize textSize = pDC->GetTextExtent(displayText);
	if (textSize.cx > textRect.Width() - 10)
	{
		// 逐字符截断直到适合
		int len = displayText.GetLength();
		while (len > 3 && pDC->GetTextExtent(displayText.Left(len) + _T("...")).cx > textRect.Width() - 10)
		{
			len--;
		}
		displayText = displayText.Left(len) + _T("...");
	}

	pDC->DrawText(displayText, textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	pDC->SelectObject(pOldFont);

	// 绘制关闭按钮
	if (m_bCloseButtonEnabled)
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
	pen.CreatePen(PS_SOLID, 2, isHovered ? RGB(255, 255, 255) : RGB(100, 100, 100));
	CPen* pOldPen = pDC->SelectObject(&pen);

	int margin = 4;
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

	// 关闭按钮位于标签右侧
	CRect closeRect;
	closeRect.left = tabRect.right - 22;
	closeRect.right = tabRect.right - 6;
	closeRect.top = tabRect.top + (tabRect.Height() - 16) / 2;
	closeRect.bottom = closeRect.top + 16;

	return closeRect;
}

CRect CModernTabCtrl::GetTabRect(int index)
{
	CRect rect;
	GetItemRect(index, &rect);
	return rect;
}

void CModernTabCtrl::OnMouseMove(UINT nFlags, CPoint point)
{
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
			if (m_bCloseButtonEnabled)
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
	// 检查是否点击了关闭按钮
	if (m_bCloseButtonEnabled && m_closeHoverIndex != -1)
	{
		// 发送自定义消息通知父窗口关闭标签页
		// 使用WM_APP + 自定义偏移
		GetParent()->SendMessage(WM_APP + 100, m_closeHoverIndex, 0);
		return;
	}

	CTabCtrl::OnLButtonDown(nFlags, point);
}
