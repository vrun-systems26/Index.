/*  Index v1 firmware
    X = horizontal (stepper A block), Z = vertical (stepper B block, positive = up)
    Y = rack servo (GPIO 25, 175 = retracted, lower = extended), magnet = GPIO 26
    Switches: X-min 32, X-max 21, Z-min 33 (C -> GND, NO -> pin)
    WiFi: "Index" / "12345678"  ->  http://192.168.4.1
    USB serial (115200): "GO n" -> "ACK n", "RET", "HOME", "STOP"
*/
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include <AccelStepper.h>

// ================= hardware =================
AccelStepper stepX(AccelStepper::FULL4WIRE, 13, 27, 4, 2);     // A block (13/27 swapped = correct direction)
AccelStepper stepZ(AccelStepper::FULL4WIRE, 17, 12, 15, 14);   // B block, positive = up
#define SERVO_PIN  25
#define MAG_PIN    26
#define X_MIN_PIN  32
#define X_MAX_PIN  21
#define Z_MIN_PIN  33

// ================= tuning =================
#define X_SPEED          250    // steps/s for X moves
#define X_ACCEL          400
#define X_HOME_SPEED     120    // slow, so hitting X-min doesn't shake
#define Z_SPEED          300
#define Z_ACCEL          300
#define Z_HOME_SPEED     250
#define BACKOFF          40     // steps past switch release = position 0
#define X_SOFT_MAX       960    // X length measured 1037 between switches
#define Z_SOFT_MAX       1150   // delivery ~1060-1120. Lower this if your Z rail can't reach it
#define Y_RETRACT        175    // 180 = fully retracted, 5 deg margin
#define Y_EXTEND_LIMIT   5      // 0 = fully extended, 5 deg margin
#define Y_STEP_DEG       2
#define Y_STEP_MS        20
#define MAG_SETTLE_MS    400
#define RETURN_LIFT      15     // steps (3 mm) Z goes ABOVE the taught height when pushing a tray back in
#define RELEASE_LIFT     50     // steps (10 mm) Z rises after the magnet turns off, to slide off the washer before retracting
#define PARK_RELEASE_MS  5000   // Z releases this long after parking (saves battery)
#define PRESS_MS         5
#define RELEASE_MS       30

const char* AP_SSID = "Index";
const char* AP_PASS = "12345678";

// ================= types =================
struct Sw  { int pin; bool state; bool raw; unsigned long since; };
struct Pos { int32_t x; int32_t z; uint8_t set; };
enum StepType { ST_HOME, ST_MOVE, ST_SERVO, ST_MAG, ST_WAIT };
struct Step { StepType t; long a; long b; };
enum FinishType { FIN_NONE, FIN_DELIVERED, FIN_RETURNED, FIN_PARKED };

// ================= globals =================
WebServer server(80);
Preferences prefs;
Servo yServo;

Sw swXMin = { X_MIN_PIN, false, false, 0 };
Sw swXMax = { X_MAX_PIN, false, false, 0 };
Sw swZMin = { Z_MIN_PIN, false, false, 0 };

const char* DEFAULT_NAMES[15] = {
  "Bandages", "Ear drops", "Ibuprofen", "Allergy tablets", "Thermometer",
  "Cough syrup", "Tweezers", "Gauze", "Antiseptic wipes", "Eye drops",
  "Screwdriver", "AA batteries", "Duct tape", "Scissors", ""
};

Pos comp[15];
Pos deliv = { 0, 0, 0 };
int32_t delivAngle = Y_RETRACT;
int32_t reachAngle = 20;
String names[15];

int  yAngle = Y_RETRACT, yTarget = Y_RETRACT;
unsigned long yLastStep = 0;
bool magOn = false;

bool homed = false;
int  activeComp = 0;          // compartment whose tray is out (0 = none)
int  pendingComp = 0;
String mode = "idle";         // idle, homing, moving, retrieving, at delivery, returning, parking, stopped
String lastMsg = "Ready. Open Setup and press Home.";
String doneMsg = "";
bool parked = false;
unsigned long parkedAt = 0;

const long KEEP = -999999;
Step seq[20];
int  seqLen = 0, seqIdx = 0;
bool seqActive = false, stepStarted = false;
unsigned long stepT0 = 0;
FinishType finishAction = FIN_NONE;
int  homePhase = 0;
long homeStart = 0;

String serialLine = "";

// ================= switches =================
void initSw(Sw &s) {
  pinMode(s.pin, INPUT_PULLUP);
  s.raw = s.state = (digitalRead(s.pin) == LOW);
  s.since = millis();
}

void updateSw(Sw &s) {
  bool r = (digitalRead(s.pin) == LOW);
  unsigned long now = millis();
  if (r != s.raw) { s.raw = r; s.since = now; }
  unsigned long need = r ? PRESS_MS : RELEASE_MS;
  if (r != s.state && now - s.since >= need) s.state = r;
}

void updateAll() { updateSw(swXMin); updateSw(swXMax); updateSw(swZMin); }
bool xMinHit() { return swXMin.state; }
bool xMaxHit() { return swXMax.state; }
bool zMinHit() { return swZMin.state; }

// ================= servo + magnet =================
void updateServo() {
  if (yAngle == yTarget) return;
  if (millis() - yLastStep < Y_STEP_MS) return;
  yLastStep = millis();
  int d = yTarget - yAngle;
  if (d >  Y_STEP_DEG) d =  Y_STEP_DEG;
  if (d < -Y_STEP_DEG) d = -Y_STEP_DEG;
  yAngle += d;
  yServo.write(yAngle);
}

void setMag(bool on) {
  magOn = on;
  digitalWrite(MAG_PIN, on ? HIGH : LOW);
}

// ================= storage =================
void saveAll() {
  prefs.putBytes("comp", comp, sizeof(comp));
  prefs.putBytes("deliv", &deliv, sizeof(deliv));
  prefs.putInt("dang", delivAngle);
  prefs.putInt("reach", reachAngle);
  for (int i = 0; i < 15; i++) prefs.putString(("n" + String(i)).c_str(), names[i]);
}

void loadAll() {
  memset(comp, 0, sizeof(comp));
  if (prefs.isKey("comp")  && prefs.getBytesLength("comp")  == sizeof(comp))  prefs.getBytes("comp", comp, sizeof(comp));
  if (prefs.isKey("deliv") && prefs.getBytesLength("deliv") == sizeof(deliv)) prefs.getBytes("deliv", &deliv, sizeof(deliv));
  delivAngle = prefs.getInt("dang", Y_RETRACT);
  reachAngle = prefs.getInt("reach", 20);
  for (int i = 0; i < 15; i++) {
    String k = "n" + String(i);
    names[i] = prefs.isKey(k.c_str()) ? prefs.getString(k.c_str(), "") : String(DEFAULT_NAMES[i]);
  }
}

// ================= sequencer =================
void abortAll(const String& why, bool lostHome) {
  seqActive = false;
  stepX.moveTo(stepX.currentPosition());     // stop where we are
  stepZ.moveTo(stepZ.currentPosition());     // Z keeps holding
  stepX.disableOutputs();                    // X doesn't need to hold
  yTarget = yAngle;
  if (finishAction == FIN_DELIVERED && magOn) activeComp = pendingComp;   // tray is probably on the magnet
  if (lostHome) homed = false;
  mode = "stopped";
  lastMsg = why;
  Serial.println(">>> STOPPED: " + why);
}

void seqClear() { seqLen = 0; seqIdx = 0; stepStarted = false; }

void add(StepType t, long a, long b) {
  if (seqLen < 20) { seq[seqLen].t = t; seq[seqLen].a = a; seq[seqLen].b = b; seqLen++; }
}

void seqStart(const String& runMode, FinishType fin, const String& msgWhenDone) {
  mode = runMode;
  finishAction = fin;
  doneMsg = msgWhenDone;
  parked = false;
  seqIdx = 0;
  stepStarted = false;
  seqActive = true;
}

bool homeTick() {
  long zp = stepZ.currentPosition(), xp = stepX.currentPosition();
  switch (homePhase) {
    case 0:                                         // wait for rack to retract, then start Z
      if (yAngle != Y_RETRACT) return false;
      stepZ.enableOutputs();
      homeStart = zp;
      if (zMinHit()) { homePhase = 1; stepZ.setSpeed(Z_HOME_SPEED); }
      else           { homePhase = 2; stepZ.setSpeed(-Z_HOME_SPEED); }
      return false;
    case 1:                                         // lift off Z-min
      stepZ.runSpeed();
      if (!zMinHit()) { homePhase = 2; stepZ.setSpeed(-Z_HOME_SPEED); homeStart = zp; }
      else if (labs(zp - homeStart) > 800) abortAll("Z-min never released - check Z-min wiring", true);
      return false;
    case 2:                                         // down to Z-min
      stepZ.runSpeed();
      if (zMinHit()) { homePhase = 3; stepZ.setSpeed(Z_HOME_SPEED); homeStart = zp; }
      else if (labs(zp - homeStart) > Z_SOFT_MAX + 300) abortAll("Z never reached Z-min - check Z-min wiring", true);
      return false;
    case 3:                                         // up until released, then BACKOFF more
      stepZ.runSpeed();
      if (!zMinHit()) { stepZ.setCurrentPosition(-BACKOFF); stepZ.moveTo(0); homePhase = 4; }
      else if (labs(zp - homeStart) > 800) abortAll("Z-min never released - check Z-min wiring", true);
      return false;
    case 4:                                         // finish Z backoff, start X
      stepZ.run();
      if (stepZ.distanceToGo() == 0) {
        stepX.enableOutputs();
        homeStart = xp;
        if (xMinHit()) { homePhase = 5; stepX.setSpeed(X_HOME_SPEED); }
        else           { homePhase = 6; stepX.setSpeed(-X_HOME_SPEED); }
      }
      return false;
    case 5:                                         // lift off X-min
      stepX.runSpeed();
      if (!xMinHit()) { homePhase = 6; stepX.setSpeed(-X_HOME_SPEED); homeStart = xp; }
      else if (labs(xp - homeStart) > 800) abortAll("X-min never released - check X-min wiring", true);
      return false;
    case 6:                                         // toward X-min
      stepX.runSpeed();
      if (xMaxHit()) abortAll("Hit X-max while homing - X direction is wrong", true);
      else if (xMinHit()) { homePhase = 7; stepX.setSpeed(X_HOME_SPEED); homeStart = xp; }
      else if (labs(xp - homeStart) > X_SOFT_MAX + 300) abortAll("X never reached X-min - check X-min wiring", true);
      return false;
    case 7:                                         // off X-min, then BACKOFF more
      stepX.runSpeed();
      if (!xMinHit()) { stepX.setCurrentPosition(-BACKOFF); stepX.moveTo(0); homePhase = 8; }
      else if (labs(xp - homeStart) > 800) abortAll("X-min never released - check X-min wiring", true);
      return false;
    case 8:
      stepX.run();
      if (stepX.distanceToGo() == 0) { homed = true; return true; }
      return false;
  }
  return false;
}

void startStep(Step &s) {
  switch (s.t) {
    case ST_HOME:
      homePhase = 0;
      yTarget = Y_RETRACT;
      break;
    case ST_MOVE: {
      long tx = (s.a == KEEP) ? stepX.currentPosition() : constrain(s.a, 0L, (long)X_SOFT_MAX);
      long tz = (s.b == KEEP) ? stepZ.currentPosition() : constrain(s.b, 0L, (long)Z_SOFT_MAX);
      stepX.enableOutputs();
      stepZ.enableOutputs();
      stepX.moveTo(tx);
      stepZ.moveTo(tz);
      break;
    }
    case ST_SERVO:
      yTarget = constrain((int)s.a, Y_EXTEND_LIMIT, Y_RETRACT);
      break;
    case ST_MAG:
      setMag(s.a != 0);
      break;
    case ST_WAIT:
      break;
  }
}

bool stepDone(Step &s) {
  switch (s.t) {
    case ST_HOME:
      return homeTick();
    case ST_MOVE:
      stepX.run();
      stepZ.run();
      if (xMaxHit())              { abortAll("X-max switch hit during a move", true); return false; }
      if (xMinHit() || zMinHit()) { abortAll("Min switch hit during a move - press Home", true); return false; }
      return stepX.distanceToGo() == 0 && stepZ.distanceToGo() == 0;
    case ST_SERVO:
      return yAngle == yTarget;
    case ST_MAG:
      return true;
    case ST_WAIT:
      return millis() - stepT0 >= (unsigned long)s.a;
  }
  return true;
}

void finishSeq() {
  seqActive = false;
  stepX.disableOutputs();                    // X is horizontal and stays put unpowered - saves battery

  switch (finishAction) {
    case FIN_DELIVERED:
      activeComp = pendingComp;
      mode = "at delivery";
      lastMsg = "Compartment " + String(activeComp) + " (" + names[activeComp - 1] + ") is ready. Press Return when done.";
      break;
    case FIN_RETURNED:
      lastMsg = "Compartment " + String(activeComp) + " returned.";
      activeComp = 0;
      parked = true; parkedAt = millis();
      mode = "idle";
      break;
    case FIN_PARKED:
      parked = true; parkedAt = millis();
      mode = "idle";
      lastMsg = "Parked.";
      break;
    default:
      mode = "idle";
      if (doneMsg.length()) lastMsg = doneMsg;
      break;
  }
  Serial.println(">>> " + lastMsg);
}

void runSeq() {
  if (!seqActive) return;
  if (seqIdx >= seqLen) { finishSeq(); return; }
  Step &s = seq[seqIdx];
  if (!stepStarted) { startStep(s); stepStarted = true; stepT0 = millis(); }
  if (stepDone(s) && seqActive) { seqIdx++; stepStarted = false; }
}

// ================= commands (web + serial) =================
String cmdRetrieve(int c) {
  if (c < 1 || c > 15)       return "ERR compartment must be 1-15";
  if (seqActive)             return "ERR busy";
  if (activeComp)            return "ERR compartment " + String(activeComp) + " is still out - press Return first";
  if (!names[c - 1].length()) return "ERR compartment " + String(c) + " is empty";
  if (!comp[c - 1].set)      return "ERR compartment " + String(c) + " position not taught";
  if (!deliv.set)            return "ERR delivery point not taught";
  Pos p = comp[c - 1];
  seqClear();
  if (!homed) add(ST_HOME, 0, 0);
  add(ST_MAG, 0, 0);
  add(ST_SERVO, Y_RETRACT, 0);
  add(ST_MOVE, p.x, p.z);                  // to the compartment
  add(ST_SERVO, reachAngle, 0);            // extend to the tray
  add(ST_MAG, 1, 0);                       // grab
  add(ST_WAIT, MAG_SETTLE_MS, 0);
  add(ST_SERVO, Y_RETRACT, 0);             // pull the tray out
  add(ST_MOVE, KEEP, deliv.z);             // rise
  add(ST_MOVE, deliv.x, KEEP);             // across
  add(ST_SERVO, delivAngle, 0);            // set it on top, magnet keeps holding
  pendingComp = c;
  seqStart("retrieving", FIN_DELIVERED, "");
  lastMsg = "Retrieving " + String(c) + " (" + names[c - 1] + ")";
  Serial.println(">>> " + lastMsg);
  return "OK";
}

String cmdReturn() {
  if (seqActive)   return "ERR busy";
  if (!activeComp) return "ERR no tray is out";
  Pos p = comp[activeComp - 1];
  seqClear();
  add(ST_SERVO, Y_RETRACT, 0);             // pull tray back onto the carriage
  add(ST_MOVE, p.x, KEEP);                 // across, still above
  add(ST_MOVE, KEEP, p.z + RETURN_LIFT);   // down to the compartment, 3 mm higher so the tray clears the opening
  add(ST_SERVO, reachAngle, 0);            // push it in
  add(ST_MAG, 0, 0);                       // let go
  add(ST_WAIT, MAG_SETTLE_MS, 0);
  add(ST_MOVE, KEEP, p.z + RETURN_LIFT + RELEASE_LIFT);   // slide up 10 mm off the washer so the tray stays put
  add(ST_SERVO, Y_RETRACT, 0);
  add(ST_MOVE, 0, 0);                      // park at home
  pendingComp = activeComp;
  seqStart("returning", FIN_RETURNED, "");
  lastMsg = "Returning compartment " + String(activeComp);
  return "OK";
}

String cmdHome() {
  if (seqActive)  return "ERR busy";
  if (activeComp) return "ERR a tray is out - press Return first";
  seqClear();
  add(ST_MAG, 0, 0);
  add(ST_HOME, 0, 0);
  seqStart("homing", FIN_NONE, "Homed.");
  return "OK homing";
}

String cmdPark() {
  if (seqActive)  return "ERR busy";
  if (activeComp) return "ERR a tray is out - press Return first";
  seqClear();
  add(ST_MAG, 0, 0);
  add(ST_SERVO, Y_RETRACT, 0);
  if (!homed) add(ST_HOME, 0, 0);
  add(ST_MOVE, 0, 0);
  seqStart("parking", FIN_PARKED, "");
  return "OK parking";
}

String cmdRelease() {
  if (seqActive)  return "ERR busy";
  if (activeComp) return "ERR a tray is out - Z would drop it. Press Return first";
  stepX.disableOutputs();
  stepZ.disableOutputs();
  homed = false;
  parked = false;
  lastMsg = "Motors released. Z dropped to the bottom - press Home before moving.";
  return "OK released";
}

String cmdStop() {
  if (!seqActive) return "OK nothing was running";
  abortAll("Stopped by you", false);
  return "OK stopped";
}

String cmdJog(const String& axis, long d) {
  if (seqActive) return "ERR busy";
  if (!homed)    return "ERR press Home first";
  seqClear();
  if (axis == "x") add(ST_MOVE, stepX.currentPosition() + d, KEEP);
  else             add(ST_MOVE, KEEP, stepZ.currentPosition() + d);
  seqStart("moving", FIN_NONE, "");
  return "OK";
}

String cmdY(long v) {
  if (seqActive) return "ERR busy";
  yTarget = constrain((int)v, Y_EXTEND_LIMIT, Y_RETRACT);
  return "OK rack -> " + String(yTarget) + " deg";
}

String cmdMag(bool on) {
  if (seqActive) return "ERR busy";
  setMag(on);
  return on ? "OK magnet ON" : "OK magnet off";
}

String cmdSave(int c) {
  if (c < 1 || c > 15) return "ERR compartment must be 1-15";
  if (!homed)          return "ERR press Home first";
  comp[c - 1].x = stepX.currentPosition();
  comp[c - 1].z = stepZ.currentPosition();
  comp[c - 1].set = 1;
  saveAll();
  return "OK saved compartment " + String(c) + " at X " + String(comp[c - 1].x) + "  Z " + String(comp[c - 1].z);
}

String cmdGoto(int c) {
  if (c < 1 || c > 15)  return "ERR compartment must be 1-15";
  if (seqActive)        return "ERR busy";
  if (activeComp)       return "ERR a tray is out - press Return first";
  if (!comp[c - 1].set) return "ERR compartment " + String(c) + " not taught";
  seqClear();
  if (!homed) add(ST_HOME, 0, 0);
  add(ST_SERVO, Y_RETRACT, 0);
  add(ST_MOVE, comp[c - 1].x, comp[c - 1].z);
  seqStart("moving", FIN_NONE, "At compartment " + String(c) + ". Nudge and Save if it's off.");
  return "OK";
}

String cmdFill() {
  if (!comp[0].set || !comp[4].set || !comp[10].set) return "ERR teach compartments 1, 5 and 11 first";
  double x1 = comp[0].x, z1 = comp[0].z;
  double cx = (comp[4].x - x1) / 4.0,  cz = (comp[4].z - z1) / 4.0;     // one column
  double rx = (comp[10].x - x1) / 2.0, rz = (comp[10].z - z1) / 2.0;    // one row
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 5; c++) {
      int i = r * 5 + c;
      comp[i].x = lround(x1 + c * cx + r * rx);
      comp[i].z = lround(z1 + c * cz + r * rz);
      comp[i].set = 1;
    }
  }
  saveAll();
  return "OK filled all 15. Now use Go to it on each and nudge any that are off.";
}

String cmdSaveDeliv() {
  if (!homed) return "ERR press Home first";
  deliv.x = stepX.currentPosition();
  deliv.z = stepZ.currentPosition();
  deliv.set = 1;
  delivAngle = yAngle;
  saveAll();
  return "OK delivery saved at X " + String(deliv.x) + "  Z " + String(deliv.z) + "  rack " + String(delivAngle) + " deg";
}

String cmdGotoDeliv() {
  if (seqActive)  return "ERR busy";
  if (activeComp) return "ERR a tray is out - press Return first";
  if (!deliv.set) return "ERR delivery point not taught";
  seqClear();
  if (!homed) add(ST_HOME, 0, 0);
  add(ST_SERVO, Y_RETRACT, 0);
  add(ST_MOVE, deliv.x, deliv.z);
  seqStart("moving", FIN_NONE, "At the delivery point (rack retracted).");
  return "OK";
}

String cmdSaveReach() {
  reachAngle = yAngle;
  saveAll();
  return "OK reach angle = " + String(reachAngle) + " deg";
}

String cleanName(String v) {
  String o = "";
  for (unsigned int i = 0; i < v.length(); i++) {
    char ch = v[i];
    if (ch == '"' || ch == '\\' || ch == '<' || ch == '>' || (uint8_t)ch < 0x20) continue;
    o += ch;
  }
  o.trim();
  if (o.length() > 24) o = o.substring(0, 24);
  return o;
}

String cmdName(int c, const String& v) {
  if (c < 1 || c > 15) return "ERR compartment must be 1-15";
  names[c - 1] = cleanName(v);
  saveAll();
  return names[c - 1].length() ? "OK compartment " + String(c) + " = " + names[c - 1]
                               : "OK compartment " + String(c) + " marked empty";
}

// ================= serial (voice page) =================
void handleSerial(const String& l) {
  if (l.startsWith("GO ")) {
    int c = l.substring(3).toInt();
    String r = cmdRetrieve(c);
    if (r == "OK") Serial.println("ACK " + String(c));
    else           Serial.println(r);
  }
  else if (l == "RET")  Serial.println(cmdReturn());
  else if (l == "HOME") Serial.println(cmdHome());
  else if (l == "STOP") Serial.println(cmdStop());
}

void readSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n') { serialLine.trim(); if (serialLine.length()) handleSerial(serialLine); serialLine = ""; }
    else if (ch != '\r' && serialLine.length() < 40) serialLine += ch;
  }
}

// ================= web page =================
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Index</title>
<style>
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,sans-serif;background:linear-gradient(105deg,#FDE9D6,#F9BFA5);min-height:100vh;color:#4A3A32}
.wrap{max-width:960px;margin:0 auto;padding:20px 16px 60px}
.top{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:12px}
.brand{font-size:26px;font-weight:700}.brand span{color:#2A5F5A}
.pill{border-radius:100px;padding:7px 14px;font:600 13px monospace;background:#DCEDEA;color:#2A5F5A}
.pill.busy{background:#F7E2D6;color:#C0704E}.pill.err{background:#F6D5D5;color:#A33}
.tabs{display:flex;gap:8px;margin-bottom:12px}
.tab{flex:1;padding:10px;border-radius:12px;background:#FFFBF8aa;border:0;font-weight:600;font-size:15px;cursor:pointer;color:#4A3A32}
.tab.on{background:#2A5F5A;color:#fff}
.card{background:#FFFBF8cc;border-radius:16px;padding:16px;margin-bottom:14px}
.msg{font:13px monospace;color:#7a6457;margin-bottom:12px;min-height:16px}
input,select{width:100%;padding:11px 12px;border-radius:10px;border:1px solid #F3C9B3;background:#FFFBF8;font-size:15px;color:#4A3A32}
.grid{display:grid;grid-template-columns:repeat(5,1fr);gap:8px;margin-top:12px}
.cell{background:#FFFBF8;border:2px solid transparent;border-radius:12px;padding:10px;min-height:76px;cursor:pointer;display:flex;flex-direction:column;justify-content:space-between}
.cell .n{font:11px monospace;color:#9C8576}
.cell .i{font-weight:600;font-size:14px;margin-top:6px;word-break:break-word}
.cell.empty .i{color:#C9B6AA;font-style:italic;font-weight:400}
.cell.sel{border-color:#F0A988}.cell.out{border-color:#2A5F5A;background:#DCEDEA}.cell.dim{opacity:.3}
.dot{color:#C0704E}
button.b{border:0;border-radius:10px;padding:11px 14px;font-weight:600;font-size:14px;cursor:pointer;background:#FFFBF8;color:#2A5F5A}
button.p{background:#2A5F5A;color:#fff}button.r{background:#C0504E;color:#fff}
.row{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}.row>*{flex:1}
.banner{background:#2A5F5A;color:#fff;border-radius:16px;padding:18px;margin-bottom:14px;text-align:center;font-size:17px}
.banner button{margin-top:12px;font-size:18px;padding:14px 28px}
.jog{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;max-width:330px;margin:12px auto 0}
.jog button{padding:18px 0;font-size:18px}
.read{font:14px monospace;display:grid;grid-template-columns:repeat(2,1fr);gap:6px}
.seg{display:flex;gap:6px}.seg button{flex:1}.seg button.on{background:#2A5F5A;color:#fff}
h3{margin:0 0 10px;font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:#9C8576}
@media(max-width:560px){.grid{grid-template-columns:repeat(3,1fr)}}
</style></head><body><div class="wrap">
<div class="top"><div class="brand">Index<span>.</span></div><div class="pill" id="pill">...</div><button class="b r" onclick="cmd('a=stop')">STOP</button></div>
<div class="tabs"><button class="tab on" id="t1" onclick="tab(1)">Cabinet</button><button class="tab" id="t2" onclick="tab(2)">Setup</button></div>
<div class="msg" id="msg"></div>

<div id="v1">
 <div class="banner" id="ban" hidden><div id="bantxt"></div><button class="b" onclick="cmd('a=ret')">Return tray</button></div>
 <div class="card">
  <input id="q" placeholder="Search... (try: ear, band, drops)" oninput="render()">
  <div class="grid" id="grid"></div>
 </div>
 <div class="card" id="selcard" hidden>
  <h3 id="seltitle"></h3>
  <input id="nm" maxlength="24" placeholder="Item name">
  <div class="row"><button class="b p" onclick="go()">Retrieve</button><button class="b" onclick="saveName()">Save name</button><button class="b" onclick="markEmpty()">Mark empty</button></div>
 </div>
</div>

<div id="v2" hidden>
 <div class="card"><h3>Position</h3><div class="read" id="rd"></div>
  <div class="row"><button class="b p" onclick="cmd('a=home')">Home</button><button class="b" onclick="cmd('a=park')">Park</button><button class="b" onclick="cmd('a=release')">Release motors</button></div></div>
 <div class="card"><h3>Jog step size</h3>
  <div class="seg" id="seg"><button class="b" data-mm="10">10 mm</button><button class="b on" data-mm="1">1 mm</button><button class="b" data-mm="0.2">0.2 mm</button></div>
  <div class="jog"><div></div><button class="b" onclick="jog('z',1)">Z up</button><div></div>
   <button class="b" onclick="jog('x',-1)">X left</button><div></div><button class="b" onclick="jog('x',1)">X right</button>
   <div></div><button class="b" onclick="jog('z',-1)">Z down</button><div></div></div></div>
 <div class="card"><h3>Rack (Y) and magnet</h3>
  <div class="msg" id="yinfo"></div>
  <input type="range" id="ys" min="5" max="175" onchange="cmd('a=y&v='+this.value)">
  <div class="row"><button class="b" onclick="cmd('a=y&v=175')">Retract</button><button class="b" onclick="cmd('a=y&v='+(S?S.reach:175))">Go to reach</button><button class="b p" onclick="cmd('a=savereach')">Save Y as reach</button></div>
  <div class="row"><button class="b" id="magb" onclick="cmd('a=mag&v='+(S&&S.mag?0:1))">Magnet</button></div></div>
 <div class="card"><h3>Teach positions</h3>
  <select id="tc" onchange="render()"></select><div class="msg" id="tcinfo" style="margin-top:8px"></div>
  <div class="row"><button class="b p" onclick="cmd('a=save&c='+tc())">Save position here</button><button class="b" onclick="cmd('a=goto&c='+tc())">Go to it</button></div>
  <div class="row"><button class="b" onclick="cmd('a=fill')">Fill grid from 1, 5, 11</button></div>
  <div class="row"><button class="b p" onclick="cmd('a=savedel')">Save delivery point here</button><button class="b" onclick="cmd('a=gotodel')">Go to delivery</button></div></div>
</div>
</div>
<script>
let S=null,sel=0,mm=1,lastCmd=0;const SPM=5;
const el=id=>document.getElementById(id);
const tc=()=>el('tc').value;
function tab(n){el('v1').hidden=n!=1;el('v2').hidden=n!=2;el('t1').classList.toggle('on',n==1);el('t2').classList.toggle('on',n==2);}
document.querySelectorAll('#seg button').forEach(b=>b.onclick=()=>{mm=parseFloat(b.dataset.mm);document.querySelectorAll('#seg button').forEach(x=>x.classList.toggle('on',x==b));});
for(let i=1;i<=15;i++){
 const o=document.createElement('option');o.value=i;o.textContent='Compartment '+i;el('tc').appendChild(o);
 const d=document.createElement('div');d.className='cell';d.id='c'+i;
 d.innerHTML='<div class="n">'+String(i).padStart(2,'0')+' <span class="dot"></span></div><div class="i"></div>';
 d.onclick=()=>pick(i);el('grid').appendChild(d);
}
async function cmd(q){try{const t=await (await fetch('/cmd?'+q)).text();el('msg').textContent=t;lastCmd=Date.now();poll();return t;}catch(e){el('msg').textContent='no link';}}
function jog(ax,s){cmd('a=jog&axis='+ax+'&d='+(s*Math.max(1,Math.round(mm*SPM))));}
function pick(i){sel=i;el('selcard').hidden=false;el('seltitle').textContent='Compartment '+i;el('nm').value=S?S.names[i-1]:'';render();}
function go(){if(sel)cmd('a=go&c='+sel);}
function saveName(){if(sel)cmd('a=name&c='+sel+'&v='+encodeURIComponent(el('nm').value));}
function markEmpty(){if(sel){el('nm').value='';cmd('a=name&c='+sel+'&v=');}}
function render(){
 if(!S)return;
 const idle=(S.mode=='idle'||S.mode=='at delivery');
 el('pill').textContent=S.mode.toUpperCase()+(S.homed?'':' - not homed');
 el('pill').className='pill'+(S.mode=='stopped'?' err':idle?'':' busy');
 if(Date.now()-lastCmd>3000)el('msg').textContent=S.msg;
 const q=el('q').value.trim().toLowerCase();
 for(let i=1;i<=15;i++){
  const c=el('c'+i),n=S.names[i-1];
  c.querySelector('.i').textContent=n||'empty';
  c.querySelector('.dot').textContent=S.taught[i-1]?'':'- not taught';
  c.className='cell'+(n?'':' empty')+(i==sel?' sel':'')+(i==S.active?' out':'')+(q&&!n.toLowerCase().includes(q)?' dim':'');
 }
 el('ban').hidden=(S.mode!='at delivery');
 if(S.active)el('bantxt').textContent='Compartment '+S.active+' ('+S.names[S.active-1]+') is ready. Take your item, then press Return.';
 el('rd').innerHTML='<div>X '+S.x+' ('+(S.x/SPM).toFixed(1)+' mm)</div><div>Z '+S.z+' ('+(S.z/SPM).toFixed(1)+' mm)</div>'+
  '<div>'+(S.homed?'Homed':'NOT homed')+'</div><div>Delivery '+(S.dset?'set':'not set')+'</div>';
 el('yinfo').textContent='Rack now '+S.y+' deg   |   reach '+S.reach+' deg   |   175 = retracted, lower = extended';
 if(document.activeElement!==el('ys'))el('ys').value=S.y;
 el('magb').textContent=S.mag?'Magnet ON (tap to turn off)':'Magnet off (tap to turn on)';
 el('magb').className='b'+(S.mag?' r':'');
 const t=tc(),p=S.pos[t-1];
 el('tcinfo').textContent=S.taught[t-1]?('Saved at X '+p[0]+'  Z '+p[1]):'Not taught yet';
}
async function poll(){try{S=await (await fetch('/status')).json();render();}catch(e){el('pill').textContent='NO LINK';}}
setInterval(poll,500);poll();
</script></body></html>
)rawliteral";

String jsonEsc(const String& s) {
  String o = "";
  for (unsigned int i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
    else if ((uint8_t)ch < 0x20) o += ' ';
    else o += ch;
  }
  return o;
}

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleStatus() {
  String j;
  j.reserve(1400);
  j = "{\"mode\":\"";  j += mode;
  j += "\",\"msg\":\""; j += jsonEsc(lastMsg);
  j += "\",\"homed\":"; j += homed ? "true" : "false";
  j += ",\"x\":";       j += String(stepX.currentPosition());
  j += ",\"z\":";       j += String(stepZ.currentPosition());
  j += ",\"y\":";       j += String(yAngle);
  j += ",\"mag\":";     j += magOn ? "true" : "false";
  j += ",\"active\":";  j += String(activeComp);
  j += ",\"reach\":";   j += String(reachAngle);
  j += ",\"dset\":";    j += deliv.set ? "true" : "false";
  j += ",\"taught\":[";
  for (int i = 0; i < 15; i++) { if (i) j += ","; j += comp[i].set ? "1" : "0"; }
  j += "],\"pos\":[";
  for (int i = 0; i < 15; i++) { if (i) j += ","; j += "["; j += String(comp[i].x); j += ","; j += String(comp[i].z); j += "]"; }
  j += "],\"names\":[";
  for (int i = 0; i < 15; i++) { if (i) j += ","; j += "\""; j += jsonEsc(names[i]); j += "\""; }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleCmd() {
  String a = server.arg("a");
  int  c = server.arg("c").toInt();
  long v = server.arg("v").toInt();
  String r;
  if      (a == "go")        r = cmdRetrieve(c);
  else if (a == "ret")       r = cmdReturn();
  else if (a == "home")      r = cmdHome();
  else if (a == "park")      r = cmdPark();
  else if (a == "release")   r = cmdRelease();
  else if (a == "stop")      r = cmdStop();
  else if (a == "jog")       r = cmdJog(server.arg("axis"), server.arg("d").toInt());
  else if (a == "y")         r = cmdY(v);
  else if (a == "mag")       r = cmdMag(v != 0);
  else if (a == "save")      r = cmdSave(c);
  else if (a == "goto")      r = cmdGoto(c);
  else if (a == "fill")      r = cmdFill();
  else if (a == "savedel")   r = cmdSaveDeliv();
  else if (a == "gotodel")   r = cmdGotoDeliv();
  else if (a == "savereach") r = cmdSaveReach();
  else if (a == "name")      r = cmdName(c, server.arg("v"));
  else                       r = "ERR unknown command";
  server.send(200, "text/plain", r);
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200);

  pinMode(MAG_PIN, OUTPUT);
  setMag(false);

  initSw(swXMin);
  initSw(swXMax);
  initSw(swZMin);

  stepX.setMaxSpeed(X_SPEED);
  stepX.setAcceleration(X_ACCEL);
  stepZ.setMaxSpeed(Z_SPEED);
  stepZ.setAcceleration(Z_ACCEL);
  stepX.disableOutputs();
  stepZ.disableOutputs();

  ESP32PWM::allocateTimer(0);
  yServo.setPeriodHertz(50);
  yServo.attach(SERVO_PIN, 500, 2400);
  yServo.write(Y_RETRACT);

  prefs.begin("index", false);
  loadAll();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/cmd", handleCmd);
  server.begin();

  Serial.println();
  Serial.println("=== Index v1 ===");
  Serial.print("WiFi \"Index\" / 12345678  ->  http://");
  Serial.println(WiFi.softAPIP());
}

void loop() {
  updateAll();
  server.handleClient();
  readSerial();
  updateServo();
  runSeq();

  // cool down: release motors a while after parking; re-home happens automatically next time
  if (parked && !seqActive && millis() - parkedAt > PARK_RELEASE_MS) {
    stepX.disableOutputs();
    stepZ.disableOutputs();
    parked = false;
    homed = false;
    lastMsg = "Motors released to cool down. Next command re-homes first.";
    Serial.println(">>> " + lastMsg);
  }
}
