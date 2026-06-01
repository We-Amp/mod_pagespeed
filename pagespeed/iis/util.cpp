#define _WINSOCKAPI_
#include <windows.h>
#include <string>
#include "pagespeed/iis/util.h"


std::wstring s2ws(const std::string& s)
{
	int slength = (int)s.length() + 1;
	int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, 0, 0);
	if (len <= 0) return std::wstring();
	std::wstring r(len - 1, L'\0');  // len-1: exclude null terminator
	MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, &r[0], len);
	return r;
}

std::string ws2s(const std::wstring& s)
{
	int slength = (int)s.length() + 1;
	int len = WideCharToMultiByte(CP_ACP, 0, s.c_str(), slength, 0, 0, 0, 0);
	if (len <= 0) return std::string();
	std::string r(len - 1, '\0');  // len-1: exclude null terminator
	WideCharToMultiByte(CP_ACP, 0, s.c_str(), slength, &r[0], len, 0, 0);
	return r;
}
