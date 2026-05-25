  /*
 * LiFePO4 12.8V 10Ah — EKF SOC Estimator + DPS5015 Charger
 * ESP32 + DPS5015 only — no INA226, no voltage divider
 *
 * ── WIRING ───────────────────────────────────────────────────────────────
 *  Power supply  → DPS5015 INPUT
 *  DPS5015 OUT+  → [MBR1045 diode] → [SWITCH] → Battery B+
 *  DPS5015 OUT−  → Battery B−
 *
 *  MBR1045: anode → DPS OUT+,  cathode → switch → Battery B+
 *  SWITCH  : 10A SPST toggle or rocker switch
 *
 *  NOTE: Set DPS to 14.85V (14.4V + 0.45V diode drop) for full charge
 *
 *  DPS5015 CH340 serial:
 *    TXD → GPIO 16 (RX2)
 *    RXD → GPIO 17 (TX2)
 *    GND → ESP32 GND
 *
 *  BUTTONS (connect pin to GND when pressed):
 *    GPIO  4 → Start charging
 *    GPIO  5 → Stop charging
 *    GPIO 18 → Set 14.4V / 4A  (bulk CC)
 *    GPIO 19 → Set 14.4V / 2A  (absorption)
 *    GPIO 23 → Set 14.4V / 1A  (maintain)
 *
 * ── SERIAL COMMANDS ──────────────────────────────────────────────────────
 *  [s] Start charging    [x] Stop
 *  [1] 4A  [2] 2A  [3] 1A
 *  [r] Read V and I now  [i] Re-init SOC from voltage
 *
 * ── EKF MODEL ────────────────────────────────────────────────────────────
 *  States : x = [SOC, V_RC]
 *  Model  : V_terminal = OCV(SOC) - I×R0 - V_RC
 *  Current: negative = charging, positive = discharging
 *
 * ── THEVENIN PARAMETERS (from INA226 pulse test) ─────────────────────────
 *  R0 = 0.16596 Ω   R1 = 0.01674 Ω
 *  C1 = 1889.51 F   τ  = 27.35 s
 * ─────────────────────────────────────────────────────────────────────────
 */

#include <HardwareSerial.h>
#include <math.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <TFT_eSPI.h>   // TFT_eSPI library — configure User_Setup.h for ST7796S

// ── Forward declaration needed by LCD colour functions ────────────────────
enum ChargeState { CHG_IDLE, CHG_CC, CHG_CV, CHG_DONE };

// ── LCD ST7796S (SPI) ─────────────────────────────────────────────────────
// Configure in TFT_eSPI User_Setup.h:
//   #define ILI9488_DRIVER        ← your module uses ILI9488 not ST7796S
//   #define TFT_CS   5
//   #define TFT_DC   2
//   #define TFT_RST  4
//   #define TFT_MOSI 23
//   #define TFT_SCLK 18
//   #define TFT_MISO 19
//   #define TFT_BL   15
//   #define SPI_FREQUENCY  27000000
TFT_eSPI tft = TFT_eSPI();

// ── Colour palette ────────────────────────────────────────────────────────
#define COL_BG        0x0A0E      // very dark navy background
#define COL_PANEL     0x1082      // dark panel
#define COL_BORDER    0x2945      // panel border
#define COL_WHITE     0xFFFF
#define COL_CYAN      0x07FF
#define COL_GREEN     0x07E0
#define COL_YELLOW    0xFFE0
#define COL_ORANGE    0xFD20
#define COL_RED       0xF800
#define COL_LBLUE     0x867F      // light blue accent
#define COL_GRAY      0x8410
#define COL_DARKGRAY  0x4208

// LCD update timer
unsigned long lastLCDUpdate = 0;
bool lcdInitDone = false;

// ── Network ───────────────────────────────────────────────────────────────
const char* WIFI_SSID   = "C3501";
const char* WIFI_PASS   = "ACSY2303";
const char* MQTT_BROKER = "192.168.100.193";
const int   MQTT_PORT   = 1883;
const char* MQTT_TOPIC  = "lee9/data";
const char* MQTT_CLIENT = "esp32-lee9";

WiFiClient   espClient;
PubSubClient mqttClient(espClient);

// ── Serial ────────────────────────────────────────────────────────────────
#define DPS_RX_PIN  16
#define DPS_TX_PIN  17
HardwareSerial dpsSerial(2);

// ── Button pins ───────────────────────────────────────────────────────────
// Updated to avoid SPI pins used by ST7796S LCD (GPIO 5,18,19,23)
#define BUTTON_ON    25   // Start charging
#define BUTTON_OFF   26   // Stop charging
#define BUTTON_SET1  27   // Set 14.4V / 4A
#define BUTTON_SET2  32   // Set 14.4V / 2A
#define BUTTON_SET3  33   // Set 14.4V / 1A
bool lastBtn[5]  = {HIGH, HIGH, HIGH, HIGH, HIGH};
int  btnPins[5]  = {25, 26, 27, 32, 33};

// ── Battery specs ─────────────────────────────────────────────────────────
#define BATT_CAPACITY_AH   6.209f
#define BATT_CAPACITY_AS   (6.209f * 3600.0f)
#define CHARGE_V_TARGET    14.40f
#define CHARGE_V_CV        14.30f   // actual voltage DPS enters CV (measured)
#define CHARGE_I_BULK       4.00f
#define CHARGE_I_ABSORB     2.00f
#define CHARGE_I_DONE       0.31f
#define BATT_V_MIN         11.04f
#define BATT_V_FULL        13.72f   // back-calculated charging OCV at 100%

// ── Current correction factor ─────────────────────────────────────────────
// Measured: multimeter=2.009A vs DPS=2.000A → ratio=1.0045
// Within measurement accuracy — no correction needed
#define CURRENT_CORRECTION  1.0f

// ── Thevenin model parameters ─────────────────────────────────────────────
// R0 recalculated: Innov=-0.136V at 2A → R0 = 0.039Ω
#define R0   0.039f
#define R1   0.01674f
#define C1   1889.51f
// τ = R1 × C1 = 27.35s

// ── EKF noise tuning ──────────────────────────────────────────────────────
// R_MEAS = 0.005 was too aggressive — caused SOC jumps from voltage pull
// Increased to 0.04 to balance coulomb counting vs voltage correction
// Innovation target: ±0.03V throughout charge cycle
#define EKF_Q_SOC   0.000001f
#define EKF_Q_VRC   0.00005f
// R_MEAS increased to 0.5 — strongly prefer coulomb counting over voltage
// EKF_R_MEAS: double counting now fixed so voltage can help correct SOC
// 0.10 = moderate trust in voltage — reduces innovation drift
// Lower = more voltage correction, Higher = more coulomb counting
#define EKF_R_MEAS  0.10f

// ── Hardware voltage drop correction ─────────────────────────────────────
// ── Hardware voltage drop correction ─────────────────────────────────────
// TOTAL drop = diode + wire resistance
// NEW measurement: V_dps=13.81V, multimeter=13.25V at 2A
//   Actual drop = 13.81 - 13.25 = 0.56V
//   Diode at 2A = 0.540V
//   Wire drop   = 0.56 - 0.54 = 0.020V → R_wire = 0.010Ω
// Previous R_wire = 0.110Ω was wrong — overcorrecting by 0.20V
#define WIRE_RESISTANCE  0.010f   // updated from new measurement

float getTotalDrop(float current_A) {
  float i = fabsf(current_A);

  // Diode drop — measured values
  float diode_drop = 0.0f;
  if      (i <= 0.0f) diode_drop = 0.35f;
  else if (i <= 0.3f) diode_drop = 0.40f;
  else if (i <= 0.5f) diode_drop = 0.45f;
  else if (i <= 1.0f) diode_drop = 0.482f;
  else if (i <= 2.0f) diode_drop = 0.540f;
  else                diode_drop = 0.542f;

  // Wire resistance — now measured as 0.010Ω at 2A
  float r_wire = WIRE_RESISTANCE;

  return diode_drop + i * r_wire;
}
#define DIODE_DROP    0.482f   // fallback

// ── OCV–SOC table (UPDATED — second discharge test) ──────────────────────
// Source: ToolkitRC discharge test, ~625mAh steps, 30min rest per point
// Updated to charging OCV table — measured from ToolkitRC charge test at C/10
// Using charging OCV removes need for hysteresis offset during charging
// Added intermediate points at 93% and 97% to improve high-SOC accuracy
const float ocvTable[][2] = {
  {0.00f, 11.58f},   // charging OCV — 0%
  {0.10f, 12.87f},   // charging OCV — 10%
  {0.20f, 13.08f},   // charging OCV — 20%
  {0.30f, 13.19f},   // charging OCV — 30%
  {0.40f, 13.22f},   // charging OCV — 40%
  {0.50f, 13.24f},   // charging OCV — 50%
  {0.60f, 13.26f},   // charging OCV — 60%
  {0.70f, 13.30f},   // charging OCV — 70%
  {0.80f, 13.34f},   // charging OCV — 80%
  {0.90f, 13.36f},   // charging OCV — 90%
  {0.93f, 13.42f},   // interpolated — 93%
  {0.97f, 13.52f},   // interpolated — 97%
  {1.00f, 13.72f},   // updated — back-calculated from V_batt=13.821V at 1.89A
};
const int OCV_N = sizeof(ocvTable) / sizeof(ocvTable[0]);  // = 11 points

// ── EKF state ─────────────────────────────────────────────────────────────
float ekf_SOC   = 0.5f;
float ekf_VRC   = 0.0f;
float lastInnov = 0.0f;
float ekf_P[2][2] = {{0.05f, 0.0f}, {0.0f, 0.01f}};
unsigned long lastEKFTime = 0;

// ── Charging state ────────────────────────────────────────────────────────
// ChargeState enum declared at top of file (needed by LCD functions)
ChargeState   chargeState       = CHG_IDLE;
unsigned long chargeStart       = 0;
float         totalChargeAh     = 0.0f;
float         lastKnownCurrent  = 0.0f;
float         activeChargeCurrent = CHARGE_I_BULK;

// ── Charging hysteresis offset ────────────────────────────────────────────
// From complete charge log analysis:
// Innovation at 50% SOC = +0.155V → offset needed = 0.08 + 0.155 = 0.235V
// But use 0.13V to balance — EKF will self-correct the remainder
// Too large offset causes SOC to drop, too small causes premature 100%
#define OCV_CHARGE_OFFSET  0.0f    // no offset needed — charging OCV table used

// ── Base OCV lookup (no hysteresis) ──────────────────────────────────────
float getOCV_base(float soc) {
  soc = constrain(soc, 0.0f, 1.0f);
  if (soc <= ocvTable[0][0])        return ocvTable[0][1];
  if (soc >= ocvTable[OCV_N-1][0])  return ocvTable[OCV_N-1][1];
  for (int i = 0; i < OCV_N-1; i++) {
    if (soc >= ocvTable[i][0] && soc <= ocvTable[i+1][0]) {
      float t = (soc-ocvTable[i][0]) / (ocvTable[i+1][0]-ocvTable[i][0]);
      return ocvTable[i][1] + t*(ocvTable[i+1][1]-ocvTable[i][1]);
    }
  }
  return ocvTable[OCV_N-1][1];
}

// ── OCV with charging hysteresis applied ─────────────────────────────────
float getOCV(float soc) {
  float ocv = getOCV_base(soc);
  // Add hysteresis offset during charging
  if (chargeState == CHG_CC || chargeState == CHG_CV)
    ocv += OCV_CHARGE_OFFSET;
  return ocv;
}

float dOCV_dSOC(float soc) {
  float d = 0.005f;
  return (getOCV(soc+d) - getOCV(soc-d)) / (2.0f*d);
}

float voltageToSOC(float v) {
  // Use base OCV table (no hysteresis) for resting voltage lookup
  if (v <= ocvTable[0][1])        return ocvTable[0][0];
  if (v >= ocvTable[OCV_N-1][1])  return ocvTable[OCV_N-1][0];
  for (int i = 0; i < OCV_N-1; i++) {
    float v0 = ocvTable[i][1], v1 = ocvTable[i+1][1];
    if (v >= v0 && v <= v1) {
      float t = (v-v0)/(v1-v0);
      return ocvTable[i][0] + t*(ocvTable[i+1][0]-ocvTable[i][0]);
    }
  }
  return 0.5f;
}

// ═════════════════════════════════════════════════════════════════════════
// EKF UPDATE
// voltage_V : DPS5015 output voltage (battery terminal during charging)
// current_A : negative = charging, positive = discharging
// ═════════════════════════════════════════════════════════════════════════
void ekfUpdate(float voltage_V, float current_A) {
  unsigned long now = millis();
  float dt = (now - lastEKFTime) / 1000.0f;
  lastEKFTime = now;
  if (dt <= 0 || dt > 120.0f) dt = 5.0f;

  float tau   = R1 * C1;
  float alpha = expf(-dt / tau);

  // ── PREDICT ──────────────────────────────────────────────────────────
  // IMPORTANT: SOC coulomb counting done exclusively by 5s fast counter
  // ekfUpdate must NOT also count coulombs — would cause double counting
  // soc_p = current SOC (already updated by fast counter)
  // Only V_RC is propagated here (RC dynamics still need updating)
  float soc_p = ekf_SOC;   // no coulomb counting here
  float vrc_p = ekf_VRC * alpha + current_A * R1 * (1.0f - alpha);

  float P00 = ekf_P[0][0] + EKF_Q_SOC;
  float P01 = ekf_P[0][1] * alpha;
  float P10 = ekf_P[1][0] * alpha;
  float P11 = ekf_P[1][1] * alpha*alpha + EKF_Q_VRC;

  // ── UPDATE ────────────────────────────────────────────────────────────
  float V_pred = getOCV(soc_p) - current_A * R0 - vrc_p;
  float H0     = dOCV_dSOC(soc_p);
  float H1     = -1.0f;
  float innov  = voltage_V - V_pred;

  float S  = H0*H0*P00 + H0*H1*P01 + H1*H0*P10 + H1*H1*P11 + EKF_R_MEAS;
  float K0 = (P00*H0 + P01*H1) / S;
  float K1 = (P10*H0 + P11*H1) / S;

  ekf_SOC = constrain(soc_p + K0*innov, 0.0f, 1.0f);
  ekf_VRC = constrain(vrc_p + K1*innov, -2.0f, 2.0f);

  // If innovation strongly negative and SOC at 100%
  // allow slight reduction to prevent getting stuck prematurely
  if (ekf_SOC >= 1.0f && innov < -0.05f) {
    ekf_SOC = constrain(1.0f + K0*innov, 0.90f, 1.0f);
  }

  ekf_P[0][0] = (1.0f-K0*H0)*P00 - K0*H1*P10;
  ekf_P[0][1] = (1.0f-K0*H0)*P01 - K0*H1*P11;
  ekf_P[1][0] = (-K1*H0)*P00 + (1.0f-K1*H1)*P10;
  ekf_P[1][1] = (-K1*H0)*P01 + (1.0f-K1*H1)*P11;
  // NOTE: Ah accumulation removed — handled exclusively by 5s coulomb counter
}

// ── Re-init SOC from resting voltage ─────────────────────────────────────
void reinitSOC(float voltage) {
  if (voltage < BATT_V_MIN || voltage > BATT_V_FULL + 0.5f) {
    Serial.printf("  [WARN] V=%.4fV out of range (%.2f–%.2fV) — SOC not reset.\n",
                  voltage, BATT_V_MIN, BATT_V_FULL);
    return;
  }
  ekf_SOC     = voltageToSOC(voltage);
  ekf_VRC     = 0.0f;
  ekf_P[0][0] = 0.05f;
  ekf_P[1][1] = 0.01f;
  lastEKFTime = millis();
  Serial.printf("  V = %.4fV  →  SOC = %.1f%%\n", voltage, ekf_SOC*100.0f);
  Serial.printf("  (Range: %.2fV=0%% to %.2fV=100%%)\n", BATT_V_MIN, BATT_V_FULL);
}

// ═════════════════════════════════════════════════════════════════════════
// MODBUS / DPS5015
// ═════════════════════════════════════════════════════════════════════════
uint16_t modbusCRC(uint8_t *buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 1) { crc >>= 1; crc ^= 0xA001; }
      else           crc >>= 1;
    }
  }
  return crc;
}

void sendFrame(uint8_t *f, int len) {
  // MODBUS RTU requires 3.5 char silence before new frame
  // Also flush any leftover bytes from previous response
  delay(10);
  while (dpsSerial.available()) dpsSerial.read();  // flush RX buffer
  delay(5);

  uint16_t crc = modbusCRC(f, len);
  f[len]   = crc & 0xFF;
  f[len+1] = crc >> 8;
  dpsSerial.write(f, len+2);
  dpsSerial.flush();  // wait until TX complete
}

void writeSingleReg(uint16_t reg, uint16_t val) {
  uint8_t f[8];
  f[0]=0x01; f[1]=0x06;
  f[2]=reg>>8; f[3]=reg&0xFF;
  f[4]=val>>8; f[5]=val&0xFF;
  sendFrame(f, 6);
  // DPS5015 responds slowly to write commands — wait 500ms and drain
  uint32_t t = millis() + 500;
  while (millis() < t) {
    if (dpsSerial.available()) dpsSerial.read();
  }
}

void writeMultiReg(uint16_t startReg, uint16_t *vals, uint16_t count) {
  uint8_t f[20];
  f[0]=0x01; f[1]=0x10;
  f[2]=startReg>>8; f[3]=startReg&0xFF;
  f[4]=count>>8;    f[5]=count&0xFF;
  f[6]=count*2;
  for (int i = 0; i < count; i++) {
    f[7+i*2]=vals[i]>>8; f[8+i*2]=vals[i]&0xFF;
  }
  sendFrame(f, 7+count*2);
  // DPS5015 responds slowly to write commands — wait 500ms and drain
  uint32_t t = millis() + 500;
  while (millis() < t) {
    if (dpsSerial.available()) dpsSerial.read();
  }
}

void dpsON()  { writeSingleReg(0x0009, 0x0001); }
void dpsOFF() { writeSingleReg(0x0009, 0x0000); }

void dpsSetVI(float voltage, float current) {
  uint16_t v[2] = {(uint16_t)(voltage*100), (uint16_t)(current*100)};
  writeMultiReg(0x0000, v, 2);
}

// Read UOUT(0x0002) and IOUT(0x0003) from DPS5015
bool dpsReadVI(float &outV, float &outI) {
  outV = 0; outI = 0;

  // Try up to 3 times
  for (int attempt = 0; attempt < 3; attempt++) {
    // MODBUS RTU inter-frame gap = 3.5 char times at 9600 baud = ~4ms
    // Use 10ms to be safe
    delay(10);

    // Flush any leftover bytes from previous failed attempts
    while (dpsSerial.available()) dpsSerial.read();
    delay(5);

    // Send read request for UOUT(0x0002) and IOUT(0x0003)
    uint8_t f[8];
    f[0]=0x01; f[1]=0x03;
    f[2]=0x00; f[3]=0x02;
    f[4]=0x00; f[5]=0x02;
    sendFrame(f, 6);

    // Wait for response — DPS5015 typically responds within 100ms
    // Give 300ms on first attempt, 500ms on retries
    uint32_t waitMs = (attempt == 0) ? 300 : 500;
    uint32_t deadline = millis() + waitMs;

    while (millis() < deadline && !dpsSerial.available()) {
      delay(5);
    }

    if (!dpsSerial.available()) {
      continue;   // no response — retry
    }

    // Read all available bytes
    uint8_t buf[20]; int len = 0;
    uint32_t readDeadline = millis() + 50;
    while (millis() < readDeadline && len < 20) {
      if (dpsSerial.available()) {
        buf[len++] = dpsSerial.read();
      }
    }

    // Validate response: addr=0x01, func=0x03, bytecount=0x04
    if (len >= 7 && buf[0]==0x01 && buf[1]==0x03 && buf[2]==0x04) {
      outV = ((buf[3]<<8)|buf[4]) / 100.0f;
      outI = ((buf[5]<<8)|buf[6]) / 100.0f;
      return true;
    }
  }
  return false;   // all 3 attempts failed
}

// ── Charger state machine ─────────────────────────────────────────────────
void updateCharger(float batt_V, float dps_V, float chg_I) {
  switch (chargeState) {
    case CHG_CC:
      // Detect CC→CV using two methods:
      // 1. DPS voltage reaches 14.30V (measured transition point)
      // 2. Current drops below 85% of SELECTED current (not hardcoded 4A)
      if (dps_V >= CHARGE_V_CV ||
         (chg_I < activeChargeCurrent * 0.85f && dps_V > 13.80f)) {
        chargeState = CHG_CV;
        Serial.println("  [CHARGER] CC → CV transition");
        Serial.printf ("  V_dps=%.2fV  I=%.3fA\n", dps_V, chg_I);
      }
      break;
    case CHG_CV:
      if (chg_I <= CHARGE_I_DONE) {
        chargeState = CHG_DONE;
        dpsOFF();
        unsigned long elapsed = (millis()-chargeStart)/1000;
        Serial.println("\n  ╔══════════════════════════════════╗");
        Serial.println("  ║      CHARGING COMPLETE           ║");
        Serial.printf ("  ║  SOC     : %5.1f%%               ║\n", ekf_SOC*100);
        Serial.printf ("  ║  Charged : %5.3f Ah             ║\n", totalChargeAh);
        Serial.printf ("  ║  Time    : %lu s               ║\n", elapsed);
        Serial.println("  ╚══════════════════════════════════╝");
      }
      break;
    default: break;
  }
}

// ── Print status ──────────────────────────────────────────────────────────
void printStatus(float batt_V, float dps_V, float dps_I, float current_A) {
  const char* st[] = {"IDLE","CC  ","CV  ","DONE"};
  char bar[21]; memset(bar,'-',20); bar[20]='\0';
  int filled = (int)(ekf_SOC * 20.0f);
  for (int i=0; i<filled; i++) bar[i]='#';

  Serial.println("─────────────────────────────────────────────────");
  Serial.printf("  V_dps   : %.4fV  (DPS output, before diode)\n", dps_V);
  Serial.printf("  V_batt  : %.4fV  (after %.3fV total drop)\n",
                batt_V, dps_V - batt_V);
  Serial.printf("  I_dps   : %.4fA  I_actual: %+.4fA (×%.3f)\n",
                dps_I, current_A, CURRENT_CORRECTION);
  Serial.printf("  V_RC    : %.5fV  (RC transient)\n",             ekf_VRC);
  Serial.printf("  OCV_est : %.4fV  (EKF model)\n",               getOCV(ekf_SOC));
  Serial.printf("  Innov   : %.4fV  (near 0 = good fit)\n",
                batt_V - (getOCV(ekf_SOC) - current_A*R0 - ekf_VRC));
  Serial.printf("  SOC     : %5.1f%%  [%s]\n",                    ekf_SOC*100.0f, bar);
  Serial.printf("  Charger : %s   Charged: %.3f Ah",
                st[chargeState], totalChargeAh);
  if (totalChargeAh > BATT_CAPACITY_AH * 1.1f)
    Serial.print("  [WARN: exceeds capacity!]");
  Serial.println();
  Serial.println("  (60s update — buttons/serial instant)");
  Serial.println("─────────────────────────────────────────────────");
}

// ═════════════════════════════════════════════════════════════════════════
// LCD DASHBOARD
// Screen: 320×480 (landscape: 480×320)
// Layout:
//   Top bar     : title + mode
//   Left panel  : SOC% large + charging bar
//   Right panel : V_batt, I_chg, V_dps, Charged Ah
//   Bottom bar  : status + innovation
// ═════════════════════════════════════════════════════════════════════════

// ── Draw static background (called once) ─────────────────────────────────
void lcdDrawBackground() {
  tft.fillScreen(COL_BG);

  // Top bar
  tft.fillRoundRect(4, 4, 472, 44, 6, COL_PANEL);
  tft.drawRoundRect(4, 4, 472, 44, 6, COL_BORDER);

  // Left panel — SOC
  tft.fillRoundRect(4, 54, 220, 210, 6, COL_PANEL);
  tft.drawRoundRect(4, 54, 220, 210, 6, COL_BORDER);

  // Right panel — measurements
  tft.fillRoundRect(232, 54, 244, 210, 6, COL_PANEL);
  tft.drawRoundRect(232, 54, 244, 210, 6, COL_BORDER);

  // Bottom panel — status
  tft.fillRoundRect(4, 272, 472, 44, 6, COL_PANEL);
  tft.drawRoundRect(4, 272, 472, 44, 6, COL_BORDER);

  // Static labels
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_GRAY, COL_PANEL);
  tft.setTextSize(1);

  // Right panel labels
  tft.drawString("V BATTERY", 240, 64);
  tft.drawString("CURRENT",   240, 118);
  tft.drawString("DPS OUTPUT",240, 172);
  tft.drawString("CHARGED",   240, 226);

  // SOC label
  tft.setTextDatum(TC_DATUM);
  tft.drawString("STATE OF CHARGE", 114, 62);
}

// ── Get SOC bar colour based on percentage ────────────────────────────────
uint16_t socBarColour(float soc) {
  if (soc > 0.60f) return COL_GREEN;
  if (soc > 0.30f) return COL_YELLOW;
  if (soc > 0.15f) return COL_ORANGE;
  return COL_RED;
}

// ── Get mode colour ───────────────────────────────────────────────────────
uint16_t modeColour(ChargeState s) {
  switch(s) {
    case CHG_CC:   return COL_CYAN;
    case CHG_CV:   return COL_YELLOW;
    case CHG_DONE: return COL_GREEN;
    default:       return COL_GRAY;
  }
}

const char* modeString(ChargeState s) {
  switch(s) { case CHG_CC: return "CC"; case CHG_CV: return "CV";
              case CHG_DONE: return "DONE"; default: return "IDLE"; }
}

// ── Update top title bar ──────────────────────────────────────────────────
void lcdUpdateTitle() {
  // Title
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_LBLUE, COL_PANEL);
  tft.setTextSize(2);
  tft.drawString("LiFePO4 CHARGER", 12, 26);

  // Mode badge
  uint16_t mc = modeColour(chargeState);
  tft.fillRoundRect(360, 10, 110, 32, 4, mc);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_BG, mc);
  tft.setTextSize(2);
  tft.drawString(modeString(chargeState), 415, 26);
}

// ── Update SOC panel ──────────────────────────────────────────────────────
void lcdUpdateSOC() {
  float soc = ekf_SOC;
  uint16_t barCol = socBarColour(soc);

  // Large SOC percentage
  char buf[8];
  sprintf(buf, "%4.1f%%", soc * 100.0f);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(barCol, COL_PANEL);
  tft.setTextSize(4);
  tft.drawString(buf, 114, 125);

  // Charging bar background
  int barX = 16, barY = 170, barW = 196, barH = 28;
  tft.fillRoundRect(barX, barY, barW, barH, 4, COL_DARKGRAY);

  // Charging bar fill
  int fillW = (int)(soc * barW);
  if (fillW > 0)
    tft.fillRoundRect(barX, barY, fillW, barH, 4, barCol);

  // Bar border
  tft.drawRoundRect(barX, barY, barW, barH, 4, COL_BORDER);

  // Bar percentage text inside
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_WHITE, COL_DARKGRAY);
  tft.setTextSize(1);
  sprintf(buf, "%d%%", (int)(soc * 100));
  tft.drawString(buf, barX + barW/2, barY + barH/2);

  // Charged Ah small label under bar
  char ahbuf[20];
  sprintf(ahbuf, "%.3f Ah charged", totalChargeAh);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_GRAY, COL_PANEL);
  tft.setTextSize(1);
  tft.drawString(ahbuf, 114, 215);
}

// ── Update right measurements panel ──────────────────────────────────────
void lcdUpdateMeasurements(float batt_V, float dps_V,
                            float dps_I, float current_A) {
  char buf[20];
  int lx = 240;   // label x
  int vx = 468;   // value x (right aligned)

  // V_batt
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_GRAY, COL_PANEL);
  tft.setTextSize(1);
  tft.drawString("V BATTERY", lx, 64);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_CYAN, COL_PANEL);
  tft.setTextSize(2);
  sprintf(buf, "%.3fV", batt_V);
  tft.drawString(buf, vx, 76);

  // I_chg
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_GRAY, COL_PANEL);
  tft.setTextSize(1);
  tft.drawString("CURRENT", lx, 118);
  tft.setTextDatum(TR_DATUM);
  uint16_t iCol = (fabsf(current_A) > 0.1f) ? COL_YELLOW : COL_GRAY;
  tft.setTextColor(iCol, COL_PANEL);
  tft.setTextSize(2);
  sprintf(buf, "%.2fA", fabsf(current_A));
  tft.drawString(buf, vx, 130);

  // V_dps
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_GRAY, COL_PANEL);
  tft.setTextSize(1);
  tft.drawString("DPS OUTPUT", lx, 172);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_LBLUE, COL_PANEL);
  tft.setTextSize(2);
  sprintf(buf, "%.2fV", dps_V);
  tft.drawString(buf, vx, 184);

  // Charged Ah
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_GRAY, COL_PANEL);
  tft.setTextSize(1);
  tft.drawString("CHARGED", lx, 226);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_GREEN, COL_PANEL);
  tft.setTextSize(2);
  sprintf(buf, "%.3fAh", totalChargeAh);
  tft.drawString(buf, vx, 238);
}

// ── Update bottom status bar ──────────────────────────────────────────────
void lcdUpdateStatus(float innov) {
  // Innovation indicator
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_GRAY, COL_PANEL);
  tft.setTextSize(1);
  tft.drawString("INNOV:", 12, 294);

  char buf[16];
  sprintf(buf, "%+.3fV", innov);
  uint16_t innovCol = (fabsf(innov) < 0.05f) ? COL_GREEN :
                      (fabsf(innov) < 0.15f) ? COL_YELLOW : COL_RED;
  tft.setTextColor(innovCol, COL_PANEL);
  tft.drawString(buf, 60, 294);

  // OCV_est
  tft.setTextColor(COL_GRAY, COL_PANEL);
  tft.drawString("OCV:", 160, 294);
  sprintf(buf, "%.3fV", getOCV(ekf_SOC));
  tft.setTextColor(COL_LBLUE, COL_PANEL);
  tft.drawString(buf, 196, 294);

  // Capacity bar (right side of status)
  tft.setTextColor(COL_GRAY, COL_PANEL);
  tft.drawString("CAP:", 310, 294);
  sprintf(buf, "%.1f%%", (totalChargeAh / BATT_CAPACITY_AH) * 100.0f);
  tft.setTextColor(COL_WHITE, COL_PANEL);
  tft.drawString(buf, 342, 294);
}

// ── Main LCD refresh (call every 2 seconds) ───────────────────────────────
void lcdUpdate(float batt_V, float dps_V, float dps_I,
               float current_A, float innov) {
  if (!lcdInitDone) {
    lcdDrawBackground();
    lcdInitDone = true;
  }
  lcdUpdateTitle();
  lcdUpdateSOC();
  lcdUpdateMeasurements(batt_V, dps_V, dps_I, current_A);
  lcdUpdateStatus(innov);
}

// ═════════════════════════════════════════════════════════════════════════
// WIFI SETUP
// ═════════════════════════════════════════════════════════════════════════
void setupWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WiFi] Connecting");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WiFi] Failed — continuing without network");
  }
}

// ── MQTT — non-blocking reconnect ────────────────────────────────────────
void ensureMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;
  static unsigned long lastAttempt = 0;
  if (millis() - lastAttempt < 5000) return;
  lastAttempt = millis();
  Serial.print("[MQTT] Connecting...");
  if (mqttClient.connect(MQTT_CLIENT)) {
    Serial.println("connected");
  } else {
    Serial.print("failed rc="); Serial.println(mqttClient.state());
  }
}

// ── MQTT PUBLISH ──────────────────────────────────────────────────────────
void publishData(float dps_V, float batt_V, float dps_I, float current_A) {
  if (!mqttClient.connected()) return;

  const char* mode = "---";
  switch (chargeState) {
    case CHG_CC:   mode = "CC";   break;
    case CHG_CV:   mode = "CV";   break;
    case CHG_DONE: mode = "DONE"; break;
    default:                      break;
  }

  char buf[256];
  snprintf(buf, sizeof(buf),
    "{\"v_dps\":%.4f,\"v_batt\":%.4f,\"i_dps\":%.4f,\"i_actual\":%.4f,"
    "\"v_rc\":%.5f,\"ocv_est\":%.4f,\"innov\":%.4f,\"soc\":%.2f,"
    "\"charger\":\"%s\",\"charged_ah\":%.4f}",
    dps_V, batt_V, dps_I, current_A,
    ekf_VRC, getOCV(ekf_SOC), lastInnov, ekf_SOC * 100.0f,
    mode, totalChargeAh
  );

  mqttClient.publish(MQTT_TOPIC, buf);
  Serial.printf("[MQTT] Published → SOC=%.1f%%  Mode=%s\n",
                ekf_SOC * 100.0f, mode);
}

// ═════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  dpsSerial.begin(9600, SERIAL_8N1, DPS_RX_PIN, DPS_TX_PIN);

  // LCD init
  tft.init();
  tft.setRotation(1);   // landscape — 480×320
  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_LBLUE, COL_BG);
  tft.setTextSize(2);
  tft.drawString("LiFePO4 Charger", 240, 140);
  tft.setTextSize(1);
  tft.setTextColor(COL_GRAY, COL_BG);
  tft.drawString("Initialising...", 240, 170);
  pinMode(15, OUTPUT); digitalWrite(15, HIGH);  // backlight ON
  for (int i=0; i<5; i++) pinMode(btnPins[i], INPUT_PULLUP);
  delay(2000);

  Serial.println("══════════════════════════════════════════════════");
  Serial.println("  LiFePO4 EKF SOC + DPS5015 — No INA226");
  Serial.printf ("  R0=%.5fΩ R1=%.5fΩ C1=%.2fF τ=%.2fs\n",
                  R0, R1, C1, R1*C1);
  Serial.printf ("  Capacity: %.1fAh\n", BATT_CAPACITY_AH);
  Serial.println("══════════════════════════════════════════════════");
  Serial.println("  SERIAL: [s] Start  [x] Stop  [1/2/3] Current");
  Serial.println("          [r] Read   [i] Re-init SOC");
  Serial.println("══════════════════════════════════════════════════");

  // Read resting voltage from DPS5015 to initialise SOC
  Serial.println("  Reading resting voltage from DPS5015...");
  float vSum = 0; int vCount = 0;
  for (int attempt = 0; attempt < 5; attempt++) {
    float v = 0, i = 0;
    if (dpsReadVI(v, i) && v > 10.0f && v < 15.0f) {
      vSum += v; vCount++;
      Serial.printf("  Sample %d: %.4fV\n", attempt+1, v);
    }
    delay(300);
  }

  if (vCount > 0) {
    float vAvg = vSum / vCount;
    // At boot DPS output is OFF so current = 0, use idle drop
    float vCorrected = vAvg - getTotalDrop(0.0f);
    Serial.printf("  DPS voltage  : %.4fV\n", vAvg);
    Serial.printf("  Batt voltage : %.4fV  (corrected)\n", vCorrected);
    reinitSOC(vCorrected);
  } else {
    ekf_SOC = 0.50f;
    ekf_P[0][0] = 0.25f;
    Serial.println("  [WARN] No DPS response — SOC defaulted to 50%");
    Serial.println("         Type [i] after connecting DPS to re-init.");
  }

  lastEKFTime = millis();
  Serial.println("  Ready.\n");

  setupWiFi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(256);
}

// ═════════════════════════════════════════════════════════════════════════
void loop() {
  ensureMQTT();
  mqttClient.loop();

  float dps_V = 0, dps_I = 0;

  // ── Serial commands ───────────────────────────────────────────────────
  if (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
      case 's': case 'S':
        if (chargeState == CHG_IDLE || chargeState == CHG_DONE) {
          chargeState   = CHG_CC;
          chargeStart   = millis();
          totalChargeAh = 0.0f;
          Serial.println(">>> Setting voltage and current...");
          dpsSetVI(CHARGE_V_TARGET, activeChargeCurrent);
          delay(200);
          Serial.println(">>> Turning DPS output ON...");
          dpsON();
          delay(200);
          float v=0, i=0;
          if (dpsReadVI(v, i))
            Serial.printf(">>> DPS confirmed: V=%.2fV I=%.2fA\n", v, i);
          else
            Serial.println(">>> [WARN] DPS did not respond after ON command");
          Serial.printf(">>> Charging started — CC 14.4V / %.0fA\n",
                        activeChargeCurrent);
        } else {
          Serial.println(">>> Already charging. [x] to stop first.");
        }
        break;
      case 'x': case 'X':
        dpsOFF();
        chargeState      = CHG_IDLE;
        lastKnownCurrent = 0.0f;   // reset — prevent stale coulomb counting
        Serial.println(">>> Charging stopped.");
        break;
      case '1':
        activeChargeCurrent = 4.0f;
        dpsSetVI(14.4f, 4.0f);
        Serial.println(">>> Set 14.4V / 4A");
        break;
      case '2':
        activeChargeCurrent = 2.0f;
        dpsSetVI(14.4f, 2.0f);
        Serial.println(">>> Set 14.4V / 2A");
        break;
      case '3':
        activeChargeCurrent = 1.0f;
        dpsSetVI(14.4f, 1.0f);
        Serial.println(">>> Set 14.4V / 1A");
        break;
      case 'r': case 'R': {
        float v=0, i=0;
        dpsReadVI(v, i);
        float v_corrected = v - getTotalDrop(i);
        Serial.printf(">>> DPS=%.4fV  Batt=%.4fV  I=%.4fA  SOC=%.1f%%\n",
                      v, v_corrected, i, ekf_SOC*100.0f);
        break;
      }
      case 'i': case 'I': {
        float v=0, i=0;
        Serial.println(">>> Re-initialising SOC...");
        if (dpsReadVI(v, i) && v > 10.0f) {
          float v_corrected = v - getTotalDrop(i);
          reinitSOC(v_corrected);
        } else Serial.println(">>> [WARN] No DPS reading.");
        break;
      }
    }
  }

  // ── Button handling with debounce ─────────────────────────────────────
  static unsigned long btnDebounce[5] = {0,0,0,0,0};
  for (int i=0; i<5; i++) {
    bool cur = digitalRead(btnPins[i]);
    if (lastBtn[i]==HIGH && cur==LOW && millis()-btnDebounce[i]>50) {
      btnDebounce[i] = millis();
      switch (i) {
        case 0:
          if (chargeState==CHG_IDLE || chargeState==CHG_DONE) {
            chargeState=CHG_CC; chargeStart=millis(); totalChargeAh=0.0f;
            Serial.println(">>> [BTN1] Setting VI...");
            dpsSetVI(CHARGE_V_TARGET, activeChargeCurrent);
            delay(200);
            Serial.println(">>> [BTN1] Turning ON...");
            dpsON();
            delay(200);
            float v=0, i=0;
            if (dpsReadVI(v, i))
              Serial.printf(">>> [BTN1] DPS confirmed: V=%.2fV I=%.2fA\n", v, i);
            else
              Serial.println(">>> [BTN1] DPS no response after ON");
            Serial.printf(">>> [BTN1] Charging started — CC 14.4V / %.0fA\n",
                          activeChargeCurrent);
          }
          break;
        case 1:
          dpsOFF();
          chargeState      = CHG_IDLE;
          lastKnownCurrent = 0.0f;
          Serial.println(">>> [BTN2] Charging stopped.");
          break;
        case 2:
          activeChargeCurrent = 4.0f;
          dpsSetVI(14.4f, 4.0f);
          Serial.println(">>> [BTN3] 14.4V/4A");
          break;
        case 3:
          activeChargeCurrent = 2.0f;
          dpsSetVI(14.4f, 2.0f);
          Serial.println(">>> [BTN4] 14.4V/2A");
          break;
        case 4:
          activeChargeCurrent = 1.0f;
          dpsSetVI(14.4f, 1.0f);
          Serial.println(">>> [BTN5] 14.4V/1A");
          break;
      }
    }
    lastBtn[i] = cur;
  }

  // ── Fast coulomb counting every 5 seconds (no DPS poll) ──────────────
  static unsigned long lastCoulomb = 0;
  if (millis()-lastCoulomb >= 5000) {
    lastCoulomb = millis();
    if ((chargeState==CHG_CC || chargeState==CHG_CV)
        && fabsf(lastKnownCurrent) > 0.1f
        && fabsf(lastKnownCurrent) < 5.0f) {
      float dt    = 5.0f;
      float alpha = expf(-dt / (R1*C1));
      ekf_SOC = constrain(
        ekf_SOC - (lastKnownCurrent*dt) / BATT_CAPACITY_AS, 0.0f, 1.0f);
      ekf_VRC = ekf_VRC*alpha + lastKnownCurrent*R1*(1.0f-alpha);
      totalChargeAh += fabsf(lastKnownCurrent) * dt / 3600.0f;
    }
  }

  // ── Fast CC→CV check every 15 seconds ────────────────────────────────
  static unsigned long lastCVcheck = 0;
  if (chargeState == CHG_CC && millis()-lastCVcheck >= 15000) {
    lastCVcheck = millis();
    float v = 0, i = 0;
    // Single attempt only — do not block buttons with retries
    while (dpsSerial.available()) dpsSerial.read();
    uint8_t f[8];
    f[0]=0x01; f[1]=0x03; f[2]=0x00; f[3]=0x02; f[4]=0x00; f[5]=0x02;
    uint16_t crc = 0xFFFF;
    for (int b=0; b<6; b++) { crc ^= f[b]; for (int j=0;j<8;j++) { if(crc&1){crc>>=1;crc^=0xA001;}else crc>>=1; } }
    f[6]=crc&0xFF; f[7]=crc>>8;
    dpsSerial.write(f, 8);
    delay(200);   // short wait only
    if (dpsSerial.available() >= 7) {
      uint8_t buf[10]; int len=0;
      while (dpsSerial.available() && len<10) buf[len++]=dpsSerial.read();
      if (len>=7 && buf[0]==0x01 && buf[1]==0x03) {
        v = ((buf[3]<<8)|buf[4]) / 100.0f;
        i = ((buf[5]<<8)|buf[6]) / 100.0f;
        if (v >= CHARGE_V_CV ||
           (i < activeChargeCurrent * 0.85f && v > 13.80f)) {
          chargeState = CHG_CV;
          lastKnownCurrent = -fabsf(i) * CURRENT_CORRECTION;
          Serial.println("  [CHARGER] CC → CV transition detected");
          Serial.printf ("  V_dps=%.2fV  I=%.3fA\n", v, i);
        }
      }
    }
  }

  // ── CV current refresh every 15 seconds ──────────────────────────────
  static unsigned long lastCVpoll = 0;
  if (chargeState == CHG_CV && millis()-lastCVpoll >= 15000) {
    lastCVpoll = millis();
    float v = 0, i = 0;
    if (dpsReadVI(v, i) && i >= 0.0f && i < 5.0f) {
      lastKnownCurrent = -fabsf(i) * CURRENT_CORRECTION;
      Serial.printf("  [CV] I=%.3fA\n", i);
      // Refresh SOC bar on LCD during CV taper
      lcdUpdateSOC();
      lcdUpdateTitle();
      if (i <= CHARGE_I_DONE) {
        chargeState = CHG_DONE;
        dpsOFF();
        unsigned long elapsed = (millis()-chargeStart)/1000;
        Serial.println("\n  ╔══════════════════════════════════╗");
        Serial.println("  ║      CHARGING COMPLETE           ║");
        Serial.printf ("  ║  SOC     : %5.1f%%               ║\n", ekf_SOC*100);
        Serial.printf ("  ║  Charged : %5.3f Ah             ║\n", totalChargeAh);
        Serial.printf ("  ║  Time    : %lu s               ║\n", elapsed);
        Serial.println("  ╚══════════════════════════════════╝");
      }
    }
  }

  // ── Full EKF update + DPS poll every 60 seconds ───────────────────────
  static unsigned long lastUpdate = 0;
  if (millis()-lastUpdate >= 60000) {
    lastUpdate = millis();

    bool got = dpsReadVI(dps_V, dps_I);

    // Only process if DPS is responding with valid voltage
    if (!got || dps_V < 10.0f) {
      static int failCount = 0;
      failCount++;
      if (failCount == 1 || failCount % 5 == 0) {
        Serial.printf("  [WARN] DPS5015 not responding (%d consecutive). Check wiring.\n",
                      failCount);
      }
    } else {
      static int failCount = 0;
      failCount = 0;   // reset on success
      // Correct for MBR1045 diode drop — dynamic based on actual current
      float diode_drop       = getTotalDrop(dps_I);
      float batt_V_corrected = dps_V - diode_drop;

      float current_A = 0.0f;
      if (chargeState==CHG_CC || chargeState==CHG_CV) {
        // Apply correction factor if DPS overreads current
        current_A        = -fabsf(dps_I) * CURRENT_CORRECTION;
        lastKnownCurrent = current_A;
      } else {
        lastKnownCurrent = 0.0f;
      }

      if (batt_V_corrected > 10.0f && batt_V_corrected < 15.0f)
        ekfUpdate(batt_V_corrected, current_A);

      updateCharger(batt_V_corrected, dps_V, dps_I);
      printStatus(batt_V_corrected, dps_V, dps_I, current_A);

      // LCD update — refresh display every 60s with full EKF data
      float innov = batt_V_corrected -
                    (getOCV(ekf_SOC) - current_A*R0 - ekf_VRC);
      lastInnov = innov;
      lcdUpdate(batt_V_corrected, dps_V, dps_I, current_A, innov);
      publishData(dps_V, batt_V_corrected, dps_I, current_A);
    }
  }
}
