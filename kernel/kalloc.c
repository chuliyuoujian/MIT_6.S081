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

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  // initlock(&kmem.lock, "kmem");
  // freerange(end, (void*)PHYSTOP);

  for(int i=0;i<NCPU;++i) {
    //CPU是8，直接开5位即可
    char lock_name[5]={0};
    snprintf(lock_name,sizeof(lock_name),"kmem%d",i);
    //printf("lock_name: %s\n",lock_name);
    initlock(&kmem[i].lock, lock_name);
  }
  freerange(end, (void*)PHYSTOP);
}


void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// // Free the page of physical memory pointed at by v,
// // which normally should have been returned by a
// // call to kalloc().  (The exception is when
// // initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  //得到CPU id
  //开启关闭中断
  push_off();
  int cpu_id=cpuid();
  pop_off();
  //按照原来的头插法，插进空闲内存
  acquire(&kmem[cpu_id].lock);
  r->next = kmem[cpu_id].freelist;
  kmem[cpu_id].freelist = r;
  release(&kmem[cpu_id].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  //得到CPU id
  //开启关闭中断
  push_off();
  int cpu_id=cpuid();
  pop_off();

  //修改后的逻辑是：我们给每一个CPU都维护一个空闲列表，如果当前的CPU有空闲块，就直接返回，否则就从其他CPU的空闲块借一个
  acquire(&kmem[cpu_id].lock);
  r = kmem[cpu_id].freelist;
  if(r)
    kmem[cpu_id].freelist = r->next;
  else {
    for(int i=0;i<NCPU;++i) {
      if(i==cpu_id) {
        continue;
      }
      acquire(&kmem[i].lock);
      r=kmem[i].freelist;
      if(r) {
        kmem[i].freelist=r->next;
      }
      release(&kmem[i].lock);
      if(r) break;
    }
  }
  release(&kmem[cpu_id].lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  
  return (void*)r;
}
