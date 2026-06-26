/* Flash storage mutex — shared between device_config and scheduler.
   All Flash erase/program operations MUST hold this lock. */
#ifndef FLASH_STORAGE_H
#define FLASH_STORAGE_H

void flash_storage_init(void);
void flash_storage_lock(void);
void flash_storage_unlock(void);

#endif
