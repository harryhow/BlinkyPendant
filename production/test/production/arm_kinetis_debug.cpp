/*
 * Simple ARM debug interface for Arduino, using the SWD (Serial Wire Debug) port.
 * Extensions for Freescale Kinetis chips.
 * 
 * Copyright (c) 2013 Micah Elizabeth Scott
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <Arduino.h>
#include "arm_kinetis_debug.h"
#include "arm_kinetis_reg.h"


//TODO: Not a global?
bool rewriteFlashCommand;


ARMKinetisDebug::ARMKinetisDebug(unsigned clockPin, unsigned dataPin, LogLevel logLevel)
    : ARMDebug(clockPin, dataPin, logLevel)
{}

bool ARMKinetisDebug::startup()
{
    return reset() && debugHalt() && detect() && peripheralInit();
}

bool ARMKinetisDebug::detect()
{
    // Make sure we're on a compatible chip. The MDM-AP peripheral is Freescale-specific.
    uint32_t idr;
    if (!apRead(REG_MDM_IDR, idr))
        return false;
    if (idr != 0x001C0000) {
        log(LOG_ERROR, "ARMKinetisDebug: Didn't find a supported MDM-AP peripheral");
        return false;
    }

    return true;
}

bool ARMKinetisDebug::reset()
{
    // System resets can be slow, give them more time than the default.
    const unsigned resetRetries = 2000;

    // Put the control register in a known state, and make sure we aren't already in the middle of a reset
    uint32_t status;
    if (!apWrite(REG_MDM_CONTROL, REG_MDM_CONTROL_CORE_HOLD_RESET))
        return false;
        
    if (!apReadPoll(REG_MDM_STATUS, status, REG_MDM_STATUS_SYS_NRESET, -1, resetRetries))
        return false;

    // System reset
    if (!apWrite(REG_MDM_CONTROL, REG_MDM_CONTROL_SYS_RESET_REQ))
        return false;
    if (!apReadPoll(REG_MDM_STATUS, status, REG_MDM_STATUS_SYS_NRESET, 0))
        return false;
    if (!apWrite(REG_MDM_CONTROL, 0))
        return false;

    // Wait until the flash controller is ready & system is out of reset.
    // Also wait for security bit to be cleared. Early in reset, the chip is determining
    // its security status. When the security bit is set, AHB-AP is disabled.
    if (!apReadPoll(REG_MDM_STATUS, status,
            REG_MDM_STATUS_SYS_NRESET | REG_MDM_STATUS_FLASH_READY | REG_MDM_STATUS_SYS_SECURITY,
            REG_MDM_STATUS_SYS_NRESET | REG_MDM_STATUS_FLASH_READY,
            resetRetries))
        return false;

    return true;
}

bool ARMKinetisDebug::debugHalt()
{
    /*
     * Enable debug, request a halt, and read back status.
     *
     * This part is somewhat timing critical, since we're racing against the watchdog
     * timer. Avoid memWait() by calling the lower-level interface directly.
     *
     * Since this is expected to fail a bunch before succeeding, mute errors temporarily.
     */

    unsigned haltRetries = 10000;
    LogLevel savedLogLevel;
    uint32_t dhcsr;

    // Point at the debug halt control/status register. We disable MEM-AP autoincrement,
    // and leave TAR pointed at DHCSR for the entire loop.
    if (memWriteCSW(CSW_32BIT) && apWrite(MEM_TAR, REG_SCB_DHCSR)) {

        setLogLevel(LOG_NONE, savedLogLevel);

        while (haltRetries) {
            haltRetries--;

            if (!apWrite(MEM_DRW, 0xA05F0003))
                continue;
            if (!apRead(MEM_DRW, dhcsr))
                continue;

            if (dhcsr & (1 << 17)) {
                // Halted!
                break;
            }
        }

        setLogLevel(savedLogLevel);
    }

    if (!haltRetries) {
        log(LOG_ERROR, "ARMKinetisDebug: Failed to put CPU in debug halt state. (DHCSR: %08x)", dhcsr);
        return false;
    }

    return true;
}

bool ARMKinetisDebug::peripheralInit()
{
  /*
     * ARM peripheral initialization, based on the peripheral startup code
     * used in Teensyduino. We set up the same peripherals that FC-Boot sets up.
     */

    uint8_t value;
    return
        // Enable peripheral clocks
        memStore(REG_SIM_SCGC5, 0x00043F82) && // clocks active to all GPIO
        memStore(REG_SIM_SCGC6,
            REG_SIM_SCGC6_RTC | REG_SIM_SCGC6_FTM0 | REG_SIM_SCGC6_FTM1 |
            REG_SIM_SCGC6_ADC0 | REG_SIM_SCGC6_FTFL) &&

        // Start in FEI mode
        // Enable capacitors for crystal
        memStoreByte(REG_OSC0_CR, REG_OSC_SC8P | REG_OSC_SC2P) &&

        // Enable osc, 8-32 MHz range, low power mode
        memStoreByte(REG_MCG_C2, REG_MCG_C2_RANGE0(2) | REG_MCG_C2_EREFS) &&

        // Switch to crystal as clock source, FLL input = 16 MHz / 512
        memStoreByte(REG_MCG_C1, REG_MCG_C1_CLKS(2) | REG_MCG_C1_FRDIV(4)) &&

        // Wait for crystal oscillator to begin
        memPollByte(REG_MCG_S, value, REG_MCG_S_OSCINIT0, -1) &&

        // Wait for FLL to use oscillator
        memPollByte(REG_MCG_S, value, REG_MCG_S_IREFST, 0) &&

        // Wait for MCGOUT to use oscillator
        memPollByte(REG_MCG_S, value, REG_MCG_S_CLKST_MASK, REG_MCG_S_CLKST(2)) &&

        // Now we're in FBE mode
        // Config PLL input for 16 MHz Crystal / 4 = 4 MHz
        memStoreByte(REG_MCG_C5, REG_MCG_C5_PRDIV0(3)) &&

        // Config PLL for 96 MHz output
        memStoreByte(REG_MCG_C6, REG_MCG_C6_PLLS | REG_MCG_C6_VDIV0(0)) &&

        // Wait for PLL to start using xtal as its input
        memPollByte(REG_MCG_S, value, REG_MCG_S_PLLST, -1) &&

        // Wait for PLL to lock
        memPollByte(REG_MCG_S, value, REG_MCG_S_LOCK0, -1) &&

        // Now we're in PBE mode
        // Config divisors: 48 MHz core, 48 MHz bus, 24 MHz flash
        memStore(REG_SIM_CLKDIV1, REG_SIM_CLKDIV1_OUTDIV1(1) |
            REG_SIM_CLKDIV1_OUTDIV2(1) | REG_SIM_CLKDIV1_OUTDIV4(3)) &&

        // Switch to PLL as clock source, FLL input = 16 MHz / 512
        memStoreByte(REG_MCG_C1, REG_MCG_C1_CLKS(0) | REG_MCG_C1_FRDIV(4)) &&

        // Wait for PLL clock to be used
        memPollByte(REG_MCG_S, value, REG_MCG_S_CLKST_MASK, REG_MCG_S_CLKST(3)) &&

        // Now we're in PEE mode
        // Configure USB for 48 MHz clock
        // USB = 96 MHz PLL / 2
        memStore(REG_SIM_CLKDIV2, REG_SIM_CLKDIV2_USBDIV(1)) &&

        // USB uses PLL clock, trace is CPU clock, CLKOUT=OSCERCLK0
        memStore(REG_SIM_SOPT2, REG_SIM_SOPT2_USBSRC | REG_SIM_SOPT2_PLLFLLSEL |
            REG_SIM_SOPT2_TRACECLKSEL | REG_SIM_SOPT2_CLKOUTSEL(6)) &&

        // Enable USB clock gate and I2C0
        memStore(REG_SIM_SCGC4, REG_SIM_SCGC4_USBOTG | REG_SIM_SCGC4_I2C0) &&

        // Reset USB core
        memStoreByte(REG_USB0_USBTRC0, REG_USB_USBTRC_USBRESET) &&
        memPollByte(REG_USB0_USBTRC0, value, REG_USB_USBTRC_USBRESET, 0) &&

        // Enable USB
        memStoreByte(REG_USB0_CTL, REG_USB_CTL_USBENSOFEN) &&
        memStoreByte(REG_USB0_USBCTRL, 0) &&

        // USB pull-up off for now
        usbSetPullup(false) &&

        // Test AHB-AP: Can we successfully write to RAM?
        testMemoryAccess();
}

bool ARMKinetisDebug::testMemoryAccess()
{
    // Try word-wide stores to SRAM
    if (!memStoreAndVerify(0x20000000, 0x31415927))
        return false;
    if (!memStoreAndVerify(0x20000000, 0x76543210))
        return false;

    // Test byte-wide memory access
    uint32_t word;
    uint8_t byte;
    if (!memStoreByte(0x20000001, 0x55))
        return false;
    if (!memStoreByte(0x20000002, 0x9F))
        return false;
    if (!memLoad(0x20000000, word))
        return false;
    if (word != 0x769F5510) {
        log(LOG_ERROR, "ARMKinetisDebug: Byte-wide AHB write seems broken! (Test word = %08x)", word);
        return false;
    }
    if (!memLoadByte(0x20000003, byte))
        return false;
    if (byte != 0x76) {
        log(LOG_ERROR, "ARMKinetisDebug: Byte-wide AHB read seems broken! (Test byte = %02x)", byte);
        return false;
    }

    // Test halfword-wide memory access
    uint16_t half;
    if (!memStoreHalf(0x20000000, 0x5abc))
        return false;
    if (!memStoreHalf(0x20000002, 0xdef0))
        return false;
    if (!memLoad(0x20000000, word))
        return false;
    if (word != 0xdef05abc) {
        log(LOG_ERROR, "ARMKinetisDebug: Halfword-wide AHB write seems broken! (Test word = %08x)", word);
        return false;
    }
    if (!memLoadHalf(0x20000002, half))
        return false;
    if (half != 0xdef0) {
        log(LOG_ERROR, "ARMKinetisDebug: Halfword-wide AHB read seems broken! (Test half = %04x)", half);
        return false;
    }

    return true;
}

bool ARMKinetisDebug::flashMassErase()
{
    // Erase all flash, even if some of it is protected.

    uint32_t status;
    if (!apRead(REG_MDM_STATUS, status))
        return false;
    if (!(status & REG_MDM_STATUS_FLASH_READY)) {
        log(LOG_ERROR, "FLASH: Flash controller not ready before mass erase");
        return false;
    }
    if ((status & REG_MDM_STATUS_FLASH_ERASE_ACK)) {
        log(LOG_ERROR, "FLASH: Mass erase already in progress");
        return false;
    }
    if (!(status & REG_MDM_STATUS_MASS_ERASE_ENABLE)) {
        log(LOG_ERROR, "FLASH: Mass erase is disabled!");
        return false;
    }

    log(LOG_NORMAL, "FLASH: Beginning mass erase operation");
    if (!apWrite(REG_MDM_CONTROL, REG_MDM_CONTROL_CORE_HOLD_RESET | REG_MDM_CONTROL_MASS_ERASE))
        return false;

    // Wait for the mass erase to begin (ACK bit set)
    if (!apReadPoll(REG_MDM_STATUS, status, REG_MDM_STATUS_FLASH_ERASE_ACK, -1)) {
        log(LOG_ERROR, "FLASH: Timed out waiting for mass erase to begin");
        return false;
    }

    // Wait for it to complete (CONTROL bit cleared)
    uint32_t control;
    if (!apReadPoll(REG_MDM_CONTROL, control, REG_MDM_CONTROL_MASS_ERASE, 0, 10000)) {
        log(LOG_ERROR, "FLASH: Timed out waiting for mass erase to complete");
        return false;
    }

    // Check status again
    if (!apRead(REG_MDM_STATUS, status))
        return false;
    if (!(status & REG_MDM_STATUS_FLASH_READY)) {
        log(LOG_ERROR, "FLASH: Flash controller not ready after mass erase");
        return false;
    }

    log(LOG_NORMAL, "FLASH: Mass erase complete");
    return true;
}

//bool ARMKinetisDebug::flashSectorBufferInit()
//{
//    // Use FlexRAM as normal RAM, and erase it. Test to make sure it's working.
//    return
//        ftfl_setFlexRAMFunction(0xFF) &&
//        memStoreAndVerify(REG_FLEXRAM_BASE, 0x12345678) &&
//        memStoreAndVerify(REG_FLEXRAM_BASE, 0xFFFFFFFF) &&
//        memStoreAndVerify(REG_FLEXRAM_BASE + FLASH_SECTOR_SIZE - 4, 0xA5559872) &&
//        memStoreAndVerify(REG_FLEXRAM_BASE + FLASH_SECTOR_SIZE - 4, 0xFFFFFFFF);
//}
//
//bool ARMKinetisDebug::flashSectorBufferWrite(uint32_t bufferOffset, const uint32_t *data, unsigned count)
//{
//    if (bufferOffset & 3) {
//        log(LOG_ERROR, "ARMKinetisDebug::flashSectorBufferWrite alignment error");
//        return false;
//    }
//    if (bufferOffset + (count * sizeof *data) > FLASH_SECTOR_SIZE) {
//        log(LOG_ERROR, "ARMKinetisDebug::flashSectorBufferWrite overrun");
//        return false;
//    }
//
//    return memStore(REG_FLEXRAM_BASE + bufferOffset, data, count);
//}

//bool ARMKinetisDebug::flashSectorProgram(uint32_t address)
//{
//    if (address & (FLASH_SECTOR_SIZE-1)) {
//        log(LOG_ERROR, "ARMKinetisDebug::flashSectorProgram alignment error");
//        return false;
//    }
//
//    return ftfl_programSection(address, FLASH_SECTOR_SIZE/4);
//}


bool ARMKinetisDebug::I2C0begin() {
//    SIM_SCGC4 |= SIM_SCGC4_I2C0;    // Enable the I2C0 clock
//    uint32_t SCGC4_VAL;
//    log(LOG_NORMAL, "i2c0begin: enable clock");    
//    if(!memLoad(REG_SIM_SCGC4, SCGC4_VAL))
//        return false;
//    if(!memStore(REG_SIM_SCGC4, SCGC4_VAL | REG_SIM_SCGC4_I2C0))
//        return false;
        
//    log(LOG_NORMAL, "i2c0begin: disable i2c");
//    
////    I2C0_C1 = I2C_C1_IICEN;         // Enable I2C
//    if(!memStoreByte(REG_I2C0_C1, 0))
//        return false;

    log(LOG_I2C, "i2c0begin: set transmission speed");
//    I2C0_F = 0x1B;                  // Set transmission speed (100KHz?)
    if(!memStoreByte(REG_I2C0_F, 0x1B))
        return false;
        
    log(LOG_I2C, "i2c0begin: enable i2c");
    
//    I2C0_C1 = I2C_C1_IICEN;         // Enable I2C
    if(!memStoreByte(REG_I2C0_C1, REG_I2C_C1_IICEN))
        return false;

    log(LOG_I2C, "i2c0begin: set muxes");  

//    // TODO: Set pin muxes!
//    PORTB_PCR0 = PORT_PCR_MUX(2);
//    PORTB_PCR1 = PORT_PCR_MUX(2);
    if(!memStore(REG_PORTB_PCR0, REG_PORT_PCR_MUX(2)))
        return false;
    if(!memStore(REG_PORTB_PCR1, REG_PORT_PCR_MUX(2)))
        return false;

    return true;
}


bool ARMKinetisDebug::I2C0waitForDone() {
    log(LOG_I2C, "I2C0waitForDone");
//  while((I2C0_S & I2C_S_IICIF) == 0) {}
//    I2C0_S |= I2C_S_IICIF;
    uint8_t I2C0_S_VALUE;
    int counts = 0;
    const int timeout = 500;
    do{
      if(!memLoadByte(REG_I2C0_S, I2C0_S_VALUE))
          return false;
      delay(10);
      counts+=10;
      if(counts > timeout)
          return false;
    }
    while ((I2C0_S_VALUE & REG_I2C_S_IICIF) == 0);
    
    return true;
}


bool ARMKinetisDebug::I2C0beginTransmission(uint8_t address) {
    log(LOG_I2C, "I2C0beginTransmission (ADDRESS=%x)", address);
//    I2C0_C1 |= I2C_C1_TX;
//    I2C0_C1 |= I2C_C1_MST;
    uint8_t I2C0_C1_VALUE;
    if(!memLoadByte(REG_I2C0_C1, I2C0_C1_VALUE))
        return false;
    if(!memStoreByte(REG_I2C0_C1, I2C0_C1_VALUE | REG_I2C_C1_TX))
        return false;
    if(!memStoreByte(REG_I2C0_C1, I2C0_C1_VALUE | REG_I2C_C1_TX | I2C_C1_MST))
        return false;

    return I2C0write(address << 1);
}


bool ARMKinetisDebug::I2C0endTransmission(bool stop) {
    log(LOG_I2C, "I2C0endTransmission (STOP=%i)", stop);
  
    if(stop) {
//        I2C0_C1 &= ~(I2C_C1_MST);
//        I2C0_C1 &= ~(I2C_C1_TX);
        uint8_t I2C0_C1_VALUE;
        if(!memLoadByte(REG_I2C0_C1, I2C0_C1_VALUE))
            return false;
        if(!memStoreByte(REG_I2C0_C1, I2C0_C1_VALUE & ~(REG_I2C_C1_MST)))
            return false;
        if(!memStoreByte(REG_I2C0_C1, I2C0_C1_VALUE & ~(REG_I2C_C1_MST | REG_I2C_C1_TX)))
            return false;

//        while(I2C0_S & I2C_S_BUSY) {};
        uint8_t I2C0_S_VALUE;
        int counts = 0;
        const int timeout = 500;  // max time to wait before failing
        do{
          if(!memLoadByte(REG_I2C0_S, I2C0_S_VALUE))
              return false;
          delay(10);
          counts+=10;
          if(counts > timeout)
              return false;
        }
        while (I2C0_S_VALUE & REG_I2C_S_BUSY);

    }
    else {
//        I2C0_C1 |= I2C_C1_RSTA;
        uint8_t I2C0_C1_VALUE;
        if(!memLoadByte(REG_I2C0_C1, I2C0_C1_VALUE))
            return false;
        if(!memStoreByte(REG_I2C0_C1, I2C0_C1_VALUE | I2C_C1_RSTA))
            return false;
    }
    
    return true;
}


bool ARMKinetisDebug::I2C0write(uint8_t data) {
    log(LOG_I2C, "I2C0write (DATA=%x)", data);
//    I2C0_D = data;
    if(!memStoreByte(REG_I2C0_D, data))
        return false;

    return I2C0waitForDone();
}


bool ARMKinetisDebug::I2C0requestFrom(uint8_t address, int length) {
    log(LOG_I2C, "I2C0requestFrom (ADDRESS=%x, LENGTH=%i)", address, length);
    if(!I2C0write(address << 1 | 0x01))
        return false;

//    I2C0_C1 &= ~(I2C_C1_TX);    // Set for RX mode, and write the device address
    uint8_t I2C0_C1_VALUE;
    if(!memLoadByte(REG_I2C0_C1, I2C0_C1_VALUE))
        return false;
    if(!memStoreByte(REG_I2C0_C1, I2C0_C1_VALUE & ~(I2C_C1_TX)))
        return false;

    //TODO: this is a hack?
    I2C0remaining = length + 1;

    uint8_t throwaway;
    return I2C0receive(throwaway);
}


bool ARMKinetisDebug::I2C0receive(uint8_t& data) {
    log(LOG_I2C, "I2C0receive (REMAINING=%i)", I2C0remaining);
  if(I2C0remaining == 0) {
        return false;
    }

    if(I2C0remaining <= 2) {           // On the last byte, don't ACK
//        I2C0_C1 |= I2C_C1_TXAK;
        uint8_t I2C0_C1_VALUE;
        if(!memLoadByte(REG_I2C0_C1, I2C0_C1_VALUE))
            return false;
        if(!memStoreByte(REG_I2C0_C1, I2C0_C1_VALUE | I2C_C1_TXAK))
            return false;
    }

    if(I2C0remaining == 1) {
        I2C0endTransmission();
//        I2C0_C1 &= ~(I2C_C1_TXAK);
        uint8_t I2C0_C1_VALUE;
        if(!memLoadByte(REG_I2C0_C1, I2C0_C1_VALUE))
            return false;
        if(!memStoreByte(REG_I2C0_C1, I2C0_C1_VALUE & ~(REG_I2C_C1_TXAK)))
            return false;
    }

    if(!memLoadByte(REG_I2C0_D, data))
        return false;
    log(LOG_I2C, "I2C0receive (READ=%i)", data);

    if(I2C0remaining >1) {
        if(!(I2C0waitForDone()))
            return false;
    }

    I2C0remaining--;

    return true;
}


bool ARMKinetisDebug::I2C0available() {
    log(LOG_I2C, "I2C0available");
  return I2C0remaining > 0;
}

bool ARMKinetisDebug::ftfl_busyWait()
{
    const unsigned retries = 1000;
    uint32_t fstat;

    if (!memPoll(REG_FTFL_FSTAT, fstat, REG_FTFL_FSTAT_CCIF, -1)) {
        log(LOG_ERROR, "FLASH: Error waiting for flash controller");
        return false;
    }

    return true;
}

bool ARMKinetisDebug::ftfl_launchCommand()
{
    // Begin a flash memory controller command, and clear any previous error status.
    return
        memStoreByte(REG_FTFL_FSTAT, REG_FTFL_FSTAT_ACCERR | REG_FTFL_FSTAT_FPVIOL | REG_FTFL_FSTAT_RDCOLERR) &&
        memStoreByte(REG_FTFL_FSTAT, REG_FTFL_FSTAT_CCIF);
}

//bool ARMKinetisDebug::ftfl_setFlexRAMFunction(uint8_t controlCode)
//{
//    return
//        ftfl_busyWait() &&
//        memStoreByte(REG_FTFL_FCCOB0, 0x81) &&
//        memStoreByte(REG_FTFL_FCCOB1, controlCode) &&
//        ftfl_launchCommand() &&
//        ftfl_busyWait() &&
//        ftfl_handleCommandStatus();
//}

//bool ARMKinetisDebug::ftfl_programSection(uint32_t address, uint32_t numLWords)
//{
//    return
//        ftfl_busyWait() &&
//        memStoreByte(REG_FTFL_FCCOB0, 0x0B) &&
//        memStoreByte(REG_FTFL_FCCOB1, address >> 16) &&
//        memStoreByte(REG_FTFL_FCCOB2, address >> 8) &&
//        memStoreByte(REG_FTFL_FCCOB3, address) &&
//        memStoreByte(REG_FTFL_FCCOB4, numLWords >> 8) &&
//        memStoreByte(REG_FTFL_FCCOB5, numLWords) &&
//        ftfl_launchCommand() &&
//        ftfl_busyWait() &&
//        ftfl_handleCommandStatus("FLASH: Error verifying sector! (FSTAT: %08x)");
//}

bool ARMKinetisDebug::ftfl_programLongword(uint32_t address, const uint32_t& longWord)
{
    // TODO: Clean me!
    // Note: Since some devices won't have flexram, we have to program in 4-byte chunks instead. Sucks a little...
    
    //Only update register bytes that we need to, to save a little time.
    static uint8_t lastAddr16 = 0;
    static uint8_t lastAddr8 = 0;
    static uint8_t lastAddr0 = 0;

    static uint8_t lastData24 = 0;
    static uint8_t lastData16 = 0;
    static uint8_t lastData8 = 0;
    static uint8_t lastData0 = 0;

    if(lastAddr16 != (address >> 16)&0xFF) {
        lastAddr16 = (address >> 16)&0xFF;
        if(!memStoreByte(REG_FTFL_FCCOB1, lastAddr16))
            return false;
    }
    
    if(lastAddr8 != (address >> 8)&0xFF) {
        lastAddr8 = (address >> 8)&0xFF;
        if(!memStoreByte(REG_FTFL_FCCOB2, lastAddr8))
            return false;
    }
    
    if(lastAddr0 != (address >> 0)&0xFF) {
        lastAddr0 = (address >> 0)&0xFF;
        if(!memStoreByte(REG_FTFL_FCCOB3, lastAddr0))
            return false;
    }

    
    if(lastData24 != (longWord >> 24)&0xFF) {
        lastData24 = (longWord >> 24)&0xFF;
        if(!memStoreByte(REG_FTFL_FCCOB4, lastData24))
            return false;
    }

    if(lastData16 != (longWord >> 16)&0xFF) {
        lastData16 = (longWord >> 16)&0xFF;
        if(!memStoreByte(REG_FTFL_FCCOB5, lastData16))
            return false;
    }
    
    if(lastData8 != (longWord >> 8)&0xFF) {
        lastData8 = (longWord >> 8)&0xFF;
        if(!memStoreByte(REG_FTFL_FCCOB6, lastData8))
            return false;
    }
    
    if(lastData0 != (longWord >> 0)&0xFF) {
        lastData0 = (longWord >> 0)&0xFF;
        if(!memStoreByte(REG_FTFL_FCCOB7, lastData0))
            return false;
    }

    // Make sure we write each byte at least once.
    if(rewriteFlashCommand) {
        rewriteFlashCommand = false;
        
        return
            ftfl_busyWait() &&  // We are so slow, don't bother.
            memStoreByte(REG_FTFL_FCCOB0, 0x06) &&
            memStoreByte(REG_FTFL_FCCOB1, address >> 16) &&
            memStoreByte(REG_FTFL_FCCOB2, address >> 8) &&
            memStoreByte(REG_FTFL_FCCOB3, address) &&
            memStoreByte(REG_FTFL_FCCOB4, longWord >> 24) &&
            memStoreByte(REG_FTFL_FCCOB5, longWord >> 16) &&
            memStoreByte(REG_FTFL_FCCOB6, longWord >> 8) &&
            memStoreByte(REG_FTFL_FCCOB7, longWord) &&
            ftfl_launchCommand();
    }
    
    return
//        ftfl_busyWait() &&  // We are so slow, don't bother.
//        memStoreByte(REG_FTFL_FCCOB0, 0x06) &&
//        memStoreByte(REG_FTFL_FCCOB1, address >> 16) &&
//        memStoreByte(REG_FTFL_FCCOB2, address >> 8) &&
//        memStoreByte(REG_FTFL_FCCOB3, address) &&
//        memStoreByte(REG_FTFL_FCCOB4, longWord >> 24) &&
//        memStoreByte(REG_FTFL_FCCOB5, longWord >> 16) &&
//        memStoreByte(REG_FTFL_FCCOB6, longWord >> 8) &&
//        memStoreByte(REG_FTFL_FCCOB7, longWord) &&
        ftfl_launchCommand();
}

bool ARMKinetisDebug::ftfl_handleCommandStatus(const char *cmdSpecificError)
{
    /*
     * Handle common errors from an FSTAT register value.
     * The indicated "errorMessage" is used for reporting a command-specific
     * error from MGSTAT0. Returns true on success, false on error.
     */

    uint32_t fstat;
    if (!memLoad(REG_FTFL_FSTAT, fstat))
        return false;

    if (fstat & FTFL_FSTAT_RDCOLERR) {
        log(LOG_ERROR, "FLASH: Bus collision error (FSTAT: %08x)", fstat);
        return false;
    }

    if (fstat & (FTFL_FSTAT_FPVIOL | FTFL_FSTAT_ACCERR)) {
        log(LOG_ERROR, "FLASH: Address access error (FSTAT: %08x)", fstat);
        return false;
    }

    if (cmdSpecificError && (fstat & FTFL_FSTAT_MGSTAT0)) {
        // Command-specifid error
        log(LOG_ERROR, cmdSpecificError, fstat);
        return false;
    }

    return true;
}


ARMKinetisDebug::FlashProgrammer::FlashProgrammer(
    ARMKinetisDebug &target, const uint32_t *image, unsigned numSectors)
    : target(target), image(image), numSectors(numSectors)
{}

bool ARMKinetisDebug::FlashProgrammer::begin()
{
    nextLongword = 0;
    numLongwords = numSectors*FLASH_SECTOR_SIZE / 4;
    isVerifying = false;
    
    rewriteFlashCommand = true;  // Set this to true to force loading all registers before programming flash.

    // Start with a mass-erase
    if (!target.flashMassErase())
        return false;

    // Reset again after mass erase, for new protection bits to take effect
    if (!(target.reset() && target.debugHalt() && target.peripheralInit()))
        return false;

//    // Use FlexRAM as normal RAM, for buffering flash sectors
//    if (!target.flashSectorBufferInit())
//        return false;

    return true;
}

bool ARMKinetisDebug::FlashProgrammer::isComplete()
{
    return isVerifying && nextSector == numSectors;
}

bool ARMKinetisDebug::FlashProgrammer::next()
{
    if (isVerifying) {       
        uint32_t address = nextSector * FLASH_SECTOR_SIZE;
        const uint32_t *ptr = image + (nextSector * FLASH_SECTOR_SIZE/4);

        target.log(LOG_NORMAL, "FLASH: Verifying sector at %08x", address);

        uint32_t buffer[FLASH_SECTOR_SIZE/4];
        if (!target.memLoad(address, buffer, FLASH_SECTOR_SIZE/4))
            return false;

        bool okay = true;

        for (unsigned i = 0; i < FLASH_SECTOR_SIZE/4; i++) {
            if (buffer[i] != ptr[i]) {
                target.log(LOG_ERROR, "FLASH: Verify error at %08x. Expected %08x, actual %08x",
                    address + i*4, ptr[i], buffer[i]);
                okay = false;
            }
        }

        if (!okay)
            return false;

        if (++nextSector == numSectors) {
            // Done with verify!
            target.log(LOG_NORMAL, "FLASH: Programming successful!");
        }

    } else {
        // TODO: This doesn't help make things faster.
        int maxSteps = 10;
        do {
          uint32_t address = nextLongword * 4;
      
          if(address%FLASH_SECTOR_SIZE == 0)
              target.log(LOG_NORMAL, "FLASH: Programming longword at %08x", address);

          
          if (!target. ftfl_programLongword(address, image[nextLongword]))
              return false;
            
            
          if (++nextLongword == numLongwords) {
              // Done programming. Another reset! Load new protection flags.
              if (!(target.reset() && target.debugHalt() && target.peripheralInit()))
                  return false;
                
              nextSector = 0;
              isVerifying = true;
          }
        }
        while ((--maxSteps > 0) && !isVerifying);
      
//        target.log(LOG_NORMAL, "FLASH: Programming sector at %08x", address);
//
//        if (!target.flashSectorBufferWrite(0, ptr, FLASH_SECTOR_SIZE/4))
//            return false;
//        if (!target.flashSectorProgram(address))
//            return false;
//        if (++nextSector == numSectors) {
//            // Done programming. Another reset! Load new protection flags.
//            if (!(target.reset() && target.debugHalt() && target.peripheralInit()))
//                return false;
//
//            nextSector = 0;
//            isVerifying = true;
//        }
    }

    return true;
}

static inline uint32_t gpioBitBandAddr(uint32_t addr, unsigned bit)
{
    return (addr - 0x40000000) * 32 + bit * 4 + 0x42000000;
}

static inline uint32_t gpioPortAddr(uint32_t base, unsigned p)
{
    return base + (p >> 12) * (REG_GPIOB_PDOR - REG_GPIOA_PDOR);
}

static inline uint32_t gpioPortBit(unsigned p)
{
    return (p >> 2) & 31;
}

bool ARMKinetisDebug::memStoreBit(uint32_t addr, unsigned bit, uint32_t data)
{
    return memStore(gpioBitBandAddr(addr, bit), data);
}

bool ARMKinetisDebug::memLoadBit(uint32_t addr, unsigned bit, uint32_t &data)
{
    return memLoad(gpioBitBandAddr(addr, bit), data);
}

bool ARMKinetisDebug::pinMode(unsigned p, int mode)
{
    // GPIO, and default drive strength + slew rate
    uint32_t pcrValue = REG_PORT_PCR_MUX(1) | REG_PORT_PCR_DSE | REG_PORT_PCR_SRE;

    // PCR address
    uint32_t pcrAddr = REG_PORTA_PCR0 + p;

    switch (mode) {
        case INPUT_PULLUP:
            // Turn on pullup
            pcrValue |= REG_PORT_PCR_PE | REG_PORT_PCR_PS;
            break;

        case INPUT:
        case OUTPUT:
            // Default PCR value
            break;

        default:
            log(LOG_ERROR, "GPIO: Unsupported pinMode %d", mode);
            return true;
    }

    // Set pin mode
    if (!memStore(pcrAddr, pcrValue))
        return false;

    // Set direction
    return memStoreBit(gpioPortAddr(REG_GPIOA_PDDR, p), gpioPortBit(p), mode == OUTPUT);
}

bool ARMKinetisDebug::digitalWrite(unsigned p, int value)
{
    return memStoreBit(gpioPortAddr(REG_GPIOA_PDOR, p), gpioPortBit(p), value != 0);
}

int ARMKinetisDebug::digitalRead(unsigned p)
{
    uint32_t data;
    if (!memLoadBit(gpioPortAddr(REG_GPIOA_PDIR, p), gpioPortBit(p), data))
        return -1;
    return data;
}

bool ARMKinetisDebug::digitalWritePort(unsigned port, unsigned value)
{
    // Write to all bits on a given port
    return memStore(gpioPortAddr(REG_GPIOA_PDOR, port), value);
}

bool ARMKinetisDebug::usbSetPullup(bool enable)
{
    return memStoreByte(REG_USB0_CONTROL, enable ? USB_CONTROL_DPPULLUPNONOTG : 0);
}
