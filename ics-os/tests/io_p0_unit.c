#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* blkcache.h's public API uses the legacy DWORD spelling. */
typedef unsigned int DWORD;
#include "kernel/iomgr/blkcache.h"
#include "kernel/devmgr/devmgr_lifecycle.h"
#include "kernel/hardware/usb/usb_identity.h"
#include "kernel/hardware/dma.h"
#include "kernel/hardware/iommu.h"
#include "kernel/hardware/vtd.h"
#include "kernel/hardware/irq_lifecycle.h"
#include "kernel/process/completion.h"

static int reschedule_count;
static int dma_release_count;
static unsigned char dma_test_storage[128] __attribute__((aligned(64)));

static void *host_dma_alloc(unsigned int size)
{
   return malloc(size);
}

static void *host_dma_test_alloc(unsigned int size)
{
   return size <= sizeof(dma_test_storage) ? dma_test_storage : 0;
}

static void host_dma_test_free(void *allocation)
{
   if (allocation == dma_test_storage)
      dma_release_count++;
}

static void host_dma_free(void *allocation)
{
   dma_release_count++;
   free(allocation);
}

static void host_acpi_set_checksum(unsigned char *table, unsigned int length)
{
   unsigned int index;
   unsigned char sum = 0;
   table[9] = 0;
   for (index = 0; index < length; index++)
      sum = (unsigned char)(sum + table[index]);
   table[9] = (unsigned char)(0 - sum);
}

void smp_reschedule_others(void)
{
   reschedule_count++;
}

void wait_queue_wake_all(wait_queue_t *queue)
{
   (void)queue;
}

void wait_event_wake_all(void *key)
{
   (void)key;
}

static int check(const char *name, int condition)
{
   if (!condition) {
      printf("not ok - %s\n", name);
      return 0;
   }
   printf("ok - %s\n", name);
   return 1;
}

int main(void)
{
   int ok = 1;
   int state = DEVMGR_STATE_LIVE;
   DWORD refs = 0;
   completion_t completion;
   unsigned char identity_data[1024];
   usb_volume_identity volume;
   usb_media_identity expected;
   usb_media_identity replacement;
   unsigned long long dma_addr;
   dma_region dma_buffer;
   dma_mapping streaming_dma;
   dma_device bounce_device = { ~0ULL, 64, 1, host_dma_alloc,
                                host_dma_free };
   dma_sg_mapping scatter_dma = { { { 0 } }, 0, 0, 0 };
   dma_segment dma_segments[2];
   iommu_domain iommu;
   unsigned long long iova;
   unsigned char dmar[72];
   vtd_dmar_info dmar_info;
   vtd_legacy_entry vtd_root;
   vtd_legacy_entry vtd_context[VTD_CONTEXT_ENTRY_COUNT];
   unsigned int vtd_agaw;
   unsigned int vtd_iotlb_offset;
   irq_vector_state irq_state = { 0 };
   irq_vector_state irq_pool[4] = { { 0 } };
   irq_vector_state irq_reserved_pool[4] = { { 0 } };
   int irq_owner_a;
   int irq_owner_b;
   int irq_owner_c;
   u8 irq_vector;
   irq_domain valid_irq_domain = { "test", 0x42, 0x43 };
   irq_domain invalid_irq_domain = { "invalid", 0x41, 0x43 };
   irq_msi_message irq_message;

   printf("TAP version 13\n1..107\n");
   ok &= check("matching writeback generation clears dirty",
               blkcache_writeback_is_current(1, 7, 7));
   ok &= check("redirty during writeback remains dirty",
               !blkcache_writeback_is_current(1, 8, 7));
   ok &= check("reused cache slot remains dirty",
               !blkcache_writeback_is_current(0, 7, 7));
   ok &= check("same device generation retains cache identity",
               blkcache_device_key_is_current(4, 4));
   ok &= check("reused device slot rejects stale cache identity",
               !blkcache_device_key_is_current(4, 5));
   ok &= check("live device reference is acquired",
               devmgr_lifecycle_get(DEVMGR_STATE_LIVE, &refs) && refs == 1);
   ok &= check("live device quiesces idempotently",
               devmgr_lifecycle_quiesce(&state) &&
               state == DEVMGR_STATE_QUIESCING &&
               devmgr_lifecycle_quiesce(&state));
   ok &= check("quiescing device rejects new references",
               !devmgr_lifecycle_get(DEVMGR_STATE_QUIESCING, &refs) && refs == 1);
   ok &= check("quiescing device waits for active reference",
               !devmgr_lifecycle_can_retire(DEVMGR_STATE_QUIESCING, refs));
   ok &= check("active reference is released",
               devmgr_lifecycle_put(&refs) && refs == 0);
   ok &= check("quiescing device retires after drain",
               devmgr_lifecycle_can_retire(DEVMGR_STATE_QUIESCING, refs) &&
               !devmgr_lifecycle_put(&refs));
   completion_init(&completion);
   ok &= check("new completion is not signaled",
               !completion_done(&completion));
   complete_all(&completion);
   ok &= check("completion-before-wait is retained",
               completion_done(&completion) && reschedule_count == 1);
   ok &= check("identity DMA accepts an aligned in-mask range",
               dma_identity_map((void *)0x2000UL, 4096, 4096,
                                0xFFFFFFFFULL, &dma_addr) &&
               dma_addr == 0x2000ULL);
   ok &= check("identity DMA rejects misalignment",
               !dma_identity_map((void *)0x2001UL, 4096, 4096,
                                 0xFFFFFFFFULL, &dma_addr));
   ok &= check("identity DMA rejects an invalid alignment",
               !dma_identity_map((void *)0x2000UL, 4096, 3,
                                 0xFFFFFFFFULL, &dma_addr));
   ok &= check("identity DMA rejects a range beyond the device mask",
               !dma_identity_map((void *)0xFFFFF000UL, 8192, 4096,
                                 0xFFFFFFFFULL, &dma_addr));
   ok &= check("identity DMA rejects address-range overflow",
               !dma_identity_map((void *)~0UL, 2, 1,
                                 ~0ULL, &dma_addr));
   ok &= check("DMA region records an owned mapping",
               dma_region_init(&dma_buffer, (void *)0x4000UL, 4096, 4096,
                               0xFFFFFFFFULL) &&
               dma_buffer.dma_addr == 0x4000ULL &&
               dma_buffer.length == 4096);
   ok &= check("DMA region translates a contained subrange",
               dma_region_map(&dma_buffer, (void *)0x4F00UL, 256,
                              &dma_addr) && dma_addr == 0x4F00ULL);
   ok &= check("DMA region rejects a crossing subrange",
               !dma_region_map(&dma_buffer, (void *)0x4F00UL, 257,
                               &dma_addr));
   ok &= check("coherent DMA allocation satisfies alignment",
               dma_alloc_coherent(&dma_buffer, 4096, 4096, ~0ULL,
                                  host_dma_alloc, free) &&
               ((unsigned long)dma_buffer.cpu_addr & 4095) == 0);
   ok &= check("coherent DMA allocation is zeroed",
               ((unsigned char *)dma_buffer.cpu_addr)[0] == 0 &&
               ((unsigned char *)dma_buffer.cpu_addr)[4095] == 0);
   ok &= check("coherent DMA maps its final byte",
               dma_region_map(&dma_buffer,
                              (unsigned char *)dma_buffer.cpu_addr + 4095,
                              1, &dma_addr));
   dma_free_coherent(&dma_buffer, free);
   ok &= check("coherent DMA release clears ownership",
               !dma_buffer.cpu_addr && !dma_buffer.allocation_base &&
               !dma_buffer.length);
   dma_release_count = 0;
   ok &= check("coherent DMA mapping failure unwinds allocation",
               !dma_alloc_coherent(&dma_buffer, 64, 64, 0xFF,
                                   host_dma_test_alloc,
                                   host_dma_test_free) &&
               dma_release_count == 1 && !dma_buffer.allocation_base);
   memset(&streaming_dma, 0, sizeof(streaming_dma));
   ok &= check("streaming DMA maps a device-read buffer",
               dma_map_single(&streaming_dma, identity_data, 512,
                              DMA_TO_DEVICE, ~0ULL) &&
               streaming_dma.active);
   ok &= check("streaming DMA records direction and complete length",
               streaming_dma.direction == DMA_TO_DEVICE &&
               streaming_dma.length == 512);
   ok &= check("streaming DMA rejects a second active map",
               !dma_map_single(&streaming_dma, identity_data, 512,
                               DMA_FROM_DEVICE, ~0ULL));
   ok &= check("streaming DMA unmap returns CPU ownership",
               dma_unmap_single(&streaming_dma) && !streaming_dma.active &&
               !streaming_dma.cpu_addr);
   ok &= check("streaming DMA rejects double unmap",
               !dma_unmap_single(&streaming_dma));
   ok &= check("streaming DMA rejects an invalid direction",
               !dma_map_single(&streaming_dma, identity_data, 512, 0,
                               ~0ULL));
   identity_data[0] = 0x5A;
   dma_release_count = 0;
   ok &= check("bounce DMA gives the device a distinct address",
               dma_map_single_device(&streaming_dma, identity_data, 512,
                                     DMA_TO_DEVICE, &bounce_device) &&
               streaming_dma.device_cpu_addr != identity_data &&
               streaming_dma.dma_addr !=
                  (unsigned long long)(unsigned long)identity_data);
   ok &= check("bounce DMA copies device-read data before submission",
               ((unsigned char *)streaming_dma.device_cpu_addr)[0] == 0x5A);
   ok &= check("bounce DMA releases its mapping on unmap",
               dma_unmap_single(&streaming_dma) && dma_release_count == 1);
   identity_data[0] = 0;
   ok &= check("bounce DMA maps a device-write buffer",
               dma_map_single_device(&streaming_dma, identity_data, 512,
                                     DMA_FROM_DEVICE, &bounce_device));
   ((unsigned char *)streaming_dma.device_cpu_addr)[0] = 0xA5;
   ok &= check("bounce DMA copies device-written data on unmap",
               dma_unmap_single(&streaming_dma) && identity_data[0] == 0xA5);
   bounce_device.dma_mask = 0xFF;
   ok &= check("bounce DMA unwinds a device-mask rejection",
               !dma_map_single_device(&streaming_dma, identity_data, 512,
                                      DMA_TO_DEVICE, &bounce_device) &&
               dma_release_count == 3 && !streaming_dma.active);
   bounce_device.dma_mask = ~0ULL;
   ok &= check("bounce DMA rejects an empty caller buffer before allocation",
               !dma_map_single_device(&streaming_dma, 0, 512,
                                      DMA_TO_DEVICE, &bounce_device) &&
               dma_release_count == 3);
      dma_segments[0].cpu_addr = identity_data;
      dma_segments[0].length = 256;
      dma_segments[1].cpu_addr = identity_data + 512;
      dma_segments[1].length = 512;
      ok &= check("scatter DMA maps multiple segments atomically",
               dma_map_sg(&scatter_dma, dma_segments, 2, DMA_FROM_DEVICE,
                       ~0ULL) && scatter_dma.active &&
               scatter_dma.count == 2);
      ok &= check("scatter DMA preserves segment addresses and direction",
               scatter_dma.mappings[0].dma_addr ==
                  (unsigned long long)(unsigned long)identity_data &&
               scatter_dma.mappings[1].dma_addr ==
                  (unsigned long long)(unsigned long)(identity_data + 512) &&
               scatter_dma.direction == DMA_FROM_DEVICE);
      ok &= check("scatter DMA unmaps every segment",
               dma_unmap_sg(&scatter_dma) && !scatter_dma.active &&
               !scatter_dma.mappings[0].active &&
               !scatter_dma.mappings[1].active);
      ok &= check("scatter DMA rejects an invalid segment count",
               !dma_map_sg(&scatter_dma, dma_segments, 0, DMA_TO_DEVICE,
                        ~0ULL));
      dma_segments[1].length = 0;
      ok &= check("scatter DMA unwinds a partial mapping failure",
               !dma_map_sg(&scatter_dma, dma_segments, 2, DMA_TO_DEVICE,
                        ~0ULL) && !scatter_dma.active &&
               !scatter_dma.count && !scatter_dma.mappings[0].active);
      dma_segments[1].length = 512;
      ok &= check("scatter DMA supports device-scoped bounce mappings",
               dma_map_sg_device(&scatter_dma, dma_segments, 2,
                                 DMA_TO_DEVICE, &bounce_device) &&
               scatter_dma.mappings[0].device_cpu_addr !=
                  dma_segments[0].cpu_addr &&
               scatter_dma.mappings[1].device_cpu_addr !=
                  dma_segments[1].cpu_addr &&
               dma_unmap_sg(&scatter_dma) && dma_release_count == 5);
   ok &= check("translated IOMMU domain validates its IOVA aperture",
               iommu_domain_init(&iommu, IOMMU_DOMAIN_TRANSLATED,
                                 0x100000ULL, 0x103FFFULL));
   ok &= check("IOMMU domain attaches one PCI requester",
               iommu_domain_attach(&iommu, 0x0123));
   ok &= check("translated IOMMU allocates a distinct least-privilege IOVA",
               iommu_domain_map(&iommu, 0x0123, 0x400000ULL, 0x2000ULL,
                                IOMMU_READ, &iova) &&
               iova == 0x100000ULL && iova != 0x400000ULL &&
               iommu.mappings[0].permissions == IOMMU_READ);
   ok &= check("IOMMU rejects wrong-requester unmap",
               !iommu_domain_unmap(&iommu, 0x0124, iova, 0x2000ULL));
   ok &= check("IOMMU requires an exact unmap range",
               !iommu_domain_unmap(&iommu, 0x0123, iova, 0x1000ULL));
   ok &= check("IOMMU refuses detach with an active mapping",
               !iommu_domain_detach(&iommu, 0x0123));
   ok &= check("IOMMU unmaps and detaches the owning requester",
               iommu_domain_unmap(&iommu, 0x0123, iova, 0x2000ULL) &&
               iommu_domain_detach(&iommu, 0x0123));
   ok &= check("translated IOMMU reuses a freed aperture range",
               iommu_domain_init(&iommu, IOMMU_DOMAIN_TRANSLATED,
                                 0x100000ULL, 0x103FFFULL) &&
               iommu_domain_attach(&iommu, 0x0123) &&
               iommu_domain_map(&iommu, 0x0123, 0x600000ULL, 0x1000ULL,
                                IOMMU_WRITE, &iova) && iova == 0x100000ULL);
   ok &= check("IOMMU validates page-aligned mappings",
               !iommu_domain_map(&iommu, 0x0123, 0x500001ULL, 0x1000ULL,
                                 IOMMU_READ, &iova));
   ok &= check("blocked IOMMU domain denies DMA mappings",
               iommu_domain_init(&iommu, IOMMU_DOMAIN_BLOCKED, 0, 0) &&
               iommu_domain_attach(&iommu, 0x0123) &&
               !iommu_domain_map(&iommu, 0x0123, 0x400000ULL, 0x1000ULL,
                                 IOMMU_WRITE, &iova));
   ok &= check("identity IOMMU domain preserves physical addresses",
               iommu_domain_init(&iommu, IOMMU_DOMAIN_IDENTITY, 0, 0) &&
               iommu_domain_attach(&iommu, 0x0123) &&
               iommu_domain_map(&iommu, 0x0123, 0x400000ULL, 0x1000ULL,
                                IOMMU_READ | IOMMU_WRITE, &iova) &&
               iova == 0x400000ULL);
   ok &= check("identity IOMMU rejects an overlapping mapping",
               !iommu_domain_map(&iommu, 0x0123, 0x400000ULL, 0x1000ULL,
                                 IOMMU_READ, &iova));
   ok &= check("IOMMU fault latches the domain closed",
               iommu_domain_fault(&iommu, 0x0123) && iommu.faulted &&
               iommu.fault_count == 1);
   ok &= check("faulted IOMMU domain rejects new mappings",
               !iommu_domain_map(&iommu, 0x0123, 0x500000ULL, 0x1000ULL,
                                 IOMMU_READ, &iova));
   memset(dmar, 0, sizeof(dmar));
   memcpy(dmar, "DMAR", 4);
   dmar[4] = sizeof(dmar);
   dmar[36] = 38;
   dmar[48] = 0;
   dmar[50] = 24;
   dmar[52] = 1;
   dmar[56] = 0;
   dmar[57] = 0;
   dmar[58] = 0xD9;
   dmar[59] = 0xFE;
   dmar[64] = 1;
   dmar[65] = 8;
   dmar[70] = 0x14;
   host_acpi_set_checksum(dmar, sizeof(dmar));
   ok &= check("VT-d parses a bounded DMAR DRHD and endpoint scope",
               vtd_parse_dmar(dmar, sizeof(dmar), &dmar_info) &&
               dmar_info.drhd_count == 1 && dmar_info.scope_count == 1 &&
               dmar_info.drhds[0].register_base == 0xFED90000ULL &&
               dmar_info.scopes[0].last_device == 0x14);
   dmar[9]++;
   ok &= check("VT-d rejects a bad DMAR checksum",
               !vtd_parse_dmar(dmar, sizeof(dmar), &dmar_info));
   dmar[9]--;
   ok &= check("VT-d rejects a truncated DMAR table",
               !vtd_parse_dmar(dmar, sizeof(dmar) - 1, &dmar_info));
   dmar[65] = 7;
   host_acpi_set_checksum(dmar, sizeof(dmar));
   ok &= check("VT-d rejects a malformed device scope",
               !vtd_parse_dmar(dmar, sizeof(dmar), &dmar_info));
   dmar[65] = 8;
   dmar[56] = 1;
   host_acpi_set_checksum(dmar, sizeof(dmar));
   ok &= check("VT-d rejects an unaligned register base",
               !vtd_parse_dmar(dmar, sizeof(dmar), &dmar_info));
   ok &= check("VT-d selects a supported adjusted address width",
               vtd_legacy_select_agaw(1ULL << 10, &vtd_agaw) &&
               vtd_agaw == 2);
   ok &= check("VT-d rejects capabilities without an address width",
               !vtd_legacy_select_agaw(0, &vtd_agaw));
   ok &= check("VT-d encodes an aligned legacy root entry",
               vtd_legacy_root_entry(&vtd_root, 0x12345000ULL) &&
               vtd_root.low == 0x12345001ULL && vtd_root.high == 0);
   ok &= check("VT-d rejects an unaligned context-table address",
               !vtd_legacy_root_entry(&vtd_root, 0x12345001ULL));
   ok &= check("VT-d populates every requester with pass-through",
               vtd_legacy_context_table(vtd_context, 1ULL << 10,
                                        VTD_ECAP_PASS_THROUGH, 7) &&
               vtd_context[0].low == 9 && vtd_context[0].high == 0x702 &&
               vtd_context[255].low == 9 &&
               vtd_context[255].high == 0x702);
   ok &= check("VT-d rejects pass-through when unsupported",
               !vtd_legacy_context_table(vtd_context, 1ULL << 10, 0, 7));
   ok &= check("VT-d encodes one scoped pass-through requester",
               vtd_legacy_context_entry(&vtd_context[0x18], 1ULL << 10,
                                        VTD_ECAP_PASS_THROUGH, 9) &&
               vtd_context[0x18].low == 9 &&
               vtd_context[0x18].high == 0x902);
   ok &= check("VT-d decodes the register IOTLB offset",
               vtd_iotlb_register_offset(0xF00ULL, &vtd_iotlb_offset) &&
               vtd_iotlb_offset == 0xF0);
   ok &= check("VT-d rejects a missing register IOTLB offset",
               !vtd_iotlb_register_offset(0, &vtd_iotlb_offset));
   ok &= check("IRQ vector accepts its first owner",
               irq_vector_state_claim(&irq_state, &irq_owner_a));
   ok &= check("IRQ vector rejects a duplicate owner",
               !irq_vector_state_claim(&irq_state, &irq_owner_b));
   ok &= check("IRQ handler enters for the current owner",
               irq_vector_state_enter(&irq_state, &irq_owner_a) &&
               irq_state.active == 1);
   ok &= check("IRQ release begins for the current owner",
               irq_vector_state_begin_release(&irq_state, &irq_owner_a));
   ok &= check("IRQ release blocks new handler entry",
               !irq_vector_state_enter(&irq_state, &irq_owner_a));
   ok &= check("IRQ vector cannot release with an active handler",
               !irq_vector_state_finish_release(&irq_state, &irq_owner_a));
   ok &= check("IRQ handler exit rejects a wrong owner",
               !irq_vector_state_exit(&irq_state, &irq_owner_b));
   ok &= check("IRQ handler exit drains the current owner",
               irq_vector_state_exit(&irq_state, &irq_owner_a) &&
               irq_state.active == 0);
   ok &= check("IRQ vector releases after handler drain",
               irq_vector_state_finish_release(&irq_state, &irq_owner_a));
   ok &= check("released IRQ vector accepts a new owner",
               irq_vector_state_claim(&irq_state, &irq_owner_b));
   ok &= check("IRQ allocator selects the first available vector",
               irq_vector_state_allocate(irq_pool, 4, 1, 2,
                                         &irq_owner_a, &irq_vector) &&
               irq_vector == 1);
   ok &= check("IRQ allocator skips a claimed vector",
               irq_vector_state_allocate(irq_pool, 4, 1, 2,
                                         &irq_owner_b, &irq_vector) &&
               irq_vector == 2);
   ok &= check("IRQ allocator reports an exhausted range",
               !irq_vector_state_allocate(irq_pool, 4, 1, 2,
                                          &irq_owner_c, &irq_vector));
   ok &= check("IRQ allocator rejects an invalid range",
               !irq_vector_state_allocate(irq_pool, 4, 3, 2,
                                          &irq_owner_c, &irq_vector));
   ok &= check("IRQ vector accepts a platform reservation",
               irq_vector_state_reserve(&irq_reserved_pool[1],
                                        &irq_owner_a));
   ok &= check("IRQ allocator skips a reserved vector",
               irq_vector_state_allocate(irq_reserved_pool, 4, 1, 2,
                                         &irq_owner_b, &irq_vector) &&
               irq_vector == 2);
   ok &= check("reserved IRQ vector rejects handler entry",
               !irq_vector_state_enter(&irq_reserved_pool[1], &irq_owner_a));
   ok &= check("reserved IRQ vector rejects normal release",
               !irq_vector_state_begin_release(&irq_reserved_pool[1],
                                               &irq_owner_a));
   ok &= check("IRQ reservation rejects a wrong-owner release",
               !irq_vector_state_unreserve(&irq_reserved_pool[1],
                                           &irq_owner_b));
   ok &= check("released IRQ reservation is reusable",
               irq_vector_state_unreserve(&irq_reserved_pool[1],
                                          &irq_owner_a) &&
               irq_vector_state_allocate(irq_reserved_pool, 4, 1, 1,
                                         &irq_owner_c, &irq_vector) &&
               irq_vector == 1);
   ok &= check("IRQ device subdomain accepts protected vector bounds",
               irq_domain_valid(&valid_irq_domain));
   ok &= check("IRQ domain rejects a kernel-reserved vector",
               !irq_domain_valid(&invalid_irq_domain));
   ok &= check("IRQ domain composes an xAPIC MSI message",
               irq_domain_compose_xapic_msi(&valid_irq_domain, 0x42, 0x2A,
                                            &irq_message) &&
               irq_message.address_lo == 0xFEE2A000u &&
               irq_message.address_hi == 0 && irq_message.data == 0x42);
   ok &= check("xAPIC MSI accepts the largest destination ID",
               irq_domain_compose_xapic_msi(&valid_irq_domain, 0x43, 0xFF,
                                            &irq_message) &&
               irq_message.address_lo == 0xFEEFF000u);
   ok &= check("xAPIC MSI rejects a vector outside its domain",
               !irq_domain_compose_xapic_msi(&valid_irq_domain, 0x44, 0,
                                             &irq_message));
   ok &= check("xAPIC MSI rejects an unencodable destination ID",
               !irq_domain_compose_xapic_msi(&valid_irq_domain, 0x42, 0x100,
                                             &irq_message));

   memset(identity_data, 0, sizeof(identity_data));
   identity_data[38] = 0x29;
   identity_data[39] = 0x12;
   identity_data[510] = 0x55;
   identity_data[511] = 0xAA;
   usb_volume_identity_init(&volume, 10, 100);
   ok &= check("FAT12/16 volume serial is parsed",
               usb_volume_identity_from_boot(identity_data, &volume) &&
               volume.type == USB_VOLUME_FAT16 && volume.value[0] == 0x12);

   memset(identity_data, 0, sizeof(identity_data));
   identity_data[66] = 0x29;
   identity_data[67] = 0x34;
   identity_data[510] = 0x55;
   identity_data[511] = 0xAA;
   usb_volume_identity_init(&volume, 10, 100);
   ok &= check("FAT32 volume serial is parsed",
               usb_volume_identity_from_boot(identity_data, &volume) &&
               volume.type == USB_VOLUME_FAT32 && volume.value[0] == 0x34);

   memset(identity_data, 0, sizeof(identity_data));
   memcpy(identity_data + 3, "EXFAT   ", 8);
   identity_data[100] = 0x56;
   usb_volume_identity_init(&volume, 10, 100);
   ok &= check("exFAT volume serial is parsed",
               usb_volume_identity_from_boot(identity_data, &volume) &&
               volume.type == USB_VOLUME_EXFAT && volume.value[0] == 0x56);

   memset(identity_data, 0, sizeof(identity_data));
   identity_data[0x38] = 0x53;
   identity_data[0x39] = 0xEF;
   identity_data[0x68] = 0x78;
   usb_volume_identity_init(&volume, 10, 100);
   ok &= check("ext4 UUID is parsed",
               usb_volume_identity_from_ext4(identity_data, &volume) &&
               volume.type == USB_VOLUME_EXT4 && volume.value[0] == 0x78);

   memset(identity_data, 0, sizeof(identity_data));
   memcpy(identity_data + 1, "CD001", 5);
   identity_data[40] = 0x9A;
   usb_volume_identity_init(&volume, 10, 100);
   ok &= check("ISO9660 volume identifier is parsed",
               usb_volume_identity_from_iso9660(identity_data, &volume) &&
               volume.type == USB_VOLUME_ISO9660 && volume.value[0] == 0x9A);

   memset(&expected, 0, sizeof(expected));
   expected.valid = 1;
   expected.count = 1;
   expected.volumes[0] = volume;
   replacement = expected;
   ok &= check("matching media identity is accepted",
               usb_media_identity_equal(&expected, &replacement));
   replacement.volumes[0].value[0] ^= 1;
   ok &= check("changed volume identity is rejected",
               !usb_media_identity_equal(&expected, &replacement));
   memset(identity_data, 0, sizeof(identity_data));
   usb_volume_identity_init(&volume, 10, 100);
   ok &= check("unknown volume signature is rejected",
               !usb_volume_identity_from_boot(identity_data, &volume) &&
               !usb_volume_identity_from_ext4(identity_data, &volume) &&
               !usb_volume_identity_from_iso9660(identity_data, &volume));
   return ok ? 0 : 1;
}
