// PDFAnnotationHelper.h: 轻量级 PDF 批注与原文件保存辅助模块
//

#pragma once

#include <afxwin.h>
#include <afxdialogex.h>
#include <afxmt.h>
#include <functional>
#include <map>
#include <vector>
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include "resource.h"

class CPDFDocument;

// 页面批注命中信息
struct PageAnnotHitInfo
{
	int annotIndex = -1;
	enum pdf_annot_type type = PDF_ANNOT_UNKNOWN;
	fz_rect fitzBounds = { 0.0f, 0.0f, 0.0f, 0.0f };
	CString contents;
	CString typeLabel;
	COLORREF textColor = RGB(0, 114, 206);
	float fontSizePt = 12.0f;
	float charSpacingPt = 0.0f;
};

// 全文档侧边栏批注汇总条目
struct DocumentAnnotationItem
{
	int pageNumber = -1;
	int annotIndex = -1;
	enum pdf_annot_type type = PDF_ANNOT_UNKNOWN;
	fz_rect fitzBounds = { 0.0f, 0.0f, 0.0f, 0.0f };
	COLORREF color = RGB(255, 212, 59);
	CString typeLabel;
	CString contents;
	CString quotedText;
	float fontSizePt = 12.0f;
	float charSpacingPt = 0.0f;
};

// 内联文字批注视觉折行条目（保证屏幕内联编辑与写回 PDF 外观流折行一致）
struct FreeTextVisualLine
{
	CString text;
	int startChar = 0;
	int endChar = 0;
};

// 按指定文字间距测量单行前 unitCount 个 UTF-16 单元的像素宽度
int MeasureTextWithCharSpacingPx(
	HDC hDC,
	HFONT hFont,
	LPCTSTR pText,
	int unitCount,
	float charSpacingPx);

// 按指定文字间距无闪烁绘制单行文本（支持任意缩放与字间距亚像素累计）
void DrawTextWithCharSpacingPx(
	HDC hDC,
	int x,
	int y,
	const CString& lineText,
	float charSpacingPx);

// 计算内联文字批注的视觉折行结果（支持 \r\n 显式换行、字间距与宽度自动折行）
void LayoutFreeTextVisualLines(
	HDC hMeasureDC,
	HFONT hMeasureFont,
	float pxPerPt,
	const CString& fullText,
	float fontSizePt,
	float charSpacingPt,
	float availWidthPt,
	std::vector<FreeTextVisualLine>& outLines);

// 添加文本标记类标准 PDF 批注（高亮 / 下划线 / 删除线）
bool AddQuadMarkupAnnotation(
	CPDFDocument* pDoc,
	int pageNumber,
	enum pdf_annot_type annotType,
	const std::vector<fz_quad>& quads,
	COLORREF color,
	const CString& contents = CString());

// 新建或更新无边框透明背景的页面文字批注（支持自定义颜色、字号、文字间距与位置尺寸）
bool UpsertFreeTextAnnotation(
	CPDFDocument* pDoc,
	int pageNumber,
	int existingAnnotIndex,
	const fz_rect& fitzRect,
	const CString& noteText,
	COLORREF textColor,
	float fontSizePt,
	float charSpacingPt,
	int* pOutAnnotIndex = nullptr);

// 在内联编辑已有文字批注期间临时隐藏/恢复底层静态批注渲染，避免移动和编辑时重影
bool SetPageAnnotationEditingHidden(
	CPDFDocument* pDoc,
	int pageNumber,
	int annotIndex,
	bool hidden);

// 添加文字批注（兼容旧接口：内部转发至无边框透明 FreeText 或便签图标）
bool AddTextNoteAnnotation(
	CPDFDocument* pDoc,
	int pageNumber,
	float fitzX,
	float fitzY,
	bool freeTextStyle,
	const CString& noteText,
	COLORREF textColor,
	float fontSizePt,
	float charSpacingPt = 0.0f);

// 命中测试页面上的已有批注（按绘制层级从上往下匹配，自动解析文字批注颜色/字号/行距）
bool HitTestPageAnnotation(
	CPDFDocument* pDoc,
	int pageNumber,
	float fitzX,
	float fitzY,
	PageAnnotHitInfo& outHit);

// 按页面与批注序号精确读取单个批注详情（供侧边栏批注列表点击/双击编辑使用）
bool GetPageAnnotationInfoAt(
	CPDFDocument* pDoc,
	int pageNumber,
	int annotIndex,
	PageAnnotHitInfo& outHit);

// 扫描收集整份文档中的所有用户批注（供侧边栏批注汇总列表使用）
bool CollectDocumentAnnotations(
	CPDFDocument* pDoc,
	CCriticalSection* pRenderLock,
	std::vector<DocumentAnnotationItem>& outItems);

// 修改已有批注的文本内容
bool UpdatePageAnnotationContents(
	CPDFDocument* pDoc,
	int pageNumber,
	int annotIndex,
	const CString& newContents);

// 删除页面上指定索引的批注
bool DeletePageAnnotationAt(
	CPDFDocument* pDoc,
	int pageNumber,
	int annotIndex);

// 将当前文档的批注与单页旋转角度安全写回目标 PDF 文件（临时文件校验 + 原子替换）
bool SaveDocumentWithAnnotationsAndRotations(
	CPDFDocument* pDoc,
	const CString& targetFilePath,
	const std::map<int, int>& viewerRotations,
	const std::function<void(const CString&)>& onBeforeReplace,
	const std::function<void(const CString&, bool)>& onAfterReplace,
	CString& outErrorMsg);

// 透明无闪烁页面内联文字批注输入代理控件（原生接管中文输入法 IME、剪贴板与撤销，视觉由双缓冲画布自绘）
class CInlineAnnotEdit : public CEdit
{
public:
	CInlineAnnotEdit() = default;
	~CInlineAnnotEdit() override = default;

	bool CreateHiddenProxy(CWnd* pParent, UINT nID);
	void SetCaretAnchorInEdit(CPoint caretPt, int lineHeightPx);

	std::function<void()> m_onTextOrCaretChanged;
	std::function<void()> m_onEscapePressed;
	std::function<bool(UINT /*vk*/, bool /*shift*/)> m_onVerticalOrLineNav;

protected:
	afx_msg UINT OnGetDlgCode();
	afx_msg BOOL OnEraseBkgnd(CDC* pDC);
	afx_msg void OnPaint();
	afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg void OnKeyUp(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg void OnChar(UINT nChar, UINT nRepCnt, UINT nFlags);
	afx_msg LRESULT OnImeStartComposition(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnImeComposition(WPARAM wParam, LPARAM lParam);
	afx_msg LRESULT OnClipboardOrUndo(WPARAM wParam, LPARAM lParam);
	DECLARE_MESSAGE_MAP()

private:
	CPoint m_caretLocalPt{ 4, 4 };
	int m_caretLineHeightPx = 18;

	void syncImeWindowPosition();
	void notifyChanged();
};

// 便签/高亮备注内容编辑对话框（用于非页面直接内联类的传统弹窗批注内容编辑）
class CPDFAnnotNoteDlg : public CDialogEx
{
public:
	CPDFAnnotNoteDlg(bool editExistingOnly = false, const CString& initialText = CString(),
		CWnd* pParent = nullptr);
	virtual ~CPDFAnnotNoteDlg() = default;

	enum { IDD = IDD_ANNOT_NOTE_DIALOG };

	bool m_freeTextStyle;
	COLORREF m_color;
	float m_fontSizePt;
	CString m_noteText;

protected:
	virtual void DoDataExchange(CDataExchange* pDX) override;
	virtual BOOL OnInitDialog() override;
	virtual void OnOK() override;

	afx_msg void OnStyleChanged();
	DECLARE_MESSAGE_MAP()

private:
	bool m_editExistingOnly;
	CButton m_radioFreeText;
	CButton m_radioIcon;
	CComboBox m_comboColor;
	CComboBox m_comboFontSize;
	CEdit m_editNote;

	void updateStyleControls();
};
