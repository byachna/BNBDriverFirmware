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
  discovers/coordinates up to eight additional wireless ESP32-C5 nodes
  over ESP-NOW. It splits the full 2.4GHz + 5GHz channel list across every
  known scanner (wired + wireless) and merges all reported APs into one
  deduplicated table (keyed by BSSID) before pushing them to the phone.

- **wifi_scanner_c5** (target `esp32c5`): runs on every ESP32-C5. Build it
  twice — once configured for the **UART** transport (the node wired
  directly to the master) and once for **ESP-NOW** (every wireless child
  node). Select the transport via `idf.py menuconfig` → "Wardrive Scanner
  (ESP32-C5) Configuration". Each node waits for a non-empty channel
  assignment and a `START_SCAN` command, then scans its assigned channels
  and streams `wd_ap_record_t` results back to the master.

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

## Developer guide

- `master_node/main/main.c` wires together BLE, UART, ESP-NOW, aggregation,
  discovery, and periodic node-status maintenance.
- `master_node/main/node_manager.c` tracks scanner liveness and redistributes
  the complete channel list whenever nodes join or become stale. The wired
  scanner is assigned index 0 when it is connected.
- `master_node/main/aggregator.c` deduplicates AP records by BSSID and forwards
  each update to the BLE notification path.
- `master_node/main/ble_gatt.c` implements the NimBLE service and translates
  phone control writes into scanner start/stop commands.
- `wifi_scanner_c5/main/main.c` owns scanner tasks, channel assignments, radio
  locking, and queued AP delivery. AP records are queued while scanning and
  sent after the radio returns to the ESP-NOW control channel. Channel
  assignments are shared between the transport callback and scan task under a
  mutex; they are not passed through the AP-record queue.
- `wifi_scanner_c5/main/wifi_scan.c` performs blocking per-channel scans;
  `espnow_transport.c` and `uart_transport.c` provide interchangeable links.

The master periodically sends discovery requests, and nodes are considered
stale after 15 seconds without a message. A newly discovered node causes the
master to recompute the channel split for all currently live scanners.

### Scanner message queue

The C5 scanner uses a FreeRTOS queue to decouple Wi-Fi scanning from transport
transmission:

1. `wd_wifi_scan_channel()` invokes `enqueue_ap_record()` for each discovered
  network. The callback converts the driver record to `wd_ap_record_t` and
  attempts a non-blocking enqueue into `s_ap_queue`.
2. `s_ap_queue` holds up to 64 AP records. If it is full, the record is
  dropped and the scanner logs `AP queue full, dropping record`; scanning is
  never stalled waiting for the master.
3. `espnow_flush_task` wakes every 500 ms, takes `s_radio_lock`, returns the
  radio to the ESP-NOW control channel with `wd_transport_resync()`, and
  drains the queue by sending `WD_MSG_AP_RECORD` messages.
4. The scan task and flush task share `s_radio_lock`, so channel scans and
  queued transmissions cannot change the single Wi-Fi radio concurrently.

The same queue is used when the scanner is configured for UART. UART does not
need channel resynchronization, but the periodic flush task and queue keep
the scanner behavior consistent across both transport modes. The queue is
intentionally best-effort: the master receives records in batches, and a
temporary backlog can cause individual AP records to be lost.

## Known limitations / next steps

- No BLE bonding/encryption or ESP-NOW payload encryption is configured;
  add these if the device will be used somewhere adversarial.
- The phone-side app and any GPS tagging of results are not included here.
- ESP-NOW child nodes assume the master is close enough for reliable
  delivery on all assigned Wi-Fi channels; range/retries can be tuned via
  `esp_now_peer_info_t` and by lowering per-channel dwell time.
