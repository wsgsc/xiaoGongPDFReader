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
	ON_WM_MOUSELEAVE()
	ON_WM_RBUTTONUP()
	ON_WM_ERASEBKGND()
	ON_WM_VSCROLL()
	ON_WM_HSCROLL()
	ON_WM_PAINT()
END_MESSAGE_MAP()

namespace
{
	void fillRoundRectGdiPlus(Gdiplus::Graphics& graphics, const CRect& rect,
		float radius, COLORREF fillColor, COLORREF borderColor, bool drawBorder)
	{
		if (rect.Width() <= 1 || rect.Height() <= 1)
			return;

		const float clampedR = max(0.0f, min(radius, min(rect.Width(), rect.Height()) * 0.5f));
		const float d = clampedR * 2.0f;
		Gdiplus::RectF rf(
			static_cast<Gdiplus::REAL>(rect.left) + 0.5f,
			static_cast<Gdiplus::REAL>(rect.top) + 0.5f,
			static_cast<Gdiplus::REAL>(rect.Width() - 1),
			static_cast<Gdiplus::REAL>(rect.Height() - 1));

		Gdiplus::SolidBrush brush(Gdiplus::Color(255,
			GetRValue(fillColor), GetGValue(fillColor), GetBValue(fillColor)));
		if (d <= 0.5f)
		{
			graphics.FillRectangle(&brush, rf);
			if (drawBorder)
			{
				Gdiplus::Pen pen(Gdiplus::Color(255,
					GetRValue(borderColor), GetGValue(borderColor), GetBValue(borderColor)), 1.0f);
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

		graphics.FillPath(&brush, &path);
		if (drawBorder)
		{
			Gdiplus::Pen pen(Gdiplus::Color(255,
				GetRValue(borderColor), GetGValue(borderColor), GetBValue(borderColor)), 1.0f);
			graphics.DrawPath(&pen, &path);
		}
	}
}

CPDFViewCtrl::~CPDFViewCtrl()
{
	delete m_pWelcomeBitmap;
	if (m_pWelcomeStream)
		m_pWelcomeStream->Release();
	if (m_welcomeTitleFont.GetSafeHandle())
		m_welcomeTitleFont.DeleteObject();
	if (m_welcomeBtnFont.GetSafeHandle())
		m_welcomeBtnFont.DeleteObject();
	if (m_welcomeSectionFont.GetSafeHandle())
		m_welcomeSectionFont.DeleteObject();
	if (m_welcomeItemTitleFont.GetSafeHandle())
		m_welcomeItemTitleFont.DeleteObject();
	if (m_welcomeItemPathFont.GetSafeHandle())
		m_welcomeItemPathFont.DeleteObject();
	if (m_welcomeBadgeFont.GetSafeHandle())
		m_welcomeBadgeFont.DeleteObject();
}

bool CPDFViewCtrl::isWelcomePageActive() const
{
	return !m_hDisplayBitmap && m_pParentDlg && m_pParentDlg->m_documents.empty();
}

void CPDFViewCtrl::ensureWelcomeFonts()
{
	auto createUiFont = [](CFont& font, int height, int weight, LPCTSTR face)
	{
		if (font.GetSafeHandle())
			return;
		font.CreateFont(height, 0, 0, 0, weight, FALSE, FALSE, 0,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
			CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
	};
	createUiFont(m_welcomeTitleFont, -24, FW_BOLD, _T("微软雅黑"));
	createUiFont(m_welcomeBtnFont, -15, FW_MEDIUM, _T("微软雅黑"));
	createUiFont(m_welcomeSectionFont, -13, FW_MEDIUM, _T("微软雅黑"));
	createUiFont(m_welcomeItemTitleFont, -14, FW_MEDIUM, _T("微软雅黑"));
	createUiFont(m_welcomeItemPathFont, -12, FW_NORMAL, _T("微软雅黑"));
	createUiFont(m_welcomeBadgeFont, -12, FW_BOLD, _T("Segoe UI"));
}

void CPDFViewCtrl::hitTestWelcomePage(CPoint point, WelcomeHitTarget& outTarget, int& outIndex) const
{
	outTarget = WELCOME_HIT_NONE;
	outIndex = -1;
	if (!isWelcomePageActive())
		return;

	if (!m_openBtnRect.IsRectEmpty() && m_openBtnRect.PtInRect(point))
	{
		outTarget = WELCOME_HIT_OPEN_BTN;
		return;
	}
	if (!m_clearRecentRect.IsRectEmpty() && m_clearRecentRect.PtInRect(point))
	{
		outTarget = WELCOME_HIT_CLEAR_BTN;
		return;
	}
	for (size_t i = 0; i < m_recentRowRects.size(); ++i)
	{
		if (i < m_recentRemoveRects.size() &&
			!m_recentRemoveRects[i].IsRectEmpty() &&
			m_recentRemoveRects[i].PtInRect(point))
		{
			outTarget = WELCOME_HIT_RECENT_REMOVE;
			outIndex = static_cast<int>(i);
			return;
		}
		if (m_recentRowRects[i].PtInRect(point))
		{
			outTarget = WELCOME_HIT_RECENT_ROW;
			outIndex = static_cast<int>(i);
			return;
		}
	}
}

void CPDFViewCtrl::DrawWelcomeImage(CDC& dc, const CRect& clientRect)
{
	m_openBtnRect.SetRectEmpty();
	m_clearRecentRect.SetRectEmpty();
	m_recentRowRects.clear();
	m_recentRemoveRects.clear();

	if (clientRect.Width() <= 40 || clientRect.Height() <= 80)
		return;

	ensureWelcomeFonts();

	if (!m_pWelcomeBitmap)
	{
		HINSTANCE hInstance = AfxGetInstanceHandle();
		HRSRC hResource = ::FindResource(hInstance, MAKEINTRESOURCE(IDR_WELCOME_PNG), RT_RCDATA);
		if (hResource)
		{
			HGLOBAL hData = ::LoadResource(hInstance, hResource);
			DWORD dataSize = ::SizeofResource(hInstance, hResource);
			const BYTE* pData = hData ? static_cast<const BYTE*>(::LockResource(hData)) : nullptr;
			if (pData && dataSize > 0)
			{
				IStream* pStream = ::SHCreateMemStream(pData, dataSize);
				if (pStream)
				{
					Gdiplus::Bitmap* pBitmap = Gdiplus::Bitmap::FromStream(pStream, FALSE);
					if (pBitmap && pBitmap->GetLastStatus() == Gdiplus::Ok &&
						pBitmap->GetWidth() > 0 && pBitmap->GetHeight() > 0)
					{
						m_pWelcomeBitmap = pBitmap;
						m_pWelcomeStream = pStream;
					}
					else
					{
						delete pBitmap;
						pStream->Release();
					}
				}
			}
		}
	}

	const bool dark = (m_readingMode == READING_NIGHT);
	const std::vector<CString>& recentFiles = m_pParentDlg->m_recentFiles;
	const bool hasRecent = !recentFiles.empty();

	const int openBtnWidth = 206;
	const int openBtnHeight = 40;
	const int rowHeight = 48;
	const int headerHeight = 36;
	const int cardBottomPad = 8;

	// 计算英雄插画与最近文件卡片的自适应高度，确保小窗口与大屏均垂直居中且不重叠
	int heroHeight = hasRecent ?
		max(116, min(205, static_cast<int>(clientRect.Height() * 0.30))) :
		max(160, min(340, static_cast<int>(clientRect.Height() * 0.50)));

	int visibleRecentCount = 0;
	if (hasRecent)
	{
		const int reservedHeight = heroHeight + 12 + openBtnHeight + 18 + headerHeight + cardBottomPad + 32;
		const int maxRowsByHeight = max(1, (clientRect.Height() - reservedHeight) / rowHeight);
		visibleRecentCount = min(static_cast<int>(recentFiles.size()), min(6, maxRowsByHeight));
	}

	const int cardHeight = (visibleRecentCount > 0) ?
		(headerHeight + visibleRecentCount * rowHeight + cardBottomPad) : 0;
	const int totalBlockHeight = heroHeight + 12 + openBtnHeight +
		(cardHeight > 0 ? (18 + cardHeight) : 0);
	int curY = max(12, (clientRect.Height() - totalBlockHeight) / 2);

	Gdiplus::Graphics graphics(dc.GetSafeHdc());
	graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
	graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

	// 1. 绘制品牌插画与标题（夜间模式下裁切插画主体并用浅色绘制标题，保证深色背景清晰可读）
	if (m_pWelcomeBitmap)
	{
		const int bmpW = static_cast<int>(m_pWelcomeBitmap->GetWidth());
		const int bmpH = static_cast<int>(m_pWelcomeBitmap->GetHeight());
		if (dark && bmpW >= 1200 && bmpH >= 800)
		{
			const int srcX = 485;
			const int srcY = 56;
			const int srcW = 576;
			const int srcH = 610;
			const int mascotMaxH = max(64, heroHeight - 58);
			const double scale = min(static_cast<double>(clientRect.Width() - 40) / srcW,
				static_cast<double>(mascotMaxH) / srcH);
			const int drawW = max(1, static_cast<int>(srcW * scale));
			const int drawH = max(1, static_cast<int>(srcH * scale));
			graphics.DrawImage(m_pWelcomeBitmap,
				Gdiplus::Rect((clientRect.Width() - drawW) / 2, curY, drawW, drawH),
				srcX, srcY, srcW, srcH, Gdiplus::UnitPixel);

			dc.SetBkMode(TRANSPARENT);
			CFont* pOldFont = dc.SelectObject(&m_welcomeTitleFont);
			dc.SetTextColor(RGB(230, 235, 240));
			CRect titleRect(20, curY + drawH + 4, clientRect.Width() - 20, curY + drawH + 34);
			dc.DrawText(_T("小龚PDF阅读器"), &titleRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

			dc.SelectObject(&m_welcomeSectionFont);
			dc.SetTextColor(RGB(154, 164, 176));
			CRect subRect(20, curY + drawH + 34, clientRect.Width() - 20, curY + heroHeight);
			dc.DrawText(_T("将 PDF 文件拖到此处，或点击下方按钮打开"), &subRect,
				DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			dc.SelectObject(pOldFont);
		}
		else
		{
			// 裁掉原图外圈多余透明留白，使插画与文字在紧凑高度下依然饱满清晰
			int srcX = 0;
			int srcY = 0;
			int srcW = bmpW;
			int srcH = bmpH;
			if (bmpW >= 1200 && bmpH >= 800)
			{
				srcX = 210;
				srcY = 50;
				srcW = bmpW - 420;
				srcH = bmpH - 90;
			}
			const double scale = min(1.0, min(
				static_cast<double>(clientRect.Width() - 32) / srcW,
				static_cast<double>(heroHeight) / srcH));
			const int drawW = max(1, static_cast<int>(srcW * scale));
			const int drawH = max(1, static_cast<int>(srcH * scale));
			graphics.DrawImage(m_pWelcomeBitmap,
				Gdiplus::Rect((clientRect.Width() - drawW) / 2,
					curY + (heroHeight - drawH) / 2, drawW, drawH),
				srcX, srcY, srcW, srcH, Gdiplus::UnitPixel);
		}
	}
	curY += heroHeight + 12;

	// 2. 绘制主操作按钮：“打开 PDF 文件 (Ctrl+O)”
	m_openBtnRect.SetRect(
		(clientRect.Width() - openBtnWidth) / 2,
		curY,
		(clientRect.Width() + openBtnWidth) / 2,
		curY + openBtnHeight);
	const bool openHovered = (m_welcomeHoverTarget == WELCOME_HIT_OPEN_BTN);
	const COLORREF openFill = openHovered ? RGB(21, 105, 214) : RGB(29, 124, 243);
	fillRoundRectGdiPlus(graphics, m_openBtnRect, 8.0f, openFill, openFill, false);

	dc.SetBkMode(TRANSPARENT);
	CFont* pOldFont = dc.SelectObject(&m_welcomeBtnFont);
	dc.SetTextColor(RGB(255, 255, 255));
	dc.DrawText(_T("打开 PDF 文件  (Ctrl+O)"), &m_openBtnRect,
		DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	curY += openBtnHeight;

	// 3. 绘制“最近打开”文件面板
	if (visibleRecentCount > 0)
	{
		curY += 18;
		const int cardWidth = min(640, max(300, clientRect.Width() - 48));
		const int cardLeft = (clientRect.Width() - cardWidth) / 2;
		CRect cardRect(cardLeft, curY, cardLeft + cardWidth, curY + cardHeight);

		const COLORREF cardBg = dark ? RGB(39, 43, 48) : RGB(255, 255, 255);
		const COLORREF cardBorder = dark ? RGB(62, 68, 76) : RGB(214, 226, 245);
		fillRoundRectGdiPlus(graphics, cardRect, 10.0f, cardBg, cardBorder, true);

		// 标题栏：“最近打开” + “清空记录”
		CRect headerTitleRect(cardRect.left + 16, cardRect.top + 4,
			cardRect.right - 96, cardRect.top + headerHeight);
		dc.SelectObject(&m_welcomeSectionFont);
		dc.SetTextColor(dark ? RGB(166, 176, 188) : RGB(95, 108, 125));
		dc.DrawText(_T("最近打开"), &headerTitleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

		m_clearRecentRect.SetRect(cardRect.right - 84, cardRect.top + 6,
			cardRect.right - 12, cardRect.top + headerHeight - 4);
		const bool clearHovered = (m_welcomeHoverTarget == WELCOME_HIT_CLEAR_BTN);
		if (clearHovered)
		{
			fillRoundRectGdiPlus(graphics, m_clearRecentRect, 5.0f,
				dark ? RGB(56, 62, 70) : RGB(236, 243, 254), 0, false);
		}
		dc.SetTextColor(clearHovered ?
			(dark ? RGB(130, 182, 255) : RGB(25, 108, 220)) :
			(dark ? RGB(145, 155, 166) : RGB(118, 130, 145)));
		dc.DrawText(_T("清空记录"), &m_clearRecentRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

		CWinApp* pApp = AfxGetApp();
		m_recentRowRects.reserve(visibleRecentCount);
		m_recentRemoveRects.reserve(visibleRecentCount);

		for (int i = 0; i < visibleRecentCount; ++i)
		{
			const CString& fullPath = recentFiles[i];
			const int rowTop = cardRect.top + headerHeight + i * rowHeight;
			CRect rowRect(cardRect.left + 8, rowTop, cardRect.right - 8, rowTop + rowHeight - 2);
			m_recentRowRects.push_back(rowRect);

			const bool rowHovered =
				((m_welcomeHoverTarget == WELCOME_HIT_RECENT_ROW ||
				  m_welcomeHoverTarget == WELCOME_HIT_RECENT_REMOVE) &&
				 m_welcomeHoverIndex == i);
			const bool removeHovered =
				(m_welcomeHoverTarget == WELCOME_HIT_RECENT_REMOVE && m_welcomeHoverIndex == i);

			if (rowHovered)
			{
				fillRoundRectGdiPlus(graphics, rowRect, 7.0f,
					dark ? RGB(52, 59, 67) : RGB(236, 244, 255), 0, false);
			}

			// 左侧蓝色圆角 P 角标
			CRect iconRect(rowRect.left + 10, rowRect.top + (rowRect.Height() - 26) / 2,
				rowRect.left + 36, rowRect.top + (rowRect.Height() + 26) / 2);
			fillRoundRectGdiPlus(graphics, iconRect, 5.0f, RGB(29, 124, 243), 0, false);
			dc.SelectObject(&m_welcomeBadgeFont);
			dc.SetTextColor(RGB(255, 255, 255));
			dc.DrawText(_T("P"), &iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

			// 右侧移除按钮（仅悬停该行时显示）
			CRect removeRect(0, 0, 0, 0);
			int rightLimit = rowRect.right - 12;
			if (rowHovered)
			{
				const int btnSize = 22;
				removeRect.SetRect(rowRect.right - 10 - btnSize,
					rowRect.top + (rowRect.Height() - btnSize) / 2,
					rowRect.right - 10,
					rowRect.top + (rowRect.Height() + btnSize) / 2);
				if (removeHovered)
				{
					fillRoundRectGdiPlus(graphics, removeRect, 4.0f,
						RGB(232, 38, 51), 0, false);
				}
				const COLORREF xColor = removeHovered ? RGB(255, 255, 255) :
					(dark ? RGB(180, 188, 198) : RGB(110, 122, 136));
				Gdiplus::Pen xPen(Gdiplus::Color(255,
					GetRValue(xColor), GetGValue(xColor), GetBValue(xColor)), 1.6f);
				graphics.DrawLine(&xPen,
					static_cast<Gdiplus::REAL>(removeRect.left + 6),
					static_cast<Gdiplus::REAL>(removeRect.top + 6),
					static_cast<Gdiplus::REAL>(removeRect.right - 6),
					static_cast<Gdiplus::REAL>(removeRect.bottom - 6));
				graphics.DrawLine(&xPen,
					static_cast<Gdiplus::REAL>(removeRect.right - 6),
					static_cast<Gdiplus::REAL>(removeRect.top + 6),
					static_cast<Gdiplus::REAL>(removeRect.left + 6),
					static_cast<Gdiplus::REAL>(removeRect.bottom - 6));
				rightLimit = removeRect.left - 8;
			}
			m_recentRemoveRects.push_back(removeRect);

			// 读取上次阅读页码并绘制胶囊标签
			const CString stateKey = m_pParentDlg->ReadingStateKey(fullPath);
			const int savedPage = pApp ? static_cast<int>(pApp->GetProfileInt(
				_T("ReadingPosition"), stateKey + _T("_Page"), -1)) : -1;
			if (savedPage >= 0 && cardWidth >= 400)
			{
				CString pageLabel;
				pageLabel.Format(_T("第 %d 页"), savedPage + 1);
				dc.SelectObject(&m_welcomeItemPathFont);
				const int pillWidth = max(54, dc.GetTextExtent(pageLabel).cx + 16);
				CRect pillRect(rightLimit - pillWidth,
					rowRect.top + (rowRect.Height() - 22) / 2,
					rightLimit,
					rowRect.top + (rowRect.Height() + 22) / 2);
				fillRoundRectGdiPlus(graphics, pillRect, 11.0f,
					dark ? RGB(47, 66, 90) : RGB(232, 242, 254), 0, false);
				dc.SetTextColor(dark ? RGB(138, 186, 255) : RGB(26, 115, 232));
				dc.DrawText(pageLabel, &pillRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
				rightLimit = pillRect.left - 10;
			}

			// 拆分文件名与所在目录
			CString fileName = fullPath;
			CString dirPath;
			const int slash = max(fullPath.ReverseFind(_T('\\')), fullPath.ReverseFind(_T('/')));
			if (slash >= 0)
			{
				fileName = fullPath.Mid(slash + 1);
				dirPath = fullPath.Left(slash);
			}

			const int textLeft = iconRect.right + 12;
			if (rightLimit > textLeft + 20)
			{
				CRect nameRect(textLeft, rowRect.top + 5, rightLimit, rowRect.top + 25);
				CRect pathRect(textLeft, rowRect.top + 25, rightLimit, rowRect.bottom - 4);

				dc.SelectObject(&m_welcomeItemTitleFont);
				dc.SetTextColor(dark ? RGB(228, 233, 238) : RGB(28, 35, 44));
				dc.DrawText(fileName, &nameRect,
					DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

				dc.SelectObject(&m_welcomeItemPathFont);
				dc.SetTextColor(dark ? RGB(142, 152, 164) : RGB(122, 134, 148));
				dc.DrawText(dirPath, &pathRect,
					DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
			}
		}
	}

	dc.SelectObject(pOldFont);
}

void CPDFViewCtrl::SetDisplayBitmap(HBITMAP hBitmap)
{
	m_hDisplayBitmap = hBitmap;
	if (hBitmap)
	{
		m_welcomeHoverTarget = WELCOME_HIT_NONE;
		m_welcomeHoverIndex = -1;
		m_welcomePressedTarget = WELCOME_HIT_NONE;
		m_welcomePressedIndex = -1;
	}
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
	if (isWelcomePageActive())
	{
		WelcomeHitTarget target = WELCOME_HIT_NONE;
		int index = -1;
		hitTestWelcomePage(point, target, index);
		m_welcomePressedTarget = target;
		m_welcomePressedIndex = index;
		if (target != WELCOME_HIT_NONE)
		{
			SetCapture();
			return;
		}
	}
	ForwardMouseMessage(WM_LBUTTONDOWN, nFlags, point);
}

void CPDFViewCtrl::OnLButtonUp(UINT nFlags, CPoint point)
{
	if (isWelcomePageActive() && m_welcomePressedTarget != WELCOME_HIT_NONE)
	{
		const WelcomeHitTarget pressedTarget = m_welcomePressedTarget;
		const int pressedIndex = m_welcomePressedIndex;
		m_welcomePressedTarget = WELCOME_HIT_NONE;
		m_welcomePressedIndex = -1;
		if (::GetCapture() == GetSafeHwnd())
			::ReleaseCapture();

		WelcomeHitTarget releaseTarget = WELCOME_HIT_NONE;
		int releaseIndex = -1;
		hitTestWelcomePage(point, releaseTarget, releaseIndex);
		if (releaseTarget == pressedTarget && releaseIndex == pressedIndex && m_pParentDlg)
		{
			if (releaseTarget == WELCOME_HIT_OPEN_BTN)
				m_pParentDlg->PostMessage(WM_COMMAND, ID_MENU_OPEN);
			else if (releaseTarget == WELCOME_HIT_CLEAR_BTN)
				m_pParentDlg->ClearRecentFiles();
			else if (releaseTarget == WELCOME_HIT_RECENT_REMOVE)
				m_pParentDlg->RemoveRecentFileAt(releaseIndex);
			else if (releaseTarget == WELCOME_HIT_RECENT_ROW)
				m_pParentDlg->OpenRecentFileAt(releaseIndex);
		}
		return;
	}
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
	if (isWelcomePageActive())
	{
		if (!m_welcomeTracking)
		{
			TRACKMOUSEEVENT tme = {};
			tme.cbSize = sizeof(tme);
			tme.dwFlags = TME_LEAVE;
			tme.hwndTrack = GetSafeHwnd();
			if (::TrackMouseEvent(&tme))
				m_welcomeTracking = TRUE;
		}
		WelcomeHitTarget target = WELCOME_HIT_NONE;
		int index = -1;
		hitTestWelcomePage(point, target, index);
		if (target != m_welcomeHoverTarget || index != m_welcomeHoverIndex)
		{
			m_welcomeHoverTarget = target;
			m_welcomeHoverIndex = index;
			::SetCursor(::LoadCursor(nullptr,
				target != WELCOME_HIT_NONE ? IDC_HAND : IDC_ARROW));
			Invalidate(FALSE);
		}
		return;
	}
	ForwardMouseMessage(WM_MOUSEMOVE, nFlags, point);
}

void CPDFViewCtrl::OnMouseLeave()
{
	m_welcomeTracking = FALSE;
	if (m_welcomeHoverTarget != WELCOME_HIT_NONE || m_welcomeHoverIndex != -1)
	{
		m_welcomeHoverTarget = WELCOME_HIT_NONE;
		m_welcomeHoverIndex = -1;
		Invalidate(FALSE);
	}
	CStatic::OnMouseLeave();
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
		if (m_readingMode == READING_EYE)
		{
			CDC tintDC;
			CBitmap tintBitmap;
			tintDC.CreateCompatibleDC(&dc);
			tintBitmap.CreateCompatibleBitmap(&dc, 1, 1);
			CBitmap* pOldTint = tintDC.SelectObject(&tintBitmap);
			tintDC.FillSolidRect(0, 0, 1, 1, RGB(92, 76, 50));
			BLENDFUNCTION blend = { AC_SRC_OVER, 0, 48, 0 };
			backDC.AlphaBlend(0, 0, clientRect.Width(), clientRect.Height(),
				&tintDC, 0, 0, 1, 1, blend);
			tintDC.SelectObject(pOldTint);
		}
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
