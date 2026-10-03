# Security boundary

Only authorized, unprotected inputs may be processed. Authorization is confirmed in UI/CLI; it is not a license verification mechanism. Encrypted Mach-O slices are rejected before reconstruction, including encrypted secondary slices/images. This project does not decrypt FairPlay, patch DRM checks, or validate Apple code signatures as a source of trust.

Input limits: 512 MiB archive, 1 GiB expanded data, 256 MiB per member/executable, 20,000 members, 250:1 maximum member expansion ratio, 8 MiB plist, bounded paths/depth. PNG dimensions are limited to 4096²; CgBI inflation is bounded. Native counts/ranges, ULEB overflow, export trie recursion and FAT overlaps are checked. Android ZIP64 archives are explicitly unsupported.

Extraction occurs under newly created private workspaces; links/special files, traversal, absolute paths, case collisions and encrypted archives are rejected. Partial extraction is removed on failure. Original executables are only read, decoded and analyzed, never executed on the conversion host. The current backend excludes memory/branches/syscalls/imports by proof, not by hoping they work on Android.

The final APK excludes Apple executables, signatures and the IPA. Development signing is not a security endorsement of the source. Source resources may be malicious; resource copying does not establish trust. Bundle data remains opaque unless a bounded converter explicitly handles it. Native analysis is memory-safe-by-validation C++, not a formal proof of parser correctness; fuzz/sanitizer testing is advisable for production deployment.

Host conversion runs with the invoking user's privileges, not a kernel/container sandbox. Do not run as root or use a workspace writable by another untrusted process. Private-directory ownership is assumed during filesystem operations; concurrent same-UID adversaries and local process tampering are outside this boundary. Production multi-user deployments should add an unprivileged container/seccomp/resource-limited worker around the CLI.

The Android app has no INTERNET permission and does not upload IPAs. APK attachments get identity/provenance checks, but full host signing validation and Android package installer verification remain necessary. An attached result does not mark the importer report READY. Content-provider access is read-only and URI-granted to the installer for a constrained internal file path.
