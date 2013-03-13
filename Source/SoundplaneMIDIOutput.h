
// Part of the Soundplane client software by Madrona Labs.
// Copyright (c) 2013 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

#ifndef __SOUNDPLANE_MIDI_OUTPUT_H_
#define __SOUNDPLANE_MIDI_OUTPUT_H_

#include "JuceHeader.h"

#include <vector>
#include <tr1/memory>

#include "MLDebug.h"
#include "TouchTracker.h"
#include "SoundplaneDataListener.h"
#include "MLTime.h"

const int kMaxMIDIVoices = 16;
const int kSoundplaneMIDIControllerY = 74;


class MIDIVoice
{
public:
	MIDIVoice();
	~MIDIVoice();

	int mAge;
	float mX;
	float mY;
	float mZ;
	float mDz;
	float mNote;
	
	int mNoteOn;
	int mNoteOff;
	int mMIDINote;
	int mMIDIVel;
	
	int mMIDIBend;
	int mMIDIPressure;
	int mMIDIYCtrl;
	
	float mStartNote;
	float mStartX;
	float mStartY;

};

class MIDIDevice
{
public:
	MIDIDevice(const std::string &, int);
	~MIDIDevice();
	const std::string& getName() { return mName; }
	juce::MidiOutput* getDevice();
	juce::MidiOutput* open();
	void close();
	
private:
	std::string mName;
	int mIndex;
	bool mIsInternal; // for interapplication devices
};

typedef std::tr1::shared_ptr<MIDIDevice> MIDIDevicePtr;

class SoundplaneMIDIOutput :
	public SoundplaneDataListener
{
public:
	SoundplaneMIDIOutput();
	~SoundplaneMIDIOutput();
	void initialize();
	
	void modelStateChanged();
	void processFrame(const MLSignal& touchFrame);
	void setDataFreq(float f) { mDataFreq = f; }
	
	void findMIDIDevices ();
	void setDevice(int d);
	void setDevice(const std::string& deviceStr);
	int getNumDevices();
	const std::string& getDeviceName(int d);
	std::list<std::string>& getDeviceList();
	
	void setActive(bool v);
	void setPressureActive(bool v);

	void setMaxTouches(int t) { mVoices = clamp(t, 0, kMaxMIDIVoices); }
	void setBendRange(int r) { mBendRange = r; }
	void setTranspose(int t) { mTranspose = t; }
	void setRetrig(int t) { mRetrig = t; }
	void setAbsRel(int t) { mAbsRel = t; }
	void setHysteresis(float t) { mHysteresis = t; }
	
private:

	int mVoices;
	
	MIDIVoice mMIDIVoices[kMaxMIDIVoices];

	std::vector<MIDIDevicePtr> mDevices;
	std::list<std::string> mDeviceList;
	juce::MidiOutput* mpCurrentDevice;
	
	bool mActive;
	float mDataFreq;
	bool mPressureActive;
	UInt64 mLastTimeDataWasSent;
	UInt64 mLastTimeNRPNWasSent;
	int mBendRange;
	int mTranspose;
	int mRetrig;
	int mAbsRel;
	float mHysteresis;
};


#endif // __SOUNDPLANE_MIDI_OUTPUT_H_
