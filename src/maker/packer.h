#ifndef PACKER_H
#define PACKER_H

#include "common.h"

typedef struct {
  const WCHAR* icon;     
  const WCHAR* logo;     
  const WCHAR* banner;  
} PackAssets;

#define ASSET_MAX_BYTES (3u * 1024 * 1024)

unsigned char* pack_slurp(const WCHAR* path, size_t max, size_t* len);
int pack_icon_valid(const unsigned char* d, size_t n);

int pack_installer(const AppConfig* cfg, const WCHAR* folder, const WCHAR* out_path,
  const unsigned char* stub, size_t stub_len, const PackAssets* assets,
  ProgressCb cb, void* ud, WCHAR* err, size_t errcap);

int pack_scan(const WCHAR* folder, unsigned* files, unsigned long long* bytes);

#endif