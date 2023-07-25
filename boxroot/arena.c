#define CAML_NAME_SPACE

#include "arena.h"

value * bxr_arena_alloc_slow()
{
  arena_data *ad = get_arena_data();
  intnat size = 2 * ARENA_POOL_SIZE(*ad);
  arena *a = malloc(sizeof(arena) + (size - START_ITEMS) * sizeof(value));
  bxr_init_arena_with_size(a, size);
  ARENA_NEXT_INDEX(a->data) = 1;
  return &a->pool[0];
}
