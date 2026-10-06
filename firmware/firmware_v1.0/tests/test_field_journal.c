/* Exercises the actual adapter with a persistent fake NVS and bounded queue.
 * Hardware flash/CRC/BT timing still require device validation. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "journal_stubs/fake_zephyr.h"
#include "../src/field_journal.c"

struct device fake_device;
static uint8_t disk[TOTAL_SLOTS+1][64];
static size_t lengths[TOTAL_SLOTS+1];
static int fail_write, fail_mount;
static jmp_buf drained;
static unsigned index_for(uint16_t key) { return key == BOOT_ID_KEY ? TOTAL_SLOTS : key - 0x100U; }
int nvs_mount(struct nvs_fs *f) { return fail_mount; }
ssize_t nvs_read(struct nvs_fs *f, uint16_t key, void *data, size_t size) {
    unsigned i=index_for(key); assert(i<=TOTAL_SLOTS);
    if (!lengths[i]) return -ENOENT;
    memcpy(data,disk[i],size); return lengths[i];
}
ssize_t nvs_write(struct nvs_fs *f, uint16_t key, const void *data, size_t size) {
    if (fail_write) return -EIO;
    unsigned i=index_for(key); assert(i<=TOTAL_SLOTS && size<=64);
    memcpy(disk[i],data,size); lengths[i]=size; return size;
}
int k_msgq_put(struct fake_queue *q, const void *r, int wait) {
    if (q->count==16) return -ENOMSG;
    memcpy(q->data[(q->head+q->count)%16],r,64); q->count++; return 0;
}
int k_msgq_get(struct fake_queue *q, void *r, int wait) {
    if (!q->count) longjmp(drained,1);
    memcpy(r,q->data[q->head],64); q->head=(q->head+1)%16; q->count--; return 0;
}
static void drain(void) { if (!setjmp(drained)) journal_worker(NULL,NULL,NULL); }
static void reset_ram(void) {
    memset(records,0,sizeof(records)); memset(frozen,0,sizeof(frozen));
    memset(&journal_queue,0,sizeof(journal_queue)); memset(error_seen,0,sizeof(error_seen));
    memset(last_error,0,sizeof(last_error)); memset(last_error_ms,0,sizeof(last_error_ms));
    sequence=boot_id=fault_next=checkpoint_next=dropped=write_failures=0;
    initialized=false; storage_status=-EAGAIN;
}
static struct stream_health health;
static void event(uint32_t now, uint8_t type) { field_journal_capture(&health,now,type,3,-12); drain(); }

int main(void) {
    reset_ram(); field_journal_init(0); assert(initialized && boot_id==1 && fs.sector_count==8);
    event(20,JOURNAL_BOOT); event(6000,STREAM_STALLED);
    assert(sequence==2);
    /* Simulated complete RAM loss; the same fault and boot ID are recovered. */
    reset_ram(); field_journal_init(2);
    assert(boot_id==2 && sequence==2 && fault_next==2);
    assert(records[1][12]==STREAM_STALLED && sys_get_le32(records[1]+8)==6000);
    event(25,JOURNAL_BOOT);
    /* Hundreds of healthy writes never replace fault records. */
    for (unsigned i=0;i<200;i++) event(300000U*(i+1),JOURNAL_CHECKPOINT);
    assert(records[1][12]==STREAM_STALLED && checkpoint_next==8);
    /* Identical error storms produce one record per hour and keep counts. */
    unsigned before=sequence;
    for (unsigned i=0;i<10000;i++) event(i,STREAM_ERROR);
    assert(sequence==before+1);
    event(3600000U,STREAM_ERROR); assert(sequence==before+2);
    /* Full queue is bounded and dropped records are explicitly counted. */
    for (unsigned i=0;i<100;i++) field_journal_capture(&health,i,STREAM_CONNECTED,255,0);
    assert(journal_queue.count==16 && dropped==84); drain();
    /* Failed writes don't replace a valid record or advance its sequence. */
    before=sequence; fail_write=1; event(7000,STREAM_STALLED);
    assert(sequence==before && write_failures==1 && storage_status==-EIO);
    fail_write=0; event(8000,STREAM_RESUMED); assert(sequence==before+1 && storage_status==0);
    /* Fault history wraps within its own fixed capacity. */
    for (unsigned i=0;i<400;i++) event(10000+i,STREAM_SUBSCRIPTION);
    const uint8_t *wire=field_journal_snapshot();
    assert(!memcmp(wire,"LJ01",4) && sys_get_le32(wire+16)==sequence);
    assert(sys_get_le32(wire+20)==84 && sys_get_le32(wire+24)==1);
    unsigned count=0; for(unsigned i=0;i<JOURNAL_FAULT_SLOTS;i++) if(sys_get_le32(records[i])) count++;
    assert(count==256);
    reset_ram(); fail_mount=-EIO; field_journal_init(0);
    assert(!initialized && storage_status==-EIO); event(1,STREAM_STALLED); assert(!journal_queue.count);
    puts("PASS: reboot retention, separate checkpoint ring, throttling, queue cap, failed writes, rollover, mount failure");
}
