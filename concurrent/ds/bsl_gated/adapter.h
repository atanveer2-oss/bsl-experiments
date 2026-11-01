// The paper's self-adjusting skip list (Definition 1.1, the gated rule) behind
// the SetBench adapter interface. Implementation: bsl_skiplist.h; adapter:
// bsl_adapter.h. Parameters: BSL_P, BSL_K, BSL_MAX_HEIGHT (see bsl_skiplist.h).
#ifndef BSL_RULE
#define BSL_RULE bsl_conc::GatedRule
#endif
#include "bsl_adapter.h"
