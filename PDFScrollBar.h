#pragma once

#include <afxwin.h>

class CXiaoGongPDFDlg;

class CPDFScrollBar : public CWnd
{
public:
	explicit CPDFScrollBar(bool vertical = true);
	void SetParentDlg(CXiaoGongPDFDlg* pDlg) { m_pParentDlg = pDlg; }
	void SetRange(int contentLength, int pageLength, int position);
	void SetDark(bool dark)
	{
		m_dark = dark;
		if (GetSafeHwnd())
			Invalidate(FALSE);
	}

protected:
	afx_msg void OnPaint();
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
	afx_msg void OnCaptureChanged(CWnd* pWnd);
	DECLARE_MESSAGE_MAP()

private:
	CRect ThumbRect() const;
	void ScrollTo(int position);

	CXiaoGongPDFDlg* m_pParentDlg;
	bool m_vertical;
	int m_contentLength;
	int m_pageLength;
	int m_position;
	int m_dragOffset;
	bool m_dragging;
	bool m_dark = false;
};
