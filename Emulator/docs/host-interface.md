# Host interface of the kick machine

This is the contract between the QEMU `kick` machine (`Emulator/qemu/hw/kick`) and the
KickEmu host (`Emulator/host`). The host only relies on what is written here; a change on
either side starts with a change to this file.

Every runtime control goes through QMP on `qmp.sock`. QOM paths are absolute. Values are
plain QMP JSON.

## Startup arguments the host passes

| Purpose | Argument |
|---|---|
| eMMC | `-drive if=none,id=emmc,file=<img>,format=raw,discard=unmap` + `-machine kick,emmc=emmc` |
| SD slot | `-drive if=none,id=sd[,file=<img>,format=raw,discard=unmap]` + `-machine kick,sd=sd` |
| Audio backend | `-audiodev <driver>[,options],id=snd0` + `-machine kick,audiodev=snd0` |
| CV link | `-chardev socket,id=cv,...` + `-machine kick,cv-chardev=cv` |
| UARTs | `-serial chardev:mcu -serial chardev:uart2 -serial chardev:midi -serial chardev:console` |

The `sd` drive is always created, with or without a file. Without a file the slot is empty
and a card can be inserted later.

## SD card slot (hot insert and eject)

| Action | QMP |
|---|---|
| Insert an image | `blockdev-change-medium {"device":"sd","filename":"<path>","format":"raw"}` |
| Eject | `eject {"device":"sd","force":true}` |
| Query | `query-block`, entry with `"device":"sd"`: `inserted` present = card in the slot |

Guest-visible behaviour on insert and eject: uSDHC2 `PRES_STATE.CINST`/`CDPL` follow the
medium, `INT_STATUS.CINS`/`CRM` are raised (interrupt if enabled), and the card-detect
GPIO used by the device tree changes level. The guest sees the same sequence as a real
card being pushed in or pulled out.

## Audio (SAI2)

The SAI streams 48 kHz stereo. The audiodev is optional. Independently of it, the SAI can
read its capture input from a WAV file and record its playback output to a WAV file.
These are properties on the machine (`/machine`), settable at startup with `-machine` and
at run time with `qom-set`:

| Property | Type | Meaning |
|---|---|---|
| `audio-wav-in` | string | WAV file fed to the capture path (replaces the audiodev input while set). `""` = off |
| `audio-wav-in-loop` | bool | restart the input file at its end (default false: silence after the end) |
| `audio-wav-out` | string | record the playback path to this file. Setting it starts a new file; `""` closes and finalises it |
| `audio-status` | read-only string (JSON) | `{"out_frames":N,"in_frames":N,"wav_in":"...","wav_in_pos":N,"wav_out":"...","wav_out_frames":N}` |

WAV input accepts PCM 16, 24 and 32 bit and IEEE float 32 bit, mono or stereo (mono is
duplicated), at 48000 Hz. Other rates are rejected with an error from `qom-set`. WAV output
is PCM 32 bit stereo at 48000 Hz (the SAI word as the guest wrote it); the header is
rewritten when the file is closed and on machine exit.

## USB

| Port | QEMU bus | Notes |
|---|---|---|
| USB-A (host) | `usb-a.0` | EHCI behind the USB2 controller; devices attach with `device_add` |
| USB-C (OTG) | `usb-c.0` | Host role only (no gadget emulation) |

Examples: `device_add {"driver":"usb-storage","bus":"usb-a.0","drive":"usbstick","id":"usbstick"}`
after `blockdev-add`, `device_add {"driver":"usb-host","bus":"usb-a.0","vendorid":...,"productid":...,"id":"pass0"}`,
`device_del {"id":"..."}` to unplug.

## Touch

Multitouch through `input-send-event` exactly as `ScreenComponent` sends it: per contact a
`mtt` begin/update/data/end sequence plus a `btn touch` event. Axis range 0..32767 for
both x and y, mapped onto the 1024x600 panel.

## Display

VNC on `vnc.sock` (RFB 3.8, security None). The host reads the panel through it.
