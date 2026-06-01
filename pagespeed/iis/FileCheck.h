#include <string>

struct FileCheck 
{
	FILETIME ft;
	unsigned long long filehash;
	void CreateFileID(std::string file);
	
	bool FileIDChanged(std::string file);	
};