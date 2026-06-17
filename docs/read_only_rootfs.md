# Read-Only Root Filesystem Configuration for NVIDIA Jetson Orin Nano

## Goal

The objective is to make the Jetson system more resilient against sudden power loss.

The operating system is mounted read-only, while application data remains writable on a dedicated data partition.

## Final Filesystem Layout

| Mount Point                  | Filesystem | Mode       | Purpose                   |
| ---------------------------- | ---------- | ---------- | ------------------------- |
| `/`                          | ext4       | Read-Only  | Operating system          |
| `/opt/JONImageProcessor/var` | ext4       | Read-Write | Application data          |
| `/tmp`                       | tmpfs      | RAM        | Temporary files           |
| `/run`                       | tmpfs      | RAM        | Runtime files and sockets |
| Journald                     | tmpfs      | RAM        | System logs               |
| `/boot/efi`                  | vfat       | Read-Write | EFI partition             |

## Partitioning

The original root partition occupied the entire SD card.

The root partition was reduced in size and a second ext4 partition was created for application data.

Example layout:

| Partition         | Mount Point                  | Purpose          |
| ----------------- | ---------------------------- | ---------------- |
| `/dev/mmcblk0p1`  | `/`                          | Operating system |
| `/dev/mmcblk0p16` | `/opt/JONImageProcessor/var` | Application data |

## Data Partition

The data partition was labeled:

```bash
sudo e2label /dev/mmcblk0p16 jondata
```

Filesystem type:

```text
ext4
```

## /etc/fstab

Example configuration:

```fstab
LABEL=rootfs / ext4 ro,noatime 0 1

UUID=4EA2-9257 /boot/efi vfat defaults 0 1

LABEL=jondata /opt/JONImageProcessor/var ext4 defaults,noatime,nofail 0 0

tmpfs /tmp tmpfs defaults,nosuid,nodev,mode=1777,size=256M 0 0
```

## Journald Configuration

Create:

```text
/etc/systemd/journald.conf.d/volatile.conf
```

Contents:

```ini
[Journal]
Storage=volatile
RuntimeMaxUse=64M
```

Restart journald:

```bash
sudo systemctl restart systemd-journald
```

Verification:

```bash
journalctl --header | head
```

Expected output:

```text
File path: /run/log/journal/...
```

## Verification

Verify root filesystem is mounted read-only:

```bash
mount | grep ' on / '
```

Expected:

```text
/dev/mmcblk0p1 on / type ext4 (ro,...)
```

Verify application data partition is writable:

```bash
touch /opt/JONImageProcessor/var/testfile
```

Verify root filesystem is read-only:

```bash
touch /etc/test
```

Expected:

```text
touch: Read-only file system
```

## Current Result

The operating system is protected against accidental modifications and most power-loss related filesystem corruption.

Writable data is restricted to:

```text
/opt/JONImageProcessor/var
```

Runtime data is stored in RAM:

```text
/tmp
/run
journald
```


