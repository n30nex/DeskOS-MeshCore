#pragma once

#include "mesh/draft_store.h"

bool d1l_compose_clipboard_copy(const char *text);
const char *d1l_compose_clipboard_text(void);
void d1l_compose_clipboard_clear(void);
bool d1l_compose_text_fits(const char *draft, const char *addition);
/* Plain-text quotation is readable by existing MeshCore clients. The excerpt
 * is shortened at UTF-8 boundaries, leaving room for the user's reply. */
bool d1l_compose_quote(char *out, size_t capacity, const char *author, const char *text);
