/*
 * known_map.c -- a stand-in for a vendor driver, whose register map the test knows exactly.
 *
 * This exists so re_triage.py can be checked against an answer rather than admired. The offsets are
 * not invented: they are the real layout of MediaTek's 8250-compatible UART (the "mt6577-uart" the
 * boot shim already looks for in the device tree), which every MT67xx uses. A driver that talks to
 * it looks like the functions below, so if the tool cannot recover this map it will not recover a
 * real one either.
 *
 * The absolute bases are written as constants on purpose -- they compile to MOVZ/MOVK, which the
 * scanner tracks, and that is exactly how an MMIO base reaches a driver that does not use a DTB.
 */
typedef unsigned int u32;
typedef unsigned char u8;

/* --- serial: base 0x11002000, the MT6769 UART ------------------------------------------------ */
#define UART 0x11002000u
#define UART_THR  0x00u   /* transmit holding register (write) */
#define UART_RBR  0x00u   /* receive buffer (read)            */
#define UART_DLL  0x00u   /* divisor latch low, when DLAB=1   */
#define UART_IER  0x04u   /* interrupt enable                 */
#define UART_DLM  0x04u   /* divisor latch high, when DLAB=1  */
#define UART_FCR  0x08u   /* FIFO control (write)             */
#define UART_LCR  0x0Cu   /* line control                     */
#define UART_MCR  0x10u   /* modem control                    */
#define UART_LSR  0x14u   /* line status (read)               */
#define UART_SCR  0x1Cu   /* scratch                          */

static volatile u32 *const uart = (volatile u32 *)UART;

void uart_init(void)
{
	uart[UART_LCR / 4] = 0x83u;      /* 8N1, DLAB=1 */
	uart[UART_DLL / 4] = 13u;        /* divisor low  */
	uart[UART_DLM / 4] = 0u;         /* divisor high */
	uart[UART_LCR / 4] = 0x03u;      /* 8N1, DLAB=0 */
	uart[UART_FCR / 4] = 0xC7u;      /* enable and clear both FIFOs */
	uart[UART_MCR / 4] = 0x03u;      /* DTR | RTS */
	uart[UART_IER / 4] = 0x01u;      /* receive-data-available */
	uart[UART_SCR / 4] = 0x5Au;      /* scratch: proves the block round-trips */
}

void uart_putc(u8 c)
{
	/* wait for transmit-holding-empty, then write. LSR bit 5. */
	while (!(uart[UART_LSR / 4] & 0x20u))
		;
	uart[UART_THR / 4] = c;
}

u8 uart_getc(void)
{
	/* wait for data-ready, then read. LSR bit 0. */
	while (!(uart[UART_LSR / 4] & 0x01u))
		;
	return (u8)uart[UART_RBR / 4];
}

/* --- display: base 0x14000000, a second block the tool must keep separate --------------------- */
#define DISP 0x14000000u
#define DISP_CFG      0x000u
#define DISP_SIZE     0x004u
#define DISP_STRIDE   0x008u
#define DISP_FMT      0x00Cu
#define DISP_STATUS   0x010u
#define DISP_IRQ_EN   0x01Cu

static volatile u32 *const disp = (volatile u32 *)DISP;

void disp_setup(u32 w, u32 h)
{
	disp[DISP_CFG / 4] = 0x1u;
	disp[DISP_SIZE / 4] = (h << 16) | w;
	disp[DISP_STRIDE / 4] = w * 4u;
	disp[DISP_FMT / 4] = 0x2u;
	disp[DISP_IRQ_EN / 4] = 0x3u;
	disp[DISP_CFG / 4] = 0x3u;
}

u32 disp_busy(void)
{
	return disp[DISP_STATUS / 4] & 0x1u;
}

/* --- a block reached through a movz/movk + add chain, to exercise offset folding -------------- */
void pmic_write(u32 off, u32 val)
{
	volatile u32 *pmic = (volatile u32 *)0x1000D000u;
	pmic[off / 4] = val;
}
