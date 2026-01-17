#line 1 "D:\\Documents\\dpv-nav\\firmware\\src\\sensors\\calib.cpp"
struct Calib3 {
  Vec3 bias;     // subtract
  Vec3 scale;    // multiply (1,1,1 if unused)
};

struct MagCalib {
  Vec3 bias;       // hard-iron offset
  float softIron[3][3]; // 3x3 (identity if unused)
};

Vec3 applyCalib(const Vec3& v, const Calib3& c) {
  return { (v.x - c.bias.x)*c.scale.x,
           (v.y - c.bias.y)*c.scale.y,
           (v.z - c.bias.z)*c.scale.z };
}

Vec3 applyMagCalib(const Vec3& v, const MagCalib& c) {
  Vec3 d{ v.x - c.bias.x, v.y - c.bias.y, v.z - c.bias.z };
  return {
    c.softIron[0][0]*d.x + c.softIron[0][1]*d.y + c.softIron[0][2]*d.z,
    c.softIron[1][0]*d.x + c.softIron[1][1]*d.y + c.softIron[1][2]*d.z,
    c.softIron[2][0]*d.x + c.softIron[2][1]*d.y + c.softIron[2][2]*d.z
  };
}
