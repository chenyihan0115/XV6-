#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"
#include "proc.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;//指向内核页表的根目录指针

extern char etext[];  // kernel.ld sets this to end of kernel code.//定义了一个字节指针，指向内核代码部分的结束位置。.

extern char trampoline[]; // trampoline.S指向trampoline.S汇编代码文件


void kvm_map_pagetable(pagetable_t pgtbl) {
  
  // 映射UART0，大小为一个页面
  kvmmap(pgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // 映射VIRTIO disk，大小为一个页面
  kvmmap(pgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // CLINT
  // 本地中断控制器，用于配置定时器。在内核引导完成后不再需要。
  // 不需要映射到进程特定的内核页表。
  // 它也位于0x02000000处，低于PLIC的0x0c000000，
  // 并且会与进程内存发生冲突，进程内存位于地址空间的低端。

  // kvmmap(pgtbl, CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // 映射PLIC(Platform-Level Interrupt Controller), 大小为0x40000
  // 这个大小可以由0x10000000 - 0x0C000000计算得到
  kvmmap(pgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  // 映射内核代码到KERBASE位置，etext是我们上面已经介绍过的内核代码结尾标志
  // 用etext - KERBASE就是代码段长度
  kvmmap(pgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  // 将内核数据段和RAM直接映射过来
  // 使用PHYSTOP - etext就是这两段应该剩余的长度
  kvmmap(pgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  // 将trampline页面映射到内核虚拟地址空间的最高一个页面
  // TRAMPOLINE的定义如下，就是最高虚拟地址减去一个页面大小
  // #define TRAMPOLINE (MAXVA - PGSIZE)
  // 注意阅读上面的链接脚本时，我们将trampsec段放置在了内核代码后面
  // 那其实也是trampoline的开头，也就是说我们其实映射了trampoline页面两次
  kvmmap(pgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

pagetable_t
kvminit_newpgtbl()  //用于创建一个新的页表，用于内核的地址空间。
{
  pagetable_t pgtbl = (pagetable_t) kalloc();//分配一页新的页表空间
  memset(pgtbl, 0, PGSIZE);//初始化为0

  kvm_map_pagetable(pgtbl);//将该页表映射到内核的全局页表上。

  return pgtbl;//返回创建的新页表
}

/*
 * create a direct-map page table for the kernel.
 */
void
kvminit()
{
  //仍然需要有全局的内核页表，用于内核 boot 过程，以及无进程在运行时使用。
  kernel_pagetable = kvminit_newpgtbl();
  // CLINT *is* however required during kernel boot up and
  // we should map it for the global kernel pagetable
  //然后，调用 kvmmap() 函数将 CLINT（本地中断控制器）映射到内核的地址空间中。这是因为 CLINT 在内核引导过程中是必需的。(对于进程的创建是不需要这一步的，只是根页表需要这一步)
  kvmmap(kernel_pagetable, CLINT, CLINT, 0x10000, PTE_R | PTE_W);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // 将kvminit得到的内核页表根目录地址放入SATP寄存器，相当于打开了分页
  w_satp(MAKE_SATP(kernel_pagetable));
  // 清除快表(TLB):修改页表项必须重新修改TLB缓存页表项，避免TLb使用一个旧的缓冲映射，其指向的物理页可能已经分配给另一个进程。
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)// 如果虚拟地址超过了最大值，陷入错误
    panic("walk");

  // 模拟三级页表的查询过程，三级列表索引两次页表即可，最后一次直接组成物理地址
  for(int level = 2; level > 0; level--) {
    // 索引到对应的PTE项
    pte_t *pte = &pagetable[PX(level, va)];
    // 确认一下索引到的PTE项是否有效(valid位是否为1)
    if(*pte & PTE_V) {
      // 如果有效接着进行下一层索引
      //从PTE中提取出物理地址，直接赋值给pagetable指针(而它是一个虚拟地址）（只有在虚拟地址==物理地址时合理，即直接映射）
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      // 如果无效(说明对应页表没有分配)
      // 则根据alloc标志位决定是否需要申请新的页表
      // < 注意，当且仅当低两级页表页(中间级、叶子级页表页)不存在且不需要分配时，walk函数会返回0 >
      // 所以我们可以通过返回值来确定walk函数失败的原因
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0) {
        if(alloc && pagetable == 0) {
          //printf("trace: failed kalloc, freelist: %p\n", kget_freelist());
        }
        return 0;
      }
      // 将申请的页表填满0(alloc有效)
      memset(pagetable, 0, PGSIZE);
      // 将申请来的页表物理地址，转化为PTE并将有效位置1，记录在当前级页表
      // 这样在下一次访问时，就可以直接索引到这个页表项
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  // 如果虚拟地址大于最大虚拟地址，返回0
  // 物理地址为0的地方是未被使用的地址空间
  // Question: 为什么不像walk函数一样直接陷入panic？：
  if(va >= MAXVA)
    return 0;
    
  // 调用walk函数，直接在用户页表中找到最低一级的PTE
  pte = walk(pagetable, va, 0);
  // 如果此PTE不存在，或者无效，或者用户无权访问
  // 都统统返回0(为什么不陷入panic?)
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  //从PTE中截取下来物理地址页号字段，直接返回
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to a kernel page table. (lab3 enables standalone kernel page tables for each and every process)
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t pgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(pgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(pagetable_t kernelpgtbl, uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;

  pte = walk(kernelpgtbl, va, 0); // read from the process-specific kernel pagetable instead
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  // a存储的是当前虚拟地址对应的页
  // last存放的是最后一个应设置的页
  // 当 a==last时，表示a已经设置完了所有页，完成了所有任务
  uint64 a, last;
  pte_t *pte;

  // 当要映射的页面大小为0时，这是一个不合理的请求，陷入panic
  if(size == 0)
    panic("mappages: size");

  // a,last向下取整到页面开始位置，设置last相当于提前设置好了终点页
  // PGROUNDDOWN这个宏在后面会详细讲解
  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  // 开始迭代式地建立映射关系
  for(;;){
    // 调用walk函数，返回当前地址a对应的PTE
    // 如果返回空指针，说明walk没能有效建立新的页表页，这可能是内存耗尽导致的
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    // 如果找到了页表项，但是有效位已经被置位，表示这块物理内存已经被使用
    // 这说明原本的虚拟地址va根本不足以支撑分配size这么多的连续空间，陷入panic
    if(*pte & PTE_V)
      panic("mappages: remap");
    // 否则就可以安稳地设置PTE项，指向对应的物理内存页，并设置标志位permission
    *pte = PA2PTE(pa) | perm | PTE_V;
    // 设置完当前页之后看看是否到达设置的最后一页，是则跳出循环
    if(a == last)
      break;
    //否则设置下一页
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
// 译：从虚拟地址va开始移除npages个页面的映射关系
// va必须是页对齐的，映射必须存在
// 释放物理内存是可选的
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  // va不是页对齐的，陷入panic  
  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  // 通过遍历释放npages * PGSIZE大小的内存
  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    // 如果虚拟地址在索引过程中对应的中间页表页不存在，陷入panic
    // 回顾一下，walk函数返回0，只有一种情况，那就是某一级页表页在查询时发现不存在
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");

    // 查找成功，但发现此PTE不存在，陷入panic
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    
    // 查找成功，但发现此PTE除了valid位有效外，其他位均为0
    // 这暗示这个PTE原本不应该出现在叶级页表(奇怪的错误)，陷入panic
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    // 否则这是一个合法的，应该被释放的PTE
    // 如果do_free被置位，那么还要释放掉PTE对应的物理内存
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
     // 最后将PTE本身全部清空，成功解除了映射关系
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
//创建一个空的用户页表，当内存耗尽时返回空指针

pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
// 译：将用户的initcode加载到页表的0地址
// 仅为第一个进程而服务
// 代码的尺寸必须小于一个页(4096 bytes)
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  // mem虽然是一个指针，但是因为内核地址空间中虚拟地址和物理地址
  // 在RAM上是直接映射的，所以它其实也就等于物理地址
  char *mem;

  //分配的大小大于一个页面
  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  //分配一页物理内存作为initcode的存放处，memset用来将当前页清空
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  //在页表中加入一条虚拟地址0 <-> mem的映射，相当于将initcode成功映射到了虚拟地址0
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  //将initcode的代码一个字节一个字节地搬运到mem地址
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// 分配PTE和物理内存来将分配给用户的内存大小从oldsz提升到newsz
// oldsz和newsz不必是页对齐的
// 成功时返回新的内存大小，出错时返回0
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  //newz的内存小于oldz不用分配，返回旧的即可
  if(newsz < oldsz)
    return oldsz;
  //向上取整
  // 计算原先内存大小需要至少多少页，因为进程地址空间紧密排列
  // 所以这里oldsz指向的其实是原先已经使用内存的下一页，崭新的一页
  oldsz = PGROUNDUP(oldsz);
  //开始进行新内存的分配
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    // 如果mem为空指针，表示内存耗尽
    // 释放之前分配的所有内存，返回0表示出错
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    
    // 如果分配成功，则将新分配的页面全部清空
    memset(mem, 0, PGSIZE);
    // 并在当前页表项中建立起来到新分配页表的映射
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      // 如果mappages函数调用返回值不为0，表明在调用walk函数时索引到的PTE无效
      // 释放之前分配的所有内存，返回0表示出错
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  // 如果成功跳出循环，表示执行成功，返回新的内存空间大小
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
// 回收用户页，使得进程的内存大小从oldsz变为newsz。oldsz和newsz不一定要是
// 页对齐的，newsz也不一定要大于oldsz。oldsz可以比当前实际所占用的内存大小更大。
// 函数返回进程新占用的内存大小
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  // 如果新的内存大小比原先内存还要大，那么什么也不用做，直接返回oldsz即可
  if(newsz >= oldsz)
    return oldsz;
  
  // 如果newsz经过圆整后占据的页面数小于oldsz
  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    // 计算出来要释放的页面数量
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    // 调用uvmunmap，清空叶级页表的PTE并释放物理内存
    // 因为我们使用了PGROUNDUP来取整页面数量，所以这里可以保证va是页对齐的
    // 因为用户地址空间是从地址0开始紧密排布的， 所以PGROUNDUP(newsz)对应着新内存大小的结束位置(向上增加：内存（向高地址扩充）)
    // 注意do_free置为1，表示一并回收物理内存
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Just like uvmdealloc, but without freeing the memory.
// Used for syncing up kernel page-table's mapping of user memory.
uint64
kvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 0);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
//递归地释放页表页，所有的叶级别页表映射关系必须已经被解除
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  // 每一个页表都正好有512个页表项PTE，所以要遍历它们并尝试逐个释放
  for(int i = 0; i < 512; i++){
    // 取得对应的PTE
    pte_t pte = pagetable[i];
    // 注意，这里通过标志位的设置来判断是否到达了叶级页表
    // 如果有效位为1，且读位、写位、可执行位都是0
    // 说明这是一个高级别(非叶级)页表项，且此项未被释放，应该去递归地释放
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      //这个 PTE 指向一个较低级别的页表。
      uint64 child = PTE2PA(pte);
      // 去递归地释放下一级页表
      freewalk((pagetable_t)child);
      // 释放完毕之后，将原有的PTE全部清空，表示已经完全释放
      pagetable[i] = 0;
    // 如果有效位为1，且读位、写位、可执行位有一位为1
    // 表示这是一个叶级PTE，且未经释放，这不符合本函数调用条件，会陷入一个panic
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
    // 这里隐藏了一个逻辑，即if(pte & PTE_V == 0)
    // 这说明当前PTE已经被释放，不用再次释放了，直接遍历下一个PTE
  }
  // 最后释放页表本身占用的内存，回收，回到上一层递归
  kfree((void*)pagetable);
}

// Free a process-specific kernel page-table,
// without freeing the underlying physical memory
void
kvm_free_kernelpgtbl(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    uint64 child = PTE2PA(pte);
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      kvm_free_kernelpgtbl((pagetable_t)child);
      pagetable[i] = 0;
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
// 译：释放用户内存页
// 然后释放页表页
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  // 如果用户内存空间大小大于0，首先调用uvmunmap完全释放所有的叶级页表映射关系和物理页
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  // 然后再释放页表页
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
// 译：给定一个父进程页表，将其内存拷贝到子进程页表中
// 同时拷贝页表和对应的物理内存
// 返回0表示成功，-1表示失败
// 失败时会释放所有已经分配的内存
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  // sz指要复制的地址空间大小，被调用时传入p->sz，表示整个地址空间
  // 对整个地址空间逐页复制
  for(i = 0; i < sz; i += PGSIZE){
    //pte不存在
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    //存在但是有效位为0
    // PTE存在但是对应页未被使用，陷入panic
    // 再次强调，用户空间的内存使用是严格紧密的，中间不会有未使用的页存在
    // 自下而上：text、data、guard page、stack、heap
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    
    //获得对应的物理地址和标志位
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    //如果没有分配成功物理内存，跳转到错误处理代码
    if((mem = kalloc()) == 0)
      goto err;
    //将父进程对应的整个页面复制到新分配的页面中
    memmove(mem, (char*)pa, PGSIZE);
    // 在新的页表中建立映射关系
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      // 如果映射不成功，则释放掉分配的内存，并转入错误处理程序
      kfree(mem);
      goto err;
    }
  }
  // 成功时返回0
  return 0;
// 错误处理程序，解除所有已经分配的映射关系，并释放对应的物理内存，返回-1
 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// Copy some of the mappings from src into dst.
// Only copies the page table and not the physical memory.
// returns 0 on success, -1 on failure.
int
kvmcopymappings(pagetable_t src, pagetable_t dst, uint64 start, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;

  // PGROUNDUP: prevent re-mapping already mapped pages (eg. when doing growproc)
  for(i = PGROUNDUP(start); i < start + sz; i += PGSIZE){
    if((pte = walk(src, i, 0)) == 0)
      panic("kvmcopymappings: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("kvmcopymappings: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    // `& ~PTE_U` marks the page as kernel page and not user page.
    // Required or else kernel can not access these pages.
    if(mappages(dst, i, PGSIZE, pa, flags & ~PTE_U) != 0){
      goto err;
    }
  }

  return 0;

 err:
  uvmunmap(dst, PGROUNDUP(start), (i - PGROUNDUP(start)) / PGSIZE, 0);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
// 译：将一个PTE标记为用户不可访问的
// 用在exec函数中来进行用户栈守护页的设置
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  // 如果找不到va对应的PTE就陷入panic
  if(pte == 0)
    panic("uvmclear");
  // 找到了将对应PTE的User位清空
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  // 当字符还没有复制完毕时
  while(len > 0){
    //找到对应当前虚拟地址dstva的物理地址pa0
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);

    //如果pa0为0，表示查找过程出错，返回-1
    if(pa0 == 0)
      return -1;

    // 这里的逻辑和copyin一样，处理非对齐的情况和防止复制超出范围
    // n表示的是当前循环可以复制的字节数量
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    // 更新循环变量
	  // 强制对齐dstva，和copyin对齐srcva的做法一致，不再赘述
    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

int copyin_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len);
int copyinstr_new(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max);

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  // printf("trace: copyin1 %p\n", *walk(pagetable, srcva, 0));
  // printf("trace: copyin2 %p\n", *walk(myproc()->kernelpgtbl, srcva, 0));
  // printf("trace: copyin3 %p\n", *(uint64*)srcva);
  return copyin_new(pagetable, dst, srcva, len);
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  // printf("trace: copyinstr %p\n", walk(pagetable, srcva, 0));
  return copyinstr_new(pagetable, dst, srcva, max);
}

int pgtblprint(pagetable_t pagetable, int depth) {
    // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if(pte & PTE_V) {
      // print
      printf("..");
      for(int j=0;j<depth;j++) {
        printf(" ..");
      }
      printf("%d: pte %p pa %p\n", i, pte, PTE2PA(pte));

      // if not a leaf page table, recursively print out the child table
      if((pte & (PTE_R|PTE_W|PTE_X)) == 0){
        // this PTE points to a lower-level page table.
        uint64 child = PTE2PA(pte);
        pgtblprint((pagetable_t)child,depth+1);
      }
    }
  }
  return 0;
}

int vmprint(pagetable_t pagetable) {
  printf("page table %p\n", pagetable);
  return pgtblprint(pagetable, 0);
}