#include <stdio.h>
#include <string.h>
#include <time.h>
#include "discord_etf.h"
#include "discord_state.h"
#include "discord_store.h"

/*
 * Measures the hot path so a change can be judged, not assumed.
 * Run: clang -O2 -Icomponents/... tools/bench.c <sources> -o /tmp/bench
 * Baseline recorded in docs/12_security_audit.md section 8.
 */

static double now_ms(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  return ts.tv_sec*1000.0 + ts.tv_nsec/1e6; }

static size_t build_etf(uint8_t *b){ size_t p=0;
  b[p++]=DISCORD_ETF_SMALL_MAP; b[p++]=3;
  b[p++]=DISCORD_ETF_STRING; b[p++]=0; b[p++]=1; b[p++]='o';
  b[p++]=DISCORD_ETF_SMALL_INTEGER; b[p++]=0;
  b[p++]=DISCORD_ETF_STRING; b[p++]=0; b[p++]=1; b[p++]='t';
  b[p++]=DISCORD_ETF_STRING; b[p++]=0;b[p++]=14; memcpy(b+p,"MESSAGE_CREATE",14); p+=14;
  b[p++]=DISCORD_ETF_STRING; b[p++]=0; b[p++]=1; b[p++]='s';
  b[p++]=DISCORD_ETF_STRING; b[p++]=0; b[p++]=19; memcpy(b+p,"1234567890123456789",19); p+=19;
  return p; }

int main(void){
  uint8_t buf[256]; size_t len=build_etf(buf);
  DiscordEtfReader r; int iters=200000;
  double t0=now_ms();
  for(int i=0;i<iters;i++){ discord_etf_reader_init(&r,buf,len,16);
    if(discord_etf_begin_map(&r)!=DISCORD_ETF_OK) return 1;
    DiscordEtfField f; while(discord_etf_next_map_field(&r,&f)==DISCORD_ETF_OK&&f.kind!=DISCORD_ETF_FIELD_END){} }
  double t1=now_ms();
  printf("ETF parse      %8.1f ns/op\n",(t1-t0)*1e6/iters);

  DcBotState s; memset(&s,0,sizeof(s)); iters=200000;
  t0=now_ms();
  for(int i=0;i<iters;i++) dc_state_rank_user(&s,(unsigned long)(i%128)+1);
  t1=now_ms();
  printf("rank_user      %8.1f ns/op\n",(t1-t0)*1e6/iters);

  DcStore st; dc_store_init(&st); iters=100000;
  t0=now_ms();
  for(int i=0;i<iters;i++){ DcUserRecord rec; memset(&rec,0,sizeof(rec));
    rec.user_id=(unsigned long)(i%256)+1; rec.xp=i; dc_store_put(&st,&rec); }
  t1=now_ms();
  printf("store_put      %8.1f ns/op\n",(t1-t0)*1e6/iters);

  iters=100000; t0=now_ms();
  for(int i=0;i<iters;i++){ DcUserRecord o; dc_store_get(&st,(unsigned long)(i%256)+1,&o); }
  t1=now_ms();
  printf("store_get      %8.1f ns/op\n",(t1-t0)*1e6/iters);
  return 0;}
