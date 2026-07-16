// SPDX-License-Identifier: GPL-2.0
/*
 *  UART driver for Cortina-Access Soc platform
 *  Copyright (C) 2021 Cortina-Access Inc.
 */
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/sysrq.h>
#include <linux/console.h>
#include <linux/serial_core.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

/***************************************
 *	UART Related registers
 ****************************************/
/* register definitions */
#define	CFG			0x00
#define	FC			0x04
#define	RX_SAMPLE		0x08
#define	RT_TUNE			0x0C
#define	TX_DAT			0x10
#define	RX_DAT			0x14
#define	INFO			0x18
#define	IE			0x1C
#define	INT			0x24
#define	STATUS			0x2C

/* CFG */
#define	CFG_STOP_2BIT		BIT(2)
#define	CFG_PARITY_EVEN	BIT(3)
#define	CFG_PARITY_EN		BIT(4)
#define	CFG_TX_EN		BIT(5)
#define	CFG_RX_EN		BIT(6)
#define	CFG_UART_EN		BIT(7)
#define	CFG_BAUD_SART_SHIFT	8

/* INFO */
#define	INFO_TX_EMPTY		BIT(3)
#define	INFO_TX_FULL		BIT(2)
#define	INFO_RX_EMPTY		BIT(1)
#define	INFO_RX_FULL		BIT(0)

/* Interrupt */
#define	RX_BREAK		BIT(7)
#define	RX_FIFO_NONEMPTYE	BIT(6)
#define	TX_FIFO_EMPTYE		BIT(5)
#define	RX_FIFO_UNDERRUNE	BIT(4)
#define	RX_FIFO_OVERRUNE	BIT(3)
#define	RX_PARITY_ERRE		BIT(2)
#define	RX_STOP_ERRE		BIT(1)
#define	TX_FIFO_OVERRUNE	BIT(0)

#define TX_TIMEOUT		5000
#define UART_NR 4
struct ca_uart_port {
	struct uart_port uart;
	char name[32];
	char has_bi;
	unsigned int may_wakeup;
};

static struct ca_uart_port *ca_uart_ports;

static irqreturn_t ca_uart_interrupt(int irq, void *dev_id);

struct ca_uart_port *ca_uart_get_port(unsigned int index)
{
	struct ca_uart_port *pca_port = ca_uart_ports;

	if (index >= UART_NR) {
		/* return 1st element if invalid index */
		index = 0;
	}

	pca_port += index;

	return pca_port;
}

/* uart_ops functions */
static unsigned int ca_uart_tx_empty(struct uart_port *port)
{
	/* Return 0 on FIXO condition, TIOCSER_TEMT otherwise */
	return (readl(port->membase + INFO) & INFO_TX_EMPTY) ? TIOCSER_TEMT : 0;
}

static void ca_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
/*
 * Even if we do not support configuring the modem control lines, this
 * function must be proided to the serial core.
 * port->ops->set_mctrl() be called in uart_configure_port()
 */
}

static unsigned int ca_uart_get_mctrl(struct uart_port *port)
{
	/* Unimplemented signals asserted, per Documentation/serial/driver */
	return TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
}

static void ca_uart_stop_tx(struct uart_port *port)
{
	/* Turn off Tx interrupts. The port lock is held at this point */
	unsigned int reg_v;

	reg_v = readl(port->membase + IE);
	writel(reg_v & ~TX_FIFO_EMPTYE, port->membase + IE);
}

static inline void ca_transmit_buffer(struct uart_port *port)
{
	struct circ_buf *xmit = &port->state->xmit;

	if (uart_circ_empty(xmit) || uart_tx_stopped(port)) {
		ca_uart_stop_tx(port);
		return;
	}

	while (!(readl(port->membase + INFO) & INFO_TX_FULL)) {
		/* send xmit->buf[xmit->tail] out the port here */
		writel(xmit->buf[xmit->tail], port->membase + TX_DAT);
		xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);
		port->icount.tx++;
		if (uart_circ_empty(xmit))
			break;
	}

	if (uart_circ_empty(xmit))
		ca_uart_stop_tx(port);
}

static void ca_uart_start_tx(struct uart_port *port)
{
	/* Turn on Tx interrupts. The port lock is held at this point */
	unsigned int reg_v;

	reg_v = readl(port->membase + IE);
	writel((reg_v | TX_FIFO_EMPTYE), port->membase + IE);

	reg_v = readl(port->membase + CFG);
	writel(reg_v | CFG_TX_EN, port->membase + CFG);

	if (readl(port->membase + INFO) & INFO_TX_EMPTY)
		ca_transmit_buffer(port);
}

static void ca_uart_stop_rx(struct uart_port *port)
{
	/* Turn off Rx interrupts. The port lock is held at this point */
	unsigned int reg_v;

	reg_v = readl(port->membase + IE);
	writel(reg_v & ~RX_FIFO_NONEMPTYE, port->membase + IE);
}

static void ca_uart_enable_ms(struct uart_port *port)
{
	/* Nope, you really can't hope to attach a modem to this */
}

static int ca_uart_startup(struct uart_port *port)
{
	unsigned int reg_v;
	int retval;
	unsigned long flags;

	/* Disable interrupt */
	writel(0, port->membase + IE);

	retval = request_irq(port->irq, ca_uart_interrupt, 0,
			     "ca_uart", port);
	if (retval)
		return retval;

	spin_lock_irqsave(&port->lock, flags);

	reg_v = readl(port->membase + CFG);
	reg_v |= (CFG_UART_EN | CFG_TX_EN | CFG_RX_EN | 0x3 /* 8-bits data */);
	writel(reg_v, port->membase + CFG);
	reg_v = readl(port->membase + IE);
	writel(reg_v | RX_FIFO_NONEMPTYE | TX_FIFO_EMPTYE, port->membase + IE);

	spin_unlock_irqrestore(&port->lock, flags);
	return 0;
}

static void ca_uart_shutdown(struct uart_port *port)
{
	ca_uart_stop_tx(port);
	ca_uart_stop_rx(port);
	free_irq(port->irq, port);
}

static void ca_uart_set_termios(struct uart_port *port,
				struct ktermios *termios,
				const struct ktermios *old)
{
	unsigned long flags;
	int baud;
	unsigned int reg_v, sample_freq = 0;

	baud = uart_get_baud_rate(port, termios, old, 0, 230400);
	reg_v = readl(port->membase + CFG);
	/* mask off the baud settings */
	reg_v &= 0xff;
	reg_v |= (port->uartclk / baud) << CFG_BAUD_SART_SHIFT;

	/* Sampling rate should be half of baud count */
	sample_freq = (reg_v >> CFG_BAUD_SART_SHIFT) / 2;

	/* See include/uapi/asm-generic/termbits.h for CSIZE definition */
	/* mask off the data width */
	reg_v &= 0xfffffffc;
	switch (termios->c_cflag & CSIZE) {
	case CS5:
		reg_v |= 0x0;
		break;
	case CS6:
		reg_v |= 0x1;
		break;
	case CS7:
		reg_v |= 0x2;
		break;
	case CS8:
	default:
		reg_v |= 0x3;
		break;
	}

	/* mask off Stop bits */
	reg_v &= ~(CFG_STOP_2BIT);
	if (termios->c_cflag & CSTOPB)
		reg_v |= CFG_STOP_2BIT;

	/* Parity */
	reg_v &= ~(CFG_PARITY_EN);
	reg_v |= CFG_PARITY_EVEN;
	if (termios->c_cflag & PARENB) {
		reg_v |= CFG_PARITY_EN;
		if (termios->c_cflag & PARODD)
			reg_v &= ~(CFG_PARITY_EVEN);
	}

	spin_lock_irqsave(&port->lock, flags);
	writel(reg_v, port->membase + CFG);
	writel(sample_freq, port->membase + RX_SAMPLE);
	spin_unlock_irqrestore(&port->lock, flags);
}

static const char *ca_uart_type(struct uart_port *port)
{
	if (port->type != PORT_CORTINA_ACCESS)
		return NULL;

	return container_of(port, struct ca_uart_port, uart)->name;
}

static void ca_uart_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_CORTINA_ACCESS;
}

static int ca_uart_verify_port(struct uart_port *port,
			       struct serial_struct *ser)
{
	if (ser->type != PORT_UNKNOWN && ser->type != PORT_CORTINA_ACCESS)
		return -EINVAL;
	return 0;
}

static void ca_access_power(struct uart_port *port, unsigned int state,
			    unsigned int oldstate)
{
	unsigned int reg_v;

	/* Read Config register */
	reg_v = readl(port->membase + CFG);
	switch (state) {
	case UART_PM_STATE_ON:
		reg_v |= CFG_UART_EN;
		break;
	case UART_PM_STATE_OFF:
		reg_v &= ~CFG_UART_EN;
		break;
	default:
		pr_err("cortina-access serial: Unknown PM state %d\n", state);
	}
	writel(reg_v, port->membase + CFG);
}

#ifdef CONFIG_CONSOLE_POLL
static int ca_poll_get_char(struct uart_port *port)
{
	unsigned int rx;

	if (readl(port->membase + INFO) & INFO_RX_EMPTY)
		return NO_POLL_CHAR;

	rx = readl(port->membase + RX_DAT);

	return rx;
}

static void ca_poll_put_char(struct uart_port *port, unsigned char c)
{
	unsigned long time_out;

	time_out = jiffies + usecs_to_jiffies(TX_TIMEOUT);

	while (time_before(jiffies, time_out) &&
	       (readl(port->membase + INFO) & INFO_TX_FULL))
		cpu_relax();

	/* Give up if FIFO stuck! */
	if ((readl(port->membase + INFO) & INFO_TX_FULL))
		return;

	writel(c, port->membase + TX_DAT);
}

#endif

static const struct uart_ops ca_uart_ops = {
	.tx_empty = ca_uart_tx_empty,
	.set_mctrl = ca_uart_set_mctrl,
	.get_mctrl = ca_uart_get_mctrl,
	.stop_tx = ca_uart_stop_tx,
	.start_tx = ca_uart_start_tx,
	.stop_rx = ca_uart_stop_rx,
	.enable_ms = ca_uart_enable_ms,
	.startup = ca_uart_startup,
	.shutdown = ca_uart_shutdown,
	.set_termios = ca_uart_set_termios,
	.type = ca_uart_type,
	.config_port = ca_uart_config_port,
	.verify_port = ca_uart_verify_port,
	.pm = ca_access_power,
#ifdef CONFIG_CONSOLE_POLL
	.poll_get_char = ca_poll_get_char,
	.poll_put_char = ca_poll_put_char,
#endif
};

static inline void ca_uart_interrupt_rx_chars(struct uart_port *port,
					      unsigned long status)
{
	struct tty_port *ttyport = &port->state->port;
	unsigned int ch;
	unsigned int rx, flg;
	struct ca_uart_port *pca_port;

	rx = readl(port->membase + INFO);
	if (INFO_RX_EMPTY & rx)
		return;

	if (status & RX_FIFO_OVERRUNE)
		port->icount.overrun++;

	pca_port = ca_uart_get_port(port->line);

	/* Read the character while FIFO is not empty */
	do {
		flg = TTY_NORMAL;
		port->icount.rx++;
		ch = readl(port->membase + RX_DAT);
		if (status & RX_PARITY_ERRE) {
			port->icount.parity++;
			flg = TTY_PARITY;
		}

		if (pca_port->has_bi) {
			/* If BI supported ? */
			if (status & RX_BREAK) {
				port->icount.brk++;
				if (uart_handle_break(port))
					goto ignore;
			}
		} else {
			/* Treat stop err as BI */
			if (status & RX_STOP_ERRE) {
				port->icount.brk++;
				if (uart_handle_break(port))
					goto ignore;
			}
		}
		if (!(ch & 0x100)) /* RX char is not valid */
			goto ignore;

		if (uart_handle_sysrq_char(port, (unsigned char)ch))
			goto ignore;

		tty_insert_flip_char(ttyport, ch, flg);
 ignore:
		rx = readl(port->membase + INFO);
	} while (!(INFO_RX_EMPTY & rx));

	spin_unlock(&port->lock);
	tty_flip_buffer_push(ttyport);
	spin_lock(&port->lock);
}

static inline void ca_uart_interrupt_tx_chars(struct uart_port *port)
{
	struct circ_buf *xmit = &port->state->xmit;

	/* Process out of band chars */
	if (port->x_char) {
		/* Send next char */
		writel(port->x_char, port->membase + TX_DAT);
		goto done;
	}

	/* Nothing to do ? */
	if (uart_circ_empty(xmit) || uart_tx_stopped(port)) {
		ca_uart_stop_tx(port);
		goto done;
	}

	ca_transmit_buffer(port);

	/* Wake up */
	if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)
		uart_write_wakeup(port);

	/* Maybe we're done after all */
	if (uart_circ_empty(xmit))
		ca_uart_stop_tx(port);

 done:
	return;
}

irqreturn_t ca_uart_interrupt(int irq, void *dev_id)
{
	struct uart_port *port = (struct uart_port *)dev_id;
	unsigned int irq_status;

	spin_lock(&port->lock);

	/* Clear interrupt! */
	irq_status = readl(port->membase + INT);
	writel(irq_status, port->membase + INT);

	/* Process any Rx chars first */
	ca_uart_interrupt_rx_chars(port, irq_status);
	/* Then use any Tx space */
	ca_uart_interrupt_tx_chars(port);

	spin_unlock(&port->lock);

	return IRQ_HANDLED;
}

#ifdef CONFIG_SERIAL_CORTINA_ACCESS_CONSOLE
void ca_console_write(struct console *co, const char *s, unsigned int count)
{
	struct uart_port *port;
	struct ca_uart_port *pca_port;
	unsigned int i, previous;
	unsigned long flags;
	int locked;

	pca_port = ca_uart_get_port(co->index);
	port = &pca_port->uart;

	local_irq_save(flags);
	if (port->sysrq) {
		locked = 0;
	} else if (oops_in_progress) {
		locked = spin_trylock(&port->lock);
	} else {
		spin_lock(&port->lock);
		locked = 1;
	}

	/* Save current state */
	previous = readl(port->membase + IE);
	/* Disable Tx interrupts so this all goes out in one go */
	ca_uart_stop_tx(port);

	/* Write all the chars */
	for (i = 0; i < count; i++) {
		/* Wait the TX buffer to be empty, which can't take forever */
		while (!(readl(port->membase + INFO) & INFO_TX_EMPTY))
			cpu_relax();

		/* Send the char */
		writel(*s, port->membase + TX_DAT);

		/* CR/LF stuff */
		if (*s++ == '\n') {
			/* Wait the TX buffer to be empty */
			while (!(readl(port->membase + INFO) & INFO_TX_EMPTY))
				cpu_relax();
			writel('\r', port->membase + TX_DAT);
		}
	}

	writel(previous, port->membase + IE);	/* Put it all back */

	if (locked)
		spin_unlock(&port->lock);
	local_irq_restore(flags);
}

static int __init ca_console_setup(struct console *co, char *options)
{
	struct uart_port *port;
	struct ca_uart_port *pca_port;
	int baud = 115200;
	int bits = 8;
	int parity = 'n';
	int flow = 'n';

	if (co->index < 0 || co->index >= UART_NR)
		return -ENODEV;

	pca_port = ca_uart_get_port(co->index);

	port = &pca_port->uart;

	/* This isn't going to do much, but it might change the baud rate. */
	if (options)
		uart_parse_options(options, &baud, &parity, &bits, &flow);

	return uart_set_options(port, co, baud, parity, bits, flow);
}

static struct uart_driver ca_uart_driver;

static struct console ca_console = {
	.name = "ttyS",
	.write = ca_console_write,
	.device = uart_console_device,
	.setup = ca_console_setup,
	.flags = CON_PRINTBUFFER,
	.index = -1,		/* Only possible option. */
	.data = &ca_uart_driver,
};

#define CA_CONSOLE (&ca_console)

/* Support EARLYCON */
static void ca_putc(struct uart_port *port, unsigned char c)
{
	unsigned int tmout;

	/* No jiffie at early boot stage!
	 * Wait up to 5ms for the character to be sent.
	 */
	tmout = TX_TIMEOUT;
	while (--tmout) {
		if (!(readl(port->membase + INFO) & INFO_TX_FULL))
			break;
		udelay(1);
	}

	/* Give up if FIFO stuck! */
	while ((readl(port->membase + INFO) & INFO_TX_FULL))
		return;

	/* Send the char */
	writel(c, port->membase + TX_DAT);
}

static void ca_early_write(struct console *con, const char *s, unsigned int n)
{
	struct earlycon_device *dev = con->data;

	uart_console_write(&dev->port, s, n, ca_putc);
}

static int __init ca_early_console_setup(struct earlycon_device *device,
					 const char *opt)
{
	if (!device->port.membase)
		return -ENODEV;

	device->con->write = ca_early_write;
	return 0;
}

EARLYCON_DECLARE(serial, ca_early_console_setup);
OF_EARLYCON_DECLARE(serial, "cortina-access,serial", ca_early_console_setup);
#else
#define CA_CONSOLE	NULL
#endif

static struct uart_driver ca_uart_driver = {
	.owner = THIS_MODULE,
	.driver_name = "cortina-access_uart",
	.dev_name = "ttyS",
	.major = TTY_MAJOR,
	.minor = 64,
	.nr = UART_NR,
	.cons = CA_CONSOLE,
};

/* Match table for of_platform binding */
static const struct of_device_id ca_uart_of_match[] = {
	{.compatible = "cortina-access,serial",},
	{}
};
MODULE_DEVICE_TABLE(of, ca_uart_of_match);

static int serial_ca_probe(struct platform_device *pdev)
{
	int ret;
	void __iomem *base;
	struct ca_uart_port *port;
	const struct of_device_id *match;

	/* assign DT node pointer */
	struct device_node *np = pdev->dev.of_node;
	struct resource mem_resource;
	u32 of_clock_frequency;
	struct clk *pclk_info;
	int uart_idx;

	/* search DT for a match */
	match = of_match_device(ca_uart_of_match, &pdev->dev);
	if (!match)
		return -EINVAL;

	if (!ca_uart_ports)
		ca_uart_ports = devm_kzalloc(&pdev->dev, UART_NR *
				     sizeof(struct ca_uart_port),
				     GFP_KERNEL);

	port = (struct ca_uart_port *)ca_uart_ports;
	for (uart_idx = 0; uart_idx < UART_NR; uart_idx++, port++) {
		/* Find empty slot */
		if (strlen(port->name) == 0)
			break;
	}
	dev_dbg(&pdev->dev, "ca_uart_ports@ 0x%p\n", ca_uart_ports);
	dev_dbg(&pdev->dev, "port=%p\n", port);

	if (uart_idx >= UART_NR)
		return -EINVAL;

	snprintf(port->name, sizeof(port->name), "Cortina-Access UART%d", uart_idx);

	/* Retrieve HW base address */
	ret = of_address_to_resource(np, 0, &mem_resource);
	if (ret) {
		dev_warn(&pdev->dev, "invalid address %d\n", ret);
		return ret;
	}

	base = devm_ioremap(&pdev->dev, mem_resource.start,
			    resource_size(&mem_resource));
	if (!base) {
		devm_kfree(&pdev->dev, port);
		return -ENOMEM;
	}

	/* assign reg base and irq from DT */
	port->uart.irq = irq_of_parse_and_map(np, 0);
	port->uart.membase = base;
	port->uart.mapbase = mem_resource.start;
	port->uart.ops = &ca_uart_ops;
	port->uart.dev = &pdev->dev;
	port->uart.line = uart_idx;
	port->uart.has_sysrq = IS_ENABLED(CONFIG_SERIAL_CORTINA_ACCESS_CONSOLE);

	/* get clock-freqency tuple from DT and store value */
	if (of_property_read_u32(np, "clock-frequency", &of_clock_frequency)) {
		/* If we are here, it means DT node did not contain
		 * clock-frequency tuple. Therefore, instead try to get
		 * clk rate through the clk driver that DT has stated
		 * we are consuming.
		 */
		pclk_info = clk_get(&pdev->dev, NULL);
		if (IS_ERR(pclk_info)) {
			dev_warn(&pdev->dev,
				 "clk or clock-frequency not defined\n");
			return PTR_ERR(pclk_info);
		}

		clk_prepare_enable(pclk_info);
		of_clock_frequency = clk_get_rate(pclk_info);
	}
	port->uart.uartclk = of_clock_frequency;

	if (of_property_read_bool(np, "wakeup-source"))
		port->may_wakeup = true;
	if (of_property_read_bool(np, "break-indicator"))
		port->has_bi = true;

	port->uart.type = PORT_CORTINA_ACCESS;

	if (port->may_wakeup)
		device_init_wakeup(&pdev->dev, true);

	ret = uart_add_one_port(&ca_uart_driver, &port->uart);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, port);

	return 0;
}

static int serial_ca_remove(struct platform_device *pdev)
{
	struct uart_port *port = platform_get_drvdata(pdev);

	if (port)
		uart_remove_one_port(&ca_uart_driver, port);

	devm_kfree(&pdev->dev, ca_uart_ports);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

#ifdef CONFIG_PM
static int serial_ca_suspend(struct platform_device *pdev,
			     pm_message_t state)
{
	struct ca_uart_port *p = (struct ca_uart_port *)pdev->dev.driver_data;

	uart_suspend_port(&ca_uart_driver, &p->uart);

	return 0;
}

static int serial_ca_resume(struct platform_device *pdev)
{
	struct ca_uart_port *p = (struct ca_uart_port *)pdev->dev.driver_data;

	uart_resume_port(&ca_uart_driver, &p->uart);

	return 0;
}
#else
  #define serial_ca_suspend NULL
  #define serial_ca_resume NULL
#endif

static struct platform_driver serial_ca_driver = {
	.probe = serial_ca_probe,
	.remove = serial_ca_remove,
#ifdef CONFIG_PM
	.suspend = serial_ca_suspend,
	.resume = serial_ca_resume,
#endif
	.driver = {
		   .name = "cortina-access_serial",
		   .owner = THIS_MODULE,
		   .of_match_table = ca_uart_of_match,
	},
};

static int __init ca_uart_init(void)
{
	int ret;

	ret = uart_register_driver(&ca_uart_driver);
	if (ret)
		return ret;

	ret = platform_driver_register(&serial_ca_driver);
	if (ret)
		uart_unregister_driver(&ca_uart_driver);

	return ret;
}

static void __exit ca_uart_exit(void)
{
	platform_driver_unregister(&serial_ca_driver);
	uart_unregister_driver(&ca_uart_driver);

	kfree(ca_uart_ports);
	ca_uart_ports = NULL;
}

module_init(ca_uart_init);
module_exit(ca_uart_exit);

MODULE_AUTHOR("Cortina-Access Inc.");
MODULE_DESCRIPTION(" Cortina-Access UART driver");
MODULE_LICENSE("GPL");
