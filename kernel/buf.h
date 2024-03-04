struct buf {
  int valid;   // 是否从磁盘读取了数据
  int disk;    // 表示缓冲区的内容是否被修改
  uint dev;    //缓冲区对应的设备号
  uint blockno;  //表示缓冲区对应的磁盘块号，指明数据在磁盘上的位置。
  struct sleeplock lock; //一个睡眠锁（sleeplock），用于保护缓冲区的并发访问。多个线程需要在访问缓冲区时先获得该锁，以确保数据一致性。
  uint refcnt;    //表示缓冲区的引用计数，用于跟踪当前有多少个指针指向该缓冲区。在释放缓冲区时，只有当引用计数为0时才能真正释放。
  struct buf *prev; // 指向LRU（Least Recently Used）缓存列表中的前一个缓冲区。LRU列表用于记录缓冲区的使用顺序，以便在缓冲区不足时选择最近最少使用的缓冲区进行替换。
  struct buf *next; //指向LRU缓存列表中的下一个缓冲区
  uchar data[BSIZE]; //存储实际数据的数组，大小为BSIZE，表示缓冲区的容量。
};

