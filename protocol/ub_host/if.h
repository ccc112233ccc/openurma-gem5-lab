// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <simbricks/base/generic.h>
#include <simbricks/base/if.h>

#include "protocol/ub_host/proto.h"

namespace openurma::proto::host {

struct Interface {
    SimbricksBaseIf base;
};

SIMBRICKS_BASEIF_GENERIC(UbHostH2D, H2DMessage, Interface)
SIMBRICKS_BASEIF_GENERIC(UbHostD2H, D2HMessage, Interface)

inline void DefaultParams(SimbricksBaseIfParams* params)
{
    SimbricksBaseIfDefaultParams(params);
    params->upper_layer_proto = kProtocolId;
    params->in_entries_size = params->out_entries_size = 8192 + 64;
}

} // namespace openurma::proto::host
