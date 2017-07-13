
// Part of the Soundplane client software by Madrona Labs.
// Copyright (c) 2013 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

#include "TouchTracker.h"

#include <algorithm>
#include <numeric>


template<size_t ROW_LENGTH>
void appendVectorToRow(std::array<Vec4, ROW_LENGTH>& row, Vec4 b)
{
	// if full (last element is not null), return
	if(row[ROW_LENGTH - 1]) { debug() << "!"; return; }
	
	auto firstNull = std::find_if(row.begin(), row.end(), [](Vec4 a){ return !bool(a); });
	*firstNull = b;
}


inline float cityBlockDistance(Vec4 a, Vec4 b)
{
	return fabs(a.x() - b.x()) + fabs(a.y() - b.y());
}

inline float cityBlockDistanceXYZ(Vec4 a, Vec4 b)
{
	// if z scale is too small, zero touches will get matched with new active ones in the same position.
	// if too big, z is more important than position and nothing works
	float kZScale = 20.f;
	
	return fabs(a.x() - b.x()) + fabs(a.y() - b.y()) + kZScale*fabs(a.z() - b.z());
}

float sensorToKeyY(float sy)
{
	float ky = 0.f;

	// Soundplane A as measured
	constexpr int mapSize = 6;
	constexpr std::array<float, mapSize> sensorMap{{0.25, 1.1, 2.8, 4.2, 5.9, 6.6}};
	constexpr std::array<float, mapSize> keyMap{{0.25, 1., 2., 3., 4., 4.75}};
	
	if(sy < sensorMap[0])
	{
		ky = keyMap[0];
	}
	else if(sy > sensorMap[mapSize - 1])
	{
		ky = keyMap[mapSize - 1];
	}
	else
	{
		// TODO piecewise map MLRange type
		for(int i = 1; i<mapSize; ++i)
		{
			if(sy <= sensorMap[i])
			{
				// piecewise linear
				float m = (sy - sensorMap[i - 1])/(sensorMap[i] - sensorMap[i - 1]);
				ky = lerp(keyMap[i - 1], keyMap[i], m);
				break;
			}
		}
	}
	
	return ky;
}


TouchTracker::TouchTracker(int w, int h) :
	mWidth(w),
	mHeight(h),
	mpIn(0),
	mFilterThreshold(0.01f),
	mOnThreshold(0.03f),
	mOffThreshold(0.02f),
	mCount(0),
	mMaxTouchesPerFrame(0),
	mNeedsClear(true),
	mSampleRate(1000.f),
	mLopassXY(5.),
	mLopassZ(50.),
	mRotate(false),
	mPairs(false)
{
	mFilteredInput.setDims(w, h);
	mFilteredInputX.setDims(w, h);
	mFilteredInputY.setDims(w, h);
	mCalibrationProgressSignal.setDims(w, h);

	// clear key states
	for(auto& row : mKeyStates.data)
	{
		row.fill(Vec4());	
	} 	
	
	// clear previous pings
	for(auto& row : mPingsHorizY1.data)
	{
		row.fill(Vec4::null());	
	} 
	for(auto& row : mPingsVertY1.data)
	{
		row.fill(Vec4::null());	
	} 
	
	for(int i = 0; i < kMaxTouches; i++)
	{
		mTouchSortOrder[i] = i;
		mRotateShuffleOrder[i] = i;
	}
}
		
TouchTracker::~TouchTracker()
{
}

void TouchTracker::setInputSignal(MLSignal* pIn)
{ 
	mpIn = pIn; 
}

void TouchTracker::setOutputSignal(MLSignal* pOut)
{ 
	mpOut = pOut; 
	int w = pOut->getWidth();
	int h = pOut->getHeight();
	
	if (w < 5)
	{
		debug() << "TouchTracker: output signal too narrow!\n";
		return;
	}
	if (h < mMaxTouchesPerFrame)
	{
		debug() << "error: TouchTracker: output signal too short to contain touches!\n";
		return;
	}
}

void TouchTracker::setMaxTouches(int t)
{
	int newT = clamp(t, 0, kTrackerMaxTouches);
	if(newT != mMaxTouchesPerFrame)
	{
		mMaxTouchesPerFrame = newT;
		
		// reset shuffle order
		for(int i = 0; i < kMaxTouches; i++)
		{
			mTouchSortOrder[i] = i;
			mRotateShuffleOrder[i] = i;
		}
	}
}

void TouchTracker::setRotate(bool b)
{ 
	mRotate = b; 
	for(int i = 0; i < kMaxTouches; i++)
	{
		mRotateShuffleOrder[i] = i;
	}
}

void TouchTracker::setPairs(bool b)
{ 
	mPairs = b;
}

void TouchTracker::clear()
{
	for (int i=0; i<kMaxTouches; i++)	
	{
		mTouches[i] = Vec4();	
		mTouches1[i] = Vec4();	
	}
	mNeedsClear = true;
}

void TouchTracker::setThresh(float f) 
{ 
	mOnThreshold = clamp(f, 0.0005f, 1.f); 
	mFilterThreshold = mOnThreshold * 0.25f; 
	mOffThreshold = mOnThreshold * 0.75f; 
	
	debug() << "mOnThreshold: " << mOnThreshold << "\n";
	
}

void TouchTracker::setLoThresh(float f) 
{ 
	mLoPressureThreshold = f*0.01;
}

void TouchTracker::setLopassXY(float k)
{ 
	mLopassXY = k; 
}

void TouchTracker::setLopassZ(float k)
{ 
	mLopassZ = k; 
}
			
// --------------------------------------------------------------------------------

#pragma mark process
	
void TouchTracker::process(int)
{	
	if (!mpIn) return;
	const MLSignal& in(*mpIn);
	
	mFilteredInput.copy(in);
	
	// clear edges (should do earlier! TODO)
	int w = in.getWidth();
	int h = in.getHeight();
	for(int j=0; j<h; ++j)
	{
		mFilteredInput(0, j) = 0;
		mFilteredInput(w - 1, j) = 0;
	}
		
	// filter out any negative values. negative values can shows up from capacitive coupling near edges,
	// from motion or bending of the whole instrument, 
	// from the elastic layer deforming and pushing up on the sensors near a touch. 
	mFilteredInput.sigMax(0.f);

	{	
		// convolve input with 3x3 smoothing kernel.
		// a lot of filtering is needed here to get good position accuracy for Soundplane A.
		float kc, kex, key, kk;	
		kc = 4./18; kex = 3./18.; key = 2./18.; kk=1./18.;
		mFilteredInput.convolve3x3xy(kc, kex, key, kk);
		mFilteredInput.convolve3x3xy(kc, kex, key, kk);
		mFilteredInput.convolve3x3xy(kc, kex, key, kk);
		mFilteredInput.convolve3x3xy(kc, kex, key, kk);

		{
			std::lock_guard<std::mutex> lock(mCalibratedSignalMutex);
			mCalibratedSignal = mFilteredInput;
		}
		
		if(mMaxTouchesPerFrame > 0)
		{
			mThresholdBits = findThresholdBits(mFilteredInput);
						
			mPingsHorizRaw = correctPingsH(findPings<kSensorRows, kSensorCols, 0>(mThresholdBits, mFilteredInput));
			mPingsVertRaw = correctPingsV(findPings<kSensorCols, kSensorRows, 1>(mThresholdBits, mFilteredInput));						
			mKeyStates = pingsToKeyStates(mPingsHorizRaw, mPingsVertRaw);
			
			// get touches, in key coordinates
			mTouchesRaw = findTouches(mKeyStates);
			mTouches = reduceCrowdedTouches(mTouchesRaw);

			/*
			// pairs
			if(mPairs)
			{
				mTouches = createPairsV(mTouches);
				mTouches = createPairsH(mTouches);
			}
			*/
			
			// sort touches by z. 
			std::sort(mTouches.begin(), mTouches.begin() + kMaxTouches, [](Vec4 a, Vec4 b){ return a.z() > b.z(); } );
			
			// match -> position filter -> z filter -> feedback
			mTouches = matchTouches(mTouches, mTouchesMatch1);	
			mTouches = filterTouchesXYAdaptive(mTouches, mTouchesMatch1);
			mTouches = filterTouchesZ(mTouches, mTouchesMatch1, 100.f, 50.f);			
			mTouchesMatch1 = mTouches;
			
			// variable z filter from user setting
			mTouches = filterTouchesZ(mTouches, mTouches2, mLopassZ*2.f, mLopassZ*0.25f);
			mTouches2 = mTouches;
			
			// after variable filter, exile decayed touches. Note this affects match feedback!
			mTouchesMatch1 = exileUnusedTouches(mTouchesMatch1, mTouches);
			
						
			//	TODO hysteresis after matching to prevent glitching 
			//			mTouches = sortTouchesWithHysteresis(mTouches, mTouchSortOrder);			
			//			mTouches = limitNumberOfTouches(mTouches);
			
			if(mRotate)
			{
				mTouches = rotateTouches(mTouches);
			}	
						
			mTouches = clampTouches(mTouches);
						
			{
				std::lock_guard<std::mutex> lock(mThresholdBitsMutex);
				mThresholdBitsOut = mThresholdBits;
			}
			
			{
				std::lock_guard<std::mutex> lock(mPingsHorizRawOutMutex);
				mPingsHorizRawOut = mPingsHorizRaw;
			}
			
			{
				std::lock_guard<std::mutex> lock(mPingsHorizOutMutex);
				mPingsHorizOut = mPingsHoriz;
			}
			
			{
				std::lock_guard<std::mutex> lock(mPingsVertOutMutex);
				mPingsVertOut = mPingsVert;
			}
			
			{
				std::lock_guard<std::mutex> lock(mPingsVertRawOutMutex);
				mPingsVertRawOut = mPingsVertRaw;
			}
			
			{
				std::lock_guard<std::mutex> lock(mKeyStatesOutMutex);
				mKeyStatesOut = mKeyStates;
			}
			
			{
				std::lock_guard<std::mutex> lock(mTouchesRawOutMutex);
				mTouchesRawOut = mTouchesRaw;
			}
		}

		outputTouches(mTouches);
		
		{
			std::lock_guard<std::mutex> lock(mTouchesOutMutex);
			mTouchesOut = mTouches;
		}

	}

	if (mCount++ > 1000) 
	{
		mCount = 0;			 
	}   
 
}

TouchTracker::SensorBitsArray TouchTracker::findThresholdBits(const MLSignal& in)
{
	SensorBitsArray y;
	
	int w = in.getWidth();
	int h = in.getHeight();
	for(int j=0; j<h; ++j)
	{
		for(int i=0; i<w; ++i)
		{
			y[j*w + i] = (in(i, j) > mLoPressureThreshold);
		}
	}
	
	return y;
}


// new ping finder using z'' minima and parabolic interpolation
template<size_t ARRAYS, size_t ARRAY_LENGTH, bool XY>
VectorArray2D<ARRAYS, ARRAY_LENGTH> TouchTracker::findPings(const SensorBitsArray& inThresh, const MLSignal& inSignal)
{
	VectorArray2D<ARRAYS, ARRAY_LENGTH> y;

	for(int j=0; j<ARRAYS; ++j)
	{
		
		// get row or column of input bits
		std::bitset<ARRAY_LENGTH> inThreshArray;
		if(!XY)
		{
			for(int k=0; k<ARRAY_LENGTH; ++k)
			{
				inThreshArray[k] = inThresh[j*kSensorCols + k];
			}
		}
		else
		{
			for(int k=0; k<ARRAY_LENGTH; ++k)
			{
				inThreshArray[k] = inThresh[k*kSensorCols + j];
			}
		}
		
		y.data[j].fill(Vec4::null());
		
		// find a span
		int intSpanStart = 0;
		int intSpanEnd = 0;		
		bool spanActive = false;
		bool spanComplete = false;
		
		for(int i=0; i<=ARRAY_LENGTH; ++i)
		{
			bool t = (i < ARRAY_LENGTH) ? inThreshArray[i] : 0;
			if(t)
			{
				if(!spanActive)
				{
					intSpanStart = i;
					spanActive = true;
				}
			}
			else
			{
				if(spanActive)
				{
					intSpanEnd = i;
					spanComplete = true;
					spanActive = false;
				}
			}
						
			if(spanComplete)
			{				
				// checking against a minimum span length will filter out some more noise.
				// tweaked by inspection---happens to be the same for x and y right now
				constexpr int kMinSpanLength = XY ? 4 : 4;
				
				// if span ends are not on borders, calculate the length for check. Otherwise we have to assume it's long enough.
				const int spanLength = ((intSpanStart > 0)&&(intSpanEnd < ARRAY_LENGTH)) ? (intSpanEnd - intSpanStart) : kMinSpanLength;
				
		//		if(spanLength >= kMinSpanLength)
				{
					// span acquired, look for pings
					float z = 0.f;
					float zm1 = 0.f;
					float zm2 = 0.f;
					float zm3 = 0.f;
					float dz = 0.f;
					float dzm1 = 0.f;
					float dzm2 = 0.f;
					float dzm3 = 0.f;
					float ddz = 0.f;
					float ddzm1 = 0.f;
					float ddzm2 = 0.f;
					
					// need to iterate before and after the span to get derivatives flowing
					constexpr int margin = 1;
					
					for(int i = intSpanStart - margin; i <= intSpanEnd + margin; ++i)
					{
						z = (within(i, 0, static_cast<int>(ARRAY_LENGTH))) ? (XY ? inSignal(j, i) : inSignal(i, j)) : 0.f;
						dz = z - zm1;
						ddz = dz - dzm1;
						
						// find ddz minima: peaks of curvature
						const float kAxisScale = XY ? 1.f : 2.f;
						float k = -ddzm1*kAxisScale;
						if((ddzm1 < ddz) && (ddzm1 < ddzm2) && (k > 0.f))
						{ 
							// get peak by quadratic interpolation
							float a = ddzm2;
							float b = ddzm1;
							float c = ddz;

							float p = ((a - c)/(a - 2.f*b + c))*0.5f;
							float x = i - 2.f + p;							
							float za = zm3;
							float zb = zm2;
							float zc = zm1;
							float zPeak = zb - 0.25f*(za - zc)*p;
							
							if(within(x, intSpanStart + 0.f, intSpanEnd - 0.f))
							{
								appendVectorToRow(y.data[j], Vec4(x, zPeak, k, 0.f));
							}
						}
						
						zm3 = zm2;
						zm2 = zm1;
						zm1 = z;
						dzm3 = dzm2;
						dzm2 = dzm1;
						dzm1 = dz;
						ddzm2 = ddzm1;
						ddzm1 = ddz;
					}	
				}
				spanComplete = false;
				intSpanStart = 0;
				intSpanEnd = 0;
			}
		}
	}

	return y;
}

float triWindow(float x, float r)
{
	float y;
	if(x > 0.f)
	{
		y = 1.0f - x/r;
	}
	else
	{
		y = 1.0f + x/r;
	}
	y = clamp(y, 0.f, 1.f);
	return y;
}


// touches appear to push away lighter touches around 2.0 key widths from them. This is probably only needed for Soundplane Model A.
TouchTracker::VectorsH TouchTracker::correctPingsH(const TouchTracker::VectorsH& pings)
{
	// ping distances are in sensor coords
	const float kCorrectCenterDist = 4.0f;
	const float kCorrectRadius = 4.0f;
	
	// by inspection. the response is not quite linear with pressure, so that could be improved.
	const float kCorrectAmount = 0.5f;
	
	TouchTracker::VectorsH out;
	
	int j = 0;
	for(auto pingsArray : pings.data)
	{
		int n = 0;
		for(Vec4 ping : pingsArray)
		{
			if(!ping) break;
			n++;
		}
		
		std::array<Vec4, kSensorCols> outArray = pingsArray;
		
		for(int i=0; i<n - 1; ++i)
		{
			Vec4 leftIn = pingsArray[i];
			Vec4 rightIn = pingsArray[i + 1];
			Vec4& leftOut = outArray[i];
			Vec4& rightOut = outArray[i + 1];
			float d = rightIn.x() - leftIn.x();
			
			if(within(d, kCorrectCenterDist - kCorrectRadius, kCorrectCenterDist + kCorrectRadius))
			{					
				float zl = leftIn.z();
				float zr = rightIn.z();
				float winScale = triWindow(leftIn.x() - (rightIn.x() - kCorrectCenterDist),  kCorrectRadius);
				
				if(zr > zl)
				{
					// nudge left of pair to right
					float zRatioScale = sqrtf(clamp(zr/zl - 1.f, 0.f, 100.0f)); 	
					float correctScaled = clamp(kCorrectAmount*winScale*zRatioScale, 0.f, 1.f);
					leftOut.setX(leftOut.x() + correctScaled);	
				}
				else
				{				
					// nudge right of pair to left
					float zRatioScale = sqrtf(clamp(zl/zr - 1.f, 0.f, 100.0f)); 	
					float correctScaled = - clamp(kCorrectAmount*winScale*zRatioScale, 0.f, 1.f);					
					rightOut.setX(rightOut.x() + correctScaled);	
				}
			}
		}
		
		out.data[j] = outArray;
		
		j++;
	}
	return out;
}

// touches appear to push away lighter touches around 2.0 key widths from them. This is probably only needed for Soundplane Model A.
TouchTracker::VectorsV TouchTracker::correctPingsV(const TouchTracker::VectorsV& pings)
{
	// ping distances are in sensor coords
	const float kCorrectCenterDist = 4.0f;
	const float kCorrectRadius = 2.0f;
	
	// by inspection. the response is not quite linear with pressure, so that could be improved.
	const float kCorrectAmount = 0.25f;
	
	TouchTracker::VectorsV out;
	
	int j = 0;
	for(auto pingsArray : pings.data)
	{
		int n = 0;
		for(Vec4 ping : pingsArray)
		{
			if(!ping) break;
			n++;
		}
		
		std::array<Vec4, kSensorRows> outArray = pingsArray;
		
		for(int i=0; i<n - 1; ++i)
		{
			Vec4 leftIn = pingsArray[i];
			Vec4 rightIn = pingsArray[i + 1];
			Vec4& leftOut = outArray[i];
			Vec4& rightOut = outArray[i + 1];
			float d = rightIn.x() - leftIn.x();
			
			if(within(d, kCorrectCenterDist - kCorrectRadius, kCorrectCenterDist + kCorrectRadius))
			{					
				float zl = leftIn.z();
				float zr = rightIn.z();
				float winScale = triWindow(leftIn.x() - (rightIn.x() - kCorrectCenterDist),  kCorrectRadius);
				
				if(zr > zl)
				{
					// nudge left of pair to right
					float zRatioScale = sqrtf(clamp(zr/zl - 1.f, 0.f, 100.0f)); 	
					float correctScaled = clamp(kCorrectAmount*winScale*zRatioScale, 0.f, 1.f);
					leftOut.setX(leftOut.x() + correctScaled);	
				}
				else
				{				
					// nudge right of pair to left
					float zRatioScale = sqrtf(clamp(zl/zr - 1.f, 0.f, 100.0f)); 	
					float correctScaled = - clamp(kCorrectAmount*winScale*zRatioScale, 0.f, 1.f);					
					rightOut.setX(rightOut.x() + correctScaled);	
				}
			}
		}
		
		out.data[j] = outArray;
		
		j++;
	}
	
	return out;
}

// convert the pings to key states by keeping the maximum vert and horiz pings in each key state, then multiplying vert by horiz. 
//
TouchTracker::KeyStates TouchTracker::pingsToKeyStates(const TouchTracker::VectorsH& pingsHoriz, const TouchTracker::VectorsV& pingsVert)
{
	MLRange sensorToKeyX(3.5f, 59.5f, 1.f, 29.f);
		
	TouchTracker::KeyStates keyStates;
	
	int j = 0;
	for(auto pingsArray : pingsHoriz.data)
	{
		for(Vec4 ping : pingsArray)
		{
			if(!ping) break;
			
			float px = sensorToKeyX(ping.x());
			float py = sensorToKeyY(j);
			float pk = ping.z(); 
			
			int kxa = clamp(static_cast<int>(floorf(px)), 0, kKeyCols - 1);
			int kya = clamp(static_cast<int>(floorf(py)), 0, kKeyRows - 1);
			Vec4& xaya = (keyStates.data[kya])[kxa];
		
			if(pk > xaya.z())
			{
				xaya.setX(px); // x at max z
				xaya.setZ(pk); // max z for x ping -> z
			}
		}
		j++;
	}

	int i = 0;
	for(auto pingsArray : pingsVert.data)
	{		
		int n = 0;
		
		for(Vec4 ping : pingsArray)
		{
			if(!ping) break;
			n++;
				
			float px = sensorToKeyX(i);
			float py = sensorToKeyY(ping.x());
			float pk = ping.z(); 
			
			int kxa = clamp(static_cast<int>(floorf(px)), 0, kKeyCols - 1);
			int kya = clamp(static_cast<int>(floorf(py)), 0, kKeyRows - 1);
			Vec4& xaya = (keyStates.data[kya])[kxa];		
			
			if(pk > xaya.w())
			{
				xaya.setY(py); // y at max z
				xaya.setW(pk); // max z for y ping -> w
			}
		}
		i++;
	}

	// get ping locations and pressures by combining vert and horiz
	{
		const float kPressureScale = 24.f;
		int j = 0;
		for(auto& keyStatesArray : keyStates.data)
		{			
			int i = 0;
			for(Vec4& key : keyStatesArray)
			{			
				float cx = key.x();
				float cy = key.y();
				float cz = key.z();
				float cw = key.w();

				if((cz > 0.f) && (cw > 0.f)) 
				{					
					key.setX(cx - i);
					key.setY(cy - j);
					key.setZ(sqrtf((cz)*(cw)) * kPressureScale);
					key.setW(0.f);
				}
				else
				{
					// return key center - doesn't matter currently because with 0 z the state is not used by the touch filter
					key = Vec4(0.5f, 0.5f, 0.f, 0.f);
				}

				i++;
			}
			j++;
		}
	}
	
	return keyStates;
}

// look at key states to find touches.
std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::findTouches(const TouchTracker::KeyStates& keyStates)
{
	std::array<Vec4, kMaxTouches> touches;
	touches.fill(Vec4()); // zero value, not null
	
	int nTouches = 0;
	int j = 0;
	for(auto& row : keyStates.data)
	{
		int i = 0;
		for(Vec4 key : row)
		{
			float x = key.x();
			float y = key.y();
			float z = key.z();

			if(z > 0.f) 
			{
				float sensorX = (i + x);
				float sensorY = (j + y);
				
				if(nTouches < kMaxTouches)
				{
					touches[nTouches++] = Vec4(sensorX, sensorY, z, 0);
				}
			}
			
			i++;
		}
		j++;
	}
	
	return touches;
}


std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::reduceCrowdedTouches(const std::array<Vec4, TouchTracker::kMaxTouches>& in)
{	
	float kCrowdedDistance = 4.f; 	
	
	// > 1 to allow close touches of near equal z to reduce each other
	float kOtherTouchZMult = 2.f;
	
	std::array<Vec4, kMaxTouches> out(in);
	
	// for each touch i, for each neighbor j of higher z, reduce i.z as linear falloff with distance.
	for(int i=0; i<mMaxTouchesPerFrame; ++i)
	{
		Vec4& ta = out[i];		
		float ax = ta.x();
		float ay = ta.y();
		float az = ta.z();
		
		for(int j = 0; j<mMaxTouchesPerFrame; ++j)
		{
			if(j != i)
			{
				Vec4 tb = in[j];
				float bz = tb.z();							
				if(bz*kOtherTouchZMult > az)
				{
					float dab = cityBlockDistance(ta, tb);
					if((dab > 0.f) && (dab < kCrowdedDistance))
					{											
						float unitySlope = 1.f - dab/kCrowdedDistance;
						unitySlope = clamp(unitySlope, 0.f, 1.f);
						
						float slopeAwayFromB = bz*kOtherTouchZMult*unitySlope;
						if(slopeAwayFromB > az)
						{
							float r = (slopeAwayFromB - az);
							az -= r;
						}
					}
				}
			}
		}
		az = clamp(az, 0.f, 1.f);
		ta.setX(ax);
		ta.setY(ay);
		ta.setZ(az);
	}
	return out;
}


// sort the input touches in z order. A hysteresis offset for each array member prevents members from changing order too often.
// side effect: the new sorted order is written to the currentSortedOrder array.
//
std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::sortTouchesWithHysteresis(const std::array<Vec4, TouchTracker::kMaxTouches>& in, std::array<int, TouchTracker::kMaxTouches>& previousSortedOrder)
{	
	const float kHysteresisOffset = 0.01f; // TODO adjust
	
	std::array<Vec4, kMaxTouches> preSort(in); // TEMP
	std::array<Vec4, kMaxTouches> postSort(in); // TEMP
	std::array<Vec4, kMaxTouches> touches(in);
	std::array<int, TouchTracker::kMaxTouches> newSortedOrder;
	
	// count input touches
	int n = 0;
	for(int i=0; i<kMaxTouches; ++i)
	{
		if(preSort[i].z() == 0.f) break;
		n++;
	}
	
	// sort by x first to give stable initial order
	std::sort(preSort.begin(), preSort.begin() + kMaxTouches, [](Vec4 a, Vec4 b){ return a.x() > b.x(); } );
	
	postSort = preSort; // TEMP
	
	// add multiples of hysteresis offset to input data according to previous sorted order
	for(int i = 0; i < kMaxTouches; i++)
	{
		int v = kMaxTouches - i;
		postSort[previousSortedOrder[i]].setZ(postSort[previousSortedOrder[i]].z() + v*kHysteresisOffset);
		postSort[i].setW(i); // stash index in w
	}
	
	std::sort(postSort.begin(), postSort.begin() + kMaxTouches, [](Vec4 a, Vec4 b){ return a.z() > b.z(); } );
	
	// get new sorted order
	for(int i = 0; i < kMaxTouches; i++)
	{
		newSortedOrder[i] = postSort[i].w();
	}
	
	// get touches in sorted order without hysteresis
	for(int i = 0; i < kMaxTouches; i++)
	{
		touches[i] = preSort[newSortedOrder[i]];
	}	
	
	// compare sorted orders
	bool orderChanged = false;
	for(int i=0; i<kMaxTouches; ++i)
	{
		if(previousSortedOrder[i] != newSortedOrder[i])
		{
			orderChanged = true;
			break;
		}
	}
	
	if(0)
		if(n > 1)
		{
			
			debug() << "\n    inputs: ";		
			for(int i=0; i<kMaxTouches; ++i)
			{			
				debug() << in[i].z() << " ";
			}
			
			debug() << "\n    pre: ";		
			for(int i=0; i<kMaxTouches; ++i)
			{			
				debug() << preSort[i].z() << " ";
			}
			
			debug() << "\n    post: ";		
			for(int i=0; i<kMaxTouches; ++i)
			{			
				debug() << postSort[i].z() << " ";
			}
			
			
			debug() << "\n   prev: ";
			for(int i = 0; i < kMaxTouches; i++)
			{
				debug() << previousSortedOrder[i] << " ";
			}
			
			debug() << "\n   new: ";
			for(int i = 0; i < kMaxTouches; i++)
			{
				debug() << newSortedOrder[i] << " ";
			}
			
			debug() << "\n    outputs: ";
			
			for(int i=0; i<kMaxTouches; ++i)
			{			
				debug() << touches[i].z() << " ";
			}
			
			
			debug() << "\n";		 
		}
	previousSortedOrder = newSortedOrder;
	
	if(mCount == 0)
	{
		debug() << "sort: ";
		for(int i = 0; i < mMaxTouchesPerFrame; i++)
		{
			debug() << touches[i];
		}
		debug() << "\n";
	}
	
	return touches;
}

std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::limitNumberOfTouches(const std::array<Vec4, TouchTracker::kMaxTouches>& in)
{	
	std::array<Vec4, kMaxTouches> touches(in);
	
	/*
	 int nTouches = 0;
	 for(int i = 0; i < touches.size(); i++)
	 {
		if (touches[i].z() == 0.)
		{
	 nTouches = i;
	 break;
		}
	 }
	 */
	
	// limit number of touches by overwriting with zeroes
	for(int i = mMaxTouchesPerFrame; i < kMaxTouches; i++)
	{
		touches[i] = Vec4();
	}
	
	if(mCount == 0)
	{
		debug() << "limit: ";
		for(int i = 0; i < mMaxTouchesPerFrame; i++)
		{
			debug() << touches[i];
		}
		debug() << "\n";
	}
	
	return touches;
}


// match incoming touches in x with previous frame of touches in x1.
// for each possible touch slot, output the touch x closest in location to the previous frame.
// if the incoming touch is a continuation of the previous one, set its age (w) to 1, otherwise to 0. 
// if there is no incoming touch to match with a previous one at index i, and no new touch needs index i, the position at index i will be maintained.
//
std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::matchTouches(const std::array<Vec4, TouchTracker::kMaxTouches>& x, const std::array<Vec4, TouchTracker::kMaxTouches>& x1)
{
	const float kMaxConnectDist = 2.f; 
	
	std::array<Vec4, kMaxTouches> newTouches;
	newTouches.fill(Vec4());
	
	std::array<int, kMaxTouches> forwardMatchIdx; 
	forwardMatchIdx.fill(-1);	
	
	std::array<int, kMaxTouches> reverseMatchIdx; 
	reverseMatchIdx.fill(-1);

	// for each previous touch, find minimum distance to a current touch.
	// matching with zero pressure is OK, because it lets us restart touches
	// that went to 0 for a little bit
	for(int i=0; i<mMaxTouchesPerFrame; ++i)
	{
		float minDist = MAXFLOAT;
		Vec4 prev = x1[i];
		
		for(int j=0; j < mMaxTouchesPerFrame; ++j)
		{
			Vec4 curr = x[j];
			float distToCurrentTouch = cityBlockDistanceXYZ(prev, curr);
			if(distToCurrentTouch < minDist)
			{
				forwardMatchIdx[i] = j;
				minDist = distToCurrentTouch;						
			}
		}
	}

	// for each current touch, find minimum distance to a previous touch
	for(int i=0; i<mMaxTouchesPerFrame; ++i)
	{
		float minDist = MAXFLOAT;
		Vec4 curr = x[i];
		for(int j=0; j < mMaxTouchesPerFrame; ++j)
		{
			Vec4 prev = x1[j];
			float distToPreviousTouch = cityBlockDistanceXYZ(prev, curr);
			if(distToPreviousTouch < minDist)
			{
				reverseMatchIdx[i] = j;
				minDist = distToPreviousTouch;						
			}
		}
	}

	// get mutual matches
	std::array<int, kMaxTouches> mutualMatches; 
	mutualMatches.fill(0);
	for(int i=0; i<mMaxTouchesPerFrame; ++i)
	{
		Vec4 curr = x[i];
		int prevIdx = reverseMatchIdx[i];
		if(prevIdx >= 0)
		{
			Vec4 prev = x1[prevIdx];
			if(forwardMatchIdx[prevIdx] == i) 	
			{
				mutualMatches[i] = true;
			}
		}
	}
	
	std::array<bool, kMaxTouches> currWrittenToNew; 
	currWrittenToNew.fill(false);
	
	// first, continue well-matched nonzero touches
	for(int i=0; i<mMaxTouchesPerFrame; ++i)
	{
		if(mutualMatches[i])
		{
			int j = reverseMatchIdx[i];
			Vec4 curr = x[i];		
			Vec4 prev = x1[j];
			if((curr.z() > mFilterThreshold) && (prev.z() > mFilterThreshold))
			{			
				// touch is continued, mark as connected and write to new touches
				curr.setW(cityBlockDistance(prev, curr) < kMaxConnectDist);						
				newTouches[j] = curr;
				currWrittenToNew[i] = true;		
			}
		}
	}
		
	// now take care of any remaining nonzero current touches
	int remaining = 0;
	for(int i=0; i<mMaxTouchesPerFrame; ++i)
	{
		Vec4 curr = x[i];		
		if((!currWrittenToNew[i]) && (curr.z() > mFilterThreshold))
		{
			int freeIdx = -1;
			float minDist = MAXFLOAT;
			
			// first, try to match same touch index (important for decay!)
			Vec4 prev = x1[i];
			if(prev.z() <= mFilterThreshold)
			{
				freeIdx = i;
			}
			
			// then try closest free touch
			if(freeIdx < 0)
			{					
				for(int j=0; j<mMaxTouchesPerFrame; ++j)
				{
					Vec4 prev = x1[j];
					if(prev.z() <= mFilterThreshold)
					{
						float d = cityBlockDistance(curr, prev);
						if(d < minDist)
						{
							minDist = d;
							freeIdx = j;
						}
					}
				}
			}
						
			// if a free index was found, write the current touch						
			if(freeIdx >= 0)
			{					
				Vec4 free = x1[freeIdx];
				curr.setW(cityBlockDistance(free, curr) < kMaxConnectDist);				
				newTouches[freeIdx] = curr;	
			}
		}
	}	
	
	// fill in any free touches with previous touches at those indices. This will allow old touches to re-link if not reused.
	for(int i=0; i < mMaxTouchesPerFrame; ++i)
	{
		Vec4 t = newTouches[i];
		if(t.z() <= mFilterThreshold)
		{
			newTouches[i].setX(x1[i].x());
			newTouches[i].setY(x1[i].y());
		}
	}

	return newTouches;
}

// input: vec4<x, y, z, k> where k is 1 if the touch is connected to the previous touch at the same index.
//
std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::filterTouchesXYAdaptive(const std::array<Vec4, TouchTracker::kMaxTouches>& in, const std::array<Vec4, TouchTracker::kMaxTouches>& inz1)
{
	float sr = 1000.f; // Soundplane A
	const float kFixedXYFreqMax = 20.f;
	const float kFixedXYFreqMin = 0.5f;
	MLRange zToXYFreq(0., 0.1, kFixedXYFreqMin, kFixedXYFreqMax); 
	
	std::array<Vec4, TouchTracker::kMaxTouches> out;
	
	for(int i=0; i<mMaxTouchesPerFrame; ++i)
	{
		float x = in[i].x();
		float y = in[i].y();
		float z = in[i].z();
		float w = in[i].w(); 
		
		float x1 = inz1[i].x();
		float y1 = inz1[i].y();
		
		// filter, or not
		float newX, newY;
		if(w > 0.f)
		{
			// get xy coeffs, adaptive based on z
			float freq = zToXYFreq.convertAndClip(z);			
			float omegaXY = freq*kMLTwoPi/sr;
			float kXY = expf(-omegaXY);
			float a0XY = 1.f - kXY;
			float b1XY = kXY;
			
			// onepole filters			
			newX = (x*a0XY) + (x1*b1XY);
			newY = (y*a0XY) + (y1*b1XY);		
		}
		else
		{
			newX = x;
			newY = y;
		}
		
		out[i] = Vec4(newX, newY, z, w);
	}
	
	return out;
}

// input: vec4<x, y, z, k> where k is 1 if the touch is connected to the previous touch at the same index.
//
std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::filterTouchesXY(const std::array<Vec4, TouchTracker::kMaxTouches>& in, const std::array<Vec4, TouchTracker::kMaxTouches>& inz1, float freq)
{
	float sr = 1000.f; // Soundplane A		
	float omegaXY = freq*kMLTwoPi/sr;
	float kXY = expf(-omegaXY);
	float a0XY = 1.f - kXY;
	float b1XY = kXY;
	
	std::array<Vec4, TouchTracker::kMaxTouches> out;
	
	for(int i=0; i<mMaxTouchesPerFrame; ++i)
	{
		float x = in[i].x();
		float y = in[i].y();
		float z = in[i].z();
		float w = in[i].w(); 
		
		float x1 = inz1[i].x();
		float y1 = inz1[i].y();
		
		// filter, or not
		float newX, newY;
		if(w > 0.f)
		{
			// onepole filters			
			newX = (x*a0XY) + (x1*b1XY);
			newY = (y*a0XY) + (y1*b1XY);		
		}
		else
		{
			newX = x;
			newY = y;
		}
		
		out[i] = Vec4(newX, newY, z, w);
	}
	
	return out;
}

std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::filterTouchesZ(const std::array<Vec4, TouchTracker::kMaxTouches>& in, const std::array<Vec4, TouchTracker::kMaxTouches>& inz1, float upFreq, float downFreq)
{
	const float sr = 1000.f;
	const float omegaUp = upFreq*kMLTwoPi/sr;
	const float kUp = expf(-omegaUp);
	const float a0Up = 1.f - kUp;
	const float b1Up = kUp;
	const float omegaDown = downFreq*kMLTwoPi/sr;
	const float kDown = expf(-omegaDown);
	const float a0Down = 1.f - kDown;
	const float b1Down = kDown;
	
	std::array<Vec4, TouchTracker::kMaxTouches> out;
	
	for(int i=0; i<mMaxTouchesPerFrame; ++i)
	{
		float x = in[i].x();
		float y = in[i].y();
		float z = in[i].z();

		float z1 = inz1[i].z();
		float w1 = inz1[i].w();		

		float newZ, newW;
		
		// filter z
		float dz = z - z1;
		if(dz > 0.f)
		{
			newZ = (z*a0Up) + (z1*b1Up);
		}
		else
		{
			newZ = (z*a0Down) + (z1*b1Down);				
		}	
		
		// gate with hysteresis
		bool gate = (w1 > 0);
		if(newZ > mOnThreshold)
		{
			gate = true;
		}
		else if (newZ < mOffThreshold)
		{
			gate = false;
		}
		
		// increment age
		if(!gate)
		{
			newW = 0;
		}
		else
		{
			newW = w1 + 1;
		}
		
		out[i] = Vec4(x, y, newZ, newW);
	}
	
	return out;
}

// if a touch has decayed to 0 after z filtering, move it off the scene so it won't match to other nearby touches.
std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::exileUnusedTouches(const std::array<Vec4, TouchTracker::kMaxTouches>& preFiltered, const std::array<Vec4, TouchTracker::kMaxTouches>& postFiltered)
{
	std::array<Vec4, TouchTracker::kMaxTouches> out(preFiltered);
	for(int i = 0; i < mMaxTouchesPerFrame; ++i)
	{
		Vec4 a = preFiltered[i];
		Vec4 b = postFiltered[i];
		
		if(b.x() > 0.f)
		{
			if(b.z() <= mFilterThreshold) 
			{
				a.setX(-1.f);
				a.setY(-10.f);
				a.setZ(0.f);
			}
		}
		
		out[i] = a;					
	}
	return out;
}

std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::createPairsV(const std::array<Vec4, TouchTracker::kMaxTouches>& in)
{	
	std::array<Vec4, kMaxTouches> out(in);
	const float r = 0.125f;
	
	// count
	int n = 0;
	for(int i = 0; i < mMaxTouchesPerFrame; ++i)
	{
		if(in[i].z() >= mFilterThreshold)
		{
			n = i + 1;
		}
	}
	
	for(int i = 0; i < n; ++i)
	{
		Vec4 touchToAdd;
		bool added = false;
		Vec4 t = out[i];
		float tz = t.z();
		if(tz >= mFilterThreshold)
		{
			float y = t.y();
			int yi = y;
			float fy = y - yi;
			if(fy < r)
			{
				touchToAdd = t;
				touchToAdd.setY(yi - r);
				t.setY(yi + r);
			}
			else if(fy > 1.0f - r)
			{
				touchToAdd = t;
				touchToAdd.setY(yi + 1 + r);
				t.setY(yi + 1 - r);
			}
		}
				
		if(touchToAdd.z() >= mFilterThreshold)
		{
			int freeIdx = -1;
			
			// find free index after i
			for(int j = i + 1; j < mMaxTouchesPerFrame; ++j)
			{
				Vec4 tj = out[j];
				if(tj.z() < mFilterThreshold)
				{
					freeIdx = j;
					break;
				}
			}
			
			if(freeIdx >= 0)
			{
				out[freeIdx] = touchToAdd;
				added = true;
			}
		}
		if(added)
		{
			out[i] = t;
		}
	}	
	return out;
}

std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::createPairsH(const std::array<Vec4, TouchTracker::kMaxTouches>& in)
{	
	std::array<Vec4, kMaxTouches> out(in);
	const float r = 0.125f;
	
	// count
	int n = 0;
	for(int i = 0; i < mMaxTouchesPerFrame; ++i)
	{
		if(in[i].z() >= mFilterThreshold)
		{
			n = i + 1;
		}
	}
	
	for(int i = 0; i < n; ++i)
	{
		Vec4 touchToAdd;
		bool added = false;
		Vec4 t = out[i];
		float tz = t.z();
		if(tz >= mFilterThreshold)
		{
			float x = t.x();
			int xi = x;
			float fx = x - xi;
			if(fx < r)
			{
				touchToAdd = t;
				touchToAdd.setX(xi - r);
				t.setX(xi + r);
			}
			else if(fx > 1.0f - r)
			{
				touchToAdd = t;
				touchToAdd.setX(xi + 1 + r);
				t.setX(xi + 1 - r);
			}
		}
		
		if(touchToAdd.z() >= mFilterThreshold)
		{
			int freeIdx = -1;
			
			// find free index after i
			for(int j = i + 1; j < mMaxTouchesPerFrame; ++j)
			{
				Vec4 tj = out[j];
				if(tj.z() < mFilterThreshold)
				{
					freeIdx = j;
					break;
				}
			}
			
			if(freeIdx >= 0)
			{
				out[freeIdx] = touchToAdd;
				added = true;
			}
		}
		if(added)
		{
			out[i] = t;
		}
	}	
	return out;
}

// rotate order of touches, changing order every time there is a new touch in a frame.
// side effect: writes to mRotateShuffleOrder
std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::rotateTouches(const std::array<Vec4, TouchTracker::kMaxTouches>& in)
{	
	std::array<Vec4, kMaxTouches> touches(in);
	if(mMaxTouchesPerFrame > 1)
	{		
		bool doRotate = false;
		for(int i = 0; i < mMaxTouchesPerFrame; ++i)
		{
			if(in[i].w() == 1)
			{
				// we have a new touch at index i.
				doRotate = true;
				break;
			}
		}
		
		if(doRotate)
		{
			// rotate the indices of all free or new touches in the shuffle order		
			int nFree = 0;
			std::array<int, kMaxTouches> freeIndexes;
			
			for(int i=0; i<mMaxTouchesPerFrame; ++i)
			{
				if((in[i].z() < mFilterThreshold) || (in[i].w() == 1))
				{
					freeIndexes[nFree++] = i;
				}
			}
					
			if(nFree > 1)
			{
				int first = mRotateShuffleOrder[freeIndexes[0]];
				for(int i=0; i < nFree - 1; ++i)
				{				
					mRotateShuffleOrder[freeIndexes[i]] = mRotateShuffleOrder[freeIndexes[i + 1]];
				}
				mRotateShuffleOrder[freeIndexes[nFree - 1]] = first;
			}
		}

		// shuffle
		for(int i = 0; i < mMaxTouchesPerFrame; ++i)
		{
			touches[mRotateShuffleOrder[i]] = in[i];
		}
	}
	return touches;
}

// clamp touches and remove hysteresis threshold.
std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::clampTouches(const std::array<Vec4, TouchTracker::kMaxTouches>& in)
{
	std::array<Vec4, TouchTracker::kMaxTouches> out;
	for(int i = 0; i < mMaxTouchesPerFrame; ++i)
	{
		Vec4 t = in[i];
		
		if(t.x() != t.x())
		{
			debug() << i << "x!";
			t.setX(0.f);
		}
		if(t.y() != t.y())
		{
			debug() << i << "y!";
			t.setY(0.f);
		}
				
		out[i] = t;			
		float newZ = (clamp(t.z() - mOnThreshold, 0.f, 1.f));
		if(t.w() == 0.f)
		{
			newZ = 0.f;
		}
		out[i].setZ(newZ);
	}
	return out;
}

void TouchTracker::outputTouches(std::array<Vec4, TouchTracker::kMaxTouches> touches)
{
	MLSignal& out = *mpOut;
	for(int i = 0; i < mMaxTouchesPerFrame; ++i)
	{
		Vec4 t = touches[i];
		out(xColumn, i) = t.x();
		out(yColumn, i) = t.y();
		out(zColumn, i) = t.z();
		out(ageColumn, i) = t.w();
	}
}
