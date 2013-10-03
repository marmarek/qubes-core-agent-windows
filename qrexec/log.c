#include "log.h"

#ifdef DBG

HANDLE hLog = INVALID_HANDLE_VALUE;

static VOID lprintf_main(PUCHAR pszErrorText, size_t cchMaxErrorTextSize, PUCHAR szFormat, va_list Args)
{
	UCHAR	szMessage[2048];
	UCHAR	szPid[20];
	size_t	cchSize;
	ULONG	nWritten;

	if (!szFormat)
		return;

	memset(szMessage, 0, sizeof(szMessage));
	if (FAILED(StringCchVPrintfA(szMessage, RTL_NUMBER_OF(szMessage), szFormat, Args)))
		return;

	printf("%s%s", szMessage, pszErrorText ? pszErrorText : "");

	if (hLog == INVALID_HANDLE_VALUE)
		hLog = CreateFile(LOG_FILE, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hLog == INVALID_HANDLE_VALUE)
		return;

	SetFilePointer(hLog, 0, NULL, FILE_END);

	if (SUCCEEDED(StringCchPrintfA(szPid, RTL_NUMBER_OF(szPid), "[%d]: ", GetCurrentProcessId())))
		if (SUCCEEDED(StringCchLengthA(szPid, RTL_NUMBER_OF(szPid), &cchSize)))
			WriteFile(hLog, szPid, cchSize, &nWritten, NULL);

	if (SUCCEEDED(StringCchLengthA(szMessage, RTL_NUMBER_OF(szMessage), &cchSize)))
		WriteFile(hLog, szMessage, cchSize, &nWritten, NULL);

	if (pszErrorText && SUCCEEDED(StringCchLengthA(pszErrorText, cchMaxErrorTextSize, &cchSize)))
		WriteFile(hLog, pszErrorText, cchSize, &nWritten, NULL);

	// do not open+close log file at the cost of one leaked file handle
	// CloseHandle(hLog);
}

VOID lprintf(PUCHAR szFormat, ...)
{
	va_list	Args;

	va_start(Args, szFormat);
	lprintf_main(NULL, 0, szFormat, Args);
}

VOID lprintf_err(ULONG uErrorCode, PUCHAR szFormat, ...)
{
	va_list	Args;
	HRESULT	hResult;
	size_t	cchErrorTextSize;
	PUCHAR	pMessage = NULL;
	UCHAR	szMessage[2048];

	memset(szMessage, 0, sizeof(szMessage));

	cchErrorTextSize = FormatMessageA( 
				FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL,
				uErrorCode,
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				(LPSTR)&pMessage,
				0,
				NULL);

	if (!cchErrorTextSize) {

		if (FAILED(StringCchPrintfA(szMessage, RTL_NUMBER_OF(szMessage), " failed with error %d\n", uErrorCode)))
			return;
	} else {

		hResult = StringCchPrintfA(
				szMessage, 
				RTL_NUMBER_OF(szMessage), 
				" failed with error %d: %s%s",
				uErrorCode, 
				pMessage,
				((cchErrorTextSize >= 1) && (0x0a == pMessage[cchErrorTextSize - 1])) ? "" : "\n");
		LocalFree(pMessage);

		if (FAILED(hResult))
			return;
	}

	va_start(Args, szFormat);
	lprintf_main(szMessage, RTL_NUMBER_OF(szMessage), szFormat, Args);
}

#endif