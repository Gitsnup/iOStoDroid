"""Playable-game conversion: native execution + API translation (no emulation).

This package converts authorized iOS game IPAs whose machine code can run
directly on the device CPU (ARMv6 user ISA on ARMv7-A/AArch32) into installable
Android APKs. The game code bytes are preserved verbatim; a convert-time
rebase map repairs absolute addresses for the load slide, import slots are
bound to a game-driven compatibility runtime, and bundle resources are
repackaged as APK assets. Nothing here emulates a CPU.
"""
