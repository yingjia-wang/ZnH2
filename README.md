## ZnH2: Augmenting ZNS-based Storage System with Host-Managed Heterogeneous Zones (ICCAD'24)

This repository contains the source code of our paper, incorporating two parts: FEMU and ZenFS.
Please refer to their original repos for detailed installation guides.
For FEMU, only the modified files (in hw/femu/) are uploaded and they are developed based on [ad786ad](https://github.com/MoatLab/FEMU/commit/ad786ad152e6113057799f2d3edc0ef3295423bf).

The new emulated device (i.e., HZNS SSD) supports different modes of heterogeneous flash. 
The user should indicate one with **hzns_mode** in the running script (e.g., run-zns.sh). 
The valid values are 0 (pure QLC), 1 (device-managed SLC/QLC), and 2 (host-managed SLC/QLC).
For example:
> -device femu,devsz_mb=131072,femu_mode=3,hzns_mode=0 \
