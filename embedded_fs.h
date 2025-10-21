#pragma once
#include "common.h"

void embedded_fs_init(void);
void embedded_fs_list(void);
void embedded_fs_cat(const char *filename);
int embedded_fs_create(const char *filename);
int embedded_fs_write(const char *filename, const char *content, size_t len);
int embedded_fs_delete(const char *filename);

