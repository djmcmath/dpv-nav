Welcome to the DPV-Nav toolkit (codename "Tern").

Nominally, this is a simple inertial navigation system designed to be installed on DPVs (aka "scooters").  I'm making this open source, and attempting to use as many readily-available parts as possible to provide something that'll be accessible to most technical divers.  Some parts of the build are technical -- you'll need to manufacture and solder custom PCBs, flash firmware to ESP32s, and so on, but none of it is too out of reach.

About the name:
- It's called Tern, after the Arctic Tern, which is just a fantastic bird.  They navigate from the north pole to the south every year -- mating in the Antarctic, then raising their young in transit and growing to maturity in the Arctic.  Then they migrate BACK to do it all again the next year.  Insane.
- But hey, if a tiny bird can navigate thousands of kilometers every year, perhaps we can build a DIY-capable device that'll allow divers to cover much smaller distances with confidence underwater.

Framing:
- This is NOT "underwater GPS."  GPS doesn't work underwater, and there's no magic that will allow us to violate the laws of physics.  Precise positioning underwater is a delightful engineering problem, and there are a variety of excellent ways to know exactly where you are.  Precision is a function of expense -- how precise your position is depends almost entirely on how much you're willing to spend to get there.
- This *is* a tool that'll tell you much more accurately where you are than anything else readily available in the space, especially at this price point.  If your current process is to read your 2in magnetic compass and keep track of time and distance using your dive computer and mental gymnastics, this will save you a lot of effort and produce a dramatically more accurate result.
- The process is called "dead reckoning," basically using heading and speed (plus an initial position from GPS on the surface) to determine an "estimated position."  Accuracy of the final position depends on quality of calibration, accuracy of initial position, and time / distance since last fix.  Each iteration takes the previous estimated position, adds the current course and speed, and produces a new estimated position.  Errors, naturally, compound, so the longer it's been since you've actually known where you were, the farther off you may be from where it says you are.
- For planning purposes, assume that the error is about 10% of the distance since your last fix.  So if you submerge and run for 500m, assume you're within 50m of the target.  That'll require dropping a marker and running a reel out to conduct a search to find the wreck.  But at least you have some confidence that you're within a reasonable distance of the right spot.
- While navigating, use depth contours to sanity-check your position.  For example, if you're headed out to the MV Scout, which is at ~76ft deep and 800m from shore, but you find yourself hitting 80ft after only 6 or 8min of run time, it means you've erred to the north.  If you're headed to the Not MT-6, and you find yourself at 250ft with no sign of the wreck, don't just keep going deeper because NAV tells you to.  Don't blindly follow the navigator -- it's giving you an estimated position, not executing your dive plan.

Workflow and usage:
- Assemble the whole device
- Flash the firmware to both units (NAV and DISPLAY)
- On the first power-up, the unit will attempt to calibrate gyro and accelerometer.  There will be a series of prompts displayed in the serial monitor walking you through this process.  
- Set up wireless (optional):
-- You can keep it as something that you connect to with a wireless device, or you can set it to automatically connect to a known wireless network.  Calibration is slightly easier if it autoconnects, but it's not required.
-- Make sure wireless is on ("WiFi" in white, not gray, on the display)
-- Connect to wireless AP "Tern," then browse to 192.168.4.1 or tern.local.
-- Enter your wireless network information in the appropriate part of the page.  It'll autoconnect to that network at next boot, if it's available.  Then it should appear as "tern.local" on your network.
- Perform a baseline calibration.  This requires broad coverage of a "sphere" of directions to provide a baseline for what the magnetic environment of the board and housing look like.  This should happen while the unit is NOT mounted on your scooter, so it should be easy to rotate through the necessary coverage.  The display will walk you through how you're doing as you're going.  
- Perform mounted calibration.  Once you've done the baseline cal and uploaded the python-processed json, perform a mounted cal.  We've got a baseline, so we know what the closest parts of the environment look like, now we're figuring out what it looks like with the scooter there.  Similar to the baseline cal, you'll get a grid on screen showing coverage, and you'll need to process the data offline and upload a json.
- Optionally: final heading corrections aka 4-point cal.  In some cases, it may be beneficial to provide 4 actual headings to correct small (<5deg) errors.  For example, maybe the IMU is mounted slightly off-axis, so the DPV is aligned to travel in a slightly different direction than the IMU is pointed.  This should not be used for broad-swath calibration or correcting major errors.  If you're getting 15-20 degree errors between indicated and actual at this point, you've done something wrong and need to start over.
- Perform a speed cal.

On a Dive:
- Plug in the battery, close the housing, make sure the unit boots cleanly
- Let it get a GPS fix on the beach.  GPS doesn't like water at all, so powering everything on and getting this first fix at your car is probably better than doing it while the scooter is at the water's edge.
- Use the web page to set a waypoint (e.g. the wreck you're trying to get to)
- Once GPS has a solid fix (4 bars) and you've set the waypoint, you can go ahead and put the unit in standby (menu "OFF").  This conserves power, but keeps GPS soft-powered, so it'll reconnect within a few seconds of getting adequate signal.  When you're ready to use it again, press and hold both buttons for about a second (much like a Shearwater).
- Use the menu to set navigation mode to be "outbound," e.g. to the waypoint.
- You'll get bearing and range to the waypoint.  (Future dev will include cross-track error and ETA)
- Some tips:
-- Get a fix as close to the target as possible.  e.g. if you can surface transit for a bit, make sure to get 4-bar GPS fix before submering.  
-- Long straight runs at constant speed probably yield best results (need to test to confirm this).  If you're doing a lot of big circles, you're probably losing a little accuracy every time you do it.
- When you arrive at the point where the navigator thinks you're at the wreck, you're probably close, but not there.  In Lake Washington, "close" is rarely the same as "close enough."  Plan on putting down a marker (weight, stake in the ground, etc) and running out a reel.  For planning purposes, you're probably within 10% of the total run length (e.g. if it's been 500m since the last fix, total error is probably <50m).