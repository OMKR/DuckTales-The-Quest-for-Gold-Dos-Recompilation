#ifndef DISPATCH86_H
#define DISPATCH86_H

#include <stdint.h>

/* img: 0 root, 1..10 overlay, DRIVER_BASE+k driver k; cs: unrelocated
 * segment (0 for drivers); ip: offset */
typedef struct {
    int img;
    uint32_t cs, ip;
    void (*fn)(void);
} Func86;

extern const Func86 func86_table[];
extern const int func86_count;

#endif
