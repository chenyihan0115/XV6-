// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO  1   // root i-number
#define BSIZE 1024  // block size 块大小

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint magic;        // Must be FSMAGIC
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define FSMAGIC 0x10203040

#define NDIRECT 11
#define NINDIRECT (BSIZE / sizeof(uint)) //这宏定义 NINDIRECT 表示一级间接块的大小，即每个一级间接块可以存储的磁盘块号的数量 一般就是1024/4 =256
#define MAXFILE (NDIRECT + NINDIRECT + NINDIRECT * NINDIRECT)

// On-disk inode structure
//定义了磁盘上的inode
struct dinode {
  short type;           //用于表示文件的类型，可以是文件（T_FILE）、目录（T_DIR）或特殊文件（T_DEVICE）等。
  short major;          // Major device number (T_DEVICE only)仅在文件类型为T_DEVICE（特殊文件，通常表示设备文件）时有效。 major 表示主设备号，minor 表示次设备号，用于唯一标识设备
  short minor;          // Minor device number (T_DEVICE only)
  short nlink;          // 表示引用该inode的目录项数量，即有多少个目录项指向这个inode，为0释放磁盘上的inode和其数据块
  uint size;            // Size of file (bytes)文件内容的字节数
  uint addrs[NDIRECT+2];   // 持有文件内容的磁盘块块号  NDIRECT 表示直接块的数量，而 addrs 数组用于存储直接块和一级间接块（indirect block）的块号。
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8) //每块的Bitmap位

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b)/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

//目录
struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

