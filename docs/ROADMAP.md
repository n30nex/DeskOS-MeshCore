# DeskOS 1.0 RC1 roadmap

Execute the highest unblocked row. Issue #71 is the GitHub controller.

| ID | Work | Completion predicate | State / current blocker |
|---|---|---|---|
| D0 | Authority reset | This PR merged; legacy authorities inactive | Complete on merge |
| R1 | Freeze exact main package | Successful `push` run on `main`; artifacts downloaded and checksums verified | Next after D0 |
| R2 | Exact flash source | One non-erasing exact app flash; correct by-id/VID/PID; settings retained; no SD format | Blocked by R1 |
| R3 | RF source | Controlled bidirectional DM with truthful ACK and required route behavior | Blocked by R2 |
| R4 | Protocol source | Boot advert, one Public send, PATH, Ping, repeater login/query | Blocked by R2 |
| R5 | Map source | Authorized fresh tile download plus cache revisit from SD | Blocked by R2 |
| R6 | Aggregate and release | Four sources match final candidate; RC1 audit true; prerelease assets published | Blocked by R2–R5 |
