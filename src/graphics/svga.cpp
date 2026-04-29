#include <graphics/svga.hpp>
#include <cpu/acpi/pci.hpp>
#include <memory/pmm.hpp>
#include <memory/vmm.hpp>

static inline void outl(uint16_t port, uint32_t value)
{
    asm volatile (
        "outl %0, %1"
        :
        : "a"(value), "Nd"(port)
    );
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    asm volatile (
        "inl %1, %0"
        : "=a"(ret)
        : "Nd"(port)
    );
    return ret;
}

SVGADriver& SVGADriver::get() {
    static SVGADriver instance;
    return instance;
}

SVGADriver::SVGADriver() 
    : initialized(false), deviceFound(false), bus(0), device(0), function(0),
      ioBase(0), framebufferPhys(0), fifoPhys(0), framebuffer(nullptr), fifo(nullptr),
      maxWidth(0), maxHeight(0), vramSize(0), fifoSize(0), capabilities(0),
      currentWidth(0), currentHeight(0), currentBPP(0), fbSize(0) {
}

bool SVGADriver::detectDevice() {
    PCI& pci = PCI::get();
    
    // Scan all PCI buses, devices, and functions
    for (uint16_t b = 0; b < 256; b++) {
        for (uint8_t d = 0; d < 32; d++) {
            for (uint8_t f = 0; f < 8; f++) {
                uint16_t vendor = pci.readConfig16(0, b, d, f, PCI_VENDOR_ID);
                if (vendor == 0xFFFF) continue;
                
                uint16_t devId = pci.readConfig16(0, b, d, f, PCI_DEVICE_ID);
                
                if (vendor == SVGA_VENDOR_ID && devId == SVGA_DEVICE_ID) {
                    bus = b;
                    device = d;
                    function = f;
                    deviceFound = true;
                    return true;
                }
            }
        }
    }
    
    return false;
}

uint32_t SVGADriver::readRegister(uint32_t index) {
    // SVGA_INDEX_PORT is at ioBase + 0 * sizeof(uint32_t)
    // SVGA_VALUE_PORT is at ioBase + 1 * sizeof(uint32_t)
    outl(ioBase, index);
    return inl(ioBase + 4);
}

void SVGADriver::writeRegister(uint32_t index, uint32_t value) {
    outl(ioBase, index);
    outl(ioBase + 4, value);
}

bool SVGADriver::mapMemory() {
    PCI& pci = PCI::get();
    
    // Read BAR0 (could be I/O ports OR memory-mapped registers)
    uint32_t bar0 = pci.readConfig32(0, bus, device, function, PCI_BAR0);
    
    // Check if BAR0 is I/O space (bit 0 set) or memory space (bit 0 clear)
    if (bar0 & 0x1) {
        // I/O space
        ioBase = bar0 & 0xFFFC;
    } else {
        // Memory-mapped I/O
        uint64_t mmioBase = bar0 & 0xFFFFFFF0;
        return false;
    }
    
    // Read BAR1 (Framebuffer)
    uint32_t bar1 = pci.readConfig32(0, bus, device, function, PCI_BAR1);
    
    if (bar1 & 0x1) {
        return false;
    }
    framebufferPhys = bar1 & 0xFFFFFFF0;
    
    // Map framebuffer using HHDM
    framebuffer = reinterpret_cast<void*>(framebufferPhys);
    
    return true;
}

bool SVGADriver::initializeFIFO() {
    // Skip FIFO initialization for now since we don't have memory mapped
    return true;
}

bool SVGADriver::initialize() {
    if (initialized)
        return true;

    PCI& pci = PCI::get();

    // Step 1: detect device FIRST
    if (!detectDevice()) {
        return false;
    }

    // Step 2: enable PCI IO space on CORRECT device
    uint16_t command = pci.readConfig16(0, bus, device, function, PCI_COMMAND);

    command |= PCI_COMMAND_IO;
    command |= PCI_COMMAND_MEMORY;
    command |= PCI_COMMAND_MASTER;

    pci.writeConfig16(0, bus, device, function, PCI_COMMAND, command);

    // verify
    command = pci.readConfig16(0, bus, device, function, PCI_COMMAND);

    // Step 3: map BARs
    if (!mapMemory())
        return false;
    {
        writeRegister(SVGA_REG_ID, SVGA_ID_2);

        uint32_t id = readRegister(SVGA_REG_ID);

    }
    // Step 4: write ID FIRST, then read back
    writeRegister(SVGA_REG_ID, SVGA_ID_2);

    uint32_t id = readRegister(SVGA_REG_ID);

    if (id != SVGA_ID_2) {
        return false;
    }

    initialized = true;
    return true;
}

bool SVGADriver::setMode(uint32_t width, uint32_t height, uint32_t bpp) {
    if (!deviceFound) {
        return false;
    }
    
    // Validate parameters
    if (width < 640 || width > maxWidth) {
        return false;
    }
    if (height < 480 || height > maxHeight) {
        return false;
    }
    if (bpp != 32) {  // Only support 32bpp for simplicity
        return false;
    }
    
    // Disable device
    writeRegister(SVGA_REG_ENABLE, 0);
    
    // Set mode parameters
    writeRegister(SVGA_REG_WIDTH, width);
    writeRegister(SVGA_REG_HEIGHT, height);
    writeRegister(SVGA_REG_BITS_PER_PIXEL, bpp);
    
    // Enable device
    writeRegister(SVGA_REG_ENABLE, 1);
    
    // Read back actual values
    currentWidth = readRegister(SVGA_REG_WIDTH);
    currentHeight = readRegister(SVGA_REG_HEIGHT);
    currentBPP = readRegister(SVGA_REG_BITS_PER_PIXEL);
    uint32_t bytesPerLine = readRegister(SVGA_REG_BYTES_PER_LINE);
    fbSize = readRegister(SVGA_REG_FB_SIZE);
    
    // Verify mode was set
    if (currentWidth != width || currentHeight != height || currentBPP != bpp) {
        return false;
    }
    
    return true;
}

void SVGADriver::getMode(uint32_t* width, uint32_t* height, uint32_t* bpp) {
    if (width) *width = currentWidth;
    if (height) *height = currentHeight;
    if (bpp) *bpp = currentBPP;
}

void* SVGADriver::reserveFIFO(uint32_t bytes) {
    // Align bytes to 4-byte boundary
    bytes = (bytes + 3) & ~3;
    
    // Wait for sufficient space
    while (true) {
        uint32_t min = fifo[SVGA_FIFO_MIN];
        uint32_t max = fifo[SVGA_FIFO_MAX];
        uint32_t nextCmd = fifo[SVGA_FIFO_NEXT_CMD];
        uint32_t stop = fifo[SVGA_FIFO_STOP];
        
        // Calculate available space
        uint32_t available;
        if (nextCmd >= stop) {
            available = max - nextCmd + stop - min;
        } else {
            available = stop - nextCmd;
        }
        
        if (available >= bytes) {
            break;
        }
        
        // Wait for space (simple busy wait for now)
        asm volatile("pause");
    }
    
    // Return pointer to reserved space
    uint32_t nextCmd = fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t max = fifo[SVGA_FIFO_MAX];
    
    if (nextCmd + bytes > max) {
        // Wrap around to beginning
        fifo[SVGA_FIFO_NEXT_CMD] = fifo[SVGA_FIFO_MIN];
        nextCmd = fifo[SVGA_FIFO_MIN];
    }
    
    return (void*)&fifo[nextCmd / 4];
}

void SVGADriver::commitFIFO(uint32_t bytes) {
    bytes = (bytes + 3) & ~3;
    
    uint32_t nextCmd = fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t max = fifo[SVGA_FIFO_MAX];
    uint32_t min = fifo[SVGA_FIFO_MIN];
    
    nextCmd += bytes;
    if (nextCmd >= max) {
        nextCmd = min;
    }
    
    // Memory barrier to ensure writes are visible
    __sync_synchronize();
    
    fifo[SVGA_FIFO_NEXT_CMD] = nextCmd;
}

bool SVGADriver::submitRectFill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!initialized) return false;
    if (x + w > currentWidth) return false;
    if (y + h > currentHeight) return false;
    if (w == 0 || h == 0) return false;
    
    // Check capability
    if (!(capabilities & SVGA_CAP_RECT_FILL)) {
        return false;
    }
    
    // Reserve FIFO space (6 uint32_t values)
    uint32_t* cmd = (uint32_t*)reserveFIFO(6 * sizeof(uint32_t));
    if (!cmd) return false;
    
    // Write command to FIFO
    cmd[0] = SVGA_CMD_RECT_FILL;
    cmd[1] = color;
    cmd[2] = x;
    cmd[3] = y;
    cmd[4] = w;
    cmd[5] = h;
    
    // Commit command
    commitFIFO(6 * sizeof(uint32_t));
    
    return true;
}

bool SVGADriver::submitUpdate(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!initialized) return false;
    if (x + w > currentWidth) return false;
    if (y + h > currentHeight) return false;
    if (w == 0 || h == 0) return false;
    
    // Reserve FIFO space (5 uint32_t values)
    uint32_t* cmd = (uint32_t*)reserveFIFO(5 * sizeof(uint32_t));
    if (!cmd) return false;
    
    // Write UPDATE command to FIFO
    cmd[0] = SVGA_CMD_UPDATE;
    cmd[1] = x;
    cmd[2] = y;
    cmd[3] = w;
    cmd[4] = h;
    
    // Commit command
    commitFIFO(5 * sizeof(uint32_t));
    
    return true;
}

void SVGADriver::syncFIFO() {
    if (!initialized) return;
    
    // Write to SYNC register
    writeRegister(SVGA_REG_SYNC, 1);
    
    // Wait for BUSY register to clear
    while (readRegister(SVGA_REG_BUSY) != 0) {
        asm volatile("pause");
    }
}
