// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 MediaTek Inc.
 *
 * Author:
 *  Min Guo <min.guo@mediatek.com>
 *  Yonglong Wu <yonglong.wu@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/role.h>
#include <linux/pinctrl/consumer.h>
#include <linux/usb/usb_phy_generic.h>
#include <linux/workqueue.h>
#include <linux/usb/hcd.h>
#include "musb_core.h"
#include "musb_dma.h"

#define USB_L1INTS		0x00a0
#define USB_L1INTM		0x00a4
#define MTK_MUSB_TXFUNCADDR	0x0480

/* MediaTek controller toggle enable and status reg */
#define MUSB_RXTOG		0x80
#define MUSB_RXTOGEN		0x82
#define MUSB_TXTOG		0x84
#define MUSB_TXTOGEN		0x86
#define MTK_TOGGLE_EN		GENMASK(15, 0)

#define TX_INT_STATUS		BIT(0)
#define RX_INT_STATUS		BIT(1)
#define USBCOM_INT_STATUS	BIT(2)
#define DMA_INT_STATUS		BIT(3)

#define DMA_INTR_STATUS_MSK	GENMASK(7, 0)
#define DMA_INTR_UNMASK_SET_MSK	GENMASK(31, 24)

#define MTK_MUSB_MAX_CLKS	4

struct mtk_musb_data {
	const char * const *clock_names;
	unsigned int num_clks;
	bool phy_optional;
	bool needs_soc_phy_recover;
};

static const char * const mtk_musb_clock_names[] = {
	"main", "mcu", "univpll",
};

static const char * const mt8163_musb_clock_names[] = {
	"usbpll", "usbmcu", "usb", "icusb",
};

static const struct mtk_musb_data mtk_musb_data = {
	.clock_names = mtk_musb_clock_names,
	.num_clks = ARRAY_SIZE(mtk_musb_clock_names),
};

static const struct mtk_musb_data mt8163_musb_data = {
	.clock_names = mt8163_musb_clock_names,
	.num_clks = ARRAY_SIZE(mt8163_musb_clock_names),
	.phy_optional = true,
	.needs_soc_phy_recover = true,
};

struct mtk_glue {
	struct device *dev;
	struct musb *musb;
	struct platform_device *musb_pdev;
	struct platform_device *usb_phy;
	struct phy *phy;
	struct usb_phy *xceiv;
	const struct mtk_musb_data *data;
	enum phy_mode phy_mode;
	struct clk_bulk_data clks[MTK_MUSB_MAX_CLKS];
	enum usb_role role;
	struct usb_role_switch *role_sw;
	void __iomem *phy_base;
	/* The vendor DTS carries drvvbus pin states that nothing referenced, so the
	 * pin was never muxed and the port could not raise bus VBUS.  A downstream
	 * device will not pull up D+ until it sees VBUS, so nothing enumerated
	 * however the drive itself was powered. */
	struct pinctrl *pinctrl;
	struct pinctrl_state *vbus_on;
	u8 last_devctl;
	bool devctl_seen;
	struct pinctrl_state *vbus_off;
	struct delayed_work diag_work;
	struct mutex role_lock;
	spinlock_t mode_lock;
	struct work_struct mode_work;
	enum usb_role pending_role;
	bool mode_pending;
	bool mode_work_running;
};

#define MT8163_USB_PHY_BANK_OFFSET	0x800

static u8 mt8163_usb_phy_read8(struct mtk_glue *glue, u32 offset)
{
	return readb(glue->phy_base + MT8163_USB_PHY_BANK_OFFSET + offset);
}

static void mt8163_usb_phy_write8(struct mtk_glue *glue, u32 offset, u8 value)
{
	writeb(value, glue->phy_base + MT8163_USB_PHY_BANK_OFFSET + offset);
}

static void mt8163_usb_phy_set8(struct mtk_glue *glue, u32 offset, u8 mask)
{
	mt8163_usb_phy_write8(glue, offset,
			       mt8163_usb_phy_read8(glue, offset) | mask);
}

static void mt8163_usb_phy_clr8(struct mtk_glue *glue, u32 offset, u8 mask)
{
	mt8163_usb_phy_write8(glue, offset,
			       mt8163_usb_phy_read8(glue, offset) & ~mask);
}

/*
 * MT8163 predates the upstream generic MediaTek USB2 PHY provider.  Its
 * vendor kernel released the integrated PHY from save-current state every
 * time the MUSB core enabled a cable session.  Without this sequence a NOP
 * transceiver is sufficient to register the UDC, but D+ never asserts and
 * the host cannot enumerate the gadget.
 */
static void mt8163_usb_phy_set_role(struct mtk_glue *glue, bool host);

static void mt8163_usb_phy_recover(struct mtk_glue *glue)
{
	/* VUSB and all four controller clocks are already enabled by probe. */
	udelay(50);

	/* Select USB rather than UART/GPIO mode and release forced suspend. */
	mt8163_usb_phy_clr8(glue, 0x1d, 0x10);
	mt8163_usb_phy_clr8(glue, 0x6b, 0x04);
	mt8163_usb_phy_clr8(glue, 0x6e, 0x01);
	mt8163_usb_phy_clr8(glue, 0x6a, 0x04);
	mt8163_usb_phy_clr8(glue, 0x21, 0x03);

	/* Release save-current pulldowns and all forced line-state controls. */
	mt8163_usb_phy_clr8(glue, 0x68, 0xc0);
	mt8163_usb_phy_clr8(glue, 0x68, 0x30);
	mt8163_usb_phy_clr8(glue, 0x68, 0x04);
	mt8163_usb_phy_clr8(glue, 0x69, 0x3c);
	mt8163_usb_phy_clr8(glue, 0x6a, 0xba);

	/* Disable charger detection, enable the device-side VBUS comparator. */
	mt8163_usb_phy_clr8(glue, 0x1a, 0x80);
	mt8163_usb_phy_set8(glue, 0x1a, 0x10);

	/* Vendor RX-sensitivity and device-mode settings. */
	mt8163_usb_phy_clr8(glue, 0x18, 0x08);
	mt8163_usb_phy_set8(glue, 0x18, 0x06);
	udelay(800);
	mt8163_usb_phy_clr8(glue, 0x6c, 0x10);
	mt8163_usb_phy_set8(glue, 0x6c, 0x2e);
	mt8163_usb_phy_set8(glue, 0x6d, 0x3e);
	mt8163_usb_phy_set8(glue, 0x05, 0x77);

	/* The sequence above hardcodes B-device settings, so re-apply whichever
	 * role is currently selected rather than silently reverting to device. */
	if (glue->role == USB_ROLE_HOST)
		mt8163_usb_phy_set_role(glue, true);

	dev_info(glue->dev,
		 "MT8163 USB2 PHY recovered: eye=%02x line=%02x force=%02x vbus=%02x\n",
		 mt8163_usb_phy_read8(glue, 0x05),
		 mt8163_usb_phy_read8(glue, 0x68),
		 mt8163_usb_phy_read8(glue, 0x6a),
		 mt8163_usb_phy_read8(glue, 0x1a));
}

/*
 * Select the PHY role.  mt8163_usb_phy_recover() leaves the PHY configured as
 * a B-device, which is all the vendor sequence ever needed: this SoC predates
 * the generic MediaTek USB2 PHY provider, so nothing else programs it and host
 * mode was never wired up.  The MUSB core would flip its own role and set the
 * session bit while the PHY stayed a peripheral, so a downstream device was
 * never detected however it was powered.
 *
 * The register is the same one phy-mtk-tphy.c drives in
 * u2_phy_instance_set_mode(): IDDIG is forced, and its level picks the role --
 * 0 is the A-device (host), 1 the B-device (peripheral).  U2PHYDTM1 sits at
 * 0x6c here, so P2C_RG_IDDIG (bit 1) falls in byte 0 and P2C_FORCE_IDDIG
 * (bit 9) in byte 1.
 */
#define MT8163_U2PHYDTM0		0x68
#define MT8163_U2PHYDTM0_PULLDOWN	0xc0	/* RG_DP/DM_PULLDOWN, BIT(6)|BIT(7) */
#define MT8163_U2PHYDTM0_FORCE_PULLDOWN	0x30	/* FORCE_DP/DM_PULLDOWN, BIT(20)|BIT(21) */
#define MT8163_U2PHYDTM0_RG_DATAIN	0x3c	/* P2C_RG_DATAIN, GENMASK(13,10) -> byte 1 */
#define MT8163_U2PHYDTM0_FORCE_DATAIN	0x80	/* P2C_FORCE_DATAIN, BIT(23) -> byte 2 */
#define MT8163_U2PHYDTM1		0x6c
#define MT8163_U2PHYDTM1_RG_IDDIG	0x02	/* P2C_RG_IDDIG, BIT(1) */
#define MT8163_U2PHYDTM1_RG_AVALID	0x04	/* P2C_RG_AVALID, BIT(2) */
#define MT8163_U2PHYDTM1_RG_BVALID	0x08	/* P2C_RG_BVALID, BIT(3) */
#define MT8163_U2PHYDTM1_RG_SESSEND	0x10	/* P2C_RG_SESSEND, BIT(4) */
#define MT8163_U2PHYDTM1_RG_VBUSVALID	0x20	/* P2C_RG_VBUSVALID, BIT(5) */
#define MT8163_U2PHYDTM1_FORCE_IDDIG	0x02	/* P2C_FORCE_IDDIG, BIT(9) */
#define MT8163_U2PHYDTM1_FORCE_OTG	0x3e	/* byte 1: force iddig..vbusvalid */

static void mt8163_usb_phy_set_role(struct mtk_glue *glue, bool host)
{
	mt8163_usb_phy_set8(glue, MT8163_U2PHYDTM1 + 1,
			    MT8163_U2PHYDTM1_FORCE_IDDIG);
	if (host)
		mt8163_usb_phy_clr8(glue, MT8163_U2PHYDTM1,
				    MT8163_U2PHYDTM1_RG_IDDIG);
	else
		mt8163_usb_phy_set8(glue, MT8163_U2PHYDTM1,
				    MT8163_U2PHYDTM1_RG_IDDIG);
	/*
	 * A host presents 15k pulldowns on D+/D-; the peripheral's 1.5k pullup
	 * against them is what signals an attach and selects the speed.  The
	 * recovery sequence clears both because it only ever configured a
	 * B-device, so without this a host-configured port sees no connection
	 * however the downstream device is powered.  DTM0 byte 0 holds the
	 * pulldowns and byte 2 the force bits, per phy-mtk-tphy.c.
	 */
	if (glue->pinctrl && glue->vbus_on && glue->vbus_off)
		pinctrl_select_state(glue->pinctrl,
				     host ? glue->vbus_on : glue->vbus_off);
	if (host) {
		mt8163_usb_phy_set8(glue, MT8163_U2PHYDTM0,
				    MT8163_U2PHYDTM0_PULLDOWN);
		mt8163_usb_phy_set8(glue, MT8163_U2PHYDTM0 + 2,
				    MT8163_U2PHYDTM0_FORCE_PULLDOWN);
	} else {
		mt8163_usb_phy_clr8(glue, MT8163_U2PHYDTM0,
				    MT8163_U2PHYDTM0_PULLDOWN);
		mt8163_usb_phy_clr8(glue, MT8163_U2PHYDTM0 + 2,
				    MT8163_U2PHYDTM0_FORCE_PULLDOWN);
	}
	dev_info(glue->dev,
		 "MT8163 USB2 PHY role=%s dtm1=%02x/%02x dtm0=%02x/%02x\n",
		 host ? "host" : "device",
		 mt8163_usb_phy_read8(glue, MT8163_U2PHYDTM1),
		 mt8163_usb_phy_read8(glue, MT8163_U2PHYDTM1 + 1),
		 mt8163_usb_phy_read8(glue, MT8163_U2PHYDTM0),
		 mt8163_usb_phy_read8(glue, MT8163_U2PHYDTM0 + 2));
}

static void mt8163_musb_diag_work(struct work_struct *work)
{
	struct mtk_glue *glue = container_of(to_delayed_work(work),
						struct mtk_glue, diag_work);
	struct musb *musb = glue->musb;
	void __iomem *epio;
	unsigned long flags;
	u8 index;
	u32 l1s, l1m;
	u16 intrrx, intrrxe, intrtx, intrtxe, rxcsr, rxcount, txcsr;

	if (!musb)
		return;

	/*
	 * DEVCTL every tick. The bit that matters is 7 (BDEVICE): it has read 1
	 * on every failed host attempt, and what it reads during a session that
	 * actually works has never been captured -- a drive inserted while host
	 * mode is on does enumerate, but nothing was logging then. Without that
	 * comparison there is no way to tell a misconfiguration from a status
	 * bit that simply means "no device attached yet".
	 */
	{
		u8 dc = readb(musb->mregs + MUSB_DEVCTL);

		/* Only on change: the log window is small and a line every couple
		   of seconds would push out the USB messages this exists to sit
		   beside. Transitions are the interesting part anyway. */
		if (!glue->devctl_seen || dc != glue->last_devctl) {
			glue->last_devctl = dc;
			glue->devctl_seen = true;
			dev_dbg(glue->dev,
				"MT8163 devctl=%02x bdevice=%u hostmode=%u vbus=%u session=%u role=%s\n",
				dc, (dc >> 7) & 1, (dc >> 2) & 1, (dc >> 3) & 3, dc & 1,
				glue->role == USB_ROLE_HOST ? "host" :
				glue->role == USB_ROLE_DEVICE ? "device" : "none");
		}
	}
	/* Keep watching. The worker was a one-shot, so it reported once ten
	   seconds after enable and never again. */
	schedule_delayed_work(&glue->diag_work, msecs_to_jiffies(2000));

	spin_lock_irqsave(&musb->lock, flags);
	l1s = musb_readl(musb->mregs, USB_L1INTS);
	l1m = musb_readl(musb->mregs, USB_L1INTM);
	intrrx = musb_readw(musb->mregs, MUSB_INTRRX);
	intrrxe = musb_readw(musb->mregs, MUSB_INTRRXE);
	intrtx = musb_readw(musb->mregs, MUSB_INTRTX);
	intrtxe = musb_readw(musb->mregs, MUSB_INTRTXE);
	index = musb_readb(musb->mregs, MUSB_INDEX);
	musb_ep_select(musb->mregs, 1);
	epio = musb->endpoints[1].regs;
	rxcsr = musb_readw(epio, MUSB_RXCSR);
	rxcount = musb_readw(epio, MUSB_RXCOUNT);
	txcsr = musb_readw(epio, MUSB_TXCSR);
	musb_ep_select(musb->mregs, index);
	spin_unlock_irqrestore(&musb->lock, flags);

	dev_dbg(glue->dev,
		"MT8163 MUSB delayed diag: l1=%08x/%08x rx=%04x/%04x tx=%04x/%04x ep1-rxcsr=%04x rxcount=%04x txcsr=%04x\n",
		l1s, l1m, intrrx, intrrxe, intrtx, intrtxe,
		rxcsr, rxcount, txcsr);
}

static int mtk_musb_clks_get(struct mtk_glue *glue)
{
	struct device *dev = glue->dev;
	unsigned int i;

	for (i = 0; i < glue->data->num_clks; i++)
		glue->clks[i].id = glue->data->clock_names[i];

	return devm_clk_bulk_get(dev, glue->data->num_clks, glue->clks);
}

/*
 * Notice a device that was already attached when host mode started.
 *
 * MUSB learns about a peripheral from a CONNECT interrupt, which fires on the
 * edge of the device asserting its D+ pullup -- and a device only does that
 * when it sees VBUS *rise*. On this board VBUS is supplied externally, so a
 * drive left plugged in is already powered, already pulled up, and generates
 * no edge however many times the role is switched. The port then sits empty
 * until the drive is physically unplugged and replugged.
 *
 * Rather than wait for an edge that cannot happen, do deliberately what the
 * connect handler does: arm the endpoint interrupts, mark the root hub port
 * connected with its change bit, and let the USB core probe. If nothing is
 * actually there the core resets an empty port, fails enumeration and clears
 * the status again, which is noisy in the log but harmless.
 */
/*
 * Restart the session with the PHY's OTG inputs forced through a full
 * off/on cycle -- the way the vendor kernel enters host mode
 * (usb20_host.c, musb_id_pin_work: "for no VBUS sensing IP").  This MUSB
 * has no VBUS comparator of its own: DEVCTL's role and VBUS bits are
 * computed entirely from the UTMI signals the PHY is told to report.
 *
 * The core latches A- versus B-device only when a session *starts*.  On
 * this board VBUS is external and permanent, so BVALID/VBUSVALID have
 * been forced high since the gadget first came up: DEVCTL.SESSION is
 * already 1, no session edge can ever happen, and the role latch keeps
 * its boot value -- bdevice=1, hostmode=0 -- no matter what IDDIG says
 * afterwards.  Clearing DEVCTL.SESSION alone cannot end the session
 * either: for a B-device the bit is hardware-owned and simply re-asserts
 * while the PHY still reports a valid session.
 *
 * So do exactly what the vendor does.  Report SessEnd=1 with every valid
 * signal low (the vendor's "USB MAC OFF" state) so the session genuinely
 * ends, wait for the FSM to settle, then request a session and raise the
 * A-side valid levels with IDDIG still forced low.  The core starts a
 * fresh session as the A-device, samples the line, finds the resident
 * drive's D+ pullup already there, and raises a real CONNECT interrupt
 * -- no attach edge required.
 */
static void mt8163_musb_restart_session_as_host(struct mtk_glue *glue)
{
	struct musb *musb = glue->musb;
	u8 devctl;
	int i;

	devctl = readb(musb->mregs + MUSB_DEVCTL);
	musb_writeb(musb->mregs, MUSB_DEVCTL, devctl & ~MUSB_DEVCTL_SESSION);

	/* USB MAC OFF: VBUSVALID=0, AVALID=0, BVALID=0, SESSEND=1, IDDIG=0 */
	mt8163_usb_phy_set8(glue, MT8163_U2PHYDTM1,
			    MT8163_U2PHYDTM1_RG_SESSEND);
	mt8163_usb_phy_clr8(glue, MT8163_U2PHYDTM1,
			    MT8163_U2PHYDTM1_RG_IDDIG |
			    MT8163_U2PHYDTM1_RG_AVALID |
			    MT8163_U2PHYDTM1_RG_BVALID |
			    MT8163_U2PHYDTM1_RG_VBUSVALID);
	mt8163_usb_phy_set8(glue, MT8163_U2PHYDTM1 + 1,
			    MT8163_U2PHYDTM1_FORCE_OTG);
	usleep_range(5000, 6000);

	devctl = readb(musb->mregs + MUSB_DEVCTL);
	musb_writeb(musb->mregs, MUSB_DEVCTL, devctl | MUSB_DEVCTL_SESSION);

	/* USB MAC ON, host: VBUSVALID=1, AVALID=1, BVALID=1, SESSEND=0, IDDIG=0 */
	mt8163_usb_phy_clr8(glue, MT8163_U2PHYDTM1,
			    MT8163_U2PHYDTM1_RG_SESSEND);
	mt8163_usb_phy_set8(glue, MT8163_U2PHYDTM1,
			    MT8163_U2PHYDTM1_RG_AVALID |
			    MT8163_U2PHYDTM1_RG_BVALID |
			    MT8163_U2PHYDTM1_RG_VBUSVALID);
	mt8163_usb_phy_set8(glue, MT8163_U2PHYDTM1 + 1,
			    MT8163_U2PHYDTM1_FORCE_OTG);

	/* The role latch is what every failed attempt got wrong; watch it. */
	for (i = 0; i < 50; i++) {
		devctl = readb(musb->mregs + MUSB_DEVCTL);
		if (!(devctl & MUSB_DEVCTL_BDEVICE))
			break;
		usleep_range(2000, 3000);
	}
	dev_info(glue->dev,
		 "MT8163 host session restart: devctl=%02x bdevice=%u hostmode=%u vbus=%u session=%u polls=%d\n",
		 devctl, (devctl >> 7) & 1, (devctl >> 2) & 1,
		 (devctl >> 3) & 3, devctl & 1, i);
}

static void mt8163_musb_rescan_port(struct mtk_glue *glue)
{
	struct musb *musb = glue->musb;
	u8 devctl;

	if (!musb || !musb->hcd)
		return;

	/*
	 * Never disturb a port that already has a device on it. Asserting the
	 * connect-change bit here is read by the core as "something changed",
	 * so on an occupied port it tears the working device down and then has
	 * to enumerate it again from scratch -- which fails, because the drive
	 * is already powered and will not re-present itself without a VBUS
	 * cycle this board cannot perform. Observed as an immediate
	 * "USB disconnect, device number N" followed by descriptor timeouts.
	 */
	if (musb->port1_status & USB_PORT_STAT_CONNECTION) {
		dev_info(glue->dev,
			 "MT8163 rescan skipped: port already has a device\n");
		return;
	}

	devctl = readb(musb->mregs + MUSB_DEVCTL);

	musb->is_active = 1;
	musb->ep0_stage = MUSB_EP0_START;

	musb->intrtxe = musb->epmask;
	musb_writew(musb->mregs, MUSB_INTRTXE, musb->intrtxe);
	musb->intrrxe = musb->epmask & 0xfffe;
	musb_writew(musb->mregs, MUSB_INTRRXE, musb->intrrxe);
	musb_writeb(musb->mregs, MUSB_INTRUSBE, 0xf7);

	musb->port1_status &= ~(USB_PORT_STAT_LOW_SPEED
				| USB_PORT_STAT_HIGH_SPEED
				| USB_PORT_STAT_ENABLE);
	musb->port1_status |= USB_PORT_STAT_CONNECTION
				| (USB_PORT_STAT_C_CONNECTION << 16);
	if (devctl & MUSB_DEVCTL_LSDEV)
		musb->port1_status |= USB_PORT_STAT_LOW_SPEED;

	musb_set_state(musb, OTG_STATE_A_HOST);
	musb->is_active = 1;

	dev_info(glue->dev, "MT8163 rescan: port1_status=%08x devctl=%02x rh_state=%d\n",
		 musb->port1_status, devctl, musb->hcd->state);

	/*
	 * Wake the root hub first. usb_hcd_poll_rh_status() returns straight
	 * away unless a status URB is outstanding, and a suspended root hub has
	 * none -- so polling on its own did nothing at all and the core never
	 * even attempted a port reset.
	 */
	usb_hcd_resume_root_hub(musb->hcd);
	usb_hcd_poll_rh_status(musb->hcd);
}

static void mtk_musb_restore_gadget_pullup(struct musb *musb)
{
	u8 power;

	/* Preserve the gadget driver's requested connection across host mode. */
	if (!musb->softconnect)
		return;

	power = readb(musb->mregs + MUSB_POWER);
	if (!(power & MUSB_POWER_SOFTCONN)) {
		power |= MUSB_POWER_SOFTCONN;
		musb_writeb(musb->mregs, MUSB_POWER, power);
	}
}

static int mtk_otg_switch_set(struct mtk_glue *glue, enum usb_role role)
{
	struct musb *musb = glue->musb;
	u8 devctl;
	enum usb_role new_role;
	int ret;

	mutex_lock(&glue->role_lock);
	devctl = readb(musb->mregs + MUSB_DEVCTL);
	if (role == glue->role) {
		ret = 0;
		goto out;
	}

	switch (role) {
	case USB_ROLE_HOST:
		/*
		 * A_WAIT_BCON, not A_WAIT_VRISE.
		 *
		 * musb_start() decides host or peripheral like this:
		 *
		 *   if (port_mode != MUSB_HOST &&
		 *       state != OTG_STATE_A_WAIT_BCON &&
		 *       (devctl & VBUS) == VBUS)   -> act as a peripheral
		 *   else                           -> start a host session
		 *
		 * With dr_mode "otg" and VBUS already present -- which it is here,
		 * because the bus is powered externally -- the first branch wins and
		 * the controller settles as a B-device, which is exactly the
		 * devctl bdevice=1 seen on every failed attempt. A_WAIT_BCON is the
		 * documented escape: it means "host, waiting for a device to
		 * connect", which is precisely the situation.
		 */
		musb->xceiv->otg->state = OTG_STATE_A_WAIT_BCON;
		glue->phy_mode = PHY_MODE_USB_HOST;
		new_role = USB_ROLE_HOST;
		if (glue->phy && glue->role == USB_ROLE_NONE)
			phy_power_on(glue->phy);

		/*
		 * Configure the PHY for host *before* starting the session.
		 * The role write below used to happen after MUSB_DEVCTL_SESSION,
		 * so the controller sampled VBUS while the PHY was still wired
		 * as a B-device -- it read the line as invalid and parked in
		 * A_WAIT_VRISE waiting for a rise that had already happened.
		 */
		if (glue->data->needs_soc_phy_recover && glue->phy_base)
			mt8163_usb_phy_set_role(glue, true);

		/*
		 * Drop the gadget's D+ pullup before becoming a host.
		 *
		 * MUSB_POWER_SOFTCONN is the peripheral-side 1.5k pullup, set by
		 * musb_pullup() when a gadget binds -- adbd, here. Nothing in the
		 * role switch ever cleared it, so the port asserted a pullup while
		 * simultaneously presenting host pulldowns: a host and a device on
		 * the same wire. A drive plugged into that sees no clean attach and
		 * answers nothing, which matches the descriptor timeouts observed.
		 */
		{
			u8 power = readb(musb->mregs + MUSB_POWER);

			if (power & MUSB_POWER_SOFTCONN) {
				power &= ~MUSB_POWER_SOFTCONN;
				musb_writeb(musb->mregs, MUSB_POWER, power);
				dev_info(glue->dev,
					 "MT8163 dropped gadget D+ pullup, power=%02x\n",
					 readb(musb->mregs + MUSB_POWER));
			}
		}

		/*
		 * Let the forced IDDIG settle before the session starts. MUSB
		 * latches the A/B decision from the PHY when the session bit is
		 * written; reading DEVCTL straight after the role write showed
		 * B_DEVICE still set, i.e. the controller had sampled the old
		 * value and would never enumerate.
		 */
		usleep_range(5000, 8000);

		/*
		 * Re-run the controller's own start path rather than only poking
		 * DEVCTL: it re-enables interrupts, sets POWER (including HSENAB,
		 * so high speed is offered) and then starts the session with the
		 * host semantics selected above.
		 */
		MUSB_HST_MODE(musb);
		/*
		 * musb_start() runs musb_platform_enable(), which on this SoC is
		 * the PHY recovery sequence -- and that clears the D+/D- pulldowns
		 * and only restores host settings when glue->role already says
		 * host. glue->role is assigned further down, so the first version
		 * of this left the port with dtm0=00/00: no host termination at
		 * all. Claim the role first, then re-assert after, so recovery
		 * cannot leave the PHY configured as a peripheral.
		 */
		glue->role = USB_ROLE_HOST;
		musb_start(musb);
		/*
		 * Everything below is the MT8163 session-edge workaround, and
		 * all of it reaches the SoC PHY through glue->phy_base.  The
		 * MT2701 and MT7623 OTG nodes in this tree bind through the
		 * generic "mediatek,mtk-musb" fallback, where
		 * needs_soc_phy_recover is false and phy_base was never mapped,
		 * so the first PHY access here would fault the kernel on a role
		 * switch.  musb_start() above is the generic path, and is all
		 * those controllers need.
		 */
		if (glue->data->needs_soc_phy_recover && glue->phy_base) {
			mt8163_usb_phy_set_role(glue, true);
			/*
			 * musb_start() folds its clear-then-maybe-set of the
			 * session bit into one register write, so when SESSION
			 * already reads 1 -- which it always does here, the
			 * forced BVALID has kept a B-session alive since boot --
			 * the controller never sees a session edge and never
			 * re-latches the role.  Cycle the session for real, with
			 * the PHY reporting session-end so the clear actually
			 * sticks.
			 */
			mt8163_musb_restart_session_as_host(glue);
			/*
			 * If the restart raised CONNECT the port is already
			 * occupied and the rescan below returns without touching
			 * it; it stays only as a fallback for a controller that
			 * latched host mode without noticing the resident pullup.
			 */
			msleep(100);
			mt8163_musb_rescan_port(glue);
		}
		break;
	case USB_ROLE_DEVICE:
		musb->xceiv->otg->state = OTG_STATE_B_IDLE;
		glue->phy_mode = PHY_MODE_USB_DEVICE;
		new_role = USB_ROLE_DEVICE;
		devctl &= ~MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		if (glue->phy && glue->role == USB_ROLE_NONE)
			phy_power_on(glue->phy);

		MUSB_DEV_MODE(musb);
		mtk_musb_restore_gadget_pullup(musb);
		break;
	case USB_ROLE_NONE:
		glue->phy_mode = PHY_MODE_USB_OTG;
		new_role = USB_ROLE_NONE;
		devctl &= ~MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		if (glue->phy && glue->role != USB_ROLE_NONE)
			phy_power_off(glue->phy);

		break;
	default:
		dev_err(glue->dev, "Invalid State\n");
		ret = -EINVAL;
		goto out;
	}

	glue->role = new_role;
	if (glue->data->needs_soc_phy_recover && glue->phy_base &&
	    new_role != USB_ROLE_HOST)
		mt8163_usb_phy_set_role(glue, false);
	/*
	 * What the controller actually sees. DEVCTL bits 4:3 are the VBUS
	 * level: 3 means above VBusValid, 0 means below SessionEnd. Without
	 * this the only evidence was the FSM state, which cannot distinguish
	 * "no VBUS" from "VBUS present but never rose".
	 */
	dev_info(glue->dev, "MT8163 role=%s devctl=%02x vbus_level=%u session=%u\n",
		 new_role == USB_ROLE_HOST ? "host" :
		 new_role == USB_ROLE_DEVICE ? "device" : "none",
		 readb(musb->mregs + MUSB_DEVCTL),
		 (readb(musb->mregs + MUSB_DEVCTL) >> 3) & 3,
		 readb(musb->mregs + MUSB_DEVCTL) & 1);
	if (glue->phy)
		phy_set_mode(glue->phy, glue->phy_mode);

	ret = 0;
out:
	mutex_unlock(&glue->role_lock);
	return ret;
}

static int musb_usb_role_sx_set(struct usb_role_switch *sw, enum usb_role role)
{
	return mtk_otg_switch_set(usb_role_switch_get_drvdata(sw), role);
}

static enum usb_role musb_usb_role_sx_get(struct usb_role_switch *sw)
{
	struct mtk_glue *glue = usb_role_switch_get_drvdata(sw);

	return glue->role;
}

static int mtk_otg_switch_init(struct mtk_glue *glue)
{
	struct usb_role_switch_desc role_sx_desc = { 0 };

	role_sx_desc.set = musb_usb_role_sx_set;
	role_sx_desc.get = musb_usb_role_sx_get;
	role_sx_desc.allow_userspace_control = true;
	role_sx_desc.fwnode = dev_fwnode(glue->dev);
	role_sx_desc.driver_data = glue;
	glue->role_sw = usb_role_switch_register(glue->dev, &role_sx_desc);

	return PTR_ERR_OR_ZERO(glue->role_sw);
}

static void mtk_otg_switch_exit(struct mtk_glue *glue)
{
	return usb_role_switch_unregister(glue->role_sw);
}

static irqreturn_t generic_interrupt(int irq, void *__hci)
{
	unsigned long flags;
	irqreturn_t retval = IRQ_NONE;
	struct musb *musb = __hci;

	spin_lock_irqsave(&musb->lock, flags);
	musb->int_usb = musb_clearb(musb->mregs, MUSB_INTRUSB);
	musb->int_rx = musb_clearw(musb->mregs, MUSB_INTRRX);
	musb->int_tx = musb_clearw(musb->mregs, MUSB_INTRTX);

	if (musb->int_rx || (musb->int_tx & ~BIT(0)))
		dev_dbg_ratelimited(musb->controller,
			"MT8163 MUSB IRQ diag: usb=%02x rx=%04x/%04x tx=%04x/%04x\n",
			musb->int_usb, musb->int_rx,
			musb_readw(musb->mregs, MUSB_INTRRXE),
			musb->int_tx,
			musb_readw(musb->mregs, MUSB_INTRTXE));

	if ((musb->int_usb & MUSB_INTR_RESET) && !is_host_active(musb)) {
		/* ep0 FADDR must be 0 when (re)entering peripheral mode */
		musb_ep_select(musb->mregs, 0);
		musb_writeb(musb->mregs, MUSB_FADDR, 0);
	}

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		retval = musb_interrupt(musb);

	spin_unlock_irqrestore(&musb->lock, flags);

	return retval;
}

static irqreturn_t mtk_musb_interrupt(int irq, void *dev_id)
{
	irqreturn_t retval = IRQ_NONE;
	struct musb *musb = (struct musb *)dev_id;
	u32 l1_ints;

	l1_ints = musb_readl(musb->mregs, USB_L1INTS) &
			musb_readl(musb->mregs, USB_L1INTM);

	if (l1_ints & (TX_INT_STATUS | RX_INT_STATUS | USBCOM_INT_STATUS))
		retval = generic_interrupt(irq, musb);

	/* Issue #8: keep a bounded diagnostic on genuine error paths.  An
	 * interrupt was signalled through the MT8163 L1 status but no MUSB
	 * handler claimed it; warn from this platform wrapper so the generic,
	 * shared musb_interrupt() core stays free of platform-specific
	 * diagnostics for the other glue drivers. */
	if (retval == IRQ_NONE &&
	    (l1_ints & (TX_INT_STATUS | RX_INT_STATUS | USBCOM_INT_STATUS)))
		dev_warn_ratelimited(musb->controller,
			"MT8163 MUSB unhandled IRQ: l1=%08x usb=%02x rx=%04x tx=%04x\n",
			l1_ints, musb->int_usb, musb->int_rx, musb->int_tx);

#if defined(CONFIG_USB_INVENTRA_DMA)
	if (l1_ints & DMA_INT_STATUS)
		retval = dma_controller_irq(irq, musb->dma_controller);
#endif
	return retval;
}

static u32 mtk_musb_busctl_offset(u8 epnum, u16 offset)
{
	return MTK_MUSB_TXFUNCADDR + offset + 8 * epnum;
}

static u8 mtk_musb_clearb(void __iomem *addr, unsigned int offset)
{
	u8 data;

	/* W1C */
	data = musb_readb(addr, offset);
	musb_writeb(addr, offset, data);
	return data;
}

static u16 mtk_musb_clearw(void __iomem *addr, unsigned int offset)
{
	u16 data;

	/* W1C */
	data = musb_readw(addr, offset);
	musb_writew(addr, offset, data);
	return data;
}

static void mtk_musb_mode_work(struct work_struct *work)
{
	struct mtk_glue *glue = container_of(work, struct mtk_glue, mode_work);
	enum usb_role role;
	unsigned long flags;
	int ret;

	for (;;) {
		spin_lock_irqsave(&glue->mode_lock, flags);
		if (!glue->mode_pending) {
			glue->mode_work_running = false;
			spin_unlock_irqrestore(&glue->mode_lock, flags);
			return;
		}
		role = glue->pending_role;
		glue->mode_pending = false;
		spin_unlock_irqrestore(&glue->mode_lock, flags);

		ret = mtk_otg_switch_set(glue, role);
		if (ret)
			dev_err(glue->dev, "deferred role switch to %d failed: %d\n",
				role, ret);
	}
}

static int mtk_musb_set_mode(struct musb *musb, u8 mode)
{
	struct device *dev = musb->controller;
	struct mtk_glue *glue = dev_get_drvdata(dev->parent);
	enum usb_role new_role;
	unsigned long flags;
	bool queue_work = false;

	switch (mode) {
	case MUSB_HOST:
		new_role = USB_ROLE_HOST;
		break;
	case MUSB_PERIPHERAL:
		new_role = USB_ROLE_DEVICE;
		break;
	case MUSB_OTG:
		new_role = USB_ROLE_NONE;
		break;
	default:
		dev_err(glue->dev, "Invalid mode request\n");
		return -EINVAL;
	}

	if (musb->port_mode != MUSB_OTG) {
		dev_err(glue->dev, "Does not support changing modes\n");
		return -EINVAL;
	}

	/* mode_store() holds musb->lock with IRQs disabled; do not sleep here. */
	spin_lock_irqsave(&glue->mode_lock, flags);
	glue->pending_role = new_role;
	glue->mode_pending = true;
	if (!glue->mode_work_running) {
		glue->mode_work_running = true;
		queue_work = true;
	}
	spin_unlock_irqrestore(&glue->mode_lock, flags);

	if (queue_work)
		schedule_work(&glue->mode_work);
	return 0;
}

static int mtk_musb_init(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct mtk_glue *glue = dev_get_drvdata(dev->parent);
	int ret;

	glue->musb = musb;
	musb->phy = glue->phy;
	musb->xceiv = glue->xceiv;
	musb->is_host = false;
	musb->isr = mtk_musb_interrupt;

	/* Set TX/RX toggle enable */
	musb_writew(musb->mregs, MUSB_TXTOGEN, MTK_TOGGLE_EN);
	musb_writew(musb->mregs, MUSB_RXTOGEN, MTK_TOGGLE_EN);

	if (musb->port_mode == MUSB_OTG) {
		ret = mtk_otg_switch_init(glue);
		if (ret)
			return ret;
	}

	if (glue->phy) {
		ret = phy_init(glue->phy);
		if (ret)
			goto err_phy_init;

		ret = phy_power_on(glue->phy);
		if (ret)
			goto err_phy_power_on;

		phy_set_mode(glue->phy, glue->phy_mode);
	}

#if defined(CONFIG_USB_INVENTRA_DMA)
	musb_writel(musb->mregs, MUSB_HSDMA_INTR,
		    DMA_INTR_STATUS_MSK | DMA_INTR_UNMASK_SET_MSK);
#endif
	musb_writel(musb->mregs, USB_L1INTM, TX_INT_STATUS | RX_INT_STATUS |
		    USBCOM_INT_STATUS | DMA_INT_STATUS);
	return 0;

err_phy_power_on:
	phy_exit(glue->phy);
err_phy_init:
	if (musb->port_mode == MUSB_OTG)
		mtk_otg_switch_exit(glue);
	return ret;
}

static void mtk_musb_enable(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct mtk_glue *glue = dev_get_drvdata(dev->parent);

	if (glue->data->needs_soc_phy_recover) {
		mt8163_usb_phy_recover(glue);
		schedule_delayed_work(&glue->diag_work, msecs_to_jiffies(10000));
	}
}

static u16 mtk_musb_get_toggle(struct musb_qh *qh, int is_out)
{
	struct musb *musb = qh->hw_ep->musb;
	u8 epnum = qh->hw_ep->epnum;
	u16 toggle;

	toggle = musb_readw(musb->mregs, is_out ? MUSB_TXTOG : MUSB_RXTOG);
	return toggle & (1 << epnum);
}

static u16 mtk_musb_set_toggle(struct musb_qh *qh, int is_out, struct urb *urb)
{
	struct musb *musb = qh->hw_ep->musb;
	u8 epnum = qh->hw_ep->epnum;
	u16 value, toggle;

	toggle = usb_gettoggle(urb->dev, qh->epnum, is_out);

	if (is_out) {
		value = musb_readw(musb->mregs, MUSB_TXTOG);
		value |= toggle << epnum;
		musb_writew(musb->mregs, MUSB_TXTOG, value);
	} else {
		value = musb_readw(musb->mregs, MUSB_RXTOG);
		value |= toggle << epnum;
		musb_writew(musb->mregs, MUSB_RXTOG, value);
	}

	return 0;
}

static int mtk_musb_exit(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct mtk_glue *glue = dev_get_drvdata(dev->parent);

	cancel_work_sync(&glue->mode_work);
	if (musb->port_mode == MUSB_OTG)
		mtk_otg_switch_exit(glue);
	if (glue->phy) {
		phy_power_off(glue->phy);
		phy_exit(glue->phy);
	}
	clk_bulk_disable_unprepare(glue->data->num_clks, glue->clks);

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	return 0;
}

static const struct musb_platform_ops mtk_musb_ops = {
	.quirks = MUSB_DMA_INVENTRA,
	.init = mtk_musb_init,
	.enable = mtk_musb_enable,
	.get_toggle = mtk_musb_get_toggle,
	.set_toggle = mtk_musb_set_toggle,
	.exit = mtk_musb_exit,
#ifdef CONFIG_USB_INVENTRA_DMA
	.dma_init = musbhs_dma_controller_create_noirq,
	.dma_exit = musbhs_dma_controller_destroy,
#endif
	.clearb = mtk_musb_clearb,
	.clearw = mtk_musb_clearw,
	.busctl_offset = mtk_musb_busctl_offset,
	.set_mode = mtk_musb_set_mode,
};

#define MTK_MUSB_MAX_EP_NUM	8
#define MTK_MUSB_RAM_BITS	11

static struct musb_fifo_cfg mtk_musb_mode_cfg[] = {
	{ .hw_ep_num = 1, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 1, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 5, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 5, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 6, .style = FIFO_TX, .maxpacket = 1024, },
	{ .hw_ep_num = 6, .style = FIFO_RX, .maxpacket = 1024, },
	{ .hw_ep_num = 7, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 7, .style = FIFO_RX, .maxpacket = 64, },
};

static const struct musb_hdrc_config mtk_musb_hdrc_config = {
	.fifo_cfg = mtk_musb_mode_cfg,
	.fifo_cfg_size = ARRAY_SIZE(mtk_musb_mode_cfg),
	.multipoint = true,
	.dyn_fifo = true,
	.num_eps = MTK_MUSB_MAX_EP_NUM,
	.ram_bits = MTK_MUSB_RAM_BITS,
};

static const struct platform_device_info mtk_dev_info = {
	.name = "musb-hdrc",
	.id = PLATFORM_DEVID_AUTO,
	.dma_mask = DMA_BIT_MASK(32),
};

static int mtk_musb_probe(struct platform_device *pdev)
{
	struct musb_hdrc_platform_data *pdata;
	struct mtk_glue *glue;
	struct platform_device_info pinfo;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int ret;

	glue = devm_kzalloc(dev, sizeof(*glue), GFP_KERNEL);
	if (!glue)
		return -ENOMEM;

	glue->dev = dev;
	glue->data = device_get_match_data(dev);
	if (!glue->data)
		return -EINVAL;
	mutex_init(&glue->role_lock);
	spin_lock_init(&glue->mode_lock);
	INIT_WORK(&glue->mode_work, mtk_musb_mode_work);
	INIT_DELAYED_WORK(&glue->diag_work, mt8163_musb_diag_work);
	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret) {
		dev_err(dev, "failed to create child devices at %p\n", np);
		return ret;
	}

	ret = mtk_musb_clks_get(glue);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	if (glue->data->needs_soc_phy_recover) {
		glue->phy_base = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(glue->phy_base))
			return PTR_ERR(glue->phy_base);

		glue->pinctrl = devm_pinctrl_get(dev);
		if (!IS_ERR(glue->pinctrl)) {
			struct pinctrl_state *init;

			init = pinctrl_lookup_state(glue->pinctrl, "drvvbus_init");
			if (!IS_ERR(init))
				pinctrl_select_state(glue->pinctrl, init);
			glue->vbus_on = pinctrl_lookup_state(glue->pinctrl,
							     "drvvbus_high");
			glue->vbus_off = pinctrl_lookup_state(glue->pinctrl,
							      "drvvbus_low");
			if (IS_ERR(glue->vbus_on) || IS_ERR(glue->vbus_off)) {
				glue->vbus_on = NULL;
				glue->vbus_off = NULL;
				dev_info(dev, "no drvvbus pin states; the port cannot source VBUS\n");
			}
		} else {
			glue->pinctrl = NULL;
		}

		ret = devm_regulator_get_enable(dev, "vusb");
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to enable vusb supply\n");
	}

	pdata->config = &mtk_musb_hdrc_config;
	pdata->platform_ops = &mtk_musb_ops;
	pdata->mode = usb_get_dr_mode(dev);

	if (IS_ENABLED(CONFIG_USB_MUSB_HOST))
		pdata->mode = USB_DR_MODE_HOST;
	else if (IS_ENABLED(CONFIG_USB_MUSB_GADGET))
		pdata->mode = USB_DR_MODE_PERIPHERAL;

	switch (pdata->mode) {
	case USB_DR_MODE_HOST:
		glue->phy_mode = PHY_MODE_USB_HOST;
		glue->role = USB_ROLE_HOST;
		break;
	case USB_DR_MODE_PERIPHERAL:
		glue->phy_mode = PHY_MODE_USB_DEVICE;
		glue->role = USB_ROLE_DEVICE;
		break;
	case USB_DR_MODE_OTG:
		glue->phy_mode = PHY_MODE_USB_OTG;
		glue->role = USB_ROLE_NONE;
		break;
	default:
		dev_err(&pdev->dev, "Error 'dr_mode' property\n");
		return -EINVAL;
	}

	if (glue->data->phy_optional)
		glue->phy = devm_phy_optional_get(dev, "usb");
	else
		glue->phy = devm_of_phy_get_by_index(dev, np, 0);
	if (IS_ERR(glue->phy)) {
		dev_err(dev, "fail to getting phy %ld\n",
			PTR_ERR(glue->phy));
		return PTR_ERR(glue->phy);
	}

	glue->usb_phy = usb_phy_generic_register();
	if (IS_ERR(glue->usb_phy)) {
		dev_err(dev, "fail to registering usb-phy %ld\n",
			PTR_ERR(glue->usb_phy));
		return PTR_ERR(glue->usb_phy);
	}

	glue->xceiv = devm_usb_get_phy(dev, USB_PHY_TYPE_USB2);
	if (IS_ERR(glue->xceiv)) {
		ret = PTR_ERR(glue->xceiv);
		dev_err(dev, "fail to getting usb-phy %d\n", ret);
		goto err_unregister_usb_phy;
	}

	platform_set_drvdata(pdev, glue);
	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	ret = clk_bulk_prepare_enable(glue->data->num_clks, glue->clks);
	if (ret) {
		dev_err_probe(dev, ret, "failed to enable clocks\n");
		goto err_enable_clk;
	}

	pinfo = mtk_dev_info;
	pinfo.parent = dev;
	pinfo.res = pdev->resource;
	pinfo.num_res = pdev->num_resources;
	pinfo.data = pdata;
	pinfo.size_data = sizeof(*pdata);
	pinfo.fwnode = of_fwnode_handle(np);
	pinfo.of_node_reused = true;

	glue->musb_pdev = platform_device_register_full(&pinfo);
	if (IS_ERR(glue->musb_pdev)) {
		ret = PTR_ERR(glue->musb_pdev);
		dev_err(dev, "failed to register musb device: %d\n", ret);
		goto err_device_register;
	}

	return 0;

err_device_register:
	clk_bulk_disable_unprepare(glue->data->num_clks, glue->clks);
err_enable_clk:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
err_unregister_usb_phy:
	usb_phy_generic_unregister(glue->usb_phy);
	return ret;
}

static int mtk_musb_remove(struct platform_device *pdev)
{
	struct mtk_glue *glue = platform_get_drvdata(pdev);
	struct platform_device *usb_phy = glue->usb_phy;

	cancel_delayed_work_sync(&glue->diag_work);
	platform_device_unregister(glue->musb_pdev);
	usb_phy_generic_unregister(usb_phy);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id mtk_musb_match[] = {
	{ .compatible = "mediatek,mt8163-usb20", .data = &mt8163_musb_data },
	{ .compatible = "mediatek,mtk-musb", .data = &mtk_musb_data },
	{},
};
MODULE_DEVICE_TABLE(of, mtk_musb_match);
#endif

static struct platform_driver mtk_musb_driver = {
	.probe = mtk_musb_probe,
	.remove = mtk_musb_remove,
	.driver = {
		   .name = "musb-mtk",
		   .of_match_table = of_match_ptr(mtk_musb_match),
	},
};

module_platform_driver(mtk_musb_driver);

MODULE_DESCRIPTION("MediaTek MUSB Glue Layer");
MODULE_AUTHOR("Min Guo <min.guo@mediatek.com>");
MODULE_LICENSE("GPL v2");
