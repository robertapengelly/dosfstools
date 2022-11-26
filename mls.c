/******************************************************************************
 * @file            mls.c
 *****************************************************************************/
#include    <ctype.h>
#include    <errno.h>
#include    <limits.h>
#include    <stdio.h>
#include    <stdlib.h>
#include    <string.h>

#include    "common.h"
#include    "msdos.h"
#include    "report.h"

#ifndef     PATH_MAX
# define    PATH_MAX                    2048
#endif

struct mls_state {

    char **dirs;
    size_t nb_dirs;
    
    const char *outfile;
    unsigned long offset;

};

static FILE *ifp;

struct dir_info {

    unsigned int current_cluster;
    
    unsigned char current_sector;
    unsigned char current_entry;
    unsigned char *scratch;
    unsigned char flags;

};

static struct mls_state *state = 0;
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
    
    fprintf (stderr, "Usage: %s [options] dirname\n\n", program_name);
    fprintf (stderr, "Options:\n\n");
    
    fprintf (stderr, "    Short options:\n\n");
    fprintf (stderr, "        -i                Specify the input target.\n");
    
    fprintf (stderr, "\n");
    
    fprintf (stderr, "    Long options:\n\n");
    fprintf (stderr, "        --help            Show this help information then exit.\n");
    fprintf (stderr, "        --offset SECTOR   Read the filesystem starting at SECTOR.\n");
       
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
        
            dynarray_add (&state->dirs, &state->nb_dirs, xstrdup (r));
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


static int seekto (long offset) {
    return fseek (ifp, (state->offset * 512) + offset, SEEK_SET);
}

static int get_next_entry (struct dir_info *di, struct msdos_dirent *de);
static int open_dir (const char *target, struct dir_info *di);

static unsigned int get_fat_entry (unsigned char *scratch, unsigned int cluster);

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
            
            if (seekto (offset * 512) || fread (di->scratch, 512, 1, ifp) != 1) {
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
            
            if (seekto (offset * 512) || fread (di->scratch, 512, 1, ifp) != 1) {
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
        
        if (seekto (offset * 512) || fread (di->scratch, 512, 1, ifp) != 1) {
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
        
        if (seekto (offset * 512) || fread (di->scratch, 512, 1, ifp) != 1) {
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
                
                if (fread (di->scratch, 512, 1, ifp) != 1) {
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
    
    if (seekto ((unsigned long) sector * 512) || fread (scratch, 512, 1, ifp) != 1) {
        return 0x0FFFFFF7;
    }
    
    offset %= 512;
    
    if (size_fat == 12) {
    
        if (offset == 511) {
        
            result = (unsigned int) scratch[offset];
            sector++;
            
            if (seekto ((unsigned long) sector * 512) || fread (scratch, 512, 1, ifp) != 1) {
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

int main (int argc, char **argv) {

    unsigned char *scratch;
    char *target, filename[13];
    
    struct dir_info di;
    struct msdos_dirent de;
    
    unsigned int bytes, timestamp;
    size_t i, j, k;
    
    if (argc && *argv) {
    
        char *p;
        program_name = *argv;
        
        if ((p = strrchr (program_name, '/'))) {
            program_name = (p + 1);
        }
    
    }
    
    state = xmalloc (sizeof (*state));
    parse_args (&argc, &argv, 1);
    
    if (!state->outfile) {
        print_help (EXIT_FAILURE);
    }
    
    if (state->nb_dirs == 0) {
        dynarray_add (&state->dirs, &state->nb_dirs, "/");
    }
    
    if ((ifp = fopen (state->outfile, "rb")) == NULL) {
    
        report_at (program_name, 0, REPORT_ERROR, "faild to open '%s' for reading", state->outfile);
        return EXIT_FAILURE;
    
    }
    
    if (seekto ((unsigned long) 0) || fread (&bs, sizeof (bs), 1, ifp) != 1) {
    
        report_at (program_name, 0, REPORT_ERROR, "failed whilst reading boot sector");
        fclose (ifp);
        
        return EXIT_FAILURE;
    
    }
    
    if (bs.boot_jump[0] != 0xEB || bs.boot_jump[1] < 0x16 || bs.boot_jump[2] != 0x90) {
    
        report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
        fclose (ifp);
        
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
        fclose (ifp);
        
        return EXIT_FAILURE;
    
    }
    
    if (!root_entries) {
    
        if (bs.boot_jump[1] < 0x58) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ifp);
            
            return EXIT_FAILURE;
        
        }
        
        root_cluster = (unsigned int) bs.fat32.root_cluster[0] | (((unsigned int) bs.fat32.root_cluster[1]) << 8) | (((unsigned int) bs.fat32.root_cluster[2]) << 16) | (((unsigned int) bs.fat32.root_cluster[3]) << 24);
        
        if (!root_cluster) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ifp);
            
            return EXIT_FAILURE;
        
        }
    
    }
    
    if (!total_sectors) {
    
        if (bs.boot_jump[1] < 0x22) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ifp);
            
            return EXIT_FAILURE;
        
        }
        
        total_sectors = (unsigned int) bs.total_sectors32[0] | (((unsigned int) bs.total_sectors32[1]) << 8) | (((unsigned int) bs.total_sectors32[2]) << 16) | (((unsigned int) bs.total_sectors32[3]) << 24);
        
        if (!total_sectors) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ifp);
            
            return EXIT_FAILURE;
        
        }
    
    }
    
    if (!sectors_per_fat) {
    
        if (bs.boot_jump[1] < 0x58) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ifp);
            
            return EXIT_FAILURE;
        
        }
        
        sectors_per_fat = (unsigned int) bs.fat32.sectors_per_fat32[0] | (((unsigned int) bs.fat32.sectors_per_fat32[1]) << 8) | (((unsigned int) bs.fat32.sectors_per_fat32[2]) << 16) | (((unsigned int) bs.fat32.sectors_per_fat32[3]) << 24);
        
        if (!sectors_per_fat) {
        
            report_at (program_name, 0, REPORT_ERROR, "%s does not have a valid FAT boot sector", state->outfile);
            fclose (ifp);
            
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
            fclose (ifp);
            
            return EXIT_FAILURE;
        
        }
        
        size_fat = 32;
    
    } else if (cluster_count <= MAX_CLUST_12) {
        size_fat = 12;
    } else if (cluster_count >= MIN_CLUST_16 && cluster_count <= MAX_CLUST_16) {
        size_fat = 16;
    } else {
    
        report_at (program_name, 0, REPORT_ERROR, "FAT is not 12, 16 or 32 bits");
        fclose (ifp);
        
        return EXIT_FAILURE;
    
    }
    
    if (!(scratch = (unsigned char *) malloc (512))) {
    
        report_at (program_name, 0, REPORT_ERROR, "Out of memory");
        fclose (ifp);
        
        return EXIT_FAILURE;
    
    }
    
    di.scratch = scratch;
    
    for (i = 0; i < state->nb_dirs; i++) {
    
        target = state->dirs[i];
        
        if (*target == '/' || *target == '\\') {
            target++;
        }
        
        if (open_dir ((char *) target, &di) < 0) {
        
            fprintf (stderr, "failed to open directory\n");
            return -1;
        
        }
        
        if (state->nb_dirs > 1) {
        
            if (!*target) {
                printf ("/\n\n");
            } else {
                printf ("%s:\n\n", target);
            }
        
        }
        
        while (!get_next_entry (&di, &de)) {
        
            /*if (!de.name[0] || de.name[0] == 0xE5 || (de.attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) {*/
            if (!de.name[0] || de.name[0] == 0xE5) {
                continue;
            }
            
            if ((de.attr & ATTR_VOLUME_ID) == ATTR_VOLUME_ID) {
                continue;
            }
            
            memset (filename, 0, 13);
            
            for (j = 0, k = 0; j < 11; ++j) {
            
                if (j < 8 && de.name[j] == ' ') {
                
                    if ((de.attr & ATTR_DIR) != ATTR_DIR && k > 0 && filename[k - 1] != '.') {
                        filename[k++] = '.';
                    }
                
                } else {
                
                    if (j == 8 && (de.attr & ATTR_DIR) != ATTR_DIR && filename[k - 1] != '.') {
                        filename[k++] = '.';
                    }
                    
                    filename[k++] = de.name[j];
                
                }
            
            }
            
            if (state->nb_dirs > 1) {
                printf ("    ");
            }
            
            printf ("%s", filename);
            
            while (k < 12) {
            
                printf (" ");
                k++;
            
            }
            
            if ((de.attr & ATTR_DIR) == ATTR_DIR) {
                printf ("    <DIR>    "); 
            } else {
                printf ("             ");
            }
            
            if ((de.attr & ATTR_DIR) == ATTR_DIR) {
                printf ("          ");
            } else {
            
                bytes = (unsigned int) de.size[0] | (((unsigned int) de.size[1]) << 8) | (((unsigned int) de.size[2]) << 16) | (((unsigned int) de.size[3]) << 24);
                printf ("%10u", bytes);
            
            }
            
            timestamp = (unsigned short) de.date[0] | (((unsigned short) de.date[1]) << 8);
            printf ("    %04d-%02d-%02d", ((timestamp >> 9) & 0x3f) + 1980, (timestamp >> 5) & 0x0f, timestamp & 0x1f);
            
            timestamp = (unsigned short) de.time[0] | (((unsigned short) de.time[1]) << 8);
            printf ("    %02d:%02d:%02d", (timestamp >> 11) & 0x3f, (timestamp >> 5) & 0x3f, (timestamp & 0x1f) << 1);
            
            printf ("\n");
        
        }
        
        if (state->nb_dirs > 1 && i < state->nb_dirs - 1) {
            printf ("\n");
        }
    
    }
    
    free (scratch);
    fclose (ifp);
    
    return EXIT_SUCCESS;

}
