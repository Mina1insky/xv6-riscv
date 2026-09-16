// Slab allocator: caches of fixed-size objects carved out of single
// Buddy pages.  The Buddy allocator provides and reclaims the pages;
// this layer multiplexes small objects inside each page.
//
// Locking rules:
//   caches_lock protects only cache descriptor slot creation, destruction
//   and enumeration.
//   cache->lock protects one cache's slab lists, freelists, bitmap and
//   counters.  While holding it, slab growth may call buddy_alloc(0), so
//   the lock order is cache->lock -> buddy.lock.  Pages are returned to
//   Buddy only after they have been removed from every cache list, had
//   their magic cleared, and cache->lock has been dropped; a page is
//   never touched again after buddy_free().  Buddy never calls into Slab.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

#define KMEM_CACHE_MAX       16
#define KMEM_CACHE_NAME_LEN  16
#define SLAB_MIN_STRIDE      16
#define SLAB_BITMAP_WORDS    4
#define SLAB_MAX_OBJECTS     (SLAB_BITMAP_WORDS * 64)
#define SLAB_MAGIC           0x5A15AB5A15AB5A15ULL
#define SLAB_MAX_PAGES       ((PHYSTOP - KERNBASE) / PGSIZE)

enum slab_state {
  SLAB_NONE = 0,
  SLAB_FREE,
  SLAB_PARTIAL,
  SLAB_FULL,
};

struct slab {
  uint64 magic;
  struct kmem_cache *cache;
  struct slab *next;
  struct slab *prev;
  void *freelist;
  uint inuse;
  uint total;
  uint state;
  uint64 allocmap[SLAB_BITMAP_WORDS];
};

struct slab_list {
  struct slab *head;
  uint count;
};

struct kmem_cache {
  struct spinlock lock;
  int used;
  char name[KMEM_CACHE_NAME_LEN];
  uint object_size;
  uint stride;
  uint align;
  uint object_offset;
  uint objects_per_slab;
  struct slab_list free;
  struct slab_list partial;
  struct slab_list full;
  uint64 live_objects;
  uint64 nr_alloc;
  uint64 nr_free;
  uint64 nr_grow;
  uint64 nr_reap;
  uint64 nr_fail;
};

static struct kmem_cache caches[KMEM_CACHE_MAX];
static struct spinlock caches_lock;
static int slab_ready;

#ifdef SLAB_SELFTEST
static void slab_selftest(void);
#endif
#ifdef SLAB_PANIC_CASE
static void slab_panic_test(void);
#endif

static int
align_up_checked(uint64 value, uint align, uint *out)
{
  uint64 a = align;
  uint64 r;

  if (a == 0 || (a & (a - 1)) != 0)
    return -1;
  r = (value + a - 1) & ~(a - 1);
  if (r < value || r > 0xffffffffULL)
    return -1;
  *out = (uint)r;
  return 0;
}

static inline int
bitmap_test(uint64 *map, uint index)
{
  return (map[index / 64] >> (index % 64)) & 1;
}

static inline void
bitmap_set(uint64 *map, uint index)
{
  map[index / 64] |= 1ULL << (index % 64);
}

static inline void
bitmap_clear(uint64 *map, uint index)
{
  map[index / 64] &= ~(1ULL << (index % 64));
}

static uint
bitmap_popcount(uint64 *map)
{
  uint n = 0;
  int i;

  for (i = 0; i < SLAB_BITMAP_WORDS; i++) {
    uint64 x = map[i];

    while (x) {
      x &= x - 1;
      n++;
    }
  }
  return n;
}

// The slab-list helpers below are called with cache->lock held.
static struct slab_list *
slab_list_for(struct kmem_cache *cache, uint state)
{
  if (state == SLAB_FREE)
    return &cache->free;
  if (state == SLAB_PARTIAL)
    return &cache->partial;
  if (state == SLAB_FULL)
    return &cache->full;
  panic("slab list state");
  return 0;
}

static void
slab_list_add(struct slab_list *list, struct slab *s, uint state)
{
  s->state = state;
  s->prev = 0;
  s->next = list->head;
  if (list->head)
    list->head->prev = s;
  list->head = s;
  list->count++;
}

static void
slab_list_del(struct slab_list *list, struct slab *s, uint expected)
{
  if (s->state != expected)
    panic("slab list del state");
  if (s->prev)
    s->prev->next = s->next;
  else
    list->head = s->next;
  if (s->next)
    s->next->prev = s->prev;
  s->next = 0;
  s->prev = 0;
  s->state = SLAB_NONE;
  if (list->count == 0)
    panic("slab list count");
  list->count--;
}

static void
slab_move(struct kmem_cache *cache, struct slab *s, uint new_state)
{
  if (s->state == new_state)
    return;
  slab_list_del(slab_list_for(cache, s->state), s, s->state);
  slab_list_add(slab_list_for(cache, new_state), s, new_state);
}

// Return the slot index of obj within slab, or -1 if obj is not a
// well-formed slot start.
static int
slab_object_index(struct kmem_cache *cache, struct slab *s, void *obj,
                  uint *index)
{
  uint64 base = (uint64)s;
  uint64 off;
  uint idx;

  if ((uint64)obj < base + cache->object_offset)
    return -1;
  off = (uint64)obj - (base + cache->object_offset);
  if (off % cache->stride != 0)
    return -1;
  idx = (uint)off / cache->stride;
  if (idx >= s->total)
    return -1;
  *index = idx;
  return 0;
}

// Called with cache->lock held.  Takes a page from Buddy, zeroes it,
// initializes the slab header and links all object slots.
static struct slab *
slab_grow_locked(struct kmem_cache *cache)
{
  struct slab *s;
  char *page, *p;
  uint i;

  page = buddy_alloc(0);
  if (page == 0) {
    cache->nr_fail++;
    return 0;
  }
  memset(page, 0, PGSIZE);

  s = (struct slab *)page;
  s->magic = SLAB_MAGIC;
  s->cache = cache;
  s->next = 0;
  s->prev = 0;
  s->freelist = 0;
  s->inuse = 0;
  s->total = cache->objects_per_slab;
  s->state = SLAB_NONE;
  for (i = 0; i < SLAB_BITMAP_WORDS; i++)
    s->allocmap[i] = 0;

  for (i = 0; i < s->total; i++) {
    p = page + cache->object_offset + (uint64)i * cache->stride;
    *(void **)p = s->freelist;
    s->freelist = p;
  }

  slab_list_add(&cache->free, s, SLAB_FREE);
  cache->nr_grow++;
  return s;
}

// Called with cache->lock held.
static int
slab_check_freelist(struct kmem_cache *cache, struct slab *s)
{
  void *p = s->freelist;
  uint n = 0;
  uint index;

  while (p) {
    if (n >= s->total)
      return 0;
    if (slab_object_index(cache, s, p, &index) != 0)
      return 0;
    if (bitmap_test(s->allocmap, index))
      return 0;
    n++;
    p = *(void **)p;
  }
  return n == s->total - s->inuse;
}

// Called with cache->lock held.
static int
slab_check_list(struct kmem_cache *cache, struct slab_list *list, uint state,
                uint64 *live)
{
  struct slab *s = list->head;
  struct slab *prev = 0;
  uint count = 0;
  uint i;

  while (s) {
    uint64 base = (uint64)s;

    if (count >= SLAB_MAX_PAGES)
      return 0;
    if (base < KERNBASE || base + PGSIZE > PHYSTOP || base % PGSIZE != 0)
      return 0;
    if (s->magic != SLAB_MAGIC || s->cache != cache || s->state != state)
      return 0;
    if (s->prev != prev)
      return 0;
    if (s->total != cache->objects_per_slab || s->inuse > s->total)
      return 0;
    if (state == SLAB_FREE && s->inuse != 0)
      return 0;
    if (state == SLAB_PARTIAL && !(s->inuse > 0 && s->inuse < s->total))
      return 0;
    if (state == SLAB_FULL && (s->inuse != s->total || s->freelist != 0))
      return 0;
    if (state != SLAB_FULL && s->freelist == 0 && s->total != 0)
      return 0;
    if (bitmap_popcount(s->allocmap) != s->inuse)
      return 0;
    for (i = s->total; i < SLAB_MAX_OBJECTS; i++)
      if (bitmap_test(s->allocmap, i))
        return 0;
    if (!slab_check_freelist(cache, s))
      return 0;

    *live += s->inuse;
    count++;
    prev = s;
    s = s->next;
  }

  return count == list->count;
}

// Called with cache->lock held.
static int
slab_check_locked(struct kmem_cache *cache)
{
  uint64 live = 0;

  if (cache->object_size == 0 || cache->stride < SLAB_MIN_STRIDE)
    return 0;
  if ((cache->align & (cache->align - 1)) != 0)
    return 0;
  if (cache->object_offset < sizeof(struct slab) ||
      cache->object_offset >= PGSIZE)
    return 0;
  if (cache->objects_per_slab == 0 ||
      cache->objects_per_slab > SLAB_MAX_OBJECTS)
    return 0;
  if ((uint64)cache->object_offset +
      (uint64)cache->stride * cache->objects_per_slab > PGSIZE)
    return 0;

  if (!slab_check_list(cache, &cache->free, SLAB_FREE, &live))
    return 0;
  if (!slab_check_list(cache, &cache->partial, SLAB_PARTIAL, &live))
    return 0;
  if (!slab_check_list(cache, &cache->full, SLAB_FULL, &live))
    return 0;

  return live == cache->live_objects;
}

void
slabinit(void)
{
  int i;

  initlock(&caches_lock, "slabcaches");
  for (i = 0; i < KMEM_CACHE_MAX; i++) {
    caches[i].used = 0;
    caches[i].name[0] = 0;
    caches[i].free.head = 0;
    caches[i].free.count = 0;
    caches[i].partial.head = 0;
    caches[i].partial.count = 0;
    caches[i].full.head = 0;
    caches[i].full.count = 0;
    caches[i].live_objects = 0;
  }
  slab_ready = 1;

#ifdef SLAB_SELFTEST
  slab_selftest();
#endif
#ifdef SLAB_PANIC_CASE
  slab_panic_test();
#endif
}

struct kmem_cache *
kmem_cache_create(char *name, uint object_size, uint align)
{
  uint stride, offset, count, i;
  uint64 sz;
  struct kmem_cache *c;
  int slot = -1;

  if (!slab_ready || name == 0 || object_size == 0)
    return 0;
  if (align == 0)
    align = sizeof(void *);
  if (align < sizeof(void *))
    align = sizeof(void *);
  if (align > PGSIZE || (align & (align - 1)) != 0)
    return 0;

  sz = object_size < SLAB_MIN_STRIDE ? SLAB_MIN_STRIDE : object_size;
  if (align_up_checked(sz, align, &stride) != 0)
    return 0;
  if (align_up_checked(sizeof(struct slab), align, &offset) != 0)
    return 0;
  if (offset >= PGSIZE)
    return 0;
  count = (PGSIZE - offset) / stride;
  if (count > SLAB_MAX_OBJECTS)
    count = SLAB_MAX_OBJECTS;
  if (count == 0)
    return 0;
  if ((uint64)offset + (uint64)stride * count > PGSIZE)
    return 0;

  acquire(&caches_lock);
  for (i = 0; i < KMEM_CACHE_MAX; i++) {
    if (!caches[i].used) {
      slot = (int)i;
      break;
    }
  }
  if (slot < 0) {
    release(&caches_lock);
    return 0;
  }

  c = &caches[slot];
  memset(c, 0, sizeof(*c));
  c->used = 1;
  for (i = 0; i < KMEM_CACHE_NAME_LEN - 1 && name[i] != 0; i++)
    c->name[i] = name[i];
  c->name[i] = 0;
  c->object_size = object_size;
  c->stride = stride;
  c->align = align;
  c->object_offset = offset;
  c->objects_per_slab = count;
  initlock(&c->lock, c->name);
  release(&caches_lock);
  return c;
}

void *
kmem_cache_alloc(struct kmem_cache *cache)
{
  struct slab *s;
  void *obj;
  uint index;

  if (cache == 0 || !cache->used || !slab_ready)
    return 0;

  acquire(&cache->lock);

  s = cache->partial.head;
  if (s == 0)
    s = cache->free.head;
  if (s == 0) {
    s = slab_grow_locked(cache);
    if (s == 0) {
      release(&cache->lock);
      return 0;
    }
  }

  if (s->magic != SLAB_MAGIC || s->cache != cache ||
      s->freelist == 0 || s->inuse >= s->total)
    panic("slab alloc state");

  obj = s->freelist;
  s->freelist = *(void **)obj;
  if (slab_object_index(cache, s, obj, &index) != 0)
    panic("slab alloc index");
  if (bitmap_test(s->allocmap, index))
    panic("slab alloc bit");
  bitmap_set(s->allocmap, index);
  s->inuse++;
  cache->live_objects++;
  cache->nr_alloc++;

  if (s->inuse == s->total)
    slab_move(cache, s, SLAB_FULL);
  else
    slab_move(cache, s, SLAB_PARTIAL);

  release(&cache->lock);

  memset(obj, 5, cache->stride); // fill with junk
  return obj;
}

void
kmem_cache_free(struct kmem_cache *cache, void *obj)
{
  struct slab *s;
  struct slab *reap = 0;
  uint64 pa, base;
  uint index;

  if (cache == 0 || obj == 0)
    panic("slab free null");
  pa = (uint64)obj;
  if (pa < KERNBASE || pa >= PHYSTOP)
    panic("slab free range");
  base = pa & ~(uint64)(PGSIZE - 1);

  acquire(&cache->lock);

  s = (struct slab *)base;
  if (s->magic != SLAB_MAGIC || s->cache != cache)
    panic("slab free slab");
  if (s->state != SLAB_FREE && s->state != SLAB_PARTIAL &&
      s->state != SLAB_FULL)
    panic("slab free state");
  if (slab_object_index(cache, s, obj, &index) != 0)
    panic("slab free interior");
  if (!bitmap_test(s->allocmap, index))
    panic("slab free double");

  bitmap_clear(s->allocmap, index);
  s->inuse--;
  cache->live_objects--;
  cache->nr_free++;

  memset(obj, 1, cache->stride); // fill with junk
  *(void **)obj = s->freelist;
  s->freelist = obj;

  if (s->inuse == 0)
    slab_move(cache, s, SLAB_FREE);
  else
    slab_move(cache, s, SLAB_PARTIAL);

  // Reclaim empty slabs.  While the cache is active, keep one empty slab
  // hot.  Once no slab is in use at all, return every empty slab so that
  // free physical page accounting (e.g. usertests) stays exact.
  for (;;) {
    int idle = cache->partial.count == 0 && cache->full.count == 0;

    if (cache->free.count > 1 || (idle && cache->free.count > 0)) {
      reap = cache->free.head;
      slab_list_del(&cache->free, reap, SLAB_FREE);
      reap->magic = 0;
      cache->nr_reap++;
      release(&cache->lock);
      buddy_free(reap, 0);
      acquire(&cache->lock);
      continue;
    }
    break;
  }

  release(&cache->lock);
}

uint
kmem_cache_shrink(struct kmem_cache *cache)
{
  struct slab *s;
  uint n = 0;

  if (cache == 0 || !cache->used || !slab_ready)
    return 0;

  for (;;) {
    acquire(&cache->lock);
    if (cache->free.head == 0) {
      release(&cache->lock);
      break;
    }
    s = cache->free.head;
    slab_list_del(&cache->free, s, SLAB_FREE);
    s->magic = 0;
    release(&cache->lock);

    buddy_free(s, 0);
    n++;
  }
  return n;
}

// Destroy a cache.  This is a quiescent interface: the caller must
// guarantee that no other CPU is concurrently allocating from or freeing
// to this cache.  Returns 0 on success, or -1 if the cache is invalid or
// still has objects in use.  All empty slabs are returned to Buddy and
// the descriptor slot becomes reusable.
int
kmem_cache_destroy(struct kmem_cache *cache)
{
  struct slab *s;

  // Reading used without cache->lock is safe only because the caller
  // guarantees quiescence (see the comment above).
  if (cache == 0 || !cache->used || !slab_ready)
    return -1;

  for (;;) {
    acquire(&cache->lock);
    if (cache->live_objects != 0 ||
        cache->partial.count != 0 || cache->full.count != 0) {
      release(&cache->lock);
      return -1;
    }
    if (cache->free.head == 0) {
      release(&cache->lock);
      break;
    }
    s = cache->free.head;
    slab_list_del(&cache->free, s, SLAB_FREE);
    s->magic = 0;
    release(&cache->lock);
    buddy_free(s, 0);
  }

  acquire(&caches_lock);
  memset(cache, 0, sizeof(*cache));
  cache->used = 0;
  release(&caches_lock);
  return 0;
}

int
kmem_cache_check(struct kmem_cache *cache)
{
  int r;

  if (cache == 0 || !cache->used || !slab_ready)
    return 0;

  acquire(&cache->lock);
  r = slab_check_locked(cache);
  release(&cache->lock);
  return r;
}

void
kmem_cache_dump(struct kmem_cache *cache)
{
  if (cache == 0 || !cache->used || !slab_ready)
    return;

  acquire(&cache->lock);
  printk("slab %s: size %d stride %d align %d off %d per-slab %d\n",
         cache->name, cache->object_size, cache->stride,
         cache->align, cache->object_offset, cache->objects_per_slab);
  printk("slab %s: slabs free %d partial %d full %d, live %ld\n",
         cache->name, cache->free.count, cache->partial.count,
         cache->full.count, cache->live_objects);
  printk("slab %s: alloc %ld free %ld grow %ld reap %ld fail %ld\n",
         cache->name, cache->nr_alloc, cache->nr_free,
         cache->nr_grow, cache->nr_reap, cache->nr_fail);
  release(&cache->lock);
}

#ifdef SLAB_SELFTEST
uint64
kmem_cache_live(struct kmem_cache *cache)
{
  uint64 n;

  if (cache == 0 || !cache->used || !slab_ready)
    return 0;
  acquire(&cache->lock);
  n = cache->live_objects;
  release(&cache->lock);
  return n;
}

uint64
kmem_cache_grow_count(struct kmem_cache *cache)
{
  uint64 n;

  if (cache == 0 || !cache->used || !slab_ready)
    return 0;
  acquire(&cache->lock);
  n = cache->nr_grow;
  release(&cache->lock);
  return n;
}

static uint64 slab_test_rng = 0x1234abcdULL;
static void *slab_test_rec[3 * SLAB_MAX_OBJECTS];

static uint64
slab_test_rand(void)
{
  slab_test_rng ^= slab_test_rng << 13;
  slab_test_rng ^= slab_test_rng >> 7;
  slab_test_rng ^= slab_test_rng << 17;
  return slab_test_rng;
}

static int
slab_test_overlap(uint64 a, uint64 asz, uint64 b, uint64 bsz)
{
  return a < b + bsz && b < a + asz;
}

static int
slab_test_used_slots(void)
{
  int i, n = 0;

  acquire(&caches_lock);
  for (i = 0; i < KMEM_CACHE_MAX; i++)
    if (caches[i].used)
      n++;
  release(&caches_lock);
  return n;
}

// T1: create parameters.
static void
slab_test_create(void)
{
  struct kmem_cache *c;
  int before = slab_test_used_slots();

  c = kmem_cache_create("t1", 64, 0);
  if (c == 0)
    panic("slab test t1 create");
  if (c->object_size != 64 || c->stride != 64 ||
      c->objects_per_slab == 0 || c->objects_per_slab > SLAB_MAX_OBJECTS)
    panic("slab test t1 layout");

  if (kmem_cache_create("bad0", 0, 0) != 0)
    panic("slab test t1 size0");
  if (kmem_cache_create("bad1", 64, 24) != 0)
    panic("slab test t1 align");
  if (kmem_cache_create("bad2", 64, 2 * PGSIZE) != 0)
    panic("slab test t1 align big");
  if (kmem_cache_create("bad3", PGSIZE, 0) != 0)
    panic("slab test t1 too big");
  if (kmem_cache_create(0, 64, 0) != 0)
    panic("slab test t1 name");
  if (slab_test_used_slots() != before + 1)
    panic("slab test t1 slots");

  if (!kmem_cache_check(c))
    panic("slab test t1 check");
  if (kmem_cache_destroy(c) != 0)
    panic("slab test t1 destroy");
  if (slab_test_used_slots() != before)
    panic("slab test t1 slots restore");
  printk("slab: T1 ok\n");
}

// T2: basic alloc/free.
static void
slab_test_basic(void)
{
  struct kmem_cache *c;
  void *p[3];
  int i, j;

  c = kmem_cache_create("t2", 64, 0);
  if (c == 0)
    panic("slab test t2 create");

  for (i = 0; i < 3; i++) {
    p[i] = kmem_cache_alloc(c);
    if (p[i] == 0)
      panic("slab test t2 alloc");
    if (((uint64)p[i] & (c->align - 1)) != 0)
      panic("slab test t2 align");
    for (j = 0; j < i; j++)
      if (p[i] == p[j])
        panic("slab test t2 distinct");
    memset(p[i], 0x5a + i, c->stride);
  }
  if (c->live_objects != 3)
    panic("slab test t2 live");
  if (!kmem_cache_check(c))
    panic("slab test t2 check");

  for (i = 2; i >= 0; i--)
    kmem_cache_free(c, p[i]);
  if (c->live_objects != 0)
    panic("slab test t2 restore");
  if (!kmem_cache_check(c))
    panic("slab test t2 final check");
  if (kmem_cache_destroy(c) != 0)
    panic("slab test t2 destroy");
  printk("slab: T2 ok\n");
}

// T3: cross-slab growth and state transitions.
static void
slab_test_cross(void)
{
  struct kmem_cache *c;
  uint n, i;

  c = kmem_cache_create("t3", 64, 0);
  if (c == 0)
    panic("slab test t3 create");
  n = c->objects_per_slab;

  for (i = 0; i < n + 3; i++) {
    slab_test_rec[i] = kmem_cache_alloc(c);
    if (slab_test_rec[i] == 0)
      panic("slab test t3 alloc");
  }
  if (c->full.count < 1 || c->partial.count < 1)
    panic("slab test t3 states");
  if (c->live_objects != n + 3)
    panic("slab test t3 live");
  if (!kmem_cache_check(c))
    panic("slab test t3 check");

  // rec[0..n-1] fill the first slab; rec[n..n+2] are in the second.
  // Emptying the second slab leaves the cache active (first slab full),
  // so one empty slab is kept hot.
  for (i = n; i < n + 3; i++)
    kmem_cache_free(c, slab_test_rec[i]);
  if (c->full.count != 1 || c->partial.count != 0 || c->free.count != 1)
    panic("slab test t3 hot page");
  if (!kmem_cache_check(c))
    panic("slab test t3 mid check");

  // Emptying the first slab makes the whole cache idle; every empty slab
  // must then be returned to Buddy.
  for (i = 0; i < n; i++)
    kmem_cache_free(c, slab_test_rec[i]);
  if (c->live_objects != 0)
    panic("slab test t3 restore");
  if (c->free.count != 0 || c->partial.count != 0 || c->full.count != 0)
    panic("slab test t3 idle reap");
  if (!kmem_cache_check(c))
    panic("slab test t3 final check");
  if (kmem_cache_destroy(c) != 0)
    panic("slab test t3 destroy");
  printk("slab: T3 ok (%d objects/slab)\n", n);
}

// T4: multiple sizes and alignment.
static void
slab_test_sizes(void)
{
  static const uint sizes[] = {16, 24, 64, 128, 512, 1024};
  struct kmem_cache *c;
  uint n, i, s;
  int k;

  for (k = 0; k < 6; k++) {
    char name[KMEM_CACHE_NAME_LEN];

    for (i = 0; i < KMEM_CACHE_NAME_LEN - 1 && "t4XXXX"[i] != 0; i++)
      name[i] = "t4XXXX"[i];
    name[i] = 0;
    name[2] = (char)('0' + k);
    c = kmem_cache_create(name, sizes[k], 0);
    if (c == 0)
      panic("slab test t4 create");
    n = c->objects_per_slab;

    for (i = 0; i < n + 1; i++) {
      void *p = kmem_cache_alloc(c);
      char *pc;

      if (p == 0)
        panic("slab test t4 alloc");
      if (((uint64)p & (c->align - 1)) != 0)
        panic("slab test t4 align");
      pc = p;
      pc[0] = (char)(0xa0 + k);
      pc[c->stride - 1] = (char)(0xb0 + k);
      slab_test_rec[i] = p;
    }
    for (s = 0; s < n + 1; s++)
      for (i = s + 1; i < n + 1; i++)
        if (slab_test_overlap((uint64)slab_test_rec[s], c->stride,
                              (uint64)slab_test_rec[i], c->stride))
          panic("slab test t4 overlap");
    if (!kmem_cache_check(c))
      panic("slab test t4 check");

    for (i = 0; i < n + 1; i++)
      kmem_cache_free(c, slab_test_rec[i]);
    if (!kmem_cache_check(c))
      panic("slab test t4 final check");
    if (kmem_cache_destroy(c) != 0)
      panic("slab test t4 destroy");
  }
  printk("slab: T4 ok\n");
}

// T5: deterministic mixed alloc/free stress.
static void
slab_test_stress(void)
{
  struct kmem_cache *c;
  int live = 0, i, it, slot;
  uint64 stride;

  c = kmem_cache_create("t5", 32, 0);
  if (c == 0)
    panic("slab test t5 create");
  stride = c->stride;
  for (i = 0; i < SLAB_MAX_OBJECTS; i++)
    slab_test_rec[i] = 0;

  for (it = 0; it < 5000; it++) {
    if (live < SLAB_MAX_OBJECTS && (live == 0 || slab_test_rand() % 3 != 0)) {
      void *p = kmem_cache_alloc(c);

      if (p) {
        if (((uint64)p & (c->align - 1)) != 0)
          panic("slab test t5 align");
        for (i = 0; i < SLAB_MAX_OBJECTS; i++)
          if (slab_test_rec[i] &&
              slab_test_overlap((uint64)p, stride,
                                (uint64)slab_test_rec[i], stride))
            panic("slab test t5 overlap");
        for (i = 0; i < SLAB_MAX_OBJECTS; i++)
          if (slab_test_rec[i] == 0)
            break;
        if (i == SLAB_MAX_OBJECTS)
          panic("slab test t5 full");
        slab_test_rec[i] = p;
        live++;
      }
    } else if (live > 0) {
      do {
        slot = slab_test_rand() % SLAB_MAX_OBJECTS;
      } while (slab_test_rec[slot] == 0);
      kmem_cache_free(c, slab_test_rec[slot]);
      slab_test_rec[slot] = 0;
      live--;
    }
    if (it % 100 == 0 && !kmem_cache_check(c))
      panic("slab test t5 check");
  }

  for (i = 0; i < SLAB_MAX_OBJECTS; i++)
    if (slab_test_rec[i]) {
      kmem_cache_free(c, slab_test_rec[i]);
      slab_test_rec[i] = 0;
    }
  if (c->live_objects != 0)
    panic("slab test t5 live");
  if (!kmem_cache_check(c))
    panic("slab test t5 final check");
  if (kmem_cache_destroy(c) != 0)
    panic("slab test t5 destroy");
  printk("slab: T5 ok\n");
}

// T6: Buddy pages fully recovered after shrink.
static void
slab_test_buddy(void)
{
  struct kmem_cache *c;
  uint64 before = buddy_free_pages();
  uint n, i;

  c = kmem_cache_create("t6", 128, 0);
  if (c == 0)
    panic("slab test t6 create");
  n = c->objects_per_slab;

  for (i = 0; i < n + 3; i++) {
    slab_test_rec[i] = kmem_cache_alloc(c);
    if (slab_test_rec[i] == 0)
      panic("slab test t6 alloc");
  }
  if (buddy_free_pages() != before - 2)
    panic("slab test t6 grow");
  if (!kmem_cache_check(c))
    panic("slab test t6 check");

  // Empty the second slab; it stays hot while the first is full.
  for (i = n; i < n + 3; i++)
    kmem_cache_free(c, slab_test_rec[i]);
  if (buddy_free_pages() != before - 2)
    panic("slab test t6 hot");

  // shrink returns exactly the empty slab.
  if (kmem_cache_shrink(c) != 1)
    panic("slab test t6 shrink");
  if (buddy_free_pages() != before - 1)
    panic("slab test t6 shrink pages");

  for (i = 0; i < n; i++)
    kmem_cache_free(c, slab_test_rec[i]);
  if (buddy_free_pages() != before)
    panic("slab test t6 restore");
  if (!buddy_check())
    panic("slab test t6 buddy");
  if (kmem_cache_destroy(c) != 0)
    panic("slab test t6 destroy");
  printk("slab: T6 ok\n");
}

// T7: cache lifecycle, busy destruction and descriptor slot reuse.
static void
slab_test_lifecycle(void)
{
  struct kmem_cache *c;
  void *p;
  int before = slab_test_used_slots();
  int i;

  if (kmem_cache_destroy(0) != -1)
    panic("slab test t7 null");

  c = kmem_cache_create("t7", 64, 0);
  if (c == 0)
    panic("slab test t7 create");
  if (slab_test_used_slots() != before + 1)
    panic("slab test t7 slot");

  p = kmem_cache_alloc(c);
  if (p == 0)
    panic("slab test t7 alloc");
  if (kmem_cache_destroy(c) != -1)
    panic("slab test t7 busy");
  kmem_cache_free(c, p);
  if (kmem_cache_destroy(c) != 0)
    panic("slab test t7 destroy");
  if (slab_test_used_slots() != before)
    panic("slab test t7 restore");
  if (kmem_cache_destroy(c) != -1)
    panic("slab test t7 double destroy");

  for (i = 0; i < 2 * KMEM_CACHE_MAX + 4; i++) {
    c = kmem_cache_create("t7r", 32, 0);
    if (c == 0)
      panic("slab test t7 reuse");
    if (kmem_cache_destroy(c) != 0)
      panic("slab test t7 reuse destroy");
  }
  if (slab_test_used_slots() != before)
    panic("slab test t7 slots restore");
  printk("slab: T7 ok\n");
}

static uint
slab_test_align_up(uint value, uint align)
{
  return (value + align - 1) & ~(align - 1);
}

// T8: boundary sizes, alignments and layout failures.
static void
slab_test_boundary(void)
{
  static const uint sizes[] = {1, 15, 16, 17, 24, 64, 512, 1024};
  static const uint aligns[] = {0, 8, 16, 64, PGSIZE};
  struct kmem_cache *c;
  int before = slab_test_used_slots();
  uint k, sz, stride, offset, count;
  void *p;

  for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
    char name[KMEM_CACHE_NAME_LEN];

    name[0] = 't';
    name[1] = '8';
    name[2] = (char)('a' + k);
    name[3] = 0;
    c = kmem_cache_create(name, sizes[k], 0);
    if (c == 0)
      panic("slab test t8 create");

    sz = sizes[k] < SLAB_MIN_STRIDE ? SLAB_MIN_STRIDE : sizes[k];
    stride = slab_test_align_up(sz, 8);
    offset = slab_test_align_up(sizeof(struct slab), 8);
    count = (PGSIZE - offset) / stride;
    if (count > SLAB_MAX_OBJECTS)
      count = SLAB_MAX_OBJECTS;
    if (c->stride != stride || c->object_offset != offset ||
        c->objects_per_slab != count)
      panic("slab test t8 layout");
    if ((uint64)offset + (uint64)stride * count > PGSIZE)
      panic("slab test t8 overflow");

    p = kmem_cache_alloc(c);
    if (p == 0)
      panic("slab test t8 alloc");
    if (((uint64)p & (c->align - 1)) != 0)
      panic("slab test t8 addr");
    ((char *)p)[0] = 0x11;
    ((char *)p)[stride - 1] = 0x22;
    kmem_cache_free(c, p);
    if (kmem_cache_destroy(c) != 0)
      panic("slab test t8 destroy");
  }

  for (k = 0; k < sizeof(aligns) / sizeof(aligns[0]); k++) {
    char name[KMEM_CACHE_NAME_LEN];

    name[0] = 't';
    name[1] = '8';
    name[2] = 'b';
    name[3] = (char)('a' + k);
    name[4] = 0;
    c = kmem_cache_create(name, 120, aligns[k]);
    if (aligns[k] == PGSIZE) {
      if (c != 0)
        panic("slab test t8 align page");
      continue;
    }
    if (c == 0)
      panic("slab test t8 align create");
    p = kmem_cache_alloc(c);
    if (p == 0)
      panic("slab test t8 align alloc");
    if (((uint64)p & (aligns[k] ? aligns[k] - 1 : sizeof(void *) - 1)) != 0)
      panic("slab test t8 align addr");
    kmem_cache_free(c, p);
    if (kmem_cache_destroy(c) != 0)
      panic("slab test t8 align destroy");
  }

  // These must all fail without consuming a descriptor slot.
  if (kmem_cache_create("t8z0", 0, 0) != 0)
    panic("slab test t8 size0");
  if (kmem_cache_create("t8z1", 64, 24) != 0)
    panic("slab test t8 align bad");
  if (kmem_cache_create("t8z2", 64, 2 * PGSIZE) != 0)
    panic("slab test t8 align big");
  if (kmem_cache_create("t8z3", PGSIZE, 0) != 0)
    panic("slab test t8 too big");
  if (kmem_cache_create("t8z4",
                        PGSIZE - (uint)sizeof(struct slab) + 1, 0) != 0)
    panic("slab test t8 no room");
  if (slab_test_used_slots() != before)
    panic("slab test t8 slots");
  printk("slab: T8 ok\n");
}

// T9: repeated grow/reap cycles across slabs.
static void
slab_test_repeat(void)
{
  struct kmem_cache *c;
  uint64 before = buddy_free_pages();
  uint n, i, it;
  int k;

  c = kmem_cache_create("t9", 128, 0);
  if (c == 0)
    panic("slab test t9 create");
  n = c->objects_per_slab;
  if (n + 2 > 3 * SLAB_MAX_OBJECTS)
    panic("slab test t9 size");

  for (it = 0; it < 8; it++) {
    uint64 grow0 = c->nr_grow;
    uint64 reap0 = c->nr_reap;

    for (i = 0; i < n + 2; i++) {
      slab_test_rec[i] = kmem_cache_alloc(c);
      if (slab_test_rec[i] == 0)
        panic("slab test t9 alloc");
      ((char *)slab_test_rec[i])[c->stride - 1] = (char)it;
    }
    if (c->full.count < 1 || c->partial.count < 1)
      panic("slab test t9 states");
    if (!kmem_cache_check(c))
      panic("slab test t9 check");

    // Deterministic out-of-order free: even indices, then odd indices.
    for (k = 0; k < 2; k++)
      for (i = (uint)k; i < n + 2; i += 2)
        kmem_cache_free(c, slab_test_rec[i]);

    if (c->live_objects != 0 || c->free.count != 0 ||
        c->partial.count != 0 || c->full.count != 0)
      panic("slab test t9 drain");
    if (c->nr_grow - grow0 != c->nr_reap - reap0)
      panic("slab test t9 balance");
    if (!kmem_cache_check(c))
      panic("slab test t9 final check");
    if (buddy_free_pages() != before)
      panic("slab test t9 pages");
  }

  if (kmem_cache_destroy(c) != 0)
    panic("slab test t9 destroy");
  printk("slab: T9 ok\n");
}

static void
slab_selftest(void)
{
  uint64 before = buddy_free_pages();

  slab_test_create();
  slab_test_basic();
  slab_test_cross();
  slab_test_sizes();
  slab_test_stress();
  slab_test_buddy();
  slab_test_lifecycle();
  slab_test_boundary();
  slab_test_repeat();

  if (slab_test_used_slots() != 0)
    panic("slab selftest slots");
  if (buddy_free_pages() != before)
    panic("slab selftest leak");
  if (!buddy_check())
    panic("slab selftest buddy");
  printk("slab: all selftests passed, free pages %ld\n", buddy_free_pages());
}
#endif

#ifdef SLAB_PANIC_CASE
static void
slab_panic_test(void)
{
#if SLAB_PANIC_CASE == 1
  // P1: double free.  Keep a second object alive so the slab is not
  // reaped between the two frees and the bitmap check is reached.
  struct kmem_cache *c = kmem_cache_create("p1", 64, 0);
  void *p, *q;

  if (c == 0)
    panic("slab panic test create");
  p = kmem_cache_alloc(c);
  q = kmem_cache_alloc(c);
  if (p == 0 || q == 0)
    panic("slab panic test alloc");
  kmem_cache_free(c, p);
  kmem_cache_free(c, p);
#elif SLAB_PANIC_CASE == 2
  // P2: free to the wrong cache.
  struct kmem_cache *a = kmem_cache_create("p2a", 64, 0);
  struct kmem_cache *b = kmem_cache_create("p2b", 64, 0);
  void *p;

  if (a == 0 || b == 0)
    panic("slab panic test create");
  p = kmem_cache_alloc(a);
  if (p == 0)
    panic("slab panic test alloc");
  kmem_cache_free(b, p);
#elif SLAB_PANIC_CASE == 3
  // P3: interior pointer.
  struct kmem_cache *c = kmem_cache_create("p3", 64, 0);
  void *p;

  if (c == 0)
    panic("slab panic test create");
  p = kmem_cache_alloc(c);
  if (p == 0)
    panic("slab panic test alloc");
  kmem_cache_free(c, (char *)p + c->stride / 2);
#elif SLAB_PANIC_CASE == 4
  // P4: free a raw Buddy page through a slab cache.
  struct kmem_cache *c = kmem_cache_create("p4", 64, 0);
  void *page;

  if (c == 0)
    panic("slab panic test create");
  page = buddy_alloc(0);
  if (page == 0)
    panic("slab panic test buddy");
  kmem_cache_free(c, page);
#elif SLAB_PANIC_CASE == 5
  // P5: corrupted slab magic.
  struct kmem_cache *c = kmem_cache_create("p5", 64, 0);
  struct slab *s;
  void *p;

  if (c == 0)
    panic("slab panic test create");
  p = kmem_cache_alloc(c);
  if (p == 0)
    panic("slab panic test alloc");
  s = (struct slab *)((uint64)p & ~(uint64)(PGSIZE - 1));
  s->magic = 0;
  kmem_cache_free(c, p);
#else
#error "SLAB_PANIC_CASE must be 1..5"
#endif
}
#endif
