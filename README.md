# ESP32 CAN BUS Sniffer

An ESP32-S3 CAN bus sniffer, exposing captured TWAI/CAN frames through a built-in Wi-Fi access point and a live web interface.

This project is designed for **listen-only** CAN monitoring, so it can observe traffic without transmitting frames onto the bus.

## Features

- ESP32-S3 TWAI (CAN) receiver using the native `driver/twai.h` API.
- Listen-only mode for passive sniffing.
- Built-in Wi-Fi Access Point mode, no external router required.
- Embedded web server on port 80.
- Live browser view of recent CAN frames.
- Simple single-file Arduino sketch, easy to modify.

## Wiring

This sketch uses the following pins:

- `CAN_TX_PIN = 5`
- `CAN_RX_PIN = 6`

Adjust these definitions if your hardware mapping is different.


## How it works

The ESP32 starts in SoftAP mode and creates its own Wi-Fi network. A phone or computer can connect directly to that access point and open the hosted web page to view captured CAN frames.

The firmware initializes the TWAI peripheral at 500 kbit/s in listen-only mode, accepts all frames, and stores recent messages in a rolling in-memory buffer displayed by the web UI.

## Default network settings

```cpp
const char* AP_SSID = "AtomS3-CAN";
const char* AP_PASS = "12345678";
```

After boot, connect to the ESP32 Wi-Fi network and open:

```text
http://192.168.4.1
```

## Usage

1. Power the ESP32 and connect it to the CAN transceiver base.
2. Join the `AtomS3-CAN` Wi-Fi network from your phone or laptop.
3. Open `http://192.168.4.1` in a browser.
4. Watch incoming CAN frames update on the page.
5. Download can_log.csv

## Example CAN output in web interface

```text
RX: ID 0x123 (STD) DATA DLC 8 Data: 10 22 F1 90 00 00 00 00
RX: ID 0x456 (EXT) DATA DLC 4 Data: 01 02 03 04
RX: ID 0x7DF (STD) RTR DLC 0 Data:
```

## Example CAN output in log file
```text
timestamp_ms;id;extended;rtr;dlc;data
1523;0x1A0;0;0;8;12 3A FF 00 00 00 07 E1
2011;0x201;1;0;4;00 FF 12 34
```

## Main configuration points

You may want to adapt these settings:

- Wi-Fi SSID and password
- CAN bitrate, currently `500 kbit/s`
- RX/TX GPIO pins
- Number of log lines kept in memory
- Web page refresh interval

## Important notes

- This project is intended for passive CAN monitoring.
- `TWAI_MODE_LISTEN_ONLY` prevents the ESP32 from actively transmitting on the bus.
- Make sure your CAN bus bitrate matches the firmware configuration.
- The web page shows recent frames only; it is not a long-term logger.

  
