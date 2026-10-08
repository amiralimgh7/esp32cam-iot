# ESP32-CAM IoT Navigation Prototype

Firmware by Amirali Moghadasi for the AI-Thinker ESP32-CAM. A phone connects to the setup access point, configures upstream Wi-Fi and a server address, and chooses a target node (1–15). A FreeRTOS task captures a QVGA JPEG approximately every five seconds while a phone is connected; the local status page shows replies and errors.

## Build

```bash
python -m pip install platformio==6.2.0
python -m platformio run
```

The environment pins `espressif32@6.12.0`, `esp32cam` and ArduinoJson 6.21.5. Flash with `python -m platformio run -t upload` when an appropriate board and serial adapter are connected. Firmware compilation has been verified; physical flashing and camera/network behavior require a board.

## Setup

The default setup AP is `ESP32-CAM-Setup`; its development password is defined in `firmware/espcam/config.h`. Override the `ESPCAM_AP_SSID` and `ESPCAM_AP_PASSWORD` build defines for your device. Connect a phone, open `http://192.168.4.1`, and enter Wi-Fi details and the backend's base URL. The board saves configuration in its Preferences storage. No personal Wi-Fi credentials are included.

## Backend contract

The firmware sends a JPEG (`Content-Type: image/jpeg`) to:

```text
POST /upload?device=esp32_fixed_01&cur=1&target=3&frame=7
```

Replies are JSON with `ok`, `frame`, `current_node`, `current_prob`, `command`, `heading`, `abs_dir` and nullable `next_node`. HTTP errors, invalid JSON and `ok=false` appear in the status UI.

## Local transport demo

```bash
python mock_server.py --port 8080
python -m unittest discover -s tests -v
```

For a board on your LAN, bind the mock explicitly with `--host 0.0.0.0` and configure the board with your computer's LAN address. The mock checks the request contract and JPEG envelope and returns `DEMO_ONLY` with zero confidence. It does not recognize images, compute a route or provide navigation guidance. The perception/navigation service described by the original course proposal was not present in the recovered source.
