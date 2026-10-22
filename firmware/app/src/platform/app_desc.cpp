// The image's app descriptor (esp_app_desc_t, the first 256 bytes of the
// image's read-only data), with this firmware's project name and version
// instead of the Arduino library builder's ("arduino-lib-builder", its git
// hash). The prebuilt esp_app_format library defines `esp_app_desc` as a weak
// symbol; this strong one replaces it, and our object is linked before the
// library, so it is the one at the start of .rodata_desc.
//
// The update check reads these fields from a downloaded image before it is
// installed (ui/src/update_check.h check_image_head): an image whose project
// is not "soccer-display" or whose version is not the one the manifest
// announced is refused. esp_app_get_description() returns this too.
// The other fields are as the library sets them (efuse block revisions and
// the MMU page size from sdkconfig); elf2image fills in app_elf_sha256.
#include <esp_app_desc.h>
#include <esp_idf_version.h>
#include <sdkconfig.h>

#include "version.h"

#define APP_DESC_STR2(x) #x
#define APP_DESC_STR(x) APP_DESC_STR2(x)

static_assert(sizeof(esp_app_desc_t) == 256, "esp_app_desc_t layout changed");
static_assert(sizeof(APP_VERSION) <= 32, "version too long for the app descriptor");
static_assert(CONFIG_MMU_PAGE_SIZE == 0x10000, "mmu_page_size below assumes 64 KB pages");

extern "C" const __attribute__((section(".rodata_desc"))) esp_app_desc_t esp_app_desc = {
    ESP_APP_DESC_MAGIC_WORD,  // magic_word
    0,                        // secure_version
    {0, 0},                   // reserv1
    APP_VERSION,              // version
    "soccer-display",         // project_name (kFirmwareProjectName)
    __TIME__,                 // time
    __DATE__,                 // date
    "v" APP_DESC_STR(ESP_IDF_VERSION_MAJOR) "." APP_DESC_STR(ESP_IDF_VERSION_MINOR) "." APP_DESC_STR(
        ESP_IDF_VERSION_PATCH),  // idf_ver
    {0},                         // app_elf_sha256 (elf2image)
    CONFIG_ESP_EFUSE_BLOCK_REV_MIN_FULL,
    CONFIG_ESP_EFUSE_BLOCK_REV_MAX_FULL,
    16,           // mmu_page_size: log2(64 KB)
    {0, 0, 0},    // reserv3
    {0},          // reserv2
};
