# BRIO Camera Boot Fix & JON Image Processor Service Setup

## Background: The BRIO Cold-Start Problem

The Logitech BRIO Ultra HD Webcam can fail to enumerate on the USB bus during system cold-start. This is not a driver or permission issue — the camera firmware simply does not respond in time to the USB host controller's enumeration sequence, resulting in kernel errors like:

```
kernel: usb 2-2: Device not responding to setup address.
kernel: usb 2-2: device not accepting address 3, error -71
```

`error -71` is `EPROTO` — a USB protocol error. Once the kernel gives up on a device, it does not retry automatically. A physical reconnect (unplug/replug) recovers the camera, as does a software USB port power-cycle through `uhubctl`.

The fix is a one-shot startup script that checks whether the camera device is present before the main service starts, and power-cycles the known USB hub port if it is not. The reset is not retried in a loop; if the camera is genuinely absent, the main service starts anyway and reports the error through its own logging.

---

## Repository Layout

```
etc/
└── systemd/
    └── system/
        ├── brio-startup-check.service   ← one-shot pre-check service
        └── jon-image-processor.service  ← main application service
scripts/
└── brio-startup-check.sh               ← USB power-cycle script
```

---

## Installation

### 1. Copy the systemd unit files

```bash
sudo cp etc/systemd/system/brio-startup-check.service \
        /etc/systemd/system/brio-startup-check.service

sudo cp etc/systemd/system/jon-image-processor.service \
        /etc/systemd/system/jon-image-processor.service
```

### 2. Copy and enable the startup script

```bash
sudo cp scripts/brio-startup-check.sh /usr/local/bin/brio-startup-check.sh
sudo chmod +x /usr/local/bin/brio-startup-check.sh
```

The script uses `uhubctl`, so install it if it is not already present:

```bash
sudo apt-get install uhubctl
```

### 3. Enable the services

```bash
sudo systemctl daemon-reload
sudo systemctl enable brio-startup-check.service
sudo systemctl enable jon-image-processor.service
```

---

## Service Dependency Chain

```
jon.target
    │
    ├── brio-startup-check.service   (Type=oneshot, runs first)
    │       └── /usr/local/bin/brio-startup-check.sh
    │               ├── /dev/video0 present?  ──► exit 0 (skip)
    │               └── not present?
    │                       ├── locate BRIO by VID:PID with uhubctl
    │                       ├── otherwise use cached hub location/port
    │                       ├── otherwise use hardcoded fallback
    │                       └── power-cycle port, wait, exit 0
    │
    └── jon-image-processor.service  (starts after brio-startup-check completes)
```

`brio-startup-check.service` always exits with code 0 so that `jon-image-processor.service` starts regardless of the camera state. The main process handles a missing camera through its own error reporting.

---

## Files

### `scripts/brio-startup-check.sh`

```bash
#!/bin/bash
VENDOR="046d"
PRODUCT="085e"
VIDEO_DEV="/dev/video0"
WAIT_AFTER_RESET=5
CACHE_FILE="/opt/JONImageProcessor/var/last_brio_port"
DEFAULT_LOCATION="2-1"   # Fallback, falls noch nie erkannt
DEFAULT_PORT="3"

mkdir -p "$(dirname "$CACHE_FILE")"

if [ -e "$VIDEO_DEV" ]; then
    echo "Camera device $VIDEO_DEV already present, skipping reset"
    exit 0
fi

echo "Camera device $VIDEO_DEV not found, trying to locate BRIO..."

# 1. Versuch: dynamisch über VID:PID finden (klappt nur wenn Kamera sauber enumeriert hat)
LOCATION_PORT=$(uhubctl 2>/dev/null | awk -v vid="$VENDOR" -v pid="$PRODUCT" '
  /^Current status for hub/ {
      loc=$5; gsub(/\[.*/,"",loc)
  }
  $0 ~ vid":"pid {
      match($0,/^  Port ([0-9]+):/,m)
      print loc":"m[1]
  }
')

if [ -n "$LOCATION_PORT" ]; then
    echo "BRIO found via VID:PID at $LOCATION_PORT"
    echo "$LOCATION_PORT" > "$CACHE_FILE"
else
    # 2. Versuch: gecachte Position vom letzten erfolgreichen Mal
    if [ -f "$CACHE_FILE" ]; then
        LOCATION_PORT=$(cat "$CACHE_FILE")
        echo "BRIO not enumerated (boot problem) - falling back to cached position $LOCATION_PORT"
    else
        # 3. Letzter Ausweg: fester Default
        LOCATION_PORT="${DEFAULT_LOCATION}:${DEFAULT_PORT}"
        echo "No cache available - falling back to hardcoded default $LOCATION_PORT"
    fi
fi

HUB_LOCATION="${LOCATION_PORT%%:*}"
HUB_PORT="${LOCATION_PORT##*:}"

if [ -z "$HUB_LOCATION" ] || [ -z "$HUB_PORT" ]; then
    echo "Could not determine hub location/port - giving up"
    exit 0
fi

echo "Power-cycling USB port $HUB_LOCATION:$HUB_PORT..."
uhubctl -l "$HUB_LOCATION" -p "$HUB_PORT" -a off
sleep 2
uhubctl -l "$HUB_LOCATION" -p "$HUB_PORT" -a on
sleep "$WAIT_AFTER_RESET"

for i in $(seq 1 10); do
    if [ -e "$VIDEO_DEV" ]; then
        echo "Camera device $VIDEO_DEV appeared after reset"
        # Position bestätigt sich als korrekt -> nochmal sichern
        echo "${HUB_LOCATION}:${HUB_PORT}" > "$CACHE_FILE"
        exit 0
    fi
    sleep 1
done

echo "Camera did not appear after reset - giving up"
exit 0
```

### `etc/systemd/system/brio-startup-check.service`

```ini
[Unit]
Description=BRIO Camera USB Reset Check
After=local-fs.target
Before=jon-image-processor.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/local/bin/brio-startup-check.sh

[Install]
WantedBy=jon.target
```

### `etc/systemd/system/jon-image-processor.service`

```ini
[Unit]
Description=JON Image Processor
After=local-fs.target brio-startup-check.service
Requires=brio-startup-check.service

[Service]
Type=simple
WorkingDirectory=/opt/JONImageProcessor
ExecStart=/opt/JONImageProcessor/bin/JONImageProcessor \
    --config /opt/JONImageProcessor/etc/jonimageprocessor.json
Restart=on-failure
RestartSec=5
StartLimitIntervalSec=60
StartLimitBurst=3
User=jonimageprocessor
Group=jonimageprocessor
SupplementaryGroups=video input render debug

[Install]
WantedBy=jon.target
```

---

## Verifying the fix

Check the startup script output after boot:

```bash
journalctl -u brio-startup-check.service
```

Expected output when the reset was needed and succeeded:

```
Camera device /dev/video0 not found, trying to locate BRIO...
BRIO found via VID:PID at 2-1:3
Power-cycling USB port 2-1:3...
Camera device /dev/video0 appeared after reset
```

Expected output when the cold-start failure prevents VID:PID enumeration but a
cached port is available:

```
Camera device /dev/video0 not found, trying to locate BRIO...
BRIO not enumerated (boot problem) - falling back to cached position 2-1:3
Power-cycling USB port 2-1:3...
Camera device /dev/video0 appeared after reset
```

Check the main service:

```bash
journalctl -u jon-image-processor.service
systemctl status jon-image-processor.service
```

---

## Notes

- The script first identifies the BRIO by USB Vendor ID `046d` / Product ID `085e` through `uhubctl`.
- On successful detection it caches the hub location/port in `/opt/JONImageProcessor/var/last_brio_port`.
- If the camera is in the cold-start failure state and cannot enumerate, the script uses the cached port. If no cache exists, it falls back to `DEFAULT_LOCATION` and `DEFAULT_PORT`.
- The USB power-cycle is performed **once only**. If the camera is missing or unplugged later, JONImageProcessor shows a `Camera DISCONNECTED` test image and periodically tries to reopen the configured camera device after it has been visible for a short settle period. Reconnect is accepted after the reopened V4L2 device delivers a valid frame.
- If you use a different camera or hub port, adjust `VENDOR`, `PRODUCT`, `DEFAULT_LOCATION`, and `DEFAULT_PORT`. Find vendor/product with `lsusb`; inspect controllable hub locations with `uhubctl`.
- If the Jetson kernel does not recreate `/dev/video0` after a USB reconnect, JONImageProcessor cannot recover the device by itself and keeps showing `Camera DISCONNECTED`.
- This issue is observed on NVIDIA Jetson (Tegra) hardware with a 3 m USB 3 cable. A shorter cable or an active (powered) USB extension may reduce the frequency of the problem at the hardware level.
- JONImageProcessor runs as a normal foreground process by default, which is the correct mode for systemd `Type=simple`.
