# JONImageProcessor – Release Notes 2.0.0

## Datum: 1. Juli 2026

Release: `2.0.0`

Funktionierender Code-Checkpoint: `8f6d7ae`

Hinweis: Der Git-Tag `2.0.0` zeigt auf den Release-Notes-Commit. Der funktionierende
AirPlay-Code-Stand ist `8f6d7ae`; die späteren Commits bis zum Tag ändern nur die
Release-Dokumentation.

## Was funktioniert

- **AirPlay-Verbindung stabil:** uxplay läuft mit `-vrtp`, hält die Session dauerhaft,
  kein Re-Connect bei Pipeline-Neustarts in JIP
- **Desktop-Mirror:** Funktioniert gut, keine wahrnehmbaren Störungen
- **Vollbild-Foto/Video:** Funktioniert grundsätzlich, Wechsel Mirror↔Vollbild klappt
- **Auflösungswechsel-Recovery:** Caps-Change-Erkennung, IDR-Caching, Pipeline-B-Neustart
  funktionieren wie designed
- **IDR-basierter Recovery:** Wenn Stream sich nach Video→Bild-Wechsel erholt,
  wird der neue IDR erkannt und der Decoder neu gestartet
- **Session-Ende-Erkennung:** Nach 15s ohne Samples wird Statusbild gezeigt
- **Letterboxing:** Korrekte Aspect-Ratio-Behandlung für alle Seitenverhältnisse
- **Architektur:** Geteilte Pipeline (A stabil, B instabil) mit appsrc/appsink-Brücke

## Bekannte verbleibende Probleme

### 1. Bildqualität bei hoher CPU-Last (130-150%)
Bei vollbewegten Vollbild-Videos oder schnellen Übergängen steigt die CPU-Last
stark an (`nvv4l2decoder`-Thread ~66%, Haupt-Loop ~35%). Bei dieser Last treten
Artefakte auf die mit dem H264-Dump korrelieren – der Stream selbst ist dann
bereits kaputt (Paketverlust/RTP-Probleme).

### 2. Mirror nach Vollbild (Non-IDR-Fallback schlägt fehl)
Der Rückwechsel Vollbild→Mirror ohne IDR landet in einer Fallback-Schleife
(`no valid frames decoded before end of stream`). Funktioniert nur wenn der
nächste echte IDR kommt (z.B. beim nächsten Moduswechsel oder IDR-Recovery).

### 3. Statische Bilder / Session-Timeout
AirPlay sendet bei statischen Bildern keine Frames – nach 15s erscheint das
Statusbild. Akzeptables Verhalten, kein echter Bug.

## Aktueller Git-Stand

Release-Tag: `2.0.0`
Code-Checkpoint: `8f6d7ae` nach IDR-Recovery + UDP-Buffer 8MB Patch
Branch: main

## uxplay-Konfiguration (produktiv)

```bash
/usr/local/bin/uxplay -nc -n "JONImageProcessor v9" -fps 20 \
  -vrtp "config-interval=-1 ! udpsink host=127.0.0.1 port=5004" \
  -as 0 -reset 0 -nohold
```

## JIP Pipeline-Architektur (aktuell)

```
Pipeline A (stabil, läuft immer):
udpsrc port=5004 buffer-size=8388608 !
rtpjitterbuffer latency=500 !
rtph264depay !
h264parse name=rtp_parse !
appsink name=rtp_sink

Pipeline B (instabil, wird bei Caps-Change/IDR-Recovery neu gestartet):
appsrc name=decoder_src !
h264parse !
nvv4l2decoder !
nvvidconv !
video/x-raw,format=I420 !
videoconvert !
video/x-raw,format=BGR !
appsink name=airplay_sink
```

## Nächste mögliche Schritte (priorisiert)

### A. BGRx-Optimierung (einfach, geringes Risiko)
```
nvv4l2decoder ! nvvidconv ! video/x-raw,format=BGRx ! videoconvert ! video/x-raw,format=BGR
```
Spart CPU indem der I420-Zwischenschritt entfällt. Risiko: könnte das
NvBufSurfaceCopy-Verhalten ändern. Rollback: eine Zeile zurückändern.

### B. AirPlay als Live-Hintergrund (großer nächster Schritt)
`PauseCameraSource` auch im aktiven Kamera-Modus laufen lassen und den
AirPlay-Frame als Hintergrund für das Compositing mit der Presenter-Kamera nutzen.
Das ist der eigentliche Anwendungsfall: Presenter vor seiner AirPlay-Präsentation.
Architektur existiert bereits (applyBackgroundImage, TensorRT-Maske).

### C. Mirror-Fallback ohne IDR verbessern
Warten auf uxplay-seitige Lösung oder andere Strategie. Aktuell: Fallback
schlägt fehl, nächster IDR (Moduswechsel) recovert. Akzeptabel als Zwischenzustand.

## Empfehlung

Aktuellen Stand als stabilen Checkpoint betrachten. Schritt B (Live-Hintergrund)
ist der eigentliche Anwendungsfall und sollte als nächstes angegangen werden,
da die Architektur bereits vorhanden ist. Schritt A vorher testen falls CPU-Last
weiterhin ein Problem ist.
