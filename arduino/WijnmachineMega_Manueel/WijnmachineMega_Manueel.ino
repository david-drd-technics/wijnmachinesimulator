#include <SPI.h>
#include <MFRC522.h>
#include <EEPROM.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// =====================================================
// CONFIGURATIE
// =====================================================
#define SS_PIN 53
#define RST_PIN 14

const byte NUM_BUTTONS = 3;
const byte NUM_RESETS  = 15;
const byte NUM_OUTPUTS = 15;
const byte NUM_MOTORS  = 3;

const byte buttonPins[NUM_BUTTONS] = {44, 46, 48};
const byte resetPins[NUM_RESETS] = {
  22, 23, 24, 25, 26,
  27, 28, 29, 30, 31,
  32, 33, 34, 35, 36
};

const byte outputPins[NUM_OUTPUTS] = {
  54, 55, 56, 57, 58,
  59, 60, 61, 62, 63,
  64, 65, 66, 67, 68
};

const byte ledPins[NUM_BUTTONS][3] = {
  {37, 39, 41},
  {43, 45, 47},
  {38, 40, 42}
};

const unsigned long DEBOUNCE_MS = 35;
const unsigned long HOMING_TIMEOUT_MS = 15000UL;
const unsigned long HOMING_MAX_STEPS = 40000UL;
const unsigned long DEFAULT_STEP_INTERVAL_US = 1200UL;
const unsigned long MIN_STEP_INTERVAL_US = 100UL;
const unsigned long MAX_STEP_INTERVAL_US = 50000UL;
const unsigned long STEP_PULSE_US = 8UL;

const byte SERIAL_BUFFER_SIZE = 120;
char serialBuffer[SERIAL_BUFFER_SIZE];
byte serialLength = 0;
bool serialOverflow = false;

const int EEPROM_MAGIC_ADDRESS = 32;
const byte EEPROM_MAGIC = 0xA7;

// =====================================================
// OBJECTEN EN STATUS
// =====================================================
MFRC522 mfrc522(SS_PIN, RST_PIN);

bool simulationMode = false;
byte counters[NUM_RESETS] = {0};
bool outputStates[NUM_OUTPUTS] = {false};

bool simButtons[NUM_BUTTONS] = {false};
bool simResets[NUM_RESETS] = {false};
bool simHomeSensors[NUM_MOTORS] = {false};

struct DebouncedInput {
  bool rawState;
  bool stableState;
  unsigned long lastChange;
};

DebouncedInput buttons[NUM_BUTTONS];
DebouncedInput resets[NUM_RESETS];

enum MotorMode {
  MOTOR_IDLE = 0,
  MOTOR_MOVE = 1,
  MOTOR_HOME = 2
};

struct Motor {
  byte dirPin;
  byte stepPin;
  byte enablePin;
  byte homePin;
  bool homeDirection;

  bool moving;
  bool direction;
  bool pulseHigh;
  bool simulatedRun;
  bool homed;
  MotorMode mode;

  unsigned long stepIntervalUs;
  unsigned long lastStepUs;
  unsigned long startedAtMs;
  unsigned long stepsTaken;
  unsigned long targetSteps;
};

Motor motors[NUM_MOTORS] = {
  // Motor 1 = transportband
  {11, 12, 10, 4, LOW,  false, LOW, false, false, false, MOTOR_IDLE, 1000, 0, 0, 0, 0},
  // Motor 2 = deur
  {7, 3, 13, 6, LOW,   false, LOW, false, false, false, MOTOR_IDLE, 1200, 0, 0, 0, 0},
  // Motor 3 = lift
  {2, 9, 8, 5, HIGH,   false, LOW, false, false, false, MOTOR_IDLE, 2400, 0, 0, 0, 0}
};

// =====================================================
// JSON / UITVOER
// =====================================================
void printJsonString(const char* value) {
  Serial.print('"');
  while (*value) {
    unsigned char c = (unsigned char)*value++;
    switch (c) {
      case '"': Serial.print(F("\\\"")); break;
      case '\\': Serial.print(F("\\\\")); break;
      case '\n': Serial.print(F("\\n")); break;
      case '\r': Serial.print(F("\\r")); break;
      case '\t': Serial.print(F("\\t")); break;
      default:
        if (c < 32) Serial.print('?');
        else Serial.write(c);
        break;
    }
  }
  Serial.print('"');
}

void sendEvent(const char* event, const char* key, const char* value) {
  Serial.print(F("{\"event\":"));
  printJsonString(event);
  Serial.print(',');
  printJsonString(key);
  Serial.print(':');
  printJsonString(value);
  Serial.println('}');
}

void sendError(const char* code) {
  sendEvent("error", "code", code);
}

void sendInfo(const char* key, const char* value) {
  sendEvent("info", key, value);
}

void sendSimulationStatus() {
  sendEvent("simulation", "status", simulationMode ? "enabled" : "disabled");
}

void sendButtonEvent(byte index, bool pressed) {
  char key[12];
  snprintf(key, sizeof(key), "button%u", index + 1);
  sendEvent("button", key, pressed ? "pressed" : "released");
}

void sendResetEvent(byte index) {
  char value[6];
  snprintf(value, sizeof(value), "%u", index + 1);
  sendEvent("reset", "resetPin", value);
}

void sendCounterEvent(byte index) {
  Serial.print(F("{\"event\":\"counter\",\"fles\":\"fles"));
  Serial.print(index + 1);
  Serial.print(F("\",\"waarde\":"));
  Serial.print(counters[index]);
  Serial.println('}');
}

void sendMotorStatus(byte motorId, const char* status) {
  Serial.print(F("{\"event\":\"motor_status\",\"motor\":"));
  Serial.print(motorId);
  Serial.print(F(",\"status\":"));
  printJsonString(status);
  Serial.println('}');
}

void sendMotorMoveEvent(byte motorId, bool direction, unsigned long steps, unsigned long intervalUs, const char* source) {
  Serial.print(F("{\"event\":\"motor_move\",\"motor\":"));
  Serial.print(motorId);
  Serial.print(F(",\"direction\":"));
  printJsonString(direction ? "REV" : "FWD");
  Serial.print(F(",\"steps\":"));
  Serial.print(steps);
  Serial.print(F(",\"interval_us\":"));
  Serial.print(intervalUs);
  Serial.print(F(",\"source\":"));
  printJsonString(source);
  Serial.println('}');
}

void sendOutputEvent(byte index) {
  Serial.print(F("{\"event\":\"output\",\"output\":"));
  Serial.print(index + 1);
  Serial.print(F(",\"state\":"));
  printJsonString(outputStates[index] ? "ON" : "OFF");
  Serial.println('}');
}

void printCounters() {
  Serial.print(F("{\"event\":\"counters\",\"values\":["));
  for (byte i = 0; i < NUM_RESETS; i++) {
    if (i > 0) Serial.print(',');
    Serial.print(F("{\"fles\":\"fles"));
    Serial.print(i + 1);
    Serial.print(F("\",\"waarde\":"));
    Serial.print(counters[i]);
    Serial.print('}');
  }
  Serial.println(F("]}"));
}

void sendInputStatus() {
  Serial.print(F("{\"event\":\"input_status\",\"buttons\":["));
  for (byte i = 0; i < NUM_BUTTONS; i++) {
    if (i > 0) Serial.print(',');
    Serial.print(buttons[i].stableState ? 1 : 0);
  }
  Serial.print(F("],\"resets\":["));
  for (byte i = 0; i < NUM_RESETS; i++) {
    if (i > 0) Serial.print(',');
    Serial.print(resets[i].stableState ? 1 : 0);
  }
  Serial.println(F("]}"));
}

void sendOutputStatus() {
  Serial.print(F("{\"event\":\"output_status\",\"values\":["));
  for (byte i = 0; i < NUM_OUTPUTS; i++) {
    if (i > 0) Serial.print(',');
    Serial.print(outputStates[i] ? 1 : 0);
  }
  Serial.println(F("]}"));
}

void sendMotorSummary() {
  for (byte i = 0; i < NUM_MOTORS; i++) {
    if (motors[i].moving) {
      if (motors[i].mode == MOTOR_HOME) sendMotorStatus(i + 1, "moving_to_home");
      else sendMotorStatus(i + 1, "moving");
    } else if (motors[i].homed) {
      sendMotorStatus(i + 1, "home_position_reached");
    } else {
      sendMotorStatus(i + 1, "idle");
    }
  }
}

void sendFullStatus() {
  sendSimulationStatus();
  printCounters();
  sendInputStatus();
  sendOutputStatus();
  sendMotorSummary();
}

// =====================================================
// EEPROM
// =====================================================
void initializeEEPROM() {
  if (EEPROM.read(EEPROM_MAGIC_ADDRESS) != EEPROM_MAGIC) {
    for (byte i = 0; i < NUM_RESETS; i++) EEPROM.update(i, 0);
    EEPROM.update(EEPROM_MAGIC_ADDRESS, EEPROM_MAGIC);
  }
  for (byte i = 0; i < NUM_RESETS; i++) counters[i] = EEPROM.read(i);
}

void saveCounter(byte index) {
  if (index < NUM_RESETS) EEPROM.update(index, counters[index]);
}

void setCounter(byte index, byte value) {
  if (index >= NUM_RESETS) return;
  counters[index] = value;
  saveCounter(index);
  sendCounterEvent(index);
}

void resetCounter(byte index) {
  setCounter(index, 0);
}

// =====================================================
// INGANGEN
// =====================================================
bool readButton(byte index) {
  return simulationMode ? simButtons[index] : (digitalRead(buttonPins[index]) == LOW);
}

bool readReset(byte index) {
  return simulationMode ? simResets[index] : (digitalRead(resetPins[index]) == LOW);
}

bool readHomeSensor(byte index) {
  return simulationMode ? simHomeSensors[index] : (digitalRead(motors[index].homePin) == LOW);
}

bool updateDebounce(DebouncedInput &input, bool currentState) {
  unsigned long now = millis();
  if (currentState != input.rawState) {
    input.rawState = currentState;
    input.lastChange = now;
  }
  if (input.stableState != input.rawState && (unsigned long)(now - input.lastChange) >= DEBOUNCE_MS) {
    input.stableState = input.rawState;
    return true;
  }
  return false;
}

void syncDebounceState() {
  for (byte i = 0; i < NUM_BUTTONS; i++) {
    bool s = readButton(i);
    buttons[i].rawState = s;
    buttons[i].stableState = s;
    buttons[i].lastChange = millis();
  }
  for (byte i = 0; i < NUM_RESETS; i++) {
    bool s = readReset(i);
    resets[i].rawState = s;
    resets[i].stableState = s;
    resets[i].lastChange = millis();
  }
}

void checkButtons() {
  if (simulationMode) return;
  for (byte i = 0; i < NUM_BUTTONS; i++) {
    bool pressed = readButton(i);
    if (updateDebounce(buttons[i], pressed)) {
      sendButtonEvent(i, buttons[i].stableState);
    }
  }
}

void checkResetPins() {
  if (simulationMode) return;
  for (byte i = 0; i < NUM_RESETS; i++) {
    bool pressed = readReset(i);
    if (updateDebounce(resets[i], pressed)) {
      if (resets[i].stableState) {
        resetCounter(i);
        sendResetEvent(i);
      }
    }
  }
}

void processSimulatedResetPulse() {
  static bool previous[NUM_RESETS] = {false};
  if (!simulationMode) {
    for (byte i = 0; i < NUM_RESETS; i++) previous[i] = false;
    return;
  }
  for (byte i = 0; i < NUM_RESETS; i++) {
    if (simResets[i] && !previous[i]) {
      resetCounter(i);
      sendResetEvent(i);
    }
    previous[i] = simResets[i];
  }
}

// =====================================================
// UITGANGEN
// =====================================================
void applyOutput(byte index, bool state) {
  if (index >= NUM_OUTPUTS) return;
  outputStates[index] = state;
  if (!simulationMode) digitalWrite(outputPins[index], state ? HIGH : LOW);
  sendOutputEvent(index);
}

void toggleOutput(byte index) {
  if (index >= NUM_OUTPUTS) return;
  applyOutput(index, !outputStates[index]);
}

// =====================================================
// MOTOREN
// =====================================================
void disableMotor(byte index) {
  digitalWrite(motors[index].stepPin, LOW);
  digitalWrite(motors[index].enablePin, HIGH);
  motors[index].pulseHigh = false;
}

void stopMotor(byte index, const char* status) {
  if (index >= NUM_MOTORS) return;
  Motor &m = motors[index];
  if (!m.simulatedRun) disableMotor(index);
  m.moving = false;
  m.mode = MOTOR_IDLE;
  m.pulseHigh = false;
  m.simulatedRun = false;
  m.stepsTaken = 0;
  m.targetSteps = 0;
  sendMotorStatus(index + 1, status);
}

void stopAllMotors() {
  for (byte i = 0; i < NUM_MOTORS; i++) {
    if (motors[i].moving) stopMotor(i, "stopped");
  }
}

bool validateStepInterval(unsigned long stepIntervalUs) {
  return stepIntervalUs >= MIN_STEP_INTERVAL_US && stepIntervalUs <= MAX_STEP_INTERVAL_US;
}

void startMotorMove(byte index, bool direction, unsigned long steps, unsigned long stepIntervalUs, const char* source) {
  if (index >= NUM_MOTORS) {
    sendError("invalid_motor");
    return;
  }
  if (steps == 0) {
    sendError("invalid_steps");
    return;
  }
  if (!validateStepInterval(stepIntervalUs)) {
    sendError("invalid_interval");
    return;
  }

  Motor &m = motors[index];
  if (m.moving) {
    sendError("motor_already_moving");
    return;
  }

  m.direction = direction;
  m.targetSteps = steps;
  m.stepsTaken = 0;
  m.stepIntervalUs = stepIntervalUs;
  m.lastStepUs = micros();
  m.startedAtMs = millis();
  m.pulseHigh = false;
  m.simulatedRun = simulationMode;
  m.mode = MOTOR_MOVE;
  m.moving = true;

  if (!simulationMode) {
    digitalWrite(m.stepPin, LOW);
    digitalWrite(m.dirPin, direction ? HIGH : LOW);
    digitalWrite(m.enablePin, LOW);
  }

  sendMotorMoveEvent(index + 1, direction, steps, stepIntervalUs, source);
  sendMotorStatus(index + 1, "moving");
}

void startHoming(byte index) {
  if (index >= NUM_MOTORS) {
    sendError("invalid_motor");
    return;
  }

  Motor &m = motors[index];
  if (m.moving) {
    sendError("motor_already_moving");
    return;
  }

  m.stepsTaken = 0;
  m.targetSteps = 0;
  m.stepIntervalUs = motors[index].stepIntervalUs;
  m.lastStepUs = micros();
  m.startedAtMs = millis();
  m.pulseHigh = false;
  m.simulatedRun = simulationMode;
  m.mode = MOTOR_HOME;
  m.moving = true;
  m.homed = false;
  m.direction = m.homeDirection;

  if (readHomeSensor(index)) {
    m.homed = true;
    m.moving = false;
    m.mode = MOTOR_IDLE;
    sendMotorStatus(index + 1, "home_position_reached");
    return;
  }

  if (!simulationMode) {
    digitalWrite(m.stepPin, LOW);
    digitalWrite(m.dirPin, m.homeDirection);
    digitalWrite(m.enablePin, LOW);
  }

  sendMotorStatus(index + 1, "moving_to_home");
}

void updateMotor(byte index) {
  Motor &m = motors[index];
  if (!m.moving) return;

  if (m.mode == MOTOR_HOME) {
    if (readHomeSensor(index)) {
      m.homed = true;
      stopMotor(index, "home_position_reached");
      return;
    }
    if ((unsigned long)(millis() - m.startedAtMs) >= HOMING_TIMEOUT_MS) {
      stopMotor(index, "homing_timeout");
      return;
    }
    if (m.stepsTaken >= HOMING_MAX_STEPS) {
      stopMotor(index, "homing_step_limit");
      return;
    }
  } else {
    if (m.stepsTaken >= m.targetSteps) {
      stopMotor(index, "move_completed");
      return;
    }
  }

  unsigned long now = micros();

  if (m.simulatedRun) {
    if ((unsigned long)(now - m.lastStepUs) >= m.stepIntervalUs) {
      m.lastStepUs = now;
      m.stepsTaken++;
    }
    return;
  }

  if (m.pulseHigh) {
    if ((unsigned long)(now - m.lastStepUs) >= STEP_PULSE_US) {
      digitalWrite(m.stepPin, LOW);
      m.pulseHigh = false;
    }
  } else {
    if ((unsigned long)(now - m.lastStepUs) >= m.stepIntervalUs) {
      digitalWrite(m.stepPin, HIGH);
      m.lastStepUs = now;
      m.pulseHigh = true;
      m.stepsTaken++;
    }
  }
}

void updateMotors() {
  for (byte i = 0; i < NUM_MOTORS; i++) updateMotor(i);
}

// =====================================================
// SIMULATIEMODUS
// =====================================================
void setSimulationMode(bool enabled) {
  stopAllMotors();
  for (byte i = 0; i < NUM_OUTPUTS; i++) { digitalWrite(outputPins[i], LOW); outputStates[i] = false; }
  sendOutputStatus();
  simulationMode = enabled;

  for (byte i = 0; i < NUM_BUTTONS; i++) simButtons[i] = false;
  for (byte i = 0; i < NUM_RESETS; i++) simResets[i] = false;
  for (byte i = 0; i < NUM_MOTORS; i++) simHomeSensors[i] = false;

  syncDebounceState();
  sendSimulationStatus();
}

// =====================================================
// RFID
// =====================================================
void checkRFID() {
  if (simulationMode) return;
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  char uid[32] = {0};
  size_t used = 0;

  for (byte i = 0; i < mfrc522.uid.size; i++) {
    int written = snprintf(uid + used, sizeof(uid) - used, i == 0 ? "%02X" : ":%02X", mfrc522.uid.uidByte[i]);
    if (written < 0 || (size_t)written >= sizeof(uid) - used) {
      sendError("rfid_uid_too_long");
      mfrc522.PICC_HaltA();
      mfrc522.PCD_StopCrypto1();
      return;
    }
    used += (size_t)written;
  }

  sendEvent("kaart_scat", "uid", uid);
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();
}

// =====================================================
// PARSING HULPFUNCTIES
// =====================================================
bool parseIndex(const char* text, byte maximum, byte &index) {
  if (*text == '\0') return false;
  unsigned int value = 0;
  while (*text) {
    if (!isdigit((unsigned char)*text)) return false;
    value = value * 10 + (*text - '0');
    if (value > maximum) return false;
    text++;
  }
  if (value < 1 || value > maximum) return false;
  index = (byte)(value - 1);
  return true;
}

bool parseULongSafe(const char* text, unsigned long &value) {
  if (*text == '\0') return false;
  char *endPtr = NULL;
  unsigned long v = strtoul(text, &endPtr, 10);
  if (*endPtr != '\0') return false;
  value = v;
  return true;
}

bool parseIndexedAction(const char* cmd, const char* prefix, byte maximum, byte &index, const char* &action) {
  size_t prefixLength = strlen(prefix);
  if (strncmp(cmd, prefix, prefixLength) != 0) return false;

  const char* start = cmd + prefixLength;
  const char* separator = strchr(start, '_');
  if (!separator) return false;

  char number[8];
  size_t length = separator - start;
  if (length == 0 || length >= sizeof(number)) return false;

  memcpy(number, start, length);
  number[length] = '\0';
  if (!parseIndex(number, maximum, index)) return false;

  action = separator + 1;
  return true;
}

bool parseOutputCommand(char* cmd) {
  // OUTPUT_5_ON / OUTPUT_5_OFF / OUTPUT_5_TOGGLE
  if (strncmp(cmd, "OUTPUT_", 7) != 0) return false;

  char work[SERIAL_BUFFER_SIZE];
  strncpy(work, cmd, sizeof(work));
  work[sizeof(work) - 1] = '\0';

  char* token = strtok(work, "_"); // OUTPUT
  token = strtok(NULL, "_");       // number
  if (!token) { sendError("invalid_output_command"); return true; }

  byte idx;
  if (!parseIndex(token, NUM_OUTPUTS, idx)) { sendError("invalid_output"); return true; }

  token = strtok(NULL, "_");       // action
  if (!token) { sendError("invalid_output_command"); return true; }

  if (strcmp(token, "ON") == 0) applyOutput(idx, true);
  else if (strcmp(token, "OFF") == 0) applyOutput(idx, false);
  else if (strcmp(token, "TOGGLE") == 0) toggleOutput(idx);
  else sendError("invalid_output_action");
  return true;
}

bool parseCounterCommand(char* cmd) {
  // RESET_COUNTER_2
  // SET_COUNTER_2_7
  if (strncmp(cmd, "RESET_COUNTER_", 14) == 0) {
    byte idx;
    if (!parseIndex(cmd + 14, NUM_RESETS, idx)) { sendError("invalid_counter"); return true; }
    resetCounter(idx);
    return true;
  }

  if (strncmp(cmd, "SET_COUNTER_", 12) == 0) {
    char work[SERIAL_BUFFER_SIZE];
    strncpy(work, cmd, sizeof(work));
    work[sizeof(work) - 1] = '\0';

    char* token = strtok(work, "_"); // SET
    token = strtok(NULL, "_");       // COUNTER
    token = strtok(NULL, "_");       // number
    if (!token) { sendError("invalid_counter_command"); return true; }

    byte idx;
    if (!parseIndex(token, NUM_RESETS, idx)) { sendError("invalid_counter"); return true; }

    token = strtok(NULL, "_");       // value
    if (!token) { sendError("invalid_counter_value"); return true; }

    unsigned long val;
    if (!parseULongSafe(token, val) || val > 255UL) { sendError("invalid_counter_value"); return true; }
    setCounter(idx, (byte)val);
    return true;
  }

  return false;
}

bool parseMotorMoveCommand(char* cmd) {
  // MOTOR_1_MOVE_FWD_2000_1000
  if (strncmp(cmd, "MOTOR_", 6) != 0) return false;

  char work[SERIAL_BUFFER_SIZE];
  strncpy(work, cmd, sizeof(work));
  work[sizeof(work) - 1] = '\0';

  char* token = strtok(work, "_"); // MOTOR
  token = strtok(NULL, "_");       // number
  if (!token) { sendError("invalid_motor_command"); return true; }

  byte idx;
  if (!parseIndex(token, NUM_MOTORS, idx)) { sendError("invalid_motor"); return true; }

  token = strtok(NULL, "_");       // action
  if (!token) { sendError("invalid_motor_command"); return true; }

  if (strcmp(token, "MOVE") == 0) {
    token = strtok(NULL, "_");     // FWD / REV
    if (!token) { sendError("invalid_motor_direction"); return true; }
    bool dir;
    if (strcmp(token, "FWD") == 0) dir = LOW;
    else if (strcmp(token, "REV") == 0) dir = HIGH;
    else { sendError("invalid_motor_direction"); return true; }

    token = strtok(NULL, "_");     // steps
    if (!token) { sendError("invalid_steps"); return true; }
    unsigned long steps;
    if (!parseULongSafe(token, steps)) { sendError("invalid_steps"); return true; }

    token = strtok(NULL, "_");     // interval
    unsigned long intervalUs = DEFAULT_STEP_INTERVAL_US;
    if (token && !parseULongSafe(token, intervalUs)) {
      sendError("invalid_interval");
      return true;
    }

    startMotorMove(idx, dir, steps, intervalUs, "serial");
    return true;
  }

  if (strcmp(token, "HOME") == 0) {
    startHoming(idx);
    return true;
  }

  if (strcmp(token, "STOP") == 0) {
    stopMotor(idx, "stopped");
    return true;
  }

  sendError("invalid_motor_action");
  return true;
}

bool parseAliasMotorCommand(char* cmd) {
  // BELT_FWD_1200_1000
  // DOOR_REV_800_1200
  // LIFT_FWD_500_2400
  const char* prefix = NULL;
  byte idx = 255;

  if (strncmp(cmd, "BELT_", 5) == 0) { prefix = "BELT_"; idx = 0; }
  else if (strncmp(cmd, "DOOR_", 5) == 0) { prefix = "DOOR_"; idx = 1; }
  else if (strncmp(cmd, "LIFT_", 5) == 0) { prefix = "LIFT_"; idx = 2; }
  else return false;

  char work[SERIAL_BUFFER_SIZE];
  strncpy(work, cmd + strlen(prefix), sizeof(work));
  work[sizeof(work) - 1] = '\0';

  char* dirToken = strtok(work, "_");
  if (!dirToken) { sendError("invalid_motor_direction"); return true; }
  bool dir;
  if (strcmp(dirToken, "FWD") == 0) dir = LOW;
  else if (strcmp(dirToken, "REV") == 0) dir = HIGH;
  else if (strcmp(dirToken, "OPEN") == 0) dir = LOW;
  else if (strcmp(dirToken, "CLOSE") == 0) dir = HIGH;
  else if (strcmp(dirToken, "UP") == 0) dir = LOW;
  else if (strcmp(dirToken, "DOWN") == 0) dir = HIGH;
  else { sendError("invalid_motor_direction"); return true; }

  char* stepsToken = strtok(NULL, "_");
  if (!stepsToken) { sendError("invalid_steps"); return true; }
  unsigned long steps;
  if (!parseULongSafe(stepsToken, steps)) { sendError("invalid_steps"); return true; }

  char* intervalToken = strtok(NULL, "_");
  unsigned long intervalUs = DEFAULT_STEP_INTERVAL_US;
  if (intervalToken && !parseULongSafe(intervalToken, intervalUs)) {
    sendError("invalid_interval");
    return true;
  }

  startMotorMove(idx, dir, steps, intervalUs, "alias");
  return true;
}

// =====================================================
// COMMAND PROCESSOR
// =====================================================
void processCommand(char* cmd) {
  if (cmd[0] == '\0') return;

  sendEvent("command_received", "cmd", cmd);

  if (strcmp(cmd, "GET_STATUS") == 0) { sendFullStatus(); return; }
  if (strcmp(cmd, "GEEFFLESINHOUD") == 0) { printCounters(); return; }
  if (strcmp(cmd, "SIM_MODE_ON") == 0) { setSimulationMode(true); return; }
  if (strcmp(cmd, "SIM_MODE_OFF") == 0) { setSimulationMode(false); return; }
  if (strcmp(cmd, "STOP_ALL") == 0) { stopAllMotors(); sendEvent("system", "status", "all_motors_stopped"); return; }

  if (parseOutputCommand(cmd)) return;
  if (parseCounterCommand(cmd)) return;
  if (parseMotorMoveCommand(cmd)) return;
  if (parseAliasMotorCommand(cmd)) return;

  if (strncmp(cmd, "SIM_", 4) == 0) {
    if (!simulationMode) {
      sendError("simulation_not_enabled");
      return;
    }

    byte index;
    const char* action;

    if (parseIndexedAction(cmd, "SIM_BUTTON_", NUM_BUTTONS, index, action)) {
      if (strcmp(action, "PRESS") == 0) {
        simButtons[index] = true;
        sendButtonEvent(index, true);
        return;
      }
      if (strcmp(action, "RELEASE") == 0) {
        simButtons[index] = false;
        sendButtonEvent(index, false);
        return;
      }
      sendError("invalid_button_action");
      return;
    }

    if (parseIndexedAction(cmd, "SIM_RESET_", NUM_RESETS, index, action)) {
      if (strcmp(action, "PRESS") == 0) {
        if (!simResets[index]) { simResets[index] = true; resetCounter(index); sendResetEvent(index); }
        return;
      }
      if (strcmp(action, "RELEASE") == 0) {
        simResets[index] = false;
        return;
      }
      sendError("invalid_reset_action");
      return;
    }

    if (parseIndexedAction(cmd, "SIM_HOME_", NUM_MOTORS, index, action)) {
      if (strcmp(action, "ON") == 0) {
        simHomeSensors[index] = true;
        sendMotorStatus(index + 1, "home_sensor_on");
        return;
      }
      if (strcmp(action, "OFF") == 0) {
        simHomeSensors[index] = false;
        motors[index].homed = false;
        sendMotorStatus(index + 1, "home_sensor_off");
        return;
      }
      sendError("invalid_home_action");
      return;
    }

    if (strncmp(cmd, "SIM_CARD_", 9) == 0) {
      const char* uid = cmd + 9;
      size_t len = strlen(uid);
      if (len == 0 || len > 29) {
        sendError("invalid_uid");
        return;
      }
      for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)uid[i]) && uid[i] != ':') {
          sendError("invalid_uid");
          return;
        }
      }
      char normalized[32];
      for (size_t i = 0; i < len; i++) normalized[i] = toupper((unsigned char)uid[i]);
      normalized[len] = '\0';
      sendEvent("kaart_scat", "uid", normalized);
      return;
    }

    sendError("unknown_simulation_command");
    return;
  }

  sendError("unknown_command");
}

// =====================================================
// SERIAL LEZEN
// =====================================================
void readSerialCommands() {
  byte processed = 0;
  while (Serial.available() && processed < 32) {
    processed++;
    char c = (char)Serial.read();

    if (c == '\r') continue;

    if (c == '\n') {
      if (serialOverflow) {
        sendError("serial_command_too_long");
      } else {
        serialBuffer[serialLength] = '\0';
        if (serialLength > 0) processCommand(serialBuffer);
      }
      serialLength = 0;
      serialOverflow = false;
      continue;
    }

    if (serialOverflow) continue;
    if (serialLength < SERIAL_BUFFER_SIZE - 1) serialBuffer[serialLength++] = c;
    else serialOverflow = true;
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(9600);
  simulationMode = false;

  for (byte i = 0; i < NUM_BUTTONS; i++) {
    for (byte j = 0; j < 3; j++) {
      pinMode(ledPins[i][j], OUTPUT);
      digitalWrite(ledPins[i][j], LOW);
    }
  }

  for (byte i = 0; i < NUM_BUTTONS; i++) pinMode(buttonPins[i], INPUT_PULLUP);

  for (byte i = 0; i < NUM_RESETS; i++) {
    pinMode(resetPins[i], INPUT_PULLUP);
    digitalWrite(outputPins[i], LOW);
    pinMode(outputPins[i], OUTPUT);
    outputStates[i] = false;
  }

  for (byte i = 0; i < NUM_MOTORS; i++) {
    Motor &m = motors[i];
    digitalWrite(m.enablePin, HIGH);
    pinMode(m.enablePin, OUTPUT);
    pinMode(m.dirPin, OUTPUT);
    pinMode(m.stepPin, OUTPUT);
    pinMode(m.homePin, INPUT_PULLUP);
    digitalWrite(m.stepPin, LOW);
    m.moving = false;
    m.homed = false;
    m.pulseHigh = false;
    m.simulatedRun = false;
    m.mode = MOTOR_IDLE;
  }

  initializeEEPROM();
  SPI.begin();
  mfrc522.PCD_Init();
  syncDebounceState();

  sendEvent("system", "status", "ready");
  sendFullStatus();
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  readSerialCommands();
  updateMotors();
  checkButtons();
  checkResetPins();
  checkRFID();
}