//lifterd from github.com/szhou42/osdev

#include "rtl8139.h"
#include "pci.h"
#include "../../debug/klog.h"
#include "../chips/ports.h"
//#include <serial.h>
//#include <string.h>
//#include <xxd.h>

pci_dev_t pci_rtl8139_device;
rtl8139_dev_t rtl8139_device;
static int rtl8139_present = 0; // set to 1 once we positively identify the device

DWORD current_packet_ptr;

// Simple transmit buffers (4 descriptors) - physically contiguous in low memory (assumed identity mapped)
static uint8_t tx_buffers[4][2048] __attribute__((aligned(4)));
static int rtl8139_poll_mode = 0; // fallback if IRQ not firing

static void rtl8139_poll(void) {
    // Read interrupt status without acknowledging everything prematurely
    WORD isr = inports(rtl8139_device.io_base + IntrStatus);
    if(!isr) return;
    if(isr & ROK) {
        klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Poll ROK");
        receive_packet();
    }
    if(isr & RER) {
        klog_warn(KLOG_SUBSYS_NETWORK, "RTL8139: RX error (ISR=0x%04x)", isr);
    }
    if(isr & TOK) {
        klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Poll TOK");
    }
    // Ack bits we handled
    outports(rtl8139_device.io_base + IntrStatus, isr);
}

void rtl8139_periodic() {
    if(rtl8139_poll_mode) {
        rtl8139_poll();
    }
}

// Four TXAD register, you must use a different one to send packet each time(for example, use the first one, second... fourth and back to the first)
uint8_t TSAD_array[4] = {0x20, 0x24, 0x28, 0x2C};
uint8_t TSD_array[4] = {0x10, 0x14, 0x18, 0x1C};


/*
 * Write 2 bytes
 * */
void outports(WORD _port, WORD _data) {
    asm volatile ("outw %1, %0" : : "dN" (_port), "a" (_data));
}


/*
 * Readt 4 bytes
 * */
//DWORD inportl(WORD _port) {
//    DWORD rv;
//    asm volatile ("inl %%dx, %%eax" : "=a" (rv) : "dN" (_port));
//    return rv;
//}

/*
 * Write 4 bytes
 * */
//void outportl(WORD _port, DWORD _data) {
//    asm volatile ("outl %%eax, %%dx" : : "dN" (_port), "a" (_data));
//}


void receive_packet() {
    // Layout: [status(2)][len(2)][data][CRC(4)]
    WORD *hdr = (WORD*)(rtl8139_device.rx_buffer + current_packet_ptr);
    WORD status = hdr[0];
    WORD packet_length = hdr[1];
    (void)status;
    
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Receiving packet, status=0x%04x, length=%d", status, packet_length);
    
    if(packet_length == 0 || packet_length > 1600) {
        klog_warn(KLOG_SUBSYS_NETWORK, "RTL8139: Invalid packet length %d, resetting buffer", packet_length);
        current_packet_ptr = 0; // reset
        outports(rtl8139_device.io_base + CAPR, current_packet_ptr - 0x10);
        return;
    }
    uint8_t *pkt = (uint8_t*)(hdr + 2);
    static uint8_t copy_buf[1600];
    unsigned copy_len = packet_length > sizeof(copy_buf) ? sizeof(copy_buf) : packet_length;
    memcpy(copy_buf, pkt, copy_len);
    
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Packet received successfully, forwarding to network stack");
    ethernet_handle_packet(copy_buf, copy_len);

    current_packet_ptr = (current_packet_ptr + packet_length + 4 + 3) & RX_READ_POINTER_MASK;
    if(current_packet_ptr > RX_BUF_SIZE)
        current_packet_ptr -= RX_BUF_SIZE;
    outports(rtl8139_device.io_base + CAPR, current_packet_ptr - 0x10);
}

void rtl8139_handler(register_t * reg) {
    WORD status = inports(rtl8139_device.io_base + 0x3e);
    
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Interrupt fired, status=0x%04x", status);

    if(status & TOK) {
        klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Packet sent successfully");
    }
    if (status & ROK) {
        klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Packet received, processing...");
        receive_packet();
    }

    outports(rtl8139_device.io_base + 0x3E, 0x5);
}

void read_mac_addr() {
    DWORD mac_part1 = inportl(rtl8139_device.io_base + 0x00);
    WORD mac_part2 = inports(rtl8139_device.io_base + 0x04);
    rtl8139_device.mac_addr[0] = mac_part1 >> 0;
    rtl8139_device.mac_addr[1] = mac_part1 >> 8;
    rtl8139_device.mac_addr[2] = mac_part1 >> 16;
    rtl8139_device.mac_addr[3] = mac_part1 >> 24;

    rtl8139_device.mac_addr[4] = mac_part2 >> 0;
    rtl8139_device.mac_addr[5] = mac_part2 >> 8;
    
    klog_info(KLOG_SUBSYS_NETWORK, "RTL8139: MAC Address: %02x:%02x:%02x:%02x:%02x:%02x", 
              rtl8139_device.mac_addr[0], rtl8139_device.mac_addr[1], rtl8139_device.mac_addr[2], 
              rtl8139_device.mac_addr[3], rtl8139_device.mac_addr[4], rtl8139_device.mac_addr[5]);
}

void get_mac_addr(uint8_t * src_mac_addr) {
    memcpy(src_mac_addr, rtl8139_device.mac_addr, 6);
}

void rtl8139_send_packet(void * data, DWORD len) {
    if(!rtl8139_present) {
        // Silently drop; device not available
        return;
    }
    if(len == 0) return;
    if(len > 1514) len = 1514; // clamp
    // Ensure minimum ethernet payload (pad) (excluding CRC which NIC adds); 60 bytes total frame min
    DWORD send_len = len < 60 ? 60 : len;
    uint8_t idx = rtl8139_device.tx_cur & 3;
    memcpy(tx_buffers[idx], data, len);
    if(send_len > len) memset(tx_buffers[idx] + len, 0, send_len - len);
    // Provide buffer physical address
    outportl(rtl8139_device.io_base + TSAD_array[idx], (DWORD)tx_buffers[idx]);
    // Kick transmission (length in low 13 bits). Set OWN cleared, set early threshold default.
    outportl(rtl8139_device.io_base + TSD_array[idx], send_len & 0x1FFF);
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Queued TX idx=%u len=%u (orig=%u)", idx, (unsigned)send_len, (unsigned)len);
    rtl8139_device.tx_cur = (idx + 1) & 3;
}

/*
 * Initialize the rtl8139 card driver
 * */
void rtl8139_init() {
    // First get the network device using PCI
    klog_info(KLOG_SUBSYS_NETWORK, "RTL8139: Initializing RTL8139 network card...");
    
    pci_rtl8139_device = icsos_pci_get_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, -1);

    if(!pci_rtl8139_device.bits) {
    klog_err(KLOG_SUBSYS_NETWORK, "RTL8139: PCI device not found. Did you start QEMU with -device rtl8139 ? Initialization aborted.");
        rtl8139_present = 0;
        return;
    }

    // Double-check vendor/device IDs (enumeration code may return a struct with bits set but not matching device)
    DWORD vend = icsos_pci_read(pci_rtl8139_device, PCI_VENDOR_ID) & 0xFFFF;
    DWORD devid = icsos_pci_read(pci_rtl8139_device, PCI_DEVICE_ID) & 0xFFFF;
    if(vend != RTL8139_VENDOR_ID || devid != RTL8139_DEVICE_ID) {
        klog_err(KLOG_SUBSYS_NETWORK, "RTL8139: Mismatch or absent device (read vendor=0x%04x device=0x%04x). Aborting init.", (unsigned)vend, (unsigned)devid);
        rtl8139_present = 0;
        return;
    }

    // Enable I/O + Bus Mastering early before touching BAR
    DWORD pci_command_reg0 = icsos_pci_read(pci_rtl8139_device, PCI_COMMAND);
    DWORD orig_cmd = pci_command_reg0;
    pci_command_reg0 |= 0x05; // IO Space + Bus Master
    if(pci_command_reg0 != orig_cmd) {
        icsos_pci_write(pci_rtl8139_device, PCI_COMMAND, pci_command_reg0);
    }

    DWORD ret = icsos_pci_read(pci_rtl8139_device, PCI_BAR0);
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Raw PCI BAR0 value: 0x%x", ret);
    rtl8139_device.bar_type = ret & 0x1;

    if((ret & ~0x3) == 0) {
        // Attempt to program an IO base now that IO space is enabled
        DWORD desired = 0xC100;
        klog_warn(KLOG_SUBSYS_NETWORK, "RTL8139: BAR0 unassigned, writing desired IO base 0x%x", desired);
        icsos_pci_write(pci_rtl8139_device, PCI_BAR0, desired | 0x1);
        ret = icsos_pci_read(pci_rtl8139_device, PCI_BAR0);
        klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: BAR0 after write=0x%x", ret);
    }

    if (ret & 0x1) { // IO space
        rtl8139_device.io_base = ret & (~0x3);
    } else {
        rtl8139_device.mem_base = ret & (~0xF);
    }

    if((ret & ~0x3) == 0) {
        // Could not program BAR via config space; fall back to a commonly used legacy window.
        rtl8139_device.bar_type = 1; // force IO
        rtl8139_device.io_base = 0xC000; // fallback base used previously that yielded a valid MAC
        klog_warn(KLOG_SUBSYS_NETWORK, "RTL8139: BAR0 still zero; using fallback I/O base 0x%04x", rtl8139_device.io_base);
    }

    klog_info(KLOG_SUBSYS_NETWORK, "RTL8139: Using %s access (base: 0x%x)",
              (rtl8139_device.bar_type == 0)? "memory":"port",
              (rtl8139_device.bar_type != 0)?rtl8139_device.io_base:rtl8139_device.mem_base);

    printf("rtl8139 use %s access (base: %x)\n", (rtl8139_device.bar_type == 0)? "mem based":"port based", (rtl8139_device.bar_type != 0)?rtl8139_device.io_base:rtl8139_device.mem_base);

    // Set current TSAD
    rtl8139_device.tx_cur = 0;
    
    // Re-read PCI command for logging (already enabled earlier)
    DWORD pci_command_reg = icsos_pci_read(pci_rtl8139_device, PCI_COMMAND);
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: PCI command register now 0x%x", pci_command_reg);

    // Send 0x00 to the CONFIG_1 register (0x52) to set the LWAKE + LWPTN to active high. this should essentially *power on* the device.
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Powering on device");
    outportb(rtl8139_device.io_base + 0x52, 0x0);

    // Soft reset
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Performing soft reset");
    outportb(rtl8139_device.io_base + 0x37, 0x10);

    // Wait for reset to complete
    int timeout = 1000;
    while((inportb(rtl8139_device.io_base + 0x37) & 0x10) != 0 && timeout > 0) {
        timeout--;
    }
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Reset complete (timeout=%d)", timeout);

    // Allocate receive buffer statically for now
    static char rx_space[8192 + 16 + 1500] __attribute__((aligned(4)));
    rtl8139_device.rx_buffer = rx_space;
    memset(rtl8139_device.rx_buffer, 0x0, 8192 + 16 + 1500);
    
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Setting up RX buffer at 0x%x", (DWORD)rtl8139_device.rx_buffer);
    outportl(rtl8139_device.io_base + 0x30, (DWORD)rtl8139_device.rx_buffer);
    current_packet_ptr = 0;

    // Sets the TOK and ROK bits high
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Enabling TX/RX interrupts");
    outports(rtl8139_device.io_base + 0x3C, 0x0005); // enable TOK + ROK

    // (1 << 7) is the WRAP bit, 0xf is AB+AM+APM+AAP
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Configuring RX settings");
    outportl(rtl8139_device.io_base + 0x44, 0xf | (1 << 7)); // accept broadcast etc.

    // Sets the RE and TE bits high
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Enabling receiver and transmitter");
    outportb(rtl8139_device.io_base + 0x37, 0x0C); // RE|TE

    // Register and enable network interrupts
    DWORD irq_num = icsos_pci_read(pci_rtl8139_device, PCI_INTERRUPT_LINE);
    if(irq_num == 0 || irq_num == 0xFF) {
        klog_warn(KLOG_SUBSYS_NETWORK, "RTL8139: Invalid IRQ line (%d) reported; defaulting to 11", irq_num);
        irq_num = 11; // common for rtl8139 in QEMU
    }
    irq_addhandler(0, irq_num, rtl8139_handler);
    klog_info(KLOG_SUBSYS_NETWORK, "RTL8139: Registered IRQ %d interrupt handler", irq_num);
    printf("Registered irq interrupt for rtl8139, irq num = %d\n", irq_num);

    // Dump PIC masks (master and slave) to verify IRQ11 (bit 3 of slave -> overall bit 11) is unmasked
    uint8_t pic_master_mask = inportb(0x21);
    uint8_t pic_slave_mask  = inportb(0xA1);
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: PIC masks master=0x%02x slave=0x%02x", pic_master_mask, pic_slave_mask);
    if(pic_slave_mask & (1 << 3)) {
        klog_warn(KLOG_SUBSYS_NETWORK, "RTL8139: IRQ11 masked -> enabling poll fallback");
        rtl8139_poll_mode = 1;
    } else {
        rtl8139_poll_mode = 0;
    }

    read_mac_addr();
    // Validate MAC (avoid all 0x00/0xFF patterns which suggest bad I/O decode)
    int mac_all_zero = 1, mac_all_ff = 1;
    for(int i=0;i<6;i++){ if(rtl8139_device.mac_addr[i]!=0x00) mac_all_zero=0; if(rtl8139_device.mac_addr[i]!=0xFF) mac_all_ff=0; }
    if(mac_all_zero || mac_all_ff) {
        klog_warn(KLOG_SUBSYS_NETWORK, "RTL8139: Suspicious MAC (decode issue). Continuing with fallback; RX/TX may fail (all_zero=%d all_ff=%d)", mac_all_zero, mac_all_ff);
    }
    rtl8139_present = 1;
    klog_info(KLOG_SUBSYS_NETWORK, "RTL8139: Initialization complete");
}
