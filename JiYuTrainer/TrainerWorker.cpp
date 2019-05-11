#include "stdafx.h"
#include "TrainerWorker.h"
#include "JiYuTrainer.h"
#include "AppPublic.h"
#include "NtHlp.h"
#include "PathHelper.h"
#include "StringHlp.h"
#include "MsgCenter.h"
#include "StringSplit.hpp"
#include "DriverLoader.h"
#include "KernelUtils.h"
#include "SysHlp.h"
#include "SettingHlp.h"

extern JTApp *currentApp;
TrainerWorkerInternal * TrainerWorkerInternal::currentTrainerWorker = nullptr;

extern NtQuerySystemInformationFun NtQuerySystemInformation;

using namespace std;

#define TIMER_RESET_PID 40115
#define TIMER_CK 40116

PSYSTEM_PROCESSES current_system_process = NULL;

TrainerWorkerInternal::TrainerWorkerInternal()
{
	currentTrainerWorker = this;
}
TrainerWorkerInternal::~TrainerWorkerInternal()
{
	if (hDesktop) {
		CloseDesktop(hDesktop);
		hDesktop = NULL;
	}

	StopInternal();
	ClearProcess();

	currentTrainerWorker = nullptr;
}

void TrainerWorkerInternal::Init()
{
	hDesktop = OpenDesktop(L"Default", 0, FALSE, DESKTOP_ENUMERATE);
	UpdateScreenSize();

	if (LocateStudentMainLocation()) JTLog(L"已定位极域电子教室位置： %s", _StudentMainPath.c_str());
	else JTLogWarn(L"无法定位极域电子教室位置");

	UpdateState();
	UpdateStudentMainInfo(false);

	InitSettings();
}
void TrainerWorkerInternal::InitSettings()
{
	SettingHlp*settings = currentApp->GetSettings();
	setAutoIncludeFullWindow = settings->GetSettingBool(L"AutoIncludeFullWindow");
	setCkInterval = currentApp->GetSettings()->GetSettingInt(L"CKInterval", 3100);
	if (setCkInterval < 1000 || setCkInterval > 10000) setCkInterval = 3000;
}
void TrainerWorkerInternal::UpdateScreenSize()
{
	screenWidth = GetSystemMetrics(SM_CXSCREEN);
	screenHeight = GetSystemMetrics(SM_CYSCREEN);
}

void TrainerWorkerInternal::Start()
{
	if (!_Running) 
	{
		_Running = true;

		//Settings
		
		SetTimer(hWndMain, TIMER_CK, setCkInterval, TimerProc);
		SetTimer(hWndMain, TIMER_RESET_PID, 2000, TimerProc);

		UpdateState();

	}
}
void TrainerWorkerInternal::Stop()
{
	if (_Running) {
		StopInternal();
		UpdateState();
	}
}
void TrainerWorkerInternal::StopInternal() {
	if (_Running) {
		_Running = false;
		KillTimer(hWndMain, TIMER_CK);
		KillTimer(hWndMain, TIMER_RESET_PID);
	}
}

void TrainerWorkerInternal::SetUpdateInfoCallback(TrainerWorkerCallback *callback)
{
	if (callback) {
		_Callback = callback;
		hWndMain = callback->GetMainHWND();
	}
}

void TrainerWorkerInternal::HandleMessageFromVirus(LPCWSTR buf)
{
	wstring act(buf);
	vector<wstring> arr;
	SplitString(act, arr, L":");
	if (arr.size() >= 2)
	{
		if (arr[0] == L"hkb")
		{
			if (arr[1] == L"succ") {
				_StudentMainControlled = true;
				JTLogInfo(L"Receive ctl success message ");
				if (_Callback) _Callback->OnBeforeSendStartConf();
				UpdateState();
			}
			else if (arr[1] == L"immck") {
				RunCk();
				JTLogInfo(L"Receive  immck message ");
			}
		}
		else if (arr[0] == L"wcd")
		{
			//wwcd
			int wcdc = _wtoi(arr[1].c_str());
			if (wcdc % 20 == 0)
				JTLogInfo(L"Receive  watch dog message %d ", wcdc);
		}
	}
}
void TrainerWorkerInternal::SendMessageToVirus(LPCWSTR buf)
{
	MsgCenterSendToVirus(buf, hWndMain);
}

bool TrainerWorkerInternal::Kill(bool autoWork)
{
	if (_StudentMainPid <= 4) {
		JTLogError(L"未找到极域主进程");
		return false;
	}
	if (_StudentMainControlled && (autoWork || MessageBox(hWndMain, L"您是否希望使用病毒进行爆破？", L"JiYuTrainer - 提示", MB_ICONASTERISK | MB_YESNO) == IDYES)) {
		//Stop sginal
		SendMessageToVirus(L"ss2:0");
		return true;
	}

	HANDLE hProcess;
	NTSTATUS status = MOpenProcessNt(_StudentMainPid, &hProcess);
	if (!NT_SUCCESS(status)) {
		if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			_StudentMainPid = 0;
			_StudentMainControlled = false;
			UpdateState();
			UpdateStudentMainInfo(!autoWork);
			return true;
		}
		else {
			JTLogError(L"打开进程错误：0x%08X，请手动结束", status);
			return false;
		}
	}
	status = MTerminateProcessNt(0, hProcess);
	if (NT_SUCCESS(status)) {
		_StudentMainPid = 0;
		_StudentMainControlled = false;
		UpdateState();
		UpdateStudentMainInfo(!autoWork);
		CloseHandle(hProcess);
		return TRUE;
	}
	else {
		if (status == STATUS_ACCESS_DENIED) goto FORCEKILL;
		else if (status != STATUS_INVALID_CID && status != STATUS_INVALID_HANDLE) {
			JTLogError(L"结束进程错误：0x%08X，请手动结束", status);
			if (!autoWork)
				MessageBox(hWndMain, L"无法结束极域电子教室，您需要使用其他工具手动结束", L"JiYuTrainer - 错误", MB_ICONERROR);;
			CloseHandle(hProcess);
			return false;
		}
		else if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			_StudentMainPid = 0;
			_StudentMainControlled = false;
			UpdateState();
			UpdateStudentMainInfo(!autoWork);
			CloseHandle(hProcess);
			return true;
		}
	}

FORCEKILL:
	if (DriverLoaded() && MessageBox(hWndMain, L"普通无法结束极域，是否调用驱动结束极域？\n（驱动可能不稳定，请慎用。您也可以使用 PCHunter 等安全软件进行强杀）", L"JiYuTrainer - 提示", MB_ICONEXCLAMATION | MB_YESNO) == IDYES)
	{
		if (KForceKill(_StudentMainPid, &status)) {
			_StudentMainPid = 0;
			_StudentMainControlled = false;
			UpdateState();
			UpdateStudentMainInfo(!autoWork);
			CloseHandle(hProcess);
			return true;
		}
		else if(!autoWork) MessageBox(hWndMain, L"驱动也无法结束，请使用 PCHunter 结束它吧！", L"错误", MB_ICONEXCLAMATION);
	}
	CloseHandle(hProcess);
	return false;
}
bool TrainerWorkerInternal::Rerun(bool autoWork)
{
	if (!_StudentMainFileLocated) {
		JTLogWarn(L"未找到极域电子教室");
		if (!autoWork && _Callback)
			_Callback->OnSimpleMessageCallback(L"<h5>我们无法在此计算机上找到极域电子教室，您需要手动启动</h5>");
		return false;
	}
	return SysHlp::RunApplication(_StudentMainPath.c_str(), NULL);
}
bool TrainerWorkerInternal::RunOperation(TrainerWorkerOp op) {
	switch (op)
	{
	case TrainerWorkerOpVirusBoom:
		MsgCenterSendToVirus(L"ss:0", hWndMain);
		return true;
	case TrainerWorkerOpVirusQuit:
		MsgCenterSendToVirus((LPWSTR)L"hk:ckend", hWndMain);
		return true;
	case TrainerWorkerOp1:
		WCHAR s[300]; swprintf_s(s, L"hk:path:%s", currentApp->GetFullPath());
		MsgCenterSendToVirus(s, hWndMain);
		swprintf_s(s, L"hk:inipath:%s", currentApp->GetPartFullPath(PART_INI));
		MsgCenterSendToVirus(s, hWndMain);
		break;
	case TrainerWorkerOpForceUnLoadVirus: {
		UnLoadAllVirus();
		break;
	}
	}
	return false;
}

bool TrainerWorkerInternal::RunCk()
{
	_LastResolveWindowCount = 0;
	_LastResoveBroadcastWindow = false;
	_LastResoveBlackScreenWindow = false;
	_FirstBlackScreenWindow = false;

	EnumDesktopWindows(hDesktop, EnumWindowsProc, (LPARAM)this);

	MsgCenterSendHWNDS(hWndMain);

	return _LastResolveWindowCount > 0;
}
void TrainerWorkerInternal::RunResetPid()
{
	FlushProcess();

	//CK GET STAT DELAY
	if (_NextLoopGetCkStat) {
		_NextLoopGetCkStat = false;
		SendMessageToVirus(L"hk:ckstat");
	}

	//Find jiyu main process
	DWORD newPid = 0;
	if (LocateStudentMain(&newPid)) { //找到极域

		if (_StudentMainPid != newPid)
		{
			_StudentMainPid = newPid;

			if (InstallVirus()) {
				_VirusInstalled = true;
				_NextLoopGetCkStat = true;

				JTLog(L"向 StudentMain.exe [%d] 注入DLL成功", newPid);
			}
			else  JTLogError(L"向 StudentMain.exe [%d] 注入DLL失败", newPid);

			JTLog(L"已锁定 StudentMain.exe [%d]", newPid);

			UpdateState();
			UpdateStudentMainInfo(false);
		}
	}
	else { //没有找到

		if (_StudentMainPid != 0)
		{
			_StudentMainPid = 0;

			JTLog(L"极域主进程 StudentMain.exe 已退出", newPid);

			UpdateState();
			UpdateStudentMainInfo(false);
		}

	}

	newPid = 0;
	if (LocateMasterHelper(&newPid)) {
		if (_MasterHelperPid != newPid)
		{
			_MasterHelperPid = newPid;
			if (InstallVirusForMaster()) JTLog(L"向 MasterHelper.exe [%d] 注入DLL成功", newPid);
			else  JTLogError(L"向 MasterHelper.exe [%d] 注入DLL失败", newPid);
		}
	}
	else {
		_MasterHelperPid = 0;
	}
}

bool TrainerWorkerInternal::FlushProcess()
{
	ClearProcess();

	DWORD dwSize = 0;
	NTSTATUS status = NtQuerySystemInformation(SystemProcessInformation, NULL, 0, &dwSize);
	if (status == STATUS_INFO_LENGTH_MISMATCH && dwSize > 0)
	{
		current_system_process = (PSYSTEM_PROCESSES)malloc(dwSize);
		status = NtQuerySystemInformation(SystemProcessInformation, current_system_process, dwSize, 0);
		if (!NT_SUCCESS(status)) {
			JTLogError(L"NtQuerySystemInformation failed ! 0x%08X", status);
			return false;
		}
	}

	return true;
}
void TrainerWorkerInternal::ClearProcess()
{
	if (current_system_process) {
		free(current_system_process);
		current_system_process = NULL;
	}
}
bool TrainerWorkerInternal::FindProcess(LPCWSTR processName, DWORD * outPid)
{
	return false;
}
bool TrainerWorkerInternal::KillProcess(DWORD pid, bool force)
{
	HANDLE hProcess;
	NTSTATUS status = MOpenProcessNt(_StudentMainPid, &hProcess);
	if (!NT_SUCCESS(status)) {
		if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			JTLogError(L"找不到进程 [%d] ", pid);
			return true;
		}
		else {
			JTLogError(L"打开进程 [%d] 错误：0x%08X，请手动结束", pid);
			return false;
		}
	}
	status = MTerminateProcessNt(0, hProcess);
	if (NT_SUCCESS(status)) {
		JTLog(L"进程 [%d] 结束成功", pid);
		CloseHandle(hProcess);
		return TRUE;
	}
	else {
		if (status == STATUS_ACCESS_DENIED) {
			if (force) goto FORCEKILL;
			else JTLogError(L"结束进程 [%d] 错误：拒绝访问。可尝试使用驱动结束", pid);
			CloseHandle(hProcess);
		}
		else if (status != STATUS_INVALID_CID && status != STATUS_INVALID_HANDLE) {
			JTLogError(L"结束进程 [%d] 错误：0x%08X，请手动结束", pid);
			CloseHandle(hProcess);
			return false;
		}
		else if (status == STATUS_INVALID_CID || status == STATUS_INVALID_HANDLE) {
			JTLogError(L"找不到进程 [%d] ", pid);
			CloseHandle(hProcess);
			return true;
		}
	}
FORCEKILL:
	if (DriverLoaded())
	{
		if (KForceKill(_StudentMainPid, &status)) {
			JTLog(L"进程 [%d] 强制结束成功", pid);
			CloseHandle(hProcess);
			return true;
		}
		else {
			JTLogError(L"驱动强制结束进程 [%d] 错误：0x%08X", pid);
		}
	}
	else JTLog(L"驱动未加载，无法强制结束进程");
	CloseHandle(hProcess);
	return false;
}
bool TrainerWorkerInternal::LocateStudentMainLocation()
{
	//注册表查找 极域 路径
	HKEY hKey;
	LRESULT lastError = RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"SOFWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\e-Learing Class V6.0", 0, KEY_READ, &hKey);
	if (lastError == ERROR_SUCCESS) {

		DWORD dwType = REG_SZ;
		WCHAR szData[MAX_PATH];
		DWORD dwSize = MAX_PATH * sizeof(WCHAR);
		lastError = RegQueryValueEx(hKey, L"DisplayIcon", 0, &dwType, (LPBYTE)szData, &dwSize);
		if (lastError = ERROR_SUCCESS) {
			if (Path::Exists(szData)) {
				_StudentMainPath = szData;
				_StudentMainFileLocated = true;
				return true;
			}
			else JTLog(L"读取注册表 [DisplayIcon] 获得了一个无效的极域电子教室路径 : %s", szData);
		}
		else JTLogWarn(L"RegQueryValueEx Failed : %d [in %s]", lastError, L"LocateStudentMainLocation RegQueryValueEx(hKey, L\"DisplayIcon\", 0, &dwType, (LPBYTE)szData, &dwSize);");

		RegCloseKey(hKey);
	}
	else JTLogWarn(L"RegOpenKeyEx Failed : %d [in %s]", lastError, L"LocateStudentMainLocation");

	//直接尝试查找
	LPCWSTR mabeInHere[4] = {
		L"c:\\Program Files\\Mythware\\极域课堂管理系统软件V6.0 2016 豪华版\\StudentMain.exe",
		L"C:\\Program Files\\Mythware\\e-Learning Class\\StudentMain.exe",
		L"C:\\e-Learning Class\\StudentMain.exe",
		L"c:\\极域课堂管理系统软件V6.0 2016 豪华版\\StudentMain.exe",
	};
	for (int i = 0; i < 4; i++) {
		if (Path::Exists(mabeInHere[i])) {
			_StudentMainPath = mabeInHere[i];
			_StudentMainFileLocated = true;
			return true;
		}
	}

	return false;
}
bool TrainerWorkerInternal::LocateStudentMain(DWORD *outFirstPid)
{
	if (current_system_process)
	{
		bool done = false;
		for (PSYSTEM_PROCESSES p = current_system_process; !done; p = PSYSTEM_PROCESSES(PCHAR(p) + p->NextEntryOffset)) {
			if (p->ImageName.Length && StrEqual(p->ImageName.Buffer, L"StudentMain.exe"))
			{
				if (outFirstPid)*outFirstPid = (DWORD)p->ProcessId;
				if (!_StudentMainFileLocated) {
					//直接通过EXE确定进程位置
					HANDLE hProcess;
					if (NT_SUCCESS(MOpenProcessNt((DWORD)p->ProcessId, &hProcess))) {
						WCHAR buffer[MAX_PATH];
						if (MGetProcessFullPathEx(hProcess, buffer)) {
							_StudentMainPath = buffer;
							_StudentMainFileLocated = true;
							JTLog(L"通过进程 StudentMain.exe [%d] 定位到位置： %s", (DWORD)p->ProcessId, _StudentMainPath);
							CloseHandle(hProcess);
							return true;
						}
						CloseHandle(hProcess);
					}
				}

				return true;
			}
			done = p->NextEntryOffset == 0;
		}
	}
	return false;
}
bool TrainerWorkerInternal::LocateMasterHelper(DWORD *outFirstPid)
{
	if (current_system_process)
	{
		bool done = false;
		for (PSYSTEM_PROCESSES p = current_system_process; !done; p = PSYSTEM_PROCESSES(PCHAR(p) + p->NextEntryOffset)) {
			if (p->ImageName.Length && StrEqual(p->ImageName.Buffer, L"MasterHelper.exe"))
			{
				if (outFirstPid)*outFirstPid = (DWORD)p->ProcessId;
				return true;
			}
			done = p->NextEntryOffset == 0;
		}
	}
	return false;
}

void TrainerWorkerInternal::UpdateStudentMainInfo(bool byUser)
{
	if (_Callback)
		_Callback->OnUpdateStudentMainInfo(_StudentMainPid > 4, _StudentMainPath.c_str(), _StudentMainPid, byUser);
}
void TrainerWorkerInternal::UpdateState()
{
	if (_Callback) 
	{
		TrainerWorkerCallback::TrainerStatus status;
		if (_StudentMainPid > 4) {
			if (_StudentMainControlled) {
				_StatusTextMain = L"已控制极域电子教室";

				if (_LastResoveBroadcastWindow)
				{
					_StatusTextMore = L"已调整极域电子教室广播窗口";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControlledAndUnLocked;
				}
				else if (_LastResoveBlackScreenWindow)
				{
					_StatusTextMore = L"已处理极域电子教室黑屏窗口";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControlledAndUnLocked;
				}

				if (!_Running) {
					_StatusTextMain += L" 但控制器未启动";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusStopped;
				}
				else if (!_LastResoveBlackScreenWindow && !_LastResoveBroadcastWindow) {
					_StatusTextMore = L"您可以放心继续您的工作";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControlled;
				}
			}
			else {
				_StatusTextMain = L"无法控制极域电子教室";
				if (!_Running) {
					_StatusTextMain = L"控制器未启动";
					_StatusTextMore = L"您已手动停止控制器<br / >当前不会对极域做任何操作";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusStopped;
				}
				else if (_VirusInstalled) {
					_StatusTextMore = L"毒已插入极域，但未正常运行<br / ><span style=\"color:#f41702\">您可能需要重新启动极域</span>";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusUnknowProblem;
				}
				else {
					_StatusTextMore = L"向极域电子教室插入病毒失败<br / >错误详情请查看 <a id=\"link_log\">日志</a>";
					status = TrainerWorkerCallback::TrainerStatus::TrainerStatusControllFailed;
				}
			}
		}
		else {
			_StatusTextMain = L"极域电子教室未运行";
			if (!_Running) {
				_StatusTextMain = L"极域电子教室未运行 并且控制器未启动";
				_StatusTextMore = L"您已手动停止控制器<br / >当前不会检测极域的运行";
				status = TrainerWorkerCallback::TrainerStatus::TrainerStatusStopped;
			}
			else if (_StudentMainFileLocated) {
				status = TrainerWorkerCallback::TrainerStatus::TrainerStatusNotRunning;
				_StatusTextMore = L"已在此计算机上找到极域电子教室<br / >你可以点击 <b>下方按钮< / b> 运行它";
			}
			else {
				status = TrainerWorkerCallback::TrainerStatus::TrainerStatusNotFound;
				_StatusTextMore = L"未在此计算机上找到极域电子教室";
			}
		}

		_Callback->OnUpdateState(status, _StatusTextMain.c_str(), _StatusTextMore.c_str());
	}
}

bool TrainerWorkerInternal::InstallVirus()
{
	return InjectDll(_StudentMainPid, currentApp->GetPartFullPath(PART_HOOKER));
}
bool TrainerWorkerInternal::InstallVirusForMaster()
{
	return InjectDll(_MasterHelperPid, currentApp->GetPartFullPath(PART_HOOKER));
}
bool TrainerWorkerInternal::InjectDll(DWORD pid, LPCWSTR dllPath)
{
	HANDLE hRemoteProcess;
	//打开进程
	NTSTATUS ntStatus = MOpenProcessNt(pid, &hRemoteProcess);
	if (!NT_SUCCESS(ntStatus)) {
		JTLogError(L"注入病毒失败！打开进程失败：0x%08X", ntStatus);
		return FALSE;
	}

	wchar_t *pszLibFileRemote;

	//使用VirtualAllocEx函数在远程进程的内存地址空间分配DLL文件名空间
	pszLibFileRemote = (wchar_t *)VirtualAllocEx(hRemoteProcess, NULL, sizeof(wchar_t) * (lstrlen(dllPath) + 1), MEM_COMMIT, PAGE_READWRITE);

	//使用WriteProcessMemory函数将DLL的路径名写入到远程进程的内存空间
	WriteProcessMemory(hRemoteProcess, pszLibFileRemote, (void *)dllPath, sizeof(wchar_t) * (lstrlen(dllPath) + 1), NULL);

	//##############################################################################
		//计算LoadLibraryA的入口地址
	PTHREAD_START_ROUTINE pfnStartAddr = (PTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandle(TEXT("Kernel32")), "LoadLibraryW");
	//(关于GetModuleHandle函数和GetProcAddress函数)

	//启动远程线程LoadLibraryW，通过远程线程调用创建新的线程
	HANDLE hRemoteThread;
	if ((hRemoteThread = CreateRemoteThread(hRemoteProcess, NULL, 0, pfnStartAddr, pszLibFileRemote, 0, NULL)) == NULL)
	{
		JTLogError(L"注入线程失败! 错误：CreateRemoteThread %d", GetLastError());
		return FALSE;
	}

	// 释放句柄

	CloseHandle(hRemoteProcess);
	CloseHandle(hRemoteThread);

	return true;
}
bool TrainerWorkerInternal::UnInjectDll(DWORD pid, LPCWSTR moduleName)
{
	HANDLE hProcess;
	//打开进程
	NTSTATUS ntStatus = MOpenProcessNt(pid, &hProcess);
	if (!NT_SUCCESS(ntStatus)) {
		JTLogError(L"卸载病毒失败！打开进程失败：0x%08X", ntStatus);
		return FALSE;
	}
	DWORD pszLibFileRemoteSize = sizeof(wchar_t) * (lstrlen(moduleName) + 1);
	wchar_t *pszLibFileRemote;
	//使用VirtualAllocEx函数在远程进程的内存地址空间分配DLL文件名空间
	pszLibFileRemote = (wchar_t *)VirtualAllocEx(hProcess, NULL, pszLibFileRemoteSize, MEM_COMMIT, PAGE_READWRITE);
	//使用WriteProcessMemory函数将DLL的路径名写入到远程进程的内存空间
	WriteProcessMemory(hProcess, pszLibFileRemote, (void *)moduleName, pszLibFileRemoteSize, NULL);

	DWORD dwHandle;
	DWORD dwID;
	LPVOID pFunc = GetProcAddress(GetModuleHandle(TEXT("Kernel32")), "GetModuleHandleW");
	HANDLE hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pFunc, pszLibFileRemote, 0, &dwID);
	if (!hThread) {
		JTLogError(L"卸载病毒失败！创建远程线程失败：%d", GetLastError());
		return FALSE;
	}

	// 等待GetModuleHandle运行完毕
	WaitForSingleObject(hThread, INFINITE);
	// 获得GetModuleHandle的返回值
	GetExitCodeThread(hThread, &dwHandle);
	// 释放目标进程中申请的空间
	VirtualFreeEx(hProcess, pszLibFileRemote, pszLibFileRemoteSize, MEM_DECOMMIT);
	CloseHandle(hThread);
	// 使目标进程调用FreeLibrary，卸载DLL
	pFunc = GetProcAddress(GetModuleHandle(TEXT("Kernel32")), "FreeLibrary"); ;
	hThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)pFunc, (LPVOID)dwHandle, 0, &dwID);
	if (!hThread) {
		JTLogError(L"卸载病毒失败！创建远程线程失败：%d", GetLastError());
		return FALSE;
	}
	
	// 等待FreeLibrary卸载完毕
	WaitForSingleObject(hThread, INFINITE);
	CloseHandle(hThread);
	CloseHandle(hProcess);

	return true;
}
bool TrainerWorkerInternal::UnLoadAllVirus()
{
	if (_MasterHelperPid > 4) 
		if(UnInjectDll(_MasterHelperPid, L"JiYuTrainerHooks.dll"))
			JTLog(L"已强制卸载 MasterHelper 病毒");
	if (_StudentMainPid > 4)
		if (UnInjectDll(_StudentMainPid, L"JiYuTrainerHooks.dll"))
			JTLog(L"已强制卸载 StudentMain 病毒");

	return false;
}

void TrainerWorkerInternal::SwitchFakeFull()
{
	if (_FakeFull)_FakeFull = false;
	else _FakeFull = true;
	FakeFull(_FakeFull);
}
void TrainerWorkerInternal::FakeFull(bool fk) {
	if (_CurrentBroadcastWnd) {
		if (fk) {
			SetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
			SetWindowLong(_CurrentBroadcastWnd, GWL_STYLE, GetWindowLong(_CurrentBroadcastWnd, GWL_STYLE) ^ (WS_BORDER | WS_OVERLAPPEDWINDOW));
			SetWindowPos(_CurrentBroadcastWnd, HWND_TOPMOST, 0, 0, screenWidth, screenHeight, SWP_SHOWWINDOW);
			SendMessage(_CurrentBroadcastWnd, WM_SIZE, 0, MAKEWPARAM(screenWidth, screenHeight));
			/*HWND jiYuGBDeskRdWnd = FindWindowExW(currentGbWnd, NULL, NULL, L"TDDesk Render Window");
			if (jiYuGBDeskRdWnd != NULL) {

			}*/
			_FakeBroadcastFull = true;
			JTLog(L"调整广播窗口假装全屏状态");
		}
		else {
			_FakeBroadcastFull = false;
			FixWindow(_CurrentBroadcastWnd, (LPWSTR)L"");
			int w = (int)((double)screenWidth * (3.0 / 4.0)), h = (int)((double)screenHeight * (double)(4.0 / 5.0));
			SetWindowPos(_CurrentBroadcastWnd, 0, (screenWidth - w) / 2, (screenHeight - h) / 2, w, h, SWP_NOZORDER | SWP_SHOWWINDOW);
			JTLog(L"取消广播窗口假装全屏状态");
		}
	}
	if (_CurrentBlackScreenWnd) {
		if (fk) {
			SetWindowLong(_CurrentBlackScreenWnd, GWL_EXSTYLE, GetWindowLong(_CurrentBlackScreenWnd, GWL_EXSTYLE) | WS_EX_TOPMOST);
			SetWindowLong(_CurrentBlackScreenWnd, GWL_STYLE, GetWindowLong(_CurrentBlackScreenWnd, GWL_STYLE) ^ (WS_BORDER | WS_OVERLAPPEDWINDOW));
			SetWindowPos(_CurrentBlackScreenWnd, HWND_TOPMOST, 0, 0, screenWidth, screenHeight, SWP_SHOWWINDOW);
			SendMessage(_CurrentBlackScreenWnd, WM_SIZE, 0, MAKEWPARAM(screenWidth, screenHeight));
			SendMessage(_CurrentBlackScreenWnd, WM_SIZE, 0, MAKEWPARAM(screenWidth, screenHeight));
			_FakeBlackScreenFull = true;
			JTLog(L"调整黑屏窗口假装全屏状态");
		}
		else {
			_FakeBlackScreenFull = false;
			FixWindow(_CurrentBlackScreenWnd, (LPWSTR)L"BlackScreen Window");
			JTLog(L"取消黑屏窗口假装全屏状态");
		}
	}
	if (!fk && !_CurrentBlackScreenWnd && !_CurrentBroadcastWnd && (_FakeBlackScreenFull || _FakeBroadcastFull)) {
		_FakeBroadcastFull = false;
		_FakeBlackScreenFull = false;
	}
}
bool TrainerWorkerInternal::ChecIsJIYuWindow(HWND hWnd, LPDWORD outPid, LPDWORD outTid) {
	if (_StudentMainPid == 0) return false;
	DWORD pid = 0, tid = GetWindowThreadProcessId(hWnd, &pid);
	if (outPid) *outPid = pid;
	if (outTid) *outTid = tid;
	return pid == _StudentMainPid;
}
bool TrainerWorkerInternal::CheckIsTargetWindow(LPWSTR text, HWND hWnd) {
	bool b = false;
	if (StrEqual(text, L"屏幕广播") || StrEqual(text, L"屏幕演播室窗口")) {
		b = true;
		_LastResoveBroadcastWindow = true;
		_CurrentBroadcastWnd = hWnd;
		if (_FakeBroadcastFull) return false;
	}
	if (StrEqual(text, L"BlackScreen Window")) {
		b = true;
		_LastResoveBlackScreenWindow = true;
		if (!_FirstBlackScreenWindow) {
			_FirstBlackScreenWindow = true;
			if (_Callback) _Callback->OnResolveBlackScreenWindow();
			//ShowTip(L"发现极域的非法黑屏窗口！", L"已将其处理并关闭，您可以继续您的工作。", 10);
		}
		_CurrentBlackScreenWnd = hWnd;
		if (_FakeBlackScreenFull) return false;
	}
	return b;
}
void TrainerWorkerInternal::FixWindow(HWND hWnd, LPWSTR text)
{
	_LastResolveWindowCount++;
	//Un top
	LONG oldLong = GetWindowLong(hWnd, GWL_EXSTYLE);
	if ((oldLong & WS_EX_TOPMOST) == WS_EX_TOPMOST)
	{
		SetWindowLong(hWnd, GWL_EXSTYLE, oldLong ^ WS_EX_TOPMOST);
		SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
	//Set border and sizeable
	SetWindowLong(hWnd, GWL_STYLE, GetWindowLong(hWnd, GWL_STYLE) | (WS_BORDER | WS_OVERLAPPEDWINDOW));

	if (StrEqual(text, L"BlackScreen Window"))
	{
		oldLong = GetWindowLong(hWnd, GWL_EXSTYLE);

		{
			SetWindowLong(hWnd, GWL_EXSTYLE, oldLong ^ WS_EX_APPWINDOW | WS_EX_NOACTIVATE);
			SetWindowPos(hWnd, 0, 20, 20, 90, 150, SWP_NOZORDER | SWP_DRAWFRAME | SWP_NOACTIVATE);
			ShowWindow(hWnd, SW_HIDE);
		}
	}

	SetWindowPos(hWnd, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOSIZE | SWP_NOMOVE | SWP_DRAWFRAME | SWP_NOACTIVATE);
}

BOOL CALLBACK TrainerWorkerInternal::EnumWindowsProc(HWND hWnd, LPARAM lParam)
{
	TrainerWorkerInternal *self =(TrainerWorkerInternal *)lParam;
	if (IsWindowVisible(hWnd) && self->ChecIsJIYuWindow(hWnd, NULL, NULL)) {
		WCHAR text[32];
		GetWindowText(hWnd, text, 32);
		if (StrEqual(text, L"JiYu Trainer Virus Window")) return TRUE;

		RECT rc;
		GetWindowRect(hWnd, &rc);
		if (self->CheckIsTargetWindow(text, hWnd)) {
			//JiYu window
			MsgCenteAppendHWND(hWnd);
			self->FixWindow(hWnd, text);
		}
		else if (self->setAutoIncludeFullWindow && rc.top == 0 && rc.left == 0 && rc.right == self->screenWidth && rc.bottom == self->screenHeight) {
			//Full window
			MsgCenteAppendHWND(hWnd);
			self->FixWindow(hWnd, text);
		}
	}
	return TRUE;
}
VOID TrainerWorkerInternal::TimerProc(HWND hWnd, UINT message, UINT_PTR iTimerID, DWORD dwTime)
{
	if (currentTrainerWorker != nullptr) 
	{
		if (iTimerID == TIMER_RESET_PID) {
			currentTrainerWorker->RunResetPid();
		}
		if (iTimerID == TIMER_CK) {
			currentTrainerWorker->RunCk();
		}
	}
}
