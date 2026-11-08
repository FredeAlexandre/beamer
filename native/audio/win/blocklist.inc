// SPDX-License-Identifier: GPL-3.0-only
// Endpoints the Windows backend never lists, opens, mutes or changes (PHASE2 §2.4 "Blocked devices").
// Included by devices_win.cpp, which defines the three macros. Matches are case-insensitive.
//
// The generic rule in devices_win.cpp already blocks every endpoint whose name, adapter or manufacturer contains
// "Elgato" when its transport is virtual (its devnode chain hangs off ROOT\, not a USB host controller). The entries
// below pin the Elgato Wave Link virtual endpoints found on lebig on 2026-11-08 (Wave Link, driver
// "ElgatoUsbAudioks" by Corsair Memory, Inc.), so they stay blocked even if a driver update renames their adapter
// or reparents it. The physical Elgato Wave:3 (USB\VID_0FD9&PID_0070: "Mic In (2- Elgato Wave:3)" and
// "Headphones (2- Elgato Wave:3)") is NOT blocked.
//
// Inventory, lebig, 2026-11-08 (endpoint name | adapter devnode | chain):
//   capture  Personal Mix (Elgato Virtual Audio) | TUSBAUDIO_ENUM\VID_0FD9&PID_0096&KS\12345678901234 | ROOT\ELGATO_ROOT_DEVICE\0000
//   capture  Stream Mix (Elgato Virtual Audio)   | same
//   capture  Chat Mix (Elgato Virtual Audio)     | same
//   render   System (Elgato Virtual Audio)       | same
//   render   Wave 3 (Elgato Virtual Audio)       | same
//
// BLOCK_NAME(friendly name, exact)
BLOCK_NAME("Personal Mix (Elgato Virtual Audio)")
BLOCK_NAME("Stream Mix (Elgato Virtual Audio)")
BLOCK_NAME("Chat Mix (Elgato Virtual Audio)")
BLOCK_NAME("System (Elgato Virtual Audio)")
BLOCK_NAME("Wave 3 (Elgato Virtual Audio)")
// BLOCK_ADAPTER(prefix of the audio adapter's devnode instance id): Elgato's virtual USB-audio emulation
BLOCK_ADAPTER("TUSBAUDIO_ENUM\\VID_0FD9&PID_0096")
BLOCK_ADAPTER("USB\\VID_0FD9&PID_0096")
// BLOCK_ANCESTOR(prefix of any devnode instance id up the adapter's chain): Elgato's root-enumerated bus
BLOCK_ANCESTOR("ROOT\\ELGATO_ROOT_DEVICE")
