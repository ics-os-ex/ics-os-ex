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

DWORD current_packet_ptr;

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
    // First, copy the data to a physically contiguous chunk of memory
    //void * transfer_data = kmalloc_a(len);
   //void * phys_addr = virtual2phys(kpage_dir, transfer_data);
    //memcpy(transfer_data, data, len);

    // Second, fill in physical address of data, and length
//    outportl(rtl8139_device.io_base + TSAD_array[rtl8139_device.tx_cur], (DWORD)phys_addr);
    outportl(rtl8139_device.io_base + TSD_array[rtl8139_device.tx_cur++], len);
    if(rtl8139_device.tx_cur > 3)
        rtl8139_device.tx_cur = 0;
}

/*
 * Initialize the rtl8139 card driver
 * */
void rtl8139_init() {
    // First get the network device using PCI
    klog_info(KLOG_SUBSYS_NETWORK, "RTL8139: Initializing RTL8139 network card...");
    
    pci_rtl8139_device = icsos_pci_get_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID, -1);

    DWORD ret = icsos_pci_read(pci_rtl8139_device, PCI_BAR0);
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Raw PCI BAR0 value: 0x%x", ret);

    rtl8139_device.bar_type = ret & 0x1;

    // Get io base or mem base by extracting the high bits
    if (rtl8139_device.bar_type) {
        // I/O space - mask the lowest 2 bits
        rtl8139_device.io_base = ret & (~0x3);
        if (rtl8139_device.io_base == 0) {
            // If no base address assigned, try a default
            klog_warn(KLOG_SUBSYS_NETWORK, "RTL8139: No I/O base assigned, using default 0xC000");
            rtl8139_device.io_base = 0xC000;
        }
    } else {
        // Memory space - mask the lowest 4 bits  
        rtl8139_device.mem_base = ret & (~0xf);
    }

    klog_info(KLOG_SUBSYS_NETWORK, "RTL8139: Using %s access (base: 0x%x)", 
              (rtl8139_device.bar_type == 0)? "memory":"port", 
              (rtl8139_device.bar_type != 0)?rtl8139_device.io_base:rtl8139_device.mem_base);

    printf("rtl8139 use %s access (base: %x)\n", (rtl8139_device.bar_type == 0)? "mem based":"port based", (rtl8139_device.bar_type != 0)?rtl8139_device.io_base:rtl8139_device.mem_base);

    // Set current TSAD
    rtl8139_device.tx_cur = 0;
    
    // Enable PCI Bus Mastering and I/O Space
    DWORD pci_command_reg = icsos_pci_read(pci_rtl8139_device, PCI_COMMAND);
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Original PCI command: 0x%x", pci_command_reg);
    
    // Enable I/O Space (bit 0) and Bus Mastering (bit 2)
    pci_command_reg |= 0x05;  // bits 0 and 2
    icsos_pci_write(pci_rtl8139_device, PCI_COMMAND, pci_command_reg);
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Updated PCI command: 0x%x", pci_command_reg);

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
    outports(rtl8139_device.io_base + 0x3C, 0x0005);

    // (1 << 7) is the WRAP bit, 0xf is AB+AM+APM+AAP
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Configuring RX settings");
    outportl(rtl8139_device.io_base + 0x44, 0xf | (1 << 7));

    // Sets the RE and TE bits high
    klog_debug(KLOG_SUBSYS_NETWORK, "RTL8139: Enabling receiver and transmitter");
    outportb(rtl8139_device.io_base + 0x37, 0x0C);

    // Register and enable network interrupts
    DWORD irq_num = icsos_pci_read(pci_rtl8139_device, PCI_INTERRUPT_LINE);
    irq_addhandler(0, irq_num, rtl8139_handler);
    klog_info(KLOG_SUBSYS_NETWORK, "RTL8139: Registered IRQ %d interrupt handler", irq_num);
    printf("Registered irq interrupt for rtl8139, irq num = %d\n", irq_num);

    read_mac_addr();
    klog_info(KLOG_SUBSYS_NETWORK, "RTL8139: Initialization complete");
}
