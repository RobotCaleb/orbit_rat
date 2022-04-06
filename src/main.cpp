#include <Arduino.h>
#include <Keyboard.h>
#include <Bounce2.h>

constexpr bool calibrate_on_startup = true; // record startup reading from sticks as center value?
constexpr bool send_joystick_hid = false; // (also) send HID joystick records for the sticks and buttons?
constexpr int n_axes = 4; // number of axes we're going to sample.
constexpr int n_buttons = 2; // number of buttons
constexpr int pan_speed = -25; // max speed of pan motion (first stick). Negative to invert motion.
constexpr int orbit_speed = -10; // max speed of orbit motion (second stick)
constexpr float deadzone = 0.02; // absolute normalized axis value must be above this to be considered active
constexpr int max_unwind_step = 100; // how many pixels per HID report to move the mouse during unwinding
constexpr int button_debounce_interval = 100; 

// which mouse buttons are held down during each stick's motion?
constexpr bool stick_active_buttons[][3] = {
  {false, true, false},
  {false, true, false}
};

// which keyboard key is held down during each stick's motion? 0 for no keys.
constexpr int stick_active_key[] = {
  0,
  KEY_LEFT_SHIFT
};

// Which pin goes to which axis?
// pan stick horizontal, pan stick vertical, orbit stick horizontal, orbit stick vertical
constexpr int axisPins[] = {
  1, 0, 8, 7
};

// which pins are stick-press buttons attached to?
// this is currently unused.
constexpr int buttonPins[] = {
  2, 9
};

// calibration: low value, center, high value.
// note that this is intentionally not `constexpr` to permit calibration.
float axisExtents[][3] = {
  {3, 520, 1021},
  {6, 498, 1019},
  {3, 530, 1021},
  {2, 513, 1022}
};

// raw state sampled from analog inputs
int axisValues[n_axes] = {0, 0, 0, 0};

// button debouncers
Bounce buttons[n_buttons] = {Bounce(), Bounce()};

bool buttonState[n_buttons] = {false, false};

// axis values in the range (-1, 1)
float normalizedAxes[n_axes] = {0, 0, 0, 0};

// read the analog sticks.
void readSticks() {
  for (int i = 0; i < n_axes; i++) {
    axisValues[i] = analogRead(axisPins[i]);
  }
}

// normalize the value within the half space.
float normalize(float low, float val, float high) {
  auto range = high - low;
  return (val - low) / range;
}

// convert the normalized stick values to the 0-1023 values for the joystick system.
unsigned int to_joy(float val) {
  return (unsigned int) ((val + 1.f) / 2.f * 1023);
}

// Normalize the raw values read from the analog sticks.
void normalizeSticks() {
  for (int i = 0; i < n_axes; i++) {
    if (axisValues[i] < axisExtents[i][1]) {
      normalizedAxes[i] = -(1 - normalize(axisExtents[i][0], axisValues[i], axisExtents[i][1]));
    }
    else {
      normalizedAxes[i] = normalize(axisExtents[i][1], axisValues[i], axisExtents[i][2]);
    }
    normalizedAxes[i] = -normalizedAxes[i]; // invert all channels to get normalized motion.
  }
}

// Do arduino setup.
void setup() {
  Serial.begin(38400);

  for (int i = 0; i < n_buttons; i++) {
    buttons[i].attach(buttonPins[i], INPUT_PULLUP);
    buttons[i].interval(button_debounce_interval);
  }

  if /*constexpr*/ (calibrate_on_startup) {
    // calibrate stick centers.
    readSticks();
    for (int i = 0; i < n_axes; i++) {
      axisExtents[i][1] = axisValues[i];
    }
  }
}

// Send joystick updates.
void sendJoystick() {
  Joystick.X(to_joy(normalizedAxes[0]));
  Joystick.Y(to_joy(normalizedAxes[1]));
  Joystick.sliderLeft(to_joy(normalizedAxes[3]));
  Joystick.Zrotate(to_joy(normalizedAxes[2]));
}

// stores how much we've offset the mouse cursor during a motion
int unwindAccumulator[2];

// are we currently in a move?
bool inMove = false;

// When we unwind the mouse motion, find the largest step we can move
// to reduce the given accumulator. This is necessary because the
// teensy HID system only spec's mouse moves between -127 and 127.
int max_step(int const& accum) {
  if (accum > 0) {
    return -min(accum, max_unwind_step);
  }
  else if (accum < 0) {
    return min(max_unwind_step, -accum);
  }

  return 0;
}

// Move the mouse back to its start point.
void doUnwind() {
  while (unwindAccumulator[0] != 0 || unwindAccumulator[1] != 0) {
    int xMove = max_step(unwindAccumulator[0]);
    int yMove = max_step(unwindAccumulator[1]);

    Mouse.move(xMove, yMove);
    unwindAccumulator[0] += xMove;
    unwindAccumulator[1] += yMove;
  }
}

// tracks which stick started a motion
int motionStartIndex = -1;

// Checks whether the given stick (represented by axes startIndex and startIndex+1) is
// in its deadzone.
bool checkDeadzone(int startIndex) {
  return abs(normalizedAxes[startIndex]) < deadzone && abs(normalizedAxes[startIndex + 1]) < deadzone;
}

void setMouseButtons(int startIndex) {
  auto stickIndex = startIndex >> 1;
  Mouse.set_buttons(stick_active_buttons[stickIndex][0], stick_active_buttons[stickIndex][1], stick_active_buttons[stickIndex][2]);
}

void setKeys(int startIndex, bool press) {
  auto stickIndex = startIndex >> 1;
  auto key = stick_active_key[stickIndex];
  
  if (!key) return;

  if (press) {
    Keyboard.press(key);
  }
  else {
    Keyboard.release(key);
  }
}

// Update mouse motion state and send HID reports.
void sendMouse() {

  // are we in a move that has now stopped?
  if (inMove && checkDeadzone(motionStartIndex)) {
    Mouse.set_buttons(0, 0, 0);
    setKeys(motionStartIndex, false);
    delay(10); // without this delay, the mouse button release may not be registered before the unwind...
    doUnwind(); // ...which would have the effect of undoing the pan/zoom we just completed instead of just resetting the cursor.
    inMove = false;
    return;    
  }

  // if we're not in a move, maybe we should start one?
  if (!inMove) {
    // are either of the sticks outside their deadzone?
    // btw, having this check inside the !inMove block
    // has the effect of "muting" any movement from the
    // other stick once one stick starts a move. 
    // We only listen to motionStartIndex stick until 
    // we start a new move.
    if (!checkDeadzone(0)) {
      motionStartIndex = 0;
    }
    else if (!checkDeadzone(2)) {
      motionStartIndex = 2;
    }
    else {
      // if everything is in deadzones, just stop.
      return;
    }

    // we're outside the deadzone, so start a new move.
    unwindAccumulator[0] = unwindAccumulator[1] = 0;
    inMove = true;
    setKeys(motionStartIndex, true);
    delay(10);
    setMouseButtons(motionStartIndex);
    delay(10); // without these delays, some programs don't register the shift or button press until after the motion has started.
  }

  // choose the speed based on which stick initiated the move.
  auto speed = motionStartIndex == 0 ? pan_speed : orbit_speed;

  int xMove = speed * normalizedAxes[motionStartIndex];
  int yMove = speed * normalizedAxes[motionStartIndex + 1];

  Mouse.move(xMove, yMove);

  unwindAccumulator[0] += xMove;
  unwindAccumulator[1] += yMove;
}

void updateButtons() {
  for (int i = 0; i < n_buttons; i++) {
    buttons[i].update();

    if (buttons[i].fell()) {
      buttonState[i] = true;
    }
    else if (buttons[i].rose() && buttonState[i]) {
      buttonState[i] = false;
    }
  }
}

void loop() {
  readSticks();
  normalizeSticks();

  if /*constexpr*/ (send_joystick_hid) { 
    sendJoystick();
  }

  sendMouse();

  delay(10); // approximately 100 updates a second. It's actually less because of delays elsewhere, but it's plenty fast for CAD or whatever.

  // temporarily uncomment this block to get values printed out to serial for calibration purposes.
  // Serial.print("axes ");
  // for (int i = 0; i < n_axes; i++) {
  //   Serial.print(axisValues[i]);
  //   Serial.print(",");
  // }
  // Serial.print("\n");
  // delay(80);
}