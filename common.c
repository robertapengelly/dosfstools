/******************************************************************************
 * @file            common.c
 *****************************************************************************/
#include    <errno.h>
#include    <stdarg.h>
#include    <stdio.h>
#include    <stdlib.h>
#include    <string.h>
#include    <time.h>

#ifndef     __PDOS__
# if     defined (__GNUC__)
#  include  <sys/time.h>
#  include  <unistd.h>
# else
#  include  <io.h>
# endif
#endif

#include    "common.h"

unsigned short generate_datestamp (void) {

#if     defined (__GNUC__) && !defined (__PDOS__)
    struct timeval create_timeval;
#else
    time_t create_time;
#endif

    struct tm *ctime = NULL;
    
#if     defined (__GNUC__) && !defined (__PDOS__)
    
    if (gettimeofday (&create_timeval, 0) == 0 && create_timeval.tv_sec != (time_t) -1) {
        ctime = localtime ((time_t *) &create_timeval.tv_sec);
    }

#else

    if (time (&create_time) != 0 && create_time != 0) {
        ctime = localtime (&create_time);
    }

#endif
    
    if (ctime != NULL && ctime->tm_year >= 80 && ctime->tm_year <= 207) {
        return (unsigned short) (ctime->tm_mday + ((ctime->tm_mon + 1) << 5) + ((ctime->tm_year - 80) << 9));
    }
    
    return 1 + (1 << 5);

}

unsigned short generate_timestamp (void) {

#if     defined (__GNUC__) && !defined (__PDOS__)
    struct timeval create_timeval;
#else
    time_t create_time;
#endif

    struct tm *ctime = NULL;
    
#if     defined (__GNUC__) && !defined (__PDOS__)
    
    if (gettimeofday (&create_timeval, 0) == 0 && create_timeval.tv_sec != (time_t) -1) {
        ctime = localtime ((time_t *) &create_timeval.tv_sec);
    }

#else

    if (time (&create_time) != 0 && create_time != 0) {
        ctime = localtime (&create_time);
    }

#endif
    
    if (ctime != NULL && ctime->tm_year >= 80 && ctime->tm_year <= 207) {
        return (unsigned short) ((ctime->tm_sec >> 1) + (ctime->tm_min << 5) + (ctime->tm_hour << 11));
    }
    
    return 0;

}
