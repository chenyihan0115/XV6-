// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

//内核之后的第一个地址，由kernel.ld定义
extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

// run结构体就是一个指向自身的指针，用于指向下一个空闲页表开始位置
struct run {
  struct run *next;
};

// 管理物理内存的结构
// 有一把锁lock保证访问时的互斥性
// 以及一个指向
struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");

  // 释放从end到PHYSTOP之间的所有物理内存
  // 回收进空闲链表freelist中
  freerange(end, (void*)PHYSTOP);
  printf("PHYSTOP is %p\n", PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  //向上对齐，防止释放当前页pa_start以下的页面
  p = (char*)PGROUNDUP((uint64)pa_start);
  
  // 逐页释放到终点页面，注意终止条件p + PGSIZE <= pa_end
  // 这本质上也加入了保护措施，防止释放有用页
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// 译：释放pa指向的页的物理内存，它通常是由调用kalloc返回的
// 特殊情况是初始化分配器时，见上面的kinit函数
void
kfree(void *pa)
{
  struct run *r;

  // 如果要释放的内存不是页对齐的，或者不在自由内存范围内，陷入panic
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  // 将要回收的这一页填满无用的数据
  // 这一步主要是为了防止在本页内存释放之后仍有进程尝试访问之，无用数据会导致进程快速崩溃
  // 这在xv6 book中有所解释
  memset(pa, 1, PGSIZE);

  //将pa强制转型为run类型的指针，准备回收到链表中
  r = (struct run*)pa;

  // 头插法，将回收的页作为链表第一项插入到空闲链表中
  // 注意使用锁机制来保持动作的安全性
  acquire(&kmem.lock);//獲取鎖，在锁里操作
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  // 取下链表头部的第一个节点，即第一个空闲页
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;//移除空闲链表
  release(&kmem.lock);
  
  // 如果r不为空，表示成功分配到了内存
  // 将其填满随机数据后返回
  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// used for tracing purposes in exp2
// void *kget_freelist(void) { return kmem.freelist; } 