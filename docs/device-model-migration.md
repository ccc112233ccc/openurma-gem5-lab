# UB device-model migration ledger

The migration boundary is architectural: guest software and official OpenEuler
UB/UDMA code remain in the host simulator; all device behavior lives in
`components/udma-model`; host and network simulators attach only through
UB-HOST v1 and UB-NET v1.  This ledger prevents a feature from being counted as
migrated merely because an old in-gem5 fallback still implements it.

| Hardware responsibility | Standalone model status | Evidence |
| --- | --- | --- |
| UBIOS discovery tables and 16 MiB aperture | migrated | firmware-table unit assertions |
| UBIOS enumeration/config/token message queue | migrated | asynchronous SQ/RQ/CQ DMA test |
| UMMU capability/control/command registers | migrated | CR0/ACK, GBPA and MCMDQ tests |
| UBASE CSQ capability queries | migrated | port/resource response and head-publication test |
| UBASE mailbox and AEQ completion | migrated | AEQ/JFC mailbox test, vector-1 delivery |
| JFC/JFR/JFS/Jetty context capture/destruction | migrated | context maps owned by `UdmaModel` |
| misc/AEQ/CEQ interrupt separation | migrated boundary | UB-HOST vector mapped to three gem5 pins |
| UE2UE CtrlQ/CRQ and TP lifecycle | migrated | QoS/SEID/TP response, allocation and activation tests |
| Type-1 MSI address/data programming | pending | replace pin-only compatibility delivery |
| UMMU translation tables and invalidation | pending | implement in device or negotiate translated DMA service |
| official SQ doorbell and WQE decode | SEND migrated | direct SQE and ring doorbell, official owner/opcode/SGE fields |
| SEND transmit and receive payload DMA | migrated | SQ SGE/inline DMA plus JFR PI/index/SGE receive DMA |
| remote memory semantics | RMA pending | WRITE ACK and READ request/response state machines required |
| CQE creation and CEQ production | SEND TX/RX migrated | CI check, owner bit, 64-byte CQE, CEQE and vector 2 |
| completion moderation | pending | model CEQ coalescing policy and timer |
| multi-port TP selection | migrated | control-plane round-robin TP-to-port binding |
| multi-port failover | pending | link-state event and TP rebinding policy required |
| ns-3-UB packet transport and switch model | adapter exists; integration pending | UB-NET v1 adapter source |
| QEMU host adapter | pending | implement the same UB-HOST v1 contract, no model fork |

Removal rule: an old `NICTopologySC` behavior can be deleted only after the
corresponding row is migrated and exercised through the standalone process.
The final state contains no semantic fallback in gem5 or QEMU adapters.
