![Badge](https://img.shields.io/badge/-Educational-Purpose-blue?style=for-the-badge&logo=academia) 

![Badge](https://img.shields.io/badge/Educational-Purpose-blue?style=for-the-badge&logo=academia) 
# SSTF I/O Scheduler for Linux

This project implements a parametrizable SSTF (Shortest Seek Time First) I/O scheduler as a Linux kernel module, along with a user-space C program (`sector_read`) to benchmark disk I/O performance.

It was developed for the Operating Systems Construction course at PUCRS, under Professor Sérgio Johann Filho

## Features

- Custom kernel module (`sstf-iosched`) for Linux 4.13.
- Adjustable parameters:
  - `queue_depth` (max requests before dispatch, e.g., 20–100),
  - `max_wait_ms` (max wait time in ms, e.g., 20–5000),
  - `debug` (enable kernel debug logs).
- User-space benchmark tool (`sector_read`) for random read/write operations.
- Includes scripts and Makefile for building and running in a QEMU + Buildroot environment.
- Academic report comparing SSTF and NOOP schedulers under varied workloads.

For a detailed explanation of the logic, and results, please refer to the 🔗[**report***](/report.pdf)
## How to Run

1. Copy files to `/modules/sstf` in your Linux build.
2. Build:
   ```
   make
   ```
3. Export Linux source (if using Buildroot):
   ```
   export LINUX_OVERRIDE_SRCDIR=`pwd`/../linux-4.13.9/
   make
   ```
4. Inside QEMU:
   ```
   modprobe sstf-iosched.ko queue_depth=20 max_wait_ms=3000 debug=1
   echo sstf > /sys/block/sdb/queue/scheduler
   ./bin/sector_read 512 2000000 250 50 5
   ```

## Authors

- Bruno Bavaresco Zaffari
- Emanuel Nogueira de Barros
- Thiago Müller de Oliveira

## License

This project is licensed under the GNU General Public License v2.0 (GPLv2).
