#include <stdint.h>
#include <fs/ahci/ahci.hpp>
#include <memory/heap.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>
#include <graphics/console.hpp>
#include <stddef.h>

const DeviceInfo supportedDevices[] = {
	{ Vendors::ATI, 0x4380, "ATI SB600" },
	{ Vendors::ATI, 0x4390, "ATI SB700/800" },
	{ Vendors::ATI, 0x4391, "ATI IXP700" },
	{ Vendors::ATI, 0x4392, "ATI SB700/800" },
	{ Vendors::ATI, 0x4393, "ATI SB700/800" },
	{ Vendors::ATI, 0x4394, "ATI SB700/800" },
	{ Vendors::ATI, 0x4395, "ATI SB700/800" },
	{ Vendors::SiS, 0x1184, "SiS 966" },
	{ Vendors::SiS, 0x1185, "SiS 966" },
	{ Vendors::SiS, 0x0186, "SiS 968" },
	{ Vendors::Acer, 0x5288, "Acer Labs M5288" },
	{ Vendors::NVIDIA, 0x044c, "NVIDIA MCP65" },
	{ Vendors::NVIDIA, 0x044d, "NVIDIA MCP65" },
	{ Vendors::NVIDIA, 0x044e, "NVIDIA MCP65" },
	{ Vendors::NVIDIA, 0x044f, "NVIDIA MCP65" },
	{ Vendors::NVIDIA, 0x045c, "NVIDIA MCP65" },
	{ Vendors::NVIDIA, 0x045d, "NVIDIA MCP65" },
	{ Vendors::NVIDIA, 0x045e, "NVIDIA MCP65" },
	{ Vendors::NVIDIA, 0x045f, "NVIDIA MCP65" },
	{ Vendors::NVIDIA, 0x0550, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x0551, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x0552, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x0553, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x0554, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x0555, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x0556, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x0557, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x0558, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x0559, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x055a, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x055b, "NVIDIA MCP67" },
	{ Vendors::NVIDIA, 0x07f0, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x07f1, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x07f2, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x07f3, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x07f4, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x07f5, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x07f6, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x07f7, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x07f8, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x07f9, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x07fa, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x07fb, "NVIDIA MCP73" },
	{ Vendors::NVIDIA, 0x0ad0, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0ad1, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0ad2, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0ad3, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0ad4, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0ad5, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0ad6, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0ad7, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0ad8, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0ad9, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0ada, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0adb, "NVIDIA MCP77" },
	{ Vendors::NVIDIA, 0x0ab4, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0ab5, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0ab6, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0ab7, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0ab8, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0ab9, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0aba, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0abb, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0abc, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0abd, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0abe, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0abf, "NVIDIA MCP79" },
	{ Vendors::NVIDIA, 0x0d84, "NVIDIA MCP89" },
	{ Vendors::NVIDIA, 0x0d85, "NVIDIA MCP89" },
	{ Vendors::NVIDIA, 0x0d86, "NVIDIA MCP89" },
	{ Vendors::NVIDIA, 0x0d87, "NVIDIA MCP89" },
	{ Vendors::NVIDIA, 0x0d88, "NVIDIA MCP89" },
	{ Vendors::NVIDIA, 0x0d89, "NVIDIA MCP89" },
	{ Vendors::NVIDIA, 0x0d8a, "NVIDIA MCP89" },
	{ Vendors::NVIDIA, 0x0d8b, "NVIDIA MCP89" },
	{ Vendors::NVIDIA, 0x0d8c, "NVIDIA MCP89" },
	{ Vendors::NVIDIA, 0x0d8d, "NVIDIA MCP89" },
	{ Vendors::NVIDIA, 0x0d8e, "NVIDIA MCP89" },
	{ Vendors::NVIDIA, 0x0d8f, "NVIDIA MCP89" },
	{ Vendors::VIA, 0x3349, "VIA VT8251" },
	{ Vendors::VIA, 0x6287, "VIA VT8251" },
	{ Vendors::Marvell, 0x6121, "Marvell 6121" },
	{ Vendors::Marvell, 0x6145, "Marvell 6145" },
	{ Vendors::JMicron, 0x2360, "JMicron JMB360" },
	{ Vendors::JMicron, 0x2361, "JMicron JMB361" },
	{ Vendors::JMicron, 0x2362, "JMicron JMB362" },
	{ Vendors::JMicron, 0x2363, "JMicron JMB363" },
	{ Vendors::JMicron, 0x2366, "JMicron JMB366" },
	{ Vendors::Intel, 0x2652, "Intel ICH6R" },
	{ Vendors::Intel, 0x2653, "Intel ICH6-M" },
	{ Vendors::Intel, 0x2681, "Intel 63xxESB" },
	{ Vendors::Intel, 0x2682, "Intel ESB2" },
	{ Vendors::Intel, 0x2683, "Intel ESB2" },
	{ Vendors::Intel, 0x27c1, "Intel ICH7R (AHCI mode)" },
	{ Vendors::Intel, 0x27c3, "Intel ICH7R (RAID mode)" },
	{ Vendors::Intel, 0x27c5, "Intel ICH7-M (AHCI mode)" },
	{ Vendors::Intel, 0x27c6, "Intel ICH7-M DH (RAID mode)" },
	{ Vendors::Intel, 0x2821, "Intel ICH8 (AHCI mode)" },
	{ Vendors::Intel, 0x2822, "Intel ICH8R / ICH9 (RAID mode)" },
	{ Vendors::Intel, 0x2824, "Intel ICH8 (AHCI mode)" },
	{ Vendors::Intel, 0x2829, "Intel ICH8M (AHCI mode)" },
	{ Vendors::Intel, 0x282a, "Intel ICH8M (RAID mode)" },
	{ Vendors::Intel, 0x2922, "Intel ICH9 (AHCI mode)" },
	{ Vendors::Intel, 0x2923, "Intel ICH9 (AHCI mode)" },
	{ Vendors::Intel, 0x2924, "Intel ICH9" },
	{ Vendors::Intel, 0x2925, "Intel ICH9" },
	{ Vendors::Intel, 0x2927, "Intel ICH9" },
	{ Vendors::Intel, 0x2929, "Intel ICH9M" },
	{ Vendors::Intel, 0x292a, "Intel ICH9M" },
	{ Vendors::Intel, 0x292b, "Intel ICH9M" },
	{ Vendors::Intel, 0x292c, "Intel ICH9M" },
	{ Vendors::Intel, 0x292f, "Intel ICH9M" },
	{ Vendors::Intel, 0x294d, "Intel ICH9" },
	{ Vendors::Intel, 0x294e, "Intel ICH9M" },
	{ Vendors::Intel, 0x3a05, "Intel ICH10" },
	{ Vendors::Intel, 0x3a22, "Intel ICH10" },
	{ Vendors::Intel, 0x3a25, "Intel ICH10" },
	{}
};

bool get_device_info(Vendors vendorID, uint16_t deviceID, const char **name, uint32_t *flags) {
	const DeviceInfo *info;
	for (info = supportedDevices; (int)info->vendor; info++) {
		if (info->vendor == vendorID && info->device == deviceID) {
			if (name)
				*name = info->name;
			if (flags)
				*flags = info->flags;
			return true;
		}
	}
	return false;
}


AHCIPort::AHCIPort(HBAPort* port, int portNum) : port(port), portNum(portNum), active(false), sectorCount(0) {
}

int AHCIPort::getType() {
    uint32_t ssts = port->ssts;

    uint8_t ipm = (ssts >> 8) & 0x0F;
    uint8_t det = ssts & 0x0F;

    if (det != HBA_PORT_DET_PRESENT) return AHCI_DEV_NULL;
    if (ipm != HBA_PORT_IPM_ACTIVE) return AHCI_DEV_NULL;

    switch (port->sig) {
        case 0xEB140101:
            return AHCI_DEV_SATAPI;
        case 0xC33C0101:
            return AHCI_DEV_SEMB;
        case 0x96690101:
            return AHCI_DEV_PM;
        default:
            return AHCI_DEV_SATA;
    }
}

void AHCIPort::stopCmd() {
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;

    while (true) {
        if (port->cmd & HBA_PxCMD_FR) continue;
        if (port->cmd & HBA_PxCMD_CR) continue;
        break;
    }
}

void AHCIPort::startCmd() {
    while (port->cmd & HBA_PxCMD_CR);

    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

int AHCIPort::findCmdSlot() {
    uint32_t slots = (port->sact | port->ci);
    for (int i = 0; i < 32; i++) {
        if ((slots & 1) == 0) {
            return i;
        }
        slots >>= 1;
    }
    return -1;
}


bool AHCIPort::initialize() {
    Console::get().log("[AHCI Port {}] Stopping command engine...", portNum);
    stopCmd();

    Console::get().log("[AHCI Port {}] Allocating command list buffer...", portNum);
    void* clb = kmalloc_aligned(1024, 1024);
    if (!clb) {
        Console::get().log("[AHCI Port {}] Failed to allocate command list buffer", portNum);
        Console::get().drawText("[AHCI] Port init failed: no command list buffer\n");
        Console::get().drawText("[AHCI] Free frames remaining: ");
        Console::get().drawNumber((int64_t)PMM::FreeFrameCount());
        Console::get().drawText("\n");
        return false;
    }

    Console::get().log("[AHCI Port {}] Allocating FIS buffer...", portNum);
    void* fb = kmalloc_aligned(256, 256);
    if (!fb) {
        Console::get().log("[AHCI Port {}] Failed to allocate FIS buffer", portNum);
        Console::get().drawText("[AHCI] Port init failed: no FIS buffer\n");
        kfree(clb);
        return false;
    }

    for (int i = 0; i < 1024; i++) {
        ((uint8_t*)clb)[i] = 0;
    }
    for (int i = 0; i < 256; i++) {
        ((uint8_t*)fb)[i] = 0;
    }

    port->clb = (uint64_t)clb & 0xFFFFFFFF;
    port->clbu = ((uint64_t)clb >> 32) & 0xFFFFFFFF;
    port->fb = (uint64_t)fb & 0xFFFFFFFF;
    port->fbu = ((uint64_t)fb >> 32) & 0xFFFFFFFF;

    Console::get().log("[AHCI Port {}] Command list at: 0x{}, FIS at: 0x{}", portNum, (uint64_t)clb, (uint64_t)fb);

    Console::get().log("[AHCI Port {}] Allocating command tables...", portNum);
    HBACmdHeader* cmdheader = (HBACmdHeader*)clb;
    for (int i = 0; i < 32; i++) {
        cmdheader[i].prdtl = 8;

        void* ctb = kmalloc_aligned(256, 128);
        if (!ctb) {
            Console::get().log("[AHCI Port {}] Failed to allocate command table {}", portNum, i);
            Console::get().drawText("[AHCI] Port init failed: no command table\n");
            for (int j = 0; j < i; j++) {
                uint64_t ctbAddr = (uint64_t)cmdheader[j].ctba | ((uint64_t)cmdheader[j].ctbau << 32);
                if (ctbAddr) {
                    kfree(reinterpret_cast<void*>(ctbAddr));
                }
            }
            kfree(fb);
            kfree(clb);
            return false;
        }

        for (int j = 0; j < 256; j++) {
            ((uint8_t*)ctb)[j] = 0;
        }

        cmdheader[i].ctba = (uint64_t)ctb & 0xFFFFFFFF;
        cmdheader[i].ctbau = ((uint64_t)ctb >> 32) & 0xFFFFFFFF;
    }

    Console::get().log("[AHCI Port {}] Starting command engine...", portNum);
    startCmd();

    Console::get().log("[AHCI Port {}] Sending IDENTIFY command...", portNum);
    uint64_t identifyFrame = PMM::AllocFrame();
    uint16_t* identifyBuffer = reinterpret_cast<uint16_t*>(identifyFrame);
    if (identifyBuffer) {
        for (int i = 0; i < 256; i++) {
            identifyBuffer[i] = 0;
        }
    }

    if (identifyBuffer && identify(identifyBuffer)) {
        sectorCount = *(uint64_t*)&identifyBuffer[100];
        if (sectorCount == 0) {
            sectorCount = *(uint32_t*)&identifyBuffer[60];
        }
        Console::get().log("[AHCI Port {}] IDENTIFY successful, sector count: {}", portNum, sectorCount);
    } else {
        Console::get().log("[AHCI Port {}] IDENTIFY command failed", portNum);
    }

    if (identifyFrame) {
        PMM::FreeFrame(identifyFrame);
    }

    active = true;
    Console::get().log("[AHCI Port {}] Port is now active", portNum);
    return true;
}

bool AHCIPort::executeCommand(uint8_t cmd, uint64_t lba, uint32_t count, void* buffer, bool isWrite) {
    port->is = (uint32_t)-1;

    int slot = findCmdSlot();
    if (slot == -1) return false;

    uint64_t clb = (uint64_t)port->clb | ((uint64_t)port->clbu << 32);
    HBACmdHeader* cmdheader = (HBACmdHeader*)clb;

    cmdheader += slot;
    cmdheader->cfl = sizeof(FISRegH2D) / sizeof(uint32_t);
    cmdheader->w = isWrite ? 1 : 0;
    cmdheader->prdtl = (uint16_t)((count - 1) / 8) + 1;

    if (cmdheader->prdtl > 8) {
        return false;
    }

    uint64_t ctb = (uint64_t)cmdheader->ctba | ((uint64_t)cmdheader->ctbau << 32);
    HBACmdTbl* cmdtbl = (HBACmdTbl*)ctb;

    for (int i = 0; i < 256; i++) {
        ((uint8_t*)cmdtbl)[i] = 0;
    }

    uint64_t buf = (uint64_t)buffer;
    int i;
    for (i = 0; i < cmdheader->prdtl - 1; i++) {
        cmdtbl->prdt_entry[i].dba = buf & 0xFFFFFFFF;
        cmdtbl->prdt_entry[i].dbau = (buf >> 32) & 0xFFFFFFFF;
        cmdtbl->prdt_entry[i].dbc = 8 * 1024 - 1;
        cmdtbl->prdt_entry[i].i = 1;
        buf += 8 * 1024;
        count -= 16;
    }

    cmdtbl->prdt_entry[i].dba = buf & 0xFFFFFFFF;
    cmdtbl->prdt_entry[i].dbau = (buf >> 32) & 0xFFFFFFFF;
    cmdtbl->prdt_entry[i].dbc = (count * 512) - 1;
    cmdtbl->prdt_entry[i].i = 1;

    FISRegH2D* cmdfis = (FISRegH2D*)(&cmdtbl->cfis);
    cmdfis->fis_type = 0x27;
    cmdfis->c = 1;
    cmdfis->command = cmd;

    cmdfis->lba0 = (uint8_t)lba;
    cmdfis->lba1 = (uint8_t)(lba >> 8);
    cmdfis->lba2 = (uint8_t)(lba >> 16);
    cmdfis->device = 1 << 6;

    cmdfis->lba3 = (uint8_t)(lba >> 24);
    cmdfis->lba4 = (uint8_t)(lba >> 32);
    cmdfis->lba5 = (uint8_t)(lba >> 40);

    cmdfis->countl = count & 0xFF;
    cmdfis->counth = (count >> 8) & 0xFF;

    int spin = 0;
    while ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && spin < 1000000) {
        spin++;
    }
    if (spin == 1000000) return false;

    port->ci = 1 << slot;

    while (true) {
        if ((port->ci & (1 << slot)) == 0) break;
        if (port->is & HBA_PxIS_TFES) return false;
    }

    if (port->is & HBA_PxIS_TFES) return false;

    return true;
}

bool AHCIPort::read(uint64_t sector, uint32_t count, void* buffer) {
    return executeCommand(ATA_CMD_READ_DMA_EX, sector, count, buffer, false);
}

bool AHCIPort::write(uint64_t sector, uint32_t count, const void* buffer) {
    return executeCommand(ATA_CMD_WRITE_DMA_EX, sector, count, (void*)buffer, true);
}

bool AHCIPort::identify(uint16_t* buffer) {
    return executeCommand(ATA_CMD_IDENTIFY, 0, 1, buffer, false);
}


AHCIController::AHCIController(uint64_t abar) : hba(nullptr), portCount(0), abar(abar) {
    for (int i = 0; i < 32; i++) {
        ports[i] = nullptr;
    }
}

AHCIController::~AHCIController() {
    for (int i = 0; i < 32; i++) {
        if (ports[i]) {
            delete ports[i];
        }
    }
}

bool AHCIController::initialize() {
    size_t abar_size = sizeof(HBAMemory);
    size_t pages_needed = (abar_size + 4095) / 4096;
    if (pages_needed < 2) pages_needed = 2;

    uint64_t abar_aligned = abar & ~0xFFF;

    Console::get().log("[AHCI] Initializing controller at ABAR: 0x{}", abar);
    Console::get().log("[AHCI] Aligned ABAR: 0x{}, Pages needed: {}", abar_aligned, pages_needed);

    for (size_t i = 0; i < pages_needed; i++) {
        void* addr = (void*)(abar_aligned + i * 4096);
        VMM::MapPage(reinterpret_cast<uint64_t>(addr), reinterpret_cast<uint64_t>(addr), PageFlags::Present | PageFlags::ReadWrite | PageFlags::CacheDisab | PageFlags::NoExecute);
    }

    uint64_t offset_in_page = abar - abar_aligned;
    hba = (HBAMemory*)(abar_aligned + offset_in_page);

    Console::get().log("[AHCI] HBA Memory mapped at: 0x{}", (uint64_t)hba);
    Console::get().log("[AHCI] HBA Version: 0x{}", hba->vs);
    Console::get().log("[AHCI] HBA Capabilities: 0x{}", hba->cap);
    Console::get().log("[AHCI] Number of ports: {}", ((hba->cap >> 0) & 0x1F) + 1);

    uint32_t pi = hba->pi;
    Console::get().log("[AHCI] Port Implemented register: 0x{}", pi);

    for (int i = 0; i < 32; i++) {
        if (pi & 1) {
            Console::get().log("[AHCI] Checking port {}...", i);
            AHCIPort* port = new AHCIPort(&hba->ports[i], i);
            int type = port->getType();

            Console::get().drawText("[AHCI] Port ");
            Console::get().drawNumber(i);
            Console::get().drawText(" type: ");
            switch(type) {
                case AHCI_DEV_SATA:
                    Console::get().drawText("SATA\n");
                    break;
                case AHCI_DEV_SATAPI:
                    Console::get().drawText("SATAPI\n");
                    break;
                case AHCI_DEV_SEMB:
                    Console::get().drawText("SEMB\n");
                    break;
                case AHCI_DEV_PM:
                    Console::get().drawText("PM\n");
                    break;
                default:
                    Console::get().drawText("NULL/Unknown\n");
                    break;
            }

            if (type == AHCI_DEV_SATA) {
                Console::get().log("[AHCI] Initializing SATA port {}...", i);
                if (port->initialize()) {
                    Console::get().log("[AHCI] Port {} initialized successfully", i);
                    Console::get().log("[AHCI] Port {} sector count: {}", i, port->getSectorCount());
                    Console::get().log("[AHCI] Port {} capacity: {} MB", i, (port->getSectorCount() * 512) / (1024 * 1024));
                    Console::get().drawText("[AHCI] Port ");
                    Console::get().drawNumber(i);
                    Console::get().drawText(" initialized\n");
                    ports[portCount++] = port;
                } else {
                    Console::get().log("[AHCI] Port {} initialization failed", i);
                    Console::get().drawText("[AHCI] Port ");
                    Console::get().drawNumber(i);
                    Console::get().drawText(" failed to initialize\n");
                    delete port;
                }
            } else {
                delete port;
            }
        }
        pi >>= 1;
    }

    Console::get().log("[AHCI] Total active ports: {}", portCount);
    Console::get().drawText("[AHCI] Active ports: ");
    Console::get().drawNumber(portCount);
    Console::get().drawText("\n");
    return portCount > 0;
}

AHCIPort* AHCIController::getPort(int index) {
    if (index < 0 || index >= portCount) return nullptr;
    return ports[index];
}

SATABlockDevice::SATABlockDevice(AHCIPort* port) : port(port), totalSize(0) {
    if (port) {
        totalSize = port->getSectorCount() * 512;
    }
}

SATABlockDevice::~SATABlockDevice() {
}

bool SATABlockDevice::read(uint64_t offset, void* buffer, uint64_t size) {
    if (!port || !buffer) return false;

    uint64_t sector = offset / 512;
    uint64_t sectorOffset = offset % 512;
    uint32_t sectorCount = (size + sectorOffset + 511) / 512;

    size_t dmaSize = sectorCount * 512;
    void* dmaBuf = kmalloc_aligned(dmaSize, 4096);
    if (!dmaBuf) return false;

    bool result = port->read(sector, sectorCount, dmaBuf);

    if (result) {
        uint8_t* src = reinterpret_cast<uint8_t*>(dmaBuf) + sectorOffset;
        uint8_t* dst = reinterpret_cast<uint8_t*>(buffer);
        for (uint64_t i = 0; i < size; i++) {
            dst[i] = src[i];
        }
    }

    kfree(dmaBuf);
    return result;
}

bool SATABlockDevice::write(uint64_t offset, const void* buffer, uint64_t size) {
    if (!port || !buffer) return false;

    uint64_t sector = offset / 512;
    uint64_t sectorOffset = offset % 512;
    uint32_t sectorCount = (size + sectorOffset + 511) / 512;

    size_t dmaSize = sectorCount * 512;
    void* dmaBuf = kmalloc_aligned(dmaSize, 4096);
    if (!dmaBuf) return false;

    if (sectorOffset != 0 || (size % 512) != 0) {
        if (!port->read(sector, sectorCount, dmaBuf)) {
            kfree(dmaBuf);
            return false;
        }
    }

    uint8_t* dst = reinterpret_cast<uint8_t*>(dmaBuf) + sectorOffset;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(buffer);
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = src[i];
    }

    bool result = port->write(sector, sectorCount, dmaBuf);
    kfree(dmaBuf);
    return result;
}

uint64_t SATABlockDevice::getSize() {
    return totalSize;
}
