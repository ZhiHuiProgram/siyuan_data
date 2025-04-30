/*
 *SD update support
 */

 #include <common.h>
 #include <environment.h>
 #include <command.h>
 #include <malloc.h>
 #include <image.h>
 #include <asm/byteorder.h>
 #include <asm/io.h>
 #include <spi_flash.h>
 #include <linux/mtd/mtd.h>
 #include <fat.h>
 
 #ifdef CONFIG_AUTO_UPDATE  /* cover the whole file */
 
 #ifdef CONFIG_AUTO_SD_UPDATE
 #ifndef CONFIG_MMC
 #error "should have defined CONFIG_MMC"
 #endif
 #include <mmc.h>
 #endif
 
 #undef AU_DEBUG
 #undef debug
 //#define	AU_DEBUG
 #ifdef	AU_DEBUG
 #define debug(fmt, args...)	printf(fmt, ##args)
 #else
 #define debug(fmt, args...)
 #endif	/* AU_DEBUG */
 
 /* possible names of files on the medium. */
 /*#define AU_FW		"ZRT_PRJ007ZN_GC2063.bin"/**/
  #define AU_FW		"ZRT_PRJ007ZN_GC2063_FW.uImage"
 #define AU_UBOOT	"boot.bin"
 #define AU_TAG		"tag.bin"
 #define AU_KERNEL	"uImage.jzlzma"
 #define AU_ROOTFS	"rootfs_camera.cpio.jzlzma"
 #define AU_RECOVERY	"recovery.bin"
 #define AU_SYSTEM	"system.bin"
 
 
 struct flash_layout {
	 long start;
	 long end;
 };
 static struct spi_flash *flash;
 
 struct medium_interface {
	 char name[20];
	 int (*init) (void);
	 void (*exit) (void);
 };
 /* layout of the FLASH. ST = start address, ND = end address. */
 #define AU_FL_FW_ST			0x000000
 #define AU_FL_FW_ND			0x1000000
 #define AU_FL_UBOOT_ST		0x0
 #define AU_FL_UBOOT_ND		0x40000
 #define AU_FL_TAG_ST		0x40000
 #define AU_FL_TAG_ND		0xc0000
 #define AU_FL_KERNEL_ST		0xc00000
 #define AU_FL_KERNEL_ND		0x4c0000
 #define AU_FL_ROOTFS_ST		0x4c0000
 #define AU_FL_ROOTFS_ND		0x7c0000
 #define AU_FL_RECOVERY_ST	0x7c0000
 #define AU_FL_RECOVERY_ND	0xac0000
 #define AU_FL_SYSTEM_ST		0xac0000
 #define AU_FL_SYSTEM_ND		0xdc0000
 
 
 static int au_stor_curr_dev; /* current device */
 
 /* index of each file in the following arrays */
 #define IDX_FW			0
 #define IDX_UBOOT		1
 #define	IDX_TAG			2
 #define IDX_KERNEL		3
 #define IDX_ROOTFS		4
 #define IDX_RECOVERY	5
 #define IDX_SYSTEM		6
 
 
 /* max. number of files which could interest us */
 #define AU_MAXFILES 7
 
 /* pointers to file names */
 char *aufile[AU_MAXFILES] = {
	 AU_FW,
	 AU_UBOOT,
	 AU_TAG,
	 AU_KERNEL,
	 AU_ROOTFS,
	 AU_RECOVERY,
	 AU_SYSTEM
 };
 
 /* sizes of flash areas for each file */
 long ausize[AU_MAXFILES] = {
	 AU_FL_FW_ND - AU_FL_FW_ST,
	 AU_FL_UBOOT_ND - AU_FL_UBOOT_ST,
	 AU_FL_TAG_ND - AU_FL_TAG_ST,
	 AU_FL_KERNEL_ND - AU_FL_KERNEL_ST,
	 AU_FL_ROOTFS_ND - AU_FL_ROOTFS_ST,
	 AU_FL_RECOVERY_ND - AU_FL_RECOVERY_ST,
	 AU_FL_SYSTEM_ND - AU_FL_SYSTEM_ST
 };
 
 /* array of flash areas start and end addresses */
 struct flash_layout aufl_layout[AU_MAXFILES] = {
	 { AU_FL_FW_ST,			AU_FL_FW_ND,		},
	 { AU_FL_UBOOT_ST,		AU_FL_UBOOT_ND,	},
	 { AU_FL_TAG_ST,			AU_FL_TAG_ND,		},
	 { AU_FL_KERNEL_ST,		AU_FL_KERNEL_ND,	},
	 { AU_FL_ROOTFS_ST,		AU_FL_ROOTFS_ND,	},
	 { AU_FL_RECOVERY_ST,		AU_FL_RECOVERY_ND,	},
	 { AU_FL_SYSTEM_ST,		AU_FL_SYSTEM_ND,	},
 };
 
 /* where to load files into memory */
 #define LOAD_ADDR	((unsigned char *)(0x80600000))
 
 /* the app is the largest image */
 #define MAX_LOADSZ ausize[IDX_FW]
 
 int LOAD_ID = IDX_FW; //default update all
 int AU_FW_EXIST = -1;
 
 
 void print_memory_region_hex(uint32_t start_address, uint32_t length) 
 {
	 uint32_t i;
	 uint32_t j;
	 uint8_t data;
	 for ( i = 0; i < length; i += 16) 
	 {
		 j = 0;
		 for (; j < 16 && i + j < length; ++j) 
		 {
			   data = *(uint8_t *)(start_address + i + j);
			 printf("%.2X ", data);
		 }
		 printf("\n");
	 }
	 printf("\n");
 }
 
 // 0: 没有按,进入升级   1: 按了 ESC  退出
 /**
  * Check if the ESC key is pressed within 3 seconds and exit.
  */
 int check_for_esc_key(int timeout_seconds)
 {
	 unsigned long end_time = get_timer(0) + timeout_seconds * CONFIG_SYS_HZ;
	 char   esc_pressed = 0;
	 while (get_timer(0) < end_time) 
	 {
		 if (tstc()) 
		 {
			 switch (getc()) 
			 {
				 case 0x1B:  /* ASCII code for ESC */
					 esc_pressed = 1;
					 return 1;
				 default:
					 break;
			 }
		 }
	 }
	 return esc_pressed;
 }
 
 static int au_check_cksum_valid(int idx, long nbytes)
 {
	 image_header_t *hdr;
	 unsigned long checksum;
 
	 hdr = (image_header_t *)LOAD_ADDR;
 
	 if (nbytes != (sizeof(*hdr) + ntohl(hdr->ih_size))) {
		 printf("Image %s bad total SIZE\n", aufile[idx]);
		 return -1;
	 }
	 /* check the data CRC */
	 checksum = ntohl(hdr->ih_dcrc);
 
	 if (crc32(0, (unsigned char const *)(LOAD_ADDR + sizeof(*hdr)),
			 ntohl(hdr->ih_size)) != checksum) {
		 printf("Image %s bad data checksum\n", aufile[idx]);
		 return -1;
	 }
 
	 return 0;
 }
 
 static int au_check_header_valid(int idx, long nbytes)
 {
	 image_header_t *hdr;
	 unsigned long checksum;
 
	 char env[20];
	 char auversion[20];
 
	 hdr = (image_header_t *)LOAD_ADDR;
	 /* check the easy ones first */
 #if 1
	 #define CHECK_VALID_DEBUG
 #else
	 #undef CHECK_VALID_DEBUG
 #endif
 
 #ifdef CHECK_VALID_DEBUG
	 printf("\nmagic %#x %#x\n", ntohl(hdr->ih_magic), IH_MAGIC);
	 printf("arch %#x %#x\n", hdr->ih_arch, IH_ARCH_MIPS);
	 printf("size %#x %#lx\n", ntohl(hdr->ih_size), nbytes);
	 printf("type %#x %#x\n", hdr->ih_type, IH_TYPE_FIRMWARE);
 #endif
	 if (nbytes < sizeof(*hdr)) {
		 printf("Image %s bad header SIZE\n", aufile[idx]);
		 return -1;
	 }
	 if (ntohl(hdr->ih_magic) != IH_MAGIC || hdr->ih_arch != IH_ARCH_MIPS) {
		 printf("Image %s bad MAGIC or ARCH\n", aufile[idx]);
		 return -1;
	 }
	 /* check the hdr CRC */
	 checksum = ntohl(hdr->ih_hcrc);
	 hdr->ih_hcrc = 0;
 
	 if (crc32(0, (unsigned char const *)hdr, sizeof(*hdr)) != checksum) {
		 printf("Image %s bad header checksum\n", aufile[idx]);
		 return -1;
	 }
	 hdr->ih_hcrc = htonl(checksum);
	 /* check the type - could do this all in one gigantic if() */
	 if ((idx == IDX_UBOOT) && (hdr->ih_type != IH_TYPE_FIRMWARE)) {
		 printf("Image %s wrong type\n", aufile[idx]);
		 return -1;
	 }
	 if ((idx == IDX_KERNEL) && (hdr->ih_type != IH_TYPE_KERNEL)) {
		 printf("Image %s wrong type\n", aufile[idx]);
		 return -1;
	 }
	 if ((idx == IDX_ROOTFS) &&
			 (hdr->ih_type != IH_TYPE_RAMDISK) &&
			 (hdr->ih_type != IH_TYPE_FILESYSTEM)) {
		 printf("Image %s wrong type\n", aufile[idx]);
		 ausize[idx] = 0;
		 return -1;
	 }
 
	 if ((idx == IDX_FW) && (hdr->ih_type != IH_TYPE_FIRMWARE)) {
		 printf("Image %s wrong type\n", aufile[idx]);
		 return -1;
	 }
	 /* recycle checksum */
	 checksum = ntohl(hdr->ih_size);
	 /* for kernel and app the image header must also fit into flash */
	 if ((idx == IDX_KERNEL) && (idx == IH_TYPE_RAMDISK))
		 checksum += sizeof(*hdr);
 
	 /* check the size does not exceed space in flash. HUSH scripts */
	 /* all have ausize[] set to 0 */
	 if ((ausize[idx] != 0) && (ausize[idx] < checksum)) {
		 printf("Image %s is bigger than FLASH\n", aufile[idx]);
		 return -1;
	 }
 
	 sprintf(env, "%lx", (unsigned long)ntohl(hdr->ih_time));
	 /*setenv(auversion, env);*/
 
	 return 0;
 }
 static int au_check_FW_header_valid()
 {
	 image_header_t *hdr;
	 unsigned long checksum;
	 
	 char env[20];
	 
	 //使用kernel的image_head_info
	 hdr = (image_header_t *)(LOAD_ADDR+AU_FL_FW_ST);
	 /* check the easy ones first */
	 
 #if 1
	 #define CHECK_VALID_DEBUG
 #else
	 #undef CHECK_VALID_DEBUG
 #endif
 
 #ifdef CHECK_VALID_DEBUG
	 long nbytes = sizeof(*hdr);
	 printf("\nmagic %#x %#x\n", ntohl(hdr->ih_magic), IH_MAGIC);
	 printf("arch %#x %#x\n", hdr->ih_arch, IH_ARCH_MIPS);
	 printf("size %#x %#lx\n", ntohl(hdr->ih_size), nbytes);
	 printf("type %#x %#x\n", hdr->ih_type, IH_TYPE_FIRMWARE);
 #endif
	 /*
	 //not check image head info len
	 if (nbytes < sizeof(*hdr)) {
		 printf("Image %s bad header SIZE\n", aufile[IDX_FW]);
		 return -1;
	 }
	 */
	 if (ntohl(hdr->ih_magic) != IH_MAGIC || hdr->ih_arch != IH_ARCH_MIPS) {
		 printf("Image %s bad MAGIC or ARCH\n", aufile[IDX_FW]);
		 return -1;
	 }
	 /* check the hdr CRC */
	 checksum = ntohl(hdr->ih_hcrc);
	 hdr->ih_hcrc = 0;
	 
	 if (crc32(0, (unsigned char const *)hdr, sizeof(*hdr)) != checksum) {
		 printf("Image %s bad header checksum\n", aufile[IDX_FW]);
		 return -1;
	 }
	 hdr->ih_hcrc = htonl(checksum);
	 /* check the type - could do this all in one gigantic if() */
	 /*
	 if ((idx == IDX_UBOOT) && (hdr->ih_type != IH_TYPE_FIRMWARE)) {
		 printf("Image %s wrong type\n", aufile[idx]);
		 return -1;
	 }
	 */
 //	if ((idx == IDX_KERNEL) && (hdr->ih_type != IH_TYPE_KERNEL)) {
	 if (hdr->ih_type != IH_TYPE_FIRMWARE) {
		 printf("Image %s wrong type\n", aufile[IDX_FW]);
		 return -1;
	 }
	 /*
	 if ((idx == IDX_ROOTFS) &&
			 (hdr->ih_type != IH_TYPE_RAMDISK) &&
			 (hdr->ih_type != IH_TYPE_FILESYSTEM)) {
		 printf("Image %s wrong type\n", aufile[idx]);
		 ausize[idx] = 0;
		 return -1;
	 }
 
	 if ((idx == IDX_FW) && (hdr->ih_type != IH_TYPE_FIRMWARE)) {
		 printf("Image %s wrong type\n", aufile[idx]);
		 return -1;
	 }
	 */
	 
	 /* recycle checksum */
	 checksum = ntohl(hdr->ih_size);
	 /* for kernel and app the image header must also fit into flash */
	 /*
	 if ((idx == IDX_KERNEL) && (idx == IH_TYPE_RAMDISK))
		 checksum += sizeof(*hdr);
	 */
	 /* check the size does not exceed space in flash. HUSH scripts */
	 /* all have ausize[] set to 0 */
	 if ((ausize[IDX_FW] != 0) && (ausize[IDX_FW] < checksum)) {
		 printf("Image %s is bigger than FLASH\n", aufile[IDX_FW]);
		 return -1;
	 }
 
	 sprintf(env, "%lx", (unsigned long)ntohl(hdr->ih_time));
	 /*setenv(auversion, env);*/
	 return 0;
 }
 static int au_do_update(int idx, long sz)
 {
	 image_header_t *hdr, nor_header;
	 unsigned long start, len;
	 unsigned long write_len;
	 int rc;
	 void *buf;
	 char *pbuf;
	 uint32_t img_crc32=0, nor_crc=0;
	 
	 if(IDX_FW == idx || IDX_KERNEL == idx)
	 {
		 if(IDX_FW == idx)
		 {
			 hdr = (image_header_t *)(LOAD_ADDR+AU_FL_FW_ST);
			 //校验和
			 img_crc32 = crc32(0, (unsigned char const *)(LOAD_ADDR)+AU_FL_FW_ST+sizeof(*hdr), ntohl(hdr->ih_size));
		 }
		 else if(IDX_KERNEL == idx)
		 {
			 hdr = (image_header_t *)LOAD_ADDR;
			 //校验和
			 img_crc32 = crc32(0, (unsigned char const *)(LOAD_ADDR), ntohl(hdr->ih_size));
		 }
		 //image_head_info 在kernel起始地址上 写入kernel时不剔除
		 flash->read(0,flash, AU_FL_FW_ST, sizeof(image_header_t), &nor_header);
		 printf("NOR Magic Number: 0x%08x\n", ntohl(nor_header.ih_magic));
		 printf("NOR Image Name: %.*s\n\n", IH_NMLEN, nor_header.ih_name);
		 printf("NOR Image Header CRC: 0x%08x\n", ntohl(nor_header.ih_hcrc));
		 printf("NOR Image Data CRC: 0x%08x\n", ntohl(nor_header.ih_dcrc));
		 
		 printf("%s Parsed Image Header:\n", aufile[idx]);
		 printf("Magic Number: 0x%08x\n", ntohl(hdr->ih_magic));
		 printf("Header CRC: 0x%08x\n", ntohl(hdr->ih_hcrc));
		 printf("Creation Time: %u\n", ntohl(hdr->ih_time));
		 printf("Image Data Size: %u\n", ntohl(hdr->ih_size));
		 printf("Load Address: 0x%08x\n", ntohl(hdr->ih_load));
		 printf("Entry Point: 0x%08x\n", ntohl(hdr->ih_ep));
		 printf("Data CRC: 0x%08x\n", ntohl(hdr->ih_dcrc));
		 printf("OS: %u\n", hdr->ih_os);
		 printf("Architecture: %u\n", hdr->ih_arch);
		 printf("Image Type: %u\n", hdr->ih_type);
		 printf("Compression Type: %u\n", hdr->ih_comp);
		 printf("Image Name: %.*s\n", IH_NMLEN, hdr->ih_name);
 
		 if( nor_header.ih_magic !=  hdr->ih_magic)
		 {
			 debug("----------Image Magic Diff,SD auto update start----------\n");
		 }else if(nor_header.ih_hcrc != hdr->ih_hcrc)
		 {
			 debug("--------Image Header CRC Diff,SD auto update start--------\n");
		 }else if(nor_header.ih_dcrc != hdr->ih_dcrc)
		 {
			 debug("---------Image Data CRC Diff,SD auto update start---------\n");
		 }else {
				 debug("---------ih_magic same, skip SD auto update start----------\n");
			 if(LOAD_ID != -1)
				  printf("%s: file same onrflash needless update,file crc32 %#x\n",aufile[idx],img_crc32);
			 return 1;
		 }
		 if(AU_FW_EXIST == 1 && IDX_KERNEL == idx)
		 {
			 printf("err:kernel file conflict with fw file,skip update kernel\n");
			 return 1;
		 }
 
		 printf("Ready to enter the upgrade, press \"ESC\" to exit within 5s\n");
		 if(check_for_esc_key(5) == 1)  
		 {
			 printf("exit upgrade\n");
			 return 1;
		 }
		 
		 if (ntohl(hdr->ih_dcrc) != img_crc32) 
		 {
			 printf("Image: %s hdr_crc32 %#X != img_crc32%#X\n", aufile[idx], ntohl(hdr->ih_dcrc), img_crc32 );
			 return -1;
		 }else
		 {
			 debug("Image %s crc32 OK %#X\n", aufile[idx], img_crc32);
		 }
	 }
	 else{
		 //校验和
		 img_crc32 = crc32(0, (unsigned char const *)(LOAD_ADDR), sz);
		 debug("file: %s img_crc32 %#X\n", aufile[idx], img_crc32);
 
		 //16M size able to load two fw
		 flash->read(0,flash, aufl_layout[idx].start, sz, LOAD_ADDR+aufl_layout[idx].end);
		 nor_crc = crc32(0, (unsigned char const *)(LOAD_ADDR+aufl_layout[idx].end), sz);
		 if(nor_crc == img_crc32)
		 {
			 if(LOAD_ID != -1)
				 printf("%s: file same onrflash needless update,file crc32 %#x\n",aufile[idx],img_crc32);
			 return 1;
		 }
		 printf("file: %s img_crc32 %#X != onr_crc32 0x%X\n", aufile[idx], img_crc32, nor_crc);
		 nor_crc = 0;
	 }
 
	 start = aufl_layout[idx].start;
	 len = aufl_layout[idx].end - aufl_layout[idx].start;
	 printf("Updating %s to flash at address 0x%lx with length 0x%lx\n", aufile[idx], start, len);
	 /*
	  * erase the address range.
	  */
	 printf("flash erase...\n");
	 rc = flash->erase(0,flash, start, len);
	 if (rc) {
		 printf("SPI flash sector erase failed\n");
		 return -1;
	 }
 
	 buf = map_physmem((unsigned long)LOAD_ADDR+sizeof(*hdr), len, MAP_WRBACK);
	 if (!buf) {
		 puts("Failed to map physical memory\n");
		 return -1;
	 }
 
	 pbuf = buf;
	 write_len  = sz-sizeof(*hdr);
	 /* copy the data from RAM to FLASH */
	 printf("flash write...\n");
	 rc = flash->write(0,flash, start, write_len, pbuf);
	 if (rc) {
		 printf("SPI flash write failed, return %d\n", rc);
		 return -1;
	 }
 
	 if(IDX_FW == idx || IDX_KERNEL == idx)
	 {
		 if(IDX_FW == idx)
		 {
			 /* check the dcrc of the copy */
			 if (crc32(0, (unsigned char const *)(buf +AU_FL_FW_ST),
				 ntohl(hdr->ih_size)) != ntohl(hdr->ih_dcrc)) {
				printf("hdr->ih_size %d\n",ntohl(hdr->ih_size));
				printf("hdr->ih_dcrc %d\n",ntohl(hdr->ih_dcrc));
				 printf("Image %s Bad Data Checksum After COPY\n", aufile[idx]);
				 return -1;
			 }
		 }
		 else //if(IDX_KERNEL == idx)
		 {
			 /* check the dcrc of the copy */
			 if (crc32(0, (unsigned char const *)(buf + sizeof(*hdr)),
				 ntohl(hdr->ih_size)) != ntohl(hdr->ih_dcrc)) {
				 printf("Image %s Bad Data Checksum After COPY\n", aufile[idx]);
				 return -1;
			 }
		 }
		 sz = ntohl(hdr->ih_size);
		 //回读
		 flash->read(0,flash, AU_FL_FW_ST, sz, LOAD_ADDR);
		 nor_crc = crc32(0, (unsigned char const *)(LOAD_ADDR), sz);
	 }
	 else {
		 //回读
		 flash->read(0,flash, start, sz, LOAD_ADDR);
		 nor_crc = crc32(0, (unsigned char const *)(LOAD_ADDR), sz);
		 debug("file %s nor_crc32 0x%X\n", aufile[idx], nor_crc);
	 }
	 if(nor_crc != img_crc32)
	 {
		 printf("%s update, crc32 failed nor_crc 0x%X !=  img_crc32 0x%X\n", aufile[idx],nor_crc, img_crc32);
		 print_memory_region_hex((uint32_t)LOAD_ADDR + aufl_layout[idx].start, 16*5);
		 return -1;
	 }
	 printf("file %s update success\n",aufile[idx]);
	 unmap_physmem(buf, len);
 
	 return 0;
 }
 
 /*
  * If none of the update file(u-boot, kernel or rootfs) was found
  * in the medium, return -1;
  * If u-boot has been updated, return 1;
  * Others, return 0;
  */
 static int update_to_flash(void)
 {
	 int i = 0;
	 long sz;
	 int res, cnt;
	 int uboot_updated = 0;
	 int image_found = 0;
	 image_header_t nor_header;
 
	 cnt = 0;
	 /* just loop thru all the possible files */
	 for (i = 0; i < AU_MAXFILES; i++) {
		 if (LOAD_ID != -1)
			 i = LOAD_ID;
		 image_found = 1;
		 if(IDX_FW == i)
		 {
			 sz = file_fat_read(aufile[i], LOAD_ADDR, MAX_LOADSZ);
			 debug("read %s sz %ld hdr %d\n",
				 aufile[i], sz, sizeof(image_header_t));
			printf("read %s sz %ld hdr %d\n",aufile[i], sz, sizeof(image_header_t));
			 if (sz <= 0 || sz <= sizeof(image_header_t)) {
				 printf("%s not found\n", aufile[i]);
				 AU_FW_EXIST = -1;
				 if (LOAD_ID != -1)
					 break;
				 else
					 continue;
			 }
			 if (au_check_FW_header_valid() < 0) {
				 debug("%s header not valid\n", aufile[i]);
				 if (LOAD_ID != -1)
					 break;
				 else
					 continue;
			 }
			 res = au_do_update(i, sz);
			 if(res < 0)
			 {
				 image_found = 0;
				 debug("update FW failed\n");
				 break;
			 }
			 else
			 {
				 if(!res){
					 debug("update FW success\n");
					 uboot_updated = 1;
					 cnt++;
					 break;
				 }
				 else {
					 AU_FW_EXIST = 1;
					 if (LOAD_ID != -1)
						 break;
					 continue;
				 }
			 }
		 }
		 
		 sz = file_fat_read(aufile[i], LOAD_ADDR, MAX_LOADSZ);
		 debug("read %s sz %ld hdr %d\n",
			 aufile[i], sz, sizeof(image_header_t));
		 if (sz <= 0 || sz <= sizeof(image_header_t)) {
			 printf("%s not found\n", aufile[i]);
			 if (LOAD_ID != -1)
				 break;
			 else
				 continue;
		 }
		 if(IDX_KERNEL == i || IDX_RECOVERY == i)//查看文件只有kernel与recovery有image_head_info
		 {
			 /* just check the header */
			 if (au_check_header_valid(i, sizeof(image_header_t)) < 0) {
				 debug("%s header not valid\n", aufile[i]);
				 if (LOAD_ID != -1)
					 break;
				 else
					 continue;
			 }
		 }
 
		 /* this is really not a good idea, but it's what the */
		 /* customer wants. */
		 res = au_do_update(i, sz);
		 if(res < 0)
		 {
			 image_found = 0;
			 uboot_updated = 0;
			 debug("update %s failed\n",aufile[i]);
			 break;
		 }
		 else {
			 //image same,skip SD auto update start
			 if(!res)
			 {
				 /* If u-boot had been updated, we need to
				  * save current env to flash */
				 if ((0 == strcmp((char *)AU_UBOOT, aufile[i])))
					 uboot_updated = 1;
				 cnt++;
				 debug("update %s success\n",aufile[i]);
			 }
		 }
		 if (LOAD_ID != -1)
			 break;
	 }
 
	 if(cnt > 0 && LOAD_ID==-1)
	 {
		 printf("file check crc32 ok, auto update sucess. reset...\n");
		 // 延迟3秒钟
		 mdelay(3000); // 使用毫秒级延时，3000毫秒即为3秒
		 // 执行重启命令
		 _machine_restart();
	 }
	 if (1 == uboot_updated)
		printf("file check crc32 ok, auto update sucess. reset...\n");
		// 延迟3秒钟
		mdelay(3000); // 使用毫秒级延时，3000毫秒即为3秒
		// 执行重启命令
		_machine_restart();
		 return 1;
	 if (1 == image_found)
		 return 0;
	 return -1;
 }
 /*
  * This is called by board_init() after the hardware has been set up
  * and is usable. Only if SPI flash initialization failed will this function
  * return -1, otherwise it will return 0;
  */
//  static int already_updated = 0;
 int do_auto_update(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
 {
	// if (already_updated) {
    //     printf("Already updated once, skip auto update!\n");
    //     return 0;
    // }
	 block_dev_desc_t *stor_dev;
	 int old_ctrlc;
	 int j;
	 int state = -1;
	 long start = -1, end = 0;
	 
	 if (argc == 1)
		 ;
	 else if (argc == 2) {
		 LOAD_ID  = simple_strtoul(argv[1], NULL, 16);
		 if (LOAD_ID < IDX_FW || LOAD_ID > AU_MAXFILES) {
			 printf("unsupport id!\n");
			 return CMD_RET_USAGE;
		 }
	 } else if (argc == 4) {
		 LOAD_ID  = simple_strtoul(argv[1], NULL, 16);
		 if (LOAD_ID < IDX_FW || LOAD_ID > AU_MAXFILES) {
			 printf("unsupport id!\n");
			 return CMD_RET_USAGE;
		 }
 
		 start  = simple_strtoul(argv[2], NULL, 16);
		 end  = simple_strtoul(argv[3], NULL, 16);
		 if (start >= 0 && end && end > start) {
			 ausize[LOAD_ID] = end  - start;
			 aufl_layout[LOAD_ID].start = start;
			 aufl_layout[LOAD_ID].end = end;
		 } else {
			 printf("error addr,use default\n");
		 }
	 } else {
		 return CMD_RET_USAGE;
	 }
	 debug("device name %s!\n", "mmc");
	 stor_dev = get_dev("mmc", 0);
	 if (NULL == stor_dev) {
		 debug("Unknow device type!\n");
		 return -1;
	 }
 
	 if (fat_register_device(stor_dev, 1) != 0) {
		 debug("Unable to use %s %d:%d for fatls\n",
				 "mmc",
				 au_stor_curr_dev,
				 1);
		 return -1;
	 }
 
	 if (file_fat_detectfs() != 0) {
		 debug("file_fat_detectfs failed\n");
		 return -1;
	 }
 
	 /*
	  * make sure that we see CTRL-C
	  * and save the old state
	  */
	 old_ctrlc = disable_ctrlc(0);
 
	 /*
	  * CONFIG_SF_DEFAULT_SPEED=1000000,
	  * CONFIG_SF_DEFAULT_MODE=0x3
	  */
	 flash = spi_flash_probe(0,0, 0, 1000000, 0x3);
	 if (!flash) {
		 printf("Failed to initialize SPI flash\n");
		 return -1;
	 }
 
	 state = update_to_flash();
 
	 /* restore the old state */
	 disable_ctrlc(old_ctrlc);
 
	 LOAD_ID = -1;
	 AU_FW_EXIST = -1;
 
	 /*
	  * no update file found
	  */
	 /*if (-1 == state)*/
		 /*continue;*/
	 /*
	  * update files have been found on current medium,
	  * so just break here
	  */
 
	 /*
	  * If u-boot has been updated, it's better to save environment to flash
	  */
	 if (1 == state) {
		 /*env_crc_update();*/
		 saveenv();
	 }
	//  if (state >= 0) {
    //     already_updated = 1;  // 刷机成功，标记已刷
    // }
	
	 return 0;
 }
 
 U_BOOT_CMD(
	 sdupdate,	9,	1,	do_auto_update,
	 "auto upgrade file from mmc to flash",
	 "LOAD_ID ADDR_START ADDR_END\n"
	 "LOAD_ID:\n"
	 "	 0-->fw-all\n"
	 "	 1-->u-boot\n"
	 "	 2-->tag\n"
	 "	 3-->kernel\n"
	 "	 4-->rootfs\n"
	 "	 5-->recovery\n"
	 "	 6-->system\n"
	 "ex:\n"
	 "	sdupdate   (update all)\n"
	 "or \n"
	 "	sdupdate 0 0x0 0x1000000"
 );
 #endif /* CONFIG_AUTO_UPDATE */
 