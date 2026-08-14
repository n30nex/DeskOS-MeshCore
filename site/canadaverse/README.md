# Canadaverse DeskOS page

This directory is the source-controlled deployment bundle for the DeskOS page
at `https://canadaverse.org/deskos/`.

- `splashpage/deskos/` is the complete standalone page and its local assets.
- `splashpage/root/index.html` is the corresponding Canadaverse launcher with
  the DeskOS project card.
- DeskOS requires no new Caddy route, proxy, script, or runtime service.

The page is static and has no analytics, external JavaScript, build step, or
server-side dependency. Deploy it by copying `deskos/` into the splashpage
root and replacing the root `index.html`, then rebuild only the `splashpage`
container. Validate the page and launcher before removing the previous image.
