/******************************************************************************
 * @file            msdos.h
 *****************************************************************************/
#ifndef     _MSDOS_H
#define     _MSDOS_H

/**
 * According to Microsoft FAT specification (fatgen103.doc) disk with
 * 4085 clusters (or more) is FAT16, but Microsoft Windows FAT driver
 * fastfat.sys detects disk with less then 4087 clusters as FAT12.
 * Linux FAT drivers msdos.ko and vfat.ko detect disk with at least
 * 4085 clusters as FAT16, therefore for compatibility reasons with
 * both systems disallow formatting disks to 4085 or 4086 clusters.
 */
#define     MAX_CLUST_12                4084
#define     MIN_CLUST_16                4087

/**
 * According to Microsoft FAT specification (fatgen103.doc) disk with
 * 65525 clusters (or more) is FAT32, but Microsoft Windows FAT driver
 * fastfat.sys, Linux FAT drivers msdos.ko and vfat.ko detect disk as
 * FAT32 when Sectors Per FAT (fat_length) is set to zero. And not by
 * number of clusters. Still there is cluster upper limit for FAT16.
 */
#define     MAX_CLUST_16                65524
#define     MIN_CLUST_32                65525

/**
 * M$ says the high 4 bits of a FAT32 FAT entry are reserved and don't belong
 * to the cluster number. So the max. cluster# is based on 2^28
 */
#define     MAX_CLUST_32                268435446

/** File Attributes. */
#define     ATTR_NONE                   0x00
#define     ATTR_READONLY               0x01
#define     ATTR_HIDDEN                 0x02
#define     ATTR_SYSTEM                 0x04
#define     ATTR_VOLUME_ID              0x08
#define     ATTR_DIR                    0x10
#define     ATTR_ARCHIVE                0x20
/*#define     ATTR_LONG_NAME              (ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_VOLUME_ID)*/

struct msdos_volume_info {

    unsigned char drive_no;
    unsigned char boot_flags;
    unsigned char ext_boot_sign;
    unsigned char volume_id[4];
    unsigned char volume_label[11];
    unsigned char fs_type[8];

};

struct msdos_boot_sector {

    unsigned char boot_jump[3];
    unsigned char system_id[8];
    unsigned char bytes_per_sector[2];
    unsigned char sectors_per_cluster;
    unsigned char reserved_sectors[2];
    unsigned char no_fats;
    unsigned char root_entries[2];
    unsigned char total_sectors16[2];
    unsigned char media_descriptor;
    unsigned char sectors_per_fat16[2];
    unsigned char sectors_per_track[2];
    unsigned char heads_per_cylinder[2];
    unsigned char hidden_sectors[4];
    unsigned char total_sectors32[4];
    
    union {
    
        struct {
        
            struct msdos_volume_info vi;
            unsigned char boot_code[448];
        
        } _oldfat;
        
        struct {
        
            unsigned char sectors_per_fat32[4];
            unsigned char flags[2];
            unsigned char version[2];
            unsigned char root_cluster[4];
            unsigned char info_sector[2];
            unsigned char backup_boot[2];
            unsigned short reserved2[6];
            
            struct msdos_volume_info vi;
            unsigned char boot_code[420];
        
        } _fat32;
    
    } fstype;
    
    unsigned char boot_sign[2];

};

#define     oldfat                      fstype._oldfat
#define     fat32                       fstype._fat32

struct fat32_fsinfo {

    unsigned char reserved1[4];
    unsigned char signature[4];
    unsigned char free_clusters[4];
    unsigned char next_cluster[4];

};

struct msdos_dirent {

    unsigned char name[11];
    unsigned char attr;
    unsigned char lcase;
    unsigned char ctime_cs;
    unsigned char ctime[2];
    unsigned char cdate[2];
    unsigned char adate[2];
    unsigned char starthi[2];
    unsigned char time[2];
    unsigned char date[2];
    unsigned char startlo[2];
    unsigned char size[4];

};

#endif      /* _MSDOS_H */
