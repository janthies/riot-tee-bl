/* Linker script for the secure bootloader. */

INCLUDE "nrf9160.ld"

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx)  : ORIGIN = FLASH_START_BL, LENGTH = FLASH_SIZE_BL
  RAM   (rwx) : ORIGIN = RAM_START_BL,   LENGTH = RAM_SIZE_BL
}

INCLUDE "nrf_common.ld"