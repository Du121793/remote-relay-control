/* Unified RTC backup register layout — single source of truth.
   STM32F767 has 32 x 32-bit backup registers (BKP_DR0..BKP_DR31).

   DR0–DR5  : fault record
   DR6      : scheduler magic + enabled mask
   DR7      : RTC signature (cluster time restore)
   DR8      : boot counter
   DR9      : relay_protection restart detection
   DR10–DR25: scheduler pause_until[0..15] (16 channels)
   DR26–DR31: free
*/

#ifndef RTC_BACKUP_LAYOUT_H
#define RTC_BACKUP_LAYOUT_H

/* ── Fault record (6 registers) ── */
#define RTC_BKP_FAULT_SIGNATURE    RTC_BKP_DR0
#define RTC_BKP_FAULT_TYPE         RTC_BKP_DR1
#define RTC_BKP_FAULT_PC           RTC_BKP_DR2
#define RTC_BKP_FAULT_LR           RTC_BKP_DR3
#define RTC_BKP_FAULT_CFSR         RTC_BKP_DR4
#define RTC_BKP_FAULT_HFSR         RTC_BKP_DR5

/* ── Scheduler magic + enabled mask ── */
#define RTC_BKP_SCHED_MAGIC_EN     RTC_BKP_DR6

/* ── Scheduler pause_until base (DR10..DR25 for channels 0..15) ── */
#define RTC_BKP_SCHED_PAUSE_BASE   RTC_BKP_DR10

/* ── RTC signature for cluster time ── */
#define RTC_BKP_RTC_SIGNATURE      RTC_BKP_DR7

/* ── Boot counter ── */
#define RTC_BKP_BOOT_COUNTER       RTC_BKP_DR8

/* ── Relay protection restart detection ── */
#define RTC_BKP_RELAY_RESTART      RTC_BKP_DR9

#endif /* RTC_BACKUP_LAYOUT_H */
