
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

	// Soundplane A as measured.
	constexpr int mapSize = 6;
	// these y locations depend on the amount of y smoothing. Currently smoothing 3 times with a [1, 1, 1]
	// kernel, for Soundplane A.
	constexpr std::array<float, mapSize> sensorMap{{0.7, 1.2, 2.7, 4.3, 5.8, 6.3}};
	constexpr std::array<float, mapSize> keyMap{{0.01, 1., 2., 3., 4., 4.99}};
	
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
	mMaxTouchesPerFrame(0),
	mSampleRate(1000.f),
	mLopassXY(5.),
	mLopassZ(50.),
	mRotate(false),
	mPairs(false),
	mCount(0)
{
	mFilteredInput.setDims(w, h);
	mInput1.setDims(w, h);
	mInput2.setDims(w, h);
	mInput3.setDims(w, h);

	// clear key states
	for(auto& row : mKeyStates.data)
	{
		row.fill(Vec4());	
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
	int kmax = kMaxTouches;
	int newT = clamp(t, 0, kmax);
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

void TouchTracker::clear()
{
	for (int i=0; i<kMaxTouches; i++)	
	{
		mTouches[i] = Vec4();	
	}
}

void TouchTracker::setThresh(float f) 
{ 
	mOnThreshold = clamp(f, 0.0005f, 1.f); 
	mFilterThreshold = mOnThreshold * 0.5f; 
	mOffThreshold = mOnThreshold * 0.75f; 
}


void TouchTracker::setLopassXY(float k)
{ 
	mLopassXY = k; 
}

void TouchTracker::setLopassZ(float k)
{ 
	mLopassZ = k; 
}


MLSignal smoothPressureX(const MLSignal& in)
{
	int i, j;
	float f;
	const float * pr1, * pr2, * pr3; // input row ptrs
	float * prOut; 	
	
	// MLTEST
	
	const float kc = 1.f;
	const float kex = 1.f;
	const float key = 0.f;
	const float kk = 0.f;
	
	// MLTEST
	// temp 
	int width = in.getWidth();
	int height = in.getHeight();
	MLSignal out(width, height);
	
	
	const float* pIn = in.getBuffer();
	float* pOut = out.getBuffer();
	
	int wb = in.getWidthBits();
	
	const float kernelSum = kc + 2.f*(kex + key) + 4.f*kk;
	
	
	j = 0;	// top row
	{
		// row ptrs
		pr2 = (pIn + (j << wb));
		pr3 = (pIn + ((j + 1) << wb));
		prOut = (pOut + (j << wb) );
		
		i = 0; // top left corner
		{
			f = kex * (pr2[i+1]);
			f += key * (pr3[i]);
			f += kk * (pr3[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		for(i = 1; i < width - 1; i++) // top side
		{
			f = kex * (pr2[i-1] + pr2[i+1]);
			f += key * (pr3[i]);
			f += kk * (pr3[i-1] + pr3[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		i = width - 1; // top right corner
		{
			f = kex * (pr2[i-1]);
			f += key * (pr3[i]);
			f += kk * (pr3[i-1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
	}
	for(j = 1; j < height - 1; j++) // center rows
	{
		// row ptrs
		pr1 = (pIn + ((j - 1) << wb));
		pr2 = (pIn + (j << wb));
		pr3 = (pIn + ((j + 1) << wb));
		prOut = (pOut + (j << wb));
		
		i = 0; // left side
		{
			f = kex * (pr2[i+1]);
			f += key * (pr1[i] + pr3[i]);
			f += kk * (pr1[i+1] + pr3[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		for(i = 1; i < width - 1; i++) // center
		{
			f = kex * (pr2[i-1] + pr2[i+1]);
			f += key * (pr1[i] + pr3[i]);
			f += kk * (pr1[i-1] + pr1[i+1] + pr3[i-1] + pr3[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		i = width - 1; // right side
		{
			f = kex * (pr2[i-1]);
			f += key * (pr1[i] + pr3[i]);
			f += kk * (pr1[i-1] + pr3[i-1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
	}
	j = height - 1;	// bottom row
	{
		// row ptrs
		pr1 = (pIn + ((j - 1) << wb));
		pr2 = (pIn + (j << wb));
		prOut = (pOut + (j << wb));
		
		i = 0; // bottom left corner
		{
			f = kex * (pr2[i+1]);
			f += key * (pr1[i]);
			f += kk * (pr1[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		for(i = 1; i < width - 1; i++) // bottom side
		{
			f = kex * (pr2[i-1] + pr2[i+1]);
			f += key * (pr1[i]);
			f += kk * (pr1[i-1] + pr1[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		i = width - 1; // bottom right corner
		{
			f = kex * (pr2[i-1]);
			f += key * (pr1[i]);
			f += kk * (pr1[i-1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
	}
	return out;
}



MLSignal smoothPressureY(const MLSignal& in)
{
	int i, j;
	float f;
	const float * pr1, * pr2, * pr3; // input row ptrs
	float * prOut; 	
	
	// MLTEST
	
	const float kc = 1.f;
	const float kex = 0.f;
	const float key = 1.f;
	const float kk = 0.f;
	
	// MLTEST
	// temp 
	int width = in.getWidth();
	int height = in.getHeight();
	MLSignal out(width, height);
	
	
	const float* pIn = in.getBuffer();
	float* pOut = out.getBuffer();
	
	int wb = in.getWidthBits();
	
	const float kernelSum = kc + 2.f*(kex + key) + 4.f*kk;
	
	
	j = 0;	// top row
	{
		// row ptrs
		pr2 = (pIn + (j << wb));
		pr3 = (pIn + ((j + 1) << wb));
		prOut = (pOut + (j << wb) );
		
		i = 0; // top left corner
		{
			f = kex * (pr2[i+1]);
			f += key * (pr3[i]);
			f += kk * (pr3[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		for(i = 1; i < width - 1; i++) // top side
		{
			f = kex * (pr2[i-1] + pr2[i+1]);
			f += key * (pr3[i]);
			f += kk * (pr3[i-1] + pr3[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		i = width - 1; // top right corner
		{
			f = kex * (pr2[i-1]);
			f += key * (pr3[i]);
			f += kk * (pr3[i-1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
	}
	for(j = 1; j < height - 1; j++) // center rows
	{
		// row ptrs
		pr1 = (pIn + ((j - 1) << wb));
		pr2 = (pIn + (j << wb));
		pr3 = (pIn + ((j + 1) << wb));
		prOut = (pOut + (j << wb));
		
		i = 0; // left side
		{
			f = kex * (pr2[i+1]);
			f += key * (pr1[i] + pr3[i]);
			f += kk * (pr1[i+1] + pr3[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		for(i = 1; i < width - 1; i++) // center
		{
			f = kex * (pr2[i-1] + pr2[i+1]);
			f += key * (pr1[i] + pr3[i]);
			f += kk * (pr1[i-1] + pr1[i+1] + pr3[i-1] + pr3[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		i = width - 1; // right side
		{
			f = kex * (pr2[i-1]);
			f += key * (pr1[i] + pr3[i]);
			f += kk * (pr1[i-1] + pr3[i-1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
	}
	j = height - 1;	// bottom row
	{
		// row ptrs
		pr1 = (pIn + ((j - 1) << wb));
		pr2 = (pIn + (j << wb));
		prOut = (pOut + (j << wb));
		
		i = 0; // bottom left corner
		{
			f = kex * (pr2[i+1]);
			f += key * (pr1[i]);
			f += kk * (pr1[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		for(i = 1; i < width - 1; i++) // bottom side
		{
			f = kex * (pr2[i-1] + pr2[i+1]);
			f += key * (pr1[i]);
			f += kk * (pr1[i-1] + pr1[i+1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
		
		i = width - 1; // bottom right corner
		{
			f = kex * (pr2[i-1]);
			f += key * (pr1[i]);
			f += kk * (pr1[i-1]);
			f += kc * pr2[i];
			prOut[i] = f/kernelSum;		
		}
	}
	return out;
}


			
// --------------------------------------------------------------------------------

#pragma mark process

// 
// TODO take pointer to input / output arrays. 

// also pass max # of touches to output. 

void TouchTracker::process(int)
{	
	if (!mpIn) return;
	MLSignal& in(*mpIn);
	
	
	// IIR filter input
	float k = 0.25f;
	mFilteredInput = in;
	mFilteredInput.scale(k);	
	mInput1.scale(1.0 - k);
	mFilteredInput.add(mInput1);
	mInput1 = mFilteredInput;
	
	// make calibrated signal available to viewers
	{
		std::lock_guard<std::mutex> lock(mCalibratedSignalMutex);
		mCalibratedSignal = mFilteredInput;
	}
	
	// filter out any negative values. negative values can show up from capacitive coupling near edges,
	// from motion or bending of the whole instrument, 
	// from the elastic layer deforming and pushing up on the sensors near a touch. 
	mFilteredInput.sigMax(0.f);
	{	
		// convolve input with 3x3 smoothing kernel.
		// a lot of filtering is needed here for Soundplane A to make sure peaks are in centers of touches.
		// it also reduces noise. 
		// the down side is, contiguous touches are harder to tell apart. a smart blob-shape algorithm
		// can make up for this later, with this filtering still intact.
		
		const float kPostSmoothScale = 64.f;
		mFilteredInput.scale(kPostSmoothScale);
		
		{
			std::lock_guard<std::mutex> lock(mSmoothedSignalMutex);
			mSmoothedSignal = (mFilteredInput);

			mSmoothedSignal = smoothPressureX(mSmoothedSignal);
			mSmoothedSignal = smoothPressureX(mSmoothedSignal);
			mSmoothedSignal = smoothPressureX(mSmoothedSignal);
			mSmoothedSignal = smoothPressureX(mSmoothedSignal);

			mSmoothedSignal = smoothPressureY(mSmoothedSignal);
			mSmoothedSignal = smoothPressureY(mSmoothedSignal);
			mSmoothedSignal = smoothPressureY(mSmoothedSignal);
			
		}
		
		// make smoothed signals available
		{
			std::lock_guard<std::mutex> lock(mCurvatureSignalXMutex);
			mCurvatureSignalX = getCurvatureX(mSmoothedSignal);
		}
		
		{
			std::lock_guard<std::mutex> lock(mCurvatureSignalYMutex);
			mCurvatureSignalY = getCurvatureY(mSmoothedSignal);
		}
		
		{
			std::lock_guard<std::mutex> lock(mCurvatureSignalMutex);
			mCurvatureSignal = mCurvatureSignalX;
			mCurvatureSignal.multiply(mCurvatureSignalY);
			mCurvatureSignal.sqrt();
		}		
		
		if(mMaxTouchesPerFrame > 0)
		{
			mTouchesRaw = findTouches(mCurvatureSignalX, mCurvatureSignalY, mCurvatureSignal);

			mTouches = (mTouchesRaw);
						
			// sort touches by z. 
			std::sort(mTouches.begin(), mTouches.begin() + kMaxTouches, [](Vec4 a, Vec4 b){ return a.z() > b.z(); } );
			
			// match -> position filter -> fixed z filter -> feedback
			mTouches = matchTouches(mTouches, mTouchesMatch1);	
			mTouches = filterTouchesXYAdaptive(mTouches, mTouchesMatch1);
			mTouchesMatch1 = mTouches;
			
			// asymmetrical z filter from user setting. Ages are created here.
			mTouches = filterTouchesZ(mTouches, mTouches2, mLopassZ*2.f, mLopassZ*0.25f);
			mTouches2 = mTouches;
			
			// after variable filter, exile decayed touches so they are not matched. Note this affects match feedback!
			mTouchesMatch1 = exileUnusedTouches(mTouchesMatch1, mTouches);
									
			//	TODO hysteresis after matching to prevent glitching when there are more
			// physical touches than mMaxTouchesPerFrame and touches are stolen
//			mTouches = sortTouchesWithHysteresis(mTouches, mTouchSortOrder);			
//			mTouches = limitNumberOfTouches(mTouches);
			
			/*
			if(mRotate)
			{
				mTouches = rotateTouches(mTouches);
			}	
			*/
			
			mTouches = clampTouches(mTouches);
			
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

MLSignal TouchTracker::getCurvatureX(const MLSignal& in)
{
	const int w = kSensorCols;
	const int h = kSensorRows;
	
	// TODO no dynamic signals MLTEST
	MLSignal cx(w, h);
	
	// rows
	for(int j=0; j<h; ++j)
	{
		float z = 0.f;
		float zm1 = 0.f;
		float dz = 0.f;
		float dzm1 = 0.f;
		float ddz = 0.f;
		
		for(int i=0; i <= w; ++i)
		{
			if(within(i, 0, w))
			{
				z = in(i, j);
			}
			else
			{
				z = 0.f;
			}
			dz = z - zm1;
			ddz = dz - dzm1;					
			zm1 = z;
			dzm1 = dz;
			
			if(i >= 1)
			{
				cx(i - 1, j) = clamp(-ddz, 0.f, 1.f);
			}
		}
	}	
		
	return cx;
}


MLSignal TouchTracker::getCurvatureY(const MLSignal& in)
{
	const int w = kSensorCols;
	const int h = kSensorRows;
	
	// TODO no dynamic signals MLTEST
	MLSignal cy(w, h);
	
	// cols
	for(int i=0; i<w; ++i)
	{
		float z = 0.f;
		float zm1 = 0.f;
		float dz = 0.f;
		float dzm1 = 0.f;
		float ddz = 0.f;
		for(int j=0; j <= h; ++j)
		{
			if(within(j, 0, h))
			{
				z = in(i, j);
			}
			else
			{
				z = 0.f;
			}
			
			dz = z - zm1;
			ddz = dz - dzm1;					
			zm1 = z;
			dzm1 = dz;
			if(j >= 1)
			{
				cy(i, j - 1) = clamp(-ddz, 0.f, 1.f);
			}
		}
	}

	return cy;
}

class compareZ 
{
public:
	bool operator() (const Vec4& a, const Vec4& b) 
	{ 
		return (a.z() > b.z());
	}
};

Vec4 correctPeakX(Vec4 pos, const MLSignal& in) 
{		
	Vec4 newPos = pos;
	const float maxCorrect = 0.5f;
	const int w = kSensorCols;
	int x = pos.x();
	int y = pos.y();
	
	if(within(x, 1, w - 1))
	{
		float a = in(x - 1, y);
		float b = in(x, y);
		float c = in(x + 1, y);
		float p = ((a - c)/(a - 2.f*b + c))*0.5f;
		float fx = x + clamp(p, -maxCorrect, maxCorrect);										
		newPos = Vec4(fx, pos.y(), pos.z(), 0.f);
	}
	
	return newPos;
}

Vec4 correctPeakY(Vec4 pos, const MLSignal& in) 
{		
	Vec4 newPos = pos;
	const float maxCorrect = 0.5f;
	const int h = kSensorRows;
	int x = pos.x();
	int y = pos.y();
	
	float a, b, c;
	if(y <= 0)
	{
		a = 0;
		b = in(x, y);
		c = in(x, y + 1);
	}
	else if(y >= h - 1)
	{
		a = in(x, y - 1);
		b = in(x, y);
		c = 0;
	}
	else
	{
		a = in(x, y - 1);
		b = in(x, y);
		c = in(x, y + 1);
	}
	float p = ((a - c)/(a - 2.f*b + c))*0.5f;		
	float fy = y + clamp(p, -maxCorrect, maxCorrect);							
	return Vec4(pos.x(), fy, pos.z(), 0.f);
}


Vec4 peakToTouch(Vec4 p)
{
	MLRange sensorToKeyX(3.5f, 59.5f, 1.f, 29.f);
	
	return Vec4(sensorToKeyX(p.x()), sensorToKeyY(p.y()), p.z(), 0.f);
}

// quick touch finder based on peaks of curvature. 
// this works well, but a different approach based on blob sizes could do a much better
// job with contiguous keys.
std::array<Vec4, TouchTracker::kMaxTouches> TouchTracker::findTouches(const MLSignal& inX, const MLSignal& inY, const MLSignal& inXY)
{
	const int w = kSensorCols;
	const int h = kSensorRows;	
	int i, j;
	
	// TODO preallocate
	std::array<Vec4, kMaxPeaks> peaks;
	std::array<Vec4, kMaxTouches> touches;
	std::array<std::bitset<kSensorCols>, kSensorRows> map;

	const MLSignal& in = inXY;
		
	// get peaks
	float f11, f12, f13;
	float f21, f22, f23;
	float f31, f32, f33;
	
	j = 0;
	{
		std::bitset<kSensorCols>& row = map[j];
		i = 0;
		{
			
		}
		for(i = 1; i < w - 1; ++i)
		{
			f21 = in(i - 1, j    ); f22 = in(i, j    ); f23 = in(i + 1, j    );
			f31 = in(i - 1, j + 1); f32 = in(i, j + 1); f33 = in(i + 1, j + 1);
			row[i] = (f22 > f21) && (f22 > mFilterThreshold) && (f22 > f23)
				&& (f22 > f31) && (f22 > f32) && (f22 > f33);
		}
		i = w - 1;
		{
			
		}
	}
	
	
	for (j = 1; j < h - 1; ++j)
	{
		std::bitset<kSensorCols>& row = map[j];
		i = 0;
		{
			
		}
		for(i = 1; i < w - 1; ++i)
		{
			f11 = in(i - 1, j - 1); f12 = in(i, j - 1); f13 = in(i + 1, j - 1);
			f21 = in(i - 1, j    ); f22 = in(i, j    ); f23 = in(i + 1, j    );
			f31 = in(i - 1, j + 1); f32 = in(i, j + 1); f33 = in(i + 1, j + 1);
			row[i] = (f22 > f11) && (f22 > f12) && (f22 > f13)
				&& (f22 > f21) && (f22 > mFilterThreshold) && (f22 > f23)
				&& (f22 > f31) && (f22 > f32) && (f22 > f33);
		}	
		i = w - 1;
		{
			
		}
	}
	
	j = h - 1;
	{
		std::bitset<kSensorCols>& row = map[j];
		i = 0;
		{
			
		}
		for(i = 1; i < w - 1; ++i)
		{
			f11 = in(i - 1, j - 1); f12 = in(i, j - 1); f13 = in(i + 1, j - 1);
			f21 = in(i - 1, j    ); f22 = in(i, j    ); f23 = in(i + 1, j    );
			row[i] = (f22 > f11) && (f22 > f12) && (f22 > f13)
				&& (f22 > f21) && (f22 > mFilterThreshold) && (f22 > f23);
		}
		i = w - 1;
		{
			
		}
	}
	
			
	// gather all peaks.
	int nPeaks = 0;
	for (int j=0; j<h; j++)
	{
		std::bitset<kSensorCols>& mapRow = map[j];
		for (int i=0; i < mWidth; i++)
		{
			// if peak
			if (mapRow[i])
			{
				float z = in(i, j);
				peaks[nPeaks++] = Vec4(i, j, z, 0.f);
				if (nPeaks >= kMaxPeaks) break;
			}
		}
	}
	
	if(nPeaks > 1)
	{
		std::sort(peaks.begin(), peaks.begin() + nPeaks, compareZ());
	}
	
	// correct and clip
	
	// WAT -- linker error if kMaxTouches used directly
	int q = kMaxTouches;	
	int nTouches = std::min(nPeaks, q);
	
	/*
	switch(nPeaks)
	{
		case 1:
			debug() << ".";
			break;
		case 2:
			debug() << ":";
			break;
		case 3:
			debug() << "-";
			break;
		case 4:
			debug() << "+";
			break;
			
		default:
			if(nPeaks > 4)
			debug() << "*";
			break;		
	}
	*/
	
	for(int i=0; i<nTouches; ++i)
	{
		Vec4 p = peaks[i];
		if(p.z() < mFilterThreshold) break;
				
		Vec4 px = correctPeakX(p, inXY);
		
		Vec4 pxy = correctPeakY(px, inXY);
		
		touches[i] = peakToTouch(pxy);	
		
		
		if(i == 0)
		{
//			debug() << "y: " << p.y() << " -> " << pxy.y() << "\n";
		}
	}
	
	return touches;
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

// TODO first touch below filter threshold(?) is on one index, then active touch switches index?! 

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
	for(int i=0; i<mMaxTouchesPerFrame; ++i)
	{
		float minDist = MAXFLOAT;
		Vec4 prev = x1[i];
		
		for(int j=0; j < mMaxTouchesPerFrame; ++j)
		{
			Vec4 curr = x[j];
			if((curr.z() > mFilterThreshold) && (prev.z() > mFilterThreshold))
			{			
				float distToCurrentTouch = cityBlockDistanceXYZ(prev, curr);
				if(distToCurrentTouch < minDist)
				{
					forwardMatchIdx[i] = j;
					minDist = distToCurrentTouch;						
				}
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
			if((curr.z() > mFilterThreshold) && (prev.z() > mFilterThreshold))
			{			
				float distToPreviousTouch = cityBlockDistanceXYZ(prev, curr);
				if(distToPreviousTouch < minDist)
				{
					reverseMatchIdx[i] = j;
					minDist = distToPreviousTouch;						
				}
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
	//		if((curr.z() > mFilterThreshold) && (prev.z() > mFilterThreshold))
			{			
				// touch is continued, mark as connected and write to new touches
				curr.setW(cityBlockDistance(prev, curr) < kMaxConnectDist);						
				newTouches[j] = curr;
				currWrittenToNew[i] = true;		
			}
		}
	}
		
	// now take care of any remaining nonzero current touches
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
	
	// these filter settings have a big and sort of delicate impact on play feel, so they are not user settable at this point
	const float kFixedXYFreqMax = 20.f;
	const float kFixedXYFreqMin = 1.f;
	MLRange zToXYFreq(0., 0.02, kFixedXYFreqMin, kFixedXYFreqMax); 
	
	std::array<Vec4, TouchTracker::kMaxTouches> out;
	
	for(int i=0; i<mMaxTouchesPerFrame; ++i)
	{
		float x = in[i].x();
		float y = in[i].y();
		float z = in[i].z();
		float w = in[i].w(); 
		
		float x1 = inz1[i].x();
		float y1 = inz1[i].y();
		
		// filter, or not, based on w from matchTouches
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
		
		// filter z fixed for note-on velocity
		
		
		// filter z variable
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
		float newZ = (clamp(t.z() - mOnThreshold, 0.f, 2.f));
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
