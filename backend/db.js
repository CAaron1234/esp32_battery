import Database from 'better-sqlite3';

const db = new Database('weight.db');

db.exec(`
  CREATE TABLE IF NOT EXISTS weight_readings (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    weight    REAL    NOT NULL,
    unit      TEXT    NOT NULL DEFAULT 'g',
    timestamp DATETIME DEFAULT (datetime('now', 'localtime'))
  )
`);

db.exec(`
  CREATE TABLE IF NOT EXISTS lee9_readings (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    v_dps      REAL,
    v_batt     REAL,
    i_dps      REAL,
    i_actual   REAL,
    v_rc       REAL,
    ocv_est    REAL,
    innov      REAL,
    soc        REAL,
    charger    TEXT,
    charged_ah REAL,
    timestamp  DATETIME DEFAULT (datetime('now', 'localtime'))
  )
`);

// Migrate old jj1_readings table if it exists
try {
  db.exec(`INSERT INTO lee9_readings SELECT * FROM jj1_readings`);
  db.exec(`DROP TABLE jj1_readings`);
  console.log('[DB] Migrated jj1_readings → lee9_readings');
} catch (_) {
  // Already migrated or table doesn't exist
}

const insertReading      = db.prepare(`INSERT INTO weight_readings (weight, unit) VALUES (@weight, @unit)`);
const getRecentReadings  = db.prepare(`SELECT * FROM weight_readings ORDER BY timestamp DESC LIMIT ?`);
const getReadingsByRange = db.prepare(`SELECT * FROM weight_readings WHERE timestamp BETWEEN ? AND ? ORDER BY timestamp ASC`);
const getCount           = db.prepare(`SELECT COUNT(*) AS count FROM weight_readings`);
const deleteOldReadings  = db.prepare(`DELETE FROM weight_readings WHERE timestamp < datetime('now', '-' || ? || ' days', 'localtime')`);

const insertLEE9      = db.prepare(`
  INSERT INTO lee9_readings (v_dps, v_batt, i_dps, i_actual, v_rc, ocv_est, innov, soc, charger, charged_ah)
  VALUES (@v_dps, @v_batt, @i_dps, @i_actual, @v_rc, @ocv_est, @innov, @soc, @charger, @charged_ah)
`);
const getRecentLEE9   = db.prepare(`SELECT * FROM lee9_readings ORDER BY timestamp DESC LIMIT ?`);
const getLEE9ByRange  = db.prepare(`SELECT * FROM lee9_readings WHERE timestamp BETWEEN ? AND ? ORDER BY timestamp ASC`);
const deleteOldLEE9   = db.prepare(`DELETE FROM lee9_readings WHERE timestamp < datetime('now', '-' || ? || ' days', 'localtime')`);

export {
  insertReading, getRecentReadings, getReadingsByRange, getCount, deleteOldReadings,
  insertLEE9, getRecentLEE9, getLEE9ByRange, deleteOldLEE9,
};
