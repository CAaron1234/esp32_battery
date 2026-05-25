# ESP32 Battery Monitor Dashboard

Real-time IoT battery monitoring system with 4 components: Mosquitto MQTT broker, ESP32 microcontroller (JJ1), Express backend, and Vue 3 frontend. Uses an Extended Kalman Filter (EKF) on the ESP32 to estimate State of Charge (SOC) for a 12.8V 10Ah LiPo battery via a DPS5015 power supply.

## Architecture

```
ESP32 (JJ1/JJ1.ino)
  │ reads DPS voltage/current via Modbus RTU (UART2)
  │ runs EKF → computes SOC, OCV_est, Innov, V_RC
  │ publishes JSON to MQTT topic "jj1/data"
  ▼
Mosquitto Broker (192.168.100.193:1883)
  │ subscribed by backend
  ▼
Express Backend (port 3000)
  │ saves to SQLite (jj1_readings table)
  │ broadcasts via WebSocket
  ▼
Vue 3 Frontend (browser)
  └ 8 live charts + stat cards via WebSocket
```

## MQTT Payload — `jj1/data`

```json
{
  "v_dps":      14.3800,   // DPS output voltage (V), before diode
  "v_batt":     13.6354,   // Battery voltage (V), after diode drop
  "i_dps":       1.8600,   // DPS current (A), always positive
  "i_actual":   -1.8600,   // Signed current (negative = charging)
  "v_rc":       -0.03103,  // EKF RC transient voltage (V)
  "ocv_est":    13.5573,   // EKF estimated OCV (V)
  "innov":      -0.1018,   // EKF innovation — near 0 = good fit (V)
  "soc":         97.50,    // State of Charge (%)
  "charger":    "CC",      // Charger mode: "CC", "CV", or "---"
  "charged_ah":  0.487     // Accumulated charge this session (Ah)
}
```

## Project Structure

```
esp32_fyp/
├── CLAUDE.md
├── mosquitto.conf            # MQTT broker config (port 1883, anonymous)
├── result.jpeg               # Sample serial output showing target data format
├── backend/
│   ├── index.js              # Express + WebSocket server, MQTT subscriber
│   ├── db.js                 # SQLite schema (weight_readings + jj1_readings)
│   ├── weight.db             # SQLite database (auto-created)
│   └── package.json
├── JJ1/
│   └── JJ1.ino               # ESP32 sketch: EKF SOC + MQTT publish
├── esp32/SY/
│   └── SY.ino                # Legacy sketch (weight sensor, not active)
└── frontend/
    ├── src/
    │   ├── App.vue            # Root: dashboard layout, WebSocket, stat cards
    │   ├── components/
    │   │   ├── BatteryChart.vue   # Reusable base chart (accepts field/color/unit props)
    │   │   ├── SocChart.vue       # SOC % — cyan
    │   │   ├── VBattChart.vue     # Battery voltage — emerald
    │   │   ├── VDpsChart.vue      # DPS output voltage — amber
    │   │   ├── IDpsChart.vue      # DPS current — sky
    │   │   ├── OcvEstChart.vue    # EKF OCV estimate — rose
    │   │   ├── InnovChart.vue     # EKF innovation — orange
    │   │   ├── VRcChart.vue       # RC transient voltage — violet
    │   │   ├── ChargedChart.vue   # Accumulated Ah — lime
    │   │   └── WeightChart.vue    # Legacy (not used)
    │   ├── main.js
    │   └── style.css
    ├── index.html
    └── package.json
```

## Components

### Mosquitto Broker
- Config: `mosquitto.conf`
- Port 1883, anonymous connections allowed
- Run: `mosquitto -c mosquitto.conf`

### ESP32 (`JJ1/JJ1.ino`)
- Language: C++ (Arduino)
- Libraries: WiFi, PubSubClient (MQTT), HardwareSerial (Modbus RTU)
- Reads voltage + current from DPS5015 via UART2 (RX: GPIO16, TX: GPIO17, 9600 baud)
- Runs EKF (Thevenin model: R0=0.18Ω, R1=0.05Ω, C1=3000F) every 1 second
- 5 buttons: ON (GPIO4), OFF (GPIO5), 14V/4A (GPIO18), 14V/2A (GPIO19), 14V/3A (GPIO21)
- Pressing ON resets `charged_ah` session counter
- Reconnects WiFi/MQTT non-blocking every 5s on failure
- Publishes to `jj1/data` at 1 Hz

### Backend (`backend/`)
- Runtime: Node.js (ESM)
- Framework: Express 5.x
- Key libraries: `better-sqlite3`, `mqtt`, `ws`, `cors`
- **MQTT**: subscribes to `jj1/data`, saves to `jj1_readings`, broadcasts via WebSocket
- **WebSocket**: sends `{type:'jj1_history', data:[]}` on connect, then `{type:'jj1_reading', data:{...}}` per update
- **REST API**:
  - `GET /api/jj1/readings?limit=N` — last N readings
  - `GET /api/jj1/readings/range?from=&to=` — by timestamp range
  - `GET /api/jj1/stats` — latest reading
- Auto-deletes readings older than 30 days

### Frontend (`frontend/`)
- Framework: Vue 3 (Composition API)
- Build: Vite 8.x, Tailwind CSS 4.x, Chart.js 4.x via vue-chartjs
- **BatteryChart.vue**: base component — props: `field`, `label`, `unit`, `color`, `decimals`
- **8 specific charts**: each imports BatteryChart with preset color/field
- WebSocket auto-connects, reconnects every 3s, configurable history (30/60/100 pts)
- Stat cards: SOC (with progress bar), V_batt, I_dps, Charger mode, V_dps, OCV_est, Innov, Charged Ah

## Configuration

All network addresses are hardcoded — change these when deploying on a different network:

| Setting | File | Detail |
|---|---|---|
| MQTT broker IP | `JJ1/JJ1.ino` | `MQTT_BROKER` constant |
| WiFi SSID/password | `JJ1/JJ1.ino` | `WIFI_SSID` / `WIFI_PASS` constants |
| Diode drop voltage | `JJ1/JJ1.ino` | `DIODE_DROP` — default 0.745V |
| MQTT broker IP | `backend/index.js` | `MQTT_BROKER` constant |
| WebSocket URL | `frontend/src/App.vue` | `new WebSocket('ws://localhost:3000')` |
| Data retention | `backend/index.js` | `DATA_RETENTION_DAYS` — default 30 |

## Running Locally

```bash
# 1. Start Mosquitto broker
mosquitto -c mosquitto.conf

# 2. Start backend
cd backend
npm install
npm start          # or: npm run dev

# 3. Start frontend dev server
cd frontend
npm install
npm run dev

# 4. Flash ESP32 via Arduino IDE
# Open JJ1/JJ1.ino, install WiFi + PubSubClient libraries, upload
```

## EKF Tuning Parameters (JJ1.ino)

| Constant | Default | Meaning |
|---|---|---|
| `BATT_CAPACITY_AH` | 10.0 | Battery usable capacity |
| `R0` | 0.18 Ω | Ohmic internal resistance |
| `R1` | 0.05 Ω | Diffusion resistance |
| `C1` | 3000 F | Diffusion capacitance (τ = 150s) |
| `EKF_Q_SOC` | 0.00005 | SOC process noise |
| `EKF_Q_VRC` | 0.0001 | V_RC process noise |
| `EKF_R_NOISE` | 0.05 | Voltage measurement noise |
| `DIODE_DROP` | 0.745 V | Diode + wiring voltage drop |

## Tech Stack

| Component | Language | Key Libraries |
|---|---|---|
| Backend | Node.js | Express, better-sqlite3, mqtt, ws |
| ESP32 | C++ (Arduino) | WiFi, PubSubClient |
| Frontend | Vue 3 | Chart.js, Tailwind CSS, Vite |
| Broker | — | Mosquitto |
| Database | SQL | SQLite (embedded) |
