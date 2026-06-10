#!/bin/bash

# ###################################################
# On my NVIDA Jetson Nano Orin the Logitech BRIO is 
# not always enumerated therefore this service is 
# resetting 1 time  the bus to recover
# ###################################################

VENDOR="046d"
PRODUCT="085e"
VIDEO_DEV="/dev/video0"
WAIT_AFTER_RESET=5

# Kamera-Device schon da?
if [ -e "$VIDEO_DEV" ]; then
    echo "Camera device $VIDEO_DEV already present, skipping reset"
    exit 0
fi

echo "Camera device $VIDEO_DEV not found, attempting USB reset..."

# BRIO im sysfs suchen und resetten
FOUND=0
for dev in /sys/bus/usb/devices/*/; do
    if [ -f "${dev}idVendor" ] && [ -f "${dev}idProduct" ]; then
        if [ "$(cat ${dev}idVendor)" = "$VENDOR" ] && [ "$(cat ${dev}idProduct)" = "$PRODUCT" ]; then
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
    echo "BRIO not found on USB bus at all - camera probably not connected"
    exit 0  # exit 0 damit der Service trotzdem startet (und dann selbst mit Fehler abbricht)
fi

# Warten bis /dev/video0 erscheint (max 10 Sekunden)
for i in $(seq 1 10); do
    if [ -e "$VIDEO_DEV" ]; then
        echo "Camera device $VIDEO_DEV appeared after reset"
        exit 0
    fi
    sleep 1
done

echo "Camera did not appear after reset - giving up"
exit 0  # exit 0 - JON starts anyway and reports the non existing cam in its own
