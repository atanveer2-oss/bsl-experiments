// Control arm: the classic skip list (height 1 + G at insertion, never
// changed) on exactly the code of bsl_gated, so that path length and
// throughput differences are the rule's and not the implementation's.
#ifndef BSL_RULE
#define BSL_RULE bsl_conc::ClassicRule
#endif
#include "../bsl_gated/bsl_adapter.h"
