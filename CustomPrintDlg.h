#pragma once

#include <afxmt.h>
#include <afxdialogex.h>
#include <vector>
#include <mupdf/fitz.h>
#include "resource.h"

class CCustomPrintDlg : public CDialogEx
{
public:
	CCustomPrintDlg(int totalPages, CWnd* pParent = nullptr);
	virtual ~CCustomPrintDlg();

	enum { IDD = IDD_PRINT_DIALOG };

	CString m_printerName;
	BOOL m_bPrintAll;
	int m_pageFrom;
	int m_pageTo;
	int m_copies;
	BOOL m_bCollate;
	BOOL m_bDuplex;

	DEVMODE* GetDevMode()
	{
		return m_devMode.empty() ? nullptr : reinterpret_cast<DEVMODE*>(m_devMode.data());
	}

protected:
	virtual void DoDataExchange(CDataExchange* pDX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();

	afx_msg void OnPrinterChange();
	afx_msg void OnPrintAllRadio();
	afx_msg void OnPrintRangeRadio();
	afx_msg void OnPrinterProperties();

	DECLARE_MESSAGE_MAP()

private:
	int m_totalPages;
	CComboBox m_printerCombo;
	CButton m_printAllRadio;
	CButton m_printRangeRadio;
	CEdit m_pageFromEdit;
	CEdit m_pageToEdit;
	CEdit m_copiesEdit;
	CButton m_collateCheck;
	CButton m_duplexCheck;
	std::vector<DWORD> m_devMode;

	void EnumeratePrinters();
	void UpdateControls();
};

bool PrintPDFPageToDC(CDC& dcPrint, fz_context* ctx, fz_document* doc,
	int pageNum, int rotation, CCriticalSection& renderLock, int horRes, int verRes);
