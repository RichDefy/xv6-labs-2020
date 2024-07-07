// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"
#define NBUCKET 13
#define BUFMAP_HASH(dev, blockno) ((((dev)<<27)|(blockno))%NBUCKET)

struct {
  struct spinlock lock;
  struct buf buf[NBUF];
  struct buf buckets[NBUCKET];
  struct spinlock buckets_lock[NBUCKET];
  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  // struct buf head;
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  // bcache.buckets[0].prev = &bcache.buckets[0];
  // bcache.buckets[0].next = &bcache.buckets[0];
  for(int i = 0; i < NBUCKET; i++){
    initlock(&bcache.buckets_lock[i],"buckets_lock");
    // bcache.buckets[i].prev = &bcache.buckets[i];
    bcache.buckets[i].next = 0;
  }
  
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.buckets[0].next;
    // b->prev = &bcache.buckets[0];
    b->timestamp = 0;
    b->refcnt = 0;
    initsleeplock(&b->lock, "buffer");
    // bcache.buckets[0].next->prev = b;
    bcache.buckets[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  
  uint key = BUFMAP_HASH(dev, blockno);
  // acquire(&bcache.lock);
  acquire(&bcache.buckets_lock[key]);

  // Is the block already cached?
  for(b = bcache.buckets[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      b->timestamp = ticks;
      release(&bcache.buckets_lock[key]);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bcache.buckets_lock[key]);
  acquire(&bcache.lock);
  for(b = bcache.buckets[key].next; b; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      acquire(&bcache.buckets_lock[key]);
      b->refcnt++;
      b->timestamp = ticks;
      release(&bcache.buckets_lock[key]);
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  
  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  struct buf *before_least = 0; 
  uint holding_bucket = -1;

  // 从当前散列桶开始查找
  for(int i = 0; i < NBUCKET; i = i + 1) {
    acquire(&bcache.buckets_lock[i]);
    int newfound = 0;
    for(b = &bcache.buckets[i]; b->next; b = b->next)
      // 使用时间戳进行LRU算法，而不是根据结点在链表中的位置
      if(b->next->refcnt == 0 && (before_least == 0 || b->next->timestamp < before_least->timestamp)){
        before_least = b;
        newfound = 1;
      }
    if(!newfound){
      release(&bcache.buckets_lock[i]);
    }
    else{
      if(holding_bucket != -1){
        release(&bcache.buckets_lock[holding_bucket]);
      }
      holding_bucket = i;
    }
  }
  if(!before_least) {
    release(&bcache.lock);
    panic("bget: no buffers");
  }
  b = before_least->next;
  // 如果是从其他散列桶窃取的，则将其以头插法插入到当前桶
  // printf("found, %d锁\n", key);
  if(holding_bucket != key) {
    before_least->next = b->next;
    release(&bcache.buckets_lock[holding_bucket]);
    acquire(&bcache.buckets_lock[key]);
    b->next = bcache.buckets[key].next;
    bcache.buckets[key].next = b;
  }
  b->dev = dev;
  b->blockno = blockno;
  b->valid = 0;
  b->refcnt = 1;
  b->timestamp = ticks;
  release(&bcache.buckets_lock[key]);
  release(&bcache.lock);
  acquiresleep(&b->lock);
  return b;
} 

  // struct buf *before_least = 0; 
  // uint holding_bucket = -1;
  // for(int i = 0; i < NBUCKET; i++){
  //   // before acquiring, we are either holding nothing, or only holding locks of
  //   // buckets that are *on the left side* of the current bucket
  //   // so no circular wait can ever happen here. (safe from deadlock)
  //   acquire(&bcache.buckets_lock[i]);
  //   int newfound = 0; // new least-recently-used buf found in this bucket
  //   for(b = &bcache.buckets[i]; b->next; b = b->next) {
  //     if(b->next->refcnt == 0 && (!before_least || b->next->timestamp < before_least->next->timestamp)) {
  //       before_least = b;
  //       newfound = 1;
  //     }
  //   }
  //   if(!newfound) {
  //     release(&bcache.buckets_lock[i]);
  //   } else {
  //     if(holding_bucket != -1) release(&bcache.buckets_lock[holding_bucket]);
  //     holding_bucket = i;
  //     // keep holding this bucket's lock....
  //   }
  // }
  // if(!before_least) {
  //   panic("bget: no buffers");
  // }
  // b = before_least->next;
  
  // if(holding_bucket != key) {
  //   // remove the buf from it's original bucket
  //   before_least->next = b->next;
  //   release(&bcache.buckets_lock[holding_bucket]);
  //   // rehash and add it to the target bucket
  //   acquire(&bcache.buckets_lock[key]);
  //   b->next = bcache.buckets[key].next;
  //   bcache.buckets[key].next = b;
  // }
  
  // b->dev = dev;
  // b->blockno = blockno;
  // b->refcnt = 1;
  // b->valid = 0;
  // release(&bcache.buckets_lock[key]);
  // release(&bcache.lock);
  // acquiresleep(&b->lock);
  // return b;
// }

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");
  uint key = BUFMAP_HASH(b->dev, b->blockno);
  releasesleep(&b->lock);

  acquire(&bcache.buckets_lock[key]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    // b->next->prev = b->prev;
    // b->prev->next = b->next;
    // b->next = bcache.buckets[key].next;
    // b->prev = &bcache.buckets[key];
    // bcache.buckets[key].next->prev = b;
    // bcache.buckets[key].next = b;
    b->timestamp = ticks;
  }
  release(&bcache.buckets_lock[key]);
}

void
bpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.buckets_lock[key]);
  b->refcnt++;
  release(&bcache.buckets_lock[key]);
}

void
bunpin(struct buf *b) {
  uint key = BUFMAP_HASH(b->dev, b->blockno);
  acquire(&bcache.buckets_lock[key]);
  b->refcnt--;
  release(&bcache.buckets_lock[key]);
}


