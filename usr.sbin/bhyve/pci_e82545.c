/*
 * Copyright (c) 2015 Nahanni Systems Inc.
 * Copyright (c) 2013 Jeremiah Lott
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/types.h>
#include <sys/uio.h>
#include <net/ethernet.h>
#include <netinet/in.h>

#include <fcntl.h>
#include <md5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "e1000_regs.h"
#include "e1000_defines.h"
#include "mii.h"

#include "bhyverun.h"
#include "pci_emul.h"
#include "mevent.h"

/* Hardware/register definitions XXX: move some to common code. */
#define E82545_VENDOR_ID_INTEL			0x8086
#define E82545_DEV_ID_82545EM_COPPER		0x100F
#define E82545_SUBDEV_ID			0x1008

#define E82545_REVISION_4			4

#define E82545_MDIC_DATA_MASK			0x0000FFFF
#define E82545_MDIC_OP_MASK			0x0c000000
#define E82545_MDIC_IE				0x20000000

#define E82545_EECD_FWE_DIS	0x00000010 /* Flash writes disabled */
#define E82545_EECD_FWE_EN	0x00000020 /* Flash writes enabled */
#define E82545_EECD_FWE_MASK	0x00000030 /* Flash writes mask */

#define E82545_BAR_REGISTER			0
#define E82545_BAR_REGISTER_LEN			(128*1024)
#define E82545_BAR_FLASH			1
#define E82545_BAR_FLASH_LEN			(64*1024)
#define E82545_BAR_IO				2
#define E82545_BAR_IO_LEN			8

#define E82545_IOADDR				0x00000000
#define E82545_IODATA				0x00000004
#define E82545_IO_REGISTER_MAX			0x0001FFFF
#define E82545_IO_FLASH_BASE			0x00080000
#define E82545_IO_FLASH_MAX			0x000FFFFF

#define E82545_ARRAY_ENTRY(reg, offset)		(reg + (offset<<2))
#define E82545_RAR_MAX				15
#define E82545_MTA_MAX				127
#define E82545_VFTA_MAX				127

/* Slightly modified from the driver versions, hardcoded for 3 opcode bits,
 * followed by 6 address bits.
 * TODO: make opcode bits and addr bits configurable?
 * NVM Commands - Microwire */
#define E82545_NVM_OPCODE_BITS	3
#define E82545_NVM_ADDR_BITS	6
#define E82545_NVM_DATA_BITS	16
#define E82545_NVM_OPADDR_BITS	(E82545_NVM_OPCODE_BITS + E82545_NVM_ADDR_BITS)
#define E82545_NVM_ADDR_MASK	((1 << E82545_NVM_ADDR_BITS)-1)
#define E82545_NVM_OPCODE_MASK	\
    (((1 << E82545_NVM_OPCODE_BITS) - 1) << E82545_NVM_ADDR_BITS)
#define E82545_NVM_OPCODE_READ	(0x6 << E82545_NVM_ADDR_BITS)	/* read */
#define E82545_NVM_OPCODE_WRITE	(0x5 << E82545_NVM_ADDR_BITS)	/* write */
#define E82545_NVM_OPCODE_ERASE	(0x7 << E82545_NVM_ADDR_BITS)	/* erase */
#define	E82545_NVM_OPCODE_EWEN	(0x4 << E82545_NVM_ADDR_BITS)	/* wr-enable */

#define	E82545_NVM_EEPROM_SIZE	64 /* 64 * 16-bit values == 128K */

#define E1000_ICR_SRPD		0x00010000

/*
 * XXX does this actually have a limit on the 82545 ?
 * There is a limit on the max number of bytes, but perhaps not
 * on descriptors ??
 */
#define I82545_MAX_TXSEGS	20

/* Legacy receive descriptor */
struct e1000_rx_desc {
	uint64_t buffer_addr;	/* Address of the descriptor's data buffer */
	uint16_t length;	/* Length of data DMAed into data buffer */
	uint16_t csum;		/* Packet checksum */
	uint8_t	 status;       	/* Descriptor status */
	uint8_t  errors;	/* Descriptor Errors */
	uint16_t special;
};

/* Transmit descriptor types */
#define	E1000_TXD_MASK		(E1000_TXD_CMD_DEXT | 0x00F00000)
#define E1000_TXD_TYP_L		(0)
#define E1000_TXD_TYP_C		(E1000_TXD_CMD_DEXT | E1000_TXD_DTYP_C)
#define E1000_TXD_TYP_D		(E1000_TXD_CMD_DEXT | E1000_TXD_DTYP_D)

/* Legacy transmit descriptor */
struct e1000_tx_desc {
	uint64_t buffer_addr;   /* Address of the descriptor's data buffer */
	union {
		uint32_t data;
		struct {
			uint16_t length;  /* Data buffer length */
			uint8_t  cso;  /* Checksum offset */
			uint8_t  cmd;  /* Descriptor control */
		} flags;
	} lower;
	union {
		uint32_t data;
		struct {
			uint8_t status; /* Descriptor status */
			uint8_t css;  /* Checksum start */
			uint16_t special;
		} fields;
	} upper;
};

/* Context descriptor */
struct e1000_context_desc {
	union {
		uint32_t ip_config;
		struct {
			uint8_t ipcss;  /* IP checksum start */
			uint8_t ipcso;  /* IP checksum offset */
			uint16_t ipcse;  /* IP checksum end */
		} ip_fields;
	} lower_setup;
	union {
		uint32_t tcp_config;
		struct {
			uint8_t tucss;  /* TCP checksum start */
			uint8_t tucso;  /* TCP checksum offset */
			uint16_t tucse;  /* TCP checksum end */
		} tcp_fields;
	} upper_setup;
	uint32_t cmd_and_length;
	union {
		uint32_t data;
		struct {
			uint8_t status;  /* Descriptor status */
			uint8_t hdr_len;  /* Header length */
			uint16_t mss;  /* Maximum segment size */
		} fields;
	} tcp_seg_setup;
};

/* Data descriptor */
struct e1000_data_desc {
	uint64_t buffer_addr;  /* Address of the descriptor's buffer address */
	union {
		uint32_t data;
		struct {
			uint16_t length;  /* Data buffer length */
			uint8_t typ_len_ext;
			uint8_t cmd;
		} flags;
	} lower;
	union {
		uint32_t data;
		struct {
			uint8_t status;  /* Descriptor status */
			uint8_t popts;  /* Packet Options */
			uint16_t special;
		} fields;
	} upper;
};

union e1000_tx_udesc {
	struct e1000_tx_desc td;
	struct e1000_context_desc cd;
	struct e1000_data_desc dd;
};

/* Tx checksum info for a packet. */
struct ck_info {
	int	ck_valid;	/* ck_info is valid */
	uint8_t	ck_start;	/* start byte of cksum calcuation */
	uint8_t	ck_off;		/* offset of cksum insertion */
	uint16_t ck_len;	/* length of cksum calc: 0 is to packet-end */
};

/*
 * Debug printf
 */
static int e82545_debug = 1;
#define DPRINTF(msg,params...) if (e82545_debug) fprintf(stderr, "e82545: " msg, params)
#define WPRINTF(msg,params...) fprintf(stderr, "e82545: " msg, params)

/* s/w representation of the RAL/RAH regs */
struct  eth_uni {
	int		eu_valid;
	int		eu_addrsel;
	struct ether_addr eu_eth;
};


struct e82545_softc {
	struct pci_devinst *esc_pi;
	struct vmctx	*esc_ctx;
	struct mevent   *esc_mevp;
	struct mevent   *esc_mevpitr;
	pthread_mutex_t	esc_mtx;
	struct ether_addr esc_mac;
	int		esc_tapfd;

	/* General */
	uint32_t	esc_CTRL;	/* x0000 device ctl */
	uint32_t	esc_FCAL;	/* x0028 flow ctl addr lo */
	uint32_t	esc_FCAH;	/* x002C flow ctl addr hi */
	uint32_t	esc_FCT;	/* x0030 flow ctl type */
	uint32_t	esc_VET;	/* x0038 VLAN eth type */
	uint32_t	esc_FCTTV;	/* x0170 flow ctl tx timer */
	uint32_t	esc_LEDCTL;	/* x0E00 LED control */
	uint32_t	esc_PBA;	/* x1000 pkt buffer allocation */
	
	/* Interrupt control */
	int		esc_irq_asserted;
	uint32_t	esc_ICR;	/* x00C0 cause read/clear */
	uint32_t	esc_ITR;	/* x00C4 intr throttling */
	uint32_t	esc_ICS;	/* x00C8 cause set */
	uint32_t	esc_IMS;	/* x00D0 mask set/read */
	uint32_t	esc_IMC;	/* x00D8 mask clear */

	/* Transmit */
	union e1000_tx_udesc *esc_txdesc;
	struct e1000_context_desc esc_txctx;
	int		esc_tx_enabled;
	uint32_t	esc_TXCW;	/* x0178 transmit config */
	uint32_t	esc_TCTL;	/* x0400 transmit ctl */
	uint32_t	esc_TIPG;	/* x0410 inter-packet gap */
	uint16_t	esc_AIT;	/* x0458 Adaptive Interframe Throttle */
	uint64_t	esc_tdba;      	/* verified 64-bit desc table addr */
	uint32_t	esc_TDBAL;	/* x3800 desc table addr, low bits */
	uint32_t	esc_TDBAH;	/* x3804 desc table addr, hi 32-bits */
	uint32_t	esc_TDLEN;	/* x3808 # descriptors in bytes */
	uint16_t	esc_TDH;	/* x3810 desc table head idx */
	uint16_t	esc_TDT;	/* x3818 desc table tail idx */
	uint32_t	esc_TIDV;	/* x3820 intr delay */
	uint32_t	esc_TXDCTL;	/* x3828 desc control */
	uint32_t	esc_TADV;	/* x382C intr absolute delay */
	
	/* L2 frame acceptance */
	struct eth_uni	esc_uni[16];	/* 16 x unicast MAC addresses */
	uint32_t	esc_fmcast[128]; /* Multicast filter bit-match */
	uint32_t	esc_fvlan[128]; /* VLAN 4096-bit filter */
	
	/* Receive */
	struct e1000_rx_desc *esc_rxdesc;
	int		esc_rx_enabled;
	int		esc_rx_loopback;
	uint32_t	esc_RCTL;	/* x0100 receive ctl */
	uint32_t	esc_FCRTL;	/* x2160 flow cntl thresh, low */
	uint32_t	esc_FCRTH;	/* x2168 flow cntl thresh, hi */
	uint64_t	esc_rdba;	/* verified 64-bit desc table addr */
	uint32_t	esc_RDBAL;	/* x2800 desc table addr, low bits */
	uint32_t	esc_RDBAH;	/* x2804 desc table addr, hi 32-bits*/
	uint32_t	esc_RDLEN;	/* x2808 #descriptors */
	uint16_t	esc_RDH;	/* x2810 desc table head idx */
	uint16_t	esc_RDT;	/* x2818 desc table tail idx */
	uint32_t	esc_RDTR;	/* x2820 intr delay */
	uint32_t	esc_RXDCTL;	/* x2828 desc control */
	uint32_t	esc_RADV;	/* x282C intr absolute delay */
	uint32_t	esc_RSRPD;	/* x2C00 recv small packet detect */
	uint32_t	esc_RXCSUM;     /* x5000 receive cksum ctl */
	
	/* IO Port register access */
	uint32_t io_addr;

	/* Shadow copy of MDIC */
	uint32_t mdi_control;
	/* Shadow copy of EECD */
	uint32_t eeprom_control;
	/* Latest NVM in/out */
	uint16_t nvm_data;
	uint16_t nvm_opaddr;
	/* stats */
	uint32_t missed_pkt_count; /* dropped for no room in rx queue */
	uint32_t pkt_rx_by_size[6];
	uint32_t pkt_tx_by_size[6];
	uint32_t good_pkt_rx_count;
	uint32_t bcast_pkt_rx_count;
	uint32_t mcast_pkt_rx_count;
	uint32_t good_pkt_tx_count;
	uint32_t bcast_pkt_tx_count;
	uint32_t mcast_pkt_tx_count;
	uint32_t oversize_rx_count;
	uint32_t tso_tx_count;
	uint64_t good_octets_rx;
	uint64_t good_octets_tx;
	uint64_t missed_octets; /* counts missed and oversized */

	uint8_t nvm_bits:6; /* number of bits remaining in/out */
	uint8_t nvm_mode:2;
#define E82545_NVM_MODE_OPADDR  0x0
#define E82545_NVM_MODE_DATAIN  0x1
#define E82545_NVM_MODE_DATAOUT 0x2
        /* EEPROM data */
        uint16_t eeprom_data[E82545_NVM_EEPROM_SIZE];
};

static void e82545_reset(struct e82545_softc *sc, int dev);
static void e82545_tap_callback(int fd, enum ev_type type, void *param);
	
static inline int
e82545_size_stat_index(uint32_t size)
{
	if (size <= 64) {
		return 0;
	} else if (size >= 1024) {
		return 5;
	} else {
		/* should be 1-4 */
		return (ffs(size) - 6);
	}
}

static void
e82545_init_eeprom(struct e82545_softc *sc)
{
	uint16_t checksum, i;

        /* mac addr */
	sc->eeprom_data[NVM_MAC_ADDR] = ((uint16_t)sc->esc_mac.octet[0]) |
		(((uint16_t)sc->esc_mac.octet[1]) << 8);
	sc->eeprom_data[NVM_MAC_ADDR+1] = ((uint16_t)sc->esc_mac.octet[2]) |
		(((uint16_t)sc->esc_mac.octet[3]) << 8);
	sc->eeprom_data[NVM_MAC_ADDR+2] = ((uint16_t)sc->esc_mac.octet[4]) |
		(((uint16_t)sc->esc_mac.octet[5]) << 8);

	/* pci ids */
	sc->eeprom_data[NVM_SUB_DEV_ID] = E82545_SUBDEV_ID;
	sc->eeprom_data[NVM_SUB_VEN_ID] = E82545_VENDOR_ID_INTEL;
	sc->eeprom_data[NVM_DEV_ID] = E82545_DEV_ID_82545EM_COPPER;
	sc->eeprom_data[NVM_VEN_ID] = E82545_VENDOR_ID_INTEL;

	/* fill in the checksum */
        checksum = 0;
	for (i = 0; i < NVM_CHECKSUM_REG; i++) {
		checksum += sc->eeprom_data[i];
	}
	checksum = NVM_SUM - checksum;
	sc->eeprom_data[NVM_CHECKSUM_REG] = checksum;
	DPRINTF("eeprom checksum: 0x%x\r\n", checksum);
}

static void
e82545_write_mdi(struct e82545_softc *sc, uint8_t reg_addr,
			uint8_t phy_addr, uint32_t data)
{
	DPRINTF("Write mdi reg:0x%x phy:0x%x data: 0x%x\r\n", reg_addr, phy_addr, data);
}

static uint32_t
e82545_read_mdi(struct e82545_softc *sc, uint8_t reg_addr,
			uint8_t phy_addr)
{
	//DPRINTF("Read mdi reg:0x%x phy:0x%x\r\n", reg_addr, phy_addr);
	switch (reg_addr) {
	case PHY_STATUS:
		return (MII_SR_LINK_STATUS | MII_SR_AUTONEG_CAPS |
			MII_SR_AUTONEG_COMPLETE);
	case PHY_AUTONEG_ADV:
		return NWAY_AR_SELECTOR_FIELD;
	case PHY_LP_ABILITY:
		return 0;
	case PHY_1000T_STATUS:
		return (SR_1000T_LP_FD_CAPS | SR_1000T_REMOTE_RX_STATUS |
			SR_1000T_LOCAL_RX_STATUS);
	case PHY_ID1:
		return (M88E1011_I_PHY_ID >> 16) & 0xFFFF;
	case PHY_ID2:
		return (M88E1011_I_PHY_ID | E82545_REVISION_4) & 0xFFFF;
	default:
		DPRINTF("Unknown mdi read reg:0x%x phy:0x%x\r\n", reg_addr, phy_addr);
		return 0;
	}
	/* not reached */
}

static void
e82545_eecd_strobe(struct e82545_softc *sc)
{
	/* Microwire state machine */
	/*
	DPRINTF("eeprom state machine srtobe "
		"0x%x 0x%x 0x%x 0x%x\r\n",
		sc->nvm_mode, sc->nvm_bits,
		sc->nvm_opaddr, sc->nvm_data);*/

	if (sc->nvm_bits == 0) {
		DPRINTF("eeprom state machine not expecting data! "
			"0x%x 0x%x 0x%x 0x%x\r\n",
			sc->nvm_mode, sc->nvm_bits,
			sc->nvm_opaddr, sc->nvm_data);
		return;
	}
	sc->nvm_bits--;
	if (sc->nvm_mode == E82545_NVM_MODE_DATAOUT) {
		/* shifting out */
		if (sc->nvm_data & 0x8000) {
			sc->eeprom_control |= E1000_EECD_DO;
		} else {
			sc->eeprom_control &= ~E1000_EECD_DO;
		}
		sc->nvm_data <<= 1;
		if (sc->nvm_bits == 0) {
			/* read done, back to opcode mode. */
			sc->nvm_opaddr = 0;
			sc->nvm_mode = E82545_NVM_MODE_OPADDR;
			sc->nvm_bits = E82545_NVM_OPADDR_BITS;
		}
	} else if (sc->nvm_mode == E82545_NVM_MODE_DATAIN) {
		/* shifting in */
		sc->nvm_data <<= 1;
		if (sc->eeprom_control & E1000_EECD_DI) {
			sc->nvm_data |= 1;
		}
		if (sc->nvm_bits == 0) {
			/* eeprom write */
			uint16_t op = sc->nvm_opaddr & E82545_NVM_OPCODE_MASK;
			uint16_t addr = sc->nvm_opaddr & E82545_NVM_ADDR_MASK;
			if (op != E82545_NVM_OPCODE_WRITE) {
				DPRINTF("Illegal eeprom write op 0x%x\r\n",
					sc->nvm_opaddr);
			} else if (addr >= E82545_NVM_EEPROM_SIZE) {
				DPRINTF("Illegal eeprom write addr 0x%x\r\n",
					sc->nvm_opaddr);
			} else {
				DPRINTF("eeprom write eeprom[0x%x] = 0x%x\r\n",
				addr, sc->nvm_data);
				sc->eeprom_data[addr] = sc->nvm_data;
			}
			/* back to opcode mode */
			sc->nvm_opaddr = 0;
			sc->nvm_mode = E82545_NVM_MODE_OPADDR;
			sc->nvm_bits = E82545_NVM_OPADDR_BITS;
		}
	} else if (sc->nvm_mode == E82545_NVM_MODE_OPADDR) {
		sc->nvm_opaddr <<= 1;
		if (sc->eeprom_control & E1000_EECD_DI) {
			sc->nvm_opaddr |= 1;
		}
		if (sc->nvm_bits == 0) {
			uint16_t op = sc->nvm_opaddr & E82545_NVM_OPCODE_MASK;
			switch (op) {
			case E82545_NVM_OPCODE_EWEN:
				DPRINTF("eeprom write enable: 0x%x\r\n",
					sc->nvm_opaddr);
				/* back to opcode mode */
				sc->nvm_opaddr = 0;
				sc->nvm_mode = E82545_NVM_MODE_OPADDR;
				sc->nvm_bits = E82545_NVM_OPADDR_BITS;
				break;
			case E82545_NVM_OPCODE_READ:
			{
				uint16_t addr = sc->nvm_opaddr &
					E82545_NVM_ADDR_MASK;
				sc->nvm_mode = E82545_NVM_MODE_DATAOUT;
				sc->nvm_bits = E82545_NVM_DATA_BITS;
				if (addr < E82545_NVM_EEPROM_SIZE) {
					sc->nvm_data = sc->eeprom_data[addr];
					DPRINTF("eeprom read: eeprom[0x%x] = 0x%x\r\n",
						addr, sc->nvm_data);
				} else {
					DPRINTF("eeprom illegal read: 0x%x\r\n",
						sc->nvm_opaddr);
					sc->nvm_data = 0;
				}
				break;
			}
			case E82545_NVM_OPCODE_WRITE:
				sc->nvm_mode = E82545_NVM_MODE_DATAIN;
				sc->nvm_bits = E82545_NVM_DATA_BITS;
				sc->nvm_data = 0;
				break;
			default:
				DPRINTF("eeprom unknown op: 0x%x\r\r",
					sc->nvm_opaddr);
				/* back to opcode mode */
				sc->nvm_opaddr = 0;
				sc->nvm_mode = E82545_NVM_MODE_OPADDR;
				sc->nvm_bits = E82545_NVM_OPADDR_BITS;
			}
		}
	} else {
		DPRINTF("eeprom state machine wrong state! "
			"0x%x 0x%x 0x%x 0x%x\r\n",
			sc->nvm_mode, sc->nvm_bits,
			sc->nvm_opaddr, sc->nvm_data);
	}
}

static void
e82545_itr_callback(int fd, enum ev_type type, void *param)
{
	uint32_t new;
	struct e82545_softc *sc = param;

	pthread_mutex_lock(&sc->esc_mtx);
	new = sc->esc_ICR & sc->esc_IMS;
	if (new && !sc->esc_irq_asserted) {
		DPRINTF("itr callback: lintr assert %x\r\n", new);
		sc->esc_irq_asserted = 1;
		pci_lintr_assert(sc->esc_pi);
	} else {
		mevent_delete(sc->esc_mevpitr);
		sc->esc_mevpitr = NULL;
	}
	pthread_mutex_unlock(&sc->esc_mtx);
}

static void
e82545_icr_assert(struct e82545_softc *sc, uint32_t bits)
{
	uint32_t new;

	DPRINTF("icr assert: 0x%x\r\n", bits);
	
	/*
	 * An interrupt is only generated if bits are set that
	 * aren't already in the ICR, these bits are unmasked,
	 * and there isn't an interrupt already pending.
	 */
	new = bits & ~sc->esc_ICR & sc->esc_IMS;
	sc->esc_ICR |= bits;

	if (new == 0) {
		DPRINTF("icr assert: masked %x, ims %x\r\n", new, sc->esc_IMS);
	} else if (sc->esc_mevpitr != NULL) {
		DPRINTF("icr assert: throttled %x, ims %x\r\n", new, sc->esc_IMS);
	} else if (!sc->esc_irq_asserted) {
		DPRINTF("icr assert: lintr assert %x\r\n", new);
		sc->esc_irq_asserted = 1;
		pci_lintr_assert(sc->esc_pi);
		if (sc->esc_ITR != 0) {
			sc->esc_mevpitr = mevent_add(
			    (sc->esc_ITR + 3905) / 3906,  /* 256ns -> 1ms */
			    EVF_TIMER, e82545_itr_callback, sc);
		}
	}
}

static void
e82545_ims_change(struct e82545_softc *sc, uint32_t bits)
{
	uint32_t new;

	/*
	 * Changing the mask may allow previously asserted
	 * but masked interrupt requests to generate an interrupt.
	 */
	new = bits & sc->esc_ICR & ~sc->esc_IMS;
	sc->esc_IMS |= bits;

	if (new == 0) {
		DPRINTF("ims change: masked %x, ims %x\r\n", new, sc->esc_IMS);
	} else if (sc->esc_mevpitr != NULL) {
		DPRINTF("ims change: throttled %x, ims %x\r\n", new, sc->esc_IMS);
	} else if (!sc->esc_irq_asserted) {
		DPRINTF("ims change: lintr assert %x\n\r", new);
		sc->esc_irq_asserted = 1;
		pci_lintr_assert(sc->esc_pi);
		if (sc->esc_ITR != 0) {
			sc->esc_mevpitr = mevent_add(
			    (sc->esc_ITR + 3905) / 3906,  /* 256ns -> 1ms */
			    EVF_TIMER, e82545_itr_callback, sc);
		}
	}
}

static void
e82545_icr_deassert(struct e82545_softc *sc, uint32_t bits)
{

	DPRINTF("icr deassert: 0x%x\r\n", bits);
	sc->esc_ICR &= ~bits;

	/*
	 * If there are no longer any interrupt sources and there
	 * was an asserted interrupt, clear it
	 */
	if (sc->esc_irq_asserted && !(sc->esc_ICR & sc->esc_IMS)) {
		DPRINTF("icr deassert: lintr deassert %x\r\n", bits);
		pci_lintr_deassert(sc->esc_pi);
		sc->esc_irq_asserted = 0;
	}
}

static void
e82545_intr_write(struct e82545_softc *sc, uint32_t offset, uint32_t value)
{

	DPRINTF("intr_write: off %x, val %x\n\r", offset, value);
	
	switch (offset) {
	case E1000_ICR:
		e82545_icr_deassert(sc, value);
		break;
	case E1000_ITR:
		sc->esc_ITR = value;
		break;
	case E1000_ICS:
		sc->esc_ICS = value;	/* not used: store for debug */
		e82545_icr_assert(sc, value);
		break;
	case E1000_IMS:
		e82545_ims_change(sc, value);
		break;
	case E1000_IMC:
		sc->esc_IMC = value;	/* for debug */
		sc->esc_IMS &= ~value;
		// XXX clear interrupts if all ICR bits now masked
		// and interrupt was pending ?
		break;
	default:
		break;
	}
}

static uint32_t
e82545_intr_read(struct e82545_softc *sc, uint32_t offset)
{
	uint32_t retval;

	retval = 0;

	DPRINTF("intr_read: off %x\n\r", offset);
	
	switch (offset) {
	case E1000_ICR:
		retval = sc->esc_ICR;
		sc->esc_ICR = 0;
		e82545_icr_deassert(sc, ~0);
		break;
	case E1000_ITR:
		retval = sc->esc_ITR;
		break;
	case E1000_ICS:
		/* write-only register */
		break;
	case E1000_IMS:
		retval = sc->esc_IMS;
		break;
	case E1000_IMC:
		/* write-only register */
		break;
	default:
		break;
	}

	return (retval);
}

static void
e82545_devctl(struct e82545_softc *sc, uint32_t val)
{

	sc->esc_CTRL = val & ~E1000_CTRL_RST;

	if (val & E1000_CTRL_RST) {
		DPRINTF("e1k: s/w reset, ctl %x\n", val);
		e82545_reset(sc, 1);
	}
	/* XXX check for phy reset ? */
}

static void
e82545_rx_update_rdba(struct e82545_softc *sc)
{

	/* XXX verify desc base/len within phys mem range */
	sc->esc_rdba = (uint64_t)sc->esc_RDBAH << 32 |
	    sc->esc_RDBAL;
	
	/* Cache host mapping of guest descriptor array */
	sc->esc_rxdesc = paddr_guest2host(sc->esc_ctx,
	    sc->esc_rdba, sc->esc_RDLEN);	
}

static void
e82545_rx_ctl(struct e82545_softc *sc, uint32_t val)
{
	int on;

	on = ((val & E1000_RCTL_EN) == E1000_RCTL_EN);

	/* Save RCTL after stripping reserved bits 31:27,24,21,14,11:10,0 */
	sc->esc_RCTL = val & ~0xF9204c01;

	DPRINTF("rx_ctl - %s RCTL %x, val %x\n",
		on ? "on" : "off", sc->esc_RCTL, val);

	/* state change requested */
	if (on != sc->esc_rx_enabled) {
		if (on) {
			/* Catch disallowed/unimplemented settings */
			//assert(val & E1000_RCTL_SECRC);
			//assert(!(val & E1000_RCTL_LBM_TCVR));

			//xassert(!(val & E1000_RCTL_VFE));  /* XXX no VFE yet */
			if (sc->esc_RCTL & E1000_RCTL_LBM_TCVR) {
				sc->esc_rx_loopback = 1;
			} else {
				sc->esc_rx_loopback = 0;
			}

			e82545_rx_update_rdba(sc);			
			sc->esc_rx_enabled = 1;
		} else {
			sc->esc_rx_enabled = 0;
			sc->esc_rx_loopback = 0;
			sc->esc_rdba = 0;
			sc->esc_rxdesc = NULL;
		}
	}
}

static void
e82545_tx_update_tdba(struct e82545_softc *sc)
{

	/* XXX verify desc base/len within phys mem range */
	sc->esc_tdba = (uint64_t)sc->esc_TDBAH << 32 | sc->esc_TDBAL;

	/* Cache host mapping of guest descriptor array */
	sc->esc_txdesc = paddr_guest2host(sc->esc_ctx, sc->esc_tdba,
            sc->esc_TDLEN);
}

static void
e82545_tx_ctl(struct e82545_softc *sc, uint32_t val)
{
	int on;
	
	on = ((val & E1000_TCTL_EN) == E1000_TCTL_EN);

	/* ignore TCTL_EN settings that don't change state */
	if (on == sc->esc_tx_enabled)
		return;

	if (on) {
		sc->esc_tx_enabled = 1;
		e82545_tx_update_tdba(sc);
	} else {
		sc->esc_tx_enabled = 0;
		sc->esc_tdba = 0;
		sc->esc_txdesc = NULL;
	}

	/* Save TCTL value after stripping reserved bits 31:25,23,2,0 */
	sc->esc_TCTL = val & ~0xFE800005;
}

int
e82545_bufsz(uint32_t rctl)
{

	switch (rctl & (E1000_RCTL_BSEX | E1000_RCTL_SZ_256)) {
	case (E1000_RCTL_SZ_2048): return (2048);
	case (E1000_RCTL_SZ_1024): return (1024);
	case (E1000_RCTL_SZ_512): return (512);
	case (E1000_RCTL_SZ_256): return (256);
	case (E1000_RCTL_BSEX|E1000_RCTL_SZ_16384): return (16384);
	case (E1000_RCTL_BSEX|E1000_RCTL_SZ_8192): return (8192);
	case (E1000_RCTL_BSEX|E1000_RCTL_SZ_4096): return (4096);
	}
	return (256);	/* Forbidden value. */
}

static uint8_t dummybuf[2048];

/* XXX one packet at a time until this is debugged */
static void
e82545_tap_callback(int fd, enum ev_type type, void *param)
{
	struct e82545_softc *sc = param;
	struct e1000_rx_desc *rxd;
	struct iovec vec[64];
	int len, maxpktsz, bufsz, n, i, size;
	uint32_t cause;

	pthread_mutex_lock(&sc->esc_mtx);
	bufsz = e82545_bufsz(sc->esc_RCTL);
	maxpktsz = (sc->esc_RCTL & E1000_RCTL_LPE) ? MAX_JUMBO_FRAME_SIZE :
	    ETHER_MAX_LEN;
	n = (maxpktsz + bufsz - 1) / bufsz;
	size = sc->esc_RDLEN / 16;

	if (!sc->esc_rx_enabled || sc->esc_rx_loopback ||
	    ((size + sc->esc_RDT - sc->esc_RDH) % size < n)) {
		/* Drop the packet */
		DPRINTF("packet dropped, disabled %d\r\n", sc->esc_rx_enabled);
		(void) read(sc->esc_tapfd, dummybuf, sizeof(dummybuf));
	} else {
		/* Grab rx descriptor pointed to by the head pointer */
		for (i = 0; i < n; i++) {
			rxd = &sc->esc_rxdesc[sc->esc_RDH + i % size];
			vec[i].iov_base = paddr_guest2host(sc->esc_ctx,
			    rxd->buffer_addr, bufsz);
			vec[i].iov_len = bufsz;
		}
		len = readv(sc->esc_tapfd, vec, n);
		if (len > 0) {
			/*
			 * Adjust the packet length based on whether the
			 * CRC needs to be stripped or if the packet is
			 * less than the minimum eth packet size.
			 * Note that no CRC bytes are copied out, nor
			 * additional padding bytes for frames smaller
			 * than the minimum size
			 */
			if (len < ETHER_MIN_LEN - ETHER_CRC_LEN)
				len = ETHER_MIN_LEN - ETHER_CRC_LEN;
			if (!(sc->esc_RCTL & E1000_RCTL_SECRC))
				len += ETHER_CRC_LEN;
		}
		n = (len + bufsz - 1) / bufsz;

		DPRINTF("packet read %d bytes, %d segs, head %d\r\n",
		    len, n, sc->esc_RDH);
		if (len > 0) {
			for (i = 0; i < n - 1; i++) {
				rxd = &sc->esc_rxdesc[sc->esc_RDH + i % size];
				rxd->length = bufsz;
				rxd->csum = 0;
				rxd->errors = 0;
				rxd->special = 0;
				rxd->status = E1000_RXD_STAT_DD;
			}

			rxd = &sc->esc_rxdesc[sc->esc_RDH + i % size];
			rxd->length = len % bufsz;
			rxd->csum = 0;
			rxd->errors = 0;
			rxd->special = 0;
			/* XXX signal no checksum for now */
			rxd->status = E1000_RXD_STAT_PIF | E1000_RXD_STAT_IXSM |
			    E1000_RXD_STAT_EOP | E1000_RXD_STAT_DD;

			cause = 0;
			if (len <= sc->esc_RSRPD) {
				cause |= E1000_ICR_SRPD | E1000_ICR_RXT0;
			} else {
				/* XXX: RDRT and RADV timers should be here. */
				cause |= E1000_ICR_RXT0;
			}
			e82545_icr_assert(sc, cause);

			/* Update head pointer */
			sc->esc_RDH = (sc->esc_RDH + n) % size;
		} else {
			DPRINTF("tap: bad len returned %d\n", len);
		}
	}

	pthread_mutex_unlock(&sc->esc_mtx);
}

static uint32_t
e82545_net_checksum(unsigned char *buf, unsigned nbytes)
{
	uint32_t sum;
	int i;

	sum = 0;
	
	/* Checksum all the pairs of bytes first... */
	for (i = 0; i < (nbytes & ~1U); i += 2) {
		sum += (u_int16_t)ntohs(*((u_int16_t *)(buf + i)));
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}

	/*
         * If there's a single byte left over, checksum it, too.
         * Network byte order is big-endian, so the remaining byte is
         * the high byte.
         */
	if (i < nbytes) {
		sum += buf[i] << 8;
		if (sum > 0xFFFF)
			sum -= 0xFFFF;
	}

	sum = ~sum & 0xFFFF;	
	
	return (htons(sum));
}

/*
 * Return the transmit descriptor type.
 */
int
e82545_txdesc_type(uint32_t lower)
{
	int type;

	type = 0;
	
	if (lower & E1000_TXD_CMD_DEXT)
		type = lower & E1000_TXD_MASK;

	return (type);
}

/*
 * Since the checksum has to be written into the packet, copy the
 * packet from guest memory into a buffer and update the checksum
 * in-place.
 *
 * A more optimized version would be able to calculate the checksum
 * from the iovec, and then insert an additional iovec that would
 * contain the 2 bytes of checksum data. This is a lot of work for
 * perhaps little gain so will be deferred.
 */
static int
e82545_transmit_checksum(struct e82545_softc *sc, struct iovec *iov,
    int iovcnt, int len, uint8_t *tbuf, struct ck_info *ck)
{
	uint16_t cksum;
	int i, cklen, tlen;

	/* Copy iovec into linear tbuf */
	for (i = 0, tlen = 0; i < iovcnt; i++) {
		memcpy(&tbuf[tlen], iov[i].iov_base, iov[i].iov_len);
		tlen += iov[i].iov_len;
	}
	assert(tlen == len);

	/* Walk through ck_info array XXX assume [2] */
	for (i = 0; i < 2; i++) {
#if 0
		/* Sanity check cksum offset/location */
		if (cso > (len - 1) || (css > (len - 1)))
			return (0);
#endif
		if (ck[i].ck_valid == 0) {
			DPRINTF("tx cksum: idx %d not valid\n\r", i);
			continue;
		}

		/*
		 * Determine how many bytes need to be checksummed.
		 * Note that ck_len is a 0-based index, so add another 1
		 * to get the number of bytes. For the case where ck_len
		 * is 0, use the existing byte count.
		 */
		DPRINTF("tx cksum: idx %d, s/off/len %d/%d/%d\n\r",
			i, ck[i].ck_start, ck[i].ck_off, ck[i].ck_len);
		cklen = ck[i].ck_len ? ck[i].ck_len - ck[i].ck_start + 1 :
			len - ck[i].ck_start;
		cksum = e82545_net_checksum(&tbuf[ck[i].ck_start], cklen);
		
		/* Insert checksum back into buffer */
		*(uint16_t *)&tbuf[ck[i].ck_off] = cksum;
	}

	return (1);
}

static void
e82545_transmit_backend(struct e82545_softc *sc, struct iovec *iov, int iovcnt)
{

	if (sc->esc_tapfd == -1)
		return;

	(void) writev(sc->esc_tapfd, iov, iovcnt);
}

static void
e82545_transmit_done(struct e82545_softc *sc, union e1000_tx_udesc **txwb,
    int nwb)
{
	int i;

	/* Write-back tx descriptor status */
	for (i = 0; i < nwb; i++)
		txwb[i]->td.upper.data |= E1000_TXD_STAT_DD;
	/* XXX wmb() */
}

static int
e82545_transmit(struct e82545_softc *sc, uint16_t dsize, uint16_t start,
    uint16_t maxdesc, int *tdwb)
{
	uint8_t *tbuf;
        struct iovec iovb[I82545_MAX_TXSEGS + 2];
	struct iovec iovcks[3];
	union e1000_tx_udesc *txwb[I82545_MAX_TXSEGS];
	struct e1000_context_desc *cd;
	struct ck_info ckinfo[2];
        struct iovec *iov;
	union  e1000_tx_udesc *dsc;
	uint16_t idx;
	int dtype, i, len, ntype, nwb, segs, tlen;
	uint8_t vlanhdr[ETHER_VLAN_ENCAP_LEN];

	ckinfo[0].ck_valid = ckinfo[1].ck_valid = 0;
	segs = 0;
	tlen = 0;
	nwb = 0;
	idx = start;
	ntype = 0;

	/*
         * iovb[0/1] will be used for the optional VLAN tag insertion. If
	 * that is requested, iov will be backed up one and the VLAN inserted
	 * after the ethernet header. This may require splitting an existing
	 * segment in 2, hence the 2 additional iovec elements.
         */
	iov = &iovb[2];
	
	for (i = 0; i < maxdesc; i++, idx = (idx + 1) % dsize) {
		dsc = &sc->esc_txdesc[idx];
		dtype = e82545_txdesc_type(dsc->td.lower.data);

		if (i == 0) {
			switch (dtype) {
			case E1000_TXD_TYP_C:
				DPRINTF("tx ctxt desc, idx %d\n\r", idx);
				/* Save context and return */
				/* XXX ignore DD processing here */
				sc->esc_txctx = dsc->cd;
				return (1);
				break;
			case E1000_TXD_TYP_L:
				DPRINTF("tx legacy desc, idx %d %08x%08x\r\n",
				    idx, dsc->td.upper.data, dsc->td.lower.data);
				/*
				 * legacy cksum start valid in first descriptor
				 */
				ntype = dtype;
				ckinfo[0].ck_start = dsc->td.upper.fields.css;
				break;
			case E1000_TXD_TYP_D:
				DPRINTF("tx data desc, idx %d %08x%08x\r\n",
				    idx, dsc->td.upper.data, dsc->td.lower.data);
				ntype = dtype;
				break;
			default:
				break;
			}
		} else {
			/* Descriptor type must be consistent */
			assert(dtype == ntype);
			DPRINTF("tx next desc, idx %d %08x%08x\r\n",
			    idx, dsc->td.upper.data, dsc->td.lower.data);
		}

		len = (dtype == E1000_TXD_TYP_L) ? dsc->td.lower.flags.length :
		    dsc->dd.lower.data & 0xFFFFF;

		if (len > 0) {
			/* Strip checksum supplied by guest. */
			if ((dsc->td.lower.data & E1000_TXD_CMD_EOP) != 0 &&
			    (dsc->td.lower.data & E1000_TXD_CMD_IFCS) == 0)
				len -= 2;
			tlen += len;
			iov[segs].iov_base = paddr_guest2host(sc->esc_ctx,
			    dsc->td.buffer_addr, len);
			iov[segs].iov_len = len;
			segs++;
		}

		/* Record the descriptor addres if write-back requested */
		if (dsc->td.lower.data & E1000_TXD_CMD_RS)
			txwb[nwb++] = dsc;

		/*
		 * Pull out info that is valid in the final descriptor
		 * and exit descriptor loop.
		 */
		if (dsc->td.lower.data & E1000_TXD_CMD_EOP) {
			if (dtype == E1000_TXD_TYP_L) {
				if (dsc->td.lower.data & E1000_TXD_CMD_IC) {
					ckinfo[0].ck_valid = 1;
					ckinfo[0].ck_off =
					    dsc->td.lower.flags.cso;
					ckinfo[0].ck_len = 0;
				}
			} else {
				cd = &sc->esc_txctx;
				if (dsc->dd.upper.fields.popts &
				    E1000_TXD_POPTS_IXSM) {
					ckinfo[0].ck_valid = 1;
					ckinfo[0].ck_start =
					    cd->lower_setup.ip_fields.ipcss;
					ckinfo[0].ck_off =
					    cd->lower_setup.ip_fields.ipcso;
					ckinfo[0].ck_len =
					    cd->lower_setup.ip_fields.ipcse;
				}
				if (dsc->dd.upper.fields.popts &
				    E1000_TXD_POPTS_TXSM) {
					ckinfo[1].ck_valid = 1;
					ckinfo[1].ck_start =
					    cd->upper_setup.tcp_fields.tucss;
					ckinfo[1].ck_off =
					    cd->upper_setup.tcp_fields.tucso;
					ckinfo[1].ck_len =
					    cd->upper_setup.tcp_fields.tucse;
				}
			}
			break;
		}
	}

	/* We have incomplete packet.  Leave it for later. */
	if (i == maxdesc)
		return (-1);

	if (ckinfo[0].ck_valid || ckinfo[1].ck_valid) {
		DPRINTF("checksum found %d\r\n", 1);
		tbuf = __builtin_alloca(tlen);
		if (e82545_transmit_checksum(sc, iov, segs, tlen, tbuf,
		    ckinfo)) {
			/* Switch to new single iovec */
			iovcks[2].iov_base = tbuf;
			iovcks[2].iov_len = tlen;
			iov = &iovcks[2];
			segs = 1;
		}
	}

	if ((sc->esc_CTRL & E1000_CTRL_VME) &&
	    (dsc->td.lower.data & E1000_TXD_CMD_VLE)) {
		vlanhdr[0] = sc->esc_VET >> 8;
		vlanhdr[1] = sc->esc_VET & 0xff;
		vlanhdr[2] = dsc->td.upper.fields.special >> 8;
		vlanhdr[3] = dsc->td.upper.fields.special & 0xff;
		iov -= 2;
		segs += 2;
		iov[0].iov_base = iov[2].iov_base;
		iov[0].iov_len = ETHER_ADDR_LEN*2;
		iov[1].iov_base = &vlanhdr;
		iov[1].iov_len = ETHER_VLAN_ENCAP_LEN;
		iov[2].iov_base += ETHER_ADDR_LEN*2;
		iov[2].iov_len -= ETHER_ADDR_LEN*2;
	}

	e82545_transmit_backend(sc, iov, segs);
	e82545_transmit_done(sc, txwb, nwb);

	/* Record if tx descs were written back */
	if (nwb)
		*tdwb = 1;

	return (i + 1);
}

static void
e82545_start_tx(struct e82545_softc *sc, uint16_t value)
{
	uint32_t cause;
	uint16_t head, tail, ndesc, size;
	int descs, tdwb;

	/*
         * Get ring size in units of descriptors. Note that this
         * is not a pow-2, but a multiple of 8 descs.
         */
	size = sc->esc_TDLEN / 16;

	/* Range check the tail ptr: 0-based */
	assert(value < size);

        head = sc->esc_TDH;
	tail = sc->esc_TDT = value;
	
	tdwb = 0;
	ndesc = (size + tail - head) % size;

	DPRINTF("start_tx  ndesc %d, head %x, tail %x\r\n", ndesc, head, tail);

	if (ndesc == 0)
		return;

	for (; ndesc > 0; ndesc -= descs) {
		descs = e82545_transmit(sc, size, head, ndesc, &tdwb);
		if (descs < 0)
			break;
		head = (head + descs) % size;
	}

	/* Update the head pointer */
	sc->esc_TDH = head;

	DPRINTF("start_tx done: head %x, tail %x\r\n",
		sc->esc_TDH, sc->esc_TDT);
	
	/*
	 * Generate an interrupt if any tx descs were written back,
	 * and/or if the ring was completely processed.
	 */
	cause = 0;
	if (tdwb)
		cause |= E1000_ICR_TXDW;
	if (sc->esc_TDH == sc->esc_TDT)
		cause |= E1000_ICR_TXQE;

	if (cause)
		e82545_icr_assert(sc, cause);
}

static void
e82545_write_ra(struct e82545_softc *sc, int reg, uint32_t wval)
{
        struct eth_uni *eu;
	int idx;

	idx = reg >> 1;
	assert(idx < 15);

	eu = &sc->esc_uni[idx];

	if (reg & 0x1) {
		/* RAH */
		eu->eu_valid = ((wval & E1000_RAH_AV) == E1000_RAH_AV);
		eu->eu_addrsel = (wval >> 16) & 0x3;
		eu->eu_eth.octet[5] = wval >> 8;
		eu->eu_eth.octet[4] = wval;
	} else {
		/* RAL */
		eu->eu_eth.octet[3] = wval >> 24;
		eu->eu_eth.octet[2] = wval >> 16;
		eu->eu_eth.octet[1] = wval >> 8;
		eu->eu_eth.octet[0] = wval;
	}
}

static uint32_t
e82545_read_ra(struct e82545_softc *sc, int reg)
{
        struct eth_uni *eu;
	uint32_t retval;
	int idx;

	idx = reg >> 1;
	assert(idx < 15);

	eu = &sc->esc_uni[idx];

	if (reg & 0x1) {
		/* RAH */
		retval = (eu->eu_valid << 31) |
			 (eu->eu_addrsel << 16) |
			 (eu->eu_eth.octet[5] << 8) |
			 eu->eu_eth.octet[4];
	} else {
		/* RAL */
		retval = (eu->eu_eth.octet[3] << 24) |
			 (eu->eu_eth.octet[2] << 16) |
			 (eu->eu_eth.octet[1] << 8) |
			 eu->eu_eth.octet[0];
	}

	return (retval);	
}

static void
e82545_write_register(struct e82545_softc *sc, uint32_t offset, uint32_t value)
{
	int ridx;
	
	if (offset & 0x3) {
		DPRINTF("Unaligned register write offset:0x%x value:0x%x\r\n", offset, value);
		return;
	}
	DPRINTF("Register write: 0x%x value: 0x%x\r\n", offset, value);

	switch (offset) {
	case E1000_CTRL:
	case E1000_CTRL_DUP:
		e82545_devctl(sc, value);
		break;
	case E1000_FCAL:
		sc->esc_FCAL = value;
		break;
	case E1000_FCAH:
		sc->esc_FCAH = value & ~0xFFFF0000;
		break;
	case E1000_FCT:
		sc->esc_FCT = value & ~0xFFFF0000;
		break;
	case E1000_VET:
		sc->esc_VET = value & ~0xFFFF0000;
		break;
	case E1000_FCTTV:
		sc->esc_FCTTV = value & ~0xFFFF0000;
		break;
	case E1000_LEDCTL:
		sc->esc_LEDCTL = value & ~0x30303000;
		break;
	case E1000_PBA:
		sc->esc_PBA = value & 0x0000FF80;
		break;
	case E1000_ICR:
	case E1000_ITR:
	case E1000_ICS:
	case E1000_IMS:
	case E1000_IMC:
		e82545_intr_write(sc, offset, value);
		break;
	case E1000_RCTL:
		e82545_rx_ctl(sc, value);
		break;
	case E1000_FCRTL:
		sc->esc_FCRTL = value & ~0xFFFF0007;
		break;
	case E1000_FCRTH:
		sc->esc_FCRTH = value & ~0xFFFF0007;
		break;
	case E1000_RDBAL(0):
		sc->esc_RDBAL = value & ~0xF;
		if (sc->esc_rx_enabled) {
			/* Apparently legal: update cached address */
			e82545_rx_update_rdba(sc);
		}
		break;
	case E1000_RDBAH(0):
		assert(!sc->esc_rx_enabled);
		sc->esc_RDBAH = value;
		break;
	case E1000_RDLEN(0):
		assert(!sc->esc_rx_enabled);
		sc->esc_RDLEN = value & ~0xFFF0007F;
		break;
	case E1000_RDH(0):
		/* XXX should only ever be zero ? Range check ? */
		sc->esc_RDH = value;
		break;
	case E1000_RDT(0):
		/* XXX if this opens up the rx ring, do something ? */
		sc->esc_RDT = value;
		break;
	case E1000_RDTR:
		/* ignore FPD bit 31 */
		sc->esc_RDTR = value & ~0xFFFF0000;
		break;
	case E1000_RXDCTL(0):
		sc->esc_RXDCTL = value & ~0xFEC0C0C0;
		break;
	case E1000_RADV:
		sc->esc_RADV = value & ~0xFFFF0000;
		break;
	case E1000_RSRPD:
		sc->esc_RSRPD = value & ~0xFFFFF000;
		break;
	case E1000_RXCSUM:
		sc->esc_RXCSUM = value & ~0xFFFFF800;
		break;
	case E1000_TXCW:
		sc->esc_TXCW = value & ~0x3FFF0000;
		break;
	case E1000_TCTL:
		e82545_tx_ctl(sc, value);
		break;
	case E1000_TIPG:
		sc->esc_TIPG = value;
		break;
	case E1000_AIT:
		sc->esc_AIT = value;
		break;
	case E1000_TDBAL(0):
		sc->esc_TDBAL = value & ~0xF;
		if (sc->esc_tx_enabled) {
			/* Apparently legal */
			e82545_tx_update_tdba(sc);
		}
		break;
	case E1000_TDBAH(0):
		//assert(!sc->esc_tx_enabled);		
		sc->esc_TDBAH = value;
		break;
	case E1000_TDLEN(0):
		//assert(!sc->esc_tx_enabled);
		sc->esc_TDLEN = value & ~0xFFF0007F;
		break;
	case E1000_TDH(0):
		//assert(!sc->esc_tx_enabled);
		/* XXX should only ever be zero ? Range check ? */
		sc->esc_TDH = value;
		break;
	case E1000_TDT(0):
		/* XXX range check ? */
		if (sc->esc_tx_enabled)
			e82545_start_tx(sc, (uint16_t)value);
		else
			sc->esc_TDT = value;
		break;
	case E1000_TIDV:
		sc->esc_TIDV = value & ~0xFFFF0000;
		break;
	case E1000_TXDCTL(0):
		//assert(!sc->esc_tx_enabled);
		sc->esc_TXDCTL = value & ~0xC0C0C0;
		break;
	case E1000_TADV:
		sc->esc_TADV = value & ~0xFFFF0000;
		break;
	case E1000_RAL(0) ... E1000_RAH(15):
		/* convert to u32 offset */
		ridx = (offset - E1000_RAL(0)) >> 2;
		e82545_write_ra(sc, ridx, value);
		break;
	case E1000_MTA ... (E1000_MTA + (127*4)):
		sc->esc_fmcast[(offset - E1000_MTA) >> 2] = value;
		break;
	case E1000_VFTA ... (E1000_VFTA + (127*4)):
		sc->esc_fvlan[(offset - E1000_VFTA) >> 2] = value;
		break;		
	case E1000_EECD:
	{
		//DPRINTF("EECD write 0x%x -> 0x%x\r\n", sc->eeprom_control, value);
		/* edge triggered low->high */
		uint32_t eecd_strobe = ((sc->eeprom_control & E1000_EECD_SK) ?
			0 : (value & E1000_EECD_SK));
		uint32_t eecd_mask = (E1000_EECD_SK|E1000_EECD_CS|
					E1000_EECD_DI|E1000_EECD_REQ);
		sc->eeprom_control &= ~eecd_mask;
		sc->eeprom_control |= (value & eecd_mask);
		/* grant/revoke immediately */
		if (value & E1000_EECD_REQ) {
			sc->eeprom_control |= E1000_EECD_GNT;
		} else {
                        sc->eeprom_control &= ~E1000_EECD_GNT;
		}
		if (eecd_strobe && (sc->eeprom_control & E1000_EECD_CS)) {
			e82545_eecd_strobe(sc);
		}
		return;
	}
	case E1000_MDIC:
	{
		uint8_t reg_addr = (uint8_t)((value & E1000_MDIC_REG_MASK) >>
						E1000_MDIC_REG_SHIFT);
		uint8_t phy_addr = (uint8_t)((value & E1000_MDIC_PHY_MASK) >>
						E1000_MDIC_PHY_SHIFT);
		sc->mdi_control =
			(value & ~(E1000_MDIC_ERROR|E1000_MDIC_DEST));
		if ((value & E1000_MDIC_READY) != 0) {
			DPRINTF("Incorrect MDIC ready bit: 0x%x\r\n", value);
			return;
		}
		switch (value & E82545_MDIC_OP_MASK) {
		case E1000_MDIC_OP_READ:
			sc->mdi_control &= ~E82545_MDIC_DATA_MASK;
			sc->mdi_control |= e82545_read_mdi(sc, reg_addr, phy_addr);
			break;
		case E1000_MDIC_OP_WRITE:
			e82545_write_mdi(sc, reg_addr, phy_addr,
				value & E82545_MDIC_DATA_MASK);
			break;
		default:
			DPRINTF("Unknown MDIC op: 0x%x\r\n", value);
			return;
		}
		/* TODO: barrier? */
		sc->mdi_control |= E1000_MDIC_READY;
		if (value & E82545_MDIC_IE) {
			// TODO: generate interrupt
		}
		return;
	}
	case E1000_MANC:
	case E1000_STATUS: 
		return;
	default:
		DPRINTF("Unknown write register: 0x%x value:%x\r\n", offset, value);
		return;
	}
}

static uint32_t
e82545_read_register(struct e82545_softc *sc, uint32_t offset)
{
	uint32_t retval;
	int ridx;
	
	if (offset & 0x3) {
		DPRINTF("Unaligned register read offset:0x%x\r\n", offset);
		return 0;
	}
		
	DPRINTF("Register read: 0x%x\r\n", offset);

	switch (offset) {
	case E1000_CTRL:
		retval = sc->esc_CTRL;
		break;
	case E1000_STATUS:
		retval = E1000_STATUS_FD | E1000_STATUS_LU |
		    E1000_STATUS_SPEED_1000;
		break;
	case E1000_FCAL:
		retval = sc->esc_FCAL;
		break;
	case E1000_FCAH:
		retval = sc->esc_FCAH;
		break;
	case E1000_FCT:
		retval = sc->esc_FCT;
		break;
	case E1000_VET:
		retval = sc->esc_VET;
		break;
	case E1000_FCTTV:
		retval = sc->esc_FCTTV;
		break;
	case E1000_LEDCTL:
		retval = sc->esc_LEDCTL;
		break;
	case E1000_PBA:
		retval = sc->esc_PBA;
		break;
	case E1000_ICR:
	case E1000_ITR:
	case E1000_ICS:
	case E1000_IMS:
	case E1000_IMC:
		retval = e82545_intr_read(sc, offset);
		break;
	case E1000_RCTL:
		retval = sc->esc_RCTL;
		break;
	case E1000_FCRTL:
		retval = sc->esc_FCRTL;
		break;
	case E1000_FCRTH:
		retval = sc->esc_FCRTH;
		break;
	case E1000_RDBAL(0):
		retval = sc->esc_RDBAL;
		break;
	case E1000_RDBAH(0):
		retval = sc->esc_RDBAH;
		break;
	case E1000_RDLEN(0):
		retval = sc->esc_RDLEN;
		break;
	case E1000_RDH(0):
		retval = sc->esc_RDH;
		break;
	case E1000_RDT(0):
		retval = sc->esc_RDT;
		break;
	case E1000_RDTR:
		retval = sc->esc_RDTR;
		break;
	case E1000_RXDCTL(0):
		retval = sc->esc_RXDCTL;
		break;
	case E1000_RADV:
		retval = sc->esc_RADV;
		break;
	case E1000_RSRPD:
		retval = sc->esc_RSRPD;
		break;
	case E1000_RXCSUM:	       
		retval = sc->esc_RXCSUM;
		break;
	case E1000_TXCW:
		retval = sc->esc_TXCW;
		break;
	case E1000_TCTL:
		retval = sc->esc_TCTL;
		break;
	case E1000_TIPG:
		retval = sc->esc_TIPG;
		break;
	case E1000_AIT:
		retval = sc->esc_AIT;
		break;
	case E1000_TDBAL(0):
		retval = sc->esc_TDBAL;
		break;
	case E1000_TDBAH(0):
		retval = sc->esc_TDBAH;
		break;
	case E1000_TDLEN(0):
		retval = sc->esc_TDLEN;
		break;
	case E1000_TDH(0):
		retval = sc->esc_TDH;
		break;
	case E1000_TDT(0):
		retval = sc->esc_TDT;
		break;
	case E1000_TIDV:
		retval = sc->esc_TIDV;
		break;
	case E1000_TXDCTL(0):
		retval = sc->esc_TXDCTL;
		break;
	case E1000_TADV:
		retval = sc->esc_TADV;
		break;
	case E1000_RAL(0) ... E1000_RAH(15):
		/* convert to u32 offset */
		ridx = (offset - E1000_RAL(0)) >> 2;
		retval = e82545_read_ra(sc, ridx);
		break;
	case E1000_MTA ... (E1000_MTA + (127*4)):
		retval = sc->esc_fmcast[(offset - E1000_MTA) >> 2];
		break;
	case E1000_VFTA ... (E1000_VFTA + (127*4)):
		retval = sc->esc_fvlan[(offset - E1000_VFTA) >> 2];
		break;		
	case E1000_EECD:
		//DPRINTF("EECD read %x\r\n", sc->eeprom_control);
		retval = sc->eeprom_control;
		break;
	case E1000_MDIC:
		retval = sc->mdi_control;
		break;
	case E1000_MANC:
		retval = 0;
		break;
	/* stats that we emulate. */
	case E1000_MPC:
		retval = sc->missed_pkt_count;
		break;
	case E1000_PRC64:
		retval = sc->pkt_rx_by_size[0];
		break;
	case E1000_PRC127:
		retval = sc->pkt_rx_by_size[1];
		break;
	case E1000_PRC255:
		retval = sc->pkt_rx_by_size[2];
		break;
	case E1000_PRC511:
		retval = sc->pkt_rx_by_size[3];
		break;
	case E1000_PRC1023:
		retval = sc->pkt_rx_by_size[4];
		break;
	case E1000_PRC1522:
		retval = sc->pkt_rx_by_size[5];
		break;
	case E1000_GPRC:
		retval = sc->good_pkt_rx_count;
		break;
	case E1000_BPRC:
		retval = sc->bcast_pkt_rx_count;
		break;
	case E1000_MPRC:
		retval = sc->mcast_pkt_rx_count;
		break;
	case E1000_GPTC:
	case E1000_TPT:
		retval = sc->good_pkt_tx_count;
		break;
	case E1000_GORCL:
		retval = (uint32_t)sc->good_octets_rx;
		break;
	case E1000_GORCH:
		retval = (uint32_t)(sc->good_octets_rx >> 32);
		break;
	case E1000_TOTL:
	case E1000_GOTCL:
		retval = (uint32_t)sc->good_octets_tx;
		break;
	case E1000_TOTH:
	case E1000_GOTCH:
		retval = (uint32_t)(sc->good_octets_tx >> 32);
		break;
	case E1000_ROC:
		retval = sc->oversize_rx_count;
		break;
	case E1000_TORL:
		retval = (uint32_t)(sc->good_octets_rx + sc->missed_octets);
		break;
	case E1000_TORH:
		retval = (uint32_t)((sc->good_octets_rx +
		    sc->missed_octets) >> 32);
		break;
	case E1000_TPR:
		retval = sc->good_pkt_rx_count + sc->missed_pkt_count +
		    sc->oversize_rx_count;
		break;
	case E1000_PTC64:
		retval = sc->pkt_tx_by_size[0];
		break;
	case E1000_PTC127:
		retval = sc->pkt_tx_by_size[1];
		break;
	case E1000_PTC255:
		retval = sc->pkt_tx_by_size[2];
		break;
	case E1000_PTC511:
		retval = sc->pkt_tx_by_size[3];
		break;
	case E1000_PTC1023:
		retval = sc->pkt_tx_by_size[4];
		break;
	case E1000_PTC1522:
		retval = sc->pkt_tx_by_size[5];
		break;
	case E1000_MPTC:
		retval = sc->mcast_pkt_tx_count;
		break;
	case E1000_BPTC:
		retval = sc->bcast_pkt_tx_count;
		break;
	case E1000_TSCTC:
		retval = sc->tso_tx_count;
		break;
	/* stats that are always 0. */
	case E1000_CRCERRS:
	case E1000_ALGNERRC:
	case E1000_SYMERRS:
	case E1000_RXERRC:
	case E1000_SCC:
	case E1000_ECOL:
	case E1000_MCC:
	case E1000_LATECOL:
	case E1000_COLC:
	case E1000_DC:
	case E1000_TNCRS:
	case E1000_SEC:
	case E1000_CEXTERR:
	case E1000_RLEC:
	case E1000_XONRXC:
	case E1000_XONTXC:
	case E1000_XOFFRXC:
	case E1000_XOFFTXC:
	case E1000_FCRUC:
	case E1000_RNBC:
	case E1000_RUC:
	case E1000_RFC:
	case E1000_RJC:
	case E1000_MGTPRC:
	case E1000_MGTPDC:
	case E1000_MGTPTC:
	case E1000_TSCTFC:
		retval = 0;
		break;
	default:
		DPRINTF("Unknown read register: 0x%x\r\n", offset);
		break;
	}

	return (retval);
}

static void
e82545_write(struct vmctx *ctx, int vcpu, struct pci_devinst *pi, int baridx,
	     uint64_t offset, int size, uint64_t value)
{
	struct e82545_softc *sc;

	//DPRINTF("Write bar:%d offset:0x%lx value:0x%lx size:%d\r\n", baridx, offset, value, size);

	sc = pi->pi_arg;

	pthread_mutex_lock(&sc->esc_mtx);

	switch (baridx) {
	case E82545_BAR_IO:
		switch (offset) {
		case E82545_IOADDR:
			if (size != 4) {
				DPRINTF("Wrong io addr write sz:%d value:0x%lx\r\n", size, value);
			} else
				sc->io_addr = (uint32_t)value;
			break;
		case E82545_IODATA:
			if (size != 4) {
				DPRINTF("Wrong io data write size:%d value:0x%lx\r\n", size, value);
			} else if (sc->io_addr > E82545_IO_REGISTER_MAX) {
				DPRINTF("Non-register io write addr:0x%x value:0x%lx\r\n", sc->io_addr, value);
			} else
				e82545_write_register(sc, sc->io_addr,
						      (uint32_t)value);
			break;
		default:
			DPRINTF("Unknown io bar write offset:0x%lx value:0x%lx size:%d\r\n", offset, value, size);
			break;
		}
	case E82545_BAR_REGISTER:
		if (size != 4) {
			DPRINTF("Wrong register write size:%d offset:0x%lx value:0x%lx\r\n", size, offset, value);
		} else
			e82545_write_register(sc, (uint32_t)offset,
					      (uint32_t)value);
		break;
	default:
		DPRINTF("Unknown write bar:%d off:0x%lx val:0x%lx size:%d\r\n",
			baridx, offset, value, size);
	}

	pthread_mutex_unlock(&sc->esc_mtx);
}

static uint64_t
e82545_read(struct vmctx *ctx, int vcpu, struct pci_devinst *pi, int baridx,
	    uint64_t offset, int size)
{
	struct e82545_softc *sc;
	uint64_t retval;
	
	//DPRINTF("Read  bar:%d offset:0x%lx size:%d\r\n", baridx, offset, size);
	sc = pi->pi_arg;
	retval = 0;

	pthread_mutex_lock(&sc->esc_mtx);

	switch (baridx) {
	case E82545_BAR_IO:
		switch (offset) {
		case E82545_IOADDR:
			if (size != 4) {
				DPRINTF("Wrong io addr read sz:%d\r\n", size);
			} else
				retval = sc->io_addr;
			break;
		case E82545_IODATA:
			if (size != 4) {
				DPRINTF("Wrong io data read sz:%d\r\n", size);
			}
			if (sc->io_addr > E82545_IO_REGISTER_MAX) {
				DPRINTF("Non-register io read addr:0x%x\r\n",
					sc->io_addr);
			} else
				retval = e82545_read_register(sc, sc->io_addr);
			break;
		default:
			DPRINTF("Unknown io bar read offset:0x%lx size:%d\r\n",
				offset, size);
			break;
		}
	case E82545_BAR_REGISTER:
		if (size != 4) {
			DPRINTF("Wrong register read size:%d offset:0x%lx\r\n",
				size, offset);
		} else
			retval = e82545_read_register(sc, (uint32_t)offset);
		break;
	default:
		DPRINTF("Unknown read bar:%d offset:0x%lx size:%d\r\n",
			baridx, offset, size);
		break;
	}

	pthread_mutex_unlock(&sc->esc_mtx);

	return (retval);
}

static void
e82545_reset(struct e82545_softc *sc, int drvr)
{
	int i;
	
	/* clear outstanding interrupts */
	if (sc->esc_irq_asserted)
		pci_lintr_deassert(sc->esc_pi);

	/* misc */
	if (!drvr) {
		sc->esc_FCAL = 0;
		sc->esc_FCAH = 0;
		sc->esc_FCT = 0;
		sc->esc_VET = 0;
		sc->esc_FCTTV = 0;
	}
	sc->esc_LEDCTL = 0x07061302;
	sc->esc_PBA = 0x00100030;
	
	/* start nvm in opcode mode. */
	sc->nvm_opaddr = 0;
	sc->nvm_mode = E82545_NVM_MODE_OPADDR;
	sc->nvm_bits = E82545_NVM_OPADDR_BITS;
	sc->eeprom_control = E1000_EECD_PRES | E82545_EECD_FWE_EN;
	e82545_init_eeprom(sc);

	/* interrupt */
	sc->esc_ICR = 0;
	sc->esc_ITR = 250;
	sc->esc_ICS = 0;
	sc->esc_IMS = 0;
	sc->esc_IMC = 0;
		
	/* L2 filters */
	if (!drvr) {
		memset(sc->esc_fvlan, 0, sizeof(sc->esc_fvlan));
		memset(sc->esc_fmcast, 0, sizeof(sc->esc_fmcast));
		memset(sc->esc_uni, 0, sizeof(sc->esc_uni));

		/* XXX not necessary on 82545 ?? */
		sc->esc_uni[0].eu_valid = 1;
		memcpy(sc->esc_uni[0].eu_eth.octet, sc->esc_mac.octet,
		    ETHER_ADDR_LEN);
	} else {
		/* Clear RAH valid bits */
		for (i = 0; i < 16; i++)
			sc->esc_uni[i].eu_valid = 0;
	}
	
	/* receive */
	if (!drvr) {
		sc->esc_RDBAL = 0;
		sc->esc_RDBAH = 0;
	}
	sc->esc_rx_enabled = 0;
	sc->esc_RCTL = 0;
	sc->esc_FCRTL = 0;
	sc->esc_FCRTH = 0;
	sc->esc_RDLEN = 0;
	sc->esc_RDH = 0;
	sc->esc_RDT = 0;
	sc->esc_RDTR = 0;
	sc->esc_RXDCTL = (1 << 24) | (1 << 16); /* default GRAN/WTHRESH */
	sc->esc_RADV = 0;
	sc->esc_RXCSUM = 0;

	/* transmit */
	if (!drvr) {
		sc->esc_TDBAL = 0;
		sc->esc_TDBAH = 0;
		sc->esc_TIPG = 0;
		sc->esc_AIT = 0;
		sc->esc_TIDV = 0;
		sc->esc_TADV = 0;
	}
	sc->esc_tx_enabled = 0;
	sc->esc_tdba = 0;
	sc->esc_txdesc = NULL;
	sc->esc_TXCW = 0;
	sc->esc_TCTL = 0;
	sc->esc_TDLEN = 0;
	sc->esc_TDT = 0;
	sc->esc_TDH = 0;
	sc->esc_TXDCTL = 0;
}

static void
e82545_open_tap(struct e82545_softc *sc, char *opts)
{
	char tbuf[80];
	
	if (opts == NULL) {
		sc->esc_tapfd = -1;
		return;
	}

	strcpy(tbuf, "/dev/");
	strlcat(tbuf, opts, sizeof(tbuf));

	sc->esc_tapfd = open(tbuf, O_RDWR);
	if (sc->esc_tapfd == -1) {
		DPRINTF("unable to open tap device %s\n", opts);
		exit(1);
	} else {
		sc->esc_mevp = mevent_add(sc->esc_tapfd,
					  EVF_READ,
					  e82545_tap_callback,
					  sc);
		if (sc->esc_mevp == NULL) {
			DPRINTF("Could not register mevent %d\n", EVF_READ);
			close(sc->esc_tapfd);
			sc->esc_tapfd = -1;
		}
	}
}

static int
e82545_init(struct vmctx *ctx, struct pci_devinst *pi, char *opts)
{
	DPRINTF("Loading with options: %s\r\n", opts);

	MD5_CTX mdctx;
	unsigned char digest[16];
	char nstr[80];
	struct e82545_softc *sc;

	/* Setup our softc */
	sc = calloc(sizeof(*sc), 1);

	pi->pi_arg = sc;
	sc->esc_pi = pi;
	sc->esc_ctx = ctx;

	pthread_mutex_init(&sc->esc_mtx, NULL);

	pci_set_cfgdata16(pi, PCIR_DEVICE, E82545_DEV_ID_82545EM_COPPER);
	pci_set_cfgdata16(pi, PCIR_VENDOR, E82545_VENDOR_ID_INTEL);
	pci_set_cfgdata8(pi,  PCIR_CLASS, PCIC_NETWORK);
	pci_set_cfgdata8(pi, PCIR_SUBCLASS, PCIS_NETWORK_ETHERNET);
	pci_set_cfgdata16(pi, PCIR_SUBDEV_0, E82545_SUBDEV_ID);
	pci_set_cfgdata16(pi, PCIR_SUBVEND_0, E82545_VENDOR_ID_INTEL);

	pci_set_cfgdata8(pi,  PCIR_HDRTYPE, PCIM_HDRTYPE_NORMAL);
	pci_set_cfgdata8(pi,  PCIR_INTPIN, 0x1);
	
	/* TODO: this card also supports msi, but the freebsd driver for it
	 * does not, so I have not implemented it. */
	pci_lintr_request(pi);

	pci_emul_alloc_bar(pi, E82545_BAR_REGISTER, PCIBAR_MEM32,
		E82545_BAR_REGISTER_LEN);
	pci_emul_alloc_bar(pi, E82545_BAR_FLASH, PCIBAR_MEM32,
		E82545_BAR_FLASH_LEN);
	pci_emul_alloc_bar(pi, E82545_BAR_IO, PCIBAR_IO,
		E82545_BAR_IO_LEN);

	/* Copied from virtio-net, slightly modified */
	/*
	 * The MAC address is the standard NetApp OUI of 00-a0-98,
	 * followed by an MD5 of the vm name. The slot/func number is
	 * prepended to this for slots other than 1:0, so that 
	 * a bootloader can netboot from the equivalent of slot 1.
	 */
	snprintf(nstr, sizeof(nstr), "e82545-%d-%d-%s", pi->pi_slot,
		 pi->pi_func, vmname);

	MD5Init(&mdctx);
	MD5Update(&mdctx, nstr, strlen(nstr));
	MD5Final(digest, &mdctx);

	sc->esc_mac.octet[0] = 0x00;
	sc->esc_mac.octet[1] = 0xa0;
	sc->esc_mac.octet[2] = 0x98;
	sc->esc_mac.octet[3] = digest[0];
	sc->esc_mac.octet[4] = digest[1];
	sc->esc_mac.octet[5] = digest[2] & 0xFE; /* make even */

	/* Open the tap device */
	e82545_open_tap(sc, opts);
	
	/* H/w initiated reset */
	e82545_reset(sc, 0);

	return (0);
}

struct pci_devemu pci_de_e82545 = {
	.pe_emu = 	"e1000",
	.pe_init =	e82545_init,
	.pe_barwrite =	e82545_write,
	.pe_barread =	e82545_read
};
PCI_EMUL_SET(pci_de_e82545);

