#define main baseline_suite
#include "firmware_test.cpp"
#undef main

static void manual_step_raw(int ch1, int ch2, int ch6=1811) {
  clock_ms += 10;
  radio(ch1, ch2, 1811, ch6, 172, 172);
  ikaLoop();
}

int main(){
  // Preserve servo jitter filtering / expanded tilt limit from titreme fix.
  ServoInputFilter f;
  for(int i=0;i<100;i++) assert(f.update(i%2?60:-60,70,45)==0);
  assert(TILT_MIN_US==1050 && TILT_MAX_US==1900 && TILT_CENTER_US==1500);

  // Actual firmware: arm manual with centered sticks first.
  ikaSetup();
  for(int i=0;i<200;i++) step();
  assert(g_motor_armed);

  // Pre-titreme classic differential mix: forward + right turn => unequal sides.
  manual_step_raw(1811,1811,1811);
  assert(g_applied_left != g_applied_right);
  assert(g_applied_left > 0);
  assert(g_applied_right >= 0);

  // Pure steering produces opposite directions, scaled by TURN_GAIN_PCT.
  manual_step_raw(1811,992,1811);
  assert(g_applied_left == -g_applied_right);
  assert(g_applied_left > 0);

  // No V2 acceleration ramp: full forward at high gear is applied immediately.
  manual_step_raw(992,1811,1811);
  assert(g_applied_left==g_applied_right && g_applied_left>950);

  // Failsafe still stops immediately.
  for(int i=0;i<31;i++) step(false);
  assert(!g_motor_armed && g_applied_left==0 && g_applied_right==0);

  std::cout << "PASS: pre-titreme classic differential drive + retained servo fixes/failsafe\n";
  return 0;
}
