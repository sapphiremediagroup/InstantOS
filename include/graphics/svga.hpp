#pragma once

#include <stdint.h>

// VMWare SVGA II Device IDs
constexpr uint16_t SVGA_VENDOR_ID = 0x15AD;
constexpr uint16_t SVGA_DEVICE_ID = 0x0405;

// SVGA II Register indices
enum SVGARegister : uint32_t {
    SVGA_REG_ID = 0,
    SVGA_REG_ENABLE = 1,
    SVGA_REG_WIDTH = 2,
    SVGA_REG_HEIGHT = 3,
    SVGA_REG_MAX_WIDTH = 4,
    SVGA_REG_MAX_HEIGHT = 5,
    SVGA_REG_DEPTH = 6,
    SVGA_REG_BITS_PER_PIXEL = 7,
    SVGA_REG_RED_MASK = 9,
    SVGA_REG_GREEN_MASK = 10,
    SVGA_REG_BLUE_MASK = 11,
    SVGA_REG_BYTES_PER_LINE = 12,
    SVGA_REG_FB_START = 13,
    SVGA_REG_FB_OFFSET = 14,
    SVGA_REG_VRAM_SIZE = 15,
    SVGA_REG_FB_SIZE = 16,
    SVGA_REG_CAPABILITIES = 17,
    SVGA_REG_MEM_START = 18,
    SVGA_REG_MEM_SIZE = 19,
    SVGA_REG_CONFIG_DONE = 20,
    SVGA_REG_SYNC = 21,
    SVGA_REG_BUSY = 22,
};

// SVGA FIFO offsets
enum SVGAFIFOOffset : uint32_t {
    SVGA_FIFO_MIN = 0,
    SVGA_FIFO_MAX = 1,
    SVGA_FIFO_NEXT_CMD = 2,
    SVGA_FIFO_STOP = 3,
};

// SVGA Commands
enum SVGACommand : uint32_t {
    SVGA_CMD_UPDATE = 1,
    SVGA_CMD_RECT_FILL = 2,
    SVGA_CMD_RECT_COPY = 3,
};

// Capability flags
enum SVGACapability : uint32_t {
    SVGA_CAP_RECT_FILL = 0x00000001,
    SVGA_CAP_RECT_COPY = 0x00000002,
};

// SVGA ID
constexpr uint32_t SVGA_ID_0 = 0;
constexpr uint32_t SVGA_ID_1 = 1;
constexpr uint32_t SVGA_ID_2 = 2;

// PCI Configuration Space offsets
constexpr uint16_t PCI_VENDOR_ID = 0x00;
constexpr uint16_t PCI_DEVICE_ID = 0x02;
constexpr uint16_t PCI_COMMAND = 0x04;
constexpr uint16_t PCI_BAR0 = 0x10;
constexpr uint16_t PCI_BAR1 = 0x14;
constexpr uint16_t PCI_BAR2 = 0x18;

// PCI Command register bits
constexpr uint16_t PCI_COMMAND_IO = 0x0001;
constexpr uint16_t PCI_COMMAND_MEMORY = 0x0002;
constexpr uint16_t PCI_COMMAND_MASTER = 0x0004;

class SVGADriver {
public:
    static SVGADriver& get();
    
    bool initialize();
    bool isInitialized() const { return initialized; }
    bool isAvailable() const { return deviceFound; }
    
    // Device capabilities
    uint32_t getMaxWidth() const { return maxWidth; }
    uint32_t getMaxHeight() const { return maxHeight; }
    uint32_t getVRAMSize() const { return vramSize; }
    
    // Display mode management
    bool setMode(uint32_t width, uint32_t height, uint32_t bpp);
    void getMode(uint32_t* width, uint32_t* height, uint32_t* bpp);
    
    // Framebuffer access
    void* getFramebuffer() { return framebuffer; }
    uint32_t getFBSize() const { return fbSize; }
    
    // Command submission
    bool submitRectFill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
    bool submitUpdate(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    
    // Synchronization
    void syncFIFO();
    
private:
    SVGADriver();
    
    bool detectDevice();
    bool mapMemory();
    bool initializeFIFO();
    
    uint32_t readRegister(uint32_t index);
    void writeRegister(uint32_t index, uint32_t value);
    
    void* reserveFIFO(uint32_t bytes);
    void commitFIFO(uint32_t bytes);
    
    bool initialized;
    bool deviceFound;
    
    // PCI location
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    
    // Device resources
    uint16_t ioBase;
    uint64_t framebufferPhys;
    uint64_t fifoPhys;
    
    void* framebuffer;
    volatile uint32_t* fifo;
    
    // Device capabilities
    uint32_t maxWidth;
    uint32_t maxHeight;
    uint32_t vramSize;
    uint32_t fifoSize;
    uint32_t capabilities;
    
    // Current mode
    uint32_t currentWidth;
    uint32_t currentHeight;
    uint32_t currentBPP;
    uint32_t fbSize;
};