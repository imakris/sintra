/* Strict public SDK declaration probe; failure is evidence, not capability success. */
#include <sys/guarded.h>
int main(void) { return guarded_close_np(-1, 0); }
