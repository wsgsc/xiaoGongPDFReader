#pragma once

#include <afxwin.h>
#include <gdiplus.h>

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

protected:
	CXiaoGongPDFDlg* m_pParentDlg;
	HBITMAP m_hDisplayBitmap;
	ReadingMode m_readingMode;
	Gdiplus::Bitmap* m_pWelcomeBitmap;
	IStream* m_pWelcomeStream;

	void DrawWelcomeImage(CDC& dc, const CRect& clientRect);
	void ForwardMouseMessage(UINT message, UINT nFlags, CPoint point);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnMButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
	afx_msg void OnPaint();
	DECLARE_MESSAGE_MAP()
};
