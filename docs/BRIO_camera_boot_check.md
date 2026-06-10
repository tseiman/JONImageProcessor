# BRIO Camera Boot Fix & JON Image Processor Service Setup

## Background: The BRIO Cold-Start Problem

The Logitech BRIO Ultra HD Webcam can fail to enumerate on the USB bus during system cold-start. This is not a driver or permission issue — the camera firmware simply does not respond in time to the USB host controller's enumeration sequence, resulting in kernel errors like:

```
kernel: usb 2-2: Device not responding to setup address.
kernel: usb 2-2: device not accepting address 3, error -71
```

`error -71` is `EPROTO` — a USB protocol error. Once the kernel gives up on a device, it does not retry automatically. A physical reconnect (unplug/replug) recovers the camera, as does a software USB port reset via sysfs.

The fix is a one-shot startup script that checks whether the camera device is present before the main service starts, and performs a single USB reset if it is not. The reset is not retried in a loop — if the camera is genuinely absent, the main service starts anyway and reports the error through its own logging.

---

## Repository Layout

```
etc/
└── systemd/
    └── system/
        ├── brio-startup-check.service   ← one-shot pre-check service
        └── jon-image-processor.service  ← main application service
scripts/
└── brio-startup-check.sh               ← USB reset script
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
    │                       ├── BRIO found on USB bus?  ──► reset, wait, exit 0
    │                       └── BRIO not on bus at all? ──► exit 0 (not connected)
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

if [ -e "$VIDEO_DEV" ]; then
    echo "Camera device $VIDEO_DEV already present, skipping reset"
    exit 0
fi

echo "Camera device $VIDEO_DEV not found, attempting USB reset..."

FOUND=0
for dev in /sys/bus/usb/devices/*/; do
    if [ -f "${dev}idVendor" ] && [ -f "${dev}idProduct" ]; then
        if [ "$(cat ${dev}idVendor)" = "$VENDOR" ] && \
           [ "$(cat ${dev}idProduct)" = "$PRODUCT" ]; then
            echo "Found BRIO at ${dev}, resetting..."
            echo 0 > "${dev}authorized"
            sleep 2
            echo 1 > "${dev}authorized"
            sleep $WAIT_AFTER_RESET
            FOUND=1
            break
        fi
    fi
done

if [ $FOUND -eq 0 ]; then
    echo "BRIO not found on USB bus - camera probably not connected"
    exit 0
fi

for i in $(seq 1 10); do
    if [ -e "$VIDEO_DEV" ]; then
        echo "Camera device $VIDEO_DEV appeared after reset"
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
    --config /opt/JONImageProcessor/etc/jonimageprocessor.json \
    --benchmark --no-daemon
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
Camera device /dev/video0 not found, attempting USB reset...
Found BRIO at /sys/bus/usb/devices/2-2/, resetting...
Camera device /dev/video0 appeared after reset
```

Check the main service:

```bash
journalctl -u jon-image-processor.service
systemctl status jon-image-processor.service
```

---

## Notes

- The script identifies the BRIO by USB Vendor ID `046d` / Product ID `085e`. If you use a different camera, adjust these values. Find them with `lsusb`.
- The USB reset is performed **once only**. There is no retry loop. If the camera does not appear within 10 seconds after the reset, the script exits cleanly and lets the main service decide how to handle the absence.
- This issue is observed on NVIDIA Jetson (Tegra) hardware with a 3 m USB 3 cable. A shorter cable or an active (powered) USB extension may reduce the frequency of the problem at the hardware level.
- The `--no-daemon` flag prevents the process from forking itself, which is required for `Type=simple`. Without it, use `Type=forking`.
