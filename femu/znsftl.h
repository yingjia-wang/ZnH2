#ifndef __FEMU_ZNSFTL_H
#define __FEMU_ZNSFTL_H

#include "../nvme.h"

enum {
    NAND_SLC_READ =  0,
    NAND_SLC_PROG = 1,
    NAND_SLC_ERASE = 2,

    NAND_QLC_READ_L = 3,
    NAND_QLC_READ_CL = 4,
    NAND_QLC_READ_CU = 5,
    NAND_QLC_READ_U = 6,
    NAND_QLC_PROG_L = 7,
    NAND_QLC_PROG_CL = 8,
    NAND_QLC_PROG_CU = 9,
    NAND_QLC_PROG_U = 10,
    NAND_QLC_ERASE = 11,

    NAND_SLC_READ_LAT = 30000,
    NAND_SLC_PROG_LAT = 160000,
    NAND_SLC_ERASE_LAT = 3000000,

    NAND_QLC_READ_L_LAT = 48000,
    NAND_QLC_READ_CL_LAT = 64000,
    NAND_QLC_READ_CU_LAT = 80000,
    NAND_QLC_READ_U_LAT = 96000,
    NAND_QLC_PROG_L_LAT = 850000,
    NAND_QLC_PROG_CL_LAT = 2300000,
    NAND_QLC_PROG_CU_LAT = 3750000,
    NAND_QLC_PROG_U_LAT = 5200000,
    NAND_QLC_ERASE_LAT = 3500000,
};

struct zone_cmd {
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

struct nand_block {
    int npgs;
    int erase_cnt;
    int wp;
};

struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

struct nand_lun {
    struct nand_plane *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct zonessd_params {  
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_blk;  /* # of NAND pages per block */

    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */
    
    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */
   
    int tt_luns;      /* total # of LUNs in the SSD */

    int pgs_per_zone; /* # pages a zone */
    int blks_per_zone;/* # blocks a zone */ 

    bool enable_gc_delay; /* if gc delay is enabled */
};

struct block_locate {
    int ch;
    int lun; 
    int pl;
    int blk;
};

struct zone_block {
    struct block_locate *bl;
};

typedef struct Zone {
    struct write_pointer *wp;
    int zone_size;
    int pos;
    int status;

    uint64_t *buffer_persisted_time;
    int32_t buffer_cur_idx;
    uint64_t lpn_wp;
} zone;

struct write_pointer {
    zone *zone;
    int blk; 
    int pg;
};

struct zonessd {
    struct zonessd_params sp;
    struct ssd_channel *ch;
    struct zone_block *maptbl; /* zone -> block mapping table */

    zone *zone_array;
    uint64_t zone_size_bs;

    int zone_num;           /* # zones in ssd */
    int zone_size;          /* # pages in zone */
    int zone_size_log2;     /* if zone size isn't divisible by 2, return 0 */

    // for HZNS
    uint64_t total_cap;
    uint64_t used_cap;
    uint64_t used_slc_cap;
    uint64_t used_qlc_cap;
};

void zonessd_init(FemuCtrl *n);
void zonessd_read(struct zonessd *ssd, NvmeRequest *req, int mode);
void zonessd_write(struct zonessd *ssd, NvmeRequest *req, int mode);
void zonessd_copy(struct zonessd *ssd, NvmeRequest *req, int mode);
void zonessd_append(struct zonessd *ssd, NvmeRequest *req, int mode);
void zonessd_reset(struct FemuCtrl *n, int pos, NvmeRequest *req, int mode);
void zonessd_migrate(struct zonessd *ssd, uint64_t slba, uint64_t wp);
    
#endif
