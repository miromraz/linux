/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Titan 170 PIX diagnostic tracepoints
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM camss_vfe170

#if !defined(_CAMSS_VFE170_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _CAMSS_VFE170_TRACE_H

#include <linux/tracepoint.h>

/*
 * One interrupt snapshot of a PIX capable VFE: top and BUS status as read,
 * and the staging frames in the two programmed address slots at that time
 * (read without the output lock). SOF/EOF/RUP are status0 bits 0/1/4, the
 * image composite done is BUS status0 bit 5.
 */
TRACE_EVENT(camss_pix_irq,
	TP_PROTO(u8 vfe, u32 status0, u32 status1, u32 bus0, u32 bus1, u8 hw0, u8 hw1),
	TP_ARGS(vfe, status0, status1, bus0, bus1, hw0, hw1),
	TP_STRUCT__entry(
		__field(u8, vfe)
		__field(u32, status0)
		__field(u32, status1)
		__field(u32, bus0)
		__field(u32, bus1)
		__field(u8, hw0)
		__field(u8, hw1)
	),
	TP_fast_assign(
		__entry->vfe = vfe;
		__entry->status0 = status0;
		__entry->status1 = status1;
		__entry->bus0 = bus0;
		__entry->bus1 = bus1;
		__entry->hw0 = hw0;
		__entry->hw1 = hw1;
	),
	TP_printk("vfe%u st0=0x%08x st1=0x%08x bus0=0x%08x bus1=0x%08x sof=%u eof=%u rup=%u done=%u hw=%u,%u",
		  __entry->vfe, __entry->status0, __entry->status1, __entry->bus0,
		  __entry->bus1, !!(__entry->status0 & BIT(0)),
		  !!(__entry->status0 & BIT(1)), !!(__entry->status0 & BIT(4)),
		  !!(__entry->bus0 & BIT(5)), __entry->hw0, __entry->hw1)
);

#endif /* _CAMSS_VFE170_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE camss-vfe-17x-trace
#include <trace/define_trace.h>
