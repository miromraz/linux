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

/*
 * A PIX SOF: its token, whether a frame was still unretired (a fault) and
 * the slot the token went to (frame and its Y address). last_y/last_c are WM3/WM4
 * STATUS_0 ("last consumed client address" in the register description),
 * read for observation only; their semantics are not relied on. diag_frm_cnt
 * is raw VFE_DIAG_SENSOR_FRM_CNT_STATUS (0xd58): CAMIF in bits 7:0,
 * RDI0/1/2 in bits 15:8/23:16/31:24. This is the exact single sample checked
 * against the nonzero 1..255 cycle for the bounded experimental PIX mode,
 * even with tracing disabled. A 255-frame loss can alias that check; these
 * separate MMIO samples are not an atomic WM/counter snapshot or DMA drain.
 */
TRACE_EVENT(camss_pix_sof,
	TP_PROTO(u8 vfe, u32 generation, u32 sequence, bool unretired, u8 frame,
		 u32 addr, u32 last_y, u32 last_c, u32 diag_frm_cnt),
	TP_ARGS(vfe, generation, sequence, unretired, frame, addr, last_y, last_c,
		diag_frm_cnt),
	TP_STRUCT__entry(
		__field(u8, vfe)
		__field(u32, generation)
		__field(u32, sequence)
		__field(bool, unretired)
		__field(u8, frame)
		__field(u32, addr)
		__field(u32, last_y)
		__field(u32, last_c)
		__field(u32, diag_frm_cnt)
	),
	TP_fast_assign(
		__entry->vfe = vfe;
		__entry->generation = generation;
		__entry->sequence = sequence;
		__entry->unretired = unretired;
		__entry->frame = frame;
		__entry->addr = addr;
		__entry->last_y = last_y;
		__entry->last_c = last_c;
		__entry->diag_frm_cnt = diag_frm_cnt;
	),
	TP_printk("vfe%u gen=%u seq=%u unretired=%u frame=%u addr=0x%08x last_y=0x%08x last_c=0x%08x diag_frm_cnt=0x%08x",
		  __entry->vfe, __entry->generation, __entry->sequence,
		  __entry->unretired, __entry->frame, __entry->addr,
		  __entry->last_y, __entry->last_c, __entry->diag_frm_cnt)
);

/*
 * An image composite done: the retired slot's frame, its Y address and
 * token; last_y/last_c are observations, diag_frm_cnt the checked raw sample.
 */
TRACE_EVENT(camss_pix_done,
	TP_PROTO(u8 vfe, u32 generation, u32 sequence, bool valid, u8 frame,
		 u32 addr, bool delivered, u32 last_y, u32 last_c, u32 diag_frm_cnt),
	TP_ARGS(vfe, generation, sequence, valid, frame, addr, delivered,
		last_y, last_c, diag_frm_cnt),
	TP_STRUCT__entry(
		__field(u8, vfe)
		__field(u32, generation)
		__field(u32, sequence)
		__field(bool, valid)
		__field(u8, frame)
		__field(u32, addr)
		__field(bool, delivered)
		__field(u32, last_y)
		__field(u32, last_c)
		__field(u32, diag_frm_cnt)
	),
	TP_fast_assign(
		__entry->vfe = vfe;
		__entry->generation = generation;
		__entry->sequence = sequence;
		__entry->valid = valid;
		__entry->frame = frame;
		__entry->addr = addr;
		__entry->delivered = delivered;
		__entry->last_y = last_y;
		__entry->last_c = last_c;
		__entry->diag_frm_cnt = diag_frm_cnt;
	),
	TP_printk("vfe%u gen=%u seq=%u valid=%u frame=%u addr=0x%08x delivered=%u last_y=0x%08x last_c=0x%08x diag_frm_cnt=0x%08x",
		  __entry->vfe, __entry->generation, __entry->sequence,
		  __entry->valid, __entry->frame, __entry->addr,
		  __entry->delivered, __entry->last_y, __entry->last_c,
		  __entry->diag_frm_cnt)
);

#endif /* _CAMSS_VFE170_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE camss-vfe-17x-trace
#include <trace/define_trace.h>
