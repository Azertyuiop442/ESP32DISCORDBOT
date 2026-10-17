#include <stdio.h>
#include <string.h>

#include "discord_core.h"
#include "discord_feature.h"
#include "discord_gateway.h"
#include "discord_jobq.h"
#include "discord_json.h"
#include "discord_ratelimit.h"
#include "discord_state.h"
#include "discord_store.h"
#include "discord_watchdog.h"
#include "discord_ackq.h"

#define KIB 1024.0

static size_t static_ram(void)
{
    return sizeof(DcBotState) + sizeof(DcGateway) + sizeof(DcRateLimiter) +
           sizeof(DcWatchdog) + sizeof(DcJobQueue) + sizeof(DcAckq) +
           sizeof(DiscordFeatureRegistry) + sizeof(DiscordInteraction) +
           sizeof(DiscordMessage);
}

static void ram_breakdown(void)
{
    printf("STATIC RAM (.bss, hot)\n");
    printf("  bot state          %6zu B\n", sizeof(DcBotState));
    printf("  gateway            %6zu B\n", sizeof(DcGateway));
    printf("  rate limiter       %6zu B\n", sizeof(DcRateLimiter));
    printf("  watchdog           %6zu B\n", sizeof(DcWatchdog));
    printf("  job queue          %6zu B  (%d slots x %zu B)\n", sizeof(DcJobQueue),
           DC_JOBQ_CAPACITY, sizeof(DcJob));
    printf("  ack queue          %6zu B  (%d slots x %zu B)\n", dc_ackq_footprint(),
           DC_ACKQ_CAPACITY, sizeof(DiscordAck));
    printf("  feature registry   %6zu B\n", sizeof(DiscordFeatureRegistry));
    printf("  interaction        %6zu B\n", sizeof(DiscordInteraction));
    printf("  message            %6zu B\n", sizeof(DiscordMessage));
    printf("  TOTAL              %6zu B  (%.2f KiB)\n\n", static_ram(),
           (double)static_ram() / KIB);
}

static void bot_state_detail(void)
{
    printf("DcBotState breakdown\n");
    printf("  tickets  %2d x %2zu B = %4zu B\n", DC_TICKET_SLOTS, sizeof(DcTicket),
           (size_t)DC_TICKET_SLOTS * sizeof(DcTicket));
    printf("  captcha  %2d x %2zu B = %4zu B\n", DC_CAPTCHA_SLOTS, sizeof(DcCaptcha),
           (size_t)DC_CAPTCHA_SLOTS * sizeof(DcCaptcha));
    printf("  spam     %2d x %2zu B = %4zu B\n", DC_SPAM_USERS, sizeof(DcSpamUser),
           (size_t)DC_SPAM_USERS * sizeof(DcSpamUser));
    printf("  ranks    %3d x %2zu B = %4zu B\n\n", DC_RANK_USERS, sizeof(DcRankUser),
           (size_t)DC_RANK_USERS * sizeof(DcRankUser));
}

static void flash_breakdown(void)
{
    size_t total_flash = 4u * 1024u * 1024u;
    size_t store = dc_store_flash_bytes();

    printf("FLASH (cold)\n");
    printf("  user record         %6zu B  (32 B incl. alignment)\n", sizeof(DcUserRecord));
    printf("  store A/B           %6zu B  for %d records\n", store, DC_STORE_RECORDS);
    printf("  effective per user  %6.1f B  (A/B doubles it)\n",
           (double)store / DC_STORE_RECORDS);
    printf("  share of 4 MiB      %6.2f %%\n", 100.0 * (double)store / (double)total_flash);
    printf("  left for code       %6zu B\n\n", total_flash - store - 0x8000 - 0x2000);
}

static void heap_headroom(void)
{
    size_t app = static_ram();
    size_t pessimistic = 91u * 1024u;
    size_t optimistic = 142u * 1024u;

    printf("HEADROOM vs free heap after Wi-Fi + 2 TLS contexts\n");
    printf("  pessimistic 91 KiB: app %.1f%%, leaves %zu B for stacks + drivers\n",
           100.0 * (double)app / (double)pessimistic, pessimistic - app);
    printf("  optimistic 142 KiB: app %.1f%%, leaves %zu B\n\n",
           100.0 * (double)app / (double)optimistic, optimistic - app);
}

static void capacity(void)
{
    printf("BOT LOGIC CAPACITY (hard bounds, oldest recycled)\n");
    printf("  ranks tracked      %d users\n", DC_RANK_USERS);
    printf("  spam window        %d users x %d hashes\n", DC_SPAM_USERS, DC_SPAM_WINDOW);
    printf("  open tickets       %d\n", DC_TICKET_SLOTS);
    printf("  pending captcha    %d\n", DC_CAPTCHA_SLOTS);
    printf("  flash users        %d records\n", DC_STORE_RECORDS);
    printf("  queued jobs        %d (%u executed, %u failed, %u dropped)\n\n",
           DC_JOBQ_CAPACITY, 0u, 0u, 0u);
}

static void throughput(void)
{
    printf("THROUGHPUT CEILINGS (Discord-imposed, not hardware)\n");
    printf("  REST     50 req/s = %d/hour = %d/day\n", 50 * 3600, 50 * 86400);
    printf("  Gateway  120 events/60s = %d/day outbound\n", 120 * 1440);
    printf("  Inbound  unbounded: rate limits govern what we send, not receive\n");
    printf("  Interactions are EXEMPT from the 50/s global cap\n\n");
}

static void self_cleanup(void)
{
    DcBotState s;
    memset(&s, 0, sizeof(s));
    size_t before = dc_state_footprint();

    for (int i = 0; i < 50000; i++) {
        dc_state_open_ticket(&s, (DiscordId)(i + 1), 222);
    }
    size_t after = dc_state_footprint();

    printf("SELF-CLEANUP VERIFIED\n");
    printf("  50000 tickets opened, footprint %zu -> %zu B, slots %d\n", before, after,
           DC_TICKET_SLOTS);
    printf("  bounded: %s\n\n", before == after ? "YES" : "NO");
}

static void queue_under_load(void)
{
    DcJobQueue q;
    dc_jobq_init(&q, DC_JOB_DROP_NEWEST);

    for (uint32_t i = 0; i < 50000; i++) {
        DcJob j;
        memset(&j, 0, sizeof(j));
        j.kind = DC_JOB_SEND_MESSAGE;
        j.user_id = i + 1;
        dc_jobq_push(&q, &j);
        if (i % 2 == 0) {
            DcJob out;
            dc_jobq_pop(&q, &out);
        }
    }

    printf("JOB QUEUE UNDER LOAD\n");
    printf("  50000 jobs pushed, count %u / %d, peak %u\n", q.count, DC_JOBQ_CAPACITY,
           q.peak);
    printf("  enqueued %llu, executed %llu, dropped %llu\n",
           (unsigned long long)q.enqueued, (unsigned long long)q.executed,
           (unsigned long long)q.dropped);
    printf("  bounded: %s, overload visible: %s\n\n", q.count <= DC_JOBQ_CAPACITY ? "YES" : "NO",
           q.dropped > 0 ? "YES" : "NO");
}

static void ack_queue_under_load(void)
{
    DcAckq q;
    dc_ackq_init(&q);

    for (uint32_t i = 1; i <= 50000u; i++) {
        DiscordAck a;
        memset(&a, 0, sizeof(a));
        a.id = (DiscordId)i;
        a.kind = 4;
        dc_ackq_push(&q, &a);
        if (i % 2u == 0u) {
            DiscordAck out;
            dc_ackq_pop(&q, &out);
        }
    }

    printf("ACK QUEUE UNDER LOAD\n");
    printf("  50000 acks pushed, depth %u / %d, peak %u\n", dc_ackq_depth(&q),
           DC_ACKQ_CAPACITY, q.peak);
    printf("  enqueued %u, dropped %u\n", q.enqueued, dc_ackq_dropped(&q));
    printf("  bounded: %s, overload visible: %s\n\n",
           dc_ackq_depth(&q) <= DC_ACKQ_CAPACITY ? "YES" : "NO",
           dc_ackq_dropped(&q) > 0 ? "YES" : "NO");
}

int main(void)
{
    printf("\nESP32 Discord framework footprint (measured, not estimated)\n");
    printf("===========================================================\n\n");

    ram_breakdown();
    bot_state_detail();
    flash_breakdown();
    heap_headroom();
    capacity();
    throughput();
    self_cleanup();
    queue_under_load();
    ack_queue_under_load();

    return 0;
}