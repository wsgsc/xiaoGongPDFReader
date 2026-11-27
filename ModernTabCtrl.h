#pragma once

#include <afxcmn.h>

// 标签页与主窗口交互消息
constexpr UINT WM_TAB_CLOSE_REQUEST   = WM_APP + 100; // wParam: tabIndex
constexpr UINT WM_TAB_REORDER_REQUEST = WM_APP + 101; // wParam: fromIndex, lParam: toIndex
constexpr UINT WM_TAB_CONTEXT_MENU    = WM_APP + 102; // wParam: tabIndex, lParam: MAKELPARAM(screenX, screenY)

// 现代化Tab Control控件
// 提供美观的标签页外观，包括：
// - 圆角边框
// - 渐变背景
// - 悬停效果与完整路径提示
// - 清晰的选中状态、标签左右拖拽排序与空白处拖拽窗口
// - 可选的关闭按钮、中键关闭与右键菜单

class CModernTabCtrl : public CTabCtrl
{
	DECLARE_DYNAMIC(CModernTabCtrl)

public:
	static constexpr int MIN_TAB_WIDTH = 135;
	static constexpr int MAX_TAB_WIDTH = 250;
	static constexpr int BLANK_DRAG_RESERVE = 44;

	CModernTabCtrl();
	virtual ~CModernTabCtrl();

	// 配置选项
	void EnableCloseButton(BOOL bEnable = TRUE);  // 启用关闭按钮
	void SetTabHeight(int height);                // 设置标签页高度
	void SetColors(COLORREF normal, COLORREF hover, COLORREF selected);  // 设置颜色
	void HideOverflowButtons();
	int CalculateTabWidth(int availableWidth, int tabCount) const;
	int HitTestTabScreen(CPoint screenPoint);
	virtual BOOL PreTranslateMessage(MSG* pMsg);

protected:
	DECLARE_MESSAGE_MAP()

	afx_msg void OnPaint();
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint point);
	afx_msg void OnMouseLeave();
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnLButtonDblClk(UINT nFlags, CPoint point);
	afx_msg void OnMButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnMButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnRButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnRButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnCaptureChanged(CWnd* pWnd);
	afx_msg void OnCancelMode();
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnDestroy();
	afx_msg LRESULT OnSetCurSelMsg(WPARAM wParam, LPARAM lParam);

private:
	// 绘制相关
	void DrawTab(CDC* pDC, int index, CRect rect, BOOL isSelected, BOOL isHovered);
	void DrawCloseButton(CDC* pDC, CRect rect, BOOL isHovered);
	BOOL IsCloseButtonVisible(int index);
	CRect GetCloseButtonRect(int index);
	CRect GetCloseButtonRectForTabRect(const CRect& tabRect) const;
	CRect GetTabRect(int index);
	int GetTabsViewportWidth(int clientWidth) const;
	int GetMaxScrollOffset() const;
	void ClampScrollOffset();
	void EnsureTabVisible(int index);
	void ResetDragState();
	int HitTestTab(CPoint point);
	CString GetTabToolTipText(int index) const;
	void UpdateHoverToolTip();

	// 状态跟踪
	int m_hoverIndex;          // 当前悬停的标签页索引
	int m_closeHoverIndex;     // 当前悬停的关闭按钮索引
	BOOL m_bCloseButtonEnabled; // 是否启用关闭按钮
	BOOL m_bTracking;          // 是否正在跟踪鼠标
	BOOL m_tabDragArmed;
	BOOL m_tabDragging;
	BOOL m_tabReordering;      // 是否正在左右拖拽调整标签顺序
	int m_tabPressedIndex;
	int m_middlePressedIndex;  // 中键按下的标签索引
	CPoint m_tabPressClientPt; // 按下标签时的客户区坐标
	int m_dragGrabOffsetX;     // 拖拽标签时鼠标相对标签左边缘偏移
	int m_dragCursorClientX;   // 拖拽标签时当前鼠标客户区 X 坐标
	CPoint m_tabDragStartScreen;
	CRect m_tabDragStartWindow;
	int m_scrollOffset;        // 标签溢出时的水平滚动偏移
	int m_lastEnsuredSel;      // 上次自动滚入可视区的选中标签
	int m_wheelRemainder;      // 高精度触控板滚轮余数
	CToolTipCtrl m_toolTip;    // 悬停完整路径/关闭按钮提示
	CString m_lastToolTipText;

	// 外观配置
	int m_tabHeight;           // 标签页高度
	COLORREF m_normalColor;    // 普通标签背景色
	COLORREF m_hoverColor;     // 悬停标签背景色
	COLORREF m_selectedColor;  // 选中标签背景色

	// GDI对象
	CFont m_font;              // 普通标签字体
	CFont m_selectedFont;      // 选中标签字体（中等字重）
	CFont m_compactFont;       // 紧凑标签字体
	CFont m_badgeFont;         // PDF 图标角标字体
};
