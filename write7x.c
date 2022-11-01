/******************************************************************************
 * @file            write7x.c
 *****************************************************************************/
#include    "write7x.h"

void write721_to_byte_array (unsigned char *dest, unsigned short val) {

    dest[0] = (val & 0xFF);
    dest[1] = (val >> 8) & 0xFF;

}

void write741_to_byte_array (unsigned char *dest, unsigned int val) {

    dest[0] = (val & 0xFF);
    dest[1] = (val >> 8) & 0xFF;
    dest[2] = (val >> 16) & 0xFF;
    dest[3] = (val >> 24) & 0xFF;

}
