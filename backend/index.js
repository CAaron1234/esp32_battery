import express             from 'express';
import cors                from 'cors';
import http                from 'http';
import { WebSocketServer } from 'ws';
import mqtt                from 'mqtt';

const PORT        = 3000;
const MQTT_BROKER = 'mqtt://192.168.100.193'; // change IP to match your network

const app    = express();
const server = http.createServer(app);

app.use(cors());
app.use(express.json());

// ── WebSocket ────────────────────────────────────────────────
const wss     = new WebSocketServer({ server });
const clients = new Set();

wss.on('connection', (ws) => {
  clients.add(ws);
  console.log(`[WS] Client connected. Total: ${clients.size}`);

  ws.on('close', () => {
    clients.delete(ws);
    console.log(`[WS] Client disconnected. Total: ${clients.size}`);
  });

  ws.on('error', (err) => {
    console.error('[WS] Error:', err.message);
    clients.delete(ws);
  });
});

function broadcast(payload) {
  const message = JSON.stringify(payload);
  for (const client of clients) {
    if (client.readyState === 1) client.send(message);
  }
}

// ── MQTT ─────────────────────────────────────────────────────
const mqttClient = mqtt.connect(MQTT_BROKER, { reconnectPeriod: 3000 });

mqttClient.on('connect', () => {
  console.log(`[MQTT] Connected to broker: ${MQTT_BROKER}`);

  mqttClient.subscribe('lee9/data', { qos: 0 }, (err) => {
    if (err) console.error('[MQTT] Subscribe error:', err.message);
    else     console.log('[MQTT] Subscribed to lee9/data');
  });
});

mqttClient.on('message', (topic, message) => {
  if (topic !== 'lee9/data') return;

  try {
    const payload = JSON.parse(message.toString());
    const { v_dps, v_batt, i_dps, i_actual, v_rc, ocv_est, innov, soc, charger, charged_ah } = payload;

    console.log(`[MQTT] Received → SOC ${soc?.toFixed(1)}%`);

    broadcast({
      type: 'lee9_reading',
      data: {
        v_dps, v_batt, i_dps, i_actual, v_rc, ocv_est, innov, soc, charger, charged_ah,
        timestamp: new Date().toLocaleString('en-MY', { timeZone: 'Asia/Kuala_Lumpur' }),
      },
    });
  } catch (err) {
    console.error('[MQTT] Failed to parse lee9/data:', err.message);
  }
});

mqttClient.on('error',     (err) => console.error('[MQTT] Error:', err.message));
mqttClient.on('offline',   ()    => console.warn('[MQTT] Client went offline'));
mqttClient.on('reconnect', ()    => console.log('[MQTT] Attempting reconnect...'));
mqttClient.on('disconnect',()    => console.warn('[MQTT] Disconnected from broker'));

// ── Start ─────────────────────────────────────────────────────
server.listen(PORT, () => {
  console.log(`
╔══════════════════════════════════════╗
║   Battery Monitor Server Running     ║
╠══════════════════════════════════════╣
║  HTTP  → http://localhost:${PORT}       ║
║  WS    → ws://localhost:${PORT}         ║
║  MQTT  → ${MQTT_BROKER}          ║
╚══════════════════════════════════════╝
  `);
});
