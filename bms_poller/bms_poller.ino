// ─────────────────────────────────────────────────────────────────────────────
// Maxvolt Energy — ESP32 BMS Poller
// BMS: 极空 (JK) RS485 Modbus V1.1
// Auto-starts polling on power-up. sendToServer() stub ready for future use.
// ─────────────────────────────────────────────────────────────────────────────

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ── WiFi Config ───────────────────────────────────────────────────────────────
#define WIFI_SSID   "AP001_MV"
#define WIFI_PASS   "M@xVolt@1234@"

// ── Server Config ─────────────────────────────────────────────────────────────
#define SERVER_URL  "http://10.11.1.201:8080/bms"
#define DEVICE_ID   "IOT101"                          // <-- unique ID per ESP32

// ── Pin & Serial Config ───────────────────────────────────────────────────────
#define BMS_SERIAL       Serial2
#define BMS_RX_PIN       16          // RO of MAX485 → ESP32 GPIO16
#define BMS_TX_PIN       17          // DI of MAX485 ← ESP32 GPIO17
#define RS485_DE_RE_PIN  4           // DE+RE tied together → GPIO4
#define BMS_BAUD         115200      // Must match BMS setting (try 9600 or 115200)

// ── BMS Config ────────────────────────────────────────────────────────────────
#define BMS_SLAVE        0x01        // Modbus slave address
#define BMS_CELL_COUNT   16          // Your actual cell count (1–32)
#define POLL_INTERVAL_MS 2000        // Polling interval in ms

// ── Register Map (极空 V1.1, base 0x1200) ────────────────────────────────────
#define REG_BASE         0x1200

// ─────────────────────────────────────────────────────────────────────────────

struct BMSData {
  float    voltage;          // V
  float    current;          // A  (positive = charging, negative = discharging)
  float    power;            // W
  uint8_t  soc;              // %
  uint8_t  soh;              // %
  int32_t  remain_mah;       // mAh
  uint32_t full_mah;         // mAh
  uint32_t cycles;
  float    temp_mos;         // °C
  float    temp_bat[4];      // °C  [0]=Bat1 [1]=Bat2 [2]=Bat3 [3]=Bat4
  uint8_t  cell_count;
  uint16_t cells[32];        // mV
  bool     valid;
};

// ── CRC16 Modbus ──────────────────────────────────────────────────────────────
static uint16_t crc16(const uint8_t* data, uint8_t len) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++)
      crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

// ── RS485 direction helpers ───────────────────────────────────────────────────
static inline void txMode() { digitalWrite(RS485_DE_RE_PIN, HIGH); }
static inline void rxMode()  { digitalWrite(RS485_DE_RE_PIN, LOW);  }

// ── Modbus FC03 — read `count` registers starting at `startReg` ──────────────
// Returns true on success, fills `out[0..count-1]`
static bool readRegs(uint8_t slave, uint16_t startReg, uint8_t count, uint16_t* out) {
  if (count == 0 || count > 125) return false;

  // Build request
  uint8_t req[8];
  req[0] = slave;
  req[1] = 0x03;
  req[2] = (startReg >> 8) & 0xFF;
  req[3] = startReg & 0xFF;
  req[4] = 0x00;
  req[5] = count;
  uint16_t crc = crc16(req, 6);
  req[6] = crc & 0xFF;          // CRC low byte first (little-endian)
  req[7] = (crc >> 8) & 0xFF;

  // Flush RX, send
  while (BMS_SERIAL.available()) BMS_SERIAL.read();
  txMode();
  BMS_SERIAL.write(req, 8);
  BMS_SERIAL.flush();           // Wait until all bytes are transmitted
  rxMode();

  // Read response: [slave][0x03][byteCount][data...][crcL][crcH]
  uint16_t expected = 3 + (uint16_t)count * 2 + 2;
  uint8_t  buf[256];
  uint16_t got = 0;
  uint32_t t   = millis();

  while (got < expected && (millis() - t) < 500) {
    if (BMS_SERIAL.available())
      buf[got++] = (uint8_t)BMS_SERIAL.read();
  }

  if (got < 5)                   return false;
  if (buf[0] != slave)           return false;
  if (buf[1] == 0x83)            return false; // BMS error response
  if (buf[1] != 0x03)            return false;
  if (buf[2] != count * 2)       return false;

  for (uint8_t i = 0; i < count; i++)
    out[i] = ((uint16_t)buf[3 + i * 2] << 8) | buf[4 + i * 2];

  return true;
}

// ── Type helpers ──────────────────────────────────────────────────────────────
static inline int16_t  asI16(uint16_t v)                { return (int16_t)v; }
static inline int32_t  asI32(uint16_t hi, uint16_t lo)  { return (int32_t)(((uint32_t)hi << 16) | lo); }
static inline uint32_t asU32(uint16_t hi, uint16_t lo)  { return ((uint32_t)hi << 16) | lo; }

// ── Read all BMS data ─────────────────────────────────────────────────────────
static BMSData readBMS() {
  BMSData d = {};
  d.cell_count = BMS_CELL_COUNT;

  // Block 1: 0x1200+0x0000, 100 registers
  //   reg[0..31]  = CellVol0..31 (mV)
  //   reg[69]     = TempMos (INT16, x0.1 °C)
  //   reg[72..73] = BatVol  (UINT32, mV)
  //   reg[74..75] = BatWatt (UINT32, mW)
  uint16_t b1[100];
  if (!readRegs(BMS_SLAVE, REG_BASE + 0x0000, 100, b1)) {
    Serial.println("[BMS] Block1 failed");
    return d;
  }

  for (uint8_t i = 0; i < min(BMS_CELL_COUNT, 32); i++)
    d.cells[i] = b1[i];

  d.temp_mos = asI16(b1[69]) * 0.1f;
  d.voltage  = asU32(b1[72], b1[73]) / 1000.0f;
  d.power    = asU32(b1[74], b1[75]) / 1000.0f;

  delay(100); // give BMS time to recover between requests

  // Block 2: 0x1200+0x0098, 30 registers
  //   reg[0..1]  = BatCurrent (INT32, mA)
  //   reg[2]     = TempBat1   (INT16, x0.1 °C)
  //   reg[3]     = TempBat2   (INT16, x0.1 °C)
  //   reg[7]     = BalanSta(hi) + SOC(lo)
  //   reg[8..9]  = SOCCapRemain (INT32, mAh)
  //   reg[10..11]= SOCFullCharge (UINT32, mAh)
  //   reg[12..13]= CycleCount (UINT32)
  //   reg[16]    = SOH(hi) + Precharge(lo)
  uint16_t b2[30];
  if (!readRegs(BMS_SLAVE, REG_BASE + 0x0098, 30, b2)) {
    Serial.println("[BMS] Block2 failed");
    return d;
  }

  d.current      = asI32(b2[0], b2[1]) / 1000.0f;
  d.temp_bat[0]  = asI16(b2[2]) * 0.1f;
  d.temp_bat[1]  = asI16(b2[3]) * 0.1f;
  d.soc          = b2[7] & 0xFF;
  d.soh          = (b2[16] >> 8) & 0xFF;
  d.remain_mah   = asI32(b2[8], b2[9]);
  d.full_mah     = asU32(b2[10], b2[11]);
  d.cycles       = asU32(b2[12], b2[13]);

  delay(100);

  // Block 3: 0x1200+0x00F8, 2 registers — TempBat3, TempBat4
  uint16_t b3[2];
  if (readRegs(BMS_SLAVE, REG_BASE + 0x00F8, 2, b3)) {
    d.temp_bat[2] = asI16(b3[0]) * 0.1f;
    d.temp_bat[3] = asI16(b3[1]) * 0.1f;
  }

  d.valid = true;
  return d;
}

// ── Print to Serial (USB monitor) ────────────────────────────────────────────
static void printBMS(const BMSData& d) {
  Serial.println("────────────────────────────────────");
  Serial.printf("Voltage  : %.3f V\n",   d.voltage);
  Serial.printf("Current  : %+.3f A  (%s)\n",
                d.current, d.current >= 0 ? "Charging" : "Discharging");
  Serial.printf("Power    : %.2f W\n",   d.power);
  Serial.printf("SOC      : %d%%\n",     d.soc);
  Serial.printf("SOH      : %d%%\n",     d.soh);
  Serial.printf("Remain   : %d mAh\n",  d.remain_mah);
  Serial.printf("Capacity : %u mAh\n",  d.full_mah);
  Serial.printf("Cycles   : %u\n",      d.cycles);
  Serial.printf("T_MOS    : %.1f C\n",  d.temp_mos);
  Serial.printf("T_Bat1   : %.1f C\n",  d.temp_bat[0]);
  Serial.printf("T_Bat2   : %.1f C\n",  d.temp_bat[1]);
  Serial.printf("T_Bat3   : %.1f C\n",  d.temp_bat[2]);
  Serial.printf("T_Bat4   : %.1f C\n",  d.temp_bat[3]);
  Serial.printf("Cells[%d]:", d.cell_count);
  for (uint8_t i = 0; i < d.cell_count; i++)
    Serial.printf(" %u", d.cells[i]);
  Serial.println();
}

// ── WiFi ──────────────────────────────────────────────────────────────────────

static void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  delay(200);
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    uint8_t reason = WiFi.status();
    Serial.printf("\n[WiFi] Failed (status=%d) — ", reason);
    switch (reason) {
      case WL_NO_SSID_AVAIL: Serial.println("SSID not found (wrong name or 5GHz only)"); break;
      case WL_CONNECT_FAILED: Serial.println("Wrong password"); break;
      case WL_DISCONNECTED:   Serial.println("Disconnected — check credentials"); break;
      default:                Serial.println("Unknown error"); break;
    }
  }
}

// ── Send to server ────────────────────────────────────────────────────────────
static void sendToServer(const BMSData& d) {
  if (WiFi.status() != WL_CONNECTED) return;

  JsonDocument doc;
  doc["device_id"]  = DEVICE_ID;
  doc["voltage"]    = d.voltage;
  doc["current"]    = d.current;
  doc["power"]      = d.power;
  doc["soc"]        = d.soc;
  doc["soh"]        = d.soh;
  doc["remain_mah"] = d.remain_mah;
  doc["full_mah"]   = d.full_mah;
  doc["cycles"]     = d.cycles;
  doc["temp_mos"]   = d.temp_mos;
  doc["charging"]   = (d.current >= 0.0f);

  JsonArray temps = doc["temp_bat"].to<JsonArray>();
  for (uint8_t i = 0; i < 4; i++)
    temps.add(d.temp_bat[i]);

  JsonArray cells = doc["cells"].to<JsonArray>();
  for (uint8_t i = 0; i < d.cell_count; i++)
    cells.add(d.cells[i]);

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  if (code == 204) {
    Serial.println("[HTTP] OK");
  } else {
    Serial.printf("[HTTP] Failed, code=%d\n", code);
  }
  http.end();
}

// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(RS485_DE_RE_PIN, OUTPUT);
  rxMode();

  BMS_SERIAL.begin(BMS_BAUD, SERIAL_8N1, BMS_RX_PIN, BMS_TX_PIN);
  delay(100);

  Serial.println("[BMS] Maxvolt Energy — BMS Poller");
  Serial.printf("[BMS] Port: Serial2  RX=%d TX=%d DE/RE=%d  Baud=%d\n",
                BMS_RX_PIN, BMS_TX_PIN, RS485_DE_RE_PIN, BMS_BAUD);
  Serial.printf("[BMS] Slave=0x%02X  Cells=%d  Poll=%dms\n",
                BMS_SLAVE, BMS_CELL_COUNT, POLL_INTERVAL_MS);
  Serial.println("[BMS] Starting...");

  connectWiFi();
}

void loop() {
  static uint32_t lastPoll    = 0;
  static uint32_t lastWiFiChk = 0;

  // Check WiFi every 30s and reconnect if dropped
  if (millis() - lastWiFiChk >= 30000) {
    lastWiFiChk = millis();
    connectWiFi();
  }

  if (WiFi.status() != WL_CONNECTED) return;  // wait for WiFi before polling

  if (millis() - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = millis();

    BMSData d = readBMS();
    if (d.valid) {
      printBMS(d);
      sendToServer(d);   // no-op until you implement it
    } else {
      Serial.println("[BMS] Poll failed — check wiring A/B and baud rate");
    }
  }
}
