//
// Sintra Qt keyword macros test
//
// Qt defines emit, signals, slots, foreach and forever as macros unless an
// application sets QT_NO_KEYWORDS. Applications often include Qt headers
// before Sintra's, so Sintra headers must not use these words as identifiers.
// This translation unit defines them the way Qt does, then includes Sintra.
//

#define slots
#define signals public
#define emit
#define foreach(variable, container) for (variable : container)
#define forever for (;;)

#include <sintra/sintra.h>

int main()
{
    return 0;
}
