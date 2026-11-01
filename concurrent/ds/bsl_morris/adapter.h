// The paper's Morris estimator (Appendix A, def:morrisgate; the rule of
// MorrisEstimatorPolicy in experiments/include/bsl/skiplist.hpp) on the code
// of bsl_gated: a small counter Q, starting at 1, incremented (by CAS) with
// probability p^Q; if the increment happens while H = Q, H rises by 1 + G.
// See MorrisRule in bsl_skiplist.h for the concurrent formulation.
#ifndef BSL_RULE
#define BSL_RULE bsl_conc::MorrisRule
#endif
#include "../bsl_gated/bsl_adapter.h"
