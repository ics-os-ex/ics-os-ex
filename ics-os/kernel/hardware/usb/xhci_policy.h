#ifndef XHCI_POLICY_H
#define XHCI_POLICY_H

/*
  Physical Intel xHCI (N150) vs QEMU.

  MSI-X messages are composed as xAPIC (address 0xFEE00000). UEFI on the
  N150 leaves x2APIC on; MMIO to that address hung the PCH. Poll until an
  x2APIC/IR MSI path exists. QEMU has COM1 and xAPIC, so MSI-X stays on.
*/
static inline int xhci_allow_msix(int com1_present, int x2apic)
{
    if (!com1_present)
        return 0;
    if (x2apic)
        return 0;
    return 1;
}

/* After HC reset, real ports can take a while to re-assert CCS. QEMU is
   already connected on the first spin. Bounded; uses the same delay helper
   as xhci_wait32. */
#define XHCI_PORT_SETTLE_SPINS 400000u

/*
  CDC gadget bulk IN. MicroPython drops TX unless a host IN TRB is already
  posted. delay() ticks are 10ms (rate 100, delay(w) waits 2w ticks), so
  delay(100) left a ~12ms listen window every 2s and Pico RPC never landed.
  Pump first, then a 1-tick wait while the console is bound.
*/
#define XHCI_CDC_IN_SPINS            40000u
#define XHCI_CDC_HOTPLUG_DELAY_IDLE  100
#define XHCI_CDC_HOTPLUG_DELAY_READY 1

static inline unsigned int xhci_cdc_hotplug_delay(int cdc_ready)
{
    return cdc_ready ? XHCI_CDC_HOTPLUG_DELAY_READY
                     : XHCI_CDC_HOTPLUG_DELAY_IDLE;
}

static inline unsigned int xhci_ccs_bit(unsigned int port)
{
    if (!port || port > 32)
        return 0;
    return 1u << (port - 1);
}

static inline unsigned int xhci_cdc_extra_ccs(unsigned int ccs,
                                               unsigned int msc_port)
{
    return ccs & ~xhci_ccs_bit(msc_port);
}

static inline int xhci_cdc_should_drop(int cdc_ready, int cdc_connected)
{
    return cdc_ready && !cdc_connected;
}

static inline int xhci_cdc_should_rebind(int cdc_ready,
                                         unsigned int extra_ccs)
{
    return !cdc_ready && extra_ccs != 0;
}

/*
  Control TD flags. xHCI 1.2 6.4.1.2.1: Setup Stage TRB bits 1-4 are
  RsvdZ, so Chain (bit 4) must stay clear. Linux also leaves Chain
  clear on Data and Status; Intel ADL-N stalled or timed out GET_DESCRIPTOR
  when Setup had Chain set. QEMU ignores the reserved bit.
*/
#define XHCI_CTRL_TRB_ISP     (1u << 2)
#define XHCI_CTRL_TRB_CHAIN   (1u << 4)
#define XHCI_CTRL_TRB_IOC     (1u << 5)
#define XHCI_CTRL_TRB_IDT     (1u << 6)
#define XHCI_CTRL_TRB_DIR_IN  (1u << 16)
#define XHCI_CTRL_TRB_TYPE(n) ((unsigned int)(n) << 10)

static inline unsigned int xhci_ctrl_setup_flags(int has_data, int dir_in)
{
    unsigned int flags = XHCI_CTRL_TRB_IDT | XHCI_CTRL_TRB_TYPE(2);

    if (has_data)
        flags |= (dir_in ? 3u : 2u) << 16;
    return flags;
}

static inline unsigned int xhci_ctrl_data_flags(int dir_in)
{
    unsigned int flags = XHCI_CTRL_TRB_TYPE(3);

    if (dir_in)
        flags |= XHCI_CTRL_TRB_DIR_IN | XHCI_CTRL_TRB_ISP;
    return flags;
}

static inline unsigned int xhci_ctrl_status_flags(int has_data, int dir_in)
{
    unsigned int flags = XHCI_CTRL_TRB_TYPE(4) | XHCI_CTRL_TRB_IOC;

    if (!has_data || !dir_in)
        flags |= XHCI_CTRL_TRB_DIR_IN;
    return flags;
}

/* Normal TRB: IOC plus ISP so a short CDC packet still raises an event. */
static inline unsigned int xhci_cdc_in_trb_flags(void)
{
    return XHCI_CTRL_TRB_IOC | XHCI_CTRL_TRB_ISP;
}

/* Empty IN polls keep the TRB posted. Stop+SetDequeue on every timeout
   wedged Intel xHCI after the first short packet. */
static inline int xhci_cdc_in_stop_on_timeout(void)
{
    return 0;
}

/* Do not Stop-EP a posted CDC IN at MSC entry. Event-ring stash keeps
   SCSI waits from stealing the completion. Stop+SetDequeue once per
   block (pump re-posts between reads) wedges Intel ADL-N CDC IN after
   a 2nd ls: console still types, Pico RPC goes 504. Empty-poll Stop
   stays forbidden (xhci_cdc_in_stop_on_timeout). */
static inline int xhci_cdc_in_cancel_for_msc(void)
{
    return 0;
}

#endif
