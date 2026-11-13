/*
 * Supremacy port - getting the game's data files out of an archive of the
 * WHDLoad install (.lha, as the WHDLoad site and collections ship it, or
 * .zip), or from the original disks as IPF images (SPS release 1384, also
 * inside a .zip): the 12 files the port reads (GAM1, GAM2, INT1, INT2,
 * Main, WIN0-WIN3, the defeat ending, WINM, DIEM) are written to a folder.
 * The IPF tracks are decoded as the WHDLoad install's Supremacy.Imager
 * reads the disks (extract.c has the details); disk 1 gives Main, INT1/2,
 * GAM1/2, disk 2 the endings and their music.
 *
 * LHA: methods -lh0- (stored) and -lh5- (LZ + static Huffman, 8 KB window),
 * header levels 0, 1 and 2, CRC-16 checked. ZIP: stored and deflate, CRC-32
 * checked. Files are matched by name (any folder in the archive), ignoring
 * case. The defeat ending is "WIN." on the Amiga; a name cannot end with a
 * dot on Windows, so it is written as "WIN_" there (the port reads either).
 */
#ifndef SUP_EXTRACT_H
#define SUP_EXTRACT_H

#include <stddef.h>

/* Extract into out_dir (created if needed); files already there count, so
 * the two disks can be given one at a time. 0: all 12 files are there;
 * 1: some came from this disk, msg names those still needed (the other
 * disk); -1: failed, msg says why (an unknown format, a damaged archive or
 * disk, missing files). */
int extract_game_data(const char *archive, const char *out_dir, char *msg, size_t msg_len);

/* 1 if `path` looks like an archive or disk the extractor reads (by its first bytes). */
int extract_is_archive(const char *path);

#endif
