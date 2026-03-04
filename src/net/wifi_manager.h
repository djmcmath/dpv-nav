#pragma once

#include <IPAddress.h>

namespace wifi {

void init();
void update();
bool isAP();
IPAddress ip();

}  // namespace wifi
