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
// my add
#define NBUCKET 13

struct
{
  struct spinlock lock;//大锁，用来防止死锁
  // my add
  struct spinlock bucket_lock[NBUCKET]; // 对应桶的锁
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head[NBUCKET];//给缓冲区分散成13个桶
} bcache;

int hash(int num)
{
  return num % NBUCKET;
}

void binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  // bcache.head.prev = &bcache.head;
  // bcache.head.next = &bcache.head;
  for (int i = 0; i < NBUCKET; ++i)
  {
    bcache.head[i].prev = &bcache.head[i];
    bcache.head[i].next = &bcache.head[i];
    initlock(&bcache.bucket_lock[i], "bucket_lock");
  }
  // 可以先把全部缓存都插入到第一个桶
  for (b = bcache.buf; b < bcache.buf + NBUF; b++)
  {
    b->next = bcache.head[0].next;
    b->prev = &bcache.head[0];
    initsleeplock(&b->lock, "buffer");
    bcache.head[0].next->prev = b;
    bcache.head[0].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;
  // my add
  struct buf *get_buf = 0;
  int hashnum = hash(blockno);

  acquire(&bcache.bucket_lock[hashnum]);
  // 先找当前桶里面的
  //  Is the block already cached?
  for (b = bcache.head[hashnum].next; b != &bcache.head[hashnum]; b = b->next)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      b->refcnt++;
      release(&bcache.bucket_lock[hashnum]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  release(&bcache.bucket_lock[hashnum]);
  // 当前的桶没有，先利用ticks找LRU，仍然没找到的话，再去其他桶借一块缓冲区
  //  Not cached.
  //  Recycle the least recently used (LRU) unused buffer.
  // 先大锁，后小锁，防止死锁
  acquire(&bcache.lock);
  acquire(&bcache.bucket_lock[hashnum]);

  //重新检查一遍当前桶，放置在释放锁和获取锁的间隙有变动
  // for (b = bcache.head[hashnum].next; b != &bcache.head[hashnum]; b = b->next)
  // {
  //   if (b->dev == dev && b->blockno == blockno)
  //   {
  //     b->refcnt++;
  //     release(&bcache.bucket_lock[hashnum]);
  //     release(&bcache.lock);
  //     acquiresleep(&b->lock);
  //     return b;
  //   }
  // }

  int mintick = 0; // 用来记录找到最小的ticks
  for (b = bcache.head[hashnum].next; b != &bcache.head[hashnum]; b = b->next)
  {
    if (b->refcnt == 0 && (!get_buf || mintick > b->lastuse))
    {
      get_buf = b;
      mintick = b->lastuse;
    }
  }
  if (get_buf)
  {
    get_buf->dev = dev;
    get_buf->blockno = blockno;
    get_buf->valid = 0;
    get_buf->refcnt ++;
    release(&bcache.bucket_lock[hashnum]);
    release(&bcache.lock);
    acquiresleep(&get_buf->lock);
    return get_buf;
  }
  //release(&bcache.bucket_lock[hashnum]);   因为从其他的桶里面获取锁需要添加到当前的桶，所以先不可以释放锁
  
  // 还没找到，就去其他桶借
  mintick = 0;
  for (int i = 0; i < NBUCKET; ++i)
  {
    if (i == hashnum)
      continue;
    acquire(&bcache.bucket_lock[i]);
    struct buf *b;
    for (b = bcache.head[i].next; b != &bcache.head[i]; b = b->next)
    {
      if (b->refcnt == 0 && (!get_buf || mintick > b->lastuse))
      {
        get_buf = b;
        mintick = b->lastuse;
      }
    }
    if (get_buf)
    {
      get_buf->dev = dev;
      get_buf->blockno = blockno;
      get_buf->valid = 0;
      get_buf->refcnt ++;
      //将这个桶里面的缓冲区放到原来的桶里面
      //从原来桶删除
      get_buf->next->prev=get_buf->prev;
      get_buf->prev->next=get_buf->next;
      release(&bcache.bucket_lock[i]);
      //添加到当前桶
      get_buf->next=bcache.head[hashnum].next;
      get_buf->prev=&bcache.head[hashnum];
      bcache.head[hashnum].next->prev=get_buf;
      bcache.head[hashnum].next=get_buf;
      release(&bcache.bucket_lock[hashnum]);
      release(&bcache.lock);
      acquiresleep(&get_buf->lock);
      return get_buf;
    }
    release(&bcache.bucket_lock[i]);
  }
  release(&bcache.bucket_lock[hashnum]);
  release(&bcache.lock);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf *
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if (!b->valid)
  {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void brelse(struct buf *b)
{
  if (!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  // acquire(&bcache.lock);
  int blocknum = hash(b->blockno);
  acquire(&bcache.bucket_lock[blocknum]); // 把大锁换掉
  b->refcnt--;
  if (b->refcnt == 0)
  {
    // no one is waiting for it.
    // b->next->prev = b->prev;
    // b->prev->next = b->next;
    // b->next = bcache.head.next;
    // b->prev = &bcache.head;
    // bcache.head.next->prev = b;
    // bcache.head.next = b;
    b->lastuse = ticks;
  }
  release(&bcache.bucket_lock[blocknum]);
  // release(&bcache.lock);
}

void bpin(struct buf *b)
{
  int hashnum = hash(b->blockno);
  acquire(&bcache.bucket_lock[hashnum]);
  b->refcnt++;
  release(&bcache.bucket_lock[hashnum]);
}

void bunpin(struct buf *b)
{
  int hashnum = hash(b->blockno);
  acquire(&bcache.bucket_lock[hashnum]);
  b->refcnt--;
  release(&bcache.bucket_lock[hashnum]);
}
