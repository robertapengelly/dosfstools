/******************************************************************************
 * @file            mcopy.c
 *****************************************************************************/
#include    <ctype.h>
#include    <errno.h>
#include    <limits.h>
#include    <stdio.h>
#include    <stdlib.h>
#include    <string.h>

#ifndef     __PDOS__
# if    defined (_WIN32)
#  include  <windows.h>
#  include  <winioctl.h>
# elif defined (__GNUC__)
#  include  <sys/ioctl.h>
#  include  <termios.h>
#  include  <unistd.h>
#  if   defined (__CYGWIN__)
#   include  <sys/socket.h>
#  endif
# endif
#endif

#include    "common.h"
#include    "mcopy.h"
#include    "msdos.h"
#include    "report.h"
#include    "write7x.h"

#ifndef     PATH_MAX
# define    PATH_MAX                    2048
#endif

static FILE *ofp;

struct dir_info {

    unsigned int current_cluster;
    
    unsigned char current_sector;
    unsigned char current_entry;
    unsigned char *scratch;
    unsigned char flags;

};

struct file_info {

    unsigned char dir_offset;
    unsigned int dir_sector;
    
    unsigned char *scratch;
    unsigned int filelen;
    
    unsigned int cluster;
    unsigned int pointer;
    
    unsigned long bytes;

};

static struct mcopy_state *state = 0;
static const char *program_name = 0;

static struct msdos_boot_sector bs;
static int size_fat = 0;

static unsigned int cluster_count = 0;
static unsigned int data_area = 0;
static unsigned int info_sector = 0;
static unsigned int number_of_fats = 0;
static unsigned int reserved_sectors = 0;
static unsigned int root_cluster = 0;
static unsigned int root_dir = 0;
static unsigned int root_entries = 0;
static unsigned int sectors_per_cluster = 0;
static unsigned int sectors_per_fat = 0;
static unsigned int total_sectors = 0;

struct option {

    const char *name;
    int index, flags;

};

#define     OPTION_NO_ARG               0x0001
#define     OPTION_HAS_ARG              0x0002

enum options {

    OPTION_IGNORED = 1,
    OPTION_HELP,
    OPTION_INPUT,
    OPTION_OFFSET

};

static struct option opts[] = {

    { "i",          OPTION_INPUT,       OPTION_HAS_ARG  },
    
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

static void print_help (int exitval) {

    if (!program_name) {
        goto _exit;
    }
    
    fprintf (stderr, "Usage: %s [options] [::]source-file [::]target-file\n\n", program_name);
    fprintf (stderr, "Options:\n\n");
    
    fprintf (stderr, "    Short options:\n\n");
    fprintf (stderr, "        -i                Specify the input target.\n");
    
    fprintf (stderr, "\n");
    
    fprintf (stderr, "    Long options:\n\n");
    fprintf (stderr, "        --help            Show this help information then exit.\n");
    fprintf (stderr, "        --offset SECTOR   Write the filesystem starting at SECTOR.\n");
       
_exit:
    
    exit (exitval);

}

static void *xmalloc (size_t size) {

    void *ptr = malloc (size);
    
    if (ptr == NULL && size) {
    
        report_at (program_name, 0, REPORT_ERROR, "memory full (malloc)");
        exit (EXIT_FAILURE);
    
    }
    
    memset (ptr, 0, size);
    return ptr;

}

static void *xrealloc (void *ptr, size_t size) {

    void *new_ptr = realloc (ptr, size);
    
    if (new_ptr == NULL && size) {
    
        report_at (program_name, 0, REPORT_ERROR, "memory full (realloc)");
        exit (EXIT_FAILURE);
    
    }
    
    return new_ptr;

}

static char *xstrdup (const char *str) {

    char *ptr = xmalloc (strlen (str) + 1);
    strcpy (ptr, str);
    
    return ptr;

}

static void dynarray_add (void *ptab, size_t *nb_ptr, void *data) {

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

static void parse_args (int *pargc, char ***pargv, int optind) {

    char **argv = *pargv;
    int argc = *pargc;
    
    struct option *popt;
    const char *optarg, *r;
    
    if (argc == optind) {
        print_help (EXIT_SUCCESS);
    }
    
    while (optind < argc) {
    
        r = argv[optind++];
        
        if (r[0] != '-' || r[1] == '\0') {
        
            dynarray_add (&state->files, &state->nb_files, xstrdup (r));
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
        
            case OPTION_HELP: {
            
                print_help (EXIT_SUCCESS);
                break;
            
            }
            
            case OPTION_INPUT: {
            
                if (state->outfile) {
                
                    report_at (program_name, 0, REPORT_ERROR, "multiple output files provided");
                    exit (EXIT_FAILURE);
                
                }
                
                state->outfile = xstrdup (optarg);
                break;
            
            }
            
            case OPTION_OFFSET: {
            
                long conversion;
                char *temp;
                
                errno = 0;
                conversion = strtol (optarg, &temp, 0);
                
                if (!*optarg || isspace ((int) *optarg) || errno || *temp) {
                
                    report_at (program_name, 0, REPORT_ERROR, "bad number for offset (%s)", optarg);
                    exit (EXIT_FAILURE);
                
                }
                
                if (conversion < 0 || (unsigned long) conversion > UINT_MAX) {
                
                    report_at (program_name, 0, REPORT_ERROR, "offset must be between 0 and %u", UINT_MAX);
                    exit (EXIT_FAILURE);
                
                }
                
                state->offset = (unsigned long) conversion;
                break;
            
            }
            
            default: {
            
                report_at (program_name, 0, REPORT_ERROR, "unsupported option '%s'", r);
                exit (EXIT_FAILURE);
            
            }
        
        }
    
    }

}


#ifndef     __PDOS__
/** Get a single character from the terminal. */
static char getch () {

#if     defined(_WIN32) && !defined (__PDOS__)

    KEY_EVENT_RECORD keyevent;
    INPUT_RECORD irec;
    DWORD events;
    
    for (;;) {
    
        ReadConsoleInput (GetStdHandle (STD_INPUT_HANDLE), &irec, 1, &events);
        
        if (irec.EventType == KEY_EVENT && ((KEY_EVENT_RECORD) irec.Event.KeyEvent).bKeyDown) {
        
            keyevent = (KEY_EVENT_RECORD) irec.Event.KeyEvent;
            
            const int ca = (int) keyevent.uChar.AsciiChar;
            const int cv = (int) keyevent.wVirtualKeyCode;
            const int key = ca == 0 ? -cv : ca + (ca > 0 ? 0 : 256);
            
            switch (key) {
            
                case  -16: continue;    /* disable Shift */
                case  -17: continue;    /* disable Ctrl / AltGr */
                case  -18: continue;    /* disable Alt / AltGr */
                case -220: continue;    /* disable first detection of "^" key (not "^" symbol) */
                case -221: continue;    /* disable first detection of "'" key (not "'" symbol) */
                case -191: continue;    /* disable AltGr + "#" */
                case  -52: continue;    /* disable AltGr + "4" */
                case  -53: continue;    /* disable AltGr + "5" */
                case  -54: continue;    /* disable AltGr + "6" */
                case  -12: continue;    /* disable num block 5 with num lock deactivated */
                case   13: return  10;  /* enter */
                case  -46: return 127;  /* delete */
                case  -49: return 251;  /* Â¹ */
                case    0: continue;
                case    1: continue;    /* disable Ctrl + a (selects all text) */
                case    2: continue;    /* disable Ctrl + b */
                case    3: continue;    /* disable Ctrl + c (terminates program) */
                case    4: continue;    /* disable Ctrl + d */
                case    5: continue;    /* disable Ctrl + e */
                case    6: continue;    /* disable Ctrl + f (opens search) */
                case    7: continue;    /* disable Ctrl + g */
                case   10: continue;    /* disable Ctrl + j */
                case   11: continue;    /* disable Ctrl + k */
                case   12: continue;    /* disable Ctrl + l */
                case   14: continue;    /* disable Ctrl + n */
                case   15: continue;    /* disable Ctrl + o */
                case   16: continue;    /* disable Ctrl + p */
                case   17: continue;    /* disable Ctrl + q */
                case   18: continue;    /* disable Ctrl + r */
                case   19: continue;    /* disable Ctrl + s */
                case   20: continue;    /* disable Ctrl + t */
                case   21: continue;    /* disable Ctrl + u */
                case   22: continue;    /* disable Ctrl + v (inserts clipboard) */
                case   23: continue;    /* disable Ctrl + w */
                case   24: continue;    /* disable Ctrl + x */
                case   25: continue;    /* disable Ctrl + y */
                case   26: continue;    /* disable Ctrl + z */
                default: return key;    /* any other ASCII/virtual character */
            
            }
        
        }
    
    }

#else

#if     defined (__PDOS__)

    setvbuf (stdin, NULL, _IONBF, 0);
    
    for (;;) {

#else

    struct termios term;
    tcgetattr (0, &term);
    
    for (;;) {
    
        term.c_lflag &= ~(ICANON | ECHO);                   /* turn off line buffering and echoing */
        tcsetattr (0, TCSANOW, &term);
        
        int nbbytes;
        ioctl (0, FIONREAD, &nbbytes);                      /* 0 is STDIN */
        
        while (!nbbytes) {
        
            sleep (1);
            fflush (stdout);
            
            ioctl (0, FIONREAD, &nbbytes);                  /* 0 is STDIN */
        
        }

#endif
        
        int key = (int) getchar ();
        
        if (key == 27 || key == 194 || key == 195) {        /* escape, 194/195 is escape for Â°ÃŸÂ´Ã¤Ã¶Ã¼Ã„Ã–Ãœ */
        
            key = (int) getchar ();
            
            if (key == 91) {                                /* [ following escape */
            
                key = (int) getchar ();                     /* get code of next char after \e[ */
                
                if (key == 49) {                            /* F5-F8 */
                
                    key = 62 + (int) getchar ();            /* 53, 55-57 */
                    
                    if (key == 115) {
                        key++;                              /* F5 code is too low by 1 */
                    }
                    
                    getchar ();                             /* take in following ~ (126), but discard code */
                
                } else if (key == 50) {                     /* insert or F9-F12 */
                
                    key = (int) getchar ();
                    
                    if (key == 126) {                       /* insert */
                        key = 45;
                    } else {                                /* F9-F12 */
                    
                        key += 71;                          /* 48, 49, 51, 52 */
                        
                        if (key < 121) {
                            key++;                          /* F11 and F12 are too low by 1 */
                        }
                        
                        getchar ();                         /* take in following ~ (126), but discard code */
                    
                    }
                
                } else if (key == 51 || key == 53 || key == 54) {               /* delete, page up/down */
                    getchar ();                             /* take in following ~ (126), but discard code */
                }
            
            } else if (key == 79) {                         /* F1-F4 */
                key = 32 + (int) getchar ();                /* 80-83 */
            }
            
            key = -key;                                     /* use negative numbers for escaped keys */
        
        }
        
#if     defined (__PDOS__)
        setvbuf (stdin, NULL, _IOLBF, 0);
#else

        term.c_lflag |= (ICANON | ECHO);                    /* turn on line buffering and echoing */
        tcsetattr (0, TCSANOW, &term);

#endif
        
        switch (key) {
        
            case  127: return (char)   8;   /* backspace */
            case  -27: return (char)  27;   /* escape */
            case  -51: return (char) 127;   /* delete */
            case -164: return (char) 132;   /* Ã¤ */
            case -182: return (char) 148;   /* Ã¶ */
            case -188: return (char) 129;   /* Ã¼ */
            case -132: return (char) 142;   /* Ã„ */
            case -150: return (char) 153;   /* Ã– */
            case -156: return (char) 154;   /* Ãœ */
            case -159: return (char) 225;   /* ÃŸ */
            case -181: return (char) 230;   /* Âµ */
            case -167: return (char) 245;   /* Â§ */
            case -176: return (char) 248;   /* Â° */
            case -178: return (char) 253;   /* Â² */
            case -179: return (char) 252;   /* Â³ */
            case -180: return (char) 239;   /* Â´ */
            case  -65: return (char) -38;   /* up arrow */
            case  -66: return (char) -40;   /* down arrow */
            case  -68: return (char) -37;   /* left arrow */
            case  -67: return (char) -39;   /* right arrow */
            case  -53: return (char) -33;   /* page up */
            case  -54: return (char) -34;   /* page down */
            case  -72: return (char) -36;   /* pos1 */
            case  -70: return (char) -35;   /* end */
            case    0: continue;
            case    1: continue;            /* disable Ctrl + a */
            case    2: continue;            /* disable Ctrl + b */
            case    3: continue;            /* disable Ctrl + c (terminates program) */
            case    4: continue;            /* disable Ctrl + d */
            case    5: continue;            /* disable Ctrl + e */
            case    6: continue;            /* disable Ctrl + f */
            case    7: continue;            /* disable Ctrl + g */
            case    8: continue;            /* disable Ctrl + h */
            case   11: continue;            /* disable Ctrl + k */
            case   12: continue;            /* disable Ctrl + l */
            case   13: continue;            /* disable Ctrl + m */
            case   14: continue;            /* disable Ctrl + n */
            case   15: continue;            /* disable Ctrl + o */
            case   16: continue;            /* disable Ctrl + p */
            case   17: continue;            /* disable Ctrl + q */
            case   18: continue;            /* disable Ctrl + r */
            case   19: continue;            /* disable Ctrl + s */
            case   20: continue;            /* disable Ctrl + t */
            case   21: continue;            /* disable Ctrl + u */
            case   22: continue;            /* disable Ctrl + v */
            case   23: continue;            /* disable Ctrl + w */
            case   24: continue;            /* disable Ctrl + x */
            case   25: continue;            /* disable Ctrl + y */
            case   26: continue;            /* disable Ctrl + z (terminates program) */
            default: return key;            /* any other ASCII character */
        
        }
    
    }

#endif

}
#endif

static int check_overwrite (const char *fn) {

#if     defined (__PDOS__)

    report_at (NULL, 0, REPORT_WARNING, "ovewriting %s", fn);
    return 1;

#else

    char ch;
    
    printf ("File %s already exists.\n", fn);
    printf ("o)verrwrite s)skip (os): ");
    
    while ((ch = tolower (getch ())) < 0) {}
    
    if (isspace (ch)) {
        printf ("\n");
    } else {
        printf ("%c\n", ch);
    }
    
    if (ch == 'o') {
        return 1;
    } else if (ch == 's') {
        return 0;
    }
    
    return check_overwrite (fn);

#endif

}


static int seekto (long offset) {
    return fseek (ofp, (state->offset * 512) + offset, SEEK_SET);
}


static int get_next_entry (struct dir_info *di, struct msdos_dirent *de);
static int open_dir (const char *target, struct dir_info *di);

static unsigned int get_fat_entry (unsigned char *scratch, unsigned int cluster);
static int set_fat_entry (unsigned char *scratch, unsigned int cluster, unsigned int value);

static unsigned int get_free_fat (unsigned char *scratch);

static int canonical_to_dir (char *dest, const char *src) {

    static const char invalid_chars[] = "\"*+,./:;<=>?[\\]|";
    
    int i, j;
    int namelen = 0, dots = 0, extlen = 0;
    
    memset (dest, ' ', 11);
    
    if (*src == '\0' || *src == '.') {
        return -1;
    }
    
    for (j = i = 0; *src != '\0'; i++) {
    
        int c = (unsigned char) *src++;
        
        if (c == '/' || c == '\\') {
            break;
        }
        
        if (i >= 12) {
            return -1;
        }
        
        if (i == 0 && c == 0xE5) {
        
            /**
             * 0xE5 in the first character of the name is a marker for delected files,
             * it needs to be translated to 0x05.
             */
            c = 0x05;
        
        } else if (c == '.') {
        
            if (dots++) {
                return -1;
            }
            
            j = 8;
            continue;
        
        }
        
        if (c <= 0x20 || strchr (invalid_chars, c)) {
            return -1;
        }
        
        if (dots) {
        
            if (++extlen > 3) {
                return -1;
            }
        
        } else {
        
            if (++namelen > 8) {
                return -1;
            }
        
        }
        
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        
        dest[j++] = c;
    
    }
    
    return 0;

}

static int copy_file (const char *source, struct file_info *fi) {

    struct msdos_dirent de;
    struct fat32_fsinfo *info;
    
    unsigned int free_clusters;
    unsigned int next_cluster;
    
    FILE *ifp;
    long bytes;
    
    unsigned char *buffer, *temp;
    unsigned int i, sector;
    
    unsigned int clust_size = sectors_per_cluster * 512;
    unsigned int start_cluster = 0, prev_cluster = 0, size = 0;
    
    unsigned long flen;
    
    if ((ifp = fopen (source, "rb")) == NULL) {
        return -1;
    }
    
    fseek (ifp, 0, SEEK_END);
    
    if ((flen = ftell (ifp)) > UINT_MAX) {
    
        fclose (ifp);
        return -1;
    
    }
    
    fseek (ifp, 0, SEEK_SET);
    
    if (!(buffer = (unsigned char *) malloc (clust_size))) {
    
        fclose (ifp);
        return -1;
    
    }
    
    memset (buffer, 0, clust_size);
    
    while ((bytes = fread (buffer, 1, clust_size, ifp)) > 0) {
    
        size += bytes;
        
        for (i = 2; i < cluster_count; i++) {
        
            if (get_fat_entry (fi->scratch, i) == 0) {
                break;
            }
        
        }
        
        if (i == cluster_count) {
        
            fclose (ifp);
            free (buffer);
            
            return -1;
        
        }
        
        if (size_fat == 32) {
        
            if (seekto ((unsigned long) info_sector * 512) || fread (fi->scratch, 512, 1, ofp) != 1) {
            
                fclose (ifp);
                free (buffer);
                
                return -1;
            
            }
            
            info = (struct fat32_fsinfo *) (fi->scratch + 0x1e0);
            
            free_clusters = (unsigned int) info->free_clusters[0] | (((unsigned int) info->free_clusters[1]) << 8) | (((unsigned int) info->free_clusters[2]) << 16) | (((unsigned int) info->free_clusters[3]) << 24);
            next_cluster = (unsigned int) info->next_cluster[0] | (((unsigned int) info->next_cluster[1]) << 8) | (((unsigned int) info->next_cluster[2]) << 16) | (((unsigned int) info->next_cluster[3]) << 24);
            
            free_clusters--;
            next_cluster++;
            
            write741_to_byte_array (info->free_clusters, free_clusters);
            write741_to_byte_array (info->next_cluster, next_cluster);
            
            if (seekto ((unsigned long) info_sector * 512) || fwrite (fi->scratch, 512, 1, ofp) != 1) {
            
                fclose (ifp);
                free (buffer);
                
                return -1;
            
            }
        
        }
        
        if (start_cluster == 0) {
        
            start_cluster = i;
            
            if (seekto ((unsigned long) fi->dir_sector * 512) || fread (fi->scratch, 512, 1, ofp) != 1) {
            
                fclose (ifp);
                free (buffer);
                
                return -1;
            
            }
            
            de = (((struct msdos_dirent *) fi->scratch)[fi->dir_offset]);
            
            write721_to_byte_array (de.startlo, start_cluster);
            write721_to_byte_array (de.starthi, start_cluster >> 16);
            
            memcpy (&(((struct msdos_dirent *) fi->scratch)[fi->dir_offset]), &de, sizeof (de));
            
            if (seekto ((unsigned long) fi->dir_sector * 512) || fwrite (fi->scratch, 512, 1, ofp) != 1) {
            
                fclose (ifp);
                free (buffer);
                
                return -1;
            
            }
        
        } else {
        
            if (prev_cluster == 0) {
            
                fclose (ifp);
                free (buffer);
                
                return -1;
            
            }
            
            if (set_fat_entry (fi->scratch, prev_cluster, i) < 0) {
            
                fclose (ifp);
                free (buffer);
                
                return -1;
            
            }
        
        }
        
        sector = data_area + ((i - 2) * sectors_per_cluster);
        
        if (seekto ((unsigned long) sector * 512)) {
        
            fclose (ifp);
            free (buffer);
            
            return -1;
        
        }
        
        temp = buffer;
        
        while (bytes > 0) {
        
            if (fwrite (temp, 512, 1, ofp) != 1) {
            
                fclose (ifp);
                free (buffer);
                
                return -1;
            
            }
            
            bytes -= 512;
            temp += 512;
        
        }
        
        memset (buffer, 0, clust_size);
        
        if (set_fat_entry (fi->scratch, i, 0x0FFFFFF8) < 0) {
        
            fclose (ifp);
            free (buffer);
            
            return -1;
        
        }
        
        if (seekto ((unsigned long) fi->dir_sector * 512) || fread (fi->scratch, 512, 1, ofp) != 1) {
        
            fclose (ifp);
            free (buffer);
            
            return -1;
        
        }
        
        de = (((struct msdos_dirent *) fi->scratch)[fi->dir_offset]);
        
        write741_to_byte_array (de.size, size);
        memcpy (&(((struct msdos_dirent *) fi->scratch)[fi->dir_offset]), &de, sizeof (de));
        
        if (seekto ((unsigned long) fi->dir_sector * 512) || fwrite (fi->scratch, 512, 1, ofp) != 1) {
        
            fclose (ifp);
            free (buffer);
            
            return -1;
        
        }
        
        prev_cluster = i;
        if (feof (ifp)) { break; }
    
    }
    
    fclose (ifp);
    free (buffer);
    
    return 0;

}

static int get_free_dirent (const char *path, struct dir_info *di, struct msdos_dirent *de) {

    int entry;
    unsigned int i, tempclust;
    
    if (open_dir (path, di) < 0) {
        return -1;
    }
    
    entry = 0;
    di->flags |= 0x01;
    
    do {
    
        entry = get_next_entry (di, de);
        
        /*if (entry == 0 && (!de->name[0] || de->name[0] == 0xE5 || (de->attr & ATTR_LONG_NAME) == ATTR_LONG_NAME)) {*/
        if (entry == 0 && (!de->name[0] || de->name[0] == 0xE5)) {
        
            /*if (de->name[0] == 0xE5 || (de->attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) {*/
            if (de->name[0] == 0xE5) {
                di->current_entry--;
            }
            
            return 0;
        
        } else if (entry == -1) {
            return -1;
        } else if (entry == 1) {
        
            if ((tempclust = get_free_fat (di->scratch) == 0x0FFFFFF7)) {
                return -1;
            }
            
            memset (di->scratch, 0, 512);
            
            for (i = 0; i < sectors_per_cluster; i++) {
            
                unsigned long offset = (unsigned long) (data_area + ((tempclust - 2) * sectors_per_cluster) + i);
                
                if (seekto (offset * 512)) {
                    return -1;
                }
                
                if (fwrite (di->scratch, 512, 1, ofp) != 1) {
                    return -1;
                }
            
            }
            
            i = 0;
            
            if (set_fat_entry (di->scratch, di->current_cluster, tempclust) < 0) {
                return -1;
            }
            
            di->current_cluster = tempclust;
            di->current_entry = 0;
            di->current_sector = 0;
            
            if (size_fat == 12) {
                tempclust = 0x0FF8;
            } else if (size_fat == 16) {
                tempclust = 0xFFF8;
            } else if (size_fat == 32) {
            
                struct fat32_fsinfo *info;
                
                unsigned int free_clusters;
                unsigned int next_cluster;
                
                tempclust = 0x0ffffff8;
                
                if (seekto ((unsigned long) info_sector * 512) || fread (di->scratch, 512, 1, ofp) != 1) {
                    return -1;
                }
                
                info = (struct fat32_fsinfo *) (di->scratch + 0x1e0);
                
                free_clusters = (unsigned int) info->free_clusters[0] | (((unsigned int) info->free_clusters[1]) << 8) | (((unsigned int) info->free_clusters[2]) << 16) | (((unsigned int) info->free_clusters[3]) << 24);
                next_cluster = (unsigned int) info->next_cluster[0] | (((unsigned int) info->next_cluster[1]) << 8) | (((unsigned int) info->next_cluster[2]) << 16) | (((unsigned int) info->next_cluster[3]) << 24);
                
                free_clusters--;
                next_cluster++;
                
                write741_to_byte_array (info->free_clusters, free_clusters);
                write741_to_byte_array (info->next_cluster, next_cluster);
                
                if (seekto ((unsigned long) info_sector * 512) || fwrite (di->scratch, 512, 1, ofp) != 1) {
                    return -1;
                }
            
            } else {
                return -1;
            }
        
        }
    
    } while (!entry);
    
    /* We should't get here! */
    return -1;

}

static int get_next_entry (struct dir_info *di, struct msdos_dirent *de) {

    unsigned long offset;
    
    if (di->current_entry >= 512 / sizeof (*de)) {
    
        di->current_entry = 0;
        di->current_sector++;
        
        if (di->current_cluster == 0) {
        
            unsigned long offset;
            
            if (di->current_sector * (512 / sizeof (*de)) >= root_entries) {
                return -1;
            }
            
            offset = (unsigned long) root_dir + di->current_sector;
            
            if (seekto (offset * 512) || fread (di->scratch, 512, 1, ofp) != 1) {
                return -1;
            }
        
        } else {
        
            if (di->current_sector >= sectors_per_cluster) {
            
                di->current_sector = 0;
                
                if ((size_fat == 12 && di->current_cluster >= 0x0FF7) || (size_fat == 16 && di->current_cluster >= 0xFFF7) || (size_fat == 32 && di->current_cluster >= 0x0FFFFFF7)) {
                
                    if (!(di->flags & 0x01)) {
                        return -1;
                    }
                    
                    return 1;
                
                }
                
                di->current_cluster = get_fat_entry (di->scratch, di->current_cluster);
            
            }
            
            offset = (unsigned long) data_area;
            offset += ((di->current_cluster - 2) * sectors_per_cluster);
            offset += di->current_sector;
            
            if (seekto (offset * 512) || fread (di->scratch, 512, 1, ofp) != 1) {
                return -1;
            }
        
        }
    
    }
    
    memcpy (de, &(((struct msdos_dirent *) di->scratch)[di->current_entry]), sizeof (*de));
    
    if (de->name[0] == 0) {
    
        if (di->flags & 0x01) {
            return 0;
        }
        
        return -1;
    
    }
    
    if (de->name[0] == 0x05) {
    
        de->name[0] = 0xE5;
        return 0;
    
    }
    
    di->current_entry++;
    return 0;

}

static int open_dir (const char *target, struct dir_info *di) {

    di->flags = 0;
    
    if (!strlen ((char *) target) || (strlen ((char *) target) == 1 && (target[0] == '/' || target[0] == '\\'))) {
    
        unsigned long offset;
        
        if (size_fat == 32) {
        
            di->current_cluster = root_cluster;
            di->current_entry = 0;
            di->current_sector = 0;
            
            offset = (unsigned long) data_area + ((di->current_cluster - 2) * sectors_per_cluster);
        
        } else {
        
            di->current_cluster = 0;
            di->current_entry = 0;
            di->current_sector = 0;
            
            offset = (unsigned long) root_dir;
        
        }
        
        if (seekto (offset * 512) || fread (di->scratch, 512, 1, ofp) != 1) {
            return -1;
        }
    
    } else {
    
        unsigned char tmpfn[12];
        unsigned char *ptr;
        
        struct msdos_dirent de;
        unsigned int result;
        
        unsigned long offset;
        
        if (size_fat == 32) {
        
            di->current_cluster = root_cluster;
            di->current_entry = 0;
            di->current_sector = 0;
            
            offset = (unsigned long) data_area + ((di->current_cluster - 2) * sectors_per_cluster);
        
        } else {
        
            di->current_cluster = 0;
            di->current_entry = 0;
            di->current_sector = 0;
            
            offset = (unsigned long) root_dir;
        
        }
        
        if (seekto (offset * 512) || fread (di->scratch, 512, 1, ofp) != 1) {
            return -1;
        }
        
        ptr = (unsigned char *) target;
        
        while ((*ptr == '/' || *ptr == '\\') && *ptr) {
            ptr++;
        }
        
        while (*ptr) {
        
            if (canonical_to_dir ((char *) tmpfn, (char *) ptr) < 0) {
            
                fprintf (stderr, "Failed to convert to 8:3\n");
                return -1;
            
            }
            
            de.name[0] = 0;
            
            do {
                result = get_next_entry (di, &de);
            } while (!result && memcmp (de.name, tmpfn, 11));
            
            if (!memcmp (de.name, tmpfn, 11) && (de.attr & ATTR_DIR) == ATTR_DIR) {
            
                unsigned long offset;
                
                di->current_cluster = (unsigned int) de.startlo[0] | ((unsigned int) de.startlo[1]) << 8 | ((unsigned int) de.starthi[0]) << 16 | ((unsigned int) de.starthi[1]) << 24;
                di->current_entry = 0;
                di->current_sector = 0;
                
                offset = (unsigned long) (data_area + ((di->current_cluster - 2) * sectors_per_cluster));
                
                if (seekto (offset * 512)) {
                    return -1;
                }
                
                if (fread (di->scratch, 512, 1, ofp) != 1) {
                    return -1;
                }
            
            } else if (!memcmp (de.name, tmpfn, 11) && !(de.attr & ATTR_DIR)) {
                return -1;
            }
            
            while (*ptr != '/' && *ptr != '\\' && *ptr) {
                ptr++;
            }
            
            if (*ptr == '/' || *ptr == '\\') {
                ptr++;
            }
        
        }
        
        if (!di->current_cluster) {
            return -1;
        }
    
    }
    
    return 0;

}

static int open_file (const char *target, unsigned char *scratch, struct file_info *fi) {

    unsigned char tmppath[PATH_MAX];
    unsigned char filename[12];
    unsigned char *p;
    
    struct dir_info di;
    struct msdos_dirent de;
    
    unsigned short date;
    unsigned short time;
    
    unsigned int tempclust;
    
    /* Zero out file structure. */
    memset (fi, 0, sizeof (*fi));
    
    /* Get a local copy of the target.  If it's larger than PATH_MAX, abort. */
    strncpy ((char *) tmppath, (char *) target, PATH_MAX);
    tmppath[PATH_MAX - 1] = 0;
    
    if (strcmp ((char *) target, (char *) tmppath)) {
        return -1;
    }
    
    /* Strip leading seperators. */
    while (tmppath[0] == '/' || tmppath[0] == '\\') {
        strcpy ((char *) tmppath, (char *) tmppath + 1);
    }
    
    /* Parse filename off the end of the suppiled target. */
    p = tmppath;
    
    while (*(p++));
    
    p--;
    
    while (p > tmppath && *p != '/' && *p != '\\') {
        p--;
    }
    
    if (*p == '/' || *p == '\\') {
        p++;
    }
    
    if (canonical_to_dir ((char *) filename, (char *) p) < 0) {
    
        fprintf (stderr, "Failed to convert to 8:3\n");
        return -1;
    
    }
    
    if (p > tmppath) {
        p--;
    }
    
    if (*p == '/' || *p == '\\' || p == tmppath) {
        *p = 0;
    }
    
    di.scratch = scratch;
    
    if (open_dir ((char *) tmppath, &di) < 0) {
    
        fprintf (stderr, "Failed to open directory\n");
        return -1;
    
    }
    
    while (!get_next_entry (&di, &de)) {
    
        if (!memcmp (de.name, filename, 11)) {
        
            di.current_entry--;
            
            if (de.attr & ATTR_DIR) {
                return -1;
            }
            
            if (!check_overwrite ((char *) target)) {
                return 0;
            }
            
            fi->cluster = (unsigned int) de.startlo[0] | ((unsigned int) de.startlo[1]) << 8 | ((unsigned int) de.starthi[0]) << 16 | ((unsigned int) de.starthi[1]) << 24;
            
            for (;;) {
            
                if (size_fat == 12 && fi->cluster == 0x0FF8) {
                    break;
                } else if (size_fat == 16 && fi->cluster == 0xFFF8) {
                    break;
                } else if (size_fat == 32 && fi->cluster == 0x0FFFFFF8) {
                    break;
                }
                
                tempclust = get_fat_entry (scratch, fi->cluster);
                
                if (set_fat_entry (scratch, fi->cluster, 0) < 0) {
                    return -1;
                }
                
                fi->cluster = tempclust;
                
                if (!fi->cluster || fi->cluster == 0x0FFFFFFF7) {
                    break;
                }
            
            }
            
            fi->cluster = 0;
            
            if (seekto ((unsigned long) fi->dir_sector * 512) || fread (scratch, 512, 1, ofp) != 1) {
                return -1;
            }
            
            date = generate_datestamp ();
            time = generate_timestamp ();
            
            memset (&de, 0, sizeof (de));
            memcpy (de.name, filename, 11);
            
            de.attr = ATTR_ARCHIVE;
            
            write721_to_byte_array (de.ctime, time);
            write721_to_byte_array (de.cdate, date);
            write721_to_byte_array (de.adate, date);
            write721_to_byte_array (de.time, time);
            write721_to_byte_array (de.date, date);
            
            fi->pointer = 0;
            
            if (di.current_cluster == 0) {
                fi->dir_sector = root_dir + di.current_sector;
            } else {
                fi->dir_sector = data_area + ((di.current_cluster - 2) * sectors_per_cluster);
            }
            
            /*fi->dir_offset = di.current_entry - 1;*/
            fi->dir_offset = di.current_entry;
            
            if (seekto ((unsigned long) fi->dir_sector * 512) || fread (scratch, 512, 1, ofp) != 1) {
                return -1;
            }
            
            memcpy (&(((struct msdos_dirent *) scratch)[fi->dir_offset]), &de, sizeof (de));
            
            if (seekto ((unsigned long) fi->dir_sector * 512) || fwrite (scratch, 512, 1, ofp) != 1) {
                return -1;
            }
            
            return 0;
        
        }
    
    }
    
    if (get_free_dirent ((char *) tmppath, &di, &de) < 0) {
        return -1;
    }
    
    date = generate_datestamp ();
    time = generate_timestamp ();
    
    memset (&de, 0, sizeof (de));
    memcpy (de.name, filename, 11);
    
    de.attr = ATTR_ARCHIVE;
    
    write721_to_byte_array (de.ctime, time);
    write721_to_byte_array (de.cdate, date);
    write721_to_byte_array (de.adate, date);
    write721_to_byte_array (de.time, time);
    write721_to_byte_array (de.date, date);
    
    fi->pointer = 0;
    
    if (di.current_cluster == 0) {
        fi->dir_sector = root_dir + di.current_sector;
    } else {
        fi->dir_sector = data_area + ((di.current_cluster - 2) * sectors_per_cluster)  + di.current_sector;
    }
    
    /*fi->dir_offset = di.current_entry - 1;*/
    fi->dir_offset = di.current_entry;
    
    if (seekto ((unsigned long) fi->dir_sector * 512) || fread (scratch, 512, 1, ofp) != 1) {
        return -1;
    }
    
    memcpy (&(((struct msdos_dirent *) scratch)[fi->dir_offset]), &de, sizeof (de));
    
    if (seekto ((unsigned long) fi->dir_sector * 512) || fwrite (scratch, 512, 1, ofp) != 1) {
        return -1;
    }
    
    return 0;

}

static int set_fat_entry (unsigned char *scratch, unsigned int cluster, unsigned int value) {

    unsigned int i, offset, sector;
    
    if (size_fat == 12) {
    
        offset = cluster + (cluster / 2);
        value &= 0x0fff;
    
    } else if (size_fat == 16) {
    
        offset = cluster * 2;
        value &= 0xffff;
    
    } else if (size_fat == 32) {
    
        offset = cluster * 4;
        value &= 0x0fffffff;
    
    } else {
        return -1;
    }
    
    /**
     * At this point, offset is the BYTE offset of the desired sector from the start
     * of the FAT.  Calculate the physical sector containing this FAT entry.
     */
    sector = (offset / 512) + reserved_sectors;
    
    if (seekto (sector * 512) || fread (scratch, 512, 1, ofp) != 1) {
        return -1;
    }
    
    /**
     * At this point, we "merely" need to extract the relevant entry.  This is
     * easy for FAT16 and FAT32, but a royal PITA for FAT12 as a single entry
     * may span a sector boundary.  The normal way around this is always to
     * read two FAT sectors, but luxary is (by design intent) unavailable.
     */
    offset %= 512;
    
    if (size_fat == 12) {
    
        if (offset == 511) {
        
            if (((cluster * 3) & 0x01) == 0) {
                scratch[offset] = (unsigned char) (value & 0xFF);
            } else {
                scratch[offset] = (unsigned char) ((scratch[offset] & 0x0F) | (value & 0xF0));
            }
            
            for (i = 0; i < number_of_fats; i++) {
            
                unsigned long temp = sector + (i * sectors_per_fat);
                
                if (seekto (temp * 512) < 0 || fwrite (scratch, 512, 1, ofp) != 1) {
                    return -1;
                }
            
            }
            
            sector++;
            
            if (seekto (sector) || fread (scratch, 512, 1, ofp) != 1) {
                return -1;
            }
            
            if (((cluster * 3) & 0x01) == 0) {
                scratch[0] = (unsigned char) ((scratch[0] & 0xF0) | (value & 0x0F));
            } else {
                scratch[0] = (unsigned char) (value & 0xFF00);
            }
            
            goto _write_fat;
        
        } else {
        
            if (((cluster * 3) & 0x01) == 0) {
            
                scratch[offset] = (unsigned char) (value & 0x00FF);
                scratch[offset + 1] = (unsigned char) ((scratch[offset + 1] & 0x00F0) | ((value & 0x0F00) >> 8));
            
            } else {
            
                scratch[offset] = (unsigned char) ((scratch[offset] & 0x000F) | ((value & 0x000F) << 4));
                scratch[offset + 1] = (unsigned char) ((value & 0x0FF0) >> 4);
            
            }
            
            goto _write_fat;
        
        }
    
    } else if (size_fat == 16) {
    
        scratch[offset] = (value & 0xFF);
        scratch[offset + 1] = (value >> 8) & 0xFF;
        
        goto _write_fat;
    
    } else if (size_fat == 32) {
    
        scratch[offset] = (value & 0xFF);
        scratch[offset + 1] = (value >> 8) & 0xFF;
        scratch[offset + 2] = (value >> 16) & 0xFF;
        scratch[offset + 3] = (scratch[offset + 3] & 0xF0) | ((value >> 24) & 0xFF);
        
        goto _write_fat;
    
    }
    
    return -1;

_write_fat:

    for (i = 0; i < number_of_fats; i++) {
    
        unsigned long temp = sector + (i * sectors_per_fat);
        
        if (seekto (temp * 512) || fwrite (scratch, 512, 1, ofp) != 1) {
            return -1;
        }
    
    }
    
    return 0;

}

static unsigned int get_fat_entry (unsigned char *scratch, unsigned int cluster) {

    unsigned int offset, sector, result;
    
    if (size_fat == 12) {
        offset = cluster + (cluster / 2);
    } else if (size_fat == 16) {
        offset = cluster * 2;
    } else if (size_fat == 32) {
        offset = cluster * 4;
    } else {
        return 0x0FFFFFF7;
    }
    
    sector = (offset / 512) + reserved_sectors;
    
    if (seekto ((unsigned long) sector * 512) || fread (scratch, 512, 1, ofp) != 1) {
        return 0x0FFFFFF7;
    }
    
    offset %= 512;
    
    if (size_fat == 12) {
    
        if (offset == 511) {
        
            result = (unsigned int) scratch[offset];
            sector++;
            
            if (seekto ((unsigned long) sector * 512) || fread (scratch, 512, 1, ofp) != 1) {
                return 0x0FFFFFF7;
            }
            
            result |= ((unsigned int) scratch[0]) << 8;
        
        } else {
            result = (unsigned int) scratch[offset] | ((unsigned int) scratch[offset + 1]) << 8;
        }
        
        if (cluster & 1) {
            result = result >> 4;
        } else {
            result = result & 0x0FFF;
        }
    
    } else if (size_fat == 16) {
        result = (unsigned int) scratch[offset] | ((unsigned int) scratch[offset + 1]) << 8;
    } else if (size_fat == 32) {
        result = ((unsigned int) scratch[offset] | ((unsigned int) scratch[offset + 1]) << 8 | ((unsigned int) scratch[offset + 2]) << 16 | ((unsigned int) scratch[offset + 3]) << 24) & 0x0FFFFFFF;
    } else {
        result = 0x0FFFFFF7;
    }
    
    return result;

}

static unsigned int get_free_fat (unsigned char *scratch) {

    unsigned int i, result = 0xFFFFFFFF;
    
    for (i = 2; i < cluster_count; i++) {
    
        result = get_fat_entry (scratch, i);
        
        if (!result) {
            return i;
        }
    
    }
    
    return 0x0FFFFFF7;

}


int get_file (const char *source, unsigned char *scratch, struct file_info *fi) {

    unsigned char tmppath[PATH_MAX];
    unsigned char filename[12];
    unsigned char *p;
    
    struct dir_info di;
    struct msdos_dirent de;
    
    /* Zero out file structure. */
    memset (fi, 0, sizeof (*fi));
    
    /* Get a local copy of the target.  If it's larger than PATH_MAX, abort. */
    strncpy ((char *) tmppath, (char *) source, PATH_MAX);
    tmppath[PATH_MAX - 1] = 0;
    
    if (strcmp ((char *) source, (char *) tmppath)) {
        return -1;
    }
    
    /* Strip leading seperators. */
    while (tmppath[0] == '/' || tmppath[0] == '\\') {
        strcpy ((char *) tmppath, (char *) tmppath + 1);
    }
    
    /* Parse filename off the end of the suppiled target. */
    p = tmppath;
    
    while (*(p++));
    
    p--;
    
    while (p > tmppath && *p != '/' && *p != '\\') {
        p--;
    }
    
    if (*p == '/' || *p == '\\') {
        p++;
    }
    
    if (canonical_to_dir ((char *) filename, (char *) p) < 0) {
    
        fprintf (stderr, "Failed to convert to 8:3\n");
        return -1;
    
    }
    
    if (p > tmppath) {
        p--;
    }
    
    if (*p == '/' || *p == '\\' || p == tmppath) {
        *p = 0;
    }
    
    di.scratch = scratch;
    
    if (open_dir ((char *) tmppath, &di) < 0) {
    
        fprintf (stderr, "Failed to open directory\n");
        return -1;
    
    }
    
    while (!get_next_entry (&di, &de)) {
    
        if (!memcmp (de.name, filename, 11)) {
        
            di.current_entry--;
            
            if (de.attr & ATTR_DIR) {
                return -1;
            }
            
            fi->cluster = (unsigned int) de.startlo[0] | ((unsigned int) de.startlo[1]) << 8 | ((unsigned int) de.starthi[0]) << 16 | ((unsigned int) de.starthi[1]) << 24;
            fi->bytes = (unsigned int) de.size[0] | ((unsigned int) de.size[1]) << 8 | ((unsigned int) de.size[2]) << 16 | ((unsigned int) de.size[3]) << 24;
            
            fi->scratch = scratch;
            return 0;
        
        }
    
    }
    
    return -1;

}

int copy_from_image (const char *target, struct file_info *fi) {

    unsigned long sector, i;
    FILE *tfp;
    
    if ((tfp = fopen (target, "rb")) == NULL) {
    
        if ((tfp = fopen (target, "wb")) == NULL) {
            return -1;
        }
    
    } else {
    
        if (!check_overwrite ((char *) target)) {
            return 0;
        }
        
        fclose (tfp);
        
        if ((fopen (target, "wb")) == NULL) {
            return -1;
        }
    
    }
    
    for (;;) {
    
        if (size_fat == 12 && fi->cluster == 0x0FF8) {
            break;
        } else if (size_fat == 16 && fi->cluster == 0xFFF8) {
            break;
        } else if (size_fat == 32 && fi->cluster == 0x0FFFFFF8) {
            break;
        }
        
        sector = data_area + ((fi->cluster - 2) * sectors_per_cluster);
        
        for (i = 0; i < sectors_per_cluster; ++i) {
        
            if (seekto (sector * 512) || fread (fi->scratch, 512, 1, ofp) != 1) {
            
                fclose (tfp);
                remove (target);
                
                return -1;
            
            }
            
            if (fi->bytes > 512) {
            
                if (fwrite (fi->scratch, 512, 1, tfp) != 1) {
                
                    fclose (tfp);
                    remove (target);
                    
                    return -1;
                
                }
                
                fi->bytes -= 512;
            
            } else {
            
                if (fwrite (fi->scratch, fi->bytes, 1, tfp) != 1) {
                
                    fclose (tfp);
                    remove (target);
                    
                    return -1;
                
                }
                
                fclose (tfp);
                return 0;
            
            }
            
            sector++;
        
        }
        
        fi->cluster = get_fat_entry (fi->scratch, fi->cluster);
        
        if (!fi->cluster || fi->cluster == 0x0FFFFFFF7) {
            break;
        }
    
    }
    
    fclose (tfp);
    return 0;

}


int main (int argc, char **argv) {

    unsigned char *scratch;
    char *target, tmppath[PATH_MAX], *source;
    
    size_t i;
    int copy_from = 1;
    
    if (argc && *argv) {
    
        char *p;
        program_name = *argv;
        
        if ((p = strrchr (program_name, '/'))) {
            program_name = (p + 1);
        }
    
    }
    
    state = xmalloc (sizeof (*state));
    parse_args (&argc, &argv, 1);
    
    if (!state->outfile || state->nb_files < 2) {
        print_help (EXIT_FAILURE);
    }
    
    target = state->files[state->nb_files - 1];
    state->nb_files--;
    
    if (*target == ':') {
    
        ++target;
        
        if (*target == ':') {
        
            copy_from = 0;
            ++target;
        
        }
    
    }
    
    if (!*target) {
        target = "/";
    }
    
    if ((ofp = fopen (state->outfile, "r+b")) == NULL) {
    
        report_at (program_name, 0, REPORT_ERROR, "faild to open '%s' for writing", state->outfile);
        return EXIT_FAILURE;
    
    }
    
    if (seekto ((unsigned long) 0) || fread (&bs, sizeof (bs), 1, ofp) != 1) {
    
        report_at (program_name, 0, REPORT_ERROR, "failed whilst reading boot sector");
        fclose (ofp);
        
        return EXIT_FAILURE;
    
    }
    
    if (bs.boot_jump[0] != 0xEB || bs.boot_jump[1] < 0x16 || bs.boot_jump[2] != 0x90) {
    
        report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
        fclose (ofp);
        
        return EXIT_FAILURE;
    
    }
    
    sectors_per_cluster = (unsigned int) bs.sectors_per_cluster;
    reserved_sectors = (unsigned int) bs.reserved_sectors[0] | (((unsigned int) bs.reserved_sectors[1]) << 8);
    number_of_fats = (unsigned int) bs.no_fats;
    root_entries = (unsigned int) bs.root_entries[0] | (((unsigned int) bs.root_entries[1]) << 8);
    total_sectors = (unsigned int) bs.total_sectors16[0] | (((unsigned int) bs.total_sectors16[1]) << 8);
    sectors_per_fat = (unsigned int) bs.sectors_per_fat16[0] | (((unsigned int) bs.sectors_per_fat16[1]) << 8);
    
    if (!sectors_per_cluster || !reserved_sectors || !number_of_fats) {
    
        report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
        fclose (ofp);
        
        return EXIT_FAILURE;
    
    }
    
    if (!root_entries) {
    
        if (bs.boot_jump[1] < 0x58) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ofp);
            
            return EXIT_FAILURE;
        
        }
        
        root_cluster = (unsigned int) bs.fat32.root_cluster[0] | (((unsigned int) bs.fat32.root_cluster[1]) << 8) | (((unsigned int) bs.fat32.root_cluster[2]) << 16) | (((unsigned int) bs.fat32.root_cluster[3]) << 24);
        
        if (!root_cluster) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ofp);
            
            return EXIT_FAILURE;
        
        }
    
    }
    
    if (!total_sectors) {
    
        if (bs.boot_jump[1] < 0x22) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ofp);
            
            return EXIT_FAILURE;
        
        }
        
        total_sectors = (unsigned int) bs.total_sectors32[0] | (((unsigned int) bs.total_sectors32[1]) << 8) | (((unsigned int) bs.total_sectors32[2]) << 16) | (((unsigned int) bs.total_sectors32[3]) << 24);
        
        if (!total_sectors) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ofp);
            
            return EXIT_FAILURE;
        
        }
    
    }
    
    if (!sectors_per_fat) {
    
        if (bs.boot_jump[1] < 0x58) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ofp);
            
            return EXIT_FAILURE;
        
        }
        
        sectors_per_fat = (unsigned int) bs.fat32.sectors_per_fat32[0] | (((unsigned int) bs.fat32.sectors_per_fat32[1]) << 8) | (((unsigned int) bs.fat32.sectors_per_fat32[2]) << 16) | (((unsigned int) bs.fat32.sectors_per_fat32[3]) << 24);
        
        if (!sectors_per_fat) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ofp);
            
            return EXIT_FAILURE;
        
        }
    
    }
    
    if (root_entries) {
    
        root_dir = reserved_sectors + (sectors_per_fat * 2);
        data_area = root_dir + (((root_entries * 32) + (512 - 1)) / 512);
    
    } else {
    
        data_area = reserved_sectors + (sectors_per_fat * 2);
        
        /*root_dir = data_area + ((root_cluster - 2) * sectors_per_cluster);*/
        /*root_dir = root_cluster;*/
    
    }
    
    cluster_count = (total_sectors - data_area) / sectors_per_cluster;
    
    if (bs.boot_jump[1] == 0x58) {
    
        info_sector = (unsigned int) bs.fat32.info_sector[0] | (((unsigned int) bs.fat32.info_sector[1]) << 8);
        
        if (!info_sector) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ofp);
            
            return EXIT_FAILURE;
        
        }
        
        size_fat = 32;
    
    } else if (cluster_count <= MAX_CLUST_12) {
        size_fat = 12;
    } else if (cluster_count >= MIN_CLUST_16 && cluster_count <= MAX_CLUST_16) {
        size_fat = 16;
    } else {
    
        report_at (program_name, 0, REPORT_ERROR, "FAT is not 12, 16 or 32 bits");
        fclose (ofp);
        
        return EXIT_FAILURE;
    
    }
    
    if (target && target[strlen (target) - 1] != '/' && target[strlen (target) - 1] != '\\') {
    
        if (state->nb_files > 1) {
        
            report_at (program_name, 0, REPORT_ERROR, "cannot copy multiple files to %s", target);
            fclose (ofp);
            
            return EXIT_FAILURE;
        
        }
    
    }
    
    if (!(scratch = (unsigned char *) malloc (512))) {
    
        report_at (program_name, 0, REPORT_ERROR, "Out of memory");
        fclose (ofp);
        
        return EXIT_FAILURE;
    
    }
    
    for (i = 0; i < state->nb_files; ++i) {
    
        char *p, *ptr;
        int need_fn = 0;
        
        size_t pathlen;
        struct file_info fi;
        
        source = state->files[i];
        
        if (copy_from) {
        
            if (memcmp (source, "::", 2)) {
        
                fclose (ofp);
                remove (state->outfile);
                
                print_help (EXIT_FAILURE);
            
            }
            
            source += 2;
        
        }
        
        if (*target == '/' || *target == '\\') {
            target++;
        }
        
        pathlen = strlen (target);
        
        if (target[pathlen - 1] == '/' || target[pathlen - 1] == '\\') {
        
            need_fn = 1;
            ptr = source;
            
            if ((p = strrchr (ptr, '/'))) {
                ptr = (p + 1);
            }
            
            pathlen += strlen (ptr);
        
        }
        
        if (pathlen > PATH_MAX) {
        
            report_at (program_name, 0, REPORT_ERROR, "target too long");
            free (scratch);
            
            fclose (ofp);
            return EXIT_FAILURE;
        
        }
        
        memset (tmppath, 0, PATH_MAX);
        strncpy (tmppath, target, strlen (target));
        
        if (need_fn) {
            strncat (tmppath, ptr, strlen (ptr));
        }
        
        if (copy_from) {
        
            if (get_file (source, scratch, &fi) < 0) {
            
                report_at (program_name, 0, REPORT_ERROR, "file '%s' does not exist", source);
                free (scratch);
                
                fclose (ofp);
                return EXIT_FAILURE;
            
            }
            
            if (copy_from_image (target, &fi) < 0) {
                
                report_at (program_name, 0, REPORT_ERROR, "failed to copy %s", source);
                free (scratch);
                
                fclose (ofp);
                return EXIT_FAILURE;
            
            }
            
            continue;
        
        }
        
        if (open_file (tmppath, scratch, &fi) < 0) {
        
            report_at (program_name, 0, REPORT_ERROR, "failed to create %s", target);
            free (scratch);
            
            fclose (ofp);
            return EXIT_FAILURE;
        
        }
        
        fi.scratch = scratch;
        
        if (copy_file (source, &fi) < 0) {
        
            report_at (program_name, 0, REPORT_ERROR, "failed to copy %s", source);
            free (scratch);
            
            fclose (ofp);
            return EXIT_FAILURE;
        
        }
    
    }
    
    free (scratch);
    fclose (ofp);
    
    return EXIT_SUCCESS;

}
