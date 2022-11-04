/******************************************************************************
 * @file            mcopy.h
 *****************************************************************************/
#ifndef     _MCOPY_H
#define     _MCOPY_H

#include    <stddef.h>

struct mcopy_state {

    char **files;
    size_t nb_files;
    
    int status;
    
    const char *outfile;
    size_t offset;

};

#endif      /* _MCOPY_H */
