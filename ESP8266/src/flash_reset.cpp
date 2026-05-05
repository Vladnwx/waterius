#include "flash_reset.h"

#include <Arduino.h>
#include <esp8266_peri.h>
#include <spi_flash.h>

extern "C"
{
    extern uint32_t Wait_SPI_Idle(SpiFlashChip *fc);
    extern void Cache_Read_Disable(void);
    extern void Cache_Read_Enable(uint8_t map, uint8_t p, uint8_t v);
}

// Выдать однобайтовую команду без адреса/данных через SPI0 (системный flash).
static void IRAM_ATTR spi0_send_cmd(uint8_t cmd)
{
    while (SPI0CMD & SPICMDUSR) {}            // дождаться idle
    SPI0U  = SPIUCOMMAND;                     // только command-фаза
    SPI0U1 = 0;
    SPI0U2 = (7U << SPILCOMMAND) | cmd;       // 8-бит команда (length-1 = 7)
    SPI0CMD = SPICMDUSR;                      // запуск транзакции
    while (SPI0CMD & SPICMDUSR) {}            // дождаться завершения
}

void IRAM_ATTR flash_software_reset()
{
    Wait_SPI_Idle(flashchip);
    Cache_Read_Disable();

    spi0_send_cmd(0x66);  // JEDEC Reset Enable (RSTEN)
    spi0_send_cmd(0x99);  // JEDEC Reset Device (RST)

    // t_RST ~30 µs (Winbond/GD/Boya datasheets). CPU @80MHz: ~2400 циклов.
    // Запас 2x — busy wait дешёвый, IRAM-функция, прерывания не трогаем.
    for (volatile uint32_t i = 0; i < 1000; i++) {}

    Cache_Read_Enable(0, 0, 1);
}
