// ShortcutsDialog.cpp: 快捷键帮助对话框实现文件
//

#include "pch.h"
#include "XiaoGongPDF.h"
#include "ShortcutsDialog.h"
#include "afxdialogex.h"

// CShortcutsDialog 对话框

IMPLEMENT_DYNAMIC(CShortcutsDialog, CDialogEx)

CShortcutsDialog::CShortcutsDialog(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_SHORTCUTS_DIALOG, pParent)
{
}

CShortcutsDialog::~CShortcutsDialog()
{
}

void CShortcutsDialog::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_SHORTCUTS_LIST, m_shortcutsList);
}

BEGIN_MESSAGE_MAP(CShortcutsDialog, CDialogEx)
END_MESSAGE_MAP()

// CShortcutsDialog 消息处理程序

BOOL CShortcutsDialog::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// 设置对话框标题
	SetWindowText(_T("快捷键帮助"));

	// 初始化快捷键列表
	InitializeShortcutsList();

	// 居中显示
	CenterWindow();

	return TRUE;
}

void CShortcutsDialog::InitializeShortcutsList()
{
	// 设置列表控件样式
	m_shortcutsList.SetExtendedStyle(
		m_shortcutsList.GetExtendedStyle() |
		LVS_EX_FULLROWSELECT |
		LVS_EX_GRIDLINES
	);

	// 添加列
	m_shortcutsList.InsertColumn(0, _T("快捷键"), LVCFMT_LEFT, 150);
	m_shortcutsList.InsertColumn(1, _T("功能说明"), LVCFMT_LEFT, 300);

	// 设置字体
	CFont font;
	font.CreatePointFont(95, _T("微软雅黑"));
	m_shortcutsList.SetFont(&font);
	font.Detach();

	// 添加快捷键
	AddShortcut(_T("← / →"), _T("上一页 / 下一页"));
	AddShortcut(_T("Home / End"), _T("首页 / 末页"));
	AddShortcut(_T(""), _T(""));

	AddShortcut(_T("鼠标滚轮"), _T("向上滚动页面/向下滚动页面"));
	AddShortcut(_T(""), _T(""));

	AddShortcut(_T("Ctrl + F"), _T("打开搜索"));
	AddShortcut(_T("F3"), _T("查找下一个"));
	AddShortcut(_T("Shift + F3"), _T("查找上一个"));
	AddShortcut(_T(""), _T(""));

	AddShortcut(_T("Ctrl + L"), _T("向左旋转（逆时针90度）"));
	AddShortcut(_T("Ctrl + R"), _T("向右旋转（顺时针90度）"));
	AddShortcut(_T(""), _T(""));

	AddShortcut(_T("F9"), _T("切换缩略图面板"));
	AddShortcut(_T("F11"), _T("全屏模式"));
	AddShortcut(_T("ESC"), _T("退出全屏"));
	AddShortcut(_T(""), _T(""));

	AddShortcut(_T("Ctrl + O"), _T("打开文件"));
	AddShortcut(_T("Ctrl + P"), _T("打印"));
	AddShortcut(_T("Ctrl + Q"), _T("退出程序"));
}

void CShortcutsDialog::AddShortcut(const CString& key, const CString& description)
{
	int nIndex = m_shortcutsList.GetItemCount();
	m_shortcutsList.InsertItem(nIndex, key);
	m_shortcutsList.SetItemText(nIndex, 1, description);
}
