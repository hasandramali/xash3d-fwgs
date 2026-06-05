#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cassert>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#include <FileSystem.h>
#include "interface.h"

class CAndroidFileSystem : public IFileSystem
{
public:
	void Mount() {}
	void Unmount() {}
	void RemoveAllSearchPaths() {}
	void AddSearchPath(const char *, const char *) {}
	bool RemoveSearchPath(const char *) { return false; }
	void RemoveFile(const char *, const char *) {}
	void CreateDirHierarchy(const char *path, const char *pathID);
	bool FileExists(const char *pFileName);
	bool IsDirectory(const char *pFileName);
	FileHandle_t Open(const char *pFileName, const char *pOptions, const char *pathID = 0L);
	void Close(FileHandle_t file);
	void Seek(FileHandle_t file, int pos, FileSystemSeek_t seekType);
	unsigned int Tell(FileHandle_t file);
	unsigned int Size(FileHandle_t file);
	unsigned int Size(const char *pFileName);
	long GetFileTime(const char *pFileName);
	void FileTimeToString(char *pStrip, int maxCharsIncludingTerminator, long fileTime);
	bool IsOk(FileHandle_t file);
	void Flush(FileHandle_t file);
	bool EndOfFile(FileHandle_t file);
	int Read(void *pOutput, int size, FileHandle_t file);
	int Write(void const *pInput, int size, FileHandle_t file);
	char *ReadLine(char *pOutput, int maxChars, FileHandle_t file);
	int FPrintf(FileHandle_t file, const char *pFormat, ...);
	void *GetReadBuffer(FileHandle_t file, int *outBufferSize, bool failIfNotInCache);
	void ReleaseReadBuffer(FileHandle_t file, void *readBuffer);
	const char *FindFirst(const char *pWildCard, FileFindHandle_t *pHandle, const char *pathID = 0L);
	const char *FindNext(FileFindHandle_t handle);
	bool FindIsDirectory(FileFindHandle_t handle);
	void FindClose(FileFindHandle_t handle);
	void GetLocalCopy(const char *pFileName) {}
	const char *GetLocalPath(const char *pFileName, char *pLocalPath, int localPathBufferSize);
	char *ParseFile(char *pFileBytes, char *pToken, bool *pWasQuoted);
	bool FullPathToRelativePath(const char *pFullpath, char *pRelative);
	bool GetCurrentDirectory(char *pDirectory, int maxlen);
	void PrintOpenedFiles() {}
	void SetWarningFunc(FileSystemWarningFunc) {}
	void SetWarningLevel(FileWarningLevel_t) {}
	void LogLevelLoadStarted(const char *) {}
	void LogLevelLoadFinished(const char *) {}
	int HintResourceNeed(const char *, int) { return 0; }
	int PauseResourcePreloading() { return 0; }
	int ResumeResourcePreloading() { return 0; }
	int SetVBuf(FileHandle_t, char *, int, long) { return 0; }
	void GetInterfaceVersion(char *p, int maxlen) { if (p && maxlen > 0) { strncpy(p, "VFileSystem009", maxlen - 1); p[maxlen - 1] = '\0'; } }
	bool IsFileImmediatelyAvailable(const char *) { return true; }
	WaitForResourcesHandle_t WaitForResources(const char *) { return (WaitForResourcesHandle_t)0; }
	bool GetWaitForResourcesProgress(WaitForResourcesHandle_t, float *, bool *) { return false; }
	void CancelWaitForResources(WaitForResourcesHandle_t) {}
	bool IsAppReadyForOfflinePlay(int) { return true; }
	bool AddPackFile(const char *, const char *) { return false; }
	FileHandle_t OpenFromCacheForRead(const char *pFileName, const char *pOptions, const char *pathID = 0L) { return Open(pFileName, pOptions, pathID); }
	void AddSearchPathNoWrite(const char *, const char *) {}
};

static CAndroidFileSystem g_FileSystem;

void CAndroidFileSystem::CreateDirHierarchy(const char *path, const char *pathID)
{
	char tmp[256];
	strncpy(tmp, path, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	for (char *p = tmp; *p; p++)
	{
		if (*p == '/')
		{
			*p = '\0';
			mkdir(tmp, 0755);
			*p = '/';
		}
	}
	mkdir(tmp, 0755);
}

bool CAndroidFileSystem::FileExists(const char *pFileName)
{
	struct stat st;
	return stat(pFileName, &st) == 0;
}

bool CAndroidFileSystem::IsDirectory(const char *pFileName)
{
	struct stat st;
	if (stat(pFileName, &st) == 0)
		return S_ISDIR(st.st_mode);
	return false;
}

FileHandle_t CAndroidFileSystem::Open(const char *pFileName, const char *pOptions, const char *pathID)
{
	FILE *f = fopen(pFileName, pOptions);
	return (FileHandle_t)f;
}

void CAndroidFileSystem::Close(FileHandle_t file)
{
	if (file) fclose((FILE *)file);
}

void CAndroidFileSystem::Seek(FileHandle_t file, int pos, FileSystemSeek_t seekType)
{
	if (!file) return;
	int whence = SEEK_SET;
	if (seekType == FILESYSTEM_SEEK_CURRENT) whence = SEEK_CUR;
	else if (seekType == FILESYSTEM_SEEK_TAIL) whence = SEEK_END;
	fseek((FILE *)file, pos, whence);
}

unsigned int CAndroidFileSystem::Tell(FileHandle_t file)
{
	if (!file) return 0;
	return (unsigned int)ftell((FILE *)file);
}

unsigned int CAndroidFileSystem::Size(FileHandle_t file)
{
	if (!file) return 0;
	unsigned int pos = ftell((FILE *)file);
	fseek((FILE *)file, 0, SEEK_END);
	unsigned int size = ftell((FILE *)file);
	fseek((FILE *)file, pos, SEEK_SET);
	return size;
}

unsigned int CAndroidFileSystem::Size(const char *pFileName)
{
	struct stat st;
	if (stat(pFileName, &st) == 0)
		return (unsigned int)st.st_size;
	return 0;
}

long CAndroidFileSystem::GetFileTime(const char *pFileName)
{
	struct stat st;
	if (stat(pFileName, &st) == 0)
		return (long)st.st_mtime;
	return 0;
}

void CAndroidFileSystem::FileTimeToString(char *pStrip, int maxCharsIncludingTerminator, long fileTime)
{
	if (pStrip && maxCharsIncludingTerminator > 0)
	{
		time_t t = (time_t)fileTime;
		struct tm *lt = localtime(&t);
		if (lt)
			strftime(pStrip, maxCharsIncludingTerminator, "%c", lt);
		else
			pStrip[0] = '\0';
	}
}

bool CAndroidFileSystem::IsOk(FileHandle_t file)
{
	return file != NULL;
}

void CAndroidFileSystem::Flush(FileHandle_t file)
{
	if (file) fflush((FILE *)file);
}

bool CAndroidFileSystem::EndOfFile(FileHandle_t file)
{
	if (!file) return true;
	return feof((FILE *)file) != 0;
}

int CAndroidFileSystem::Read(void *pOutput, int size, FileHandle_t file)
{
	if (!file) return 0;
	return (int)fread(pOutput, 1, size, (FILE *)file);
}

int CAndroidFileSystem::Write(void const *pInput, int size, FileHandle_t file)
{
	if (!file) return 0;
	return (int)fwrite(pInput, 1, size, (FILE *)file);
}

char *CAndroidFileSystem::ReadLine(char *pOutput, int maxChars, FileHandle_t file)
{
	if (!file || !pOutput || maxChars <= 0)
	{
		static char empty[] = "";
		return empty;
	}
	return fgets(pOutput, maxChars, (FILE *)file);
}

int CAndroidFileSystem::FPrintf(FileHandle_t file, const char *pFormat, ...)
{
	if (!file) return -1;
	va_list args;
	va_start(args, pFormat);
	int ret = vfprintf((FILE *)file, pFormat, args);
	va_end(args);
	return ret;
}

void *CAndroidFileSystem::GetReadBuffer(FileHandle_t file, int *outBufferSize, bool failIfNotInCache)
{
	if (outBufferSize) *outBufferSize = 0;
	return NULL;
}

void CAndroidFileSystem::ReleaseReadBuffer(FileHandle_t file, void *readBuffer) {}

static struct FindState
{
	DIR *dir;
	char result[256];
} g_FindState;

const char *CAndroidFileSystem::FindFirst(const char *pWildCard, FileFindHandle_t *pHandle, const char *pathID)
{
	char dir[256];
	strncpy(dir, pWildCard, sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = '\0';
	else
		strcpy(dir, ".");

	g_FindState.dir = opendir(dir);
	if (!g_FindState.dir)
	{
		*pHandle = (FileFindHandle_t)0;
		return NULL;
	}

	*pHandle = (FileFindHandle_t)&g_FindState;
	return FindNext(*pHandle);
}

const char *CAndroidFileSystem::FindNext(FileFindHandle_t handle)
{
	if (!handle) return NULL;
	FindState *state = (FindState *)handle;
	struct dirent *entry;
	while ((entry = readdir(state->dir)) != NULL)
	{
		if (entry->d_name[0] == '.') continue;
		strncpy(state->result, entry->d_name, sizeof(state->result) - 1);
		state->result[sizeof(state->result) - 1] = '\0';
		return state->result;
	}
	return NULL;
}

bool CAndroidFileSystem::FindIsDirectory(FileFindHandle_t handle)
{
	return false;
}

void CAndroidFileSystem::FindClose(FileFindHandle_t handle)
{
	if (handle)
	{
		FindState *state = (FindState *)handle;
		if (state->dir)
		{
			closedir(state->dir);
			state->dir = NULL;
		}
	}
}

const char *CAndroidFileSystem::GetLocalPath(const char *pFileName, char *pLocalPath, int localPathBufferSize)
{
	if (pLocalPath && localPathBufferSize > 0)
	{
		strncpy(pLocalPath, pFileName, localPathBufferSize - 1);
		pLocalPath[localPathBufferSize - 1] = '\0';
	}
	return pLocalPath;
}

char *CAndroidFileSystem::ParseFile(char *pFileBytes, char *pToken, bool *pWasQuoted)
{
	if (!pFileBytes || !pToken) return NULL;
	pToken[0] = '\0';
	if (pWasQuoted) *pWasQuoted = false;

	char *p = pFileBytes;
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	if (*p == '\0') return NULL;

	if (*p == '"')
	{
		if (pWasQuoted) *pWasQuoted = true;
		p++;
		int i = 0;
		while (*p && *p != '"' && i < 1023)
			pToken[i++] = *p++;
		pToken[i] = '\0';
		if (*p == '"') p++;
	}
	else
	{
		int i = 0;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && i < 1023)
			pToken[i++] = *p++;
		pToken[i] = '\0';
	}
	return p;
}

bool CAndroidFileSystem::FullPathToRelativePath(const char *pFullpath, char *pRelative)
{
	if (pRelative && pFullpath)
	{
		strcpy(pRelative, pFullpath);
		return true;
	}
	return false;
}

bool CAndroidFileSystem::GetCurrentDirectory(char *pDirectory, int maxlen)
{
	if (!pDirectory || maxlen <= 0) return false;
	return getcwd(pDirectory, maxlen) != NULL;
}

EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CAndroidFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION, g_FileSystem);
