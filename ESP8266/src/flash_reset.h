#ifndef FLASH_RESET_H
#define FLASH_RESET_H

/*
    JEDEC software reset SPI flash (команды 0x66 RSTEN + 0x99 RST).

    Workaround для ESP-01 модулей с проблемной flash-памятью
    (например, BoyaMicro 25Q80ES*, vendor_id = 0x68): на таких чипах
    после снятия CHIP_EN при сохранённом VCC внутреннее состояние
    flash может зависнуть, и при следующем подъёме CHIP_EN ESP не
    стартует. Программный reset возвращает чип в дефолтный SPI-режим
    перед тем, как Attiny снимет EN.

    Должна вызываться непосредственно перед уходом в deep sleep.
    На время выдачи команд отключается instruction cache; перед
    возвратом cache восстанавливается, чтобы код deepSleep мог
    исполниться из flash.
*/
void flash_software_reset();

#endif // FLASH_RESET_H
