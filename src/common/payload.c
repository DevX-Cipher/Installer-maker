#include "payload.h"
#include <string.h>
#include <stdlib.h>
#include "lzma_decoder.h"

#define HEADER_SIZE 20

typedef struct { unsigned long long stub, cfg, pay; } Trailer;

static const unsigned char* g_data;
static size_t               g_len;
static int                  g_state;
static LZMAByteVector       g_vec;
static unsigned char*       g_raw;

static unsigned long long rd(const unsigned char* p, int bytes)
{
  unsigned long long v = 0;
  int i;
  for (i = 0; i < bytes; ++i) v |= (unsigned long long)p[i] << (8 * i);
  return v;
}

static HANDLE open_self(void)
{
  WCHAR self[MAX_PATH];
  GetModuleFileNameW(NULL, self, MAX_PATH);
  return CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
}

static int seek_read(HANDLE h, unsigned long long off, void* buf, unsigned long long n)
{
  LARGE_INTEGER li;
  unsigned char* p = (unsigned char*)buf;
  li.QuadPart = (LONGLONG)off;
  if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return 0;
  while (n) {
    DWORD want = n > (1u << 28) ? (1u << 28) : (DWORD)n, got = 0;
    if (!ReadFile(h, p, want, &got, NULL) || got != want) return 0;
    p += got; n -= got;
  }
  return 1;
}

static int read_trailer(HANDLE h, Trailer* t)
{
  LARGE_INTEGER sz;
  unsigned char f[TRAILER_SIZE];
  if (!GetFileSizeEx(h, &sz) || (unsigned long long)sz.QuadPart < TRAILER_SIZE) return 0;
  if (!seek_read(h, (unsigned long long)sz.QuadPart - TRAILER_SIZE, f, TRAILER_SIZE)) return 0;
  if (memcmp(f, TRAILER_MAGIC, 8) != 0) return 0;
  t->cfg = rd(f + 8, 8);
  t->pay = rd(f + 16, 8);
  if (t->cfg > (unsigned long long)sz.QuadPart || t->pay > (unsigned long long)sz.QuadPart) return 0;
  if (t->cfg + t->pay + TRAILER_SIZE > (unsigned long long)sz.QuadPart) return 0;
  t->stub = (unsigned long long)sz.QuadPart - TRAILER_SIZE - t->cfg - t->pay;
  return 1;
}

static const unsigned char* g_logo, * g_banner;
static size_t g_logo_len, g_banner_len;

const unsigned char* payload_logo(size_t* n) { *n = g_logo_len; return g_logo; }
const unsigned char* payload_banner(size_t* n) { *n = g_banner_len; return g_banner; }

int payload_load_config(AppConfig* out)
{
  HANDLE h = open_self();
  Trailer t;
  char* text;
  int ok = 0;

  if (h == INVALID_HANDLE_VALUE) return 0;
  if (read_trailer(h, &t) && t.cfg > 0 && t.cfg < (32ull << 20)) {
    text = (char*)malloc((size_t)t.cfg + 1);
    if (text) {
      int keep = 0;
      if (seek_read(h, t.stub, text, t.cfg)) {
        unsigned long long tl;
        text[t.cfg] = 0;
        ok = cfg_parse(out, text);
        tl = strlen(text) + 1;
        if (ok && out->logo_len + (unsigned long long)out->banner_len + tl == t.cfg) {
          if (out->logo_len) { g_logo = (const unsigned char*)text + tl; g_logo_len = out->logo_len; keep = 1; }
          if (out->banner_len) { g_banner = (const unsigned char*)text + tl + out->logo_len; g_banner_len = out->banner_len; keep = 1; }
        }
      }
      if (!keep) free(text);
    }
  }
  CloseHandle(h);
  return ok;
}

int payload_write_uninstaller(const WCHAR* dst)
{
  HANDLE h = open_self(), o;
  Trailer t;
  unsigned char* buf;
  unsigned char f[TRAILER_SIZE];
  unsigned long long n;
  DWORD wr;
  int ok = 0, i;

  if (h == INVALID_HANDLE_VALUE) return 0;
  if (!read_trailer(h, &t)) { CloseHandle(h); return 0; }
  n = t.stub + t.cfg;
  buf = (unsigned char*)malloc((size_t)n);
  if (buf && seek_read(h, 0, buf, n)) {
    memcpy(f, TRAILER_MAGIC, 8);
    for (i = 0; i < 8; ++i) f[8 + i] = (unsigned char)(t.cfg >> (8 * i));
    memset(f + 16, 0, 8);
    o = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (o != INVALID_HANDLE_VALUE) {
      ok = WriteFile(o, buf, (DWORD)n, &wr, NULL) && wr == (DWORD)n &&
        WriteFile(o, f, TRAILER_SIZE, &wr, NULL) && wr == TRAILER_SIZE;
      CloseHandle(o);
    }
  }
  free(buf);
  CloseHandle(h);
  return ok;
}

static int load_payload(void)
{
  HANDLE h;
  Trailer t;

  if (g_state) return g_state > 0;
  g_state = -1;

  h = open_self();
  if (h == INVALID_HANDLE_VALUE) return 0;
  if (!read_trailer(h, &t) || t.pay < 13) { CloseHandle(h); return 0; }
  g_raw = (unsigned char*)malloc((size_t)t.pay);
  if (!g_raw || !seek_read(h, t.stub + t.cfg, g_raw, t.pay)) {
    CloseHandle(h); free(g_raw); g_raw = NULL; return 0;
  }
  CloseHandle(h);

  if (memcmp(g_raw, "CDPK0001", 8) == 0) {
    g_data = g_raw; g_len = (size_t)t.pay;
  }
  else {
    memset(&g_vec, 0, sizeof(g_vec));
    if (lzma_decompress(g_raw, (size_t)t.pay, &g_vec, ((size_t)-1) / 2) != 0) {
      free(g_vec.data); memset(&g_vec, 0, sizeof(g_vec));
      return 0;
    }
    g_data = g_vec.data; g_len = g_vec.length;
  }
  if (g_len < HEADER_SIZE || memcmp(g_data, "CDPK0001", 8) != 0) return 0;
  g_state = 1;
  return 1;
}

unsigned long long payload_peek_total(void)
{
  HANDLE h = open_self();
  Trailer t;
  unsigned char hd[20];
  unsigned long long r = 0;

  if (h == INVALID_HANDLE_VALUE) return 0;
  if (read_trailer(h, &t) && t.pay >= 20 && seek_read(h, t.stub + t.cfg, hd, 20)) {
    if (memcmp(hd, "CDPK0001", 8) == 0) r = rd(hd + 12, 8);
    else {
      r = rd(hd + 5, 8);   /* uncompressed size from the LZMA header */
      if (r == ~0ULL) r = 0;
    }
  }
  CloseHandle(h);
  return r;
}

int payload_open(Payload* p)
{
  memset(p, 0, sizeof(*p));
  if (!load_payload()) return 0;

  p->base = g_data;
  p->size = g_len;
  p->count = (unsigned)rd(g_data + 8, 4);
  p->total = rd(g_data + 12, 8);
  p->off = HEADER_SIZE;
  return 1;
}

int payload_next(Payload* p, PayloadEntry* e)
{
  size_t len;
  unsigned long long sz;

  if (p->off == p->size) return 0;
  if (p->size - p->off < 2) return -1;
  len = (size_t)rd(p->base + p->off, 2);
  p->off += 2;
  if (len == 0 || len >= sizeof(e->path) || p->size - p->off < len + 8) return -1;

  memcpy(e->path, p->base + p->off, len);
  e->path[len] = 0;
  p->off += len;

  sz = rd(p->base + p->off, 8);
  p->off += 8;
  if (sz > p->size - p->off) return -1;

  e->size = sz;
  e->data = p->base + p->off;
  p->off += (size_t)sz;
  return 1;
}
