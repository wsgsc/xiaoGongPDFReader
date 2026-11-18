
// XiaoGongPDF.h: PROJECT_NAME 应用程序的主头文件
//

#pragma once

#ifndef __AFXWIN_H__
	#error "在包含此文件之前包含 'pch.h' 以生成 PCH"
#endif

#include "resource.h"		// 主符号

#include <mupdf/fitz.h>

// 主窗口唯一识别属性名（避免通过标题匹配误伤 IDE/资源管理器窗口）
constexpr LPCTSTR XIAOGONG_MAINWND_PROP = _T("XiaoGongPDF_MainWnd_Prop_2024");

// CXiaoGongPDFApp:
// 有关此类的实现，请参阅 XiaoGongPDF.cpp
//

class CXiaoGongPDFApp : public CWinApp
{
public:
	CXiaoGongPDFApp();
	virtual ~CXiaoGongPDFApp();

// 重写
public:
	virtual BOOL InitInstance();
	virtual int ExitInstance();

// 实现
	DECLARE_MESSAGE_MAP()

private:
	HANDLE m_hMutex;  // 单实例互斥量
	ULONG_PTR m_gdiplusToken;  // GDI+ Token
};

extern CXiaoGongPDFApp theApp;
