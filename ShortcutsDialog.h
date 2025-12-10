// ShortcutsDialog.h: 快捷键帮助对话框头文件
//

#pragma once
#include "afxdialogex.h"

// CShortcutsDialog 对话框

class CShortcutsDialog : public CDialogEx
{
	DECLARE_DYNAMIC(CShortcutsDialog)

public:
	CShortcutsDialog(CWnd* pParent = nullptr);   // 标准构造函数
	virtual ~CShortcutsDialog();

// 对话框数据
#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_SHORTCUTS_DIALOG };
#endif

protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV 支持
	virtual BOOL OnInitDialog();

	DECLARE_MESSAGE_MAP()

private:
	CListCtrl m_shortcutsList;  // 快捷键列表控件

	void InitializeShortcutsList();  // 初始化快捷键列表
	void AddShortcut(const CString& key, const CString& description);  // 添加快捷键项
};
