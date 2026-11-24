#pragma once

#include <afxwin.h>
#include <gdiplus.h>
#include <vector>

class CXiaoGongPDFDlg;

enum ReadingMode { READING_NORMAL, READING_NIGHT, READING_EYE };
constexpr COLORREF PDF_VIEW_BACKGROUND = RGB(232, 239, 253);
constexpr COLORREF DARK_VIEW_BACKGROUND = RGB(30, 33, 36);

// 自定义 PDF 预览控件（双缓冲无闪烁渲染、欢迎页与护眼/夜间模式合成）
class CPDFViewCtrl : public CStatic
{
public:
	CPDFViewCtrl()
		: m_pParentDlg(nullptr)
		, m_hDisplayBitmap(nullptr)
		, m_readingMode(READING_NORMAL)
		, m_pWelcomeBitmap(nullptr)
		, m_pWelcomeStream(nullptr)
		, m_welcomeHoverTarget(WELCOME_HIT_NONE)
		, m_welcomeHoverIndex(-1)
		, m_welcomePressedTarget(WELCOME_HIT_NONE)
		, m_welcomePressedIndex(-1)
		, m_welcomeTracking(FALSE)
	{
	}
	~CPDFViewCtrl() override;

	void SetParentDlg(CXiaoGongPDFDlg* pDlg) { m_pParentDlg = pDlg; }
	void SetReadingMode(ReadingMode mode)
	{
		m_readingMode = mode;
		if (GetSafeHwnd())
			Invalidate(FALSE);
	}
	void SetDisplayBitmap(HBITMAP hBitmap);
	bool IsHoveringWelcomeAction() const { return m_welcomeHoverTarget != WELCOME_HIT_NONE; }

protected:
	enum WelcomeHitTarget
	{
		WELCOME_HIT_NONE = 0,
		WELCOME_HIT_OPEN_BTN,
		WELCOME_HIT_CLEAR_BTN,
		WELCOME_HIT_RECENT_ROW,
		WELCOME_HIT_RECENT_REMOVE
	};

	CXiaoGongPDFDlg* m_pParentDlg;
	HBITMAP m_hDisplayBitmap;
	ReadingMode m_readingMode;
	Gdiplus::Bitmap* m_pWelcomeBitmap;
	IStream* m_pWelcomeStream;
	CRect m_openBtnRect;
	CRect m_clearRecentRect;
	std::vector<CRect> m_recentRowRects;
	std::vector<CRect> m_recentRemoveRects;
	WelcomeHitTarget m_welcomeHoverTarget;
	int m_welcomeHoverIndex;
	WelcomeHitTarget m_welcomePressedTarget;
	int m_welcomePressedIndex;
	BOOL m_welcomeTracking;
	CFont m_welcomeTitleFont;
	CFont m_welcomeBtnFont;
	CFont m_welcomeSectionFont;
	CFont m_welcomeItemTitleFont;
	CFont m_welcomeItemPathFont;
	CFont m_welcomeBadgeFont;

	bool isWelcomePageActive() const;
	void ensureWelcomeFonts();
	void hitTestWelcomePage(CPoint point, WelcomeHitTarget& outTarget, int& outIndex) const;
	void DrawWelcomeImage(CDC& dc, const CRect& clientRect);
	void ForwardMouseMessage(UINT message, UINT nFlags, CPoint point);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnMButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnMouseLeave();
	afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg void OnPaint();
	DECLARE_MESSAGE_MAP()
};
