// PDFEditDialog.h: PDF页面编辑对话框头文件
// 用于编辑PDF页面顺序和删除页面

#pragma once
#include "afxdialogex.h"
#include "resource.h"
#include <mupdf/fitz.h>
#include <vector>

// 页面信息结构
struct PageInfo {
	int originalIndex;      // 原始页码（从0开始）
	bool isDeleted;         // 是否被删除
	HBITMAP hThumbnail;     // 缩略图句柄
	int displayIndex;       // 当前显示索引
	CRect bounds;           // 当前位置（用于动画）
	CString sourceFile;     // 源PDF文件路径（用于多文档合并）
	int sourcePageIndex;    // 在源文件中的页码
	int rotationAngle;      // 旋转角度（0, 90, 180, 270）

	PageInfo() : originalIndex(0), isDeleted(false), hThumbnail(nullptr), displayIndex(0), sourcePageIndex(0), rotationAngle(0) {}
	PageInfo(int index) : originalIndex(index), isDeleted(false), hThumbnail(nullptr), displayIndex(index), sourcePageIndex(index), rotationAngle(0) {}
};

// 动画状态结构
struct AnimState {
	bool isAnimating;
	DWORD startTime;
	std::vector<CPoint> startPositions;
	std::vector<CPoint> targetPositions;
	int duration;  // ms

	AnimState() : isAnimating(false), startTime(0), duration(200) {}
};

// CPDFEditDialog 对话框
class CPDFEditDialog : public CDialogEx
{
	DECLARE_DYNAMIC(CPDFEditDialog)

public:
	CPDFEditDialog(fz_context* ctx, fz_document* doc, const CString& filePath, CWnd* pParent = nullptr);
	virtual ~CPDFEditDialog();

	enum { IDD = IDD_PDF_EDIT_DIALOG };

	CString GetSavedFilePath() const { return m_savedFilePath; }

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	virtual void OnCancel();

	DECLARE_MESSAGE_MAP()

private:
	// MuPDF对象（不拥有，由外部管理）
	fz_context* m_ctx;
	fz_document* m_doc;
	CString m_filePath;
	int m_totalPages;

	// 控件
	CListCtrl m_thumbnailList;
	CButton m_btnSave;
	CButton m_btnReset;
	CButton m_btnAddPDF;
	CStatic m_staticInfo;

	// 页面数据
	std::vector<PageInfo> m_pages;
	std::vector<PageInfo> m_originalPages;  // 用于重置

	// 拖拽相关（优化版）
	bool m_isDragging;              // 是否正在拖拽
	bool m_potentialDrag;           // 潜在拖拽（按下但未超过阈值）
	int m_dragIndex;                // 被拖拽项的索引
	int m_dropIndex;                // 当前drop位置
	int m_lastDropIndex;            // 上次的drop位置
	CPoint m_dragStartPoint;        // 拖拽起始点（用于防误触）
	CPoint m_clickOffset;           // 鼠标点击相对缩略图的偏移
	CImageList* m_pDragImage;       // 拖拽图像
	PageInfo m_draggedPage;         // 被拖拽的页面信息（临时保存）
	int m_draggedPageOriginalPos;   // 被拖拽页面的原始位置（在m_pages中的索引）

	// 动画系统
	AnimState m_animState;          // 动画状态

	// 自动滚动
	int m_autoScrollSpeed;          // 自动滚动速度
	CPoint m_lastMousePoint;        // 最后鼠标位置

	// 定时器ID
	enum {
		TIMER_ANIMATION = 1,        // 动画定时器
		TIMER_AUTO_SCROLL = 2       // 自动滚动定时器
	};

	// 常量定义
	static const int DRAG_THRESHOLD = 5;      // 拖拽阈值（像素）
	static const int AUTO_SCROLL_ZONE = 50;   // 自动滚动触发区域（像素）
	static const int ANIM_DURATION_NORMAL = 200;   // 正常动画时长（ms）
	static const int ANIM_DURATION_FAST = 150;     // 快速动画时长（ms）

	// 保存结果
	CString m_savedFilePath;

	// ImageList
	CImageList m_imageList;

	// 初始化和渲染
	void InitializeThumbnailList();
	void LoadAllThumbnails();
	HBITMAP RenderThumbnail(int pageNumber);
	void RefreshThumbnailList();
	void UpdateInfoBar();

	// 拖拽功能
	afx_msg void OnBeginDrag(NMHDR* pNMHDR, LRESULT* pResult);
	afx_msg void OnMouseMove(UINT nFlags, CPoint point);
	afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
	afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnNMRClick(NMHDR* pNMHDR, LRESULT* pResult);

	// 拖拽辅助函数
	void BeginDragOperation();
	void EndDragOperation();
	int CalculateInsertIndex(CPoint point);
	void UpdateInsertIndex(CPoint point);
	void HandleAutoScroll(CPoint point);

	// 删除功能
	void DeleteSelectedPages();

	// 旋转功能
	void RotateSelectedPages(int angle);
	HBITMAP RotateBitmap(HBITMAP hBitmap, int angle);

	// 动画系统
	void StartReflowAnimation();
	void UpdateAnimation();
	void StopAnimation();
	CPoint LerpPoint(CPoint start, CPoint end, float t);
	float EaseOutCubic(float t);

	// 保存功能
	afx_msg void OnBnClickedBtnSave();
	bool SaveToNewFile(const CString& newFilePath);

	// 添加PDF功能
	afx_msg void OnBnClickedBtnAddPDF();
	bool ImportPDF(const CString& pdfPath);
	bool ImportImage(const CString& imagePath);
	bool ImportFile(const CString& filePath);

	// 重置功能
	afx_msg void OnBnClickedBtnReset();

	// 双击查看
	afx_msg void OnNMDblclk(NMHDR* pNMHDR, LRESULT* pResult);

	// 列表选择变化
	afx_msg void OnItemChanged(NMHDR* pNMHDR, LRESULT* pResult);

	// 资源清理
	void CleanupThumbnails();
};
