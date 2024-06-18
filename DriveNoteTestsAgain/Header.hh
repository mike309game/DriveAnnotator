#define _CRT_SECURE_NO_WARNINGS 1
#include <unordered_map>
#include <string>
#include <Windows.h>
#include <iostream>
#include <fstream>
#include <stdio.h>
#include <assert.h>
#include <stdint.h>

#define NODESC "<%D"
#define INHDESC "<%H" //inherit desc

extern std::vector<char*> g_Pools;
extern HANDLE g_Heap;

#define ALLOC(size) HeapAlloc(g_Heap, HEAP_NO_SERIALIZE, size)
#define ALLOCZERO(size) HeapAlloc(g_Heap, HEAP_NO_SERIALIZE | HEAP_ZERO_MEMORY, size)
#define REALLOC(handle, size) HeapReAlloc(g_Heap, HEAP_NO_SERIALIZE, handle, size)
#define FREE(handle) HeapFree(g_Heap, HEAP_NO_SERIALIZE, handle)

typedef uint64_t Hash;

//because std::string adds too much overhead
//UPDATE: this barely helps
class Desc {
private:
	char* m_Str;
	void SetStr(char* str) {
		FREE(m_Str); //heapfree knows when it's NULL
		auto len = strlen(str) + 1;
		m_Str = (char*)ALLOC(len);
		CopyMemory(m_Str, str, len);
	}
public:
	inline char* GetStr() {
		return m_Str;
	}
	Desc() { m_Str = (char*)ALLOCZERO(4); }
	Desc(char* str) : Desc() { SetStr(str); }
	Desc(std::string& str) : Desc((char*)str.c_str()) {  }
	Desc& operator=(const char* other) {
		SetStr((char*)other);
		return *this;
	}
	/*Desc& operator=(std::string& other) { //broken?
		SetStr((char*)other.c_str());
		return *this;
	}*/
	operator char*() {
		return m_Str;
	}
	//deleted along with heap not needed; slows down
	/*~Desc() {
		FREE(m_Str); //this makes things destruct A LOT slower??????????
	}*/
};

class File {
public:
	//i shit you not, using stl strings here skyrockets the ram usage and adds 400 more mbs than there should be.
	Desc* m_Desc; //store a pointer because we malloc memory holding this
	char* m_Path;
	
	//not anymore
	//~File() { delete m_Desc; }
};

class Dir : public File {
public:
	//arrays that get freed on heap destruction
	Hash* m_Folders; //don't store pointers to something in map because it can be reallocated (? maybe)
	File* m_Files;
	
	//not anymore
	/*~Dir() {
		if (!m_Files) return;
		for (File* ptr = m_Files; ptr->m_Path != nullptr; ptr++) {
			delete ptr->m_Desc; //these are alloced with new
		}
	}*/
};

extern std::unordered_map<Hash, Dir> g_Map;

//for padding this struct has 4 garbage bytes.
//i don't care about packing the struct because i don't wanna
//risk losing any sorta performance
typedef struct {
	Hash hash;
	int pathLen;
	int descLen;
	int foldersLen;
	int filesLen;
	int fileCount;
} SaveFolderInfo;

typedef struct {
	File* m_File;
	std::wstring m_DisplayName;
	bool m_IsDir;
} ListItem;

typedef struct {
	std::wstring m_Path;
	Dir* m_Dest; //intended dir
	int idx;
} QueueItem;

std::wstring Widen(const char* str); std::wstring Widen(std::string& str);
std::string Unwiden(const wchar_t* str); std::string Unwiden(std::wstring& str);

uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);
inline uint64_t crc64(const char *s) { return crc64(0, (unsigned char*)s, strlen(s)); }
inline uint64_t crc64(std::string s) { return crc64(0, (unsigned char*)s.c_str(), strlen(s.c_str())); }

int FoldersLen(Hash* folders);
int FilesLen(File* files);
Dir* FindFolder(Hash hash);
Dir* FindFolder(const char* path);
Dir* FindParentDir(Dir* dir);
Dir* GetDescSource(Dir* dir);
std::string GetFileDesc(Dir* dir, File* file);
void LoadFromCsv(std::istream& is);
void SaveMapStl(FILE* fp);
void LoadFromBin(FILE* fp);
void SaveMapBin(FILE* fp);

#define POOLMAX (1048576 * 10)
#define THUMBTHREADS 4
#define DRIVELETTER 'F'
#define BMLOADING ((HBITMAP)1)
#define BMEND ((HBITMAP)-11)

char* Mem_Alloc(size_t len);
void Mem_FreeAll();