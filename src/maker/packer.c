#include "packer.h"
#include "payload.h"
#include "appicon.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define R_GetTickCount GetTickCount
#include "lzma_encoder.h"
#include "lzma_decoder.h"

typedef struct { char rel[512]; WCHAR* full; uint64_t size; } Ent;

typedef struct {
  Ent* v;
  size_t n, cap;
  uint64_t total;
  WCHAR skip[MAX_PATH];     
  WCHAR err[256];
} Scan;

static void set_err(WCHAR* err, size_t cap, const WCHAR* msg)
{
  if (err && cap) lstrcpynW(err, msg, (int)cap);
}

static int scan_add(Scan* s, const WCHAR* full, const char* rel, uint64_t size)
{
  Ent* e;
  if (s->n == s->cap) {
    size_t nc = s->cap ? s->cap * 2 : 128;
    Ent* nv = (Ent*)realloc(s->v, nc * sizeof(Ent));
    if (!nv) { lstrcpyW(s->err, L"Out of memory."); return 0; }
    s->v = nv; s->cap = nc;
  }
  e = &s->v[s->n];
  e->full = _wcsdup(full);
  if (!e->full) { lstrcpyW(s->err, L"Out of memory."); return 0; }
  strcpy(e->rel, rel);
  e->size = size;
  ++s->n;
  s->total += size;
  return 1;
}

static int walk(Scan* s, const WCHAR* dir, const char* rel)
{
  WCHAR pat[MAX_PATH + 4];
  WIN32_FIND_DATAW fd;
  HANDLE h;

  if (lstrlenW(dir) + 3 >= MAX_PATH) { lstrcpyW(s->err, L"A folder path is too long."); return 0; }
  path_join(pat, MAX_PATH, dir, L"*");
  h = FindFirstFileW(pat, &fd);
  if (h == INVALID_HANDLE_VALUE) return 1;
  do {
    WCHAR full[MAX_PATH];
    char name[1024], nrel[512];
    if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
    if (lstrlenW(dir) + 1 + lstrlenW(fd.cFileName) >= MAX_PATH) {
      lstrcpyW(s->err, L"A file path is too long."); FindClose(h); return 0;
    }
    path_join(full, MAX_PATH, dir, fd.cFileName);
    if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL)) continue;
    if (snprintf(nrel, sizeof(nrel), "%s%s%s", rel, rel[0] ? "/" : "", name) >= (int)sizeof(nrel)) {
      lstrcpyW(s->err, L"A file path is too long to pack."); FindClose(h); return 0;
    }
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
      if (!walk(s, full, nrel)) { FindClose(h); return 0; }
    }
    else {
      uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
      if (!rel[0] && (!lstrcmpiW(fd.cFileName, UNINSTALL_EXE) || !lstrcmpiW(fd.cFileName, LIST_FILE))) continue;
      if (s->skip[0] && !lstrcmpiW(full, s->skip)) continue;
      if (!scan_add(s, full, nrel, sz)) { FindClose(h); return 0; }
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return 1;
}

static void scan_free(Scan* s)
{
  size_t i;
  for (i = 0; i < s->n; ++i) free(s->v[i].full);
  free(s->v);
  memset(s, 0, sizeof(*s));
}

static int cmp_ent(const void* a, const void* b)
{
  return strcmp(((const Ent*)a)->rel, ((const Ent*)b)->rel);
}

int pack_scan(const WCHAR* folder, unsigned* files, unsigned long long* bytes)
{
  Scan s;
  int ok;
  memset(&s, 0, sizeof(s));
  ok = walk(&s, folder, "");
  *files = (unsigned)s.n;
  *bytes = s.total;
  scan_free(&s);
  return ok;
}

static void put_le(unsigned char* p, uint64_t v, int bytes)
{
  int i;
  for (i = 0; i < bytes; ++i) p[i] = (unsigned char)(v >> (8 * i));
}

static int read_file(const WCHAR* path, unsigned char* dst, uint64_t size)
{
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  int ok = 1;
  if (h == INVALID_HANDLE_VALUE) return 0;
  while (size) {
    DWORD want = size > (1u << 24) ? (1u << 24) : (DWORD)size, got = 0;
    if (!ReadFile(h, dst, want, &got, NULL) || got != want) { ok = 0; break; }
    dst += got; size -= got;
  }
  CloseHandle(h);
  return ok;
}

typedef struct { ProgressCb cb; void* ud; } PackProg;

static void lzma_prog(size_t done, size_t total, void* p)
{
  PackProg* pp = (PackProg*)p;
  if (pp->cb) pp->cb(pp->ud, 30 + (total ? (int)((unsigned long long)done * 65 / total) : 65), NULL, 0);
}

static int write_all(HANDLE h, const void* p, size_t n)
{
  const unsigned char* b = (const unsigned char*)p;
  while (n) {
    DWORD want = n > (1u << 24) ? (1u << 24) : (DWORD)n, wr = 0;
    if (!WriteFile(h, b, want, &wr, NULL) || wr != want) return 0;
    b += wr; n -= wr;
  }
  return 1;
}

static int read_file_h(HANDLE h, unsigned char* dst, size_t size)
{
  while (size) {
    DWORD want = size > (1u << 24) ? (1u << 24) : (DWORD)size, got = 0;
    if (!ReadFile(h, dst, want, &got, NULL) || got != want) return 0;
    dst += got; size -= got;
  }
  return 1;
}

unsigned char* pack_slurp(const WCHAR* path, size_t max, size_t* len)
{
  HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  LARGE_INTEGER sz;
  unsigned char* b = NULL;
  if (h == INVALID_HANDLE_VALUE) return NULL;
  if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && (unsigned long long)sz.QuadPart <= max) {
    b = (unsigned char*)malloc((size_t)sz.QuadPart);
    if (b && !read_file_h(h, b, (size_t)sz.QuadPart)) { free(b); b = NULL; }
    else if (b) *len = (size_t)sz.QuadPart;
  }
  CloseHandle(h);
  return b;
}

#pragma pack(push, 1)
typedef struct { WORD reserved, type, count; } IcoDir;
typedef struct { BYTE w, h, colors, res; WORD planes, bpp; DWORD size, offset; } IcoEntry;
typedef struct { BYTE w, h, colors, res; WORD planes, bpp; DWORD size; WORD id; } GrpEntry;
#pragma pack(pop)

int pack_icon_valid(const unsigned char* d, size_t n)
{
  const IcoDir* dir;
  const IcoEntry* e;
  size_t i;
  if (n < sizeof(IcoDir)) return 0;
  dir = (const IcoDir*)d;
  if (dir->reserved != 0 || dir->type != 1 || dir->count == 0 || dir->count > 64) return 0;
  if (n < sizeof(IcoDir) + sizeof(IcoEntry) * dir->count) return 0;
  e = (const IcoEntry*)(d + sizeof(IcoDir));
  for (i = 0; i < dir->count; ++i)
    if (e[i].size == 0 || (size_t)e[i].offset > n || (size_t)e[i].size > n - e[i].offset) return 0;
  return 1;
}

static int apply_icon(const unsigned char* stub, size_t stub_len, const unsigned char* ico, size_t ico_len,
  unsigned char** out, size_t* out_len)
{
  WCHAR dir[MAX_PATH], tmp[MAX_PATH];
  const IcoDir* d = (const IcoDir*)ico;
  const IcoEntry* e = (const IcoEntry*)(ico + sizeof(IcoDir));
  unsigned char* grp;
  size_t gl = sizeof(IcoDir) + sizeof(GrpEntry) * d->count, i;
  HANDLE f, ur;
  int ok = 0;

  if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"imk", 0, tmp)) return 0;
  f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
  if (f == INVALID_HANDLE_VALUE) { DeleteFileW(tmp); return 0; }
  ok = write_all(f, stub, stub_len);
  CloseHandle(f);
  if (!ok) { DeleteFileW(tmp); return 0; }
  ok = 0;

  grp = (unsigned char*)calloc(1, gl);
  ur = BeginUpdateResourceW(tmp, FALSE);
  if (grp && ur) {
    GrpEntry* g = (GrpEntry*)(grp + sizeof(IcoDir));
    int good = 1;
    memcpy(grp, d, sizeof(IcoDir));
    for (i = 0; i < d->count && good; ++i) {
      memcpy(&g[i], &e[i], 12);
      g[i].id = (WORD)(i + 1);
      good = UpdateResourceW(ur, RT_ICON, MAKEINTRESOURCEW(i + 1), 0, (LPVOID)(ico + e[i].offset), e[i].size);
    }
    if (good) good = UpdateResourceW(ur, RT_GROUP_ICON, MAKEINTRESOURCEW(APP_ICON_ID), 0, grp, (DWORD)gl);
    ok = EndUpdateResourceW(ur, !good) && good;
  }
  else if (ur) EndUpdateResourceW(ur, TRUE);
  free(grp);

  if (ok) {
    *out = pack_slurp(tmp, 256u << 20, out_len);
    ok = *out != NULL;
  }
  DeleteFileW(tmp);
  return ok;
}

static int sha_hex_ok(const WCHAR* s)
{
  int i, n = lstrlenW(s);
  if (n == 0) return 1;
  if (n != 64) return 0;
  for (i = 0; i < n; ++i)
    if (!((s[i] >= L'0' && s[i] <= L'9') || (s[i] >= L'a' && s[i] <= L'f') || (s[i] >= L'A' && s[i] <= L'F')))
      return 0;
  return 1;
}

static int link_cfg_ok(const AppConfig* c, WCHAR* err, size_t cap)
{
  char u8[600];
  if (_wcsnicmp(c->url, L"https://", 8) != 0 || lstrlenW(c->url) < 12 || wcschr(c->url, L' ')) {
    set_err(err, cap, L"The download link must start with https://");
    return 0;
  }
  if (!c->exe[0] || !WideCharToMultiByte(CP_UTF8, 0, c->exe, -1, u8, sizeof(u8), NULL, NULL) ||
    !rel_path_is_safe(u8)) {
    set_err(err, cap, L"The main program must be a relative path.");
    return 0;
  }
  if (!sha_hex_ok(c->sha256)) {
    set_err(err, cap, L"The checksum must be 64 hex characters.");
    return 0;
  }
  return 1;
}

int pack_installer(const AppConfig* cfg_in, const WCHAR* folder, const WCHAR* out_path,
  const unsigned char* stub, size_t stub_len, const PackAssets* assets,
  ProgressCb cb, void* ud, WCHAR* err, size_t errcap)
{
  Scan s;
  AppConfig cfgv;
  const AppConfig* cfg = &cfgv;
  char exe_rel[300], cfgtxt[4096];
  unsigned char* raw = NULL, * packed = NULL, f[TRAILER_SIZE];
  unsigned char* logo = NULL, * banner = NULL, * ico = NULL, * stub2 = NULL, * blob = NULL;
  size_t i, raw_len, csz = 0, pos, logo_n = 0, banner_n = 0, ico_n = 0, blob_n = 0;
  int cfg_len, ok = 0;
  LZMAByteVector chk;
  HANDLE ho;
  PackProg pp;
  WCHAR tmp[300];

  cfgv = *cfg_in;
  memset(&s, 0, sizeof(s));
  memset(&chk, 0, sizeof(chk));
  pp.cb = cb; pp.ud = ud;
  lstrcpynW(s.skip, out_path, MAX_PATH);

  if (assets && assets->icon && assets->icon[0]) {
    ico = pack_slurp(assets->icon, 1u << 20, &ico_n);
    if (!ico || !pack_icon_valid(ico, ico_n)) { set_err(err, errcap, L"The icon is not a valid .ico file."); goto done; }
  }
  if (assets && assets->logo && assets->logo[0]) {
    logo = pack_slurp(assets->logo, ASSET_MAX_BYTES, &logo_n);
    if (!logo) { set_err(err, errcap, L"The logo image could not be read (max 3 MB)."); goto done; }
    cfgv.logo_len = (unsigned)logo_n;
  }
  if (assets && assets->banner && assets->banner[0]) {
    banner = pack_slurp(assets->banner, ASSET_MAX_BYTES, &banner_n);
    if (!banner) { set_err(err, errcap, L"The banner image could not be read (max 3 MB)."); goto done; }
    cfgv.banner_len = (unsigned)banner_n;
  }
  if (ico) {
    if (cb) cb(ud, 0, L"Applying icon...", 0);
    if (!apply_icon(stub, stub_len, ico, ico_n, &stub2, &stub_len)) {
      set_err(err, errcap, L"Could not apply the icon to the installer.");
      goto done;
    }
    stub = stub2;
  }
  if (cfg->source_link) {
    if (cb) cb(ud, 20, L"Checking download details...", 0);
    if (!link_cfg_ok(cfg, err, errcap)) goto done;
    if (cb) cb(ud, 90, L"Download details checked", 1);
  }
  else {
    if (cb) cb(ud, 0, L"Scanning files...", 0);
    if (!walk(&s, folder, "")) { set_err(err, errcap, s.err[0] ? s.err : L"Could not read the source folder."); goto done; }
  if (s.n == 0) { set_err(err, errcap, L"The source folder has no files to pack."); goto done; }
  if (s.total > 1500ull * 1024 * 1024) { set_err(err, errcap, L"The source folder is larger than 1.5 GB."); goto done; }

  if (!WideCharToMultiByte(CP_UTF8, 0, cfg->exe, -1, exe_rel, sizeof(exe_rel), NULL, NULL)) {
    set_err(err, errcap, L"The main executable path is invalid."); goto done;
  }
  for (i = 0; exe_rel[i]; ++i) if (exe_rel[i] == '\\') exe_rel[i] = '/';
  qsort(s.v, s.n, sizeof(Ent), cmp_ent);
  for (i = 0; i < s.n; ++i) if (!_stricmp(s.v[i].rel, exe_rel)) break;
  if (i == s.n) {
    wsprintfW(tmp, L"%s was not found in the source folder.", cfg->exe);
    set_err(err, errcap, tmp);
    goto done;
  }

  raw_len = 20;
  for (i = 0; i < s.n; ++i) raw_len += 2 + strlen(s.v[i].rel) + 8 + (size_t)s.v[i].size;
  raw = (unsigned char*)malloc(raw_len);
  if (!raw) { set_err(err, errcap, L"Not enough memory to pack these files."); goto done; }

  memcpy(raw, "CDPK0001", 8);
  put_le(raw + 8, s.n, 4);
  put_le(raw + 12, s.total, 8);
  pos = 20;
  for (i = 0; i < s.n; ++i) {
    Ent* e = &s.v[i];
    size_t rl = strlen(e->rel);
    {
      WCHAR wr[260];
      if (!MultiByteToWideChar(CP_UTF8, 0, e->rel, -1, wr, 200)) wr[0] = 0;
      wr[199] = 0;
      wsprintfW(tmp, L"Reading %s", wr);
    }
    if (cb) cb(ud, (int)(i * 30 / s.n), tmp, 0);
    put_le(raw + pos, rl, 2); pos += 2;
    memcpy(raw + pos, e->rel, rl); pos += rl;
    put_le(raw + pos, e->size, 8); pos += 8;
    if (!read_file(e->full, raw + pos, e->size)) {
      wsprintfW(tmp, L"Could not read %s.", e->full);
      set_err(err, errcap, tmp);
      goto done;
    }
    pos += (size_t)e->size;
  }
  if (cb) cb(ud, 30, L"Collected files", 1);

  if (cb) cb(ud, 30, L"Compressing...", 0);
  packed = lzma_compress(raw, raw_len, &csz, NULL, lzma_prog, &pp);
  if (!packed) { set_err(err, errcap, L"Compression failed."); goto done; }
  if (cb) cb(ud, 95, L"Verifying...", 0);
  if (lzma_decompress(packed, csz, &chk, raw_len) != 0 || chk.length != raw_len ||
    memcmp(chk.data, raw, raw_len) != 0) {
    set_err(err, errcap, L"Verification failed: compressed data does not match the source files.");
    goto done;
  }
  if (cb) cb(ud, 96, L"Compressed and verified", 1);
  }

  cfg_len = cfg_serialize(cfg, cfgtxt, sizeof(cfgtxt));
  if (cfg_len <= 0) { set_err(err, errcap, L"The installer details are too long."); goto done; }
  blob_n = (size_t)cfg_len + 1 + logo_n + banner_n;
  blob = (unsigned char*)calloc(1, blob_n);
  if (!blob) { set_err(err, errcap, L"Not enough memory to pack these files."); goto done; }
  memcpy(blob, cfgtxt, (size_t)cfg_len);
  if (logo_n) memcpy(blob + cfg_len + 1, logo, logo_n);
  if (banner_n) memcpy(blob + cfg_len + 1 + logo_n, banner, banner_n);

  if (cb) cb(ud, 97, L"Writing installer...", 0);
  ho = CreateFileW(out_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (ho == INVALID_HANDLE_VALUE) {
    set_err(err, errcap, L"Could not create the output file. Is it open, or the folder read-only?");
    goto done;
  }
  memcpy(f, TRAILER_MAGIC, 8);
  put_le(f + 8, (uint64_t)blob_n, 8);
  put_le(f + 16, (uint64_t)csz, 8);
  ok = write_all(ho, stub, stub_len) && write_all(ho, blob, blob_n) &&
    write_all(ho, packed, csz) && write_all(ho, f, TRAILER_SIZE);
  CloseHandle(ho);
  if (!ok) {
    DeleteFileW(out_path);
    set_err(err, errcap, L"Could not write the output file. Is the disk full?");
    goto done;
  }
  if (cb) cb(ud, 100, L"Installer created", 1);

done:
  free(raw); free(packed); free(chk.data);
  free(logo); free(banner); free(ico); free(stub2); free(blob);
  scan_free(&s);
  return ok;
}