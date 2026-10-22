
set pagination off
thread apply all bt
print cpu_state.pc
print cpu_state.oldpc
print cs
print cpu_state.flags
print monitors[0].mon_scrnsz_x
print monitors[0].mon_scrnsz_y
set monitors[0].mon_screenshots_raw = 1
print (int)monitors[0].mon_screenshots_raw
print "--- CPU State ---"
p cpu_state
quit
