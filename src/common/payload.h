#ifndef PAYLOAD_H
#define PAYLOAD_H

#include "common.h"

#define TRAILER_MAGIC "IMKR0001"
#define TRAILER_SIZE  24

typedef struct {
  const unsigned char* base;
  size_t size, off;
  unsigned count;
  unsigned long long total;
} Payload;

typedef struct {
  char path[512];
  unsigned long long size;
  const unsigned char* data;
} PayloadEntry;

int payload_load_config(AppConfig* out);             
const unsigned char* payload_logo(size_t* n);        
const unsigned char* payload_banner(size_t* n);
int payload_open(Payload* p);
int payload_next(Payload* p, PayloadEntry* e);
unsigned long long payload_peek_total(void);
int payload_write_uninstaller(const WCHAR* dst);    

#endif