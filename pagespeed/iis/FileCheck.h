// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#include <string>

struct FileCheck 
{
	FILETIME ft;
	unsigned long long filehash;
	void CreateFileID(std::string file);
	
	bool FileIDChanged(std::string file);	
};