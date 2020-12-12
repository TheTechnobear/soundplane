
// MadronaLib: a C++ framework for DSP applications.
// Copyright (c) 2013 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

#include "MLDebug.h"

#if DEBUG_TO_COUT
#else

MLTextStream::MLTextStream(const char* name) : 
	mName(name), 
	mActive(true),
	mpListener(0),
	mItemsInLocalStream(0)
{
	
}

MLTextStream::~MLTextStream()
{
	mActive = false;
	mpListener = 0;
	flush();
}

void MLTextStream::sendOutputToListener(MLTextStreamListener* pL)
{
	// transfer any startup items
	if(!mpListener)
	{
		if(pL)
		{
			std::string outStr = mLocalStream.str();
			pL->getStream() << outStr;
			pL->getStream() << "---------------\n";
		}
	}
	mpListener = pL;
}

void MLTextStream::flush()
{
	mLocalStream.flush();
	mItemsInLocalStream = 0;
}

#ifdef ML_WINDOWS
#define UNICODE

#include <Windows.h>
const int kWideBufSize = 16384;
static wchar_t wideBuf[kWideBufSize];
void MLTextStream::display()
{
	/*
	if (!(juce::MessageManager::getInstance()->isThisTheMessageThread())) 
	{
		return;
	}
	*/
	if(mpListener)
	{
		mpListener->display();
	}
	else
	{
#if DEBUG
		
		// no listener, send to standard output
		flush();
		std::string outStr = mLocalStream.str();
		int size = outStr.size();
		if(size > 0)
		{
		//	if(outStr[size - 1] == '\n')
			{		
				const char* cStr = outStr.c_str();
				mLocalStream.str("");
				{
					MultiByteToWideChar(0, 0, cStr, -1, wideBuf, kWideBufSize);
					OutputDebugString(wideBuf);
				}
			}

						
		//	Logger::outputDebugString(mLocalStream.str().c_str());


		}
#endif // DEBUG
	}
}
#elif ML_LINUX
void MLTextStream::display()
{
}

#endif 

// global entry points

// Send a message to the application or plugin’s debug output.
// in release builds this will be disabled completely.
//

// Send a message to the application or plugin’s console, if one exists. 
//
MLTextStream& MLConsole()
{
	static MLTextStream theConsoleMessageStream("console");
	return theConsoleMessageStream;
}

// Send a message to the application or plugin’s console, if one exists. 
//
#if DEBUG
MLTextStream& debug()
{
	static MLTextStream theDebugMessageStream("debug");
	return theDebugMessageStream;
}
#else

MLDummyStream::MLDummyStream(const char* name)  {}
MLDummyStream::~MLDummyStream()  {}
MLDummyStream& debug()
{
	static MLDummyStream theDebugMessageStream("dummy");
	return theDebugMessageStream;
}
#endif 

#endif // DEBUG_TO_COUT



/*
class MLDebugThread : public juce::Thread
{
public:
	MLDebugThread() :
	Thread(juce::String("madronalib_debug"))
	{
	}
	
	~MLDebugThread()
	{
		stopThread(100);
	}
	
	void run()
	{
		while(1)
		{
			if (threadShouldExit())
				return;
			debug().display();
			wait(10);
		}
	}
};

MLDebugThread& theDebugThread()
{
	static MLDebugThread theDebugThreadObject;
	return theDebugThreadObject;
}

*/