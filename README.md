# BNB Driver Master — ESP32 Wardriving Rig

A Wi-Fi wardriving device built around an **ESP32-WROOM-32E** master node
(handles Bluetooth to your phone and ESP-NOW to child nodes) and one or more
**ESP32-C5** scanner nodes (dual-band 2.4/5GHz Wi-Fi scanning).

## Architecture

```
                         ┌─────────────────────────┐
                         │   Phone (BLE central)    │
                         └────────────▲─────────────┘
                                      │ BLE (NimBLE GATT)
                         ┌────────────┴─────────────┐
                         │   master_node             │
                         │   ESP32-WROOM-32E         │
                         │   - BLE GATT server       │
                         │   - ESP-NOW master        │
                         │   - UART master           │
                         │   - AP aggregator/dedup   │
                         └───┬───────────────────┬───┘
                    UART TX/RX│                   │ ESP-NOW (Wi-Fi, unicast)
                         ┌────┴─────┐       ┌──────┴──────┐   ┌─────────────┐
                         │ ESP32-C5  │       │  ESP32-C5   │...│  ESP32-C5   │
                         │ (wired)   │       │ (wireless)  │   │ (wireless)  │
                         │ 2.4/5GHz  │       │ 2.4/5GHz    │   │ 2.4/5GHz    │
                         │ scanner   │       │ scanner     │   │ scanner     │
                         └───────────┘       └─────────────┘   └─────────────┘
```

- **master_node** (target `esp32`): runs on the WROOM-32E. Advertises a BLE
  GATT service to stream discovered APs to a phone app and accept
  start/stop commands. Talks to the directly-wired ESP32-C5 over UART, and
  discovers/coordinates any number of additional wireless ESP32-C5 nodes
  over ESP-NOW. It splits the full 2.4GHz + 5GHz channel list across every
  known scanner (wired + wireless) and merges all reported APs into one
  deduplicated table (keyed by BSSID) before pushing them to the phone.

- **wifi_scanner_c5** (target `esp32c5`): runs on every ESP32-C5. Build it
  twice — once configured for the **UART** transport (the node wired
  directly to the master) and once for **ESP-NOW** (every wireless child
  node). Select the transport via `idf.py menuconfig` → "Wardrive Scanner
  (ESP32-C5) Configuration". Each node scans whatever channels it's
  assigned (or all channels if none have been assigned yet) and streams
  `wd_ap_record_t` results back to the master.

- **components/wardrive_proto**: the shared wire protocol (message types,
  the `wd_ap_record_t` / `wd_channel_assignment_t` structs, CRC16 and a
  byte-stream framer for UART) used identically by both firmware images.

## Wiring

Master (ESP32-WROOM-32E) UART pins ↔ local ESP32-C5 UART pins (cross TX/RX,
common ground):

| Master (WROOM-32E) | ESP32-C5 (wired) |
|---------------------|------------------|
| GPIO17 (TX)          | GPIO9 (RX)       |
| GPIO16 (RX)          | GPIO8 (TX)       |
| GND                  | GND              |

Note: GPIO16/17 aren't broken out on the ESP32-C5-DevKitC-1 (they're used
internally by the WROOM-1 module's flash/PSRAM SPI bus), so the C5 side
uses GPIO8/9 instead — free, non-strapping pins on header J1. Avoid
GPIO11/12 on the C5 (already used as U0TXD/U0RXD by its own USB-UART
bridge) and the strapping pins (2, 3, 7, 25, 26, 27, 28).

Pins/baud/UART port are configurable via `idf.py menuconfig` on both
projects (must match on each end). Additional ESP32-C5 nodes need no wired
connection at all — they join over ESP-NOW and are assigned Wi-Fi channels
wirelessly by the master.

## Building

```bash
# one-time toolchain setup (already done if you've built these once)
$HOME/.espressif/v6.1/esp-idf/install.sh esp32,esp32c5
source $HOME/.espressif/v6.1/esp-idf/export.sh

# Master node (ESP32-WROOM-32E)
cd master_node
idf.py set-target esp32
idf.py menuconfig   # optional: adjust UART pins/BLE name/ESP-NOW channel
idf.py -p /dev/ttyUSB0 flash monitor

# Wired scanner (ESP32-C5, connected via UART)
cd ../wifi_scanner_c5
idf.py set-target esp32c5
idf.py menuconfig   # Wardrive Scanner Configuration -> Link to master node -> UART
idf.py -p /dev/ttyUSB1 flash monitor

# Additional wireless scanner nodes (ESP32-C5, one build per extra node)
cd ../wifi_scanner_c5
idf.py menuconfig   # Link to master node -> ESP-NOW (this is the default)
idf.py -p /dev/ttyUSBn flash monitor
```

## Protocol notes

- UART frames: `AA 55 <len:2 LE> <type:1> <payload> <crc16:2 LE>` (CRC16/CCITT
  over type+payload). See `components/wardrive_proto/wd_uart_frame.*`.
- ESP-NOW frames: `<type:1> <payload>`, sent unicast once a node's MAC is
  known (learned from its first received packet), broadcast otherwise.
- Message types (`wd_msg_type_t`): `HELLO_REQ/RESP`, `ASSIGN_CHANNELS`,
  `START_SCAN`, `STOP_SCAN`, `AP_RECORD`, `SCAN_CYCLE_DONE`, `HEARTBEAT`.
- The master's BLE service exposes three characteristics: an AP-stream
  (notify) characteristic, a control (write: `0x01` start / `0x02` stop)
  characteristic, and a status (read: AP count + node count) characteristic.

## Known limitations / next steps

- No BLE bonding/encryption or ESP-NOW payload encryption is configured;
  add these if the device will be used somewhere adversarial.
- The phone-side app and any GPS tagging of results are not included here.
- ESP-NOW child nodes assume the master is close enough for reliable
  delivery on all assigned Wi-Fi channels; range/retries can be tuned via
  `esp_now_peer_info_t` and by lowering per-channel dwell time.
