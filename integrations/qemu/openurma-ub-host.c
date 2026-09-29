/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"

#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/misc/openurma-ub-host.h"
#include "openurma/ub_host_proto.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"
#include "system/memory.h"

#define OPENURMA_UB_HOST_IRQS 3
#define OPENURMA_UB_HOST_DEFAULT_POLL_NS 1000
#define OPENURMA_UB_HOST_MMIO_TIMEOUT_US (30 * G_USEC_PER_SEC)

typedef struct OpenUrmaUbHostState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq[OPENURMA_UB_HOST_IRQS];
    QEMUTimer *poll_timer;
    char *socket_path;
    uint64_t mmio_base;
    uint64_t mmio_size;
    uint64_t poll_ns;
    uint64_t next_request;
    uint64_t expected_request;
    uint64_t completion_value;
    uint16_t completion_status;
    bool completion_seen;
    struct OpenUrmaUbHostInterface interface;
    struct OpenUrmaUbHostDeviceIntro device_intro;
    uint64_t mmio_transactions;
    uint64_t dma_transactions;
    uint64_t interrupts;
    uint64_t polls;
} OpenUrmaUbHostState;

OBJECT_DECLARE_SIMPLE_TYPE(OpenUrmaUbHostState, OPENURMA_UB_HOST)

static uint64_t openurma_now_ps(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) * 1000;
}

static void openurma_zero_message(volatile void *object, size_t size)
{
    volatile uint8_t *bytes = object;
    volatile union SimbricksProtoBaseMsg *base = object;
    uint64_t timestamp = base->header.timestamp;

    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
    base->header.timestamp = timestamp;
}

static volatile union OpenUrmaUbHostH2DMessage *
openurma_alloc_h2d(OpenUrmaUbHostState *s)
{
    volatile union OpenUrmaUbHostH2DMessage *message;

    while ((message = OpenUrmaUbHostH2DOutAlloc(
                &s->interface, openurma_now_ps())) == NULL) {
        g_thread_yield();
    }
    return message;
}

static void openurma_send_dma_completion(OpenUrmaUbHostState *s,
                                         uint64_t request_id,
                                         uint32_t length, bool read,
                                         bool success, const uint8_t *data)
{
    volatile union OpenUrmaUbHostH2DMessage *message = openurma_alloc_h2d(s);

    openurma_zero_message(&message->completion, sizeof(message->completion));
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
    OpenUrmaUbHostH2DOutSend(
        &s->interface, message,
        read ? OPENURMA_H2D_DMA_READ_COMPLETION
             : OPENURMA_H2D_DMA_WRITE_COMPLETION);
}

static void openurma_handle_dma(OpenUrmaUbHostState *s,
                                volatile union OpenUrmaUbHostD2HMessage *message,
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
    openurma_send_dma_completion(s, request_id, length, read,
                                 result == MEMTX_OK, buffer);
    g_free(buffer);
}

static void openurma_handle_interrupt(
    OpenUrmaUbHostState *s,
    const volatile struct OpenUrmaUbHostInterrupt *interrupt)
{
    unsigned vector = interrupt->vector;

    if (vector >= OPENURMA_UB_HOST_IRQS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "openurma-ub-host: invalid interrupt vector %u\n",
                      vector);
        return;
    }
    ++s->interrupts;
    switch (interrupt->action) {
    case OPENURMA_INTERRUPT_LOWER:
        qemu_set_irq(s->irq[vector], 0);
        break;
    case OPENURMA_INTERRUPT_RAISE:
        qemu_set_irq(s->irq[vector], 1);
        break;
    case OPENURMA_INTERRUPT_PULSE:
        qemu_irq_pulse(s->irq[vector]);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "openurma-ub-host: invalid interrupt action %u\n",
                      interrupt->action);
    }
}

static bool openurma_service_device(OpenUrmaUbHostState *s)
{
    volatile union OpenUrmaUbHostD2HMessage *message;
    bool progress = false;

    while ((message = OpenUrmaUbHostD2HInPoll(
                &s->interface, openurma_now_ps())) != NULL) {
        uint8_t type = OpenUrmaUbHostD2HInType(&s->interface, message);
        progress = true;
        switch (type) {
        case OPENURMA_D2H_MMIO_COMPLETION:
            if (message->completion.request_id == s->expected_request) {
                s->completion_value = message->completion.value;
                s->completion_status = message->completion.status;
                s->completion_seen = true;
            }
            break;
        case OPENURMA_D2H_DMA_READ:
            openurma_handle_dma(s, message, true);
            break;
        case OPENURMA_D2H_DMA_WRITE:
            openurma_handle_dma(s, message, false);
            break;
        case OPENURMA_D2H_INTERRUPT:
            openurma_handle_interrupt(s, &message->interrupt);
            break;
        case OPENURMA_D2H_LIFECYCLE:
            qemu_log_mask(LOG_UNIMP,
                          "openurma-ub-host: lifecycle message ignored in initial QEMU adapter\n");
            break;
        default:
            if (type != SIMBRICKS_PROTO_MSG_TYPE_SYNC &&
                type != SIMBRICKS_PROTO_MSG_TYPE_TERMINATE) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "openurma-ub-host: unknown message type %#x\n",
                              type);
            }
        }
        OpenUrmaUbHostD2HInDone(&s->interface, message);
    }
    return progress;
}

static uint64_t openurma_mmio_transaction(OpenUrmaUbHostState *s,
                                          hwaddr offset, unsigned size,
                                          uint64_t value, bool write)
{
    volatile union OpenUrmaUbHostH2DMessage *message = openurma_alloc_h2d(s);
    uint64_t request_id = ++s->next_request;
    gint64 deadline = g_get_monotonic_time() + OPENURMA_UB_HOST_MMIO_TIMEOUT_US;

    ++s->mmio_transactions;
    openurma_zero_message(&message->mmio, sizeof(message->mmio));
    message->mmio.request_id = request_id;
    message->mmio.offset = offset;
    message->mmio.value = value;
    message->mmio.length = size;
    message->mmio.byte_enable = size >= 16 ? UINT16_MAX : (1U << size) - 1;
    s->expected_request = request_id;
    s->completion_seen = false;
    OpenUrmaUbHostH2DOutSend(
        &s->interface, message,
        write ? OPENURMA_H2D_MMIO_WRITE : OPENURMA_H2D_MMIO_READ);

    while (!s->completion_seen) {
        openurma_service_device(s);
        if (g_get_monotonic_time() > deadline) {
            error_report("openurma-ub-host: MMIO request %" PRIu64 " timed out",
                         request_id);
            exit(EXIT_FAILURE);
        }
        g_thread_yield();
    }
    if (s->completion_status != 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "openurma-ub-host: device rejected MMIO request %" PRIu64
                      " status=%u\n", request_id, s->completion_status);
    }
    return s->completion_value;
}

static uint64_t openurma_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
    return openurma_mmio_transaction(opaque, offset, size, 0, false);
}

static void openurma_mmio_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    openurma_mmio_transaction(opaque, offset, size, value, true);
}

static const MemoryRegionOps openurma_mmio_ops = {
    .read = openurma_mmio_read,
    .write = openurma_mmio_write,
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

static void openurma_poll(void *opaque)
{
    OpenUrmaUbHostState *s = opaque;

    ++s->polls;
    openurma_service_device(s);
    timer_mod(s->poll_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->poll_ns);
}

static void openurma_realize(DeviceState *dev, Error **errp)
{
    OpenUrmaUbHostState *s = OPENURMA_UB_HOST(dev);
    struct SimbricksBaseIfParams params = { 0 };
    struct OpenUrmaUbHostIntro host_intro = {
        .version = OPENURMA_UB_HOST_VERSION,
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
        error_setg(errp, "openurma-ub-host requires a socket path");
        return;
    }
    openurma_ub_host_default_params(&params);
    params.sock_path = s->socket_path;
    params.sync_mode = kSimbricksBaseIfSyncDisabled;
    if (SimbricksBaseIfInit(&s->interface.base, &params) != 0 ||
        SimbricksBaseIfConnect(&s->interface.base) != 0 ||
        SimBricksBaseIfEstablish(&establish, 1) != 0) {
        error_setg(errp, "cannot connect UB-HOST socket %s", s->socket_path);
        return;
    }
    if (s->device_intro.version != OPENURMA_UB_HOST_VERSION) {
        error_setg(errp, "UB-HOST protocol version mismatch");
        SimbricksBaseIfClose(&s->interface.base);
        return;
    }
    s->poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, openurma_poll, s);
    timer_mod(s->poll_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->poll_ns);
    info_report("openurma-ub-host: connected %s, %u port(s)",
                s->socket_path, s->device_intro.port_count);
}

static void openurma_unrealize(DeviceState *dev)
{
    OpenUrmaUbHostState *s = OPENURMA_UB_HOST(dev);

    if (s->poll_timer) {
        timer_free(s->poll_timer);
        s->poll_timer = NULL;
    }
    if (s->interface.base.conn_state != 0) {
        SimbricksBaseIfClose(&s->interface.base);
    }
    info_report("[QEMU_UB_HOST_PROFILE] mmio=%" PRIu64 " dma=%" PRIu64
                " irq=%" PRIu64 " polls=%" PRIu64,
                s->mmio_transactions, s->dma_transactions,
                s->interrupts, s->polls);
}

static void openurma_instance_init(Object *obj)
{
    OpenUrmaUbHostState *s = OPENURMA_UB_HOST(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->poll_ns = OPENURMA_UB_HOST_DEFAULT_POLL_NS;
    memory_region_init_io(&s->mmio, obj, &openurma_mmio_ops, s,
                          TYPE_OPENURMA_UB_HOST, UINT64_C(0x1000000));
    sysbus_init_mmio(sbd, &s->mmio);
    for (unsigned i = 0; i < OPENURMA_UB_HOST_IRQS; ++i) {
        sysbus_init_irq(sbd, &s->irq[i]);
    }
}

static const Property openurma_properties[] = {
    DEFINE_PROP_STRING("socket", OpenUrmaUbHostState, socket_path),
    DEFINE_PROP_UINT64("mmio-base", OpenUrmaUbHostState, mmio_base,
                       UINT64_C(0x2d000000)),
    DEFINE_PROP_UINT64("mmio-size", OpenUrmaUbHostState, mmio_size,
                       UINT64_C(0x1000000)),
    DEFINE_PROP_UINT64("poll-ns", OpenUrmaUbHostState, poll_ns,
                       OPENURMA_UB_HOST_DEFAULT_POLL_NS),
};

static void openurma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = openurma_realize;
    dc->unrealize = openurma_unrealize;
    device_class_set_props(dc, openurma_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo openurma_info = {
    .name = TYPE_OPENURMA_UB_HOST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(OpenUrmaUbHostState),
    .instance_init = openurma_instance_init,
    .class_init = openurma_class_init,
};

static void openurma_register_types(void)
{
    type_register_static(&openurma_info);
}
type_init(openurma_register_types)

void openurma_ub_host_create(const char *socket_path, hwaddr mmio_base,
                             hwaddr mmio_size, DeviceState *gic,
                             unsigned irq_base)
{
    DeviceState *dev = qdev_new(TYPE_OPENURMA_UB_HOST);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    qdev_prop_set_string(dev, "socket", socket_path);
    qdev_prop_set_uint64(dev, "mmio-base", mmio_base);
    qdev_prop_set_uint64(dev, "mmio-size", mmio_size);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(), mmio_base,
                                        sysbus_mmio_get_region(sbd, 0), 10);
    for (unsigned i = 0; i < OPENURMA_UB_HOST_IRQS; ++i) {
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(gic, irq_base + i));
    }
}
