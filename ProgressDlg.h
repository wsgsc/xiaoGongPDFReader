// ProgressDlg.h: 进度对话框类
// 用于显示PDF加载进度，避免加载大文件时UI卡死

#pragma once
#include <afxwin.h>

class CProgressDlg : public CDialogEx
{
	DECLARE_DYNAMIC(CProgressDlg)

public:
	CProgressDlg(CWnd* pParent = nullptr);
	virtual ~CProgressDlg();

	// 对话框数据
	enum { IDD = IDD_PROGRESS_DIALOG };

	// 设置进度信息
	void SetProgress(int percent);
	void SetStatus(const CString& status);
	void SetIndeterminate(bool indeterminate);

	// 检查是否取消
	bool IsCancelled() const { return m_bCancelled; }

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void OnCancel();

	DECLARE_MESSAGE_MAP()

private:
	CProgressCtrl m_progressCtrl;
	CStatic m_statusText;
	bool m_bCancelled;
	bool m_bIndeterminate;
};
