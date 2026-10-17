/* Amiga disk image extraction: ADF (directly) and IPF (via CAPSImg). */
#ifndef DISKIMAGE_H
#define DISKIMAGE_H

#include <stddef.h>

#define ADF_SIZE 901120   /* 80 cylinders x 2 heads x 11 sectors x 512 */

/* Read an .adf or .ipf file into a 901120-byte ADF buffer.
 * Returns 1 on success, else 0 with a message in err. */
int disk_read_image(const char *path, unsigned char *adf, char *err, size_t errlen);

/* Extract all files of an AmigaDOS (OFS/FFS) disk into outdir/<volume>/.
 * volume receives the disk name (e.g. "DT1"). Returns the number of files
 * written, or -1 with a message in err. */
int disk_extract(const unsigned char *adf, const char *outdir, char *volume, size_t vollen,
                 char *err, size_t errlen);

#endif
