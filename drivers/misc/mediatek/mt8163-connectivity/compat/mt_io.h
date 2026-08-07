/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MT8163_CONN_MT_IO_H
#define _MT8163_CONN_MT_IO_H

#include <linux/io.h>

#define mt_reg_sync_writeb(value, addr)					\
	do {								\
		writeb((value), (void __iomem *)(addr));			\
		mb();							\
	} while (0)

#define mt_reg_sync_writew(value, addr)					\
	do {								\
		writew((value), (void __iomem *)(addr));			\
		mb();							\
	} while (0)

#define mt_reg_sync_writel(value, addr)					\
	do {								\
		writel((value), (void __iomem *)(addr));			\
		mb();							\
	} while (0)

#endif
