/* File-descriptor syscalls (implemented in sysfile.c). All pointer arguments
 * are USER pointers and are only accessed through usercopy.h. */
#pragma once
#include "common.h"
int sys_write(uint32_t fd, const void *ubuf, uint32_t len);
int sys_read(uint32_t fd, void *ubuf, uint32_t len);
int sys_open(const char *upath, uint32_t flags, uint32_t mode);
int sys_close(uint32_t fd);
int sys_stat(const char *upath, void *ustat);
int sys_fstat(uint32_t fd, void *ustat);
int sys_mkdir(const char *upath, uint32_t mode);
int sys_unlink(const char *upath);
int sys_rmdir(const char *upath);
int sys_lseek(uint32_t fd, int off, uint32_t whence);
int sys_readdir(uint32_t fd, void *udirent, uint32_t max);
