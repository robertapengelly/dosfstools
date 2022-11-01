/******************************************************************************
 * @file            lib.c
 *****************************************************************************/
#include    <ctype.h>
#include    <errno.h>
#include    <limits.h>
#include    <stddef.h>
#include    <stdio.h>
#include    <stdlib.h>
#include    <string.h>

#include    "common.h"
#include    "lib.h"
#include    "mkfs.h"
#include    "report.h"

struct option {

    const char *name;
    int index, flags;

};

#define     OPTION_NO_ARG               0x0001
#define     OPTION_HAS_ARG              0x0002

enum options {

    OPTION_IGNORED = 0,
    OPTION_BLOCKS,
    OPTION_BOOT,
    OPTION_FAT,
    OPTION_HELP,
    OPTION_NAME,
    OPTION_OFFSET,
    OPTION_VERBOSE

};

static struct option opts[] = {

    { "F",          OPTION_FAT,         OPTION_HAS_ARG  },
    { "n",          OPTION_NAME,        OPTION_HAS_ARG  },
    { "v",          OPTION_VERBOSE,     OPTION_NO_ARG   },
    
    { "-boot",      OPTION_BOOT,        OPTION_HAS_ARG  },
    { "-blocks",    OPTION_BLOCKS,      OPTION_HAS_ARG  },
    { "-help",      OPTION_HELP,        OPTION_NO_ARG   },
    { "-offset",    OPTION_OFFSET,      OPTION_HAS_ARG  },
    
    { 0,            0,                  0               }

};

static int strstart (const char *val, const char **str) {

    const char *p = val;
    const char *q = *str;
    
    while (*p != '\0') {
    
        if (*p != *q) {
            return 0;
        }
        
        ++p;
        ++q;
    
    }
    
    *str = q;
    return 1;

}

static void print_help (void) {

    if (!program_name) {
        goto _exit;
    }
    
    fprintf (stderr, "Usage: %s [options] file\n\n", program_name);
    fprintf (stderr, "Options:\n\n");
    
    fprintf (stderr, "    Short options:\n\n");
    fprintf (stderr, "        -F BITS           Select FAT size BITS (12, 16, or 32).\n");
    fprintf (stderr, "        -n LABEL          Set volume label as LABEL (max 11 characters).\n");
    fprintf (stderr, "        -v                Verbose execution.\n");
    
    fprintf (stderr, "\n");
    
    fprintf (stderr, "    Long options:\n\n");
    fprintf (stderr, "        --blocks BLOCKS   Create file target then create the filesystem in it.\n");
    fprintf (stderr, "        --boot FILE       Use FILE as the boot sector.\n");
    fprintf (stderr, "        --help            Show this help information then exit.\n");
    fprintf (stderr, "        --offset SECTOR   Write the filesystem starting at SECTOR.\n");
    
_exit:
    
    exit (EXIT_SUCCESS);

}

static int is_label_char (int ch) {
    return ((ch & 0x80) || (ch == 0x20) || isalnum (ch) || strchr ("!#$%'-@_{}~", ch));
}

char *xstrdup (const char *str) {

    char *ptr = xmalloc (strlen (str) + 1);
    strcpy (ptr, str);
    
    return ptr;

}

int xstrcasecmp (const char *s1, const char *s2) {

    const unsigned char *p1;
    const unsigned char *p2;
    
    p1 = (const unsigned char *) s1;
    p2 = (const unsigned char *) s2;
    
    while (*p1 != '\0') {
    
        if (toupper (*p1) < toupper (*p2)) {
            return (-1);
        } else if (toupper (*p1) > toupper (*p2)) {
            return (1);
        }
        
        p1++;
        p2++;
    
    }
    
    if (*p2 == '\0') {
        return (0);
    } else {
        return (-1);
    }

}

void *xmalloc (size_t size) {

    void *ptr = malloc (size);
    
    if (ptr == NULL && size) {
    
        report_at (program_name, 0, REPORT_ERROR, "memory full (malloc)");
        exit (EXIT_FAILURE);
    
    }
    
    memset (ptr, 0, size);
    return ptr;

}

void *xrealloc (void *ptr, size_t size) {

    void *new_ptr = realloc (ptr, size);
    
    if (new_ptr == NULL && size) {
    
        report_at (program_name, 0, REPORT_ERROR, "memory full (realloc)");
        exit (EXIT_FAILURE);
    
    }
    
    return new_ptr;

}

void dynarray_add (void *ptab, size_t *nb_ptr, void *data) {

    int nb, nb_alloc;
    void **pp;
    
    nb = *nb_ptr;
    pp = *(void ***) ptab;
    
    if ((nb & (nb - 1)) == 0) {
    
        if (!nb) {
            nb_alloc = 1;
        } else {
            nb_alloc = nb * 2;
        }
        
        pp = xrealloc (pp, nb_alloc * sizeof (void *));
        *(void ***) ptab = pp;
    
    }
    
    pp[nb++] = data;
    *nb_ptr = nb;

}

void parse_args (int *pargc, char ***pargv, int optind) {

    char **argv = *pargv;
    int argc = *pargc;
    
    struct option *popt;
    const char *optarg, *r;
    
    if (argc == optind) {
        print_help ();
    }
    
    memcpy (state->label, "NO NAME    ", 11);
    
    while (optind < argc) {
    
        r = argv[optind++];
        
        if (r[0] != '-' || r[1] == '\0') {
        
            if (state->outfile) {
            
                report_at (program_name, 0, REPORT_ERROR, "multiple output files provided");
                exit (EXIT_FAILURE);
            
            }
            
            state->outfile = xstrdup (r);
            continue;
        
        }
        
        for (popt = opts; popt; ++popt) {
        
            const char *p1 = popt->name;
            const char *r1 = (r + 1);
            
            if (!p1) {
            
                report_at (program_name, 0, REPORT_ERROR, "invalid option -- '%s'", r);
                exit (EXIT_FAILURE);
            
            }
            
            if (!strstart (p1, &r1)) {
                continue;
            }
            
            optarg = r1;
            
            if (popt->flags & OPTION_HAS_ARG) {
            
                if (*optarg == '\0') {
                
                    if (optind >= argc) {
                    
                        report_at (program_name, 0, REPORT_ERROR, "argument to '%s' is missing", r);
                        exit (EXIT_FAILURE);
                    
                    }
                    
                    optarg = argv[optind++];
                
                }
            
            } else if (*optarg != '\0') {
                continue;
            }
            
            break;
        
        }
        
        switch (popt->index) {
        
            case OPTION_BLOCKS: {
            
                long conversion;
                char *temp;
                
                errno = 0;
                conversion = strtol (optarg, &temp, 0);
                
                if (!*optarg || isspace ((int) *optarg) || errno || *temp) {
                
                    report_at (program_name, 0, REPORT_ERROR, "bad number for blocks");
                    exit (EXIT_FAILURE);
                
                }
                
                if (conversion < 0) {
                
                    report_at (program_name, 0, REPORT_ERROR, "blocks must be greater than zero");
                    exit (EXIT_FAILURE);
                
                }
                
                state->blocks = conversion;
                break;
            
            }
            
            case OPTION_BOOT: {
            
                state->boot = xstrdup (optarg);
                break;
            
            }
            
            case OPTION_FAT: {
            
                long conversion;
                char *temp;
                
                errno = 0;
                conversion = strtol (optarg, &temp, 0);
                
                if (!*optarg || isspace ((int) *optarg) || errno || *temp) {
                
                    report_at (program_name, 0, REPORT_ERROR, "bad number for fat size");
                    exit (EXIT_FAILURE);
                
                }
                
                switch (conversion) {
                
                    case 12:
                    case 16:
                    case 32:
                    
                        break;
                    
                    default:
                    
                        report_at (program_name, 0, REPORT_ERROR, "fat size can either be 12, 16 or 32 bits");
                        exit (EXIT_FAILURE);
                
                }
                
                state->size_fat = conversion;
                state->size_fat_by_user = 1;
                
                break;
            
            }
            
            case OPTION_HELP: {
            
                print_help ();
                break;
            
            }
            
            case OPTION_NAME: {
            
                int n;
                
                if (strlen (optarg) > 11) {
                
                    report_at (program_name, 0, REPORT_ERROR, "volume label cannot exceed 11 characters");
                    exit (EXIT_FAILURE);
                
                }
                
                for (n = 0; optarg[n] != '\0'; n++) {
                
                    if (!is_label_char (optarg[n])) {
                    
                        report_at (program_name, 0, REPORT_ERROR, "volume label contains an invalid character");
                        exit (EXIT_FAILURE);
                    
                    }
                    
                    state->label[n] = toupper (optarg[n]);
                
                }
                
                break;
            
            }
            
            case OPTION_OFFSET: {
            
                long conversion;
                char *temp;
                
                errno = 0;
                conversion = strtol (optarg, &temp, 0);
                
                if (!*optarg || isspace ((int) *optarg) || *temp || errno) {
                
                    report_at (program_name, 0, REPORT_ERROR, "bad number for offset");
                    exit (EXIT_FAILURE);
                
                }
                
                if (conversion < 0 || (unsigned long) conversion > UINT_MAX) {
                
                    report_at (program_name, 0, REPORT_ERROR, "offset must be between 0 and %u", UINT_MAX);
                    exit (EXIT_FAILURE);
                
                }
                
                state->offset = conversion;
                break;
            
            }
            
            case OPTION_VERBOSE: {
            
                state->verbose++;
                break;
            
            }
            
            default: {
            
                report_at (program_name, 0, REPORT_ERROR, "unsupported option '%s'", r);
                exit (EXIT_FAILURE);
            
            }
        
        }
    
    }

}
