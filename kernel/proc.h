// Saved registers for kernel context switches.
struct context {
  uint64 ra;
  uint64 sp;

  // callee-saved
  uint64 s0;
  uint64 s1;
  uint64 s2;
  uint64 s3;
  uint64 s4;
  uint64 s5;
  uint64 s6;
  uint64 s7;
  uint64 s8;
  uint64 s9;
  uint64 s10;
  uint64 s11;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;          // The process running on this cpu, or null.
  struct context context;     // swtch() here to enter scheduler().
  int noff;                   // Depth of push_off() nesting.
  int intena;                 // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// the sscratch register points here.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
// the trapframe includes callee-saved user registers like s0-s11 because the
// return-to-user path via usertrapret() doesn't return through
// the entire kernel call stack.
struct trapframe {
  /*   0 */ uint64 kernel_satp;   // kernel page table
  /*   8 */ uint64 kernel_sp;     // top of process's kernel stack
  /*  16 */ uint64 kernel_trap;   // usertrap()
  /*  24 */ uint64 epc;           // saved user program counter
  /*  32 */ uint64 kernel_hartid; // saved kernel tp
  /*  40 */ uint64 ra;
  /*  48 */ uint64 sp;
  /*  56 */ uint64 gp;
  /*  64 */ uint64 tp;
  /*  72 */ uint64 t0;
  /*  80 */ uint64 t1;
  /*  88 */ uint64 t2;
  /*  96 */ uint64 s0;
  /* 104 */ uint64 s1;
  /* 112 */ uint64 a0;
  /* 120 */ uint64 a1;
  /* 128 */ uint64 a2;
  /* 136 */ uint64 a3;
  /* 144 */ uint64 a4;
  /* 152 */ uint64 a5;
  /* 160 */ uint64 a6;
  /* 168 */ uint64 a7;
  /* 176 */ uint64 s2;
  /* 184 */ uint64 s3;
  /* 192 */ uint64 s4;
  /* 200 */ uint64 s5;
  /* 208 */ uint64 s6;
  /* 216 */ uint64 s7;
  /* 224 */ uint64 s8;
  /* 232 */ uint64 s9;
  /* 240 */ uint64 s10;
  /* 248 */ uint64 s11;
  /* 256 */ uint64 t3;
  /* 264 */ uint64 t4;
  /* 272 */ uint64 t5;
  /* 280 */ uint64 t6;
};

enum procstate { UNUSED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };
//空闲，睡眠，就绪，运行，终止


// Per-process state
struct proc {
  struct spinlock lock;        //自旋锁，用于在多线程环境下保护进程结构体中的共享数据。

  // p->lock must be held when using these:
  enum procstate state;        // 进程状态，可能的取值包括运行、就绪、睡眠、停止等。
  struct proc *parent;         // 指向父进程的指针
  void *chan;                  // If non-zero, sleeping on chan 用于睡眠和唤醒的通道。
  int killed;                  // 表示进程是否已被杀死的标志。
  int xstate;                  // 用于存储进程的退出状态，将返回给父进程的wait系统调用。
  int pid;                     // Process ID

  // these are private to the process, so p->lock need not be held.
  //进程的私有成员，不需要持有进程锁来访问，不用担心并发访问的问题
  uint64 kstack;               // 内核栈的虚拟地址。
  uint64 sz;                   // 进程内存空间的大小（以字节为单位）
  pagetable_t pagetable;       // 用户空间页表
  struct trapframe *trapframe; // 用于存储进程的中断帧，是一个数据页，用于中断处理。
  // struct usyscall  *usyscall;
  struct context context;      // 用于进程上下文切换的数据结构。
  struct file *ofile[NOFILE];  // 打开文件表，存储进程打开的文件描述符。
  struct inode *cwd;           // 当前工作目录的inode指针。
  char name[16];               // 进程名，用于调试目的
  pagetable_t kernelpgtbl;     // 内核页表，用于映射内核空间
};
