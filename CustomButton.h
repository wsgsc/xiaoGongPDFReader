#pragma once
#include <afxwin.h>

// 自定义按钮类 - 支持圆角、渐变、鼠标悬停效果
class CCustomButton : public CButton
{
	DECLARE_DYNAMIC(CCustomButton)

public:
	CCustomButton();
	virtual ~CCustomButton();

	// 设置按钮颜色
	void SetColors(COLORREF normalColor, COLORREF hoverColor, COLORREF pressedColor);
	void SetTextColor(COLORREF textColor);
	void SetCornerRadius(int radius);

protected:
	virtual void DrawItem(LPDRAWITEMSTRUCT lpDrawItemStruct);
	virtual void PreSubclassWindow();

	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnMouseHover(UINT nFlags, CPoint point);
	afx_msg void OnMouseLeave();
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);

	DECLARE_MESSAGE_MAP()

private:
	COLORREF m_normalColor;      // 正常状态颜色
	COLORREF m_hoverColor;       // 鼠标悬停颜色
	COLORREF m_pressedColor;     // 按下状态颜色
	COLORREF m_textColor;        // 文字颜色
	int m_cornerRadius;          // 圆角半径
	BOOL m_bHover;               // 鼠标悬停标志
	BOOL m_bTracking;            // 鼠标跟踪标志
	CFont m_buttonFont;          // 按钮字体

	void DrawRoundRect(CDC* pDC, CRect rect, int radius, COLORREF color);
};
