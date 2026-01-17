#line 1 "D:\\Documents\\dpv-nav\\firmware\\src\\math\\orientation.h"
#pragma once
#include "mahony.h"

struct Euler { float yaw, pitch, roll; }; // radians

Euler quatToEulerRad(const Quaternion& q);

// heading in degrees [0,360)
float headingDegFromYawRad(float yawRad, float declinationDeg = 0.0f);
