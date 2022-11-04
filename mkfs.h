/******************************************************************************
 * @file            parted.h
 *****************************************************************************/
#ifndef     _PARTED_H
#define     _PARTED_H

#include    <stddef.h>

struct mkfs_state {

    const char *boot, *outfile;
    char label[12];
    
    int create, size_fat, size_fat_by_user, verbose;
    size_t blocks, offset;
    
    unsigned char sectors_per_cluster;

};

extern struct mkfs_state *state;
extern const char *program_name;

#endif      /* _PARTED_H */
