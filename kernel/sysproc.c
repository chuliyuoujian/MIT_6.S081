#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "date.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  if (argint(0, &n) < 0)
    return -1;
  exit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  if (argaddr(0, &p) < 0)
    return -1;
  return wait(p);
}

uint64
sys_sbrk(void)
{
  int addr;
  int n;

  if (argint(0, &n) < 0)
    return -1;

  addr = myproc()->sz;
  if (growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  if (argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n)
  {
    if (myproc()->killed)
    {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc);//注意声明函数
#ifdef LAB_PGTBL
int sys_pgaccess(void)
{
  // lab pgtbl: your code here.
  // my add
  uint64 va;
  int pgcnt;
  uint64 pmask;
  if (argaddr(0, &va) < 0)
    return -1;
  if (argint(1, &pgcnt) < 0)
    return -1;
  if (argaddr(2, &pmask) < 0)
    return -1;
  struct proc *p = myproc();
  uint64 mask = 0;
  if (pgcnt > 64)
    return -1; // 我们用位掩码来记录每页是否被访问过，如果大于64没法记录了
  for (int i = 0; i < pgcnt; ++i)
  { // 遍历要检查的每页
    pte_t *pte = 0;
    if ((pte = walk(p->pagetable, va + i * PGSIZE, 0)) < 0)/*va是我们要开始找的pgcnt个页的起始点，到下一页也要偏移PGSIZE*/
      return -1;
    if ((*pte) & PTE_A)
    {
      mask |= (1 << i); // 将这一页标记被访问过了
      *pte ^= PTE_A;//hints:确保清除 PTE_A 检查是否设置后。否则，将无法确定是否访问了该页面。
    }
  }
  // 将mask拷贝到用户空间pmask上
  if (copyout(p->pagetable, pmask, (char *)&mask, sizeof(mask)) < 0)
    return -1;
  return 0;
}
#endif

uint64
sys_kill(void)
{
  int pid;

  if (argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}
