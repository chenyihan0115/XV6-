#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t *pgdir, uint64 addr, struct inode *ip, uint offset, uint sz);
//创建用户地址空间
int
exec(char *path, char **argv)
{
  // 一系列需要使用的变量声明
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG+1], stackbase;
  struct elfhdr elf;
  struct inode *ip;
  struct proghdr ph;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc(); //当前进程

  // begin_op是开启文件系统的日志功能
  // 每当进行一个与文件系统相关的系统调用时都要记录
  begin_op();
  
  // namei同样是一个文件系统操作，它返回对应路径文件的索引节点(index node)的内存拷贝
  // 索引节点中记录着文件的一些元数据(meta data)
  // 如果出错就使用end_op结束当前调用的日志功能
  if((ip = namei(path)) == 0){
    end_op();
    return -1;
  }

  //给索引节点加锁，防止访问冲突
  ilock(ip);

  // Check ELF header
  // 读取ELF文件头，查看文件头部的魔数(MAGIC NUMBER)是否符合要求，这在xv6中有详细说明
  // 如果不符合就跳转到错误处理程序
  if(readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  // 创建一个用户页表，将trampoline页面和trapframe页面映射进去
  // 保持用户内存部分留白
  if((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  // Load program into memory.
  // 译：将程序加载到内存中去
  // elf.phoff字段指向program header的开始地址，program header通常紧随elf header之后
  // elf.phnum字段表示program header的个数，在xv6中只有一条program header
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    // 读取对应的program header， 出错则转入错误处理程序
    if(readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    // 如果不是LOAD段，则读取下一段，xv6中只定义了LOAD这一种类型的program header(kernel/elf.h)
    // LOAD意为可载入内存的程序段
    if(ph.type != ELF_PROG_LOAD)
      continue;

    // memsz：在内存中的段大小(以字节计)
    // filesz：文件镜像大小
    // 一般来说，filesz <= memsz，中间的差值使用0来填充
    // memsz < filesz就是一种异常的情况，会跳转到错误处理程序
    if(ph.memsz < ph.filesz)
      goto bad;

    // 安全检测，防止当前程序段载入之后地址溢出 
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;

    // 尝试为当前程序段分配地址空间并建立映射关系
    // 这里正好满足了loadseg要求的映射关系建立的要求 
    // uvmalloc函数见完全解析系列博客(2)
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    // limit added by exp3, program should not go over PLIC or else
    // kernel page-table's mapping of the program would not fit.
    if(sz1 >= PLIC) {
      goto bad;
    }

    // 更新sz大小，sz记录着当前已复制的地址空间的大小
    sz = sz1;

    // 如果ph.vaddr不是页对齐的，则跳转到出错程序
	  // 这也是为了呼应loadseg函数va必须对齐的要求
    if(ph.vaddr % PGSIZE != 0)
      goto bad;

    // 调用loadseg函数将程序段读入前面已经分配好的页面
    // 如读取不成功则跳转到错误处理程序
    if(loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
      goto bad;
  }

  // iunlockput函数实际上将iunlock和iput函数结合了起来
  // iunlock是释放ip的锁，和前面的ilock对应
  // iput是在索引节点引用减少时尝试回收节点的函数
  iunlockput(ip);

  // 结束日志操作，和前面的begin_op对应
  end_op();

  // 将索引节点置为空指针
  ip = 0;

  // 获取当前进程并读取出原先进程占用内存大小
  // myproc这个函数定义在kernel/proc.c中
  // 有关进程也是一个很大的话题，需要仔细研究
  p = myproc();
  uint64 oldsz = p->sz;

  // Allocate two pages at the next page boundary.
  // Use the second as the user stack.
  // 译：在当前页之后再分配两页内存
  // 并使用第二页作为用户栈
  // 这和xv6 book中展示的用户地址空间是完全一致的，可以参考一下
  // 在text、data段之后是一页guard page，然后是user stack
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz, sz + 2*PGSIZE)) == 0)
    goto bad;
  sz = sz1;

  // 清除守护页的用户权限 
  uvmclear(pagetable, sz-2*PGSIZE);

  // sz当前的位置就是栈指针stack pointer的位置，即栈顶
  // stackbase是栈底位置，即栈顶位置减去一个页面
  sp = sz;
  stackbase = sp - PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  // 在用户栈中压入参数字符串
  // 准备剩下的栈空间在ustack变量中
  // 读取exec函数传递进来的参数列表，直至遇到结束符
  for(argc = 0; argv[argc]; argc++) {
    // 传入的参数超过上限，则转入错误处理程序
    if(argc >= MAXARG)
      goto bad;
      
    // 栈顶指针下移，给存放的参数留下足够空间
    // 多下移一个字节是为了存放结束符
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned 对其sp指针

    // 如果超过了栈底指针，表示栈溢出了
    if(sp < stackbase)
      goto bad;

    // 使用copyout函数将参数从内核态拷贝到用户页表的对应位置
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    
    // 将参数的地址放置在ustack变量的对应位置
    // 注意：ustack数组存放的是函数参数的地址(虚拟地址)
    ustack[argc] = sp;
  }
  // 在ustack数组的结尾存放一个空字符串，表示结束
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  // 将参数的地址数组放入用户栈中，即将ustack数组拷贝到用户地址空间中
  // argc个参数加一个空字符串，一共是argc + 1个参数
  // 对齐指针到16字节并检测是否越界
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;

  // 从内核态拷贝ustack到用户地址空间的对应位置
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in a0.
  // 译：用户main程序的参数
  // argc作为返回值返回，存放在a0中，稍后我们就会看到这个调用的返回值就是argc
  // sp作为argv，即指向参数0的指针的指针，存放在a1中返回
  // < 疑问：按照xv6 book所述，这里的栈里应该还有argv和argc的存放 >
  // < 这个地方留个坑，等研究完系统调用和陷阱的全流程之后再来解释 >
  p->trapframe->a1 = sp;

  // Save program name for debugging.
  // 译：保留程序名，方便debug
  // 将程序名截取下来放到进程p的name中去
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));


  // synchronize kernel page-table's mapping of user memory
  //同步内核页表对用户内存的映射
  uvmunmap(p->kernelpgtbl, 0, PGROUNDUP(oldsz)/PGSIZE, 0);
  kvmcopymappings(pagetable, p->kernelpgtbl, 0, sz);
  

  // TODO: unmap old program mapping [0,oldsz] in kernel page table    
  // TODO: map [0,sz] in the new user page-table to kernel page-table

  // Commit to the user image.
  // 译：提交用户镜像
  // 在此，首先将用户的原始地址空间保存下来，以备后面释放
  // 然后将新的地址空间全部设置好
  // epc设置为elf.entry，elf.entry这个地址里存放的是进程的开始执行地址
  // sp设置为当前栈顶位置sp
  oldpagetable = p->pagetable;
  p->pagetable = pagetable;
  p->sz = sz;
  p->trapframe->epc = elf.entry;  // initial program counter = main
  p->trapframe->sp = sp; // initial stack pointer
  proc_freepagetable(oldpagetable, oldsz);


  vmprint(p->pagetable);
  return argc; // this ends up in a0, the first argument to main(argc, argv) :这最终会结束在 a0 寄存器，也就是 main(argc, argv) 函数的第一个参数中

 // 错误处理程序所做的事：
 // 释放已经分配的新进程的空间
 // 解锁索引节点并结束日志记录，返回-1表示出错
 bad:
  if(pagetable)
    proc_freepagetable(pagetable, sz);
  if(ip){
    iunlockput(ip);
    end_op();
  }
  return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
// 译：将一个程序段读入页表pagetable的虚拟地址va处
// va必须是页对齐的，va到va + sz范围内的页必须已经被映射好
// 返回0表示成功，-1表示失败
static int
loadseg(pagetable_t pagetable, uint64 va, struct inode *ip, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  if((va % PGSIZE) != 0)
    panic("loadseg: va must be page aligned");

  // 迭代地复制程序段，每次复制一个页面
  for(i = 0; i < sz; i += PGSIZE){
    pa = walkaddr(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    
    // 这里是防止最后一页要复制的字节不满一页而导致的复制溢出
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;

    // readi系统调用是从索引节点对应的文件数据缓冲区中读取数据的函数
	  // 下面的调用从索引节点ip指向的文件的off偏移处读取n个字节放入物理地址pa
	  // 第二个参数为0表示处于内核地址空间
	  // 再次地，我们用到了内核地址空间的直接映射(direct-mapping)
	  // 就算pa是某个用户页表翻译出来的物理地址，在内核地址空间中也会被译为同等的地址
    if(readi(ip, 0, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
