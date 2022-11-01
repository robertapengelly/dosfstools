/******************************************************************************
 * @file            mmd.h
 *****************************************************************************/
#ifndef     _MMD_H
#define     _MMD_H

#include    <stddef.h>

struct mmd_state {

    char **dirs;
    size_t nb_dirs;
    
    const char *outfile;
    unsigned long offset;

};

#endif      /* _MMD_H */
