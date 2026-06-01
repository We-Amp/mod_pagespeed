
#define _WINSOCKAPI_
#include <Windows.h>
#include "pagespeed/iis/FileCheck.h"	
#include "pagespeed/kernel/base/md5_hasher.h"
#include <string>

using namespace net_instaweb;

void FileCheck::CreateFileID(std::string file)
{
	memset(&ft,0,sizeof(ft));
	filehash=0;
	// Check if we can read this another way without opening the file
	HANDLE hFile=CreateFileA(file.c_str(),GENERIC_READ ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
	if (hFile==INVALID_HANDLE_VALUE)
	{
		return;
	}
	GetFileTime(hFile,NULL,NULL,&ft);
	DWORD sizeHigh;
	DWORD size=GetFileSize(hFile,&sizeHigh);
	DWORD read;
	char *temp=new char[size];
	if (ReadFile(hFile,temp,size,&read,NULL))
	{
		MD5Hasher hasher;
		filehash=hasher.HashToUint64(StringPiece(temp,read));
	}
	delete [] temp;
	CloseHandle(hFile);
}
bool FileCheck::FileIDChanged(std::string file)
{
	FILETIME myTime;
		
	HANDLE hFile=CreateFileA(file.c_str(),GENERIC_READ ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
	if (hFile==INVALID_HANDLE_VALUE)
	{
		memset(&myTime,0,sizeof(FILETIME));
	}
	else
	{
		GetFileTime(hFile,NULL,NULL,&myTime);
	}
		
	bool changed=(myTime.dwHighDateTime!=ft.dwHighDateTime || myTime.dwLowDateTime!=ft.dwLowDateTime);		
		
	if (hFile!=INVALID_HANDLE_VALUE)
	{
		if (changed)
		{
			DWORD sizeHigh;
			DWORD size=GetFileSize(hFile,&sizeHigh);
			DWORD read;
			char *temp=new char[size];
			if (ReadFile(hFile,temp,size,&read,NULL))
			{
				MD5Hasher hasher;
				uint64 myfilehash=hasher.HashToUint64(StringPiece(temp,read));
				changed=(myfilehash!=filehash);
			}	
			delete [] temp;
		}
		CloseHandle(hFile);
	}
	return changed;
}
