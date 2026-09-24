// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <simbricks/base/generic.h>
#include <simbricks/base/if.h>
#ifdef __cplusplus
}
#endif

#include "protocol/ub_net/proto.h"

namespace openurma::proto::net {

struct Interface {
    SimbricksBaseIf base;
};

SIMBRICKS_BASEIF_GENERIC(UbNet, Message, Interface)

inline void DefaultParams(SimbricksBaseIfParams* params)
{
    SimbricksBaseIfDefaultParams(params);
    params->upper_layer_proto = kProtocolId;
    params->in_entries_size = params->out_entries_size = 16384 + 64;
}

} // namespace openurma::proto::net
