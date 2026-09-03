#include <stdint.h>
#include <string.h>

#include "CH59x_common.h"
#include "iap_config.h"

#if KBD_TRIMODE_DISPATCHER
#define RUNTIME_ADDR       0x0C00u
#define RUNTIME_MAGIC      0x52554E54u
#define RUNTIME_VERSION    0x0001u
typedef struct __attribute__((packed)) {
    uint32_t magic; uint16_t version; uint16_t flags; uint32_t seq;
    uint8_t current_layer; uint8_t last_mode; uint8_t reserved[238];
    uint32_t crc32;
} runtime_page_t;

static uint32_t crc32_calc(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) { crc ^= *data++; for (uint8_t i=0; i<8; ++i)
        crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1); }
    return crc ^ 0xFFFFFFFFu;
}
#endif

static uint8_t ReadImageFlag(void)
{
    __attribute__((aligned(4))) uint8_t buf[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    EEPROM_READ(IAP_DATAFLASH_ADD, (uint32_t *)buf, 4);
    if (buf[0] == IMAGE_A_FLAG || buf[0] == IMAGE_B_FLAG || buf[0] == IMAGE_IAP_FLAG) {
        return buf[0];
    }
    return IMAGE_A_FLAG;
}

static void WriteImageFlag(uint8_t flag)
{
    __attribute__((aligned(4))) uint8_t buf[4] = {0};
    EEPROM_ERASE(IAP_DATAFLASH_ADD, EEPROM_PAGE_SIZE);
    buf[0] = flag;
    EEPROM_WRITE(IAP_DATAFLASH_ADD, (uint32_t *)buf, 4);
}

#if KBD_TRIMODE_DISPATCHER
static int ValidateImage(uint32_t addr, uint32_t marker)
{
    volatile uint32_t first_word = *(volatile uint32_t *)addr;
    return first_word != 0xFFFFFFFFu && first_word != 0u &&
           *(volatile uint32_t *)(addr + 4u) == marker;
}

static int CopyImageBtoTarget(uint32_t target, uint32_t size)
{
    __attribute__((aligned(4))) uint8_t buf[IAP_COPY_CHUNK_SIZE];
    uint32_t offset = 0;

    if (FLASH_ROM_ERASE(target, size) != 0) {
        return -1;
    }

    while (offset < size) {
        uint32_t chunk = IAP_COPY_CHUNK_SIZE;
        if (offset + chunk > size) {
            chunk = size - offset;
        }

        memcpy(buf, (const void *)(IMAGE_B_START_ADD + offset), chunk);
        if (FLASH_ROM_WRITE(target + offset, buf, chunk) != 0) {
            return -1;
        }
        if (memcmp((const void *)(target + offset), buf, chunk) != 0) {
            return -1;
        }
        offset += chunk;
    }

    return (*(volatile uint32_t *)target == *(volatile uint32_t *)IMAGE_B_START_ADD) ? 0 : -1;
}

static uint8_t ReadPreferredMode(void)
{
    runtime_page_t best __attribute__((aligned(4))) = {0};
    runtime_page_t cur __attribute__((aligned(4)));
    uint8_t found = 0;
    for (uint8_t page = 0; page < 4; ++page) {
        EEPROM_READ(RUNTIME_ADDR + ((uint32_t)page * EEPROM_PAGE_SIZE), &cur, sizeof(cur));
        if (cur.magic != RUNTIME_MAGIC || cur.version != RUNTIME_VERSION ||
            crc32_calc((const uint8_t *)&cur, sizeof(cur) - 4u) != cur.crc32) continue;
        if (!found || cur.seq > best.seq) { best = cur; found = 1; }
    }
    return found && best.last_mode <= 2u ? best.last_mode : 0u;
}

static uint8_t ReadSelectedMode(void)
{
    trimode_selector_t selector __attribute__((aligned(4)));
    if (EEPROM_READ(TRIMODE_SELECTOR_DATAFLASH_ADD, &selector,
                    sizeof(selector)) == 0 &&
        selector.magic == TRIMODE_SELECTOR_MAGIC &&
        selector.tail == TRIMODE_SELECTOR_TAIL &&
        selector.mode <= 2u &&
        selector.mode_inv == (uint8_t)~selector.mode) {
        return selector.mode;
    }
    return ReadPreferredMode();
}

static void JumpToImage(uint32_t marker)
{
    uint32_t addr = 0;
    if (marker == IMAGE_FLAG_2G4 && ValidateImage(IMAGE_2G4_START_ADD, IMAGE_FLAG_2G4))
        addr = IMAGE_2G4_START_ADD;
    else if (ValidateImage(IMAGE_BLE_START_ADD, IMAGE_FLAG_BLE))
        addr = IMAGE_BLE_START_ADD;
    else if (ValidateImage(IMAGE_2G4_START_ADD, IMAGE_FLAG_2G4))
        addr = IMAGE_2G4_START_ADD;
    if (addr) ((void (*)(void))addr)();
}
#else
#define jumpApp ((void (*)(void))((uint32_t)IMAGE_A_START_ADD))

static int ValidateImage(uint32_t addr)
{
    volatile uint32_t first_word = *(volatile uint32_t *)addr;
    return first_word != 0xFFFFFFFFu && first_word != 0u;
}

static int CopyImageBtoA(void)
{
    __attribute__((aligned(4))) uint8_t buf[IAP_COPY_CHUNK_SIZE];
    uint32_t offset = 0;

    if (FLASH_ROM_ERASE(IMAGE_A_START_ADD, IMAGE_A_SIZE) != 0) return -1;
    while (offset < IMAGE_SIZE) {
        uint32_t chunk = IAP_COPY_CHUNK_SIZE;
        if (offset + chunk > IMAGE_SIZE) chunk = IMAGE_SIZE - offset;
        memcpy(buf, (const void *)(IMAGE_B_START_ADD + offset), chunk);
        if (FLASH_ROM_WRITE(IMAGE_A_START_ADD + offset, buf, chunk) != 0) return -1;
        offset += chunk;
    }
    return (*(volatile uint32_t *)IMAGE_A_START_ADD ==
            *(volatile uint32_t *)IMAGE_B_START_ADD) ? 0 : -1;
}
#endif

int main(void)
{
#if (defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif

    SetSysClock(CLK_SOURCE_PLL_60MHz);

#if (defined(HAL_SLEEP)) && (HAL_SLEEP == TRUE)
    GPIOA_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
    GPIOB_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PU);
#endif

#if KBD_TRIMODE_DISPATCHER
    uint32_t preferred = ReadSelectedMode() == 2u ? IMAGE_FLAG_2G4 : IMAGE_FLAG_BLE;
    if (ReadImageFlag() == IMAGE_IAP_FLAG) {
        uint32_t marker = *(volatile uint32_t *)(IMAGE_B_START_ADD + 4u);
        uint32_t target = marker == IMAGE_FLAG_2G4 ? IMAGE_2G4_START_ADD :
                          marker == IMAGE_FLAG_BLE ? IMAGE_BLE_START_ADD : 0u;
        uint32_t size = marker == IMAGE_FLAG_2G4 ? IMAGE_2G4_SIZE :
                        marker == IMAGE_FLAG_BLE ? IMAGE_BLE_SIZE : 0u;
        if (target && ValidateImage(IMAGE_B_START_ADD, marker) &&
            CopyImageBtoTarget(target, size) == 0 && ValidateImage(target, marker)) {
            WriteImageFlag(IMAGE_A_FLAG);
            FLASH_ROM_ERASE(IMAGE_B_START_ADD, IMAGE_B_SIZE);
            preferred = marker;
        } else if (target) {
            preferred = marker == IMAGE_FLAG_2G4 ? IMAGE_FLAG_BLE : IMAGE_FLAG_2G4;
        }
    }
    JumpToImage(preferred);
#else
    if (ReadImageFlag() == IMAGE_IAP_FLAG) {
        if (ValidateImage(IMAGE_B_START_ADD) && CopyImageBtoA() == 0) {
            WriteImageFlag(IMAGE_A_FLAG);
            FLASH_ROM_ERASE(IMAGE_B_START_ADD, IMAGE_B_SIZE);
        } else {
            WriteImageFlag(IMAGE_A_FLAG);
        }
    }
    if (ValidateImage(IMAGE_A_START_ADD)) jumpApp();
#endif
    while (1) {
    }
}
