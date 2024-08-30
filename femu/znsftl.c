#include "znsftl.h"

static void zonessd_init_params(struct zonessd *ssd)
{
    struct zonessd_params *spp = &ssd->sp;
    
    spp->secsz = 4096;       // sector 4KB
    spp->secs_per_pg = 4;    // page 16KB
    spp->pgs_per_blk = 1024;  // block 16MB
    spp->blks_per_pl = 128; 
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 8;           // ssd 128GB

    // spp->pg_rd_lat = NAND_SLC_READ_LATENCY;
    // spp->pg_wr_lat = NAND_SLC_PROG_LATENCY;
    // spp->blk_er_lat = NAND_SLC_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->pgs_per_zone = ssd->zone_size_bs / spp->secsz / spp->secs_per_pg;
    spp->blks_per_zone = spp->pgs_per_zone / spp->pgs_per_blk;

    spp->enable_gc_delay = true;
}

static void zonessd_init_nand_blk(struct nand_block *blk, struct zonessd_params *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void zonessd_init_nand_plane(struct nand_plane *pl, struct zonessd_params *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        zonessd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void zonessd_init_nand_lun(struct nand_lun *lun, struct zonessd_params *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        zonessd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void zonessd_init_ch(struct ssd_channel *ch, struct zonessd_params *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        zonessd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = false;
}

static void zonessd_init_zone_wp(zone *Zone, int pos) {
    Zone->wp->zone = Zone;
    Zone->wp->pg = 0;
    Zone->wp->blk = 0;
}

static void zonessd_init_zone_array(struct zonessd *ssd) {
    ssd->zone_array = g_malloc0(sizeof(zone) * ssd->zone_num);
    for (int i = 0; i < ssd->zone_num; i++) {
        ssd->zone_array[i].wp = g_malloc0(sizeof(struct write_pointer));
        zonessd_init_zone_wp(&(ssd->zone_array[i]), i);
        ssd->zone_array[i].pos = i;
        ssd->zone_array[i].zone_size = ssd->zone_size;

        ssd->zone_array[i].buffer_persisted_time = g_malloc0(sizeof(uint64_t) * 10);
        ssd->zone_array[i].buffer_cur_idx = 0;
        ssd->zone_array[i].lpn_wp = 0;
    }
}

static void zonessd_init_maptbl(struct zonessd *ssd)
{
    long long blk_cnt = 0;
    struct zonessd_params *spp = &ssd->sp;
    ssd->maptbl = g_malloc0(sizeof(struct zone_block) * ssd->zone_num);
    for (int zonenum = 0; zonenum < ssd->zone_num; zonenum++) {
        ssd->maptbl[zonenum].bl = g_malloc0(sizeof(struct block_locate) * ssd->sp.blks_per_zone);
        for (int blknum = 0; blknum < ssd->sp.blks_per_zone; blknum++) {
            ssd->maptbl[zonenum].bl[blknum].blk = blk_cnt / spp->tt_pls;
            ssd->maptbl[zonenum].bl[blknum].ch = (blk_cnt % spp->tt_pls) / spp->nchs;
            ssd->maptbl[zonenum].bl[blknum].lun = (blk_cnt % spp->tt_pls % spp->nchs) / spp->pls_per_lun;
            ssd->maptbl[zonenum].bl[blknum].pl = blk_cnt % spp->tt_pls % spp->nchs % spp->pls_per_lun;

            blk_cnt++;
        }
    }
}

void zonessd_init(FemuCtrl *n) {
    struct zonessd *ssd = n->znsssd;
    struct zonessd_params *spp = &ssd->sp;

    assert(ssd);

    ssd->zone_size_bs = n->zone_size_bs;

    zonessd_init_params(ssd);

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        zonessd_init_ch(&ssd->ch[i], spp);
    }

    ssd->zone_size = n->zone_size / spp->secs_per_pg;
    ssd->zone_size_log2 = 0;
    if (is_power_of_2(n->zone_size)) {
        ssd->zone_size_log2 = 63 - clz64(ssd->zone_size);
    }

    ssd->zone_num = spp->tt_pgs / spp->pgs_per_zone;
    ssd->zone_array = g_malloc0(sizeof(zone) * ssd->zone_num);

    zonessd_init_zone_array(ssd);

    /* initialize maptbl */
    zonessd_init_maptbl(ssd);

    ssd->total_cap = ssd->zone_num * ssd->zone_size * spp->secs_per_pg;
    ssd->used_cap = ssd->used_slc_cap = ssd->used_qlc_cap = 0;
    femu_debug("total_cap %lu zone_num %d zone_size %d\n", ssd->total_cap, ssd->zone_num, ssd->zone_size);
}
/* -------------------------------------------------------------------------------------------*/

static uint64_t ssd_advance_latency(struct zonessd *ssd, struct block_locate *bl, struct zone_cmd *ncmd) {
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    struct nand_lun *lun = &ssd->ch[bl->ch].lun[bl->lun];

    uint64_t prolonged_lat;

    if (c == NAND_SLC_READ) prolonged_lat = NAND_SLC_READ_LAT;
    else if (c == NAND_SLC_PROG) prolonged_lat = NAND_SLC_PROG_LAT;
    else if (c == NAND_SLC_ERASE) prolonged_lat = NAND_SLC_ERASE_LAT;
    else if (c == NAND_QLC_READ_L) prolonged_lat = NAND_QLC_READ_L_LAT;
    else if (c == NAND_QLC_READ_CL) prolonged_lat = NAND_QLC_READ_CL_LAT;
    else if (c == NAND_QLC_READ_CU) prolonged_lat = NAND_QLC_READ_CU_LAT;
    else if (c == NAND_QLC_READ_U) prolonged_lat = NAND_QLC_READ_U_LAT;
    else if (c == NAND_QLC_PROG_L) prolonged_lat = NAND_QLC_PROG_L_LAT;
    else if (c == NAND_QLC_PROG_CL) prolonged_lat = NAND_QLC_PROG_CL_LAT;
    else if (c == NAND_QLC_PROG_CU) prolonged_lat = NAND_QLC_PROG_CU_LAT;
    else if (c == NAND_QLC_PROG_U) prolonged_lat = NAND_QLC_PROG_U_LAT;
    else if (c == NAND_QLC_ERASE) prolonged_lat = NAND_QLC_ERASE_LAT;
    else assert(false);

    uint64_t nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                    lun->next_lun_avail_time;
    lun->next_lun_avail_time = nand_stime + prolonged_lat;

    return lun->next_lun_avail_time - cmd_stime;
}

static uint64_t ssd_advance_stripe_latency(struct zonessd *ssd, struct block_locate *bl, struct zone_cmd *ncmd) {
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime, maxlat = 0, sublat;
    struct nand_lun *lun = NULL;

    uint64_t prolonged_lat;
  
    if (c == NAND_SLC_PROG) prolonged_lat = NAND_SLC_PROG_LAT;
    else if (c == NAND_QLC_PROG_L) prolonged_lat = NAND_QLC_PROG_L_LAT;
    else if (c == NAND_QLC_PROG_CL) prolonged_lat = NAND_QLC_PROG_CL_LAT;
    else if (c == NAND_QLC_PROG_CU) prolonged_lat = NAND_QLC_PROG_CU_LAT;
    else if (c == NAND_QLC_PROG_U) prolonged_lat = NAND_QLC_PROG_U_LAT;
    else assert(false);

    // TO FIX
    for (int i = 7; i >= 0; i--) {
      for (int j = 0; j < 8; j++) {
        lun = &ssd->ch[bl->ch - i].lun[j];
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                      lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + prolonged_lat;

        sublat = lun->next_lun_avail_time - cmd_stime;
        maxlat = (sublat > maxlat) ? sublat : maxlat;
      }
    }

    return maxlat;
}

static inline uint32_t zns_zone_idx(struct zonessd *ssd, uint64_t slba)
{
    return (ssd->zone_size_log2 > 0 ? slba >> ssd->zone_size_log2 : slba /
            ssd->zone_size);
}

static void check_addr(int num, int limit) {
    if (num >= limit) {
        printf("num : %d  limit : %d\n", num, limit);
    }
    assert(num >= 0 && num < limit);
}

static struct block_locate *zonessd_l2p(struct zonessd *ssd, uint64_t slba) 
{
    uint32_t zone_idx = zns_zone_idx(ssd, slba);
    uint32_t block_idx = (slba - zone_idx * ssd->sp.pgs_per_zone) % ssd->sp.blks_per_zone;
    check_addr(zone_idx, ssd->zone_num);
    struct block_locate *bl = &ssd->maptbl[zone_idx].bl[block_idx];

    return bl;
}

// static void ssd_advance_wp(struct zonessd *ssd, int zone_idx) {
//     struct zonessd_params *sp = &ssd->sp;
//     struct write_pointer *wp = ssd->zone_array[zone_idx].wp;

//     check_addr(wp->blk, sp->blks_per_zone);
//     wp->blk++;
//     if (wp->blk == sp->blks_per_zone) {
//         wp->blk = 0;
//         check_addr(wp->pg, sp->pgs_per_blk);
//         wp->pg++;
//     }
// }

void zonessd_reset(struct FemuCtrl *n, int pos, NvmeRequest *req, int mode) {
    struct zonessd *ssd = n->znsssd;
    zone *Zone = &ssd->zone_array[pos];
    uint64_t maxlat = 0, sublat;
    for (int i = 0; i < ssd->sp.blks_per_zone; i++) {
        struct block_locate *bl = &ssd->maptbl[pos].bl[i];

        struct zone_cmd gce;
        gce.cmd = (mode == 0? NAND_SLC_ERASE : NAND_QLC_ERASE);
        gce.stime = req->stime;
        sublat = ssd_advance_latency(ssd, bl, &gce);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }
    Zone->wp->pg = 0;
    Zone->wp->blk = 0;

    req->reqlat = maxlat;

    return ;
}

void zonessd_read(struct zonessd *ssd, NvmeRequest *req, int mode)
{
    struct zonessd_params *spp = &ssd->sp;
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    uint64_t start_lpn = le64_to_cpu(rw->slba) / spp->secs_per_pg;
    uint64_t end_lpn = (le64_to_cpu(rw->slba) + (uint32_t)le16_to_cpu(rw->nlb)) / spp->secs_per_pg;

    uint64_t maxlat = 0, sublat;
    /* normal IO read path */
    for (uint64_t lpn = start_lpn; lpn <= end_lpn; lpn++) {
        struct block_locate *bl = zonessd_l2p(ssd, lpn);
        int page_type = (lpn / spp->tt_luns) % 4; // TO FIX: assume a zone spans all dies now

        struct zone_cmd srd;
        if (mode == 0) 
          srd.cmd = NAND_SLC_READ;
        else {
          // mode = 1, qlc
          if (page_type == 0)
            srd.cmd = NAND_QLC_READ_L;
          else if (page_type == 1)
            srd.cmd = NAND_QLC_READ_CL;
          else if (page_type == 2)
            srd.cmd = NAND_QLC_READ_CU;
          else
            srd.cmd = NAND_QLC_READ_U;
        }
        srd.stime = req->stime;
        sublat = ssd_advance_latency(ssd, bl, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    req->reqlat = maxlat;
    req->expire_time += maxlat;

    return ;
}

void zonessd_write(struct zonessd *ssd, NvmeRequest *req, int mode)
{
    struct zonessd_params *spp = &ssd->sp;
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    uint64_t start_lpn = le64_to_cpu(rw->slba) / spp->secs_per_pg;
    uint64_t end_lpn = (le64_to_cpu(rw->slba) + (uint32_t)le16_to_cpu(rw->nlb)) / spp->secs_per_pg;
    uint32_t zone_idx = zns_zone_idx(ssd, start_lpn);
    zone *z = &ssd->zone_array[zone_idx];

    uint64_t maxlat = 0, sublat;
    /* normal IO write path */
    for (uint64_t lpn = start_lpn; lpn <= end_lpn; lpn++) {
        struct block_locate *bl = zonessd_l2p(ssd, lpn);
        int page_type = (lpn / spp->tt_luns) % 4; // TO FIX: assume a zone spans all dies now

        // ssd_advance_wp(ssd, zone_idx);
        struct zone_cmd srd;
        if (mode == 0) 
          srd.cmd = NAND_SLC_PROG;
        else {
          // mode = 1, qlc
          if (page_type == 0)
            srd.cmd = NAND_QLC_PROG_L;
          else if (page_type == 1)
            srd.cmd = NAND_QLC_PROG_CL;
          else if (page_type == 2)
            srd.cmd = NAND_QLC_PROG_CU;
          else
            srd.cmd = NAND_QLC_PROG_U;
        }
        srd.stime = req->stime;

        // indicate we can flush a stripe
        if (bl->lun == 7 && bl->ch == 7 && z->lpn_wp < lpn) { // TO FIX
          sublat = ssd_advance_stripe_latency(ssd, bl, &srd);
          
          // record the buffer persisted time
          z->buffer_persisted_time[z->buffer_cur_idx] = req->stime + sublat;
          femu_debug("record bpt id %u time %lu\n", z->buffer_cur_idx, 
                        z->buffer_persisted_time[z->buffer_cur_idx]);
          if (++z->buffer_cur_idx == 10) z->buffer_cur_idx = 0; // TO FIX

          maxlat = (sublat > maxlat) ? sublat : maxlat;
        }
        z->lpn_wp = lpn;
    }

    // TO FIX: two stripe buffers now
    int buffer_num = 1, idx = z->buffer_cur_idx;
    for (int i = 0; i < buffer_num; i++) 
      idx = idx == 0? 9 : (idx - 1);
    if (req->stime < z->buffer_persisted_time[idx]) {
      req->reqlat = z->buffer_persisted_time[idx] - req->stime;
      req->expire_time = req->stime + req->reqlat;
    } else {
      req->reqlat = 0;
      req->expire_time = req->stime;
    }

    femu_debug("check bpt id %u time %lu stime %lu write lat %lu\n", idx, 
                  z->buffer_persisted_time[idx], req->stime, req->reqlat);

    return ;
}

void zonessd_copy(struct zonessd *ssd, NvmeRequest *req, int mode)
{
    struct zonessd_params *spp = &ssd->sp;
    NvmeRwCmd *rw = (NvmeRwCmd *)&req->cmd;
    uint64_t src_start_lpn = req->cmd.cdw14 / spp->secs_per_pg;
    uint64_t src_end_lpn = (req->cmd.cdw14 + req->cmd.cdw15) / spp->secs_per_pg;
    uint64_t dst_start_lpn = le64_to_cpu(rw->slba) / spp->secs_per_pg;
    uint64_t dst_end_lpn = (le64_to_cpu(rw->slba) + req->cmd.cdw15) / spp->secs_per_pg;
    uint32_t zone_idx = zns_zone_idx(ssd, dst_start_lpn);
    zone *z = &ssd->zone_array[zone_idx];

    uint64_t maxlat = 0, sublat;
    struct block_locate *bl = NULL;
    struct zone_cmd srd;
    int page_type;
    for (uint64_t lpn = src_start_lpn; lpn <= src_end_lpn; lpn++) {
        bl = zonessd_l2p(ssd, lpn);
        page_type = (lpn / spp->tt_luns) % 4; // TO FIX: assume a zone spans all dies now

        srd.cmd = NAND_SLC_READ;
        srd.stime = req->stime;

        sublat = ssd_advance_latency(ssd, bl, &srd);
        maxlat = (sublat > maxlat) ? sublat : maxlat;
    }

    for (uint64_t lpn = dst_start_lpn; lpn <= dst_end_lpn; lpn++) {
        bl = zonessd_l2p(ssd, lpn);
        page_type = (lpn / spp->tt_luns) % 4; // TO FIX: assume a zone spans all dies now

        // mode = 1, qlc
        if (page_type == 0)
          srd.cmd = NAND_QLC_PROG_L;
        else if (page_type == 1)
          srd.cmd = NAND_QLC_PROG_CL;
        else if (page_type == 2)
          srd.cmd = NAND_QLC_PROG_CU;
        else
          srd.cmd = NAND_QLC_PROG_U;
        srd.stime = req->stime;

        // indicate we can flush a stripe
        if (bl->lun == 7 && bl->ch == 7 && z->lpn_wp < lpn) { // TO FIX
          sublat = ssd_advance_stripe_latency(ssd, bl, &srd);
          
          // record the buffer persisted time
          z->buffer_persisted_time[z->buffer_cur_idx] = req->stime + sublat;
          femu_debug("record bpt id %u time %lu\n", z->buffer_cur_idx, 
                        z->buffer_persisted_time[z->buffer_cur_idx]);
          if (++z->buffer_cur_idx == 10) z->buffer_cur_idx = 0; // TO FIX

          maxlat = (sublat > maxlat) ? sublat : maxlat;
        }
        z->lpn_wp = lpn;
    }

    // TO FIX: two stripe buffers now
    int buffer_num = 1, idx = z->buffer_cur_idx;
    for (int i = 0; i < buffer_num; i++) 
      idx = idx == 0? 9 : (idx - 1);
    if (req->stime < z->buffer_persisted_time[idx]) {
      req->reqlat = z->buffer_persisted_time[idx] - req->stime;
      req->expire_time = req->stime + req->reqlat;
    } else {
      req->reqlat = 0;
      req->expire_time = req->stime;
    }

    femu_debug("zonessd_copy: src [%lu, %lu] dst [%lu, %lu] maxlat %lu\n", 
             src_start_lpn, src_end_lpn, dst_start_lpn, dst_end_lpn, maxlat);

    return ;
}

void zonessd_migrate(struct zonessd *ssd, uint64_t slba, uint64_t wp) 
{
    struct zonessd_params *spp = &ssd->sp;
    uint64_t start_lpn = slba / spp->secs_per_pg;
    uint64_t end_lpn = wp / spp->secs_per_pg;
    uint64_t maxlat = 0, sublat;
    uint32_t zone_idx = zns_zone_idx(ssd, start_lpn);
    zone *z = &ssd->zone_array[zone_idx];
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    for (uint64_t lpn = start_lpn; lpn <= end_lpn; lpn++) {
        struct block_locate *bl = zonessd_l2p(ssd, lpn);
        int page_type = (lpn / spp->tt_luns) % 4; // TO FIX: assume a zone spans all dies now

        struct zone_cmd srd;
        srd.cmd = NAND_SLC_READ;
        srd.stime = 0;
        ssd_advance_latency(ssd, bl, &srd);
        
        if (page_type == 0)
          srd.cmd = NAND_QLC_PROG_L;
        else if (page_type == 1)
          srd.cmd = NAND_QLC_PROG_CL;
        else if (page_type == 2)
          srd.cmd = NAND_QLC_PROG_CU;
        else
          srd.cmd = NAND_QLC_PROG_U;
        srd.stime = 0;

        // indicate we can flush a stripe
        if (bl->lun == 7 && bl->ch == 7 && z->lpn_wp < lpn) { // TO FIX
          sublat = ssd_advance_stripe_latency(ssd, bl, &srd);
          
          // record the buffer persisted time
          z->buffer_persisted_time[z->buffer_cur_idx] = now + sublat;
          femu_debug("record bpt id %u time %lu\n", z->buffer_cur_idx, 
                        z->buffer_persisted_time[z->buffer_cur_idx]);
          if (++z->buffer_cur_idx == 10) z->buffer_cur_idx = 0; // TO FIX

          maxlat = (sublat > maxlat) ? sublat : maxlat;
        }
        z->lpn_wp = lpn;
    }
}

void zonessd_append(struct zonessd *ssd, NvmeRequest *req, int mode) {
    printf("Not support append now\n");
    return ;
}
