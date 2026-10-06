typedef int atomic_t;
static inline int atomic_get(const atomic_t *p) { return *p; }
static inline int atomic_inc(atomic_t *p) { return (*p)++; }
