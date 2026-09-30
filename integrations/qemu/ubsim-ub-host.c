/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/misc/ubsim-ub-host.h"
#include "hw/misc/ubsim-tcg-sync.h"
#include "ubsim/ub_host_proto.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/memory.h"

#define UBSIM_UB_HOST_IRQS 3
#define UBSIM_UB_HOST_DEFAULT_POLL_NS 1000
#define UBSIM_UB_HOST_DEFAULT_LATENCY_PS 100000
#define UBSIM_UB_HOST_MMIO_TIMEOUT_US (30 * G_USEC_PER_SEC)

typedef struct UbSimUbHostState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq[UBSIM_UB_HOST_IRQS];
    QEMUTimer *poll_timer;
    QEMUTimer *sync_timer;
    QEMUTimer *dummy_timer;
    char *socket_path;
    char *sync_trigger_path;
    uint64_t mmio_base;
    uint64_t mmio_size;
    uint64_t poll_ns;
    uint64_t link_latency_ps;
    uint64_t sync_interval_ps;
    int64_t time_base_ns;
    bool sync;
    bool lifecycle_sync;
    bool lifecycle_active;
    bool lifecycle_pending;
    uint64_t lifecycle_generation;
    uint64_t next_request;
    uint64_t expected_request;
    uint64_t completion_value;
    uint16_t completion_status;
    bool completion_seen;
    CPUState *request_cpu;
    hwaddr request_offset;
    unsigned request_size;
    bool request_processing;
    bool request_pending;
    struct UbSimUbHostInterface interface;
    struct UbSimUbHostDeviceIntro device_intro;
    uint64_t mmio_transactions;
    uint64_t dma_transactions;
    uint64_t interrupts;
    uint64_t polls;
} UbSimUbHostState;

OBJECT_DECLARE_SIMPLE_TYPE(UbSimUbHostState, UBSIM_UB_HOST)

static bool ubsim_sync_active(const UbSimUbHostState *s)
{
    return SimbricksBaseIfSyncEnabled(
        (struct SimbricksBaseIf *)&s->interface.base);
}

static uint64_t ubsim_now_ps(const UbSimUbHostState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    return now <= s->time_base_ns ? 0 : (now - s->time_base_ns) * 1000;
}

static int64_t ubsim_time_from_ps(const UbSimUbHostState *s, uint64_t ps)
{
    return s->time_base_ns + DIV_ROUND_UP(ps, 1000);
}

static void ubsim_zero_message(volatile void *object, size_t size)
{
    volatile uint8_t *bytes = object;
    volatile union SimbricksProtoBaseMsg *base = object;
    uint64_t timestamp = base->header.timestamp;

    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
    base->header.timestamp = timestamp;
}

static volatile union UbSimUbHostH2DMessage *
ubsim_alloc_h2d(UbSimUbHostState *s)
{
    volatile union UbSimUbHostH2DMessage *message;

    while ((message = UbSimUbHostH2DOutAlloc(
                &s->interface, ubsim_now_ps(s))) == NULL) {
        g_thread_yield();
    }
    return message;
}

static void ubsim_send_dma_completion(UbSimUbHostState *s,
                                         uint64_t request_id,
                                         uint32_t length, bool read,
                                         bool success, const uint8_t *data)
{
    volatile union UbSimUbHostH2DMessage *message = ubsim_alloc_h2d(s);

    ubsim_zero_message(&message->completion, sizeof(message->completion));
    message->completion.request_id = request_id;
    message->completion.length = length;
    message->completion.status = success ? 0 : 4;
    if (read && success) {
        volatile uint8_t *destination =
            (volatile uint8_t *)message + sizeof(*message);
        for (uint32_t i = 0; i < length; ++i) {
            destination[i] = data[i];
        }
    }
    UbSimUbHostH2DOutSend(
        &s->interface, message,
        read ? UBSIM_H2D_DMA_READ_COMPLETION
             : UBSIM_H2D_DMA_WRITE_COMPLETION);
}

static void ubsim_handle_dma(UbSimUbHostState *s,
                                volatile union UbSimUbHostD2HMessage *message,
                                bool read)
{
    uint64_t request_id = message->dma.request_id;
    uint64_t address = message->dma.address;
    uint32_t length = message->dma.length;
    uint8_t *buffer = g_malloc(length);
    MemTxResult result;

    ++s->dma_transactions;
    if (read) {
        result = address_space_read(&address_space_memory, address,
                                    MEMTXATTRS_UNSPECIFIED, buffer, length);
    } else {
        volatile uint8_t *source =
            (volatile uint8_t *)message + sizeof(*message);
        for (uint32_t i = 0; i < length; ++i) {
            buffer[i] = source[i];
        }
        result = address_space_write(&address_space_memory, address,
                                     MEMTXATTRS_UNSPECIFIED, buffer, length);
    }
    ubsim_send_dma_completion(s, request_id, length, read,
                                 result == MEMTX_OK, buffer);
    g_free(buffer);
}

static void ubsim_handle_interrupt(
    UbSimUbHostState *s,
    const volatile struct UbSimUbHostInterrupt *interrupt)
{
    unsigned vector = interrupt->vector;

    if (vector >= UBSIM_UB_HOST_IRQS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ubsim-ub-host: invalid interrupt vector %u\n",
                      vector);
        return;
    }
    ++s->interrupts;
    switch (interrupt->action) {
    case UBSIM_INTERRUPT_LOWER:
        qemu_set_irq(s->irq[vector], 0);
        break;
    case UBSIM_INTERRUPT_RAISE:
        qemu_set_irq(s->irq[vector], 1);
        break;
    case UBSIM_INTERRUPT_PULSE:
        qemu_irq_pulse(s->irq[vector]);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ubsim-ub-host: invalid interrupt action %u\n",
                      interrupt->action);
    }
}

static bool ubsim_service_device(UbSimUbHostState *s)
{
    volatile union UbSimUbHostD2HMessage *message;
    bool progress = false;

    while ((message = UbSimUbHostD2HInPoll(
                &s->interface, ubsim_now_ps(s))) != NULL) {
        uint8_t type = UbSimUbHostD2HInType(&s->interface, message);
        progress = true;
        switch (type) {
        case UBSIM_D2H_MMIO_COMPLETION:
            if (message->completion.request_id == s->expected_request) {
                s->completion_value = message->completion.value;
                s->completion_status = message->completion.status;
                s->completion_seen = true;
                if (s->mmio_transactions <= 8) {
                    info_report("[QEMU_UB_MMIO_COMPLETE] request=%" PRIu64
                                " status=%u value=%#" PRIx64
                                " virtual_ps=%" PRIu64,
                                s->expected_request, s->completion_status,
                                s->completion_value, ubsim_now_ps(s));
                }
                if (ubsim_sync_active(s) && s->request_processing) {
                    s->request_processing = false;
                    ubsim_tcg_resume_cpu(s->request_cpu);
                }
            }
            break;
        case UBSIM_D2H_DMA_READ:
            ubsim_handle_dma(s, message, true);
            break;
        case UBSIM_D2H_DMA_WRITE:
            ubsim_handle_dma(s, message, false);
            break;
        case UBSIM_D2H_INTERRUPT:
            ubsim_handle_interrupt(s, &message->interrupt);
            break;
        case UBSIM_D2H_LIFECYCLE:
            if (s->lifecycle_sync && s->lifecycle_pending &&
                message->lifecycle.action == UBSIM_LIFECYCLE_COMMIT_SYNC &&
                message->lifecycle.generation == s->lifecycle_generation &&
                message->lifecycle.enabled) {
                s->lifecycle_pending = false;
                s->lifecycle_active = true;
            }
            break;
        default:
            if (type != SIMBRICKS_PROTO_MSG_TYPE_SYNC &&
                type != SIMBRICKS_PROTO_MSG_TYPE_TERMINATE) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "ubsim-ub-host: unknown message type %#x\n",
                              type);
            }
        }
        UbSimUbHostD2HInDone(&s->interface, message);
    }
    return progress;
}

static uint64_t ubsim_mmio_transaction(UbSimUbHostState *s,
                                          hwaddr offset, unsigned size,
                                          uint64_t value, bool write)
{
    volatile union UbSimUbHostH2DMessage *message = ubsim_alloc_h2d(s);
    uint64_t request_id = ++s->next_request;
    gint64 deadline = g_get_monotonic_time() + UBSIM_UB_HOST_MMIO_TIMEOUT_US;

    ++s->mmio_transactions;
    ubsim_zero_message(&message->mmio, sizeof(message->mmio));
    message->mmio.request_id = request_id;
    message->mmio.offset = offset;
    message->mmio.value = value;
    message->mmio.length = size;
    message->mmio.byte_enable = size >= 16 ? UINT16_MAX : (1U << size) - 1;
    s->expected_request = request_id;
    s->completion_seen = false;
    UbSimUbHostH2DOutSend(
        &s->interface, message,
        write ? UBSIM_H2D_MMIO_WRITE : UBSIM_H2D_MMIO_READ);

    while (!s->completion_seen) {
        ubsim_service_device(s);
        if (g_get_monotonic_time() > deadline) {
            error_report("ubsim-ub-host: MMIO request %" PRIu64 " timed out",
                         request_id);
            exit(EXIT_FAILURE);
        }
        g_thread_yield();
    }
    if (s->completion_status != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "ubsim-ub-host: device rejected MMIO request %" PRIu64
                      " status=%u\n", request_id, s->completion_status);
    }
    return s->completion_value;
}

/* In timing mode a read completion belongs to simulated future time.  Leave
 * the guest instruction uncommitted, halt this vCPU, and return the value when
 * TCG retries the same access after the completion timer has fired. */
static uint64_t ubsim_mmio_timing_read(UbSimUbHostState *s,
                                       hwaddr offset, unsigned size)
{
    volatile union UbSimUbHostH2DMessage *message;
    uint64_t request_id;

    g_assert(current_cpu != NULL);
    if (s->request_pending) {
        if (s->request_processing) {
            ubsim_tcg_suspend_cpu(current_cpu);
        }
        if (s->request_offset == offset && s->request_size == size) {
            if (s->mmio_transactions <= 8) {
                info_report("[QEMU_UB_MMIO_RETRY] request=%" PRIu64
                            " offset=%#" HWADDR_PRIx " size=%u status=%u"
                            " value=%#" PRIx64,
                            s->expected_request, offset, size,
                            s->completion_status, s->completion_value);
            }
            s->request_pending = false;
            if (s->completion_status != 0) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "ubsim-ub-host: timed MMIO read failed status=%u\n",
                              s->completion_status);
            }
            return s->completion_value;
        }
        s->request_pending = false;
    }

    message = ubsim_alloc_h2d(s);
    request_id = ++s->next_request;
    ++s->mmio_transactions;
    if (s->mmio_transactions <= 8) {
        info_report("[QEMU_UB_MMIO_START] request=%" PRIu64
                    " offset=%#" HWADDR_PRIx " size=%u virtual_ps=%" PRIu64,
                    request_id, offset, size, ubsim_now_ps(s));
    }
    ubsim_zero_message(&message->mmio, sizeof(message->mmio));
    message->mmio.request_id = request_id;
    message->mmio.offset = offset;
    message->mmio.length = size;
    message->mmio.byte_enable = size >= 16 ? UINT16_MAX : (1U << size) - 1;
    s->expected_request = request_id;
    s->completion_seen = false;
    s->request_cpu = current_cpu;
    s->request_offset = offset;
    s->request_size = size;
    s->request_processing = true;
    s->request_pending = true;
    UbSimUbHostH2DOutSend(&s->interface, message, UBSIM_H2D_MMIO_READ);
    ubsim_tcg_suspend_cpu(current_cpu);
}

/* Writes are posted on the simulated host link.  Ordering is retained by the
 * queue; a later read cannot overtake the write at the device. */
static void ubsim_mmio_timing_write(UbSimUbHostState *s, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    volatile union UbSimUbHostH2DMessage *message = ubsim_alloc_h2d(s);

    ++s->mmio_transactions;
    ubsim_zero_message(&message->mmio, sizeof(message->mmio));
    message->mmio.request_id = ++s->next_request;
    message->mmio.offset = offset;
    message->mmio.value = value;
    message->mmio.length = size;
    message->mmio.byte_enable = size >= 16 ? UINT16_MAX : (1U << size) - 1;
    UbSimUbHostH2DOutSend(&s->interface, message, UBSIM_H2D_MMIO_WRITE);
}

static uint64_t ubsim_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    UbSimUbHostState *s = opaque;

    if (ubsim_sync_active(s) && current_cpu != NULL) {
        return ubsim_mmio_timing_read(s, offset, size);
    }
    return ubsim_mmio_transaction(s, offset, size, 0, false);
}

static void ubsim_mmio_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    UbSimUbHostState *s = opaque;

    if (ubsim_sync_active(s)) {
        ubsim_mmio_timing_write(s, offset, value, size);
        return;
    }
    ubsim_mmio_transaction(s, offset, size, value, true);
}

static const MemoryRegionOps ubsim_mmio_ops = {
    .read = ubsim_mmio_read,
    .write = ubsim_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void ubsim_lifecycle_prepare(UbSimUbHostState *s)
{
    volatile union UbSimUbHostH2DMessage *message;

    if (!s->lifecycle_sync || s->lifecycle_active || s->lifecycle_pending ||
        !s->sync_trigger_path ||
        !g_file_test(s->sync_trigger_path, G_FILE_TEST_EXISTS)) {
        return;
    }
    message = ubsim_alloc_h2d(s);
    ubsim_zero_message(&message->lifecycle, sizeof(message->lifecycle));
    message->lifecycle.generation = ++s->lifecycle_generation;
    message->lifecycle.action = UBSIM_LIFECYCLE_PREPARE_SYNC;
    message->lifecycle.enabled = 1;
    UbSimUbHostH2DOutSend(&s->interface, message, UBSIM_H2D_LIFECYCLE);
    s->lifecycle_pending = true;
    info_report("[QEMU_UB_HOST_FENCE] generation=%" PRIu64 " prepare=1",
                s->lifecycle_generation);
}

static void ubsim_lifecycle_commit(UbSimUbHostState *s)
{
    if (!s->lifecycle_active || ubsim_sync_active(s)) {
        return;
    }
    s->time_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->interface.base.in_timestamp = 0;
    s->interface.base.out_timestamp = 0;
    s->interface.base.sync = true;
    if (UbSimUbHostH2DOutSync(&s->interface, 0) != 0) {
        error_report("ubsim-ub-host: initial lifecycle SYNC backpressured");
        exit(EXIT_FAILURE);
    }
    timer_mod(s->sync_timer, ubsim_time_from_ps(
                  s, UbSimUbHostH2DOutNextSync(&s->interface)));
    info_report("[QEMU_UB_HOST_FENCE] generation=%" PRIu64
                " active=1 epoch_origin_ns=%" PRId64,
                s->lifecycle_generation, s->time_base_ns);
}

static void ubsim_poll(void *opaque)
{
    UbSimUbHostState *s = opaque;

    ++s->polls;
    if (ubsim_sync_active(s) && s->polls % 10000 == 0) {
        info_report("[QEMU_UB_HOST_PROGRESS] polls=%" PRIu64
                    " virtual_ns=%" PRIu64,
                    s->polls, ubsim_now_ps(s) / 1000);
    }
    ubsim_service_device(s);
    ubsim_lifecycle_prepare(s);
    ubsim_lifecycle_commit(s);
    if (!ubsim_sync_active(s)) {
        timer_mod(s->poll_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->poll_ns);
        return;
    }

    /* Do not periodically sample a synchronized ring.  Wait only at this
     * simulator boundary until the peer publishes its next timestamp, then
     * let icount run directly to that virtual timer deadline. */
    for (;;) {
        volatile union UbSimUbHostD2HMessage *next =
            UbSimUbHostD2HInPeek(&s->interface, UINT64_MAX);
        uint64_t timestamp = UbSimUbHostD2HInTimestamp(&s->interface);

        if (next != NULL && timestamp <= ubsim_now_ps(s)) {
            ubsim_service_device(s);
            continue;
        }
        if (next != NULL) {
            timer_mod(s->dummy_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
            timer_mod(s->poll_timer, ubsim_time_from_ps(s, timestamp));
            return;
        }
        if (SimbricksBaseIfInTerminated(&s->interface.base)) {
            return;
        }
    }
}

static void ubsim_sync(void *opaque)
{
    UbSimUbHostState *s = opaque;
    uint64_t now = ubsim_now_ps(s);

    while (UbSimUbHostH2DOutSync(&s->interface, now) != 0) {
    }
    timer_mod(s->sync_timer, ubsim_time_from_ps(
                  s, UbSimUbHostH2DOutNextSync(&s->interface)));
}

static void ubsim_dummy(void *opaque)
{
    (void)opaque;
}

static void ubsim_realize(DeviceState *dev, Error **errp)
{
    UbSimUbHostState *s = UBSIM_UB_HOST(dev);
    struct SimbricksBaseIfParams params = { 0 };
    struct UbSimUbHostIntro host_intro = {
        .version = UBSIM_UB_HOST_VERSION,
        .address_bits = 64,
        .mmio_base = s->mmio_base,
        .mmio_size = s->mmio_size,
    };
    struct SimBricksBaseIfEstablishData establish = {
        .base_if = &s->interface.base,
        .tx_intro = &host_intro,
        .tx_intro_len = sizeof(host_intro),
        .rx_intro = &s->device_intro,
        .rx_intro_len = sizeof(s->device_intro),
    };

    if (!s->socket_path || !*s->socket_path) {
        error_setg(errp, "ubsim-ub-host requires a socket path");
        return;
    }
    ubsim_ub_host_default_params(&params);
    params.sock_path = s->socket_path;
    params.link_latency = s->link_latency_ps;
    params.sync_interval = s->sync_interval_ps;
    params.sync_mode = s->sync ? kSimbricksBaseIfSyncRequired
                               : kSimbricksBaseIfSyncDisabled;
    if (SimbricksBaseIfInit(&s->interface.base, &params) != 0 ||
        SimbricksBaseIfConnect(&s->interface.base) != 0 ||
        SimBricksBaseIfEstablish(&establish, 1) != 0) {
        error_setg(errp, "cannot connect UB-HOST socket %s", s->socket_path);
        return;
    }
    if (s->device_intro.version != UBSIM_UB_HOST_VERSION) {
        error_setg(errp, "UB-HOST protocol version mismatch");
        SimbricksBaseIfClose(&s->interface.base);
        return;
    }
    s->time_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ubsim_poll, s);
    if (s->sync || s->lifecycle_sync) {
        s->dummy_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ubsim_dummy, s);
        s->sync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ubsim_sync, s);
    }
    if (s->sync) {
        if (UbSimUbHostH2DOutSync(&s->interface, 0) != 0) {
            error_setg(errp, "cannot send initial UB-HOST synchronization");
            return;
        }
        timer_mod(s->sync_timer, ubsim_time_from_ps(
                      s, UbSimUbHostH2DOutNextSync(&s->interface)));
        ubsim_poll(s);
    } else {
        timer_mod(s->poll_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->poll_ns);
    }
    info_report("ubsim-ub-host: connected %s, %u port(s), sync=%s",
                s->socket_path, s->device_intro.port_count,
                s->sync ? "required" :
                (s->lifecycle_sync ? "lifecycle" : "off"));
}

static void ubsim_unrealize(DeviceState *dev)
{
    UbSimUbHostState *s = UBSIM_UB_HOST(dev);

    if (s->poll_timer) {
        timer_free(s->poll_timer);
        s->poll_timer = NULL;
    }
    if (s->sync_timer) {
        timer_free(s->sync_timer);
        s->sync_timer = NULL;
    }
    if (s->dummy_timer) {
        timer_free(s->dummy_timer);
        s->dummy_timer = NULL;
    }
    if (s->request_processing && s->request_cpu) {
        ubsim_tcg_resume_cpu(s->request_cpu);
    }
    if (s->interface.base.conn_state != 0) {
        SimbricksBaseIfClose(&s->interface.base);
    }
    info_report("[QEMU_UB_HOST_PROFILE] mmio=%" PRIu64 " dma=%" PRIu64
                " irq=%" PRIu64 " polls=%" PRIu64,
                s->mmio_transactions, s->dma_transactions,
                s->interrupts, s->polls);
}

static void ubsim_instance_init(Object *obj)
{
    UbSimUbHostState *s = UBSIM_UB_HOST(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->poll_ns = UBSIM_UB_HOST_DEFAULT_POLL_NS;
    s->link_latency_ps = UBSIM_UB_HOST_DEFAULT_LATENCY_PS;
    s->sync_interval_ps = UBSIM_UB_HOST_DEFAULT_LATENCY_PS;
    memory_region_init_io(&s->mmio, obj, &ubsim_mmio_ops, s,
                          TYPE_UBSIM_UB_HOST, UINT64_C(0x1000000));
    /* Timed reads deliberately leave the MMIO callback via a TCG longjmp and
     * retry the instruction after the external completion arrives.  QEMU's
     * normal callback epilogue therefore cannot clear the device reentrancy
     * guard.  SimBricks BARs use the same opt-out for this suspend/retry
     * protocol. */
    s->mmio.disable_reentrancy_guard = true;
    sysbus_init_mmio(sbd, &s->mmio);
    for (unsigned i = 0; i < UBSIM_UB_HOST_IRQS; ++i) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static const Property ubsim_properties[] = {
    DEFINE_PROP_STRING("socket", UbSimUbHostState, socket_path),
    DEFINE_PROP_STRING("sync-trigger", UbSimUbHostState, sync_trigger_path),
    DEFINE_PROP_UINT64("mmio-base", UbSimUbHostState, mmio_base,
                       UINT64_C(0x2d000000)),
    DEFINE_PROP_UINT64("mmio-size", UbSimUbHostState, mmio_size,
                       UINT64_C(0x1000000)),
    DEFINE_PROP_UINT64("poll-ns", UbSimUbHostState, poll_ns,
                       UBSIM_UB_HOST_DEFAULT_POLL_NS),
    DEFINE_PROP_BOOL("sync", UbSimUbHostState, sync, false),
    DEFINE_PROP_BOOL("lifecycle-sync", UbSimUbHostState, lifecycle_sync,
                     false),
    DEFINE_PROP_UINT64("link-latency-ps", UbSimUbHostState,
                       link_latency_ps, UBSIM_UB_HOST_DEFAULT_LATENCY_PS),
    DEFINE_PROP_UINT64("sync-interval-ps", UbSimUbHostState,
                       sync_interval_ps, UBSIM_UB_HOST_DEFAULT_LATENCY_PS),
};

static void ubsim_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ubsim_realize;
    dc->unrealize = ubsim_unrealize;
    device_class_set_props(dc, ubsim_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo ubsim_info = {
    .name = TYPE_UBSIM_UB_HOST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(UbSimUbHostState),
    .instance_init = ubsim_instance_init,
    .class_init = ubsim_class_init,
};

static void ubsim_register_types(void)
{
    type_register_static(&ubsim_info);
}
type_init(ubsim_register_types)

static uint64_t ubsim_env_u64(const char *name, uint64_t fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    uint64_t parsed;

    if (!value || !*value) {
        return fallback;
    }
    errno = 0;
    parsed = g_ascii_strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0) {
        error_report("invalid positive integer in %s=%s", name, value);
        exit(EXIT_FAILURE);
    }
    return parsed;
}

void ubsim_ub_host_create(const char *socket_path, hwaddr mmio_base,
                             hwaddr mmio_size, DeviceState *gic,
                             unsigned irq_base)
{
    DeviceState *dev = qdev_new(TYPE_UBSIM_UB_HOST);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    qdev_prop_set_string(dev, "socket", socket_path);
    qdev_prop_set_uint64(dev, "mmio-base", mmio_base);
    qdev_prop_set_uint64(dev, "mmio-size", mmio_size);
    qdev_prop_set_uint64(dev, "poll-ns",
        ubsim_env_u64("UBSIM_QEMU_UB_HOST_POLL_NS",
                      UBSIM_UB_HOST_DEFAULT_POLL_NS));
    const char *sync = getenv("UBSIM_QEMU_UB_HOST_SYNC");
    qdev_prop_set_bit(dev, "sync",
                      sync && (!strcmp(sync, "1") || !strcmp(sync, "on") ||
                               !strcmp(sync, "required")));
    const char *lifecycle = getenv("UBSIM_QEMU_UB_HOST_LIFECYCLE_SYNC");
    qdev_prop_set_bit(dev, "lifecycle-sync",
                      lifecycle && (!strcmp(lifecycle, "1") ||
                                    !strcmp(lifecycle, "on")));
    const char *trigger = getenv("UBSIM_QEMU_UB_HOST_SYNC_TRIGGER");
    if (trigger && *trigger) {
        qdev_prop_set_string(dev, "sync-trigger", trigger);
    }
    qdev_prop_set_uint64(dev, "link-latency-ps",
        ubsim_env_u64("UBSIM_QEMU_UB_HOST_LATENCY_PS",
                      UBSIM_UB_HOST_DEFAULT_LATENCY_PS));
    qdev_prop_set_uint64(dev, "sync-interval-ps",
        ubsim_env_u64("UBSIM_QEMU_UB_HOST_SYNC_INTERVAL_PS",
                      UBSIM_UB_HOST_DEFAULT_LATENCY_PS));
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(), mmio_base,
                                        sysbus_mmio_get_region(sbd, 0), 10);
    for (unsigned i = 0; i < UBSIM_UB_HOST_IRQS; ++i) {
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(gic, irq_base + i));
    }
}
