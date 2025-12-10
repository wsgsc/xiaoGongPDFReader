#pragma once

// 现代化Tab Control控件
// 提供美观的标签页外观，包括：
// - 圆角边框
// - 渐变背景
// - 悬停效果
// - 清晰的选中状态
// - 可选的关闭按钮

class CModernTabCtrl : public CTabCtrl
{
	DECLARE_DYNAMIC(CModernTabCtrl)

public:
	CModernTabCtrl();
	virtual ~CModernTabCtrl();

	// 配置选项
	void EnableCloseButton(BOOL bEnable = TRUE);  // 启用关闭按钮
	void SetTabHeight(int height);                // 设置标签页高度
	void SetColors(COLORREF normal, COLORREF hover, COLORREF selected);  // 设置颜色

protected:
	DECLARE_MESSAGE_MAP()

	afx_msg void OnPaint();
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnMouseLeave();
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnDestroy();

private:
	// 绘制相关
	void DrawTab(CDC* pDC, int index, CRect rect, BOOL isSelected, BOOL isHovered);
	void DrawCloseButton(CDC* pDC, CRect rect, BOOL isHovered);
	CRect GetCloseButtonRect(int index);
	CRect GetTabRect(int index);

	// 状态跟踪
	int m_hoverIndex;          // 当前悬停的标签页索引
	int m_closeHoverIndex;     // 当前悬停的关闭按钮索引
	BOOL m_bCloseButtonEnabled; // 是否启用关闭按钮
	BOOL m_bTracking;          // 是否正在跟踪鼠标

	// 外观配置
	int m_tabHeight;           // 标签页高度
	COLORREF m_normalColor;    // 普通标签背景色
	COLORREF m_hoverColor;     // 悬停标签背景色
	COLORREF m_selectedColor;  // 选中标签背景色

	// GDI对象
	CFont m_font;              // 标签字体
	CFont m_boldFont;          // 粗体字体（选中时）
};
