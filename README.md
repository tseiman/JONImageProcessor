# JONImageProcessor

JONImageProcessor ist ein C++ Grundprojekt fuer einen Jetson Orin Nano Image Processor. Das Programm liest spaeter ein Kamerabild, verarbeitet es und gibt das Ergebnis fullscreen ueber HDMI oder DisplayPort aus.

Dieser erste Stand stellt die Projektbasis bereit: CMake, OpenCV, Kommandozeilenoptionen, Datei- oder Kamera-Input, eine einfache Dummy-Maske als sichtbares Overlay und Ausgabe in ein Fenster oder eine MP4-Datei.

## Build

Voraussetzungen auf Linux:

- CMake
- C++17 Compiler
- OpenCV mit CMake-Paketdateien

```bash
cmake -B build -S .
cmake --build build
```

Das Executable liegt danach unter:

```bash
./build/JONImageProcessor
```

## Beispielaufrufe

```bash
./build/JONImageProcessor --help
```

```bash
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  --output window
```

```bash
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  --output file \
  --output-file output.mp4
```

```bash
./build/JONImageProcessor \
  --device /dev/video0 \
  --width 1280 \
  --height 720 \
  --mask-width 256 \
  --mask-height 144
```

## Kommandozeilenoptionen

Die Hilfe wird aus derselben zentralen Options-Tabelle erzeugt, die auch fuer `getopt_long` verwendet wird:

```bash
./build/JONImageProcessor --help
```

Wichtige Optionen:

- `--input <path>` liest ein Video aus einer Datei.
- `--device <path>` liest von einem Kamera-Device, Default ist `/dev/video0`.
- `--output window` zeigt ein OpenCV-Fenster.
- `--output file` schreibt eine MP4-Datei.
- `--fullscreen` schaltet das Fenster in fullscreen, wenn `--output window` genutzt wird.
- `--width`, `--height`, `--mask-width` und `--mask-height` konfigurieren Verarbeitungs- und Maskengroessen.

Im Fenstermodus beendet `ESC` oder `q` das Programm sauber.

## Jetson Orin Nano Hinweise

Der aktuelle Stand verwendet bewusst nur CMake, C++17 und OpenCV, damit er auf einer normalen Linux VM und spaeter auf dem Jetson Orin Nano baubar bleibt.

Auf dem Jetson sollten OpenCV und Kamera-Zugriff vor dem Service-Betrieb separat verifiziert werden. Fuer USB-Kameras ist `/dev/video0` der Default. Je nach Kamera, Treiber und Performance-Ziel kann spaeter eine GStreamer-basierte OpenCV-Pipeline sinnvoll sein.

## Geplanter Service-Betrieb

Spaetere Versionen sollen als Linux Service/Daemon per systemd automatisch starten und das verarbeitete Kamerabild fullscreen auf HDMI oder DisplayPort ausgeben.

In diesem ersten Schritt sind noch keine systemd Unit, kein Daemon-Modus und keine Display-spezifische Initialisierung enthalten.

## Planned runtime control

Die Verarbeitungseinstellungen liegen bereits in einer zentralen `ProcessorConfig`-Struktur. Diese Struktur ist bewusst so angelegt, dass sie spaeter zur Laufzeit aktualisiert werden kann.

Geplant ist ein lokaler Unix Domain Socket, der JSON-Kommandos annimmt. Darueber sollen spaeter Einstellungen wie Background Image, Blur Strength, Transparency, Fullscreen und Mask Debug Overlay geaendert werden koennen.

Noch nicht implementiert:

- keine Unix Domain Socket Steuerung
- keine WebAPI
- kein JSON Runtime Protocol

## Testdaten

Das Verzeichnis `testdata/` ist fuer lokale Testvideos vorgesehen und wird nicht pauschal ignoriert.
