#ifndef FAKE_ZEPHYR_H
#define FAKE_ZEPHYR_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#define BUILD_ASSERT(x) _Static_assert(x, #x)
#define ARG_UNUSED(x) (void)(x)
#define K_FOREVER -1
#define K_NO_WAIT 0
#define CONFIG_LOG_DEFAULT_LEVEL 3
#define LOG_MODULE_REGISTER(...)
#define LOG_INF(...)
#define LOG_ERR(...)
#define K_THREAD_DEFINE(...)
struct device { int unused; };
struct flash_pages_info { size_t size; };
struct nvs_fs { const struct device *flash_device; off_t offset; uint16_t sector_size, sector_count; };
struct fake_lock { int unused; };
#define K_MUTEX_DEFINE(name) struct fake_lock name
#define K_SEM_DEFINE(name, initial, max) struct fake_lock name
struct fake_queue { uint8_t data[16][64]; unsigned head, count; };
#define K_MSGQ_DEFINE(name, size, count, align) struct fake_queue name
extern struct device fake_device;
#define FIXED_PARTITION_OFFSET(x) 0xec000U
#define FIXED_PARTITION_SIZE(x) 0x8000U
#define FIXED_PARTITION_DEVICE(x) (&fake_device)
static inline bool device_is_ready(const struct device *d) { return true; }
static inline int flash_get_page_info_by_offs(const struct device *d, off_t o, struct flash_pages_info *i) { i->size = 4096; return 0; }
static inline void k_mutex_lock(struct fake_lock *l, int t) {}
static inline void k_mutex_unlock(struct fake_lock *l) {}
static inline void k_sem_take(struct fake_lock *l, int t) {}
static inline void k_sem_give(struct fake_lock *l) {}
int k_msgq_get(struct fake_queue *q, void *r, int wait);
int k_msgq_put(struct fake_queue *q, const void *r, int wait);
int nvs_mount(struct nvs_fs *f);
ssize_t nvs_read(struct nvs_fs *f, uint16_t key, void *data, size_t size);
ssize_t nvs_write(struct nvs_fs *f, uint16_t key, const void *data, size_t size);
static inline void sys_put_le16(uint16_t v, uint8_t *p) { p[0]=v; p[1]=v>>8; }
static inline void sys_put_le32(uint32_t v, uint8_t *p) { for (int i=0;i<4;i++) p[i]=v>>(8*i); }
static inline uint32_t sys_get_le32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
#endif
