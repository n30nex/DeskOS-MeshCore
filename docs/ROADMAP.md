# DeskOS 1.0 production release roadmap

Execute the highest unblocked row. Issue #71 is the GitHub controller.

| ID | Work | Completion predicate | State / current blocker |
|---|---|---|---|
| D0 | Product scope | `core_1_0` is frozen; deferred features remain deferred | Complete |
| R1 | End-user package | Package and manifest say DeskOS 1.0 and contain no internal qualification material | Complete |
| R2 | Production artifacts | Exact main build yields ESP32 app/full BINs, production RP2040 UF2, checksums, and `START_HERE.md` | Complete |
| R3 | Normal installation | Install the exact package on the attached D1L using the same non-erasing user workflow documented in `START_HERE.md` | Complete |
| R4 | Ordinary use | Ship the runtime-documented dark touch UI for use on the owner's mesh; no controlled peer or lab receipt | Complete |
| R5 | RC1 publication | Publish `v1.0.0-rc.1` with the ZIP, BINs, UF2, checksums, and instructions | Complete |
| R6 | Stable publication | Publish the same artifact bytes as `v1.0.0`, verify public downloads, and close issue #71 | Complete |
| R7 | Explicit install paths | Name the app BIN update, full clean 8 MB BIN, and shared complete RP2040 UF2; provide Windows/Linux commands for both ESP32 paths | In progress |
| R8 | Packaging correction | Publish `v1.0.1` from the exact successful main package with explicit update/fresh asset names and instructions | Blocked by R7 |
