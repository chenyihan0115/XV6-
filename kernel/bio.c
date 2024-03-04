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
//buffer缓存层

#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;//这是一个自旋锁，用于在多线程环境中对缓存进行并发控制。
  struct buf buf[NBUF];//这是一个数组，用于存储缓冲区（buf）的实例。数组大小为NBUF，表示缓冲区的数量

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;//这是一个buf类型的结构体，用于构建一个链表，表示所有缓冲区的链表。：链表头部是最近使用的缓冲区，尾部是最早使用的，buffer链表就按使用时间顺序来排序。
} bcache;//缓存的结构体：整个缓存系统的状态

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");//初始化了缓存锁，这是一个用于对整个缓存系统进行并发访问控制的自旋锁。

  // Create linked list of buffers
  //初始化了一个空的双向链表头 bcache.head，它的 prev 和 next 指向自身。
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;

  //遍历缓冲区数组 bcache.buf
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    // 将当前缓冲区插入到链表头部。
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    //对缓冲区的睡眠锁进行初始化。
    initsleeplock(&b->lock, "buffer");
    //更新链表头的prev和next指针，确保链表的正确性
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }

}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
//扫描buffer链表，通过给定设备号和扇区号来寻找缓冲区:如果存在，就返回被锁定的buffer，如果给定没有buffer，必须生成一个
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  acquire(&bcache.lock);//获取扇区号的锁  获取缓存锁

  // 判断块是否已经在缓存中
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;//指向缓冲器的计数加一
      release(&bcache.lock); //释放缓存锁
      acquiresleep(&b->lock);//获取睡眠锁
      return b; // 返回找到的缓冲区
    }
  }

  //如果块不在缓存中，则需要分配一个新的缓冲区
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {//找到未被引用的缓存区
      b->dev = dev;  
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);//sleep-lock保护的是块的缓冲内容的读写，lock保护的是被缓存的块的信息
      return b;
    }
  }
  panic("bget: no buffers");//如果所有buffer都在使用，bget就会panic
}

// Return a locked buf with the contents of the indicated block.
//读取数据
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);// 通过bget函数获取指定设备和块号的缓冲区
  if(!b->valid) {// 如果缓冲区中的数据无效
    virtio_disk_rw(b, 0);//通过virtio_disk_rw函数从磁盘读取数据到缓冲区
    b->valid = 1;//标记数据有效
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
//将修改后的buffer写入磁盘对应的块，且必须被锁住确保每次只有一个线程使用buffer
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))//使用holdingsleep函数检查当前线程是否持有缓冲区的睡眠锁:否则该线程不允许进行写操作
    panic("bwrite");
  virtio_disk_rw(b, 1);//写入磁盘
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
//释放锁sleep,
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);//确保操作是在临界区里的，以确保对缓存的并发访问是受控制的。
  b->refcnt--;
  if (b->refcnt == 0) {//检查缓冲区的引用计数是否为零，如果是则表示没有线程在等待这个缓冲区。
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;//从LRU链表移除该缓冲区b

    b->next = bcache.head.next;//将b缓冲区插入到LRU链表的头部，表示最近被使用过
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;

  }
  
  release(&bcache.lock);
}

//增加和减少缓冲区引用计数的函数
void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


