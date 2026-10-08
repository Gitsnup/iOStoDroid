"""Canonical CPU architecture names shared by every stage of the pipeline.

The native analyzer names slices from ``CPU_SUBTYPE_ARM_*``; this module is the
single place that decides which of those names are 32-bit ARM (and therefore
lowerable to ARM64 offline) and in which order a slice is preferred.
"""

from __future__ import annotations

ARM64 = ("arm64",)
ARM64E = ("arm64e",)

#: Every 32-bit ARM slice iOS has ever shipped for phones, oldest first.
ARM32 = (
    "armv4t",
    "armv5tej",
    "armv6",
    "armv6m",
    "armv7",
    "armv7f",
    "armv7s",
    "armv7k",
    "armv7m",
    "armv7em",
    "armv8-32",
    "arm32-unknown",
)

#: Preference order when a FAT image offers several convertible slices.
CONVERTIBLE_ORDER: tuple[str, ...] = (
    "arm64",
    "armv7s",
    "armv7",
    "armv7f",
    "armv7k",
    "armv8-32",
    "armv6",
    "armv5tej",
    "armv4t",
    "armv6m",
    "armv7m",
    "armv7em",
    "arm32-unknown",
)


def is_arm32(architecture: str) -> bool:
    return architecture in ARM32


def is_convertible(architecture: str) -> bool:
    """True when an offline ARM32/ARM64 lowering backend exists for the slice."""
    return architecture in ARM64 or is_arm32(architecture)


def priority(architecture: str) -> int:
    try:
        return CONVERTIBLE_ORDER.index(architecture)
    except ValueError:
        return len(CONVERTIBLE_ORDER)
