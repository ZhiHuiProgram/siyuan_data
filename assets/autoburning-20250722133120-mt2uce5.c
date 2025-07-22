#include <common.h>
#include <command.h>
#include <mmc.h>
#include <fat.h>
#include <fs.h>   
#include <spi_flash.h>
#include <image.h>
#include <console.h>
#include <linux/types.h>
#include <watchdog.h>
#include <blk.h>
#include <image.h>
#include <spi.h>
#include <asm/global_data.h>
#include <environment.h>
DECLARE_GLOBAL_DATA_PTR;

#define FW_FILE_NAME "flash_combined.image"
#define FW_FLASH_ADDR 0x0
#define FW_MAX_SIZE   0x1000000
#define LOAD_ADDR     ((void *)0x40008000)
static struct spi_flash *flash;

static int check_for_esc_key(int timeout_s)
{
    ulong end = get_timer(0) + timeout_s * CONFIG_SYS_HZ;
    while (get_timer(0) < end) {
        if (tstc() && getc() == 0x1B)
            return 1;
    }
    return 0;
}

static int check_image_valid(image_header_t *i_head, int max_size)
{
    if (ntohl(i_head->ih_magic) != IH_MAGIC) {
        printf("Bad magic\n");
        return -1;
    }

    if (!image_check_hcrc(i_head)) {
        printf("Bad header CRC\n");
        return -1;
    }

    if (crc32(0, (uchar *)(i_head + 1), ntohl(i_head->ih_size)) != ntohl(i_head->ih_dcrc)) {
        printf("Bad data CRC\n");
        return -1;
    }

    if (sizeof(*i_head) + ntohl(i_head->ih_size) > max_size) {
        printf("Image too large\n");
        return -1;
    }

    return 0;
}

static int do_mcupdate(void)
{
    long size;
    image_header_t *i_head;
    ulong fw_size;
    int ret;
    struct mmc *mmc;
    struct blk_desc *mmc_dev;

    printf("MC6357 Auto Update Start\n");

    if (mmc_initialize(gd->bd)) {
        printf("MMC initialization failed\n");
        return 1;
    }
    
    /* 再取第 0 号设备 */
    mmc = find_mmc_device(0);
    if (!mmc) {
        printf("Failed to find MMC device 0\n");
        return 1;
    }

    mmc_dev = blk_get_dev("mmc", 0);
    if (!mmc_dev) {
        printf("blk_get_dev failed\n");
        return 1;
    }

    if (fat_register_device(mmc_dev, 1) != 0) {
        printf("FAT fs init failed\n");
        return 1;
    }
    fs_set_blk_dev("mmc", "1:1", FS_TYPE_FAT);

    loff_t _fw_size;
    ret = fat_size(FW_FILE_NAME, &_fw_size);
    if (ret < 0) {
        printf("Error: 找不到文件 \"%s\"，fat_size 返回 %d\n",
            FW_FILE_NAME, ret);
        return -1;
    }
    if (_fw_size > FW_MAX_SIZE) {
        printf("Error: 文件太大 %llu > FW_MAX_SIZE %u\n",
            (unsigned long long)_fw_size, FW_MAX_SIZE);
        return -1;
    }

    ulong load_addr = (ulong)LOAD_ADDR;
    size = file_fat_read(FW_FILE_NAME, (void *)load_addr, FW_MAX_SIZE);
    printf("file_fat_read => size=%ld, dest=0x%08lx  file=%s\n", size, load_addr, FW_FILE_NAME);

    // flush_cache(load_addr, sizeof(image_header_t));
    flush_cache(load_addr, size);

    if (size <= sizeof(image_header_t)) {
        printf("Load failed or file too small: %ld\n", size);
        return 1;
    }
    i_head = (image_header_t *)LOAD_ADDR;

    if (check_image_valid(i_head, size) != 0)
        return 1;

    fw_size = ntohl(i_head->ih_size);
    printf("Firmware size: %lu bytes\n", fw_size);

    printf("Press ESC within 5s to cancel update...\n");
    if (check_for_esc_key(5)) {
        printf("Upgrade cancelled.\n");
        return 0;
    }

    flash = spi_flash_probe(0, 0, 1000000, SPI_MODE_3);
    if (!flash) {
        printf("SPI flash init failed\n");
        return 1;
    }

    printf("Erasing flash at 0x%x size 0x%lx...\n", FW_FLASH_ADDR, fw_size);
    ret = flash->erase(flash, FW_FLASH_ADDR, fw_size);
    if (ret) {
        printf("Flash erase failed\n");
        return 1;
    }

    printf("Writing flash...\n");
    ret = flash->write(flash, FW_FLASH_ADDR, fw_size, (const void *)(i_head + 1));
    if (ret) {
        printf("Flash write failed\n");
        return 1;
    }

    void *verify_buf = (void *)((ulong)LOAD_ADDR + FW_MAX_SIZE);
    memset(verify_buf, 0, fw_size);
    flash->read(flash, FW_FLASH_ADDR, fw_size, verify_buf);

    if (crc32(0, verify_buf, fw_size) != ntohl(i_head->ih_dcrc)) {
        printf("Post-write CRC mismatch!\n");
        return 1;
    }

    printf("Firmware update OK, rebooting...\n");
    mdelay(3000);
    do_reset(NULL, 0, 0, NULL);
    return 0;
}
void burning_boot(void){
    do_mcupdate();
}