#ifndef DRAW_H
#define DRAW_H

#include "common.h"

typedef struct {
  COLORREF bg, panel, card, head, border, sel, btnhov, btnbd,
    accent, acchov, text, text2, dim, err;
} Theme;

extern Theme g_th;
extern int   g_light;
void set_theme(int light);      
void apply_theme(HWND hwnd, int light);  
int  system_prefers_light(void);

#define C_BG      g_th.bg
#define C_PANEL   g_th.panel
#define C_CARD    g_th.card
#define C_HEAD    g_th.head
#define C_BORDER  g_th.border
#define C_SEL     g_th.sel
#define C_BTNHOV  g_th.btnhov
#define C_BTNBD   g_th.btnbd
#define C_ACCENT  g_th.accent
#define C_ACCHOV  g_th.acchov
#define C_TEXT    g_th.text
#define C_TEXT2   g_th.text2
#define C_DIM     g_th.dim
#define C_ERR     g_th.err
#define C_WHITE   RGB(0xff, 0xff, 0xff)

#define TXT_L  (DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX)
#define TXT_C  (DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX)
#define TXT_R  (DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX)
#define TXT_W  (DT_LEFT | DT_WORDBREAK | DT_NOPREFIX)

enum { IK_HEAD, IK_TEXT, IK_NOTE, IK_CHECK, IK_BTN, IK_ACCENT, IK_THEME, IK_SEG };

typedef struct {
  int kind, id, enabled;
  const WCHAR* text;
  RECT r;
  int* val;
  int sel;
} Item;

typedef struct DImage DImage;
extern DImage* g_logo_img;     
extern DImage* g_banner_img;   

DImage* img_load(const void* data, size_t len);   
void    img_free(DImage* im);
void    img_draw(HDC dc, DImage* im, RECT dst, int cover, int rad);

extern int     g_dpi;
extern HFONT   g_font, g_fontb, g_fontt;
extern HBRUSH  g_editbrush;

int   S(int v);
RECT  R(int l, int t, int r, int b);
RECT  RAW(int l, int t, int r, int b);
WCHAR* dupw(const WCHAR* s);

void  fillr(HDC dc, RECT r, COLORREF c);
void  rrect(HDC dc, RECT r, COLORREF fill, COLORREF border, int rad);
void  line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int wd);
void  circle(HDC dc, int cx, int cy, int rad, COLORREF c);
void  txt(HDC dc, HFONT f, COLORREF c, RECT r, UINT fmt, const WCHAR* t);

void  draw_item(HDC dc, const Item* it, int hov);
void  draw_logo(HDC dc, const WCHAR* name);
void  draw_logo_at(HDC dc, RECT r, const WCHAR* name, DImage* img, HFONT f);
void  draw_check_badge(HDC dc);
void  draw_progress_page(HDC dc, int pct, const WCHAR* status, const WCHAR (*log)[120], int nlog, int busy);

void  draw_init(void);
void  draw_shutdown(void);
void  dark_title_bar(HWND hwnd);

#endif