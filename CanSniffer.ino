#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include "driver/twai.h"

// AtomS3 mini + Atomic CAN Base CA-IS3050G
#define CAN_TX_PIN  5
#define CAN_RX_PIN  6

bool twaiStarted = false;

// ===== Log persistant sur SPIFFS =====
const char* LOG_FILE_PATH = "/can_log.txt";
bool spiffsOk = false;

String pendingBuffer;              // lignes pas encore écrites sur la flash
int pendingLines = 0;
unsigned long lastFlushMillis = 0;
const unsigned long LOG_FLUSH_INTERVAL_MS = 1000;   // écrit au moins 1x/seconde
const int LOG_FLUSH_LINE_THRESHOLD = 20;            // ou dès que 20 lignes sont en attente
const size_t MAX_LOG_FILE_SIZE = 900000;            // ~900 Ko, à ajuster selon le schéma de partition SPIFFS choisi

// ===== Point d'accès WiFi =====
const char* AP_SSID = "AtomS3-CAN";
const char* AP_PASS = "12345678";   // min 8 caractères pour WPA2

WebServer server(80);

// Buffer des dernières lignes CAN
static const int MAX_LINES = 150;
String canLines[MAX_LINES];
int canWriteIndex = 0;
int canCount = 0;

bool initSPIFFS() {
  // true = formate automatiquement si le système de fichiers est absent/corrompu
  return SPIFFS.begin(true);
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

  // Garde-fou anti-saturation de la flash : repart de zéro si le fichier devient trop gros
  File check = SPIFFS.open(LOG_FILE_PATH, FILE_READ);
  if (check) {
    size_t sz = check.size();
    check.close();
    if (sz > MAX_LOG_FILE_SIZE) {
      SPIFFS.remove(LOG_FILE_PATH);
      File nf = SPIFFS.open(LOG_FILE_PATH, FILE_APPEND);
      if (nf) {
        nf.println("=== Log précédent supprimé (taille max atteinte), redémarrage du log ===");
        nf.close();
      }
    }
  }
}

void addCanLine(const String& line) {
  canLines[canWriteIndex] = line;
  canWriteIndex = (canWriteIndex + 1) % MAX_LINES;
  if (canCount < MAX_LINES) canCount++;

  // Ajoute aussi la ligne au buffer d'écriture persistant (SPIFFS)
  pendingBuffer += line;
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
    <div class="sub">ESP32 en point d'accès WiFi direct</div>
  </header>

  <div class="wrap">
    <div class="top">
      <span>Connecté au WiFi de l'ESP32</span>
      <div class="actions">
        <button id="saveBtn" onclick="saveLog()">💾 Télécharger le log complet</button>
        <button id="clearBtn" class="danger" onclick="clearLog()">🗑️ Effacer le log</button>
      </div>
    </div>
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

    function saveLog() {
      // Déclenche le téléchargement du log complet stocké sur SPIFFS (route /download)
      const a = document.createElement('a');
      a.href = '/download';
      a.download = 'can_log.txt';
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
    }

    async function clearLog() {
      if (!confirm("Effacer définitivement le log enregistré sur la carte ?")) return;
      try {
        await fetch('/clear', { method: 'POST' });
        refreshLog();
      } catch (e) {
        alert('Erreur lors de la suppression du log.');
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
  flushLogToSPIFFS(true); // force l'écriture des dernières lignes avant de servir le fichier

  if (!spiffsOk) {
    server.send(500, "text/plain; charset=utf-8", "SPIFFS indisponible, log complet non accessible.");
    return;
  }

  File f = SPIFFS.open(LOG_FILE_PATH, FILE_READ);
  if (!f) {
    server.send(200, "text/plain; charset=utf-8", "Aucun log enregistré pour le moment.");
    return;
  }

  // Envoie le fichier en tant que téléchargement (attachment) directement depuis la flash
  server.sendHeader("Content-Disposition", "attachment; filename=can_log.txt");
  server.streamFile(f, "text/plain; charset=utf-8");
  f.close();
}

void handleClearLog() {
  if (spiffsOk) {
    SPIFFS.remove(LOG_FILE_PATH);
  }
  pendingBuffer = "";
  pendingLines = 0;
  canCount = 0;
  canWriteIndex = 0;

  addCanLine("=== Log effacé par l'utilisateur ===");
  server.send(200, "text/plain; charset=utf-8", "OK");
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
  server.on("/download", handleDownload);
  server.on("/clear", HTTP_POST, handleClearLog);
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

  spiffsOk = initSPIFFS();

  addCanLine("=== HELLO CAN SNIFFER (AtomS3 + CA-IS3050G) ===");
  if (spiffsOk) {
    addCanLine("SPIFFS monté, log persistant activé (" + String(LOG_FILE_PATH) + ")");
  } else {
    addCanLine("ERREUR: SPIFFS indisponible, log persistant désactivé");
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
