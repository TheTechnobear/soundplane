
// Part of the Soundplane client software by Madrona Labs.
// Copyright (c) 2013 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

#ifndef __SOUNDPLANE_OSC_OUTPUT_H_
#define __SOUNDPLANE_OSC_OUTPUT_H_

#include <vector>
#include <list>
#include <memory>
#include <stdint.h>

#include "MLDebug.h"
#include "MLTime.h"
#include "SoundplaneDataListener.h"
#include "SoundplaneModelA.h"
#include "TouchTracker.h"
#include "JuceHeader.h"

#include "OscOutboundPacketStream.h"
#include "UdpSocket.h"

extern const char* kDefaultHostnameString;

// default port for t3d plugin communication. Plugins may be receiving on different ports.
const int kDefaultUDPPort = 3123;

// maximum number of ports from kDefaultUDPPort to (kDefaultUDPPort + kNumUDPPorts - 1)
const int kNumUDPPorts = 16;

// Soundplane app input port for Kyma and other config messages
const int kDefaultUDPReceivePort = 3124; // 3122?

const int kUDPOutputBufferSize = 4096;

class OSCVoice
{
public:
	OSCVoice();
	~OSCVoice();

	float startX;
	float startY;
    float x;
    float y;
    float z;
    float note;
	VoiceState mState;
};

class SoundplaneOSCOutput :
	public SoundplaneDataListener
{
public:
	SoundplaneOSCOutput();
	~SoundplaneOSCOutput();
	
	int getKymaMode();
	void setKymaMode(bool m);
	void setKymaPort(int p);

	void connect();

    // SoundplaneDataListener
    void processSoundplaneMessage(const SoundplaneDataMessage* msg);
    
	void setDataFreq(float f) { mDataFreq = f; }
	
	void setActive(bool v);
	void setMaxTouches(int t) { mMaxTouches = clamp(t, 0, kSoundplaneMaxTouches); }
	
	void setSerialNumber(int s) { mSerialNumber = s; }
	void notify(int connected);
	
	void doInfrequentTasks();

private:	
	
	void initializeSocket(int port);
	osc::OutboundPacketStream& getPacketStreamForOffset(int offset);
	UdpTransmitSocket* getTransmitSocketForOffset(int portOffset);
	void sendFrame();
	void sendFrameToKyma();

	int mMaxTouches;	
	
	std::vector< std::vector<OSCVoice> > mOSCVoices;
	int mPortOffsetsByTouch[kSoundplaneMaxTouches];
	int mPrevPortOffsetsByTouch[kSoundplaneMaxTouches];
	
    SoundplaneDataMessage mMessagesByZone[kSoundplaneAMaxZones];
    
	float mDataFreq;
	uint64_t mCurrFrameStartTime;
	uint64_t mLastFrameStartTime;
    bool mTimeToSendNewFrame;

	std::vector< std::vector < char > > mUDPBuffers;
	std::vector< std::unique_ptr< osc::OutboundPacketStream > > mUDPPacketStreams;
	std::vector< std::unique_ptr< UdpTransmitSocket > > mUDPSockets;
	
	int mCurrentBaseUDPPort;
	osc::int32 mFrameId;
	int mSerialNumber;
	
	uint64_t lastInfrequentTaskTime;
	bool mKymaMode;
	int mKymaPort;
    bool mGotNoteChangesThisFrame;
    bool mGotMatrixThisFrame;
    SoundplaneDataMessage mMatrixMessage;
};


#endif // __SOUNDPLANE_OSC_OUTPUT_H_
