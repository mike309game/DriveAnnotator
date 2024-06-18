// DriveNoteTestsAgain.cpp : Defines the entry point for the console application.
//

//this is magic, i don't know how it works, but it makes controls look like normal windows 10 ones
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#include "Header.hh"
#include <string>
#include "stdafx.h"
#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <windows.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <GdiPlus.h>
#include <CommCtrl.h>
#include "resource.h"
#include <assert.h>
#include <mutex>
#include <queue>
#include <random>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <CommCtrl.h>
#include <random>

typedef void LoadSaveFunc(HWND hwnd);

INT_PTR CALLBACK WndProc(HWND hWnd, UINT uMessage, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK LoadSaveProc(HWND hWnd, UINT uMessage, WPARAM wParam, LPARAM lParam);
DWORD ThumbProc(LPVOID);
int ChangeDir(Dir* dir, bool autoSel = true);
void DeleteBitmaps(Dir* newDir);
void OnSelectListItem(int idx);

#define CLR_ERRORBG		(RGB(255,214,214))
#define CLR_DESCRIBEDBG (RGB(163,255,199))
#define CLR_INHERITEDBG (RGB(164,124,194))
HBRUSH g_ErrorBgBrush;
HBRUSH g_DescribedBgBrush;
HBRUSH g_InheritedBgBrush;

HWND g_ListView;

HWND g_PrevDirBtn; bool g_PrevDirBtnEnabled;
HWND g_FwrdDirBtn; bool g_FwrdDirBtnEnabled;
HWND g_UpDirBtn; bool g_UpDirBtnEnabled;
HWND g_RandDirBtn;
std::vector<Dir*>			g_History;
std::vector<Dir*>::iterator g_HistoryPos;

HWND g_PathEdit; bool g_PathEditIgnore = true; bool g_PathEditError = false;

HWND g_DescEdit; COLORREF g_DescEditColour = CLR_ERRORBG;
HWND g_InheritSetBtn;
HWND g_ClearDescBtn;

Dir* g_Dir = nullptr;

HBITMAP g_LoadingBm;
HBITMAP g_ErrorBm;

std::mutex g_QueueMutex;
std::mutex g_BmListMutex;
std::atomic_bool g_killThreads;
std::unordered_map<Hash, Dir> g_Map;
std::queue<QueueItem> g_Queue;
std::vector<ListItem> g_ListItems;
int g_ListItemSelected = -1;
HBITMAP* g_BmList = nullptr;

HANDLE g_Threads[THUMBTHREADS];

std::mt19937 g_Rnd;

void SelectListViewItem(int i) {
	ListView_EnsureVisible(g_ListView, i, FALSE);
	ListView_SetItemState(g_ListView, i, LVNI_SELECTED | LVNI_FOCUSED, LVNI_SELECTED | LVNI_FOCUSED);
}

bool IsIndexValid(int i) {
	return (i >= 0) && (i < g_ListItems.size());
}

int ChangeDir(Dir* dir, bool autoSel) {
	assert(dir != nullptr);
	Dir* oldDir = g_Dir;
	OnSelectListItem(-1); //previously selected item doesn't exist anymore
	ListView_DeleteAllItems(g_ListView);
	g_ListItems.clear();
	size_t filesBegin = 0;

	static auto sorter = [](ListItem& a, ListItem& b) -> bool {
		return CompareStringEx(
			LOCALE_NAME_INVARIANT,
			LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
			a.m_DisplayName.c_str(), -1,
			b.m_DisplayName.c_str(), -1,
			NULL, NULL, 0
		) == CSTR_LESS_THAN;
	};

	if (dir->m_Folders) {
		for (Hash* ptr = dir->m_Folders; *ptr != -1; ptr++) {
			auto folder = FindFolder(*ptr);
			ListItem li;
			auto pathStr = std::string(folder->m_Path);
			li.m_DisplayName = Widen(pathStr.substr(pathStr.find_last_of('\\') + 1));
			li.m_File = dynamic_cast<File*>(folder);
			li.m_IsDir = true;
			g_ListItems.push_back(li);
		}
		std::sort(g_ListItems.begin(), g_ListItems.end(), sorter);
		filesBegin = g_ListItems.size(); //points to 1st file idx
	}

	if (dir->m_Files) {
		for (File* ptr = dir->m_Files; ptr->m_Path != 0; ptr++) {
			ListItem li;
			li.m_DisplayName = Widen(ptr->m_Path);
			li.m_File = ptr;
			li.m_IsDir = false;
			g_ListItems.push_back(li);
		}
		std::sort(g_ListItems.begin() + filesBegin, g_ListItems.end(), sorter);
	}

	//if not root folder then insert .. item
	/*if (strcmp(dir->m_Path, ".") != 0) {
		ListItem li;
		auto pathStr = std::string(dir->m_Path);
		pathStr = pathStr.substr(0, pathStr.find_last_of('\\'));
		li.m_DisplayName = L"..";
		li.m_IsDir = true;
		li.m_File = dynamic_cast<File*>(FindFolder(pathStr.c_str()));
		foldersSorted.insert(foldersSorted.begin(), li);
	}*/

	DeleteBitmaps(dir); //sets g_Dir w/o race problems

	ListView_SetItemCount(g_ListView, g_ListItems.size());

	if (autoSel) {
		int idx = 0;
		for (auto& i : g_ListItems) {
			if (i.m_File == dynamic_cast<File*>(oldDir)) {
				SelectListViewItem(idx);
				break;
			}
			idx++;
		}
	}

	SetWindowText(g_PathEdit, Widen(g_Dir->m_Path).c_str());
	
	return 0;
}

std::wstring RealPath(std::wstring path) {
	path[0] = L'F';
	path.insert(path.begin() + 1, L':');
	return path;
}

std::wstring ListItemRealPath(int i) {
	return RealPath(Widen(g_Dir->m_Path) + L"\\" + g_ListItems[i].m_DisplayName);
}

HBITMAP GetThumbnail(int i) {
	HBITMAP ret;// = g_LoadingBm;
	g_BmListMutex.lock();
	ret = g_BmList[i];
	if (ret == BMLOADING) { //still loading
		g_BmListMutex.unlock();
		ret = g_LoadingBm;
	}
	else if(ret == 0){ //hasn't gotten thumb yet
		g_BmList[i] = BMLOADING;
		g_BmListMutex.unlock();
		ret = g_LoadingBm;
		
		QueueItem qi;
		qi.idx = i;
		qi.m_Dest = g_Dir;
		/*name[0] = DRIVELETTER;
		name.insert(name.begin() + 1, ':');*/
		//std::wstring path = Widen(std::string(g_Dir->m_Path)) + L"\\" + g_ListItems[i].m_DisplayName;
		qi.m_Path = ListItemRealPath(i);

		g_QueueMutex.lock();
		g_Queue.push(qi);
		g_QueueMutex.unlock();
	}
	else { //when list has good bitmap
		g_BmListMutex.unlock();
	}
	
	return ret;
}

void DeleteBitmaps(Dir* newDir = g_Dir) {
	g_BmListMutex.lock();
	g_Dir = newDir;
	if (g_BmList != nullptr) {
		for (auto ptr = g_BmList;;) {
			auto hbm = *ptr++;
			if (hbm == BMLOADING || hbm == 0 || hbm == g_ErrorBm) continue;
			if (hbm == BMEND) break;
			assert(DeleteObject(hbm));
		}
		FREE(g_BmList);
	}
	if (newDir != nullptr) {
		auto len = g_ListItems.size();
		g_BmList = (HBITMAP*)ALLOCZERO((len+1) * sizeof(HBITMAP));
		g_BmList[len] = BMEND;
	}
	g_BmListMutex.unlock();
}

void OnSelectListItem(int idx) {
	g_DescEditColour = GetSysColor(COLOR_WINDOW);
	g_ListItemSelected = idx;
	if (idx == -1) {
		//SetWindowText(g_DescEdit, L"<><><>[|NEGATIVE INDEX|]<><><>");
		SetWindowText(g_DescEdit, L"");
		EnableWindow(g_DescEdit, FALSE);
		g_DescEditColour = -1;
		return;
	}
	
	auto& li = g_ListItems[idx];
	auto desc = GetFileDesc(g_Dir, li.m_File);
	SetWindowText(g_DescEdit, Widen(desc).c_str());

	//if item inherits description, don't allow editing, otherwise allow editing
	if (strcmp(*li.m_File->m_Desc, INHDESC) == 0) {
		g_DescEditColour = CLR_INHERITEDBG;
		EnableWindow(g_DescEdit, FALSE);
	}
	else {
		EnableWindow(g_DescEdit, TRUE);
	}
}

void ChangeSelectedListItemDesc(const char* str) {
	if (!IsIndexValid(g_ListItemSelected)) return; //failsafe
	
	auto& i = g_ListItems[g_ListItemSelected];
	if (strlen(str) == 0) *i.m_File->m_Desc = NODESC;
	else *i.m_File->m_Desc = str;
	ListView_Update(g_ListView, g_ListItemSelected); //opportunity to change colour and desc preview

	DWORD selbegin;
	DWORD selend;
	SendMessage(g_DescEdit, EM_GETSEL, (WPARAM)&selbegin, (LPARAM)&selend);
	OnSelectListItem(g_ListItemSelected); //resets caret when typing things 
	SendMessage(g_DescEdit, EM_SETSEL, (WPARAM)&selbegin, (LPARAM)&selend);
}

/*

Iterator starts at a random directory, while directory desc isn't empty, increase iterator,
wrap back to start if the end is reached, break loop if the whole map has been traversed, which
means all folders have been documented

*/
Dir* SelectRandomDir() {
	auto rand = g_Rnd() % g_Map.size();
	auto start = g_Map.begin(); std::advance(start, rand);

	auto it = start;
	do {
		if (strcmp(*((it++)->second.m_Desc), NODESC) == 0) { //break if we got a blank dir
			return &it->second;
		}
		if (it == g_Map.end()) it = g_Map.begin(); //wrap around
	} while (it != start);

	MessageBox(NULL, L"IT'S DONE", L"FINALLY", 0);
	return FindFolder("."); //failsafe
}

void SetInheritRecursive(Dir* dir) {
	assert(dir);
	*dir->m_Desc = INHDESC;
	for (Hash* ptr = dir->m_Folders; *ptr != -1; ptr++) {
		auto f = FindFolder(*ptr);
		assert(f);
		SetInheritRecursive(f);
	}
	if (dir->m_Files) {
		for (File* ptr = dir->m_Files; ptr->m_Path != nullptr; ptr++) {
			*ptr->m_Desc = INHDESC;
		}
	}
}

int PASCAL wWinMain(HINSTANCE hInstance,
	HINSTANCE hPrevInstance,
	LPWSTR lpCmdLine,
	int nCmdShow)
{
	setlocale(LC_ALL, "en_US.UTF-8");
	g_Heap = HeapCreate(HEAP_NO_SERIALIZE, 1024, 0);
	//AllocConsole();
	//freopen("CONOUT$", "w", stdout);

	{
		INITCOMMONCONTROLSEX cce;
		cce.dwSize = sizeof cce;
		cce.dwICC = ICC_WIN95_CLASSES;
		InitCommonControlsEx(&cce); //i don't know if i uninitialise this
	}

	HWND hwnd;

	g_LoadingBm = (HBITMAP)LoadImage(hInstance, MAKEINTRESOURCE(IDB_LOADTHUMB), IMAGE_BITMAP, 32, 32, LR_CREATEDIBSECTION | LR_SHARED);
	g_ErrorBm = (HBITMAP)LoadImage(hInstance, MAKEINTRESOURCE(IDB_THUMBERR), IMAGE_BITMAP, 32, 32, LR_CREATEDIBSECTION | LR_SHARED);

	g_ErrorBgBrush = CreateSolidBrush(CLR_ERRORBG);
	g_DescribedBgBrush = CreateSolidBrush(CLR_DESCRIBEDBG);
	g_InheritedBgBrush = CreateSolidBrush(CLR_INHERITEDBG);

	g_Rnd.seed(time(NULL));
	
	//For converting csv to bin
	/*std::ifstream ifs("mylist.csv");
	LoadFromCsv(ifs);
	ifs.close();
	FILE* fp = fopen("bin0000.bin", "wb");
	SaveMapStl(fp);
	fclose(fp);*/

	//Saving bin
	/*fp = fopen("delme.bin", "wb");
	SaveMapBin(fp);
	fclose(fp);*/
	g_killThreads.store(false);

	//load and unload loop stress test
//	for (;;) {
//		static int i = 0;
//		//postmessage don't work here
//		//SendMessage(hwnd, WM_SETTEXT, 0, (LPARAM)L"Loading");
//		FILE* fp = fopen("resavetest.bin", "rb");
//		LoadFromBin(fp);
//		fclose(fp);
//		fp = fopen("resavetest.bin", "wb");
//		SaveMapBin(fp);
//		fclose(fp);
//		//WM_QUIT fucks up message loop
//		//PostMessage(hwnd, WM_CLOSE, 0, 0);
//		g_Map.clear();
//		Mem_FreeAll();
//		HeapDestroy(g_Heap);
//		g_Heap = HeapCreate(HEAP_NO_SERIALIZE, 1024, 0);
//		std::cout << i++ << std::endl;
//	}

	if(1){ //Load window
		LoadSaveFunc* loadFun = [](HWND hwnd) {
			//postmessage don't work here
			SendMessage(hwnd, WM_SETTEXT, 0, (LPARAM)L"Loading");
			FILE* fp = fopen("FINAL.bin", "rb");
			LoadFromBin(fp);
			fclose(fp);
			//WM_QUIT fucks up message loop
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		};
		DialogBoxParam(hInstance, MAKEINTRESOURCE(IDD_LoadSave), NULL, LoadSaveProc, (LPARAM)loadFun);
	}
	
	//Listing map to sorted text
	/*std::ofstream ofs("SHIT3.txt");
	std::vector<std::string> asd;
	//asd.reserve(g_Map.size());
	int idx = 0;
	for (auto& i : g_Map) {
		asd.push_back(std::string(i.second.m_Path));
	}
	std::sort(asd.begin(), asd.end());
	for (auto& i : asd)
		ofs << i << std::endl;
	ofs.close();*/
	
	for (int i = 0; i < THUMBTHREADS; i++) {
		g_Threads[i] = CreateThread(
			NULL,
			0,
			(LPTHREAD_START_ROUTINE)ThumbProc,
			NULL,
			0,
			NULL
		);
	}
	hwnd = (HWND)DialogBox(hInstance, MAKEINTRESOURCE(IDD_DIALOG1), NULL, WndProc);
	/*UpdateWindow(hwnd);
	ShowWindow(hwnd, nCmdShow);

	MSG msg;
	while (GetMessage(&msg, hwnd, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}*/

	WaitForMultipleObjects(THUMBTHREADS, g_Threads, TRUE, INFINITE);

	if (1){ //Free up things
		LoadSaveFunc* saveFun = [](HWND hwnd) {
			//postmessage don't work here
			SendMessage(hwnd, WM_SETTEXT, 0, (LPARAM)L"Saving");
			FILE* fp = fopen("FINAL.bin", "wb");
			SaveMapBin(fp);
			fclose(fp);
			SendMessage(hwnd, WM_SETTEXT, 0, (LPARAM)L"Delete bitmaps");
			DeleteBitmaps(nullptr);
			SendMessage(hwnd, WM_SETTEXT, 0, (LPARAM)L"Clearing map");
			g_Map.clear();
			SendMessage(hwnd, WM_SETTEXT, 0, (LPARAM)L"Mem freeall");
			Mem_FreeAll();
			SendMessage(hwnd, WM_SETTEXT, 0, (LPARAM)L"Gdi objs");
			DeleteObject(g_LoadingBm);
			DeleteObject(g_ErrorBm);
			DeleteObject(g_ErrorBgBrush);
			DeleteObject(g_DescribedBgBrush);
			DeleteObject(g_InheritedBgBrush);
			SendMessage(hwnd, WM_SETTEXT, 0, (LPARAM)L"Heap byebye");
			HeapDestroy(g_Heap); //destroys all pools and my alloced mem

			//WM_QUIT fucks up message loop
			PostMessage(hwnd, WM_CLOSE, 0, 0);
		};
		DialogBoxParam(hInstance, MAKEINTRESOURCE(IDD_LoadSave), NULL, LoadSaveProc, (LPARAM)saveFun);
	}

	//clearing the map takes way too much time for no apparent reason, and the contents are probably allocated
	//in the process heap either way which is freed instantly upon exit
	//g_Map.clear(); //frees desc objects
	//puts("Cleared map");
	return 0;
}

INT_PTR CALLBACK LoadSaveProc(HWND hWnd,
	UINT uMessage,
	WPARAM wParam,
	LPARAM lParam)
{
	switch (uMessage) {
	case WM_INITDIALOG:
	{
		auto fun = (LoadSaveFunc*)lParam;
		CreateThread(
			NULL,
			0,
			(LPTHREAD_START_ROUTINE)(*fun),
			hWnd,
			0,
			NULL
		);
		break;
	}
	}
	return DefWindowProc(hWnd, uMessage, wParam, lParam);
}

DWORD ThumbProc(LPVOID) {
	//am i doing this right?
	CoInitializeEx(NULL, COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
	for (;;) {
		if (g_killThreads.load()) break;
		g_QueueMutex.lock();
		if (!g_Queue.empty()) {
			auto q = g_Queue.front();
			g_Queue.pop();
			g_QueueMutex.unlock();

			//default to error bmp
			
			//don't do this because then we can't free the error bitmap on success
			//HBITMAP hbm = (HBITMAP)CopyImage(g_ErrorBm, IMAGE_BITMAP, 32, 32, LR_SHARED | LR_CREATEDIBSECTION);
			HBITMAP hbm = g_ErrorBm;

			IShellItemImageFactory *psi;
			if (SUCCEEDED(SHCreateItemFromParsingName(q.m_Path.c_str(), NULL, IID_PPV_ARGS(&psi)))) {
				SIZE s = { 32, 32 };
				psi->GetImage(s, SIIGBF_RESIZETOFIT, &hbm); //if the function fails, it never overwrites the bitmap
				psi->Release();
			}

			g_BmListMutex.lock();
			if (g_Dir != q.m_Dest) { //throw it away because directory has been changed
				if (hbm != g_ErrorBm) DeleteObject(hbm); //don't delete error bitmap
			} else {
				g_BmList[q.idx] = hbm;
				PostMessage(g_ListView, LVM_UPDATE, (WPARAM)q.idx, 0);
			}
			g_BmListMutex.unlock();
		}
		else {
			g_QueueMutex.unlock();
			Sleep(100);
		}
	}
	CoUninitialize();
	return 0;
}

void UpdateNavButtons() {
	g_PrevDirBtnEnabled = g_HistoryPos != g_History.begin();
	g_FwrdDirBtnEnabled = g_HistoryPos + 1 != g_History.end();
	g_UpDirBtnEnabled = strcmp(g_Dir->m_Path, ".") != 0;

	EnableWindow(g_PrevDirBtn, g_PrevDirBtnEnabled);
	EnableWindow(g_FwrdDirBtn, g_FwrdDirBtnEnabled);
	EnableWindow(g_UpDirBtn, g_UpDirBtnEnabled);
}

void PushNavHistory(Dir* dir) {
	//i don't know why. i really don't know why. but if i don't assign g_HistoryPos to the returned value of insert
	//instead of leaving it as is it becomes a bogus iterator that can't be used. I really, really don't know why.

	//In theory, when having just launched the program, history will contain one item - the root dir - and the iterator
	//will point to that. ++historypos will set itself to the history's end, and then insert will use historypos to append the
	//directory at the end, and since the stored end is not the end anymore, it SHOULD BY ALL MEANS point to an iterator of
	//this just-inserted item. But it's a bogus value the debugger says it's 0xFEEEFEEE, and then increasing this
	//bogus iterator throws an exception.

	//UPDATE: I FIGURED IT OUT reallocations invalidate every stored iterator Lol
	g_HistoryPos = g_History.insert(++g_HistoryPos, dir);
	g_History.erase(g_HistoryPos+1, g_History.end());
	UpdateNavButtons();
}


INT_PTR CALLBACK WndProc(HWND hWnd,
	UINT uMessage,
	WPARAM wParam,
	LPARAM lParam)
{
	switch (uMessage) {
	case WM_CLOSE:
		g_killThreads.store(true);
		EndDialog(hWnd, 0);
		break;
	case WM_CONTEXTMENU: {
		bool showExplorer = false;
		bool showDescSrc = false;
		bool canRecursiveInherit = false;

		HWND wnd = (HWND)wParam;
		if (wnd != g_ListView) return FALSE;
		auto hmenu = LoadMenu(GetModuleHandle(NULL), MAKEINTRESOURCE(IDM_OpenInFs));
		auto hmenureal = GetSubMenu(hmenu, 0); //i don't know why but using just hmenu creates a completely blank option
		if (IsIndexValid(g_ListItemSelected)) {
			auto& i = g_ListItems[g_ListItemSelected];
			showExplorer = true;
			showDescSrc = strcmp(*i.m_File->m_Desc, INHDESC) == 0;
			canRecursiveInherit = i.m_IsDir;
		}
		EnableMenuItem(hmenureal, IDM_OpenInExplorerReal, showExplorer ? MF_ENABLED : MF_GRAYED);
		EnableMenuItem(hmenureal, IDM_ShowDescSource, showDescSrc ? MF_ENABLED : MF_GRAYED);
		EnableMenuItem(hmenureal, IDM_SetInheritRecursive, canRecursiveInherit ? MF_ENABLED : MF_GRAYED);
		TrackPopupMenu(hmenureal, TPM_LEFTALIGN | TPM_RIGHTBUTTON, LOWORD(lParam), HIWORD(lParam), 0, hWnd, NULL);
		DestroyMenu(hmenu);
		break;
	}
	case WM_COMMAND: {
		HWND hwndPossible = (HWND)lParam;
		if (hwndPossible == g_DescEdit) {
			switch (HIWORD(wParam)) {
			case EN_CHANGE: {
				auto len = GetWindowTextLength(g_DescEdit);
				wchar_t* buff = new wchar_t[len+1];
				buff[len] = L'\0';
				GetWindowText(g_DescEdit, buff, len+1);
				ChangeSelectedListItemDesc(Unwiden(buff).c_str());
				delete[] buff;
				break;
			}
			default:
				return FALSE;
			}
			return TRUE;
		}
		else if (hwndPossible == g_PathEdit) {
			switch (HIWORD(wParam)) {
			case EN_CHANGE: {
				wchar_t accum[0xFFFF];
				GetWindowText(g_PathEdit, accum, 0xFFFF);
				auto d = FindFolder(Unwiden(accum).c_str());
				g_PathEditError = d == nullptr;
				if (!g_PathEditIgnore) { //ignore when normally navigating
					if (!g_PathEditError) {
						DWORD selbegin;
						DWORD selend;
						SendMessage(g_PathEdit, EM_GETSEL, (WPARAM)&selbegin, (LPARAM)&selend);
						g_PathEditIgnore = true;
						ChangeDir(d);
						PushNavHistory(d);
						SendMessage(g_PathEdit, EM_SETSEL, (WPARAM)&selbegin, (LPARAM)&selend);
					}
				}
				InvalidateRect(g_PathEdit, NULL, TRUE);
				g_PathEditIgnore = false;
				break;
			}
			default:
				return FALSE;
			}
			return TRUE;
		}
		switch (LOWORD(wParam)) {
		case IDC_PrevDir:
			g_PathEditIgnore = true;
			ChangeDir(*(--g_HistoryPos));
			UpdateNavButtons();
			break;
		case IDC_FwdDir:
			g_PathEditIgnore = true;
			ChangeDir(*(++g_HistoryPos));
			UpdateNavButtons();
			break;
		case IDC_UpDir: {
			/*auto pathStr = std::string(g_Dir->m_Path);
			pathStr = pathStr.substr(0, pathStr.find_last_of('\\'));
			auto folder = FindFolder(pathStr.c_str());*/
			g_PathEditIgnore = true;
			auto folder = FindParentDir(g_Dir);
			ChangeDir(folder);
			PushNavHistory(folder);
			break;
		}
		case IDC_RANDDIR: {
			g_PathEditIgnore = true;
			auto folder = SelectRandomDir();
			ChangeDir(folder, false);
			PushNavHistory(folder);
			break;
		}
		case IDC_Clear:
			ChangeSelectedListItemDesc("");
			break;
		case IDC_SetInherit:
			if (strcmp(g_Dir->m_Path, ".") != 0)
				ChangeSelectedListItemDesc(INHDESC);
			else
				MessageBox(hWnd, L"Don't do that", L"Why", MB_OK);
			break;
		case IDM_OpenInExplorerReal: {
			auto path = RealPath(Widen(g_Dir->m_Path) + L"\\" + g_ListItems[g_ListItemSelected].m_DisplayName);
			_wsystem((L"EXPLORER /select,\"" + path + L"\"").c_str());
			break;
		}
		case IDM_ShowDescSource: {
			auto src = GetDescSource(g_Dir);
			if (src == nullptr) break; //hushy
			auto srcParent = FindParentDir(src);
			if (srcParent == nullptr) break; //hush 2
			ChangeDir(srcParent, false);
			PushNavHistory(srcParent);
			for (int i = 0; i < g_ListItems.size(); i++) {
				if (g_ListItems[i].m_File == dynamic_cast<File*>(src)) {
					SelectListViewItem(i);
					break;
				}
			}
			//assert(0 && "couldn't select source item");
			break;
		}
		case IDM_SetInheritRecursive: {
			auto ret = MessageBox(hWnd, L"Are you absolutely POSITIVE", L"waawa", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
			if (ret == IDYES) {
				auto& i = g_ListItems[g_ListItemSelected];
				SetInheritRecursive(static_cast<Dir*>(i.m_File));
				ListView_Update(g_ListView, g_ListItemSelected);
			}
			break;
		}
		default:
			return FALSE;
			break;
		}
		break;
	}
	case WM_NOTIFY:
		{
			LPNMHDR lpnmh = (LPNMHDR)lParam;
			if (lpnmh->hwndFrom == g_ListView) {
				//MessageBox(NULL, std::to_wstring((int)lpnmh->code).c_str(), L"", 0);
				switch (lpnmh->code) {
				case LVN_GETDISPINFO: {
					LV_DISPINFO *lpdi = (LV_DISPINFO *)lParam;
					if (lpdi->item.iSubItem == 0) {
						if (lpdi->item.mask & LVIF_TEXT) {
							auto& name = g_ListItems[lpdi->item.iItem].m_DisplayName;
							lpdi->item.pszText = (LPWSTR)name.c_str();
							lpdi->item.cchTextMax = name.size();
						}
					}
				}
				case LVN_ITEMCHANGED: {
					LPNMLISTVIEW i = (LPNMLISTVIEW)lParam;
					if ((i->uOldState ^ i->uNewState) & LVIS_SELECTED) {
						//std::cout << i->iItem << std::endl;
						if ((i->uNewState & LVIS_SELECTED))
							OnSelectListItem(i->iItem);
						else
							OnSelectListItem(-1);
					}
					break;
				}
				case LVN_ODCACHEHINT: return 0;
//				case LVN_ODFINDITEM: { //LIST VIEWS ARE BROKEN AND THIS DOESN'T WORK WHEN IT SHOULD ABSOLUTELY BE WORKING
//					LPNMLVFINDITEM lpFindItem = (LPNMLVFINDITEM)lParam;
//					
//					//from what i've observed nobody gaf about if this is not looking for strings
//					if (lpFindItem->lvfi.flags & LVFI_STRING) {
//						for (int i = lpFindItem->iStart; i < g_ListItems.size(); i++) {
//							auto& item = g_ListItems[i];
//							if (lpFindItem->lvfi.flags) {
//								if (CompareStringEx(
//									LOCALE_NAME_INVARIANT,
//									LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS,
//									lpFindItem->lvfi.psz, -1,
//									item.m_DisplayName.c_str(), lstrlenW(lpFindItem->lvfi.psz),
//									NULL, NULL, 0
//								) == CSTR_EQUAL) {
//									return i;
//									
//								}
//							}
//							/*else {
//								if (item.m_DisplayName == lpFindItem->lvfi.psz) {
//									return i;
//								}
//							}*/
//						}
//					}
//					return -1;
//					break;
//				}
				case NM_RETURN: //enter pressed
				case NM_DBLCLK: {
					LPNMITEMACTIVATE item = (LPNMITEMACTIVATE)lParam;
					if (item->iItem == -1) return FALSE; //this can happen and i don't know why
					auto& i = g_ListItems[item->iItem];
					if (i.m_IsDir) {
						auto dir = FindFolder(i.m_File->m_Path);
						g_PathEditIgnore = true;
						ChangeDir(dir);
						PushNavHistory(dir);
					}
					break;
				}
				default:
					break;
				}
			}
			else {
				return FALSE;
			}
		}
		break;
	case WM_MEASUREITEM:
		{
			LPMEASUREITEMSTRUCT lpmis = (LPMEASUREITEMSTRUCT)lParam;
			if (lpmis->CtlID == IDC_FileList) {
				lpmis->itemHeight = 32 + 4 + 1;
			}
			else {
				return FALSE;
			}
		}
		break;
	case WM_DRAWITEM:
		{
			LPDRAWITEMSTRUCT lpdis = (LPDRAWITEMSTRUCT)lParam;
			HDC hdc = lpdis->hDC;
			RECT rcItem = lpdis->rcItem;
			if (lpdis->CtlID == IDC_FileList) {
				auto& li = g_ListItems[lpdis->itemID];

				if (lpdis->itemState & ODS_SELECTED) {
					FillRect(hdc, &rcItem, GetSysColorBrush(COLOR_HIGHLIGHT));
				}
				else {
					if (strcmp(*li.m_File->m_Desc, INHDESC) == 0) {
						FillRect(hdc, &rcItem, g_InheritedBgBrush);
					}
					else if (strcmp(*li.m_File->m_Desc, NODESC) == 0) {
						FillRect(hdc, &rcItem, GetSysColorBrush(COLOR_WINDOW));
					}
					else {
						FillRect(hdc, &rcItem, g_DescribedBgBrush);
					}
				}
				auto txtrect = rcItem;
				txtrect.left += 2 + 32 + 2;
				txtrect.top += 2 + 0;
				
				//DrawIconEx(hdc, rcItem.left + 2, rcItem.top + 4, g_IconTest, 32, 32, 0, NULL, DI_NORMAL | DI_COMPAT);
				//auto baba = std::to_wstring(lpdis->itemID);
				DrawText(hdc, li.m_DisplayName.c_str(), li.m_DisplayName.length(), &txtrect, DT_SINGLELINE | DT_VCENTER);

				{
					auto descRect = txtrect;
					descRect.left = ListView_GetColumnWidth(g_ListView, 0);
					auto desc = Widen(GetFileDesc(g_Dir, li.m_File));
					DrawText(hdc, desc.c_str(), desc.length(), &descRect, DT_SINGLELINE | DT_VCENTER);
				}

				HBITMAP thumb = GetThumbnail(lpdis->itemID);
				HDC hdcMem = CreateCompatibleDC(hdc);
				HGDIOBJ objOld = SelectObject(hdcMem, thumb);

				BITMAP bm = {};
				GetObject(thumb, sizeof bm, &bm);

				POINT bmPos;
				bmPos.x = rcItem.left + 2 + (16 - (bm.bmWidth >> 1));
				bmPos.y = rcItem.top + 2 + (16 - (bm.bmHeight >> 1));
				if (bm.bmBitsPixel == 8) {
					TransparentBlt(hdc, bmPos.x, bmPos.y, bm.bmWidth, bm.bmHeight, hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, RGB(255, 0, 255));
				}
				else {
					BLENDFUNCTION bf{};
					bf.BlendOp = AC_SRC_OVER;
					bf.AlphaFormat = AC_SRC_ALPHA;
					bf.BlendFlags = 0;
					bf.SourceConstantAlpha = 255;
					AlphaBlend(hdc, bmPos.x, bmPos.y, bm.bmWidth, bm.bmHeight, hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, bf);
					//BitBlt(hdc, rcItem.left + 2, rcItem.top + 4, bm.bmWidth, bm.bmHeight, hdcMem, 0, 0, SRCCOPY);
				}
				SelectObject(hdcMem, objOld);
				DeleteObject(hdcMem);
				rcItem.top += 32 + 4;
				FillRect(hdc, &rcItem, GetSysColorBrush(COLOR_WINDOWFRAME));
			}
			else {
				return FALSE;
			}
		}
		break;
	case WM_CTLCOLOREDIT:
	case WM_CTLCOLORSTATIC: { //path and desc edit background colours
		HDC hdc = (HDC)wParam;
		HWND hwnd = (HWND)lParam;
		if (hwnd == g_PathEdit) {
			if (g_PathEditError) {
				SetBkColor(hdc, CLR_ERRORBG);
				SetDCBrushColor(hdc, CLR_ERRORBG);
				return (LRESULT)GetStockObject(DC_BRUSH);
			}
		}
		else if (hwnd == g_DescEdit) {
			if (g_DescEditColour == -1) return FALSE; //greyed out; idx is -1
			SetBkColor(hdc, g_DescEditColour);
			SetDCBrushColor(hdc, g_DescEditColour);
			return (LRESULT)GetStockObject(DC_BRUSH);
		}
		return FALSE;
	}
	case WM_INITDIALOG:
	{
		g_PrevDirBtn =	GetDlgItem(hWnd, IDC_PrevDir);
		g_FwrdDirBtn =	GetDlgItem(hWnd, IDC_FwdDir);
		g_UpDirBtn =	GetDlgItem(hWnd, IDC_UpDir);
		g_RandDirBtn =	GetDlgItem(hWnd, IDC_RANDDIR);
		g_PathEdit =	GetDlgItem(hWnd, IDC_PathBar);
		g_DescEdit =	GetDlgItem(hWnd, IDC_DescEdit);
		g_InheritSetBtn = GetDlgItem(hWnd, IDC_SetInherit);
		g_ClearDescBtn = GetDlgItem(hWnd, IDC_Clear);

		g_ListView = GetDlgItem(hWnd, IDC_FileList);
		assert(g_ListView);

		ListView_SetUnicodeFormat(g_ListView, TRUE);
		LV_COLUMN c{};
		c.cx = 300;
		c.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT ;
		c.fmt = LVCFMT_LEFT;
		c.pszText = L"Filename";
		ListView_InsertColumn(g_ListView, 0, &c);
		c.pszText = L"Description";
		ListView_InsertColumn(g_ListView, 1, &c);
		ListView_DeleteAllItems(g_ListView);
		//ListView_SetItemCount(g_ListView, 0);
		ListView_SetExtendedListViewStyle(g_ListView, LVS_EX_FULLROWSELECT | 0);
		//SetWindowLongPtr(g_PathEdit, GWLP_USERDATA, GetWindowLongPtr(g_PathEdit, GWLP_WNDPROC));
		//SetWindowLongPtr(g_PathEdit, GWLP_WNDPROC, (LONG)&PathProc);


		auto sty = GetWindowLong(g_ListView, GWL_STYLE);
		SetWindowLong(g_ListView, GWL_STYLE, sty | LVS_REPORT | LVS_OWNERDATA | LVS_OWNERDRAWFIXED);
		auto root = FindFolder(".");
		ChangeDir(root);
		g_History.push_back(root);
		g_HistoryPos = g_History.begin();
		UpdateNavButtons();
	}
		break;
	default:
		return FALSE;
		break;
	}
	//don't do this, apparently, don't know why, works fine though
	//return DefWindowProc(hWnd, uMessage, wParam, lParam);
	return TRUE;
}