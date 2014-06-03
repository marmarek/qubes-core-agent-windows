#include "wait-for-volume.h"
#include <dbt.h>
#include <winioctl.h>

// All functions except WaitForVolume run in a separate thread.

// thread-local storage
__declspec(thread)
static DWORD g_NewVolumeBitmask = 0;

// Notification thread's window message pump.
static void MessagePump(void)
{
	MSG msg;
	int retval;

	while ((retval = GetMessage(&msg, NULL, 0, 0)) != 0)
	{
		if (retval == -1)
		{
			perror("GetMessage");
			break;
		}
		else
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
}

static BOOL DoRegisterDeviceInterface(
	IN GUID interfaceClassGuid,
	IN HWND hWnd,
	OUT HDEVNOTIFY *deviceNotify
	)
{
	DEV_BROADCAST_DEVICEINTERFACE notificationFilter;

	ZeroMemory(&notificationFilter, sizeof(notificationFilter));
	notificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
	notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	notificationFilter.dbcc_classguid = interfaceClassGuid;

	*deviceNotify = RegisterDeviceNotification(
		hWnd,                       // events recipient
		&notificationFilter,        // type of device
		DEVICE_NOTIFY_WINDOW_HANDLE // type of recipient handle
		);

	if (NULL == *deviceNotify)
	{
		perror("RegisterDeviceNotification");
		return FALSE;
	}

	return TRUE;
}

static INT_PTR WINAPI DevNotifyWndProc(
	HWND hWnd,
	UINT message,
	WPARAM wParam,
	LPARAM lParam
	)
{
	LRESULT retval = 1;
	static HDEVNOTIFY hDeviceNotify;
	PDEV_BROADCAST_HDR arrival;
	PDEV_BROADCAST_VOLUME volume;

	switch (message)
	{
	case WM_CREATE:
		if (!DoRegisterDeviceInterface(
			GUID_DEVINTERFACE_DISK,
			hWnd,
			&hDeviceNotify))
		{
			perror("DoRegisterDeviceInterfaceToHwnd");
			ExitThread(1);
		}

		break;

	case WM_DEVICECHANGE:
	{
							PDEV_BROADCAST_DEVICEINTERFACE b = (PDEV_BROADCAST_DEVICEINTERFACE)lParam;

							switch (wParam)
							{
							case DBT_DEVICEARRIVAL:
								arrival = (PDEV_BROADCAST_HDR)lParam;
								logf("DBT_DEVICEARRIVAL: type %d", arrival->dbch_devicetype);
								if (arrival->dbch_devicetype == DBT_DEVTYP_VOLUME)
								{
									// New volume mounted.
									volume = (PDEV_BROADCAST_VOLUME)lParam;
									logf("mask: 0x%x", volume->dbcv_unitmask);
									// Each bit corresponds to disk letter for the newly mounted volume.
									g_NewVolumeBitmask = volume->dbcv_unitmask;
									goto close;
								}
								break;
							case DBT_DEVICEREMOVECOMPLETE:
								logf("DBT_DEVICEREMOVECOMPLETE");
								break;
							case DBT_DEVNODES_CHANGED:
								logf("DBT_DEVNODES_CHANGED");
								break;
							default:
								logf("Unknown device change: %d", wParam);
								break;
							}
	}
		break;

	close:
	case WM_CLOSE:
		if (!UnregisterDeviceNotification(hDeviceNotify))
		{
			perror("UnregisterDeviceNotification");
		}
		DestroyWindow(hWnd);
		break;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	default:
		// Send all other messages on to the default windows handler.
		retval = DefWindowProc(hWnd, message, wParam, lParam);
		break;
	}

	return retval;
}

#define WND_CLASS_NAME L"QDevNotifyWindowClass"

static BOOL InitWindowClass(void)
{
	WNDCLASSEX wndClass = { 0 };

	wndClass.cbSize = sizeof(WNDCLASSEX);
	wndClass.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
	wndClass.hInstance = GetModuleHandle(NULL);
	wndClass.lpfnWndProc = DevNotifyWndProc;
	wndClass.lpszClassName = WND_CLASS_NAME;
	wndClass.lpszMenuName = NULL;
	wndClass.hIconSm = wndClass.hIcon;


	if (!RegisterClassEx(&wndClass))
	{
		perror("RegisterClassEx");
		return FALSE;
	}

	return TRUE;
}

// param: PWCHAR, new volume's disk letter will be stored there.
static DWORD WINAPI DevNotifyThread(PVOID param)
{
	HWND hWnd;
	DWORD diskIndex = 0;

	g_NewVolumeBitmask = 0;

	if (!InitWindowClass())
		return 1;

	hWnd = CreateWindowEx(
		WS_EX_CLIENTEDGE | WS_EX_APPWINDOW,
		WND_CLASS_NAME,
		L"QDevNotify",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, 0,
		640, 480,
		NULL, NULL,
		GetModuleHandle(NULL),
		NULL);

	if (hWnd == NULL)
	{
		perror("CreateWindowEx: main appwindow hWnd");
		return 2;
	}

	MessagePump();

	if (g_NewVolumeBitmask == 0) // failed?
		return 3;

	// Translate bitmask into disk letter.
	BitScanForward(&diskIndex, g_NewVolumeBitmask);
	*(PWCHAR)param = L'A' + (WCHAR)diskIndex;

	return 0;
}

//////////////////

// Waits until a new volume is recognized by the system and automounted.
// Returns volume's disk letter or 0 if failed.
WCHAR WaitForVolumeArrival(void)
{
	WCHAR diskLetter;
	HANDLE notifyThread;
	DWORD exitCode;

	// Create a thread that will receive device notifications, pass pointer to disk letter as parameter.
	notifyThread = CreateThread(NULL, 0, DevNotifyThread, &diskLetter, 0, NULL);
	if (!notifyThread)
	{
		perror("CreateThread");
		return 0;
	}

	// Wait for the thread to exit, with timeout.
	// 60 seconds should be more than enough to initialize a volume (without format).
	if (WaitForSingleObject(notifyThread, 60000) != WAIT_OBJECT_0)
	{
		errorf("wait for the notify thread failed or timed out");
		TerminateThread(notifyThread, 0);
		return 0;
	}

	// Check thread's exit code.
	if (!GetExitCodeThread(notifyThread, &exitCode))
	{
		perror("GetExitCodeThread");
		return 0;
	}

	logf("Notify thread exit code: %d", exitCode);
	if (exitCode != 0)
	{
		errorf("Notify thread failed");
		return 0;
	}

	// All OK.
	return diskLetter;
}
