//#include "calib.h"
#include "imu.h"
#include "../types/types.h"

imu::Vec3f applyCalib(const imu::Vec3f& v, const Calib3& c) {
  return { (v.x - c.bias.x)*c.scale.x,
           (v.y - c.bias.y)*c.scale.y,
           (v.z - c.bias.z)*c.scale.z };
}

imu::Vec3f applyMagCalib(const imu::Vec3f& v, const MagCalib& c) {
  imu::Vec3f d{ v.x - c.bias.x, v.y - c.bias.y, v.z - c.bias.z };
  return {
    c.softIron[0][0]*d.x + c.softIron[0][1]*d.y + c.softIron[0][2]*d.z,
    c.softIron[1][0]*d.x + c.softIron[1][1]*d.y + c.softIron[1][2]*d.z,
    c.softIron[2][0]*d.x + c.softIron[2][1]*d.y + c.softIron[2][2]*d.z
  };
}
