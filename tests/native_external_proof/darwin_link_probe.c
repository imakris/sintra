/* SDK link probe only; manual PRIVATE prototypes, not public-header support. */
#include <stdint.h>
#include <stddef.h>
extern int change_fdguard_np(int, const uint64_t *, unsigned,
                            const uint64_t *, unsigned, int *);
extern int guarded_close_np(int, const uint64_t *);
int main(void) {
    uint64_t guard = 1;
    int flags = 0;
    int a = change_fdguard_np(-1, NULL, 0, &guard, 15, &flags);
    int b = guarded_close_np(-1, &guard);
    return a + b;
}
