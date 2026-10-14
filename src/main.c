/*
 * Secure bootloader for nRF9160 RIOT-TEE
 *
 * Boot flow:
 *   R1: Provision a device-unique root key in the KMU (first boot only) and
 *       load it into the CryptoCell KDR on every boot
 *   R2: Verify riot-tee firmware integrity via SHA-256 hash
 *   R3: Lock the KDR until the next reset and write-protect the bootloader
 *       flash
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

/* UICR OTP words (freely readable, write-once). The riot-tee firmware hash
   uses OTP[0..7]. OTP[8] marks a completed provisioning and is written last. */
#define FW_HASH_OTP_BASE    0
#define FW_HASH_WORDS       8
#define PROVISIONED_OTP     8
#define PROVISIONED_MAGIC   0x52544231  /* "RTB1" */
#define ERASED              0xFFFFFFFF

/* Number of bootloader flash regions to protect */
#define BL_REGION_COUNT (BL_FLASH_SIZE / 0x8000)

typedef enum {
    UNPROVISIONED,  /* everything provisioning writes is still erased */
    PROVISIONED,    /* the marker is set, provisioning completed */
    INCONSISTENT,   /* provisioning was cut off, e.g. by a power loss */
} provisioning_state_t;

/* ---- Forward declarations ---- */
static void cc_enable(void);
static void cc_disable(void);
static void sha256_hash(const uint8_t *data, uint32_t len, uint32_t digest[8]);
static provisioning_state_t provisioning_state(void);
static void provision(const uint32_t fw_hash[FW_HASH_WORDS]);
static int hash_matches_otp(const uint32_t fw_hash[FW_HASH_WORDS]);
static int kmu_provision_key(int slot, const uint32_t key[4], uint32_t dest_addr);
static int kmu_push_kdr_and_lock(int slot);
static uint32_t uicr_read(const volatile uint32_t *word);
static void uicr_write(volatile uint32_t *word, uint32_t value);
static void wipe(void *buf, size_t len);
static void nvmc_enable_write(void);
static void nvmc_disable_write(void);
static void spu_protect_region(int region);
static void jump_to_image(uint32_t image_addr) __attribute__((noreturn));
static void halt(void) __attribute__((noreturn));

int main(void)
{
#ifdef BL_TIMING_PIN
    nrf_gpio_cfg_output(BL_TIMING_PIN);
    nrf_gpio_pin_set(BL_TIMING_PIN);
    nrf_gpio_pin_clear(BL_TIMING_PIN);
#endif

    /* Step 1: Enable CryptoCell */
    cc_enable();
    cc3xx_lowlevel_init();

    /* ==== R2: Always hash the riot-tee image first ==== */
    uint32_t fw_hash[FW_HASH_WORDS];
    sha256_hash((const uint8_t *)TEE_FLASH_START, TEE_FLASH_SIZE, fw_hash);

    switch (provisioning_state())
    {
    case UNPROVISIONED:
        /* ==== R1: First boot, provision root key and firmware hash ==== */
        provision(fw_hash);
        break;
    case PROVISIONED:
        /* ==== R2: Later boots, verify riot-tee firmware integrity ==== */
        if (!hash_matches_otp(fw_hash))
        {
            halt();
        }
        break;
    default:
        /* A cut-off provisioning left a partial key or hash behind. */
        halt();
    }

    /* Steps 4 and 5: Push the root key from the KMU into the CryptoCell KDR
       registers, then set the CryptoCell lifecycle state to Secure. From then
       on, the KDR cannot be written again until the next reset. */
    if (kmu_push_kdr_and_lock(ROOT_KEY_SLOT) != 0)
    {
        halt();
    }

    /* Step 6: Disable the hard-coded RTL key K_PRTL, which is the same for
       all devices with the same part code. */
    TEE_CC_HOST_RGF->HOST_IOT_KPRTL_LOCK = 1;

    cc_disable();

    /* ==== R3: Write-protect the bootloader flash regions ==== */

    /* Step 11: Remove write permission and set LOCK for bootloader regions */
    for (int i = 0; i < BL_REGION_COUNT; i++)
    {
        spu_protect_region(i);
    }

#ifdef BL_TIMING_PIN
    nrf_gpio_pin_set(BL_TIMING_PIN);
    nrf_gpio_pin_clear(BL_TIMING_PIN);
#endif

    /* ==== R4: Transfer execution to riot-tee (steps 14 to 16) ==== */
    jump_to_image(TEE_FLASH_START);
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

/* ---- Provisioning ---- */

static provisioning_state_t provisioning_state(void)
{
    if (uicr_read(&NRF_UICR_S->OTP[PROVISIONED_OTP]) == PROVISIONED_MAGIC)
    {
        return PROVISIONED;
    }

    /* Without the marker, everything provisioning writes must be erased.
       The key slot header can be read without selecting the slot, and DEST
       is the first word that provisioning writes into the slot. */
    if (uicr_read(&NRF_UICR_S->OTP[PROVISIONED_OTP]) != ERASED ||
        uicr_read(&NRF_UICR_S->KEYSLOT.CONFIG[ROOT_KEY_SLOT].DEST) != ERASED ||
        uicr_read(&NRF_UICR_S->KEYSLOT.CONFIG[ROOT_KEY_SLOT].PERM) != ERASED)
    {
        return INCONSISTENT;
    }
    for (int i = 0; i < FW_HASH_WORDS; i++)
    {
        if (uicr_read(&NRF_UICR_S->OTP[FW_HASH_OTP_BASE + i]) != ERASED)
        {
            return INCONSISTENT;
        }
    }
    return UNPROVISIONED;
}

/* Generates the root key, stores it in the KMU, then stores the firmware
   hash, and marks provisioning as complete as the very last write. */
static void provision(const uint32_t fw_hash[FW_HASH_WORDS])
{
    /* Step 2: Generate 128 bits of entropy via TRNG */
    uint32_t root_key[4];
    if (cc3xx_lowlevel_rng_get_random((uint8_t *)root_key, sizeof(root_key),
                                      CC3XX_RNG_CRYPTOGRAPHICALLY_SECURE) != CC3XX_ERR_SUCCESS)
    {
        halt();
    }

    nvmc_enable_write();

    /* Steps 3 and 5: Write the root key to a KMU key slot in UICR
       (write-once). The slot can only be pushed to the KDR, nobody can read
       or change it. */
    int err = kmu_provision_key(ROOT_KEY_SLOT, root_key,
                                (uint32_t)&TEE_CC_HOST_RGF->HOST_IOT_KDR0);
    wipe(root_key, sizeof(root_key));
    if (err != 0)
    {
        halt();
    }

    /* Step 8: Store the firmware hash in OTP */
    for (int i = 0; i < FW_HASH_WORDS; i++)
    {
        uicr_write(&NRF_UICR_S->OTP[FW_HASH_OTP_BASE + i], fw_hash[i]);
    }

    uicr_write(&NRF_UICR_S->OTP[PROVISIONED_OTP], PROVISIONED_MAGIC);

    nvmc_disable_write();
}

/* Steps 8 to 9: Compare with the stored hash. The comparison does not stop
   at the first difference. */
static int hash_matches_otp(const uint32_t fw_hash[FW_HASH_WORDS])
{
    uint32_t diff = 0;
    for (int i = 0; i < FW_HASH_WORDS; i++)
    {
        diff |= fw_hash[i] ^ uicr_read(&NRF_UICR_S->OTP[FW_HASH_OTP_BASE + i]);
    }
    return diff == 0;
}

/* ---- KMU and KDR ---- */

/* Writes a key slot in the order of the product specification: select the
   slot, destination, key value, permissions, deselect. Key slot ID n+1
   belongs to UICR index n. Returns -1 if the KMU blocked a write, for
   example because the slot was not empty. */
static int kmu_provision_key(int slot, const uint32_t key[4], uint32_t dest_addr)
{
    NRF_KMU_S->SELECTKEYSLOT = slot + 1;

    uicr_write(&NRF_UICR_S->KEYSLOT.CONFIG[slot].DEST, dest_addr);
    for (int i = 0; i < 4; i++)
    {
        uicr_write(&NRF_UICR_S->KEYSLOT.KEY[slot].VALUE[i], key[i]);
    }
    /* STATE=active, PUSH=enabled, READ=disabled, WRITE=disabled. Flash bits
       can only go 1->0, so clearing WRITE (bit 0) and READ (bit 1) suffices. */
    uicr_write(&NRF_UICR_S->KEYSLOT.CONFIG[slot].PERM, 0xFFFFFFFC);

    int blocked = NRF_KMU_S->STATUS & KMU_STATUS_BLOCKED_Msk;
    NRF_KMU_S->SELECTKEYSLOT = 0;
    return blocked ? -1 : 0;
}

/* Pushes a key slot into the KDR and then sets the lifecycle state to
   Secure. After a reset, the KDR keeps the first value written to it,
   unless the state is switched to DebugEnable, which allows overwriting it.
   The state can be set only once per reset, so setting it to Secure keeps
   later software from switching to DebugEnable and replacing the key. The
   KDR registers report 1 on a read once the whole key is retained. Returns
   0 on success. */
static int kmu_push_kdr_and_lock(int slot)
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
    if (failed)
    {
        return -1;
    }

    TEE_CC_HOST_RGF->HOST_IOT_LCS = CC_HOST_RGF_HOST_IOT_LCS_LCS_Secure;
    while (TEE_CC_HOST_RGF->HOST_IOT_LCS != (CC_HOST_RGF_HOST_IOT_LCS_LCS_IS_VALID_Msk |
                                             CC_HOST_RGF_HOST_IOT_LCS_LCS_Secure))
        ;
    return 0;
}

/* ---- UICR access ---- */

/* nRF9160 anomaly 7: two back-to-back flash reads, one from UICR and one
   from main flash, can go wrong. The erratum names a UICR read followed by a
   main flash read. On the board, the opposite order also failed: a literal
   pool load directly before a UICR read returned a wrong address. Every UICR
   access is therefore enclosed in DSBs. */
static uint32_t uicr_read(const volatile uint32_t *word)
{
    __DSB();
    uint32_t value = *word;
    __DSB();
    return value;
}

/* Writes one word of UICR and waits until the NVMC is done. */
static void uicr_write(volatile uint32_t *word, uint32_t value)
{
    __DSB();
    *word = value;
    __DSB();
    while (!NRF_NVMC_S->READY)
        ;
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

/* ---- SPU flash region protection ---- */

/* Removes the write permission and locks the region until the next reset.
   Read and execute stay, because the bootloader still runs from here until
   the jump. LOCK alone would freeze the reset default, which allows writes. */
static void spu_protect_region(int region)
{
    uint32_t perm = NRF_SPU_S->FLASHREGION[region].PERM;
    perm &= ~SPU_FLASHREGION_PERM_WRITE_Msk;
    NRF_SPU_S->FLASHREGION[region].PERM = perm | SPU_FLASHREGION_PERM_LOCK_Msk;
}

/* ---- Hand-over ---- */

/* Sets VTOR and MSP from the image's vector table and branches to its reset
   handler. Both happen in one asm block, so no code runs on the old stack
   after MSP has changed. */
static void jump_to_image(uint32_t image_addr)
{
    const uint32_t *vtor = (const uint32_t *)image_addr;

    SCB->VTOR = image_addr;
    __DSB();
    __ISB();

    __asm volatile (
        "msr msp, %0\n"
        "bx  %1\n"
        :
        : "r" (vtor[0]), "r" (vtor[1])
    );
    __builtin_unreachable();
}

/* ---- Halt ---- */

static void halt(void)
{
    while (1)
        ;
}
