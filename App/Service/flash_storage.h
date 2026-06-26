#ifndef FLASH_STORAGE_H
#define FLASH_STORAGE_H

#ifdef __cplusplus
extern "C" {
#endif

void flash_storage_init(void);
void flash_storage_lock(void);
void flash_storage_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* FLASH_STORAGE_H */
