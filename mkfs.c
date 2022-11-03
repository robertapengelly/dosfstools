/******************************************************************************
 * @file            mkfs.c
 *****************************************************************************/
#include    <limits.h>
#include    <stddef.h>
#include    <stdio.h>
#include    <stdlib.h>
#include    <string.h>

#include    "common.h"
#include    "lib.h"
#include    "mkfs.h"
#include    "msdos.h"
#include    "report.h"
#include    "write7x.h"

#ifndef     __PDOS__
# if     defined (__GNUC__)
#  include  <sys/time.h>
#  include  <unistd.h>
# else
#  include  <io.h>
# endif
#endif

static int align_structures = 1;
static int orphaned_sectors = 0;

static FILE *ofp;
static size_t image_size = 0;

static long total_sectors = 0;

static int heads_per_cylinder = 255;
static int sectors_per_track = 63;

static unsigned int backup_boot = 0;
static unsigned int cluster_count = 0;
static unsigned int hidden_sectors = 0;
static unsigned int info_sector = 0;
static unsigned int media_descriptor = 0xf8;
static unsigned int number_of_fats = 2;
static unsigned int reserved_sectors = 0;
static unsigned int root_cluster = 2;
static unsigned int root_entries = 512;
static unsigned int sectors_per_cluster = 4;
static unsigned int sectors_per_fat = 0;

struct mkfs_state *state = 0;
const char *program_name = 0;

static unsigned char dummy_boot_code[] =

    "\x31\xC0"                                                                  /* xor ax, ax */
    "\xFA"                                                                      /* cli */
    "\x8E\xD0"                                                                  /* mov ss, ax */
    "\xBC\x00\x7C"                                                              /* mov sp, 0x7c00 */
    "\xFB"                                                                      /* sti */
    "\x0E\x1F"                                                                  /* push cs, pop ds */
    "\xEB\x19"                                                                  /* jmp XSTRING */
    
    "\x5E"                                                                      /* PRN: pop si */
    "\xFC"                                                                      /* cld */
    "\xAC"                                                                      /* XLOOP: lodsb */
    "\x08\xC0"                                                                  /* or al, al */
    "\x74\x09"                                                                  /* jz EOF */
    "\xB4\x0E"                                                                  /* mov ah, 0x0e */
    "\xBB\x07\x00"                                                              /* mov bx, 7 */
    "\xCD\x10"                                                                  /* int 0x10 */
    "\xEB\xF2"                                                                  /* jmp short XLOOP */
    
    "\x31\xC0"                                                                  /* EOF: xor ax, ax */
    "\xCD\x16"                                                                  /* int 0x16 */
    "\xCD\x19"                                                                  /* int 0x19 */
    "\xF4"                                                                      /* HANG: hlt */
    "\xEB\xFD"                                                                  /* jmp short HANG */
    
    "\xE8\xE4\xFF"                                                              /* XSTRINGL call PRN */
    
    "Non-System disk or disk read error\r\n"
    "Replace and strike any key when ready\r\n";

static int cdiv (int a, int b) {
    return (a + b - 1) / b;
}

static int seekto (long offset) {
    return fseek (ofp, (state->offset * 512) + offset, SEEK_SET);
}

static int set_fat_entry (unsigned int cluster, unsigned int value) {

    unsigned char *scratch;
    unsigned int i, offset, sector;
    
    if (!(scratch = (unsigned char *) malloc (512))) {
        return -1;
    }
    
    if (state->size_fat == 12) {
    
        offset = cluster + (cluster / 2);
        value &= 0x0fff;
    
    } else if (state->size_fat == 16) {
    
        offset = cluster * 2;
        value &= 0xffff;
    
    } else if (state->size_fat == 32) {
    
        offset = cluster * 4;
        value &= 0x0fffffff;
    
    } else {
    
        free (scratch);
        return -1;
    
    }
    
    /**
     * At this point, offset is the BYTE offset of the desired sector from the start
     * of the FAT.  Calculate the physical sector containing this FAT entry.
     */
    sector = (offset / 512) + reserved_sectors;
    
    if (seekto (sector * 512)) {
    
        free (scratch);
        
        report_at (program_name, 0, REPORT_ERROR, "failed whilst seeking %s", state->outfile);
        return -1;
    
    }
    
    if (fread (scratch, 512, 1, ofp) != 1) {
    
        free (scratch);
        
        report_at (program_name, 0, REPORT_ERROR, "failed whilst reading %s", state->outfile);
        return -1;
    
    }
    
    /**
     * At this point, we "merely" need to extract the relevant entry.  This is
     * easy for FAT16 and FAT32, but a royal PITA for FAT12 as a single entry
     * may span a sector boundary.  The normal way around this is always to
     * read two FAT sectors, but luxary is (by design intent) unavailable.
     */
    offset %= 512;
    
    if (state->size_fat == 12) {
    
        if (offset == 511) {
        
            if (((cluster * 3) & 0x01) == 0) {
                scratch[offset] = (unsigned char) (value & 0xFF);
            } else {
                scratch[offset] = (unsigned char) ((scratch[offset] & 0x0F) | (value & 0xF0));
            }
            
            for (i = 0; i < number_of_fats; i++) {
            
                long temp = sector + (i * sectors_per_fat);
                
                if (seekto (temp * 512) || fwrite (scratch, 512, 1, ofp) != 1) {
                
                    free (scratch);
                    return -1;
                
                }
            
            }
            
            sector++;
            
            if (seekto (sector * 512) || fread (scratch, 512, 1, ofp) != 1) {
            
                free (scratch);
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
    
    } else if (state->size_fat == 16) {
    
        scratch[offset] = (value & 0xFF);
        scratch[offset + 1] = (value >> 8) & 0xFF;
        
        goto _write_fat;
    
    } else if (state->size_fat == 32) {
    
        scratch[offset] = (value & 0xFF);
        scratch[offset + 1] = (value >> 8) & 0xFF;
        scratch[offset + 2] = (value >> 16) & 0xFF;
        scratch[offset + 3] = (scratch[offset + 3] & 0xF0) | ((value >> 24) & 0xFF);
        
        goto _write_fat;
    
    }
    
    free (scratch);
    return -1;

_write_fat:

    for (i = 0; i < number_of_fats; i++) {
    
        long temp = sector + (i * sectors_per_fat);
        
        if (seekto (temp * 512) || fwrite (scratch, 512, 1, ofp) != 1) {
        
            free (scratch);
            return -1;
        
        }
    
    }
    
    free (scratch);
    return 0;

}

static unsigned int align_object (unsigned int sectors, unsigned int clustsize) {

    if (align_structures) {
        return (sectors + clustsize - 1) & ~(clustsize - 1);
    }
    
    return sectors;

}

static unsigned int generate_volume_id (void) {

#if     defined (__PDOS__)

    srand (time (NULL));
    
    /* rand() returns int from [0,RAND_MAX], therefor only 31-bits. */
    return (((unsigned int) (rand () & 0xFFFF)) << 16) | ((unsigned int) (rand() & 0xFFFF));

#elif   defined (__GNUC__)

    struct timeval now;
    
    if (gettimeofday (&now, 0) != 0 || now.tv_sec == (time_t) -1 || now.tv_sec < 0) {
    
        srand (getpid ());
        
        /*- rand() returns int from [0,RAND_MAX], therefor only 31-bits. */
        return (((unsigned int) (rand () & 0xFFFF)) << 16) | ((unsigned int) (rand() & 0xFFFF));
    
    }
    
    /* volume id = current time, fudged for more uniqueness. */
    return ((unsigned int) now.tv_sec << 20) | (unsigned int) now.tv_usec;

#elif   defined (__WATCOMC__)

    srand (getpid ());
    
    /* rand() returns int from [0,RAND_MAX], therefor only 31-bits. */
    return (((unsigned int) (rand () & 0xFFFF)) << 16) | ((unsigned int) (rand() & 0xFFFF));

#endif

}

static void establish_bpb (void) {

    unsigned int maxclustsize, root_dir_sectors;
    
    unsigned int clust12, clust16, clust32;
    unsigned int fatdata1216, fatdata32;
    unsigned int fatlength12, fatlength16, fatlength32;
    unsigned int maxclust12, maxclust16, maxclust32;
    
    int cylinder_times_heads;
    total_sectors = (image_size / 512) + orphaned_sectors;
    
    if ((unsigned long) total_sectors > UINT_MAX) {
    
        report_at (program_name, 0, REPORT_WARNING, "target too large, space at end will be left unused.\n");
        total_sectors = UINT_MAX;
    
    }
    
    if (total_sectors > (long) 65535 * 16 * 63) {
    
        heads_per_cylinder = 255;
        sectors_per_track = 63;
        
        cylinder_times_heads = total_sectors / sectors_per_track;
    
    } else {
    
        sectors_per_track = 17;
        cylinder_times_heads = total_sectors / sectors_per_track;
        
        heads_per_cylinder = (cylinder_times_heads + 1023) >> 10;
        
        if (heads_per_cylinder < 4) {
            heads_per_cylinder = 4;
        }
        
        if (cylinder_times_heads >= heads_per_cylinder << 10 || heads_per_cylinder > 16) {
        
            sectors_per_track = 31;
            heads_per_cylinder = 16;
            
            cylinder_times_heads = total_sectors / sectors_per_track;
        
        }
        
        if (cylinder_times_heads >= heads_per_cylinder << 10) {
        
            sectors_per_track = 63;
            heads_per_cylinder = 16;
            
            cylinder_times_heads = total_sectors / sectors_per_track;
        
        }
    
    }
    
    switch (total_sectors) {
    
        case 320:                                                               /* 160KB 5.25" */
        
            sectors_per_cluster = 2;
            root_entries = 112;
            media_descriptor = 0xfe;
            sectors_per_track = 8;
            heads_per_cylinder = 1;
            break;
        
        case 360:                                                               /* 180KB 5.25" */
        
            sectors_per_cluster = 2;
            root_entries = 112;
            media_descriptor = 0xfc;
            sectors_per_track = 9;
            heads_per_cylinder = 1;
            break;
        
        case 640:                                                               /* 320KB 5.25" */
        
            sectors_per_cluster = 2;
            root_entries = 112;
            media_descriptor = 0xff;
            sectors_per_track = 8;
            heads_per_cylinder = 2;
            break;
        
        case 720:                                                               /* 360KB 5.25" */
        
            sectors_per_cluster = 2;
            root_entries = 112;
            media_descriptor = 0xfd;
            sectors_per_track = 9;
            heads_per_cylinder = 2;
            break;
        
        case 1280:                                                              /* 640KB 5.25" / 3.5" */
        
            sectors_per_cluster = 2;
            root_entries = 112;
            media_descriptor = 0xfb;
            sectors_per_track = 8;
            heads_per_cylinder = 2;
            break;
        
        case 1440:                                                              /* 720KB 5.25" / 3.5" */
        
            sectors_per_cluster = 2;
            root_entries = 112;
            media_descriptor = 0xf9;
            sectors_per_track = 9;
            heads_per_cylinder = 2;
            break;
        
        case 1640:                                                              /* 820KB 3.5" */
        
            sectors_per_cluster = 2;
            root_entries = 112;
            media_descriptor = 0xf9;
            sectors_per_track = 10;
            heads_per_cylinder = 2;
            break;
        
        case 2400:                                                              /* 1.20MB 5.25" / 3.5" */
        
            sectors_per_cluster = 1;
            root_entries = 224;
            media_descriptor = 0xf9;
            sectors_per_track = 15;
            heads_per_cylinder = 2;
            break;
        
        case 2880:                                                              /* 1.44MB 3.5" */
        
            sectors_per_cluster = 1;
            root_entries = 224;
            media_descriptor = 0xf0;
            sectors_per_track = 18;
            heads_per_cylinder = 2;
            break;
        
        case 3360:                                                              /* 1.68MB 3.5" */
        
            sectors_per_cluster = 1;
            root_entries = 224;
            media_descriptor = 0xf0;
            sectors_per_track = 21;
            heads_per_cylinder = 2;
            break;
        
        case 3444:                                                              /* 1.72MB 3.5" */
        
            sectors_per_cluster = 1;
            root_entries = 224;
            media_descriptor = 0xf0;
            sectors_per_track = 21;
            heads_per_cylinder = 2;
            break;
        
        case 5760:                                                              /* 2.88MB 3.5" */
        
            sectors_per_cluster = 2;
            root_entries = 240;
            media_descriptor = 0xf0;
            sectors_per_track = 36;
            heads_per_cylinder = 2;
            break;
    
    }
    
    if (!state->size_fat && image_size >= (long) 512 * 1024 * 1024) {
        state->size_fat = 32;
    }
    
    if (state->size_fat == 32) {
    
        root_entries = 0;
        
        /*
         * For FAT32, try to do the same as M$'s format command
         * (see http://www.win.tue.nl/~aeb/linux/fs/fat/fatgen103.pdf p. 20):
         * 
         * fs size <= 260M: 0.5k clusters
         * fs size <=   8G:   4k clusters
         * fs size <=  16G:   8k clusters
         * fs size <=  32G:  16k clusters
         * fs size >   32G:  32k clusters
         */
        sectors_per_cluster = (total_sectors > 32 * 1024 * 1024 * 2 ? 64 :
                               total_sectors > 16 * 1024 * 1024 * 2 ? 32 :
                               total_sectors >  8 * 1024 * 1024 * 2 ? 16 :
                               total_sectors >       260 * 1024 * 2 ?  8 : 1);
    
    }
    
    hidden_sectors = state->offset;
    
    if (!reserved_sectors) {
        reserved_sectors = (state->size_fat == 32 ? 32 : 1);
    }
    
    /*if (align_structures) {*/
    
        /** Align number of sectors to be multiple of sectors per track, needed by DOS and mtools. */
        /*total_sectors = total_sectors / sectors_per_track * sectors_per_track;*/
    
    /*}*/
    
    if (total_sectors <= 8192) {
    
        if (align_structures && state->verbose) {
            report_at (program_name, 0, REPORT_WARNING, "Disabling alignment due to tiny filsystem\n");
        }
        
        align_structures = 0;
    
    }
    
    maxclustsize = 128;
    root_dir_sectors = cdiv (root_entries * 32, 512);
    
    do {
    
        fatdata32 = total_sectors - align_object (reserved_sectors, sectors_per_cluster);
        fatdata1216 = fatdata32 - align_object (root_dir_sectors, sectors_per_cluster);
        
        if (state->verbose) {
            fprintf (stderr, "Trying with %d sectors/cluster:\n", sectors_per_cluster);
        }
        
        /**
         * The factor 2 below avoids cut-off errors for number_of_fats == 1.
         * The "number_of_fats * 3" is for the reserved first two FAT entries.
         */
        clust12 = 2 * ((long) fatdata1216 * 512 + number_of_fats * 3) / (2 * (int) sectors_per_cluster * 512 + number_of_fats * 3);
        fatlength12 = cdiv (((clust12 + 2) * 3 + 1) >> 1, 512);
        fatlength12 = align_object (fatlength12, sectors_per_cluster);
        
        /**
         * Need to recalculate number of clusters, since the unused parts of the
         * FATs and data area together could make up space for an additional,
         * not really present cluster.
         */
        clust12 = (fatdata1216 - number_of_fats * fatlength12) / sectors_per_cluster;
        maxclust12 = (fatlength12 * 2 * 512) / 3;
        
        if (maxclust12 > MAX_CLUST_12) {
            maxclust12 = MAX_CLUST_12;
        }
        
        if (state->verbose && (state->size_fat == 0 || state->size_fat == 12)) {
            fprintf (stderr, "Trying FAT12: #clu=%u, fatlen=%u, maxclu=%u, limit=%u\n", clust12, fatlength12, maxclust12, MAX_CLUST_12);
        }
        
        if (clust12 > maxclust12) {
        
            clust12 = 0;
            
            if (state->verbose && (state->size_fat == 0 || state->size_fat == 12)) {
                fprintf (stderr, "Trying FAT12: too many clusters\n");
            }
        
        }
        
        clust16 = ((long) fatdata1216 * 512 + number_of_fats * 4) / ((int) sectors_per_cluster * 512 + number_of_fats * 2);
        fatlength16 = cdiv ((clust16 + 2) * 2, 512);
        fatlength16 = align_object (fatlength16, sectors_per_cluster);
        
        /**
         * Need to recalculate number of clusters, since the unused parts of the
         * FATs and data area together could make up space for an additional,
         * not really present cluster.
         */
        clust16 = (fatdata1216 - number_of_fats * fatlength16) / sectors_per_cluster;
        maxclust16 = (fatlength16 * 512) / 2;
        
        if (maxclust16 > MAX_CLUST_16) {
            maxclust16 = MAX_CLUST_16;
        }
        
        if (state->verbose && (state->size_fat == 0 || state->size_fat == 16)) {
            fprintf (stderr, "Trying FAT16: #clu=%u, fatlen=%u, maxclu=%u, limit=%u/%u\n", clust16, fatlength16, maxclust16, MIN_CLUST_16, MAX_CLUST_16);
        }
        
        if (clust16 > maxclust16) {
        
            clust16 = 0;
            
            if (state->verbose && (state->size_fat == 0 || state->size_fat == 16)) {
                fprintf (stderr, "Trying FAT16: too many clusters\n");
            }
        
        }
        
        /** This avoids that the filesystem will be misdetected as having a 12-bit FAT. */
        if (clust16 && clust16 < MIN_CLUST_16) {
        
            clust16 = 0;
            
            if (state->verbose && (state->size_fat == 0 || state->size_fat == 16)) {
                fprintf (stderr, "Trying FAT16: not enough clusters, would be misdected as FAT12\n");
            }
        
        }
        
        clust32 = ((long) fatdata32 * 512 + number_of_fats * 8) / ((int) sectors_per_cluster * 512 + number_of_fats * 4);
        fatlength32 = cdiv ((clust32 + 2) * 4, 512);
        fatlength32 = align_object (fatlength32, sectors_per_cluster);
        
        /**
         * Need to recalculate number of clusters, since the unused parts of the
         * FATs and data area together could make up space for an additional,
         * not really present cluster.
         */
        clust32 = (fatdata32 - number_of_fats * fatlength32) / sectors_per_cluster;
        maxclust32 = (fatlength32 * 512) / 4;
        
        if (maxclust32 > MAX_CLUST_32) {
            maxclust32 = MAX_CLUST_32;
        }
        
        if (state->verbose && (state->size_fat == 0 || state->size_fat == 32)) {
            fprintf (stderr, "Trying FAT32: #clu=%u, fatlen=%u, maxclu=%u, limit=%u/%u\n", clust32, fatlength32, maxclust32, MIN_CLUST_32, MAX_CLUST_32);
        }
        
        if (clust32 > maxclust32) {
        
            clust32 = 0;
            
            if (state->verbose && (state->size_fat == 0 || state->size_fat == 32)) {
                fprintf (stderr, "Trying FAT32: too many clusters\n");
            }
        
        }
        
        if (clust32 && clust32 < MIN_CLUST_32 && !(state->size_fat_by_user && state->size_fat == 32)) {
        
            clust32 = 0;
            
            if (state->verbose && (state->size_fat == 0 || state->size_fat == 32)) {
                fprintf (stderr, "Trying FAT32: not enough clusters\n");
            }
        
        }
        
        if ((clust12 && (state->size_fat == 0 || state->size_fat == 12)) || (clust16 && (state->size_fat == 0 || state->size_fat == 16)) || (clust32 && state->size_fat == 32)) {
            break;
        }
        
        sectors_per_cluster <<= 1;
    
    } while (sectors_per_cluster && sectors_per_cluster <= maxclustsize);
    
    /** Use the optimal FAT size if not specified. */
    if (!state->size_fat) {
    
        state->size_fat = (clust16 > clust12 ? 16 : 12);
        
        if (state->verbose) {
            report_at (program_name, 0, REPORT_WARNING, "Choosing %d-bits for FAT\n", state->size_fat);
        }
    
    }
    
    switch (state->size_fat) {
    
        case 12:
        
            cluster_count = clust12;
            sectors_per_fat = fatlength12;
            break;
        
        case 16:
        
            cluster_count = clust16;
            sectors_per_fat = fatlength16;
            break;
        
        case 32:
        
            cluster_count = clust32;
            sectors_per_fat = fatlength32;
            break;
        
        default:
        
            report_at (program_name, 0, REPORT_ERROR, "FAT not 12, 16 or 32 bits");
            
            fclose (ofp);
            remove (state->outfile);
            
            exit (EXIT_FAILURE);
    
    }
    
    /** Adjust the reserved number of sectors for alignment. */
    reserved_sectors = align_object (reserved_sectors, sectors_per_cluster);
    
    /** Adjust the number of root directory entries to help enforce alignment. */
    if (align_structures) {
        root_entries = align_object (root_dir_sectors, sectors_per_cluster) * (512 >> 5);
    }
    
    if (state->size_fat == 32) {
    
        if (!info_sector) {
            info_sector = 1;
        }
        
        if (!backup_boot) {
        
            if (reserved_sectors >= 7 && info_sector != 6) {
                backup_boot = 6;
            } else if (reserved_sectors > 3 + info_sector && info_sector != reserved_sectors - 2 && info_sector != reserved_sectors - 1) {
                backup_boot = reserved_sectors - 2;
            } else if (reserved_sectors >= 3 && info_sector != reserved_sectors - 1) {
                backup_boot = reserved_sectors - 1;
            }
        
        }
        
        if (backup_boot) {
        
            if (backup_boot == info_sector) {
            
                report_at (program_name, 0, REPORT_ERROR, "Backup boot sector must not be the same as the info sector (%d)", info_sector);
                
                fclose (ofp);
                remove (state->outfile);
                
                exit (EXIT_FAILURE);
            
            } else if (backup_boot >= reserved_sectors) {
            
                report_at (program_name, 0, REPORT_ERROR, "Backup boot sector must be a reserved sector");
                
                fclose (ofp);
                remove (state->outfile);
                
                exit (EXIT_FAILURE);
            
            }
        
        }
        
        if (state->verbose) {
            fprintf (stderr, "Using sector %d as backup boot sector (0 = none)\n", backup_boot);
        }
    
    }
    
    if (!cluster_count) {
    
        report_at (program_name, 0, REPORT_ERROR, "Not enough clusters to make a viable filesystem");
        
        fclose (ofp);
        remove (state->outfile);
        
        exit (EXIT_FAILURE);
    
    }

}

static void add_volume_label (void) {

    struct msdos_dirent *de;
    unsigned short date, time;
    
    unsigned char *scratch;
    long offset = 0;
    
    if (!(scratch = (unsigned char *) malloc (512))) {
    
        report_at (program_name, 0, REPORT_ERROR, "Failed to allocate memory");
        
        fclose (ofp);
        remove (state->outfile);
        
        exit (EXIT_FAILURE);
    
    }
    
    if (state->size_fat == 32) {
    
        long temp = reserved_sectors + (sectors_per_fat * 2);
        offset += temp + ((root_cluster - 2) * sectors_per_cluster);
    
    } else {
        offset += reserved_sectors + (sectors_per_fat * 2);
    }
    
    if (seekto (offset * 512) || fwrite (scratch, 512, 1, ofp) != 1) {
    
        report_at (program_name, 0, REPORT_ERROR, "Failed whilst reading root directory");
        free (scratch);
        
        fclose (ofp);
        remove (state->outfile);
        
        exit (EXIT_FAILURE);
    
    }
    
    de = (struct msdos_dirent *) scratch;
    memset (de, 0, sizeof (*de));
    
    date = generate_datestamp ();
    time = generate_timestamp ();
    
    memcpy (de->name, state->label, 11);
    de->attr = ATTR_VOLUME_ID;
    
    write721_to_byte_array (de->ctime, time);
    write721_to_byte_array (de->cdate, date);
    write721_to_byte_array (de->adate, date);
    write721_to_byte_array (de->time, time);
    write721_to_byte_array (de->date, date);
    
    if (seekto (offset * 512) || fwrite (scratch, 512, 1, ofp) != 1) {
    
        report_at (program_name, 0, REPORT_ERROR, "Failed whilst writing root directory");
        free (scratch);
        
        fclose (ofp);
        remove (state->outfile);
        
        exit (EXIT_FAILURE);
    
    }
    
    free (scratch);

}

static void wipe_target (void) {

    unsigned int i, sectors_to_wipe = 0;
    void *blank;
    
    if (!(blank = malloc (512))) {
    
        report_at (program_name, 0, REPORT_ERROR, "Failed to allocate memory");
        
        fclose (ofp);
        remove (state->outfile);
        
        exit (EXIT_FAILURE);
    
    }
    
    memset (blank, 0, 512);
    
    sectors_to_wipe += reserved_sectors;
    sectors_to_wipe += sectors_per_fat * 2;
    
    if (root_entries) {
        sectors_to_wipe += (root_entries * 32) / 512;
    } else {
        sectors_to_wipe += 1;
    }
    
    seekto (0);
    
    for (i = 0; i < sectors_to_wipe; i++) {
    
        if (fwrite (blank, 512, 1, ofp) != 1) {
        
            report_at (program_name, 0, REPORT_ERROR, "Failed whilst writing blank sector");
            free (blank);
            
            fclose (ofp);
            remove (state->outfile);
            
            exit (EXIT_FAILURE);
        
        }
    
    }
    
    free (blank);

}

static void write_reserved (void) {

    struct msdos_boot_sector bs;
    struct msdos_volume_info *vi = (state->size_fat == 32 ? &bs.fstype._fat32.vi : &bs.fstype._oldfat.vi);
    
    memset (&bs, 0, sizeof (bs));
    
    if (state->boot) {
    
        FILE *ifp;
        
        if ((ifp = fopen (state->boot, "rb")) == NULL) {
        
            report_at (program_name, 0, REPORT_ERROR, "unable to open %s", state->boot);
            
            fclose (ofp);
            remove (state->outfile);
            
            exit (EXIT_FAILURE);
        
        }
        
        fseek (ifp, 0, SEEK_END);
        
        if (ftell (ifp) != sizeof (bs)) {
        
            report_at (program_name, 0, REPORT_ERROR, "boot sector must be %lu bytes in size", (unsigned long) sizeof (bs));
            fclose (ifp);
            
            fclose (ofp);
            remove (state->outfile);
            
            exit (EXIT_FAILURE);
        
        }
        
        fseek (ifp, 0, SEEK_SET);
        
        if (fread (&bs, sizeof (bs), 1, ifp) != 1) {
        
            report_at (program_name, 0, REPORT_ERROR, "failed to read %s", state->boot);
            fclose (ifp);
            
            fclose (ofp);
            remove (state->outfile);
            
            exit (EXIT_FAILURE);
        
        }
        
        fclose (ifp);
    
    } else {
    
        bs.boot_jump[0] = 0xEB;
        bs.boot_jump[1] = ((state->size_fat == 32 ? (char *) &bs.fstype._fat32.boot_code : (char *) &bs.fstype._oldfat.boot_code) - (char *) &bs) - 2;
        bs.boot_jump[2] = 0x90;
        
        if (state->size_fat == 32) {
            memcpy (bs.fstype._fat32.boot_code, dummy_boot_code, sizeof (dummy_boot_code));
        } else {
            memcpy (bs.fstype._oldfat.boot_code, dummy_boot_code, sizeof (dummy_boot_code));
        }
        
        bs.boot_sign[0] = 0x55;
        bs.boot_sign[1] = 0xAA;
    
    }
    
    if (bs.boot_jump[0] != 0xEB || bs.boot_jump[1] < 0x16 || bs.boot_jump[2] != 0x90) {
        goto _write_reserved;
    }
    
    memcpy (bs.system_id, "MSWIN4.1", 8);
    
    bs.sectors_per_cluster = sectors_per_cluster;
    bs.no_fats = number_of_fats;
    bs.media_descriptor = media_descriptor;
    
    write721_to_byte_array (bs.bytes_per_sector, 512);
    write721_to_byte_array (bs.reserved_sectors, reserved_sectors);
    write721_to_byte_array (bs.root_entries, root_entries);
    write721_to_byte_array (bs.total_sectors16, total_sectors);
    write721_to_byte_array (bs.sectors_per_fat16, sectors_per_fat);
    
    if (bs.boot_jump[1] < 0x22) {
        goto _write_reserved;
    }
    
    write721_to_byte_array (bs.sectors_per_track, sectors_per_track);
    write721_to_byte_array (bs.heads_per_cylinder, heads_per_cylinder);
    write741_to_byte_array (bs.hidden_sectors, hidden_sectors);
    
    if (total_sectors > USHRT_MAX) {
    
        write721_to_byte_array (bs.total_sectors16, 0);
        write741_to_byte_array (bs.total_sectors32, total_sectors);
    
    }
    
    if (state->size_fat == 32) {
    
        if (bs.boot_jump[1] < 0x58) {
            goto _write_reserved;
        }
        
        write721_to_byte_array (bs.sectors_per_fat16, 0);
        write741_to_byte_array (bs.fstype._fat32.sectors_per_fat32, sectors_per_fat);
        
        write741_to_byte_array (bs.fstype._fat32.root_cluster, root_cluster);
        write721_to_byte_array (bs.fstype._fat32.info_sector, info_sector);
        write721_to_byte_array (bs.fstype._fat32.backup_boot, backup_boot);
        
        vi->drive_no = (media_descriptor == 0xF8 ? 0x80 : 0x00);
        vi->ext_boot_sign = 0x29;
        
        write741_to_byte_array (vi->volume_id, generate_volume_id ());
        memcpy (vi->volume_label, state->label, 11);
        memcpy (vi->fs_type, "FAT32   ", 8);
    
    } else {
    
        if (bs.boot_jump[1] < 0x3C) {
            goto _write_reserved;
        }
        
        vi->drive_no = (media_descriptor == 0xF8 ? 0x80 : 0x00);
        vi->ext_boot_sign = 0x29;
        
        write741_to_byte_array (vi->volume_id, generate_volume_id ());
        memcpy (vi->volume_label, state->label, 11);
        
        if (state->size_fat == 12) {
            memcpy (vi->fs_type, "FAT12   ", 8);
        } else {
            memcpy (vi->fs_type, "FAT16   ", 8);
        }
    
    }

_write_reserved:

    seekto (0);
    
    if (fwrite (&bs, sizeof (bs), 1, ofp) != 1) {
    
        report_at (program_name, 0, REPORT_ERROR, "Failed whilst writing %s", state->outfile);
        
        fclose (ofp);
        remove (state->outfile);
        
        exit (EXIT_FAILURE);
    
    }
    
    if (state->size_fat == 32) {
    
        if (info_sector) {
        
            struct fat32_fsinfo *info;
            unsigned char *buffer;
            
            if (seekto (info_sector * 512)) {
            
                report_at (program_name, 0, REPORT_ERROR, "Failed whilst seeking %s", state->outfile);
                
                fclose (ofp);
                remove (state->outfile);
                
                exit (EXIT_FAILURE);
            
            }
            
            if (!(buffer = (unsigned char *) malloc (512))) {
            
                report_at (program_name, 0, REPORT_ERROR, "Failed to allocate memory");
                
                fclose (ofp);
                remove (state->outfile);
                
                exit (EXIT_FAILURE);
            
            }
            
            memset (buffer, 0, 512);
            
            /** fsinfo structure is at offset 0x1e0 in info sector by observation. */
            info = (struct fat32_fsinfo *) (buffer + 0x1e0);
            
            /** Info sector magic. */
            buffer[0] = 'R';
            buffer[1] = 'R';
            buffer[2] = 'a';
            buffer[3] = 'A';
            
            /** Magic for fsinfo structure. */
            write741_to_byte_array (info->signature, 0x61417272);
            
            /** We've allocated cluster 2 for the root directory. */
            write741_to_byte_array (info->free_clusters, cluster_count - 1);
            write741_to_byte_array (info->next_cluster, 2);
            
            /** Info sector also must have boot sign. */
            write721_to_byte_array (buffer + 0x1fe, 0xAA55);
            
            if (fwrite (buffer, 512, 1, ofp) != 1) {
            
                report_at (program_name, 0, REPORT_ERROR, "Failed whilst writing info sector");
                free (buffer);
                
                fclose (ofp);
                remove (state->outfile);
                
                exit (EXIT_FAILURE);
            
            }
            
            free (buffer);
        
        }
        
        if (backup_boot) {
        
            if (seekto (backup_boot * 512) || fwrite (&bs, sizeof (bs), 1, ofp) != 1) {
            
                report_at (program_name, 0, REPORT_ERROR, "Failed whilst writing info sector");
                
                fclose (ofp);
                remove (state->outfile);
                
                exit (EXIT_FAILURE);
            
            }
        
        }
    
    }

}

int main (int argc, char **argv) {

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
    
        report_at (program_name, 0, REPORT_ERROR, "no outfile file provided");
        return EXIT_FAILURE;
    
    }
    
    image_size = state->blocks * 1024;
    image_size += state->offset * 512;
    
    if ((ofp = fopen (state->outfile, "r+b")) == NULL) {
    
        size_t len;
        void *zero;
        
        if ((ofp = fopen (state->outfile, "w+b")) == NULL) {
        
            report_at (program_name, 0, REPORT_ERROR, "failed to open '%s' for writing", state->outfile);
            return EXIT_FAILURE;
        
        }
        
        len = image_size;
        zero = xmalloc (512);
        
        while (len > 0) {
        
            if (fwrite (zero, 512, 1, ofp) != 1) {
            
                report_at (program_name, 0, REPORT_ERROR, "failed whilst writing '%s'", state->outfile);
                fclose (ofp);
                
                free (zero);
                remove (state->outfile);
                
                return EXIT_FAILURE;
            
            }
            
            len -= 512;
        
        }
        
        free (zero);
    
    }
    
    if (image_size == 0) {
    
        fseek (ofp, 0, SEEK_END);
        image_size = ftell (ofp);
    
    }
    
    seekto (0);
    
    if (state->offset * 512 > image_size) {
    
        report_at (program_name, 0, REPORT_ERROR, "size (%lu) of %s is less than the requested offset (%lu)", image_size, state->outfile, state->offset * 512);
        
        fclose (ofp);
        remove (state->outfile);
        
        return EXIT_FAILURE;
    
    }
    
    image_size -= state->offset * 512;
    
    if (state->blocks) {
    
        if (state->blocks * 1024 > image_size) {
        
            report_at (program_name, 0, REPORT_ERROR, "size (%lu) of %s is less than the requested size (%lu)", image_size, state->outfile, state->blocks * 1024);
            
            fclose (ofp);
            remove (state->outfile);
            
            return EXIT_FAILURE;
        
        }
        
        image_size = state->blocks * 1024;
    
    }
    
    orphaned_sectors = (image_size % 1024) / 512;
    
    establish_bpb ();
    wipe_target ();
    
    write_reserved ();
    
    if (set_fat_entry (0, 0xFFFFFF00 | media_descriptor) < 0 || set_fat_entry (1, 0xFFFFFFFF) < 0) {
    
        report_at (program_name, 0, REPORT_ERROR, "Failed whilst setting FAT entry");
        
        fclose (ofp);
        remove (state->outfile);
        
        return EXIT_FAILURE;
    
    }
    
    if (state->size_fat == 32) {
    
        if (set_fat_entry (2, 0x0FFFFFF8) < 0) {
        
            report_at (program_name, 0, REPORT_ERROR, "Failed whilst setting FAT entry");
            
            fclose (ofp);
            remove (state->outfile);
            
            return EXIT_FAILURE;
        
        }
    
    }
    
    if (memcmp (state->label, "NO NAME    ", 11) != 0) {
        add_volume_label ();
    }
    
    fclose (ofp);
    return EXIT_SUCCESS;

}
