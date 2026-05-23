#include <Arduino.h>
#include "driver/twai.h"

// AtomS3 mini + Atomic CAN Base CA-IS3050G
#define CAN_TX_PIN  5
#define CAN_RX_PIN  6

bool twaiStarted = false;

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("=== HELLO CAN SNIFFER (AtomS3 + CA-IS3050G) ===");

  // Config TWAI in listen only, 500 kbit/s
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
    (gpio_num_t)CAN_TX_PIN,
    (gpio_num_t)CAN_RX_PIN,
    TWAI_MODE_LISTEN_ONLY  // IMPORTANT: listen only
  );
  g_config.tx_queue_len = 0;   // we don't need TX
  g_config.rx_queue_len = 32;  // RX

  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t err = twai_driver_install(&g_config, &t_config, &f_config);
  if (err != ESP_OK) {
    Serial.printf("twai_driver_install FAILED: %d\n", (int)err);
    return;
  }

  err = twai_start();
  if (err != ESP_OK) {
    Serial.printf("twai_start FAILED: %d\n", (int)err);
    return;
  }

  twaiStarted = true;
  Serial.println("TWAI listen-only started at 500 kbit/s (TX=5, RX=6)");
}

void loop() {
  if (!twaiStarted) {
    delay(1000);
    return;
  }

  twai_message_t rx_msg;
  esp_err_t err = twai_receive(&rx_msg, pdMS_TO_TICKS(100));
  if (err == ESP_OK) {
    Serial.print("RX: ID 0x");
    Serial.print(rx_msg.identifier, HEX);
    Serial.print(rx_msg.extd ? " (EXT) " : " (STD) ");
    Serial.print(rx_msg.rtr ? "RTR " : "DATA ");
    Serial.print("DLC ");
    Serial.print(rx_msg.data_length_code);
    Serial.print(" Data: ");

    for (int i = 0; i < rx_msg.data_length_code; i++) {
      Serial.printf("%02X ", rx_msg.data[i]);
    }

    Serial.println();
  }
}