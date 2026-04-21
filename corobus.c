#include "corobus.h"
#include "libcoro.h"
#include "rlist.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Вспомогательная функция для тестов (обработка флага --max-points)
// ============================================================================

int doCmdMaxPoints(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--max-points") == 0 ||
            strcmp(argv[i], "-m") == 0) {
            return 1;
        }
    }
    return 0;
}
// ============================================================================
// Структуры данных
// ============================================================================

struct data_vector {
    unsigned *data;
    size_t size;
    size_t capacity;
};

struct wakeup_entry {
    struct rlist base;
    struct coro *coro;
};

struct wakeup_queue {
    struct rlist coros;
};

struct coro_bus_channel {
    size_t size_limit;
    struct wakeup_queue send_queue;
    struct wakeup_queue recv_queue;
    struct data_vector data;
};

struct coro_bus {
    struct coro_bus_channel **channels;
    int channel_count;
};

// ============================================================================
// Работа с очередью сообщений
// ============================================================================

static void data_vector_append_many(struct data_vector *vector,
    const unsigned *data, size_t count)
{
    if (vector->size + count > vector->capacity) {
        if (vector->capacity == 0)
            vector->capacity = 4;
        else
            vector->capacity *= 2;
        if (vector->capacity < vector->size + count)
            vector->capacity = vector->size + count;
        vector->data = realloc(vector->data,
            sizeof(vector->data[0]) * vector->capacity);
    }
    memcpy(&vector->data[vector->size], data, sizeof(data[0]) * count);
    vector->size += count;
}

static void data_vector_append(struct data_vector *vector, unsigned data)
{
    data_vector_append_many(vector, &data, 1);
}

static void data_vector_pop_first_many(struct data_vector *vector,
    unsigned *data, size_t count)
{
    assert(count <= vector->size);
    memcpy(data, vector->data, sizeof(data[0]) * count);
    vector->size -= count;
    memmove(vector->data, &vector->data[count],
        vector->size * sizeof(vector->data[0]));
}

static unsigned data_vector_pop_first(struct data_vector *vector)
{
    unsigned data = 0;
    data_vector_pop_first_many(vector, &data, 1);
    return data;
}

// ============================================================================
// Работа с очередями ожидания
// ============================================================================

static void wakeup_queue_suspend_this(struct wakeup_queue *queue)
{
    struct wakeup_entry *entry = malloc(sizeof(*entry));
    entry->coro = coro_this();
    rlist_add_tail_entry(&queue->coros, entry, base);
    coro_suspend();
    rlist_del_entry(entry, base);
    free(entry);
}

static void wakeup_queue_wakeup_first(struct wakeup_queue *queue)
{
    if (rlist_empty(&queue->coros))
        return;
    struct wakeup_entry *entry = rlist_first_entry(&queue->coros,
        struct wakeup_entry, base);
    coro_wakeup(entry->coro);
}

// ============================================================================
// Глобальная ошибка
// ============================================================================

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code coro_bus_errno(void)
{
    return global_error;
}

void coro_bus_errno_set(enum coro_bus_error_code err)
{
    global_error = err;
}

// ============================================================================
// Создание и удаление шины
// ============================================================================

struct coro_bus *coro_bus_new(void)
{
    struct coro_bus *bus = malloc(sizeof(*bus));
    bus->channels = NULL;
    bus->channel_count = 0;
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return bus;
}

void coro_bus_delete(struct coro_bus *bus)
{
    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] != NULL) {
            coro_bus_channel_close(bus, i);
        }
    }
    free(bus->channels);
    free(bus);
}

// ============================================================================
// Управление каналами
// ============================================================================

int coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{
    int slot = -1;
    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] == NULL) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        slot = bus->channel_count;
        bus->channel_count++;
        bus->channels = realloc(bus->channels,
            bus->channel_count * sizeof(struct coro_bus_channel*));
    }
    
    struct coro_bus_channel *ch = malloc(sizeof(*ch));
    ch->size_limit = size_limit;
    ch->data.data = malloc(sizeof(unsigned) * size_limit);
    ch->data.size = 0;
    ch->data.capacity = size_limit;
    rlist_create(&ch->send_queue.coros);
    rlist_create(&ch->recv_queue.coros);
    
    bus->channels[slot] = ch;
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return slot;
}

void coro_bus_channel_close(struct coro_bus *bus, int channel)
{
    if (!bus || channel < 0 || channel >= bus->channel_count)
        return;
    
    struct coro_bus_channel *ch = bus->channels[channel];
    if (!ch)
        return;
    
    while (!rlist_empty(&ch->send_queue.coros)) {
        struct wakeup_entry *entry = rlist_first_entry(
            &ch->send_queue.coros, struct wakeup_entry, base);
        rlist_del(&entry->base);
        coro_wakeup(entry->coro);
    }
    
    while (!rlist_empty(&ch->recv_queue.coros)) {
        struct wakeup_entry *entry = rlist_first_entry(
            &ch->recv_queue.coros, struct wakeup_entry, base);
        rlist_del(&entry->base);
        coro_wakeup(entry->coro);
    }
    
    free(ch->data.data);
    free(ch);
    bus->channels[channel] = NULL;
}

// ============================================================================
// Отправка и получение
// ============================================================================

int coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{
    if (channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }
    
    struct coro_bus_channel *ch = bus->channels[channel];
    
    if (ch->data.size >= ch->size_limit) {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }
    
    data_vector_append(&ch->data, data);
    wakeup_queue_wakeup_first(&ch->recv_queue);
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

int coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{
    while (1) {
        int rc = coro_bus_try_send(bus, channel, data);
        if (rc == 0)
            return 0;
        
        if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK)
            return -1;
        
        if (channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }
        
        struct coro_bus_channel *ch = bus->channels[channel];
        wakeup_queue_suspend_this(&ch->send_queue);
        
        if (bus->channels[channel] == NULL) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }
    }
}

int coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data)
{
    if (channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }
    
    struct coro_bus_channel *ch = bus->channels[channel];
    
    if (ch->data.size == 0) {
        coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        return -1;
    }
    
    *data = data_vector_pop_first(&ch->data);
    wakeup_queue_wakeup_first(&ch->send_queue);
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

int coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{
    while (1) {
        int rc = coro_bus_try_recv(bus, channel, data);
        if (rc == 0)
            return 0;
        
        if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK)
            return -1;
        
        if (channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }
        
        struct coro_bus_channel *ch = bus->channels[channel];
        wakeup_queue_suspend_this(&ch->recv_queue);
        
        if (bus->channels[channel] == NULL) {
            coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            return -1;
        }
    }
}

// ============================================================================
// BROADCAST - ОТПРАВКА ВО ВСЕ КАНАЛЫ
// ============================================================================

#if NEED_BROADCAST

int coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
    // 1. Проверяем, есть ли вообще каналы
    int has_channels = 0;
    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] != NULL) {
            has_channels = 1;
            break;
        }
    }
    
    if (!has_channels) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }
    
    // 2. Бесконечный цикл - ждём, пока ВСЕ каналы не освободятся
    while (1) {
        int all_have_space = 1;
        int blocked_channel = -1;
        
        // 3. Проверяем ВСЕ каналы (НЕ отправляем!)
        for (int i = 0; i < bus->channel_count; i++) {
            struct coro_bus_channel *ch = bus->channels[i];
            if (ch == NULL) continue;
            
            if (ch->data.size >= ch->size_limit) {
                all_have_space = 0;
                blocked_channel = i;
                break;
            }
        }
        
        // 4. Если все каналы имеют место - отправляем!
        if (all_have_space) {
            for (int i = 0; i < bus->channel_count; i++) {
                struct coro_bus_channel *ch = bus->channels[i];
                if (ch == NULL) continue;
                
                data_vector_append(&ch->data, data);
                wakeup_queue_wakeup_first(&ch->recv_queue);
            }
            coro_bus_errno_set(CORO_BUS_ERR_NONE);
            return 0;
        }
        
        // 5. Есть полный канал - засыпаем в его очереди
        if (blocked_channel != -1) {
            struct coro_bus_channel *ch = bus->channels[blocked_channel];
            if (ch == NULL) continue;
            
            wakeup_queue_suspend_this(&ch->send_queue);
            // Проснулись - проверяем все каналы заново
        }
    }
}

int coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
    // 1. Проверяем, есть ли вообще каналы
    int has_channels = 0;
    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] != NULL) {
            has_channels = 1;
            break;
        }
    }
    
    if (!has_channels) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }
    
    // 2. Проверяем ВСЕ каналы - если хоть один полон, сразу ошибка
    for (int i = 0; i < bus->channel_count; i++) {
        struct coro_bus_channel *ch = bus->channels[i];
        if (ch == NULL) continue;
        
        if (ch->data.size >= ch->size_limit) {
            coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
            return -1;
        }
    }
    
    // 3. Все каналы имеют место - отправляем!
    for (int i = 0; i < bus->channel_count; i++) {
        struct coro_bus_channel *ch = bus->channels[i];
        if (ch == NULL) continue;
        
        data_vector_append(&ch->data, data);
        wakeup_queue_wakeup_first(&ch->recv_queue);
    }
    
    coro_bus_errno_set(CORO_BUS_ERR_NONE);
    return 0;
}

#endif /* NEED_BROADCAST */

