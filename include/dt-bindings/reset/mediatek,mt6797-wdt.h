/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Copyright (c) 2026 Miroslav Mraz
 */

#ifndef _DT_BINDINGS_RESET_MT6797_WDT_H
#define _DT_BINDINGS_RESET_MT6797_WDT_H

/*
 * TOPRGU (watchdog) software reset lines, indexed by WDT_SWSYSRST bit.
 * Only the connectivity MCU line is described; the SoC reserves lines 0..11
 * for other subsystems, so the controller exposes 13 lines.
 */
#define MT6797_TOPRGU_CONMCU_SW_RST	12

#define MT6797_TOPRGU_SW_RST_NUM	13

#endif /* _DT_BINDINGS_RESET_MT6797_WDT_H */
