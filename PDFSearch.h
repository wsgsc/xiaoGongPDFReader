#pragma once

#include <afxwin.h>
#include <atomic>
#include <vector>
#include "SearchTypes.h"

class CPDFDocument;

struct PDFSearchParams
{
	HWND notifyWnd;
	CPDFDocument* pDoc;
	UINT_PTR taskId;
	std::vector<char> utf8Keyword;
	std::vector<char> utf8Path;
	std::vector<char> utf8Password;
	bool caseSensitive;
	bool failed;
	std::vector<SearchMatch> results;
	std::atomic<bool> cancelled;
	CWinThread* pThread;

	PDFSearchParams()
		: notifyWnd(nullptr)
		, pDoc(nullptr)
		, taskId(0)
		, caseSensitive(false)
		, failed(false)
		, cancelled(false)
		, pThread(nullptr)
	{
	}

	~PDFSearchParams()
	{
		if (!utf8Password.empty())
			SecureZeroMemory(utf8Password.data(), utf8Password.size());
	}
};

UINT PDFSearchThreadProc(LPVOID pParam);
