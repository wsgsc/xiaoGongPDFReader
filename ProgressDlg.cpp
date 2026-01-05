// ProgressDlg.cpp: 进度对话框实现

#include "pch.h"
#include "resource.h"
#include "ProgressDlg.h"

IMPLEMENT_DYNAMIC(CProgressDlg, CDialogEx)

CProgressDlg::CProgressDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_PROGRESS_DIALOG, pParent)
	, m_bCancelled(false)
	, m_bIndeterminate(true)
{
}

CProgressDlg::~CProgressDlg()
{
}

void CProgressDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_PROGRESS_BAR, m_progressCtrl);
	DDX_Control(pDX, IDC_PROGRESS_STATUS, m_statusText);
}

BEGIN_MESSAGE_MAP(CProgressDlg, CDialogEx)
END_MESSAGE_MAP()

BOOL CProgressDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// 设置对话框标题
	SetWindowTextW(L"\u52a0\u8f7d\u4e2d");

	// 设置进度条范围
	m_progressCtrl.SetRange(0, 100);
	m_progressCtrl.SetPos(0);

	// 如果是不确定进度，设置为Marquee样式
	if (m_bIndeterminate)
	{
		m_progressCtrl.ModifyStyle(0, PBS_MARQUEE);
		m_progressCtrl.SetMarquee(TRUE, 30);
	}

	// 设置初始状态文本
	m_statusText.SetWindowTextW(L"\u6b63\u5728\u52a0\u8f7dPDF\u6587\u4ef6\uff0c\u8bf7\u7a0d\u5019...");

	// 设置取消按钮文本
	CWnd* pCancelBtn = GetDlgItem(IDCANCEL);
	if (pCancelBtn)
	{
		pCancelBtn->SetWindowTextW(L"\u53d6\u6d88");
	}

	// 居中显示
	CenterWindow();

	return TRUE;
}

void CProgressDlg::OnCancel()
{
	m_bCancelled = true;
	CDialogEx::OnCancel();
}

void CProgressDlg::SetProgress(int percent)
{
	if (m_progressCtrl.GetSafeHwnd())
	{
		// 如果之前是不确定进度，切换为确定进度
		if (m_bIndeterminate)
		{
			m_progressCtrl.SetMarquee(FALSE, 0);
			m_progressCtrl.ModifyStyle(PBS_MARQUEE, 0);
			m_bIndeterminate = false;
		}

		m_progressCtrl.SetPos(percent);
	}
}

void CProgressDlg::SetStatus(const CString& status)
{
	if (m_statusText.GetSafeHwnd())
	{
		m_statusText.SetWindowText(status);
	}
}

void CProgressDlg::SetIndeterminate(bool indeterminate)
{
	m_bIndeterminate = indeterminate;

	if (m_progressCtrl.GetSafeHwnd())
	{
		if (indeterminate)
		{
			m_progressCtrl.ModifyStyle(0, PBS_MARQUEE);
			m_progressCtrl.SetMarquee(TRUE, 30);
		}
		else
		{
			m_progressCtrl.SetMarquee(FALSE, 0);
			m_progressCtrl.ModifyStyle(PBS_MARQUEE, 0);
		}
	}
}
