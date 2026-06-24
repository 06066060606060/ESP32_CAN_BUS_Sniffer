#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "driver/twai.h"

// AtomS3 mini + Atomic CAN Base CA-IS3050G
#define CAN_TX_PIN  5
#define CAN_RX_PIN  6

bool twaiStarted = false;

// ===== Point d'accès WiFi =====
const char* AP_SSID = "AtomS3-CAN";
const char* AP_PASS = "12345678";   // min 8 caractères pour WPA2

WebServer server(80);

// Buffer des dernières lignes CAN
static const int MAX_LINES = 150;
String canLines[MAX_LINES];
int canWriteIndex = 0;
int canCount = 0;

void addCanLine(const String& line) {
  canLines[canWriteIndex] = line;
  canWriteIndex = (canWriteIndex + 1) % MAX_LINES;
  if (canCount < MAX_LINES) canCount++;
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
    out = "Aucune trame CAN reçue pour le moment.\n";
  }

  return out;
}

void handleRoot() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
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
    }
  </style>
</head>
<body>
  <header>
    <h1>CAN Sniffer AtomS3</h1>
    <div class="sub">ESP32 en point d'accès WiFi direct</div>
  </header>

  <div class="wrap">
    <div class="top">Connecté au WiFi de l'ESP32</div>
    <pre id="log">Chargement...</pre>
  </div>

  <script>
    async function refreshLog() {
      try {
        const r = await fetch('/can');
        const t = await r.text();
        document.getElementById('log').textContent = t;
      } catch (e) {
        document.getElementById('log').textContent = 'Erreur de lecture.';
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

void startAccessPoint() {
  WiFi.mode(WIFI_AP);

  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  if (!ok) {
    addCanLine("ERREUR: impossible de créer le point d'accès WiFi");
    return;
  }

  IPAddress ip = WiFi.softAPIP();
  addCanLine("WiFi AP démarré");
  addCanLine("SSID: " + String(AP_SSID));
  addCanLine("IP: " + ip.toString());
}

void startWebServer() {
  server.on("/", handleRoot);
  server.on("/can", handleCan);
  server.begin();
  addCanLine("Serveur HTTP démarré sur port 80");
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

  addCanLine("=== HELLO CAN SNIFFER (AtomS3 + CA-IS3050G) ===");

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
    char line[140];
    int pos = 0;

    pos += snprintf(line + pos, sizeof(line) - pos, "RX: ID 0x%lX ", (unsigned long)rx_msg.identifier);
    pos += snprintf(line + pos, sizeof(line) - pos, "%s ", rx_msg.extd ? "(EXT)" : "(STD)");
    pos += snprintf(line + pos, sizeof(line) - pos, "%s ", rx_msg.rtr ? "RTR" : "DATA");
    pos += snprintf(line + pos, sizeof(line) - pos, "DLC %d Data:", rx_msg.data_length_code);

    for (int i = 0; i < rx_msg.data_length_code && pos < (int)sizeof(line) - 4; i++) {
      pos += snprintf(line + pos, sizeof(line) - pos, " %02X", rx_msg.data[i]);
    }

    addCanLine(String(line));
  }
}
