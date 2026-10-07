# DeskOS D1L 1.9.0

DeskOS 1.9 adds everyday controls to the full-feature D1L interface:

- Standard or Large body text and keyboard characters, saved across restart.
- A paged mention picker that inserts a saved chat contact's advertised name
  at the cursor without replacing the draft or sending a message.
- Contact auto-add rules for chat, repeater, room and sensor roles, plus a hop
  limit. Existing contacts keep updating and a full book never evicts entries.
  Supported phone commands share the same saved policy.
- Optional automatic daylight saving for the contemporary US/Canada and
  Europe/UK rules. Historical messages use their own timestamp; the protocol
  and security clocks remain UTC. Fixed offsets remain the default.

The settings/identity envelope is unchanged from 1.8. Contact policy is stored
separately and bound to the identity, so an identity reset ignores old policy.
Preserving USB updates retain identity, settings and SD data. The firmware
never formats SD. Use the signed package and public checksums from the tagged
release; its record identifies the exact build and physical acceptance.

Mentions are ordinary name-based text, with no guarantee of recipient alerts.
Structured reply identifiers, additional languages/layouts, regional time
rules beyond the two supported sets, optional sensors and the WadaMesh app
suite remain future work. Map attribution and headings retain their dedicated
font sizes. SD throughput and remote telemetry/TRACE responses retain the
limitations described by the release record.

Daylight-saving references: [NRC Canada](https://www.cnrc.canada.ca/en/certifications-evaluations-standards/canadas-official-time/time-zones-daylight-saving-time)
and [UK clock changes](https://www.gov.uk/when-do-the-clocks-change).
