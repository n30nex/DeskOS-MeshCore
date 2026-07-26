MeshCore DeskOS D1L SD data root

This directory is prepared by scripts/prepare_deskos_sd.py.
DeskOS never formats the card. Keep the card FAT32 and do not rename or move
manifest.json.

Runtime data is stored below:
  stores/messages/public
  stores/messages/dm
  stores/nodes
  stores/routes
  stores/packet_log
  exports
  map/tiles

DeskOS installs the included Natural Resources Canada CBMT provider metadata
at map/offline-provider.json when that file is absent. It never replaces a
different provider file already on the card.

Map tiles may be preloaded only from data you are licensed or otherwise
permitted to store for offline use. OpenStreetMap Standard's public tile
servers do not permit background prefetch or offline-area downloads.
