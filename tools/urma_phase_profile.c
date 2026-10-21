/* ARM64 guest LD_PRELOAD probe for the perftest READ posting/polling path.
 * The clock reads are confined to the three interposed UMDK entry points.
 * Report at process exit, outside the measured perftest ROI. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef int (*post_fn)(void *, void *, void **);
typedef int (*poll_fn)(void *, int, void *);

struct phase_stat {
    uint64_t calls;
    uint64_t ticks;
    uint64_t max_ticks;
};

static post_fn real_post_jetty;
static post_fn real_post_jfs;
static poll_fn real_poll;
static struct phase_stat jetty_posts, jfs_posts, polls, empty_polls, positive_polls;
static struct phase_stat post_to_post, post_to_poll, poll_to_post, poll_to_poll;
static uint64_t completions, first_post_tick, last_completion_tick;
static uint64_t previous_call_end;
static int previous_call_kind;

enum { KIND_POST = 1, KIND_POLL = 2 };

static inline uint64_t now_ticks(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r" (value));
    return value;
}

static inline uint64_t timer_frequency(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r" (value));
    return value;
}

static void add(struct phase_stat *stat, uint64_t delta)
{
    ++stat->calls;
    stat->ticks += delta;
    if (delta > stat->max_ticks)
        stat->max_ticks = delta;
}

static void enter_call(int kind, uint64_t start)
{
    if (!previous_call_end || start < previous_call_end)
        return;
    struct phase_stat *stat = NULL;
    if (previous_call_kind == KIND_POST && kind == KIND_POST)
        stat = &post_to_post;
    else if (previous_call_kind == KIND_POST && kind == KIND_POLL)
        stat = &post_to_poll;
    else if (previous_call_kind == KIND_POLL && kind == KIND_POST)
        stat = &poll_to_post;
    else if (previous_call_kind == KIND_POLL && kind == KIND_POLL)
        stat = &poll_to_poll;
    if (stat)
        add(stat, start - previous_call_end);
}

static void leave_call(int kind, uint64_t end)
{
    previous_call_end = end;
    previous_call_kind = kind;
}

__attribute__((constructor)) static void phase_init(void)
{
    *(void **)(&real_post_jetty) = dlsym(RTLD_NEXT, "urma_post_jetty_send_wr");
    *(void **)(&real_post_jfs) = dlsym(RTLD_NEXT, "urma_post_jfs_wr");
    *(void **)(&real_poll) = dlsym(RTLD_NEXT, "urma_poll_jfc");
    if (!real_post_jetty || !real_post_jfs || !real_poll) {
        fputs("[URMA_PHASE] missing UMDK symbol\n", stderr);
        abort();
    }
}

int urma_post_jetty_send_wr(void *jetty, void *wr, void **bad_wr)
{
    const uint64_t start = now_ticks();
    if (!first_post_tick)
        first_post_tick = start;
    enter_call(KIND_POST, start);
    const int result = real_post_jetty(jetty, wr, bad_wr);
    const uint64_t end = now_ticks();
    add(&jetty_posts, end - start);
    leave_call(KIND_POST, end);
    return result;
}

int urma_post_jfs_wr(void *jfs, void *wr, void **bad_wr)
{
    const uint64_t start = now_ticks();
    if (!first_post_tick)
        first_post_tick = start;
    enter_call(KIND_POST, start);
    const int result = real_post_jfs(jfs, wr, bad_wr);
    const uint64_t end = now_ticks();
    add(&jfs_posts, end - start);
    leave_call(KIND_POST, end);
    return result;
}

int urma_poll_jfc(void *jfc, int count, void *cr)
{
    const uint64_t start = now_ticks();
    if (first_post_tick)
        enter_call(KIND_POLL, start);
    const int result = real_poll(jfc, count, cr);
    const uint64_t end = now_ticks();
    if (first_post_tick) {
        add(&polls, end - start);
        if (result == 0)
            add(&empty_polls, end - start);
        else if (result > 0) {
            add(&positive_polls, end - start);
            completions += (uint64_t)result;
            last_completion_tick = end;
        }
        leave_call(KIND_POLL, end);
    }
    return result;
}

__attribute__((destructor)) static void phase_report(void)
{
    const uint64_t span = last_completion_tick > first_post_tick ?
        last_completion_tick - first_post_tick : 0;
    const uint64_t posted = jetty_posts.ticks + jfs_posts.ticks;
    const uint64_t attributed = posted + polls.ticks;
    fprintf(stderr,
        "[URMA_PHASE] frequency_hz=%" PRIu64 " span_ticks=%" PRIu64
        " post_jetty_calls=%" PRIu64 " post_jetty_ticks=%" PRIu64
        " post_jfs_calls=%" PRIu64 " post_jfs_ticks=%" PRIu64
        " poll_calls=%" PRIu64 " poll_ticks=%" PRIu64
        " poll_empty_calls=%" PRIu64 " poll_empty_ticks=%" PRIu64
        " poll_positive_calls=%" PRIu64 " poll_positive_ticks=%" PRIu64
        " cqes=%" PRIu64 " other_ticks=%" PRIu64
        " gap_post_post_ticks=%" PRIu64 " gap_post_poll_ticks=%" PRIu64
        " gap_poll_post_ticks=%" PRIu64 " gap_poll_poll_ticks=%" PRIu64
        " gap_post_post_calls=%" PRIu64 " gap_post_poll_calls=%" PRIu64
        " gap_poll_post_calls=%" PRIu64 " gap_poll_poll_calls=%" PRIu64
        " gap_post_post_max=%" PRIu64 " gap_post_poll_max=%" PRIu64
        " gap_poll_post_max=%" PRIu64 " gap_poll_poll_max=%" PRIu64
        " post_max=%" PRIu64 " poll_max=%" PRIu64 "\n",
        timer_frequency(), span,
        jetty_posts.calls, jetty_posts.ticks,
        jfs_posts.calls, jfs_posts.ticks,
        polls.calls, polls.ticks,
        empty_polls.calls, empty_polls.ticks,
        positive_polls.calls, positive_polls.ticks,
        completions, span > attributed ? span - attributed : 0,
        post_to_post.ticks, post_to_poll.ticks,
        poll_to_post.ticks, poll_to_poll.ticks,
        post_to_post.calls, post_to_poll.calls,
        poll_to_post.calls, poll_to_poll.calls,
        post_to_post.max_ticks, post_to_poll.max_ticks,
        poll_to_post.max_ticks, poll_to_poll.max_ticks,
        jetty_posts.max_ticks > jfs_posts.max_ticks ?
            jetty_posts.max_ticks : jfs_posts.max_ticks,
        polls.max_ticks);
}
