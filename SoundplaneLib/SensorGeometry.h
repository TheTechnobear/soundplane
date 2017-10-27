// geometry for Soundplane Model A.
// Copyright (c) 2013 Madrona Labs LLC. http://www.madronalabs.com
// Distributed under the MIT license: http://madrona-labs.mit-license.org/

#pragma once

namespace SensorGeometry
{
constexpr int width = 64;
constexpr int height = 8;
constexpr int elements = width*height;
};

typedef std::array<float, SensorGeometry::elements> SensorFrame;
