# LGMouVd

LGMouVd is a mouse-class filter driver for the Looking Glass IDD. It attaches
to the LGInput absolute mouse collection and sets `MOUSE_VIRTUAL_DESKTOP` on
every absolute packet, so the pointer can be placed on any monitor of the
virtual machine instead of the primary only.

## Why it exists

Windows maps a HID absolute mouse onto the primary monitor. With more than one
Looking Glass monitor the IDD can only reach the others by injecting input
through its helper, which does not reach the secure desktop and is visible to
applications as injected. With the filter attached the IDD sends every
monitor's input through the HID mouse: a real input device, covering the
secure desktop and raw input.

The filter is kernel mode and the Looking Glass IDD is not, so it is kept
apart from it. The IDD detects the filter when it is present and needs no
configuration.

## Requirements

* The Looking Glass IDD with the multi-connector input patches:
  https://github.com/gnif/LookingGlass/pull/1343 (which builds on
  https://github.com/gnif/LookingGlass/pull/1342).
* Windows 10 or 11, x64.

## Building

Build `LGMouVd.vcxproj` with Visual Studio 2022 and the Windows Driver Kit
10.0.26100 (KMDF 1.15).

## Signing

A kernel driver only loads on a system with Secure Boot when it has been
signed through Microsoft's attestation program. Releases of this driver are
to be signed under the A2 Creative, Inc. Microsoft Partner Center account.
This has not been completed yet. Until then the driver must be test-signed,
and the guest must have test-signing enabled, which requires Secure Boot to
be off.

## Installing

Install `LGMouVd.inf` with `pnputil`, or attach the filter by hand: register
`LGMouVd.sys` as a kernel service, set `UpperFilters` to `LGMouVd` on the
`HID\LGInput&Col01` device and restart the device. To remove it, delete the
value and restart the device again.

## Limits

* The clipboard stays with the primary connector.
* A connector whose monitor is inactive drops absolute input.
* All connectors share one input device; a lease change on one releases
  buttons held from another.
* Releasing capture returns the host pointer to the window that captured it.
