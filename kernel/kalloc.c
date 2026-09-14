// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define BUDDY_MAX_ORDER 14
#define BUDDY_NR_PAGES ((PHYSTOP - KERNBASE) / PGSIZE)

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

enum page_state {
  PAGE_UNUSED = 0,
  PAGE_FREE,
  PAGE_ALLOCATED,
};

struct page {
  struct page *next;
  struct page *prev;
  short order;
  uchar state;
};

struct free_area {
  struct page *head;
  uint64 nr_blocks;
};

static struct page pages[BUDDY_NR_PAGES];

static struct {
  struct spinlock lock;
  struct free_area area[BUDDY_MAX_ORDER + 1];
  uint64 managed_start;
  uint64 managed_first_index;
  uint64 free_pages;
  uint64 nr_alloc;
  uint64 nr_free;
  uint64 nr_split;
  uint64 nr_merge;
  uint64 nr_fail;
} buddy;

static uint64
pa_to_index(uint64 pa)
{
  return (pa - KERNBASE) / PGSIZE;
}

static uint64
index_to_pa(uint64 index)
{
  return KERNBASE + index * PGSIZE;
}

static uint64
page_to_index(struct page *p)
{
  return (uint64)(p - pages);
}

static inline uint64
page_to_pa(struct page *p)
{
  return index_to_pa(page_to_index(p));
}

// The free-list helpers below assume the caller holds buddy.lock,
// or that they are called single-threaded during kinit().
static void
free_list_add(struct page *p, int order)
{
  struct free_area *area = &buddy.area[order];

  p->state = PAGE_FREE;
  p->order = order;
  p->prev = 0;
  p->next = area->head;
  if (area->head)
    area->head->prev = p;
  area->head = p;
  area->nr_blocks++;
}

static void
free_list_del(struct page *p, int order)
{
  struct free_area *area = &buddy.area[order];

  if (p->prev)
    p->prev->next = p->next;
  else
    area->head = p->next;
  if (p->next)
    p->next->prev = p->prev;

  p->prev = 0;
  p->next = 0;
  p->state = PAGE_UNUSED;
  p->order = -1;
  area->nr_blocks--;
}

// Caller must hold buddy.lock.
static int
buddy_check_locked(void)
{
  uint64 total = 0;
  int order;

  for (order = 0; order <= BUDDY_MAX_ORDER; order++) {
    struct free_area *area = &buddy.area[order];
    struct page *prev = 0;
    struct page *p = area->head;
    uint64 count = 0;
    uint64 npages = 1ULL << order;

    while (p) {
      uint64 index;

      if (count >= BUDDY_NR_PAGES)
        return 0;
      if (p < pages || p >= &pages[BUDDY_NR_PAGES])
        return 0;

      index = page_to_index(p);

      if (p->state != PAGE_FREE || p->order != order)
        return 0;
      if (p->prev != prev)
        return 0;
      if ((index & (npages - 1)) != 0)
        return 0;
      if (index < buddy.managed_first_index ||
          index + npages > BUDDY_NR_PAGES)
        return 0;

      // Two free buddies of the same order should have been merged.
      if (order < BUDDY_MAX_ORDER) {
        uint64 bi = index ^ (1ULL << order);
        if (bi >= buddy.managed_first_index &&
            bi + npages <= BUDDY_NR_PAGES) {
          struct page *bp = &pages[bi];
          if (bp->state == PAGE_FREE && bp->order == order)
            return 0;
        }
      }

      total += npages;
      count++;
      prev = p;
      p = p->next;
    }

    if (count != area->nr_blocks)
      return 0;
  }

  if (total != buddy.free_pages)
    return 0;

  return 1;
}

#ifdef BUDDY_DEBUG
static void
buddy_selftest(void)
{
  uint64 before = buddy.free_pages;
  void *a, *b, *c;

  a = buddy_alloc(0);
  b = buddy_alloc(0);
  c = buddy_alloc(2);
  if (a == 0 || b == 0 || c == 0)
    panic("buddy selftest alloc");
  if ((((uint64)c - KERNBASE) & ((PGSIZE << 2) - 1)) != 0)
    panic("buddy selftest align");

  buddy_free(c, 2);
  buddy_free(a, 0);
  buddy_free(b, 0);

  if (!buddy_check())
    panic("buddy selftest check");
  if (buddy.free_pages != before)
    panic("buddy selftest leak");
  printk("buddy: selftest passed\n");
}
#endif

void
kinit()
{
  uint64 first, limit, index;
  int order, i;

  initlock(&buddy.lock, "buddy");

  for (i = 0; i <= BUDDY_MAX_ORDER; i++) {
    buddy.area[i].head = 0;
    buddy.area[i].nr_blocks = 0;
  }

  buddy.free_pages = 0;
  buddy.nr_alloc = 0;
  buddy.nr_free = 0;
  buddy.nr_split = 0;
  buddy.nr_merge = 0;
  buddy.nr_fail = 0;

  for (i = 0; i < BUDDY_NR_PAGES; i++) {
    pages[i].next = 0;
    pages[i].prev = 0;
    pages[i].state = PAGE_UNUSED;
    pages[i].order = -1;
  }

  buddy.managed_start = PGROUNDUP((uint64)end);
  buddy.managed_first_index = pa_to_index(buddy.managed_start);
  first = buddy.managed_first_index;
  limit = pa_to_index(PHYSTOP);

  index = first;
  while (index < limit) {
    order = BUDDY_MAX_ORDER;

    while (order > 0) {
      uint64 npages = 1ULL << order;

      if ((index & (npages - 1)) == 0 && index + npages <= limit)
        break;
      order--;
    }

    free_list_add(&pages[index], order);
    buddy.free_pages += 1ULL << order;
    index += 1ULL << order;
  }

  if (!buddy_check_locked())
    panic("buddy init");

#ifdef BUDDY_DEBUG
  buddy_selftest();
#endif
}

// Allocate 2^order contiguous physical pages, aligned to 2^order pages
// relative to KERNBASE.  Returns 0 if the order is invalid or no block
// is available.
void *
buddy_alloc(int order)
{
  int current;
  uint64 index, pa;
  struct page *p;

  if (order < 0 || order > BUDDY_MAX_ORDER)
    return 0;

  acquire(&buddy.lock);

  for (current = order; current <= BUDDY_MAX_ORDER; current++)
    if (buddy.area[current].head)
      break;

  if (current > BUDDY_MAX_ORDER) {
    buddy.nr_fail++;
    release(&buddy.lock);
    return 0;
  }

  p = buddy.area[current].head;
  free_list_del(p, current);
  index = page_to_index(p);

  while (current > order) {
    current--;
    free_list_add(&pages[index + (1ULL << current)], current);
    buddy.nr_split++;
  }

  p->state = PAGE_ALLOCATED;
  p->order = order;
  p->prev = 0;
  p->next = 0;

  buddy.free_pages -= 1ULL << order;
  buddy.nr_alloc++;

  release(&buddy.lock);

  pa = index_to_pa(index);
  memset((void *)pa, 5, (uint64)PGSIZE << order); // fill with junk
  return (void *)pa;
}

// Free a 2^order block previously returned by buddy_alloc.
void
buddy_free(void *pa, int order)
{
  uint64 index, buddy_index, npages;
  struct page *p, *bp;

  if (order < 0 || order > BUDDY_MAX_ORDER)
    panic("buddy_free order");
  if (pa == 0 || ((uint64)pa % PGSIZE) != 0)
    panic("buddy_free address");
  if ((uint64)pa < buddy.managed_start ||
      (uint64)pa + ((uint64)PGSIZE << order) > PHYSTOP)
    panic("buddy_free range");
  if ((((uint64)pa - KERNBASE) & (((uint64)PGSIZE << order) - 1)) != 0)
    panic("buddy_free alignment");

  acquire(&buddy.lock);

  index = pa_to_index((uint64)pa);
  p = &pages[index];
  if (p->state != PAGE_ALLOCATED)
    panic("buddy_free state");
  if (p->order != order)
    panic("buddy_free order mismatch");

  npages = 1ULL << order;

  // Poison before the block can re-enter a free list, while holding the
  // lock so that no free-list node can be overwritten concurrently.
  memset(pa, 1, (uint64)PGSIZE << order);

  p->prev = 0;
  p->next = 0;
  p->state = PAGE_UNUSED;
  p->order = -1;

  while (order < BUDDY_MAX_ORDER) {
    buddy_index = index ^ (1ULL << order);

    if (buddy_index < buddy.managed_first_index ||
        buddy_index + (1ULL << order) > BUDDY_NR_PAGES)
      break;

    bp = &pages[buddy_index];
    if (bp->state != PAGE_FREE || bp->order != order)
      break;

    free_list_del(bp, order);
    if (buddy_index < index)
      index = buddy_index;
    order++;
    buddy.nr_merge++;
  }

  free_list_add(&pages[index], order);
  buddy.free_pages += npages;
  buddy.nr_free++;

  release(&buddy.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  return buddy_alloc(0);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().
void
kfree(void *pa)
{
  buddy_free(pa, 0);
}

int
buddy_check(void)
{
  int r;

  acquire(&buddy.lock);
  r = buddy_check_locked();
  release(&buddy.lock);
  return r;
}

uint64
buddy_free_pages(void)
{
  uint64 n;

  acquire(&buddy.lock);
  n = buddy.free_pages;
  release(&buddy.lock);
  return n;
}

void
buddy_dump(void)
{
  int order;

  acquire(&buddy.lock);
  for (order = 0; order <= BUDDY_MAX_ORDER; order++)
    if (buddy.area[order].nr_blocks)
      printk("buddy: order %d: %ld blocks\n",
             order, buddy.area[order].nr_blocks);
  printk("buddy: free %ld pages, alloc %ld free %ld split %ld merge %ld fail %ld\n",
         buddy.free_pages, buddy.nr_alloc, buddy.nr_free,
         buddy.nr_split, buddy.nr_merge, buddy.nr_fail);
  release(&buddy.lock);
}
