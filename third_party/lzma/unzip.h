#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "common.h"

#define WSIZE     32768
#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define MAXCODES  (MAXLCODES + MAXDCODES)
#define FIXLCODES 288

typedef struct { short* count; short* symbol; } Huff;

typedef struct {
  const unsigned char* in;
  size_t inlen, incnt;
  int bitbuf, bitcnt;
  unsigned char win[WSIZE];
  unsigned long long outcnt;
  HANDLE h;
  unsigned crc;
  DWORD werr;
  void (*adv)(void*, unsigned);
  void* ctx;
  jmp_buf env;
} Inf;

typedef struct {
  ProgressCb cb;
  void* ud;
  unsigned long long done, total;
  int pct0, pct1, last;
} Prog;

static unsigned g_crc[256];

static void crc_init(void)
{
  unsigned n, c;
  int k;
  for (n = 0; n < 256; ++n) {
    c = n;
    for (k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    g_crc[n] = c;
  }
}

static unsigned crc_upd(unsigned crc, const unsigned char* p, size_t n)
{
  crc = ~crc;
  while (n--) crc = g_crc[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

static unsigned rd16(const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const unsigned char* p)
{
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void zerr(WCHAR* err, size_t cap, const WCHAR* msg)
{
  if (err && cap) lstrcpynW(err, msg, (int)cap);
}

static void prog_adv(void* c, unsigned n)
{
  Prog* p = (Prog*)c;
  int pct;
  p->done += n;
  pct = p->pct0 + (int)((unsigned long long)(p->pct1 - p->pct0) * p->done / (p->total ? p->total : 1));
  if (pct != p->last) { p->last = pct; if (p->cb) p->cb(p->ud, pct, NULL, 0); }
}

static int flush_win(Inf* s, unsigned n)
{
  DWORD wr = 0;
  if (!WriteFile(s->h, s->win, n, &wr, NULL) || wr != n) { s->werr = GetLastError(); return 0; }
  s->crc = crc_upd(s->crc, s->win, n);
  if (s->adv) s->adv(s->ctx, n);
  return 1;
}

static void outb(Inf* s, unsigned char b)
{
  s->win[(unsigned)(s->outcnt & (WSIZE - 1))] = b;
  ++s->outcnt;
  if ((s->outcnt & (WSIZE - 1)) == 0 && !flush_win(s, WSIZE)) longjmp(s->env, 2);
}

static int bits(Inf* s, int need)
{
  long val = s->bitbuf;
  while (s->bitcnt < need) {
    if (s->incnt == s->inlen) longjmp(s->env, 1);
    val |= (long)s->in[s->incnt++] << s->bitcnt;
    s->bitcnt += 8;
  }
  s->bitbuf = (int)(val >> need);
  s->bitcnt -= need;
  return (int)(val & ((1L << need) - 1));
}

static int decode(Inf* s, const Huff* h)
{
  int len, code, first, count, index, bitbuf, left;
  short* next;

  bitbuf = s->bitbuf;
  left = s->bitcnt;
  code = first = index = 0;
  len = 1;
  next = h->count + 1;
  for (;;) {
    while (left--) {
      code |= bitbuf & 1;
      bitbuf >>= 1;
      count = *next++;
      if (code - count < first) {
        s->bitbuf = bitbuf;
        s->bitcnt = (s->bitcnt - len) & 7;
        return h->symbol[index + (code - first)];
      }
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
      len++;
    }
    left = (MAXBITS + 1) - len;
    if (left == 0) break;
    if (s->incnt == s->inlen) longjmp(s->env, 1);
    bitbuf = s->in[s->incnt++];
    if (left > 8) left = 8;
  }
  return -10;
}

static int construct(Huff* h, const short* length, int n)
{
  int symbol, len, left;
  short offs[MAXBITS + 1];

  for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
  for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
  if (h->count[0] == n) return 0;
  left = 1;
  for (len = 1; len <= MAXBITS; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return left;
  }
  offs[1] = 0;
  for (len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
  for (symbol = 0; symbol < n; symbol++)
    if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
  return left;
}

static int codes(Inf* s, const Huff* lencode, const Huff* distcode)
{
  static const short lens[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
  static const short lext[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
  static const short dists[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
  static const short dext[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
  int symbol, len;
  unsigned dist;

  do {
    symbol = decode(s, lencode);
    if (symbol < 0) return symbol;
    if (symbol < 256) outb(s, (unsigned char)symbol);
    else if (symbol > 256) {
      symbol -= 257;
      if (symbol >= 29) return -10;
      len = lens[symbol] + bits(s, lext[symbol]);
      symbol = decode(s, distcode);
      if (symbol < 0) return symbol;
      if (symbol >= 30) return -12;
      dist = (unsigned)dists[symbol] + (unsigned)bits(s, dext[symbol]);
      if (dist > s->outcnt) return -11;
      while (len--) outb(s, s->win[(unsigned)((s->outcnt - dist) & (WSIZE - 1))]);
    }
  } while (symbol != 256);
  return 0;
}

static int stored_block(Inf* s)
{
  unsigned len;
  s->bitbuf = 0;
  s->bitcnt = 0;
  if (s->incnt + 4 > s->inlen) return 2;
  len = s->in[s->incnt++];
  len |= (unsigned)s->in[s->incnt++] << 8;
  if (s->in[s->incnt++] != (~len & 0xFF) || s->in[s->incnt++] != ((~len >> 8) & 0xFF)) return -2;
  if (s->incnt + len > s->inlen) return 2;
  while (len--) outb(s, s->in[s->incnt++]);
  return 0;
}

static int fixed_block(Inf* s)
{
  short lencnt[MAXBITS + 1], lensym[FIXLCODES], distcnt[MAXBITS + 1], distsym[MAXDCODES];
  short lengths[FIXLCODES];
  Huff lc, dc;
  int symbol;

  lc.count = lencnt; lc.symbol = lensym;
  dc.count = distcnt; dc.symbol = distsym;
  for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
  for (; symbol < 256; symbol++) lengths[symbol] = 9;
  for (; symbol < 280; symbol++) lengths[symbol] = 7;
  for (; symbol < FIXLCODES; symbol++) lengths[symbol] = 8;
  construct(&lc, lengths, FIXLCODES);
  for (symbol = 0; symbol < MAXDCODES; symbol++) lengths[symbol] = 5;
  construct(&dc, lengths, MAXDCODES);
  return codes(s, &lc, &dc);
}

static int dynamic_block(Inf* s)
{
  static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
  short lengths[MAXCODES], lencnt[MAXBITS + 1], lensym[MAXLCODES], distcnt[MAXBITS + 1], distsym[MAXDCODES];
  Huff lc, dc;
  int nlen, ndist, ncode, index, err;

  lc.count = lencnt; lc.symbol = lensym;
  dc.count = distcnt; dc.symbol = distsym;
  nlen = bits(s, 5) + 257;
  ndist = bits(s, 5) + 1;
  ncode = bits(s, 4) + 4;
  if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;

  for (index = 0; index < ncode; index++) lengths[order[index]] = (short)bits(s, 3);
  for (; index < 19; index++) lengths[order[index]] = 0;
  if (construct(&lc, lengths, 19) != 0) return -4;

  index = 0;
  while (index < nlen + ndist) {
    int symbol, len;
    symbol = decode(s, &lc);
    if (symbol < 0) return symbol;
    if (symbol < 16) lengths[index++] = (short)symbol;
    else {
      len = 0;
      if (symbol == 16) {
        if (index == 0) return -5;
        len = lengths[index - 1];
        symbol = 3 + bits(s, 2);
      }
      else if (symbol == 17) symbol = 3 + bits(s, 3);
      else symbol = 11 + bits(s, 7);
      if (index + symbol > nlen + ndist) return -6;
      while (symbol--) lengths[index++] = (short)len;
    }
  }
  if (lengths[256] == 0) return -9;
  err = construct(&lc, lengths, nlen);
  if (err && (err < 0 || nlen != lc.count[0] + lc.count[1])) return -7;
  err = construct(&dc, lengths + nlen, ndist);
  if (err && (err < 0 || ndist != dc.count[0] + dc.count[1])) return -8;
  return codes(s, &lc, &dc);
}

static int inflate_entry(Inf* s, const unsigned char* in, size_t inlen, unsigned long long expect)
{
  int last, type, ret;

  s->in = in; s->inlen = inlen; s->incnt = 0;
  s->bitbuf = 0; s->bitcnt = 0; s->outcnt = 0; s->crc = 0;
  ret = setjmp(s->env);
  if (ret) return ret == 2 ? -20 : 1;
  do {
    last = bits(s, 1);
    type = bits(s, 2);
    ret = type == 0 ? stored_block(s) : type == 1 ? fixed_block(s) : type == 2 ? dynamic_block(s) : -1;
    if (ret) return ret;
  } while (!last);
  if ((s->outcnt & (WSIZE - 1)) && !flush_win(s, (unsigned)(s->outcnt & (WSIZE - 1)))) return -20;
  return s->outcnt == expect ? 0 : -21;
}

static int store_entry(Inf* s, const unsigned char* d, unsigned long long n)
{
  s->crc = 0;
  while (n) {
    DWORD c = n > (1u << 20) ? (1u << 20) : (DWORD)n, wr = 0;
    if (!WriteFile(s->h, d, c, &wr, NULL) || wr != c) { s->werr = GetLastError(); return 0; }
    s->crc = crc_upd(s->crc, d, c);
    if (s->adv) s->adv(s->ctx, c);
    d += c; n -= c;
  }
  return 1;
}


int unzip_extract(const WCHAR* zipfile, const WCHAR* dest, ProgressCb cb, void* ud,
  int pct0, int pct1, HANDLE list, WCHAR* err, size_t errcap, unsigned long long* total_out)
{
  HANDLE hf, hm = NULL;
  const unsigned char* z = NULL, * cd, * p, * eocd = NULL;
  LARGE_INTEGER fsz;
  unsigned long long total = 0, n;
  size_t i;
  unsigned count, k, nfiles = 0, cdsize, cdoff;
  Inf* s = NULL;
  Prog pr;
  int ok = 0;
  WCHAR rel[MAX_PATH], full[MAX_PATH], par[MAX_PATH], tmp[700];
  char name[1024], u8[1024];
  char prefix[512];
  size_t plen = 0, q;
  int strip = 1, first = 1;
  DWORD wr;
  WCHAR* w;

  crc_init();
  hf = CreateFileW(zipfile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (hf == INVALID_HANDLE_VALUE) { zerr(err, errcap, L"Could not open the downloaded file."); return 0; }
  if (!GetFileSizeEx(hf, &fsz) || fsz.QuadPart < 22) {
    CloseHandle(hf);
    zerr(err, errcap, L"The download is not a valid zip file.");
    return 0;
  }
  n = (unsigned long long)fsz.QuadPart;
  hm = CreateFileMappingW(hf, NULL, PAGE_READONLY, 0, 0, NULL);
  if (hm) z = (const unsigned char*)MapViewOfFile(hm, FILE_MAP_READ, 0, 0, 0);
  if (!z) { zerr(err, errcap, L"Could not read the downloaded file."); goto done; }

  for (i = (size_t)n - 22; ; --i) {
    if (rd32(z + i) == 0x06054b50u && i + 22 + rd16(z + i + 20) <= n) { eocd = z + i; break; }
    if (i == 0 || n - i >= 22 + 65535) break;
  }
  if (!eocd) { zerr(err, errcap, L"The download is not a valid zip file."); goto done; }
  count = rd16(eocd + 10); cdsize = rd32(eocd + 12); cdoff = rd32(eocd + 16);
  if (count == 0xFFFFu || cdsize == 0xFFFFFFFFu || cdoff == 0xFFFFFFFFu) {
    zerr(err, errcap, L"This zip file is too large or uses an unsupported format (zip64).");
    goto done;
  }
  if (rd16(eocd + 4) != 0 || rd16(eocd + 6) != 0 || (unsigned long long)cdoff + cdsize > n) {
    zerr(err, errcap, L"The zip file is damaged or split across several files.");
    goto done;
  }
  cd = z + cdoff;

  p = cd;
  for (k = 0; k < count; ++k) {
    size_t nl, el, cl;
    unsigned method;
    if ((size_t)(p - cd) + 46 > cdsize || rd32(p) != 0x02014b50u) {
      zerr(err, errcap, L"The zip file is damaged."); goto done;
    }
    nl = rd16(p + 28); el = rd16(p + 30); cl = rd16(p + 32);
    if ((size_t)(p - cd) + 46 + nl + el + cl > cdsize) { zerr(err, errcap, L"The zip file is damaged."); goto done; }
    if (!(nl && p[46 + nl - 1] == '/')) {
      method = rd16(p + 10);
      if (rd16(p + 8) & 1) { zerr(err, errcap, L"The zip file is password protected."); goto done; }
      if (method != 0 && method != 8) {
        wsprintfW(tmp, L"This zip file uses an unsupported compression method (%u).", method);
        zerr(err, errcap, tmp);
        goto done;
      }
      if (rd32(p + 20) == 0xFFFFFFFFu || rd32(p + 24) == 0xFFFFFFFFu) {
        zerr(err, errcap, L"This zip file is too large or uses an unsupported format (zip64).");
        goto done;
      }
      {
        size_t j2;
        for (j2 = 0; j2 < nl && p[46 + j2] != '/' && p[46 + j2] != '\\'; ++j2) {}
        if (j2 == nl || j2 + 1 >= sizeof(prefix)) strip = 0;   /* file at the root */
        else if (first) {
          for (q = 0; q <= j2; ++q) prefix[q] = p[46 + q] == '\\' ? '/' : (char)p[46 + q];
          plen = j2 + 1;
          first = 0;
        }
        else if (plen != j2 + 1) strip = 0;
        else for (q = 0; q < plen; ++q) {
          char ch = p[46 + q] == '\\' ? '/' : (char)p[46 + q];
          if (ch != prefix[q]) { strip = 0; break; }
        }
      }
      total += rd32(p + 24);
      ++nfiles;
    }
    p += 46 + nl + el + cl;
  }
  if (!nfiles) { zerr(err, errcap, L"The zip file has no files in it."); goto done; }

  s = (Inf*)malloc(sizeof(Inf));
  if (!s) { zerr(err, errcap, L"Not enough memory to unpack the download."); goto done; }
  memset(&pr, 0, sizeof(pr));
  pr.cb = cb; pr.ud = ud; pr.total = total; pr.pct0 = pct0; pr.pct1 = pct1; pr.last = -1;
  s->adv = prog_adv; s->ctx = &pr; s->werr = 0;

  p = cd;
  for (k = 0; k < count; ++k) {
    size_t nl = rd16(p + 28), el = rd16(p + 30), cl = rd16(p + 32), lnl, lel, j;
    unsigned method = rd16(p + 10), crc = rd32(p + 16);
    unsigned long long csize = rd32(p + 20), usize = rd32(p + 24), lo = rd32(p + 42), ds;
    const unsigned char* next = p + 46 + nl + el + cl;
    int r;
    DWORD e;

    if (nl && p[46 + nl - 1] == '/') { p = next; continue; }
    if (nl == 0 || nl >= sizeof(name)) { zerr(err, errcap, L"The download contains an invalid path."); goto done; }
    memcpy(name, p + 46, nl);
    name[nl] = 0;
    for (j = 0; name[j]; ++j) if (name[j] == '\\') name[j] = '/';
    if (strip && !first && strncmp(name, prefix, plen) == 0)
      memmove(name, name + plen, strlen(name) - plen + 1);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, rel, MAX_PATH)) strcpy(u8, name);
    else if (!(MultiByteToWideChar(CP_OEMCP, 0, name, -1, rel, MAX_PATH) &&
      WideCharToMultiByte(CP_UTF8, 0, rel, -1, u8, sizeof(u8), NULL, NULL))) {
      zerr(err, errcap, L"The download contains an invalid path."); goto done;
    }
    if (!rel_path_is_safe(u8)) { zerr(err, errcap, L"The download contains an invalid path."); goto done; }
    for (w = rel; *w; ++w) if (*w == L'/') *w = L'\\';

    path_join(full, MAX_PATH, dest, rel);
    lstrcpynW(par, full, MAX_PATH);
    w = wcsrchr(par, L'\\');
    if (w) { *w = 0; make_dirs(par); }

    wsprintfW(tmp, L"Copying %s", rel);
    if (cb) cb(ud, pr.last < 0 ? pct0 : pr.last, tmp, 0);

    if (lo + 30 > n || rd32(z + lo) != 0x04034b50u) { zerr(err, errcap, L"The zip file is damaged."); goto done; }
    lnl = rd16(z + lo + 26); lel = rd16(z + lo + 28);
    ds = lo + 30 + lnl + lel;
    if (ds + csize > n) { zerr(err, errcap, L"The zip file is damaged."); goto done; }

    s->h = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s->h == INVALID_HANDLE_VALUE) { s->werr = GetLastError(); r = -20; }
    else {
      s->crc = 0;
      if (method == 0) r = (csize == usize && store_entry(s, z + ds, csize)) ? 0 : (csize == usize ? -20 : -21);
      else r = inflate_entry(s, z + ds, (size_t)csize, usize);
      CloseHandle(s->h);
    }
    if (r == -20) {
      e = s->werr;
      wsprintfW(tmp, L"Could not write %s (error %lu). If %s is running, close it and try again.",
        rel, e, g_cfg.name);
      zerr(err, errcap, tmp);
      goto done;
    }
    if (r != 0 || s->crc != crc) {
      zerr(err, errcap, L"The zip file is damaged (a file failed its checksum).");
      goto done;
    }
    WriteFile(list, u8, (DWORD)strlen(u8), &wr, NULL);
    WriteFile(list, "\n", 1, &wr, NULL);
    p = next;
  }
  *total_out = total;
  ok = 1;

done:
  free(s);
  if (z) UnmapViewOfFile(z);
  if (hm) CloseHandle(hm);
  CloseHandle(hf);
  return ok;
}