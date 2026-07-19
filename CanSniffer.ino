#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include "driver/twai.h"

// AtomS3 mini + Atomic CAN Base CA-IS3050G
#define CAN_TX_PIN  5
#define CAN_RX_PIN  6

bool twaiStarted = false;

// ===== Persistent log on SPIFFS (CSV format) =====
const char* LOG_FILE_PATH = "/can_log.csv";
const char* CSV_HEADER = "timestamp_ms;id;extended;rtr;dlc;data";
bool spiffsOk = false;

String pendingBuffer;              // lines not yet written to flash
int pendingLines = 0;
unsigned long lastFlushMillis = 0;
const unsigned long LOG_FLUSH_INTERVAL_MS = 1000;   // flush at least once per second
const int LOG_FLUSH_LINE_THRESHOLD = 20;            // or as soon as 20 lines are pending
const size_t MAX_LOG_FILE_SIZE = 900000;            // ~900 KB, adjust based on the chosen SPIFFS partition scheme

// ===== WiFi access point =====
const char* AP_SSID = "AtomS3-CAN";
const char* AP_PASS = "12345678";   // min 8 characters for WPA2

WebServer server(80);

// Buffer of the latest CAN lines
static const int MAX_LINES = 150;
String canLines[MAX_LINES];
int canWriteIndex = 0;
int canCount = 0;

bool initSPIFFS() {
  // true = automatically format if the file system is missing/corrupted
  return SPIFFS.begin(true);
}

// Creates the CSV file with its header if it doesn't exist yet
bool ensureLogFileWithHeader() {
  if (!spiffsOk) return false;
  if (!SPIFFS.exists(LOG_FILE_PATH)) {
    File f = SPIFFS.open(LOG_FILE_PATH, FILE_WRITE); // creates/overwrites with an empty file
    if (!f) return false;
    f.println(CSV_HEADER);
    f.close();
  }
  return true;
}

void flushLogToSPIFFS(bool force) {
  if (!spiffsOk) return;
  if (pendingBuffer.length() == 0) return;

  bool timeToFlush = force ||
                      pendingLines >= LOG_FLUSH_LINE_THRESHOLD ||
                      (millis() - lastFlushMillis) >= LOG_FLUSH_INTERVAL_MS;
  if (!timeToFlush) return;

  File f = SPIFFS.open(LOG_FILE_PATH, FILE_APPEND);
  if (f) {
    f.print(pendingBuffer);
    f.close();
  }
  pendingBuffer = "";
  pendingLines = 0;
  lastFlushMillis = millis();

  // Flash overflow safeguard: starts fresh (with the CSV header) if the file gets too large
  File check = SPIFFS.open(LOG_FILE_PATH, FILE_READ);
  if (check) {
    size_t sz = check.size();
    check.close();
    if (sz > MAX_LOG_FILE_SIZE) {
      SPIFFS.remove(LOG_FILE_PATH);
      File nf = SPIFFS.open(LOG_FILE_PATH, FILE_WRITE);
      if (nf) {
        nf.println(CSV_HEADER);
        nf.println("# Previous log deleted (max size reached), restarting log");
        nf.close();
      }
    }
  }
}

// Status/error messages (not CAN frames): shown live and written as CSV comments (#)
void addCanLine(const String& line) {
  canLines[canWriteIndex] = line;
  canWriteIndex = (canWriteIndex + 1) % MAX_LINES;
  if (canCount < MAX_LINES) canCount++;

  pendingBuffer += "# ";
  pendingBuffer += line;
  pendingBuffer += "\n";
  pendingLines++;
  flushLogToSPIFFS(false);
}

// Received CAN frame: shown live in readable text, and written as a CSV row in the persistent file
void addCanFrame(uint32_t id, bool extd, bool rtr, uint8_t dlc, const uint8_t* data) {
  unsigned long ts = millis();

  // --- Readable line for the live web display (RAM buffer) ---
  char line[140];
  int pos = 0;
  pos += snprintf(line + pos, sizeof(line) - pos, "RX: ID 0x%lX ", (unsigned long)id);
  pos += snprintf(line + pos, sizeof(line) - pos, "%s ", extd ? "(EXT)" : "(STD)");
  pos += snprintf(line + pos, sizeof(line) - pos, "%s ", rtr ? "RTR" : "DATA");
  pos += snprintf(line + pos, sizeof(line) - pos, "DLC %d Data:", dlc);
  if (!rtr) {
    for (int i = 0; i < dlc && pos < (int)sizeof(line) - 4; i++) {
      pos += snprintf(line + pos, sizeof(line) - pos, " %02X", data[i]);
    }
  }

  canLines[canWriteIndex] = String(line);
  canWriteIndex = (canWriteIndex + 1) % MAX_LINES;
  if (canCount < MAX_LINES) canCount++;

  // --- CSV row for the persistent log: timestamp_ms;id;extended;rtr;dlc;data ---
  char csvLine[160];
  int cpos = 0;
  cpos += snprintf(csvLine + cpos, sizeof(csvLine) - cpos, "%lu;0x%lX;%d;%d;%d;",
                    ts, (unsigned long)id, extd ? 1 : 0, rtr ? 1 : 0, dlc);
  if (!rtr) {
    for (int i = 0; i < dlc && cpos < (int)sizeof(csvLine) - 4; i++) {
      cpos += snprintf(csvLine + cpos, sizeof(csvLine) - cpos, i > 0 ? " %02X" : "%02X", data[i]);
    }
  }

  pendingBuffer += csvLine;
  pendingBuffer += "\n";
  pendingLines++;
  flushLogToSPIFFS(false);
}

String getCanLog() {
  String out;
  out.reserve(6000);

  int start = (canCount == MAX_LINES) ? canWriteIndex : 0;
  for (int i = 0; i < canCount; i++) {
    int idx = (start + i) % MAX_LINES;
    out += canLines[idx];
    out += "\n";
  }

  if (canCount == 0) {
    out = "No CAN frame received yet.\n";
  }

  return out;
}

void handleRoot() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>AtomS3 CAN Sniffer</title>
  <style>
    body {
      margin: 0;
      background: #0f1115;
      color: #e8e8e8;
      font-family: Arial, sans-serif;
    }
    header {
      padding: 16px;
      background: #171a21;
      border-bottom: 1px solid #2c3440;
    }
    h1 {
      margin: 0 0 6px 0;
      font-size: 20px;
    }
    .sub {
      color: #9fb0c3;
      font-size: 14px;
    }
    .wrap {
      padding: 16px;
    }
    pre {
      background: #11161d;
      border: 1px solid #2c3440;
      border-radius: 10px;
      padding: 12px;
      margin: 0;
      white-space: pre-wrap;
      word-break: break-word;
      font-family: Consolas, monospace;
      font-size: 13px;
      min-height: 60vh;
    }
    .top {
      margin-bottom: 12px;
      color: #8fd3ff;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
    }
    .actions {
      display: flex;
      gap: 8px;
    }
    button {
      background: #1f7ae0;
      color: #fff;
      border: none;
      border-radius: 8px;
      padding: 8px 14px;
      font-size: 13px;
      cursor: pointer;
      font-family: Arial, sans-serif;
    }
    button:hover {
      background: #1662ba;
    }
    button.danger {
      background: #b3261e;
    }
    button.danger:hover {
      background: #8f1e18;
    }
  </style>
</head>
<body>
  <header>
    <h1>CAN Sniffer AtomS3</h1>
    <div class="sub">ESP32 in direct WiFi access point mode</div>
  </header>

  <div class="wrap">
    <div class="top">
      <span>Connected to the ESP32 WiFi</span>
      <div class="actions">
        <button id="saveBtn" onclick="saveLog()">💾 Download full log (CSV)</button>
        <button id="clearBtn" class="danger" onclick="clearLog()">🗑️ Clear log</button>
      </div>
    </div>
    <pre id="log">Loading...</pre>
  </div>

  <script>
    async function refreshLog() {
      try {
        const r = await fetch('/can');
        const t = await r.text();
        document.getElementById('log').textContent = t;
      } catch (e) {
        document.getElementById('log').textContent = 'Read error.';
      }
    }

    function saveLog() {
      // Triggers download of the full CSV log stored on SPIFFS (via the /download route)
      const a = document.createElement('a');
      a.href = '/download';
      a.download = 'can_log.csv';
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
    }

    async function clearLog() {
      if (!confirm("Permanently delete the log stored on the device?")) return;
      try {
        await fetch('/clear', { method: 'POST' });
        refreshLog();
      } catch (e) {
        alert('Error while deleting the log.');
      }
    }

    refreshLog();
    setInterval(refreshLog, 500);
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html; charset=utf-8", page);
}

void handleCan() {
  server.send(200, "text/plain; charset=utf-8", getCanLog());
}

void handleDownload() {
  flushLogToSPIFFS(true); // forces writing the last pending lines before serving the file

  if (!spiffsOk) {
    server.send(500, "text/plain; charset=utf-8", "SPIFFS unavailable, full log not accessible.");
    return;
  }

  File f = SPIFFS.open(LOG_FILE_PATH, FILE_READ);
  if (!f) {
    server.send(200, "text/plain; charset=utf-8", "No log recorded yet.");
    return;
  }

  // Sends the CSV file as a download (attachment) directly from flash
  server.sendHeader("Content-Disposition", "attachment; filename=can_log.csv");
  server.streamFile(f, "text/csv; charset=utf-8");
  f.close();
}

void handleClearLog() {
  if (spiffsOk) {
    SPIFFS.remove(LOG_FILE_PATH);
    ensureLogFileWithHeader(); // recreates the CSV file with its header
  }
  pendingBuffer = "";
  pendingLines = 0;
  canCount = 0;
  canWriteIndex = 0;

  addCanLine("=== Log cleared by user ===");
  server.send(200, "text/plain; charset=utf-8", "OK");
}

void startAccessPoint() {
  WiFi.mode(WIFI_AP);

  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  if (!ok) {
    addCanLine("ERROR: unable to create WiFi access point");
    return;
  }

  IPAddress ip = WiFi.softAPIP();
  addCanLine("WiFi AP started");
  addCanLine("SSID: " + String(AP_SSID));
  addCanLine("IP: " + ip.toString());
}

void startWebServer() {
  server.on("/", handleRoot);
  server.on("/can", handleCan);
  server.on("/download", handleDownload);
  server.on("/clear", HTTP_POST, handleClearLog);
  server.begin();
  addCanLine("HTTP server started on port 80");
}

void startTWAI() {
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
    (gpio_num_t)CAN_TX_PIN,
    (gpio_num_t)CAN_RX_PIN,
    TWAI_MODE_LISTEN_ONLY
  );
  g_config.tx_queue_len = 0;
  g_config.rx_queue_len = 32;

  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
  if (err != ESP_OK) {
    addCanLine("twai_driver_install FAILED: " + String((int)err));
    return;
  }

  err = twai_start();
  if (err != ESP_OK) {
    addCanLine("twai_start FAILED: " + String((int)err));
    return;
  }

  twaiStarted = true;
  addCanLine("TWAI listen-only started at 500 kbit/s (TX=5, RX=6)");
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  spiffsOk = initSPIFFS();
  if (spiffsOk) {
    ensureLogFileWithHeader();
  }

  addCanLine("=== HELLO CAN SNIFFER (AtomS3 + CA-IS3050G) ===");
  if (spiffsOk) {
    addCanLine("SPIFFS mounted, persistent CSV log enabled (" + String(LOG_FILE_PATH) + ")");
  } else {
    addCanLine("ERROR: SPIFFS unavailable, persistent log disabled");
  }

  startAccessPoint();
  startWebServer();
  startTWAI();
}

void loop() {
  server.handleClient();

  if (!twaiStarted) {
    delay(20);
    return;
  }

  twai_message_t rx_msg;
  esp_err_t err = twai_receive(&rx_msg, pdMS_TO_TICKS(10));

  if (err == ESP_OK) {
    addCanFrame(rx_msg.identifier, rx_msg.extd, rx_msg.rtr, rx_msg.data_length_code, rx_msg.data);
  }
}
