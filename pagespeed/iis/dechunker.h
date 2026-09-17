// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#ifndef DECHUNKER_H
#define DECHUNKER_H
#include <Windows.h>
#include <cstdlib>
#include <climits>


class Dechunker
{
	bool chunked;
	int currentmode;
	int sizepos;
	char *currentData;
	size_t currentDatalength;
	size_t currentblocklength;
	char sizeBuffer[20];
	bool validStream;
	bool eof;
	char *data;
	size_t datalength;
	void setInvalidStream()
	{
		validStream=false;
		data=NULL;
		datalength=0;
		eof=true;
	}
	bool expect(char c,char expected,int newmode)
	{
		bool isValid=c==expected;
		if (isValid) 
			currentmode=newmode;
		else
			setInvalidStream();
		return isValid;
	}
public:
	Dechunker()
	{
		Initialize(false);
	}
	Dechunker(bool chunked)
	{
		
		Initialize(chunked);
	}
	bool Initialize(bool chunked)
	{
		this->chunked=chunked;
		eof=false;
		datalength=0;
		data=NULL;
		validStream=true;
		sizepos=0;
		currentmode=0;
		currentblocklength=0;
		currentData=NULL;
		currentDatalength=0;	
		return true;
	}

	bool FeedChunk(char *data,size_t datalength)
	{
		if (!validStream || eof)
		{
			return false;
		}
		this->data=data;
		this->datalength=datalength;
		this->currentData=NULL;
		this->currentDatalength=0;
		return true;
	}
	bool ParseNext()
	{
		if (!chunked)
		{
			if (data) // if we have data return the block as seen
			{
				this->currentData=data;
				this->currentDatalength=datalength;
				this->data=NULL; // for next call we reset data
				this->datalength=0;
				return true;
			}
			else // no data (second) or invalid call
			{
				this->currentData=NULL;
				this->currentDatalength=0;
				return false;
			}
		}
		if (!validStream || eof) return false;
		this->currentData=NULL;
		this->currentDatalength=0;
	
		char *endptr=data+datalength;
			if (data==endptr) // end of content
				return false;		
		while(data!=endptr)
		{
			char c=*data;
			switch(currentmode)
			{
			case 0: // parse chunklength;
				if (sizepos==19)
				{
					setInvalidStream();
					return false;
				}
				c=tolower(c);
				if ((c>='a' && c<='f') || (c>='0' && c<='9')) 
				{
					sizeBuffer[sizepos++]=c;			
				}
				else
				{
					sizeBuffer[sizepos]=0;
					{
						char *endptr = nullptr;
						unsigned long parsed = strtoul(sizeBuffer, &endptr, 16);
						if (endptr == sizeBuffer || parsed == ULONG_MAX) {
							setInvalidStream();
							return false;
						}
						currentblocklength = static_cast<size_t>(parsed);
					}
					sizepos=0;
					if (currentblocklength==0)
					{
						data=NULL;
						datalength=0;
						currentData=NULL;
						currentDatalength=0;
						eof=true; 
						currentmode=0;
						return false;
					}
					currentmode=1;
					continue; // pushback
				}				
				break;
			case 1: if (!expect(c,'\r',2)) return false; break; // expect \r
			case 2: if (!expect(c,'\n',3)) return false; break;// parse '\n''				
			case 3: // parse data (optimized, not char by char)
				{
					int dataleft=endptr-data;
					if (dataleft==0) // no data in block
					{
						data=NULL;
						datalength=0;
						return false;
					}
					if (dataleft<=currentblocklength)
					{
						currentDatalength=dataleft;
						currentblocklength-=dataleft;					
					}
					else
					{
						currentDatalength=currentblocklength;
						currentblocklength=0;
						currentmode=4;
					}
					currentData=data;	
					data+=currentDatalength;
					datalength=endptr-data;
					return true;
				}
			case 4: if (!expect(c,'\r',5)) return false; break; // parse '\r'
			case 5: if (!expect(c,'\n',0)) return false; break; // parse '\n'

			}
			data++;
		}
		currentData=NULL;
		currentDatalength=0;
		return false;		
	}
	bool HasData()
	{
		return currentData!=NULL;			
	}
	bool GetDataBlock(char **datablock,size_t *datablocklength)
	{
		*datablock=currentData;
		*datablocklength=currentDatalength;
		return currentData!=NULL;
	}
};
#endif