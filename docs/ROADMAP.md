# DeskOS 1.0 production release roadmap

Execute the highest unblocked row. Issue #71 is the GitHub controller.

| ID | Work | Completion predicate | State / current blocker |
|---|---|---|---|
| D0 | Product scope | `core_1_0` is frozen; deferred features remain deferred | Complete |
| R1 | End-user package | Package and manifest say DeskOS 1.0 and contain no internal qualification material | In progress |
| R2 | Production artifacts | Exact main build yields ESP32 app/full BINs, production RP2040 UF2, checksums, and `START_HERE.md` | Blocked by R1 |
| R3 | Normal installation | Install the exact package on the attached D1L using the same non-erasing user workflow documented in `START_HERE.md` | Blocked by R2 |
| R4 | Ordinary use | Confirm boot, touch UI, navigation, Public messaging, settings, and configured Map as normal product use; no controlled peer or lab receipt | Blocked by R3 |
| R5 | RC1 publication | Publish `v1.0.0-rc.1` with the ZIP, BINs, UF2, checksums, and instructions | Blocked by R4 |
| R6 | Stable publication | Publish the same artifact bytes as `v1.0.0`, verify public downloads, and close issue #71 | Blocked by R5 |
