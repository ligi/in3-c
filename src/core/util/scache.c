#include "scache.h"
#include "mem.h"

bytes_t* in3_cache_get_entry(cache_entry_t* cache, bytes_t* key) {
  while (cache) {
    if (b_cmp(key, &cache->key)) return &cache->value;
    cache = cache->next;
  }
  return NULL;
}
void in3_cache_free(cache_entry_t* cache) {
  cache_entry_t* p;
  while (cache) {
    _free(cache->key.data);
    if (cache->must_free)
      _free(cache->value.data);
    p     = cache;
    cache = cache->next;
    _free(p);
  }
}