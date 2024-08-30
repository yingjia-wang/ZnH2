// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
// Copyright (c) 2019-present, Western Digital Corporation
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#if !defined(ROCKSDB_LITE) && !defined(OS_WIN)

#include "zbd_zenfs.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libzbd/zbd.h>
#include <linux/blkzoned.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "rocksdb/env.h"
#include "rocksdb/io_status.h"
#include "snapshot.h"
#include "zbdlib_zenfs.h"
#include "zonefs_zenfs.h"

#define KB (1024)
#define MB (1024 * KB)

/* Number of reserved zones for metadata
 * Two non-offline meta zones are needed to be able
 * to roll the metadata log safely. One extra
 * is allocated to cover for one zone going offline.
 */
#define ZENFS_META_ZONES (3)

/* Minimum of number of zones that makes sense */
#define ZENFS_MIN_ZONES (32)

namespace ROCKSDB_NAMESPACE {

Zone::Zone(ZonedBlockDevice *zbd, ZonedBlockDeviceBackend *zbd_be,
           std::unique_ptr<ZoneList> &zones, unsigned int idx)
    : zbd_(zbd),
      zbd_be_(zbd_be),
      busy_(false),
      start_(zbd_be->ZoneStart(zones, idx)),
      max_capacity_(zbd_be->ZoneMaxCapacity(zones, idx)),
      wp_(zbd_be->ZoneWp(zones, idx)) {
  lifetime_ = Env::WLTH_NOT_SET;
  used_capacity_ = 0;
  capacity_ = 0;
  // all zones are configured in SLC first
  mode_ = 0;
  available_capacity_ = max_capacity_ >> 2;
  if (zbd_be->ZoneIsWritable(zones, idx))
    capacity_ = available_capacity_ - (wp_ - start_);
}

bool Zone::IsUsed() { return (used_capacity_ > 0); }
uint64_t Zone::GetCapacityLeft() { return capacity_; }
bool Zone::IsFull() { return (capacity_ == 0); }
bool Zone::IsEmpty() { return (wp_ == start_); }
uint64_t Zone::GetZoneNr() { return start_ / zbd_->GetZoneSize(); }

void Zone::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"start\":" << start_ << ",";
  json_stream << "\"capacity\":" << capacity_ << ",";
  json_stream << "\"max_capacity\":" << max_capacity_ << ",";
  json_stream << "\"wp\":" << wp_ << ",";
  json_stream << "\"lifetime\":" << lifetime_ << ",";
  json_stream << "\"used_capacity\":" << used_capacity_;
  json_stream << "}";
}

IOStatus Zone::Reset() {
  bool offline;
  uint64_t max_capacity;

  assert(!IsUsed());
  assert(IsBusy());

  bool full = IsFull();

  IOStatus ios = zbd_be_->Reset(start_, &offline, &max_capacity);
  if (ios != IOStatus::OK()) return ios;

  if (offline) {
    capacity_ = 0;
  } else {
    max_capacity_ = max_capacity;
    available_capacity_ = max_capacity_ >> 2;
    capacity_ = available_capacity_;
  }
  wp_ = start_;
  lifetime_ = Env::WLTH_NOT_SET;
  // printf("reset zone %lu mode %d\n", GetZoneNr(), mode_);
  if (!full && mode_) {
    std::unique_lock<std::mutex> lk(zbd_->active_qlc_zone_mtx_);
    zbd_->active_qlc_io_zones_--;
    // printf("reset QLC zone %lu active %ld\n", GetZoneNr(), zbd_->active_qlc_io_zones_.load());
  }
  mode_ = 0;

  return IOStatus::OK();
}

IOStatus Zone::Finish() {
  assert(IsBusy());

  IOStatus ios = zbd_be_->Finish(start_);
  if (ios != IOStatus::OK()) return ios;

  capacity_ = 0;
  wp_ = start_ + zbd_->GetZoneSize();
  if (mode_) {
    std::unique_lock<std::mutex> lk(zbd_->active_qlc_zone_mtx_);
    zbd_->active_qlc_io_zones_--;
    // printf("finish QLC zone %lu active %ld\n", GetZoneNr(), zbd_->active_qlc_io_zones_.load());
  }

  return IOStatus::OK();
}

IOStatus Zone::ExplicitOpen(bool mode) {
  // (1) only opening a QLC zone calls this fuction
  // (2) a zone already in QLC can not be opened again
  assert(IsBusy());
  if (!mode) printf("only opening a QLC zone calls this function!\n");
  if (mode_) printf("a zone already in QLC can not be opened again!\n"); 

  IOStatus ios = zbd_be_->ExplicitOpen(start_ / zbd_be_->GetBlockSize(), mode);
  if (ios != IOStatus::OK()) return ios;

  mode_ = mode;
  capacity_ = available_capacity_ = max_capacity_;
  // printf("explicitly open zone %lu mode %d remain %lu ava %lu\n", GetZoneNr(), mode, capacity_, available_capacity_);

  return IOStatus::OK();
}

IOStatus Zone::Close() {
  assert(IsBusy());

  if (!(IsEmpty() || IsFull())) {
    IOStatus ios = zbd_be_->Close(start_);
    if (ios != IOStatus::OK()) return ios;
  } else {
    if (mode_) {
      std::unique_lock<std::mutex> lk(zbd_->active_qlc_zone_mtx_);
      zbd_->active_qlc_io_zones_--;
      // printf("close QLC zone %lu active %ld\n", GetZoneNr(), zbd_->active_qlc_io_zones_.load());
    }
  }

  return IOStatus::OK();
}

IOStatus Zone::Append(char *data, uint32_t size) {
  ZenFSMetricsLatencyGuard guard(zbd_->GetMetrics(), ZENFS_ZONE_WRITE_LATENCY,
                                 Env::Default());
  zbd_->GetMetrics()->ReportThroughput(ZENFS_ZONE_WRITE_THROUGHPUT, size);
  char *ptr = data;
  uint32_t left = size;
  int ret;
  // printf("Zone::Append %lu mode %d cap %lu size %u\n", GetZoneNr(), mode_, capacity_, size);

  if (capacity_ < size)
    return IOStatus::NoSpace("Not enough capacity for append");

  assert((size % zbd_->GetBlockSize()) == 0);

  while (left) {
    ret = zbd_be_->Write(ptr, left, wp_);
    if (ret < 0) {
      return IOStatus::IOError(strerror(errno));
    }

    ptr += ret;
    wp_ += ret;
    capacity_ -= ret;
    left -= ret;
    zbd_->AddBytesWritten(ret);
  }

  return IOStatus::OK();
}

inline IOStatus Zone::CheckRelease() {
  if (!Release()) {
    assert(false);
    return IOStatus::Corruption("Failed to unset busy flag of zone " +
                                std::to_string(GetZoneNr()));
  }

  return IOStatus::OK();
}

Zone *ZonedBlockDevice::GetIOZone(uint64_t offset) {
  for (const auto z : io_zones)
    if (z->start_ <= offset && offset < (z->start_ + zbd_be_->GetZoneSize()))
      return z;
  return nullptr;
}

ZonedBlockDevice::ZonedBlockDevice(std::string path, ZbdBackendType backend,
                                   std::shared_ptr<Logger> logger,
                                   std::shared_ptr<ZenFSMetrics> metrics)
    : logger_(logger), metrics_(metrics) {
  if (backend == ZbdBackendType::kBlockDev) {
    zbd_be_ = std::unique_ptr<ZbdlibBackend>(new ZbdlibBackend(path));
    Info(logger_, "New Zoned Block Device: %s", zbd_be_->GetFilename().c_str());
  } else if (backend == ZbdBackendType::kZoneFS) {
    zbd_be_ = std::unique_ptr<ZoneFsBackend>(new ZoneFsBackend(path));
    Info(logger_, "New zonefs backing: %s", zbd_be_->GetFilename().c_str());
  }
}

IOStatus ZonedBlockDevice::Open(bool readonly, bool exclusive) {
  std::unique_ptr<ZoneList> zone_rep;
  unsigned int max_nr_active_zones;
  unsigned int max_nr_open_zones;
  Status s;
  uint64_t i = 0;
  uint64_t m = 0;
  // Reserve one zone for metadata and another one for extent migration
  int reserved_zones = 2;

  if (!readonly && !exclusive)
    return IOStatus::InvalidArgument("Write opens must be exclusive");

  IOStatus ios = zbd_be_->Open(readonly, exclusive, &max_nr_active_zones,
                               &max_nr_open_zones);
  if (ios != IOStatus::OK()) return ios;

  if (zbd_be_->GetNrZones() < ZENFS_MIN_ZONES) {
    return IOStatus::NotSupported("To few zones on zoned backend (" +
                                  std::to_string(ZENFS_MIN_ZONES) +
                                  " required)");
  }

  if (max_nr_active_zones == 0)
    max_nr_active_io_zones_ = zbd_be_->GetNrZones();
  else
    max_nr_active_io_zones_ = max_nr_active_zones - reserved_zones;

  if (max_nr_open_zones == 0)
    max_nr_open_io_zones_ = zbd_be_->GetNrZones();
  else
    max_nr_open_io_zones_ = max_nr_open_zones - reserved_zones;

  Info(logger_, "Zone block device nr zones: %u max active: %u max open: %u \n",
       zbd_be_->GetNrZones(), max_nr_active_zones, max_nr_open_zones);

  zone_rep = zbd_be_->ListZones();
  if (zone_rep == nullptr || zone_rep->ZoneCount() != zbd_be_->GetNrZones()) {
    Error(logger_, "Failed to list zones");
    return IOStatus::IOError("Failed to list zones");
  }

  while (m < ZENFS_META_ZONES && i < zone_rep->ZoneCount()) {
    /* Only use sequential write required zones */
    if (zbd_be_->ZoneIsSwr(zone_rep, i)) {
      if (!zbd_be_->ZoneIsOffline(zone_rep, i)) {
        meta_zones.push_back(new Zone(this, zbd_be_.get(), zone_rep, i));
      }
      m++;
    }
    i++;
  }

  active_io_zones_ = 0;
  open_io_zones_ = 0;
  active_qlc_io_zones_ = 0;

  for (; i < zone_rep->ZoneCount(); i++) {
    /* Only use sequential write required zones */
    if (zbd_be_->ZoneIsSwr(zone_rep, i)) {
      if (!zbd_be_->ZoneIsOffline(zone_rep, i)) {
        Zone *newZone = new Zone(this, zbd_be_.get(), zone_rep, i);
        if (!newZone->Acquire()) {
          assert(false);
          return IOStatus::Corruption("Failed to set busy flag of zone " +
                                      std::to_string(newZone->GetZoneNr()));
        }
        io_zones.push_back(newZone);
        if (zbd_be_->ZoneIsActive(zone_rep, i)) {
          active_io_zones_++;
          if (zbd_be_->ZoneIsOpen(zone_rep, i)) {
            if (!readonly) {
              newZone->Close();
            }
          }
        }
        IOStatus status = newZone->CheckRelease();
        if (!status.ok()) {
          return status;
        }
      }
    }
  }

  start_time_ = time(NULL);

  return IOStatus::OK();
}

uint64_t ZonedBlockDevice::GetFreeSpace() {
  uint64_t free = 0;
  for (const auto z : io_zones) {
    free += z->capacity_;
  }
  return free;
}

uint64_t ZonedBlockDevice::GetUsedSpace() {
  uint64_t used = 0;
  for (const auto z : io_zones) {
    used += z->used_capacity_;
  }
  return used;
}

uint64_t ZonedBlockDevice::GetReclaimableSpace() {
  uint64_t reclaimable = 0;
  for (const auto z : io_zones) {
    if (z->IsFull()) reclaimable += (z->available_capacity_ - z->used_capacity_);
  }
  return reclaimable;
}

void ZonedBlockDevice::LogZoneStats() {
  // TO FIX
  uint64_t used_capacity = 0;
  uint64_t reclaimable_capacity = 0;
  uint64_t reclaimables_max_capacity = 0;
  uint64_t active = 0;

  for (const auto z : io_zones) {
    used_capacity += z->used_capacity_;

    if (z->used_capacity_) {
      reclaimable_capacity += z->max_capacity_ - z->used_capacity_;
      reclaimables_max_capacity += z->max_capacity_;
    }

    if (!(z->IsFull() || z->IsEmpty())) active++;
  }

  if (reclaimables_max_capacity == 0) reclaimables_max_capacity = 1;

  Info(logger_,
       "[Zonestats:time(s),used_cap(MB),reclaimable_cap(MB), "
       "avg_reclaimable(%%), active(#), active_zones(#), open_zones(#)] %ld "
       "%lu %lu %lu %lu %ld %ld\n",
       time(NULL) - start_time_, used_capacity / MB, reclaimable_capacity / MB,
       100 * reclaimable_capacity / reclaimables_max_capacity, active,
       active_io_zones_.load(), open_io_zones_.load());
}

// for HZNS
uint64_t ZonedBlockDevice::GetUsedRatio() {
  uint64_t used = 0, used_slc = 0, total = 0, delta;

  for (const auto z : io_zones) {
    delta = z->available_capacity_ - z->capacity_;
    if (!z->mode_) { // SLC
      used += delta << 2;
      used_slc += delta << 2;
    } else { // QLC
      used += delta;
    }
    total += z->max_capacity_;
  }

  return ((100 * used_slc / total) << 32) | (100 * used / total);
}

// for HZNS
void ZonedBlockDevice::LogHybridZoneStats([[maybe_unused]] Zone *zone) {
  uint64_t valid = 0, used = 0, used_slc = 0, ava = 0, total = 0, delta;
  uint64_t active = 0, slc = 0, qlc = 0;

  for (const auto z : io_zones) {
    delta = z->available_capacity_ - z->capacity_;
    if (!z->mode_) { // SLC
      used += delta << 2;
      used_slc += delta << 2;
      slc++;
    } else { // QLC
      used += delta;
      qlc++;
      // printf("-- qlc zone %lu used %lu cap %lu\n", z->GetZoneNr(), 
      //        z->used_capacity_.load(), z->GetCapacityLeft());
    }
    valid += z->used_capacity_;
    ava += z->available_capacity_;
    total += z->max_capacity_;

    if (!(z->IsFull() || z->IsEmpty())) active++;
  }
  if (total == 0) total = 1;

  // here, 'used' also includes invalid data still remained in zones
  // printf("[time(s), valid(MB), ava(MB), used_r/slc_used_r(%%),"
  //       " active/active_z/open_z/active_qlc_z(#), slc/qlc(#)]"
  //       " %ld %lu %lu %lu/%lu %lu/%ld/%ld/%ld %lu/%lu\n", 
  //       time(NULL) - start_time_, valid / MB, ava / MB, 100 * used / total, 100 * used_slc / total, 
  //       active, active_io_zones_.load(), open_io_zones_.load(), active_qlc_io_zones_.load(), slc, qlc);

  // if (zone)
  //   printf("[now_z %lu mode %d used %lu cap %lu ava %lu]\n", zone->GetZoneNr(), zone->mode_, 
  //          zone->used_capacity_.load(), zone->GetCapacityLeft(), zone->available_capacity_);
}

void ZonedBlockDevice::LogZoneUsage() {
  for (const auto z : io_zones) {
    int64_t used = z->used_capacity_;

    if (used > 0) {
      Debug(logger_, "Zone 0x%lX used capacity: %ld bytes (%ld MB)\n",
            z->start_, used, used / MB);
    }
  }
}

void ZonedBlockDevice::LogGarbageInfo() {
  // Log zone garbage stats vector.
  //
  // The values in the vector represents how many zones with target garbage
  // percent. Garbage percent of each index: [0%, <10%, < 20%, ... <100%, 100%]
  // For example `[100, 1, 2, 3....]` means 100 zones are empty, 1 zone has less
  // than 10% garbage, 2 zones have  10% ~ 20% garbage ect.
  //
  // We don't need to lock io_zones since we only read data and we don't need
  // the result to be precise.
  int zone_gc_stat[12] = {0};
  for (auto z : io_zones) {
    if (!z->Acquire()) {
      continue;
    }

    if (z->IsEmpty()) {
      zone_gc_stat[0]++;
      z->Release();
      continue;
    }

    // TO FIX
    double garbage_rate = 0;
    if (z->IsFull()) {
      garbage_rate =
          double(z->max_capacity_ - z->used_capacity_) / z->max_capacity_;
    } else {
      garbage_rate =
          double(z->wp_ - z->start_ - z->used_capacity_) / z->max_capacity_;
    }
    assert(garbage_rate >= 0);
    int idx = int((garbage_rate + 0.1) * 10);
    zone_gc_stat[idx]++;

    z->Release();
  }

  std::stringstream ss;
  ss << "Zone Garbage Stats: [";
  for (int i = 0; i < 12; i++) {
    ss << zone_gc_stat[i] << " ";
  }
  ss << "]";
  Info(logger_, "%s", ss.str().data());
}

ZonedBlockDevice::~ZonedBlockDevice() {
  for (const auto z : meta_zones) {
    delete z;
  }

  for (const auto z : io_zones) {
    delete z;
  }
}

#define LIFETIME_DIFF_NOT_GOOD (100)
#define LIFETIME_DIFF_COULD_BE_WORSE (50)

unsigned int GetLifeTimeDiff(Env::WriteLifeTimeHint zone_lifetime,
                             Env::WriteLifeTimeHint file_lifetime) {
  assert(file_lifetime <= Env::WLTH_EXTREME);

  if ((file_lifetime == Env::WLTH_NOT_SET) ||
      (file_lifetime == Env::WLTH_NONE)) {
    if (file_lifetime == zone_lifetime) {
      return 0;
    } else {
      return LIFETIME_DIFF_NOT_GOOD;
    }
  }

  if (zone_lifetime > file_lifetime) return zone_lifetime - file_lifetime;
  if (zone_lifetime == file_lifetime) return LIFETIME_DIFF_COULD_BE_WORSE;

  return LIFETIME_DIFF_NOT_GOOD;
}

IOStatus ZonedBlockDevice::AllocateMetaZone(Zone **out_meta_zone) {
  assert(out_meta_zone);
  *out_meta_zone = nullptr;
  ZenFSMetricsLatencyGuard guard(metrics_, ZENFS_META_ALLOC_LATENCY,
                                 Env::Default());
  metrics_->ReportQPS(ZENFS_META_ALLOC_QPS, 1);

  for (const auto z : meta_zones) {
    /* If the zone is not used, reset and use it */
    if (z->Acquire()) {
      if (!z->IsUsed()) {
        if (!z->IsEmpty() && !z->Reset().ok()) {
          Warn(logger_, "Failed resetting zone!");
          IOStatus status = z->CheckRelease();
          if (!status.ok()) return status;
          continue;
        }
        *out_meta_zone = z;
        return IOStatus::OK();
      }
    }
  }
  assert(true);
  Error(logger_, "Out of metadata zones, we should go to read only now.");
  return IOStatus::NoSpace("Out of metadata zones");
}

IOStatus ZonedBlockDevice::ResetUnusedIOZones() {
  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (!z->IsEmpty() && !z->IsUsed()) {
        bool full = z->IsFull();
        IOStatus reset_status = z->Reset();
        // printf("ResetUnusedIOZones %lu\n", z->GetZoneNr());
        IOStatus release_status = z->CheckRelease();
        if (!reset_status.ok()) return reset_status;
        if (!release_status.ok()) return release_status;
        if (!full) PutActiveIOZoneToken();
      } else {
        IOStatus release_status = z->CheckRelease();
        if (!release_status.ok()) return release_status;
      }
    }
  }
  return IOStatus::OK();
}

void ZonedBlockDevice::WaitForOpenIOZoneToken(bool prioritized) {
  long allocator_open_limit;

  /* Avoid non-priortized allocators from starving prioritized ones */
  if (prioritized) {
    allocator_open_limit = max_nr_open_io_zones_;
  } else {
    allocator_open_limit = max_nr_open_io_zones_ - 1;
  }

  /* Wait for an open IO Zone token - after this function returns
   * the caller is allowed to write to a closed zone. The callee
   * is responsible for calling a PutOpenIOZoneToken to return the resource
   */
  std::unique_lock<std::mutex> lk(zone_resources_mtx_);
  zone_resources_.wait(lk, [this, allocator_open_limit] {
    if (open_io_zones_.load() < allocator_open_limit) {
      open_io_zones_++;
      return true;
    } else {
      return false;
    }
  });
}

bool ZonedBlockDevice::GetActiveIOZoneTokenIfAvailable() {
  /* Grap an active IO Zone token if available - after this function returns
   * the caller is allowed to write to a closed zone. The callee
   * is responsible for calling a PutActiveIOZoneToken to return the resource
   */
  std::unique_lock<std::mutex> lk(zone_resources_mtx_);
  if (active_io_zones_.load() < max_nr_active_io_zones_) {
    active_io_zones_++;
    return true;
  }
  return false;
}

void ZonedBlockDevice::PutOpenIOZoneToken() {
  {
    std::unique_lock<std::mutex> lk(zone_resources_mtx_);
    open_io_zones_--;
  }
  zone_resources_.notify_one();
}

void ZonedBlockDevice::PutActiveIOZoneToken() {
  {
    std::unique_lock<std::mutex> lk(zone_resources_mtx_);
    active_io_zones_--;
  }
  zone_resources_.notify_one();
}

IOStatus ZonedBlockDevice::ApplyFinishThreshold() {
  IOStatus s;

  if (finish_threshold_ == 0) return IOStatus::OK();

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      bool within_finish_threshold =
          z->capacity_ < (z->max_capacity_ * finish_threshold_ / 100);
      if (!(z->IsEmpty() || z->IsFull()) && within_finish_threshold) {
        /* If there is less than finish_threshold_% remaining capacity in a
         * non-open-zone, finish the zone */
        s = z->Finish();
        if (!s.ok()) {
          z->Release();
          Debug(logger_, "Failed finishing zone");
          return s;
        }
        s = z->CheckRelease();
        if (!s.ok()) return s;
        PutActiveIOZoneToken();
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::FinishCheapestIOZone() {
  IOStatus s;
  Zone *finish_victim = nullptr;

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (z->IsEmpty() || z->IsFull()) {
        s = z->CheckRelease();
        if (!s.ok()) return s;
        continue;
      }
      if (finish_victim == nullptr) {
        finish_victim = z;
        continue;
      }
      if (finish_victim->capacity_ > z->capacity_) {
        s = finish_victim->CheckRelease();
        if (!s.ok()) return s;
        finish_victim = z;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  // If all non-busy zones are empty or full, we should return success.
  if (finish_victim == nullptr) {
    Info(logger_, "All non-busy zones are empty or full, skip.");
    return IOStatus::OK();
  }

  // printf("FinishCheapestIOZone %lu cap %lu\n", finish_victim->GetZoneNr(), finish_victim->GetCapacityLeft());
  s = finish_victim->Finish();
  IOStatus release_status = finish_victim->CheckRelease();

  if (s.ok()) {
    PutActiveIOZoneToken();
  }

  if (!release_status.ok()) {
    return release_status;
  }

  return s;
}

IOStatus ZonedBlockDevice::GetBestOpenZoneMatch(
    Env::WriteLifeTimeHint file_lifetime, unsigned int *best_diff_out,
    Zone **zone_out, uint32_t min_capacity, bool qlc) {
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  Zone *allocated_zone = nullptr;
  IOStatus s;

  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if ((z->used_capacity_ > 0) && !z->IsFull() &&
          z->capacity_ >= min_capacity && (z->mode_ || !qlc)) {
        unsigned int diff = GetLifeTimeDiff(z->lifetime_, file_lifetime);
        // if (qlc)
        //   printf("-- consider zone %lu zone_lt %d diff %u\n", z->GetZoneNr(), z->lifetime_, diff);
        if (diff <= best_diff) {
          // for zones with the same lifetime, we prefer those in SLC when space is not in shortage,
          // and prefer those in QLC when space is in shortage 
          if (diff == best_diff && !qlc) {
            if ((space_shortage_ && !z->mode_) || (!space_shortage_ && z->mode_)) {
              s = z->CheckRelease();
              if (!s.ok()) return s;
              continue;
            }
          }     

          if (allocated_zone != nullptr) {
            s = allocated_zone->CheckRelease();
            if (!s.ok()) {
              IOStatus s_ = z->CheckRelease();
              if (!s_.ok()) return s_;
              return s;
            }
          }
          allocated_zone = z;
          best_diff = diff;
          // if (qlc)
          //   printf("-- allocated zone %lu bd %u\n", allocated_zone->GetZoneNr(), best_diff);
        } else {
          s = z->CheckRelease();
          if (!s.ok()) return s;
        }
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }

  *best_diff_out = best_diff;
  *zone_out = allocated_zone;

  // if (qlc && allocated_zone != nullptr)
  //   printf("get qlc zone %lu lt %d bd %u\n", allocated_zone->GetZoneNr(), allocated_zone->lifetime_, best_diff);

  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::AllocateEmptyZone(Zone **zone_out) {
  IOStatus s;
  Zone *allocated_zone = nullptr;
  for (const auto z : io_zones) {
    if (z->Acquire()) {
      if (z->IsEmpty()) {
        allocated_zone = z;
        break;
      } else {
        s = z->CheckRelease();
        if (!s.ok()) return s;
      }
    }
  }
  *zone_out = allocated_zone;
  // printf("AllocateEmptyZone %lu mode %d cap %lu\n", allocated_zone->GetZoneNr(), allocated_zone->mode_, allocated_zone->max_capacity_);
  return IOStatus::OK();
}

IOStatus ZonedBlockDevice::InvalidateCache(uint64_t pos, uint64_t size) {
  int ret = zbd_be_->InvalidateCache(pos, size);

  if (ret) {
    return IOStatus::IOError("Failed to invalidate cache");
  }
  return IOStatus::OK();
}

int ZonedBlockDevice::Read(char *buf, uint64_t offset, int n, bool direct) {
  int ret = 0;
  int left = n;
  int r = -1;

  while (left) {
    r = zbd_be_->Read(buf, left, offset, direct);
    if (r <= 0) {
      if (r == -1 && errno == EINTR) {
        continue;
      }
      break;
    }
    ret += r;
    buf += r;
    left -= r;
    offset += r;
  }

  if (r < 0) return r;
  return ret;
}

IOStatus ZonedBlockDevice::Copy(uint64_t dst, uint64_t src, uint64_t len) {

  return zbd_be_->Copy(dst, src, len);
}

IOStatus ZonedBlockDevice::ReleaseMigrateZone(Zone *zone) {
  IOStatus s = IOStatus::OK();
  {
    std::unique_lock<std::mutex> lock(migrate_zone_mtx_);
    migrating_ = false;
    if (zone != nullptr) {
      s = zone->CheckRelease();
      Info(logger_, "ReleaseMigrateZone: %lu", zone->start_);
    }
  }
  migrate_resource_.notify_one();
  return s;
}

IOStatus ZonedBlockDevice::TakeMigrateZone(Zone **out_zone,
                                           Env::WriteLifeTimeHint file_lifetime,
                                           uint32_t min_capacity) {
  std::unique_lock<std::mutex> lock(migrate_zone_mtx_);
  migrate_resource_.wait(lock, [this] { return !migrating_; });

  migrating_ = true;

  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  auto s =
      GetBestOpenZoneMatch(file_lifetime, &best_diff, out_zone, min_capacity, true /* qlc */);
  if (s.ok() && (*out_zone) != nullptr) {
    Info(logger_, "TakeMigrateZone: %lu", (*out_zone)->start_);
  } else {
    migrating_ = false;
  }

  return s;
}

IOStatus ZonedBlockDevice::AllocateIOZone(Env::WriteLifeTimeHint file_lifetime,
                                          IOType io_type, Zone **out_zone) {
  Zone *allocated_zone = nullptr;
  unsigned int best_diff = LIFETIME_DIFF_NOT_GOOD;
  int new_zone = 0;
  IOStatus s;
  // printf("in AllocateIOZone, file_lifetime %d\n", file_lifetime);

  auto tag = ZENFS_WAL_IO_ALLOC_LATENCY;
  if (io_type != IOType::kWAL) {
    // L0 flushes have lifetime MEDIUM
    if (file_lifetime == Env::WLTH_MEDIUM) {
      tag = ZENFS_L0_IO_ALLOC_LATENCY;
    } else {
      tag = ZENFS_NON_WAL_IO_ALLOC_LATENCY;
    }
  }

  ZenFSMetricsLatencyGuard guard(metrics_, tag, Env::Default());
  metrics_->ReportQPS(ZENFS_IO_ALLOC_QPS, 1);

  // Check if a deferred IO error was set
  s = GetZoneDeferredStatus();
  if (!s.ok()) {
    return s;
  }

  if (io_type != IOType::kWAL) {
    s = ApplyFinishThreshold();
    if (!s.ok()) {
      return s;
    }
  }

  WaitForOpenIOZoneToken(io_type == IOType::kWAL);

  /* Try to fill an already open zone(with the best life time diff) */
  s = GetBestOpenZoneMatch(file_lifetime, &best_diff, &allocated_zone);
  if (!s.ok()) {
    PutOpenIOZoneToken();
    return s;
  }

  // Holding allocated_zone if != nullptr

  if (best_diff >= LIFETIME_DIFF_COULD_BE_WORSE) {
    bool got_token = GetActiveIOZoneTokenIfAvailable();

    /* If we did not get a token, try to use the best match, even if the life
     * time diff not good but a better choice than to finish an existing zone
     * and open a new one
     */
    if (allocated_zone != nullptr) {
      if (!got_token && best_diff == LIFETIME_DIFF_COULD_BE_WORSE) {
        Debug(logger_,
              "Allocator: avoided a finish by relaxing lifetime diff "
              "requirement\n");
      } else {
        s = allocated_zone->CheckRelease();
        if (!s.ok()) {
          PutOpenIOZoneToken();
          if (got_token) PutActiveIOZoneToken();
          return s;
        }
        allocated_zone = nullptr;
      }
    }

    /* If we haven't found an open zone to fill, open a new zone */
    if (allocated_zone == nullptr) {
      /* We have to make sure we can open an empty zone */
      while (!got_token && !GetActiveIOZoneTokenIfAvailable()) {
        s = FinishCheapestIOZone();
        if (!s.ok()) {
          PutOpenIOZoneToken();
          return s;
        }
      }

      s = AllocateEmptyZone(&allocated_zone);
      if (!s.ok()) {
        PutActiveIOZoneToken();
        PutOpenIOZoneToken();
        return s;
      }

      if (allocated_zone != nullptr) {
        assert(allocated_zone->IsBusy());
        allocated_zone->lifetime_ = file_lifetime;
        new_zone = true;
        // Reaching here means the lifetime of the new file should be longer than existing zones,
        // so that ZenFS prefers not to further extend the lifetime of existing zones.

        // If space shortage is observed, the new zone is preferred to be opened in QLC mode;
        // however, when the number of QLC zones reaches the maximum, the new zone can only be opened in SLC mode. 
        if (space_shortage_) {   
          std::unique_lock<std::mutex> lk(active_qlc_zone_mtx_);     
          if (active_qlc_io_zones_.load() < 9) { // currently, set the maximum QLC zones to 9
            allocated_zone->ExplicitOpen(true);
            active_qlc_io_zones_++;
          }
          // printf("observe space shortage, allocate new zone %lu active_qlc %ld\n", 
          //         allocated_zone->GetZoneNr(), active_qlc_io_zones_.load());
        }
      } else {
        PutActiveIOZoneToken();
      }
    }
  }

  if (allocated_zone) {
    assert(allocated_zone->IsBusy());
    Debug(logger_,
          "Allocating zone(new=%d) start: 0x%lx wp: 0x%lx lt: %d file lt: %d\n",
          new_zone, allocated_zone->start_, allocated_zone->wp_,
          allocated_zone->lifetime_, file_lifetime);
  } else {
    PutOpenIOZoneToken();
  }

  if (io_type != IOType::kWAL) {
    LogZoneStats();
  }

  *out_zone = allocated_zone;
  // LogHybridZoneStats(allocated_zone);

  metrics_->ReportGeneral(ZENFS_OPEN_ZONES_COUNT, open_io_zones_);
  metrics_->ReportGeneral(ZENFS_ACTIVE_ZONES_COUNT, active_io_zones_);

  return IOStatus::OK();
}

std::string ZonedBlockDevice::GetFilename() { return zbd_be_->GetFilename(); }

uint32_t ZonedBlockDevice::GetBlockSize() { return zbd_be_->GetBlockSize(); }

uint64_t ZonedBlockDevice::GetZoneSize() { return zbd_be_->GetZoneSize(); }

uint32_t ZonedBlockDevice::GetNrZones() { return zbd_be_->GetNrZones(); }

void ZonedBlockDevice::EncodeJsonZone(std::ostream &json_stream,
                                      const std::vector<Zone *> zones) {
  bool first_element = true;
  json_stream << "[";
  for (Zone *zone : zones) {
    if (first_element) {
      first_element = false;
    } else {
      json_stream << ",";
    }
    zone->EncodeJson(json_stream);
  }

  json_stream << "]";
}

void ZonedBlockDevice::EncodeJson(std::ostream &json_stream) {
  json_stream << "{";
  json_stream << "\"meta\":";
  EncodeJsonZone(json_stream, meta_zones);
  json_stream << ",\"io\":";
  EncodeJsonZone(json_stream, io_zones);
  json_stream << "}";
}

IOStatus ZonedBlockDevice::GetZoneDeferredStatus() {
  std::lock_guard<std::mutex> lock(zone_deferred_status_mutex_);
  return zone_deferred_status_;
}

void ZonedBlockDevice::SetZoneDeferredStatus(IOStatus status) {
  std::lock_guard<std::mutex> lk(zone_deferred_status_mutex_);
  if (!zone_deferred_status_.ok()) {
    zone_deferred_status_ = status;
  }
}

void ZonedBlockDevice::GetZoneSnapshot(std::vector<ZoneSnapshot> &snapshot) {
  for (auto *zone : io_zones) {
    snapshot.emplace_back(*zone);
  }
}

}  // namespace ROCKSDB_NAMESPACE

#endif  // !defined(ROCKSDB_LITE) && !defined(OS_WIN)
