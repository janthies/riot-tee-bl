/*
 * Secure bootloader for nRF9160 RIOT-TEE
 *
 * Boot flow:
 *   R1: Provision a device-unique root key in the KMU (first boot only) and
 *       load it into the CryptoCell KDR on every boot
 *   R2: Verify riot-tee firmware integrity via SHA-256 hash
 *   R3: Lock bootloader flash regions via SPU
 *   R4: Transfer execution to riot-tee
 */

#include <stdint.h>
#include <string.h>

#include "nrf9160.h"
#include "nrf9160_bitfields.h"

/* CryptoCell register definitions */
#include "cc310_registers.h"

/* cc3xx low-level driver */
#include "cc3xx_init.h"
#include "cc3xx_hash.h"
#include "cc3xx_rng.h"

#ifdef BL_TIMING_PIN
#include "nrf_gpio.h"
#endif

/* KMU key slot used for the device root key (4 x 32-bit = 128 bits) */
#define ROOT_KEY_SLOT 0

/* OTP index for storing the riot-tee firmware hash (8 words = 256 bits).
   Uses OTP[0..7] in the UICR OTP region (freely readable, write-once). */
#define FW_HASH_OTP_BASE 0

/* Number of bootloader flash regions to lock */
#define BL_REGION_COUNT (BL_FLASH_SIZE / 0x8000)

/* ---- Forward declarations ---- */
static void cc_enable(void);
static void cc_disable(void);
static void sha256_hash(const uint8_t *data, uint32_t len, uint32_t digest[8]);
static int kmu_provision_key(int slot, const uint32_t key[4], uint32_t dest_addr);
static int kmu_push_kdr(int slot);
static void wipe(void *buf, size_t len);
static void nvmc_enable_write(void);
static void nvmc_disable_write(void);
static void spu_lock_region(int region);
static void halt(void);

int main(void)
{
    /* ==== R1: Provision device-unique root key ==== */

#ifdef BL_TIMING_PIN
    nrf_gpio_cfg_output(BL_TIMING_PIN);
    nrf_gpio_pin_set(BL_TIMING_PIN);
    nrf_gpio_pin_clear(BL_TIMING_PIN);
#endif

    /* Step 1: Enable CryptoCell */
    cc_enable();
    cc3xx_lowlevel_init();

    int first_boot = (NRF_UICR_S->OTP[FW_HASH_OTP_BASE] == 0xFFFFFFFF);
    uint32_t root_key[4] = {0};

    /* ==== R2: Always hash the riot-tee image first ==== */
    /* Disable flash cache to ensure we hash actual flash contents */
    NRF_NVMC_S->ICACHECNF = 0;

    uint32_t fw_hash[8];
    sha256_hash((const uint8_t *)TEE_FLASH_START, TEE_FLASH_SIZE, fw_hash);

    /* Re-enable cache */
    NRF_NVMC_S->ICACHECNF = NVMC_ICACHECNF_CACHEEN_Enabled;

    if (first_boot)
    {
        /* First boot: generate and store root key */

        /* Step 2: Generate 128 bits of entropy via TRNG */
        if (cc3xx_lowlevel_rng_get_random((uint8_t *)root_key, sizeof(root_key),
                                          CC3XX_RNG_CRYPTOGRAPHICALLY_SECURE) != CC3XX_ERR_SUCCESS)
        {
            halt();
        }

        /* Step 3: Write root key to KMU key slot in UICR (write-once) */
        nvmc_enable_write();
        int err = kmu_provision_key(ROOT_KEY_SLOT, root_key,
                                    (uint32_t)&TEE_CC_HOST_RGF->HOST_IOT_KDR0);
        wipe(root_key, sizeof(root_key));
        if (err != 0)
        {
            halt();
        }

        /* Store the firmware hash in OTP */
        for (int i = 0; i < 8; i++)
        {
            NRF_UICR_S->OTP[FW_HASH_OTP_BASE + i] = fw_hash[i];
            while (!NRF_NVMC_S->READY)
                ;
        }
        nvmc_disable_write();
    }
    else
    {
        /* ==== R2: Verify riot-tee firmware integrity ==== */

        /* Read stored hash from UICR OTP */
        uint32_t stored_hash[8];
        for (int i = 0; i < 8; i++)
        {
            stored_hash[i] = NRF_UICR_S->OTP[FW_HASH_OTP_BASE + i];
        }

        /* Compare hashes, halt on mismatch */
        int match = 1;
        for (int i = 0; i < 8; i++)
        {
            if (fw_hash[i] != stored_hash[i])
            {
                match = 0;
            }
        }
        if (!match)
        {
            halt();
        }
    }

    /* Step 4: Push the root key from the KMU into the CryptoCell KDR
       registers, on the first boot as on every later one. */
    if (kmu_push_kdr(ROOT_KEY_SLOT) != 0)
    {
        halt();
    }

    /* Step 6: Lock KPRTL to prevent further KDR writes */
    TEE_CC_HOST_RGF->HOST_IOT_KPRTL_LOCK = 1;

    cc_disable();

    /* ==== R3: Lock bootloader flash regions ==== */

    /* Step 11: Set SPU FLASHREGION[n].PERM.LOCK for bootloader regions */
    for (int i = 0; i < BL_REGION_COUNT; i++)
    {
        spu_lock_region(i);
    }

    /* ==== R4: Transfer execution to riot-tee ==== */

    /* Step 14: Set VTOR to riot-tee vector table */
    uint32_t *vtor = (uint32_t *)TEE_FLASH_START;
    SCB->VTOR = TEE_FLASH_START;

    /* Step 15: Set MSP from riot-tee vector table */
    __set_MSP(vtor[0]);

#ifdef BL_TIMING_PIN
    nrf_gpio_pin_set(BL_TIMING_PIN);
    nrf_gpio_pin_clear(BL_TIMING_PIN);
#endif

    /* Step 16: Branch to riot-tee reset handler */
    void (*riot_tee_reset)(void) = (void (*)(void))(vtor[1]);
    riot_tee_reset();

    /* Should never reach here */
    halt();
}

/* ---- CryptoCell enable/disable ---- */

static void cc_enable(void)
{
    NRF_CRYPTOCELL_S->ENABLE = 1;
}

static void cc_disable(void)
{
    NRF_CRYPTOCELL_S->ENABLE = 0;
}

/* ---- TRNG ---- */

/* ---- SHA-256 via cc3xx low-level driver ---- */

static void sha256_hash(const uint8_t *data, uint32_t len, uint32_t digest[8])
{
    cc3xx_lowlevel_hash_init(CC3XX_HASH_ALG_SHA256);

    /* Feed data in chunks through a RAM buffer to avoid cc3xx DMA
       reading stale flash data from the AHB bus cache. */
    uint8_t buf[256];
    uint32_t offset = 0;
    while (offset < len)
    {
        uint32_t chunk = (len - offset > sizeof(buf)) ? sizeof(buf) : (len - offset);
        memcpy(buf, data + offset, chunk);
        cc3xx_lowlevel_hash_update(buf, chunk);
        offset += chunk;
    }

    cc3xx_lowlevel_hash_finish(digest, SHA256_OUTPUT_SIZE);
    cc3xx_lowlevel_hash_uninit();
}

/* ---- KMU and KDR ---- */

/* Writes a key slot in the order of the product specification: select the
   slot, destination, key value, permissions, deselect. Key slot ID n+1
   belongs to UICR index n. Returns -1 if the KMU blocked a write, for
   example because the slot was not empty. */
static int kmu_provision_key(int slot, const uint32_t key[4], uint32_t dest_addr)
{
    NRF_KMU_S->SELECTKEYSLOT = slot + 1;

    NRF_UICR_S->KEYSLOT.CONFIG[slot].DEST = dest_addr;
    while (!NRF_NVMC_S->READY)
        ;
    for (int i = 0; i < 4; i++)
    {
        NRF_UICR_S->KEYSLOT.KEY[slot].VALUE[i] = key[i];
        while (!NRF_NVMC_S->READY)
            ;
    }
    /* STATE=active, PUSH=enabled, READ=disabled, WRITE=disabled. Flash bits
       can only go 1->0, so clearing WRITE (bit 0) and READ (bit 1) suffices. */
    NRF_UICR_S->KEYSLOT.CONFIG[slot].PERM = 0xFFFFFFFC;
    while (!NRF_NVMC_S->READY)
        ;

    int blocked = NRF_KMU_S->STATUS & KMU_STATUS_BLOCKED_Msk;
    NRF_KMU_S->SELECTKEYSLOT = 0;
    return blocked ? -1 : 0;
}

/* Pushes a key slot into the KDR. The KDR registers report 1 on a read once
   the whole key is retained. Returns 0 on success. */
static int kmu_push_kdr(int slot)
{
    /* The KDR can only be loaded once per reset. */
    if (TEE_CC_HOST_RGF->HOST_IOT_KDR0 == 1)
    {
        return -1;
    }

    NRF_KMU_S->EVENTS_KEYSLOT_PUSHED = 0;
    NRF_KMU_S->EVENTS_KEYSLOT_REVOKED = 0;
    NRF_KMU_S->EVENTS_KEYSLOT_ERROR = 0;
    NRF_KMU_S->SELECTKEYSLOT = slot + 1;
    NRF_KMU_S->TASKS_PUSH_KEYSLOT = 1;
    while (!NRF_KMU_S->EVENTS_KEYSLOT_PUSHED &&
           !NRF_KMU_S->EVENTS_KEYSLOT_REVOKED &&
           !NRF_KMU_S->EVENTS_KEYSLOT_ERROR)
        ;
    int failed = NRF_KMU_S->EVENTS_KEYSLOT_REVOKED ||
                 NRF_KMU_S->EVENTS_KEYSLOT_ERROR ||
                 TEE_CC_HOST_RGF->HOST_IOT_KDR0 != 1;
    NRF_KMU_S->SELECTKEYSLOT = 0;
    return failed ? -1 : 0;
}

/* Clears key material. The volatile pointer keeps the compiler from dropping
   the stores because the buffer is not read afterwards. */
static void wipe(void *buf, size_t len)
{
    volatile uint8_t *p = buf;
    while (len--)
    {
        *p++ = 0;
    }
}

/* ---- NVMC (flash write control) ---- */

static void nvmc_enable_write(void)
{
    NRF_NVMC_S->CONFIG = NVMC_CONFIG_WEN_Wen;
    while (!NRF_NVMC_S->READY)
        ;
}

static void nvmc_disable_write(void)
{
    NRF_NVMC_S->CONFIG = NVMC_CONFIG_WEN_Ren;
    while (!NRF_NVMC_S->READY)
        ;
}

/* ---- SPU flash region locking ---- */

static void spu_lock_region(int region)
{
    /* Set the LOCK bit to prevent further modification of this region's permissions */
    NRF_SPU_S->FLASHREGION[region].PERM |= SPU_FLASHREGION_PERM_LOCK_Msk;
}

/* ---- Halt ---- */

static void halt(void)
{
    while (1)
        ;
}
