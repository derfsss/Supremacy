/* Supremacy port - ASL file / folder requesters on AmigaOS 4 (see os4_dialog.c). */
#ifndef SUP_OS4_DIALOG_H
#define SUP_OS4_DIALOG_H

#include <stddef.h>

#ifdef __amigaos4__
/* Opens the requester and waits. folders: pick a drawer; many: several files
 * (written one per line). pattern: an AmigaDOS pattern or NULL. Returns 1
 * with the path(s) in `out`, 0 if cancelled, -1 if it cannot open. */
int os4_request(const char *title, const char *initial, int folders, int many,
                const char *pattern, char *out, size_t n);
/* An Intuition requester with an OK button (SDL's message box is avoided on
 * OS4 for the same reason as its file dialog). */
void os4_message(const char *title, const char *text);
#endif

#endif
