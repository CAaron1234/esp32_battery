import express             from 'express';
import cors                from 'cors';
import http                from 'http';
import { WebSocketServer } from 'ws';
import mqtt                from 'mqtt';
import {
  insertReading, getRecentReadings, getReadingsByRange, getCount, deleteOldReadings,
  insertLEE9, getRecentLEE9, getLEE9ByRange, deleteOldLEE9,
} from './db.js';

const PORT               = 3000;
const MQTT_BROKER        = 'mqtt://192.168.100.193'; // change IP to match your network
const DATA_RETENTION_DAYS = 30;

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

  // Send last 50 LEE9 readings as history
  const history = getRecentLEE9.all(50).reverse();
  ws.send(JSON.stringify({ type: 'lee9_history', data: history }));

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

    const result = insertLEE9.run({ v_dps, v_batt, i_dps, i_actual, v_rc, ocv_est, innov, soc, charger, charged_ah });
    console.log(`[DB] Saved LEE9 reading #${result.lastInsertRowid} → SOC ${soc?.toFixed(1)}%`);

    broadcast({
      type: 'lee9_reading',
      data: {
        id: result.lastInsertRowid,
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

// ── REST API — LEE9 ──────────────────────────────────────────
app.get('/api/lee9/readings', (req, res) => {
  const limit = parseInt(req.query.limit) || 50;
  res.json({ success: true, data: getRecentLEE9.all(limit).reverse() });
});

app.get('/api/lee9/readings/range', (req, res) => {
  const { from, to } = req.query;
  if (!from || !to) return res.status(400).json({ success: false, message: 'Missing from or to query params' });
  res.json({ success: true, data: getLEE9ByRange.all(from, to) });
});

app.get('/api/lee9/stats', (req, res) => {
  const latest = getRecentLEE9.all(1)[0] || null;
  res.json({ success: true, data: { latest } });
});

// ── REST API — Weight (legacy) ────────────────────────────────
app.get('/api/readings', (req, res) => {
  const limit = parseInt(req.query.limit) || 50;
  res.json({ success: true, data: getRecentReadings.all(limit).reverse() });
});

// ── Cleanup ───────────────────────────────────────────────────
function cleanupOldData() {
  const r1 = deleteOldReadings.run(DATA_RETENTION_DAYS);
  const r2 = deleteOldLEE9.run(DATA_RETENTION_DAYS);
  const total = r1.changes + r2.changes;
  if (total > 0) console.log(`[DB] Auto-cleanup: removed ${total} old readings`);
}
cleanupOldData();
setInterval(cleanupOldData, 24 * 60 * 60 * 1000);

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
