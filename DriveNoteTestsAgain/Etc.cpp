#include "Header.hh"

/* Memory */

std::vector<char*> g_Pools;
HANDLE g_Heap;

static size_t _poolPtrCurrent = POOLMAX;

//marginal performance gain (maybe)

char* Mem_Alloc(size_t len) {
	//len += 2; //always space for wide null term
	if ((len + _poolPtrCurrent) >= POOLMAX) {
		_poolPtrCurrent = 0;
		g_Pools.push_back((char*)ALLOCZERO(POOLMAX));
	}
	_poolPtrCurrent += len;
	return g_Pools.back() + (_poolPtrCurrent - len);
}

void Mem_FreeAll() {
	//job done by destroying g_Heap
	/*for (auto pool : g_Pools)
		FREE((HANDLE)pool);*/
	g_Pools.clear();
	_poolPtrCurrent = POOLMAX;
}

/* utf8 <-> wide */

//static char _strAccum[0xFFFF];

std::wstring Widen(const char* str) {
	auto len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
	wchar_t* buff = (wchar_t*)ALLOC(len * sizeof(wchar_t));
	MultiByteToWideChar(CP_UTF8, 0, str, -1, buff, len);
	auto widened = std::wstring(buff);
	FREE(buff);
	return widened;
}

std::string Unwiden(const wchar_t* str) {
	auto len = WideCharToMultiByte(CP_UTF8, 0, str, -1, NULL, 0, NULL, NULL);
	char* buff = (char*)ALLOC(len);
	WideCharToMultiByte(CP_UTF8, 0, str, -1, buff, len, NULL, NULL);
	auto unwidened = std::string(buff);
	FREE(buff);
	return unwidened;
}

std::wstring Widen(std::string& str) { return Widen(str.c_str()); }
std::string Unwiden(std::wstring& str) { return Unwiden(str.c_str()); }

static char* PoolStr(std::string& str) {
	auto ptr = Mem_Alloc(str.size() + 1);
	CopyMemory(ptr, str.c_str(), str.size());
	return ptr;
	//ptr[str.size()] = 0;
}

//static std::hash<std::string> _folderHasher;

/* Actual map operations */

int FoldersLen(Hash* folders) {
	int count = 0;
	for (Hash* ptr = folders; *ptr != -1; ptr++)
		count++;
	return count;
}
int FilesLen(File* files) {
	int count = 0;
	for (File* ptr = files; ptr->m_Path != nullptr; ptr++)
		count++;
	return count;
}

Dir* FindFolder(Hash hash) {
	auto it = g_Map.find(hash);
	if(it == g_Map.end()) return nullptr;
	return &it->second;
}

Dir* FindFolder(const char* path) {
	Hash hash = crc64(path);
	return FindFolder(hash);
}

Dir* FindParentDir(Dir* dir) {
	auto pathStr = std::string(dir->m_Path);
	auto last = pathStr.find_last_of('\\');
	if (last == -1) return nullptr; //means this is the root dir
	pathStr = pathStr.substr(0, last);
	return FindFolder(pathStr.c_str());
}

//returns null if root dir was hit
Dir* GetDescSource(Dir* dir) {
	while (strcmp(*dir->m_Desc, INHDESC) == 0) { //while dir's desc is inherited
		dir = FindParentDir(dir);
		if (dir == nullptr) return nullptr; //root dir has been hit whoop
	}
	return dir;
}

std::string GetFileDesc(Dir* dir, File* file) {
	Desc* d = file->m_Desc;
	if (strcmp(*d, INHDESC) == 0) {
		auto src = GetDescSource(dir);
		if (src != nullptr)
			d = src->m_Desc;
		else //has hit root dir when searching for the desc
			return "";
	}
	
	if (strcmp(*d, NODESC) == 0) {
		return "";
	}
	return std::string(*d);
}

void LoadFromBin(FILE* fp) {
#define R(dest, elementsize, count, fp) CopyMemory(dest, __ptr, elementsize * count); __ptr += elementsize * count;
	char* data;
	{ //read out file into memory for less io during load UPDATE this did not help
		fseek(fp, 0, SEEK_END);
		auto size = ftell(fp);
		rewind(fp);
		data = (char*)ALLOC(size);
		fread(data, 1, size, fp);
	}
	char* __ptr = data;

	char* descBuff = (char*)ALLOC(1024);
	int descBuffLen = 1024;

	size_t folderCount;
	R(&folderCount, sizeof size_t, 1, fp);
	g_Map.reserve(folderCount);
	for (int i = 0; i < folderCount; i++) {
		SaveFolderInfo fi;
		auto s = sizeof fi;
		R(&fi, sizeof fi, 1, fp);
		auto& folder = g_Map[fi.hash];
		folder.m_Path = Mem_Alloc(fi.pathLen+1);
		R(folder.m_Path, fi.pathLen, 1, fp);
		if (fi.descLen == 0)
			assert(0); //folder.m_Desc = new Desc();
		else {
			if (descBuffLen < fi.descLen + 1) {
				FREE(descBuff);
				descBuffLen = fi.descLen + 1;
				char* descBuff = (char*)ALLOC(descBuffLen);
			}
			descBuff[fi.descLen] = '\0';
			R(descBuff, fi.descLen, 1, fp);
			folder.m_Desc = (Desc*)Mem_Alloc(sizeof Desc);// new Desc(buff);
			*folder.m_Desc = descBuff;
		}
		folder.m_Folders = (Hash*)Mem_Alloc(fi.foldersLen);
		R(folder.m_Folders, fi.foldersLen, 1, fp);
		if (fi.filesLen == 0)
			folder.m_Files = nullptr;
		else {
			auto ptr = Mem_Alloc(fi.filesLen);
			R(ptr, fi.filesLen, 1, fp);
			folder.m_Files = (File*)Mem_Alloc(sizeof(File) * (fi.fileCount+1)); //last item is already terminator cuz zerod out
			for (int i = 0; i < fi.fileCount; i++) {
				//assert(fi.hash != 657248216);
				folder.m_Files[i].m_Path = ptr;				ptr += strlen(ptr)+1;
				folder.m_Files[i].m_Desc = (Desc*)Mem_Alloc(sizeof Desc);// new Desc(ptr);
				*folder.m_Files[i].m_Desc = ptr;
				ptr += strlen(ptr) + 1;
			}
			
		}
	}
	FREE((HANDLE)data);
	FREE(descBuff);
#undef R
}

void SaveMapBin(FILE* fp) {
	size_t folderCount = g_Map.size();
	fwrite(&folderCount, sizeof size_t, 1, fp);
	for (auto& it : g_Map) {
		auto& folder = it.second;
		SaveFolderInfo fi;
		fi.hash = it.first;
		fi.pathLen = strlen(folder.m_Path);
		fi.descLen = strlen(*folder.m_Desc);
		fi.foldersLen = sizeof(Hash) * (FoldersLen(folder.m_Folders)+1);
		fi.filesLen = 0;
		fi.fileCount = 0;
		if (folder.m_Files != nullptr) {
			for (File* ptr = folder.m_Files; ptr->m_Path != nullptr; ptr++) {
				fi.fileCount++;
				fi.filesLen += strlen(ptr->m_Path) + 1;
				fi.filesLen += strlen(*ptr->m_Desc) + 1;
			}
		}
		fwrite(&fi, sizeof fi, 1, fp);

		fputs(folder.m_Path, fp);
		fputs(*folder.m_Desc, fp);
		
		fwrite(folder.m_Folders, fi.foldersLen, 1, fp);
		
		for (int i = 0; i < fi.fileCount; i++) {
			fputs(folder.m_Files[i].m_Path, fp); fputc(0, fp);
			fputs(*folder.m_Files[i].m_Desc, fp); fputc(0, fp);
		}
	}
}

//DO NOT UTILISE
/*void LoadFromTxt(std::ifstream& is) {
	std::string accum;
	std::getline(is, accum);
	auto folders = std::stoi(accum);
	g_Map.reserve(folders);
	for (auto i = 0; i < folders; i++) {
		std::getline(is, accum); //accum is path
		auto hash = _folderHasher(accum); //less io = good
		auto& dir = g_Map[hash]; //will auto create entry
		dir.m_Path = PoolStr(accum);
		std::getline(is, accum); //get desc
		dir.m_Desc = new std::string(accum);
		std::getline(is, accum); //get child folder count
		auto childCount = std::stoi(accum);
		if (childCount == 0)
			dir.m_Folders = nullptr;
		else {
			dir.m_Folders = (size_t*)Mem_Alloc(sizeof(size_t) * (childCount+1));
			dir.m_Folders[childCount] = -1; //array terminator
		}
		for (int i = 0; i < childCount; i++) {
			std::getline(is, accum); //get hash
			dir.m_Folders[i] = std::stoull(accum);
		}
		std::getline(is, accum); //get child file count
		childCount = std::stoi(accum);
		if (childCount == 0)
			dir.m_Files = nullptr;
		else {
			dir.m_Files = (File*)Mem_Alloc(sizeof(File) * (childCount + 1));
			dir.m_Files[childCount].m_Path = nullptr; //null path ends array
		}
		for (int i = 0; i < childCount; i++) {
			std::getline(is, accum); //get path
			dir.m_Files[i].m_Path = PoolStr(accum);
			std::getline(is, accum); //get desc
			dir.m_Files[i].m_Desc = new std::string(accum);
		}
	}
}*/

/* Stl map operations */

class FileStl {
public:
	std::string m_Desc;
	std::string m_Path;
	FileStl(std::string path, std::string desc = NODESC) : m_Desc(desc), m_Path(path) {};
};

class DirStl : public FileStl {
public:
	std::vector<DirStl*> m_Folders;
	std::vector<FileStl> m_Files;
	DirStl(std::string path, std::string desc = NODESC) : FileStl(path, desc) {};
};

static std::unordered_map<Hash, DirStl> _stlMap;

static DirStl* GetDirStl(std::string path) {
	
	Hash hash = crc64(path);
	auto it = _stlMap.find(hash);
	if (it == _stlMap.end()) {
		it = ((_stlMap.emplace(hash, path).first));
	}
	return &(it->second);
}

/* File load/save */

void SaveMapStl(FILE* fp) {
	size_t folderCount = _stlMap.size();
	fwrite(&folderCount, sizeof size_t, 1, fp);
	for (auto& it : _stlMap) {
		auto& folder = it.second;
		SaveFolderInfo fi;
		fi.hash = it.first;
		fi.pathLen = folder.m_Path.size();
		fi.descLen = folder.m_Desc.size();
		fi.foldersLen = sizeof(Hash) * (folder.m_Folders.size() + 1);

		fi.filesLen = 0;
		fi.fileCount = folder.m_Files.size();
		for (auto& f : folder.m_Files) {
			fi.filesLen += f.m_Path.size() + 1;
			fi.filesLen += f.m_Desc.size() + 1;
		}

		fwrite(&fi, sizeof fi, 1, fp);

		fwrite(folder.m_Path.c_str(), fi.pathLen, 1, fp);
		fwrite(folder.m_Desc.c_str(), fi.descLen, 1, fp);
		for (auto i : folder.m_Folders) {
			Hash hash = crc64(i->m_Path);
			fwrite(&hash, sizeof hash, 1, fp);
		}
		Hash asd = -1;
		fwrite(&asd, sizeof asd, 1, fp); //terminator

		for (auto& i : folder.m_Files) {
			fwrite(i.m_Path.c_str(), i.m_Path.size() + 1, 1, fp);
			fwrite(i.m_Desc.c_str(), i.m_Desc.size() + 1, 1, fp);
		}
	}
}

/*void SaveMapStl(std::ofstream& os) {
	os << _stlMap.size() << std::endl; 
	for (auto& it : _stlMap) {
		auto& dir = it.second;
		os << dir.m_Path << std::endl; //push folder path
		//os << it.first << std::endl; //push hash
		os << dir.m_Desc << std::endl; //push my desc
		os << dir.m_Folders.size() << std::endl; //push folder child count
		for (auto child : dir.m_Folders) {
			//os << child->m_Path << std::endl; //push child folder path (so that at load time we can get map iterator to add to parent's list)
			os << _folderHasher(child->m_Path) << std::endl;
		}
		os << dir.m_Files.size() << std::endl; //push file count
		for (auto& child : dir.m_Files) {
			os << child.m_Path << std::endl; //push child file path
			os << child.m_Desc << std::endl; //push child file desc
		}
	}
	//os << "<STOP";
}*/

void LoadFromCsv(std::istream& is) { //badly optimised
	std::string accum;
	std::getline(is, accum); //skip first line that defines csv format. also skips bom
	for(;;) {
		std::getline(is, accum);
		std::string fname;
		size_t argspos;
		bool quoted = accum[0] == '"';
		if (quoted) {
			argspos = accum.find_first_of('"', 1) + 2;
			fname = accum.substr(1, argspos - 3);
		}
		else {
			argspos = accum.find_first_of(',') + 1;
			fname = accum.substr(0, argspos - 1);
		}
		/*if (fname.find("Samplepack18e") != -1) {
			assert(0);
		}*/
		argspos = accum.find_first_of(',', argspos)+1;
		//bool isDir = accum.substr(argspos, (accum.find_first_of(',', argspos) - argspos)) == "True"; //wow i'm silly
		bool isDir = accum[argspos] == 'T';
		auto last = fname.find_last_of('\\');
		if (isDir) {
			//ssert(!quoted);
			DirStl* me = GetDirStl(fname); //make dir
			if (fname == ".\\.") continue; //don't fuck up with root dir
			auto parent = GetDirStl(fname.substr(0, last));
			//GetDirStl(fname.substr(0, last))->m_Folders.push_back(me); //fake error, compiles, but ide keeps yelling
			parent->m_Folders.push_back(me); //IT'S STILL YELLING YET COMPILING
		}
		else {
			DirStl* dir = GetDirStl(fname.substr(0, last));
			fname = fname.substr(last+1, -1);
			dir->m_Files.emplace_back(fname);
		}
		if (is.eof()) break;
	}
	
}