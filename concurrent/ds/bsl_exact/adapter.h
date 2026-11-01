// The paper's exact estimator (Appendix A, def:estimators; the rule of
// ExactEstimatorPolicy in experiments/include/bsl/skiplist.hpp) on the code
// of bsl_gated: a per-item 64-bit counter f, incremented on every successful
// search; if then H <= rank = floor(log_b f), H is redrawn as rank + G.
// Every hit writes the counter, unlike the gated rule.
#ifndef BSL_RULE
#define BSL_RULE bsl_conc::ExactRule
#endif
#include "../bsl_gated/bsl_adapter.h"
