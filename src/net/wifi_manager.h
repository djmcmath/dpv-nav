#pragma once

#include <IPAddress.h>

namespace wifi {

void init();
void stop();
void update();
bool isAP();
IPAddress ip();

}  // namespace wifi
