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
	m_shortcutsList.InsertColumn(0, _T("快捷键"), LVCFMT_LEFT, 235);
	m_shortcutsList.InsertColumn(1, _T("功能说明"), LVCFMT_LEFT, 245);

	// 设置字体
	if (!m_listFont.GetSafeHandle())
		m_listFont.CreatePointFont(95, _T("微软雅黑"));
	m_shortcutsList.SetFont(&m_listFont);

	// 添加快捷键
	AddShortcut(_T("← / →, PageUp / PageDown"), _T("上一页 / 下一页"));
	AddShortcut(_T("Home / End"), _T("首页 / 末页"));
	AddShortcut(_T("Ctrl + G"), _T("快速输入页码跳转"));
	AddShortcut(_T(""), _T(""));

	AddShortcut(_T("鼠标滚轮"), _T("向上滚动页面 / 向下滚动页面"));
	AddShortcut(_T("Ctrl + 滚轮, Ctrl + + / -"), _T("放大 / 缩小页面"));
	AddShortcut(_T("Ctrl + 0"), _T("恢复适合宽度缩放"));
	AddShortcut(_T("空格 + 左键拖拽 / 中键拖拽"), _T("临时抓手拖动页面"));
	AddShortcut(_T("Ctrl + C"), _T("复制选中的页面文本"));
	AddShortcut(_T("Ctrl + H / Ctrl + U"), _T("对选中文本一键添加高亮 / 下划线批注"));
	AddShortcut(_T("Ctrl + T"), _T("在页面添加可移动文字批注（支持颜色/字号/文字间距）"));
	AddShortcut(_T("Ctrl + Shift + S"), _T("框选区域高清截图（复制或保存 PNG）"));
	AddShortcut(_T("Ctrl + Shift + E"), _T("导出当前页/指定页为 PNG / JPG / 长图"));
	AddShortcut(_T(""), _T(""));

	AddShortcut(_T("Ctrl + F"), _T("打开文档搜索"));
	AddShortcut(_T("Ctrl + Shift + F"), _T("切换搜索是否区分大小写"));
	AddShortcut(_T("F3 / Shift + F3"), _T("查找下一个 / 上一个匹配项"));
	AddShortcut(_T("Esc"), _T("关闭搜索框 / 取消搜索 / 退出全屏"));
	AddShortcut(_T(""), _T(""));

	AddShortcut(_T("Ctrl + L / Ctrl + R"), _T("向左 / 向右旋转当前页 90 度（自动记忆）"));
	AddShortcut(_T("Ctrl + S"), _T("将批注与页面旋转保存写回原 PDF"));
	AddShortcut(_T("Ctrl + D"), _T("查看文档属性与当前页物理尺寸（幅面）"));
	AddShortcut(_T("F9"), _T("显示 / 隐藏侧栏（目录 / 缩略图 / 批注列表）"));
	AddShortcut(_T("F11"), _T("进入 / 退出全屏模式"));
	AddShortcut(_T(""), _T(""));

	AddShortcut(_T("Ctrl + O"), _T("打开 PDF 文件"));
	AddShortcut(_T("Ctrl + W / 鼠标中键点标签"), _T("关闭当前标签页"));
	AddShortcut(_T("Ctrl + Tab / Ctrl + Shift + Tab"), _T("切换下一个 / 上一个标签页"));
	AddShortcut(_T("Ctrl + P"), _T("打印当前文档"));
	AddShortcut(_T("Ctrl + Q"), _T("退出程序"));
}

void CShortcutsDialog::AddShortcut(const CString& key, const CString& description)
{
	int nIndex = m_shortcutsList.GetItemCount();
	m_shortcutsList.InsertItem(nIndex, key);
	m_shortcutsList.SetItemText(nIndex, 1, description);
}
