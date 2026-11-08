// SPDX-License-Identifier: GPL-3.0-only
// Exact device names that the macOS backend never lists, opens, mutes or changes (PHASE2 §2.4),
// in addition to the rule "name or manufacturer contains Elgato and the transport is virtual".
// An entry only blocks a device whose transport is virtual: Elgato's physical USB microphones
// (e.g. "Wave:3") stay allowed. Wave Link's virtual channels on the Mac, should the manufacturer
// string ever not say Elgato:
"Wave Link Stream",
"Wave Link Monitor",
"Wave Link MicrophoneFX",
"Wave Link System",
"Wave Link Music",
"Wave Link Browser",
"Wave Link Voice Chat",
"Wave Link Game",
"Wave Link SFX",
"Wave Link Aux 1",
"Wave Link Aux 2",
