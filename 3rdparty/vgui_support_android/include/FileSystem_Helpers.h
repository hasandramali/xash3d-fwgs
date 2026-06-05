#ifndef FILESYSTEM_HELPERS_H
#define FILESYSTEM_HELPERS_H
#ifdef _WIN32
#pragma once
#endif

class IFileSystem;

struct characterset_t;
const char* ParseFile( const char* pFileBytes, char* pToken, bool* pWasQuoted, characterset_t *pCharSet = NULL );
char* ParseFile( char* pFileBytes, char* pToken, bool* pWasQuoted );
bool FS_GetFileTypeForFullPath( char const *pFullPath, wchar_t *buf, size_t bufSizeInBytes );
bool FS_IsFileWritable( IFileSystem* pFileSystem, char const *pFileName, const char *pPathID = 0 );

#endif
