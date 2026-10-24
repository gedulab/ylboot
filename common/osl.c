#include <common.h>
#include <malloc.h>
#include <sysmem.h>
#include <asm/io.h>
#include <fs.h>
#include <linux/errno.h>
#include "osl.h"
#include "ndb.h"
#include <menu.h>	/* round 3c: the prototypes of menu_show() and osl_countdown_input() */
#include "boot_rkimg.h"
#include <asm/arch/boot_mode.h>
#include "efi_loader.h"

DECLARE_GLOBAL_DATA_PTR;

osl_mgr omgr; // the global object for osl mgr.
static const char* devparts[] = {
	"0:1", "0:2", "0:3"
};
static const char* ini_paths[] = {
	"/ylboot.ini", "/boot/ylboot.ini"
};
extern char* yl_boot_args[];
extern int yl_pwr_btn_click_count; // defined and set in rk8xx_pwrkey.c
void bootmenu_show(int delay);

static int osl_feed_sum_section(osl_mgr* om, char* name, char* value)
{
    int ret = 0;
    switch(*name) {
    	case 'd': // default
			if(om->ini_section_ == NULL)
				om->ini_section_ = value;
			else
				printf("osl: boot choice %s in ini is ignored by enforced %s\n", value, om->ini_section_);
			break;
		default:
			break;
	}

	return ret;
}

static int osl_feed_boot_section(osl_mgr* om, char* name, char* value)
{
    int ret = 0;
    switch(*name) {
    case 'k': // kernel
        om->krnl_ = value;
        break;
    case 'i': // initramfs
        om->initramfs_ = value;
        break;
    case 'd': // dtb
        om->dtb_ = value;
        break;
    case 'B': // BootArgs
        om->cmdline_ = value;
        yl_boot_args[0] = value; // to be compatable with old code TODO 
        break; 
	case 'r':
		om->root_ = value;
		break;
    default:
        ret = -1;
    }
    printf("osl: received ini config %s=%s\n", name, value);

    return ret;
}
// seek specified section and return pointer to the first line of the section
static char* osl_seek_section(char* ini, loff_t length, const char* section)
{
	char* line_start = ini;
	char* line_end;
	int slen = strlen(section);
	bool found = false;

	do {
		if(*line_start == '[') {
			if(strncmp(line_start+1, section, slen) == 0 && *(line_start+ slen+1) == ']') {
				found = true; 
			}
		}
		line_end = strchr(line_start, '\n');
		if(line_end == NULL) {
			line_end = line_start + strlen(line_start);
		}
		line_start = line_end + 1;
	} while (!found && (line_end - ini) < length);

	return found ? line_start: NULL;
}
// scan all items in one secion
static int osl_scan_items(osl_mgr* om, char* start, char* file_end, PFN_INI_FEED_VPAIR pfn_feed)
{
    char* line_end, *line_start = start,*equal_sep,*value;

    do {
        line_end = strchr(line_start, '\n');
        if(line_end) {
			if( *(line_end - 1) == '\r') {
				printf("Badbad, ini file contains MSFT's line ends [%s]\n", line_start);
				*(line_end - 1) = 0;
			}
			*line_end = 0; // cut it;
		}
		else
			line_end = line_start + strlen(line_start);

		if(*line_start == '[') {
			// reach another section
			break;
		} else if(*line_start == '#')			
			goto tag_next_line; // comment line 
        equal_sep = strchr(line_start, '=');
        if(equal_sep) {
            *equal_sep = 0;
			value = equal_sep + 1;
			if(strlen(value) > 0 && value[0] != ' ')
            	pfn_feed(om, line_start, value);
			*equal_sep = '='; // restore it for future parse
        } // else ignore, might be comments, etc.

tag_next_line:
        line_start = line_end + 1;
    } while(line_end != NULL && line_start < file_end);
	
	return 0;
}
// scan all section titles of one INI file, used to generate menu items
static int osl_scan_section_headers(osl_mgr* om, char* start, char* file_end, PFN_INI_FEED_SECTION pfn_feed,
	const char* ifname, const char* devpart)
{
    char* line_end, *line_start = start,*s_start,*s_end;
	bool sum_skipped = false;
    do {
        line_end = strchr(line_start, '\n');
        if(line_end) {
			*line_end = 0; // cut it;
		}
		else
			line_end = line_start + strlen(line_start);

		if(*line_start != '[') 
			goto tag_next_line; // comment line 
		s_start = line_start + 1;
        s_end = strchr(line_start, ']');
        if(s_end) {
			if(!sum_skipped) {
				sum_skipped = true;
				goto tag_next_line;
			}
            *s_end = 0;
			pfn_feed(om, s_start, ifname, devpart);
			*s_end = ']'; // restore it for future parse
        } // else ignore, might be comments, etc.

tag_next_line:
        line_start = line_end + 1;
    } while(line_end != NULL && line_start < file_end);
	
	return 0;
}
// read a file
static int osl_read_file(osl_mgr* om, const char * ifname, const char* devpart, int fstype, const char* path)
{
	int ret = 0;
	loff_t read;

	if(path == NULL) {
		puts("osl: bad call to osl_read_file() with NULL path\n");
		return -E_BAD_ARGUMENT;
	}
    ret = fs_set_blk_dev(ifname, devpart, fstype);
	if (ret) {
		goto failure;
	}
    if(om->ini_buffer_ == NULL) {
        om->ini_buffer_ = (char*)malloc(MAX_INI_BYTES);  
        if(om->ini_buffer_ == NULL) {
            puts("osl: malloc ini buffer failed, oom.");
            return -ENOMEM;
        }
    }    
    ret = fs_read(path, (ulong)om->ini_buffer_, 0, MAX_INI_BYTES - 1, &read);
	if (ret < 0) {
		goto failure;
	}
	om->ini_bytes_ = read;
	printf("osl: ini is read to %p total %lld\n", om->ini_buffer_, read);
	*(om->ini_buffer_ + read) = 0; // append a zero to the end.

    return 0;
failure:
	printf("failed to read boot ini for %d\n", ret);
	return ret;
}

// read boot options from an ini file, extended by Raymond in GSL
static int osl_read_boot_ini(osl_mgr* om, const char * ifname, const char* devpart, int fstype, const char* path)
{
	int ret; 
	char* line_start;

	ret = osl_read_file(om, ifname, devpart, fstype, path);
	if(ret < 0) {
		printf("osl: read file %s %s %s failed %d\n", ifname, devpart, path, ret);
		return ret;
	}

    // parse the file and read summary section firstly
    line_start = osl_seek_section(om->ini_buffer_, om->ini_bytes_, "sum");
    if(line_start == NULL) {
        puts("osl: sum section is not found, use defaults\n");
	} else   
		osl_scan_items(om, line_start, om->ini_buffer_ + om->ini_bytes_, osl_feed_sum_section);
	
	// make sure it's not null
	if(om->ini_section_ == NULL) {
		om->ini_section_ = "krnl5"; // default is kernel 5
	}
	// save boot options into om's attributes.
    line_start = osl_seek_section(om->ini_buffer_, om->ini_bytes_, om->ini_section_);
    if(line_start != NULL) {
		osl_scan_items(om, line_start, om->ini_buffer_ + om->ini_bytes_, osl_feed_boot_section);
	} else {
        printf("osl: section %s is not found\n", om->ini_section_);
        return -ENOENT;
    }	

    return 0;
}

static void osl_set_boot_mode(int devtag)
{
	int value;

	switch(devtag) {
	case 'n':
		value = BOOT_NVME;
		break;
	case 's':
		value = BOOT_SDC;
		break;
	case 'e':
		value = BOOT_EMMC;
		break;
	case 'u':
		value = BOOT_USB;
		break;
	default:
		value = BOOT_NVME;
		break;
	}

	writel(value, (void *)CONFIG_ROCKCHIP_BOOT_MODE_REG);
}

static int boot_from_emmc(int retry);
static int boot_from_nvme(int retry);
static int boot_from_sdcard(int retry);

static enum boot_yl_kernel_ver {
	YL_KERNEL_DEFAULT = 0,
	YL_KERNEL_5X,
	YL_KERNEL_6X,
	YL_KERNEL_KASAN,
} cur_yl_kernel_ver;

static char* boot_image_cmd[] = {
	"load nvme 0 ${loadaddr} Image\0",
	"load nvme 0 ${loadaddr} Image-5x\0",
	"load nvme 0 ${loadaddr} Image-6x\0",
	"load nvme 0 ${loadaddr} Image-KASAN\0",
};

static char* init_image_cmd[] = {
	"load nvme 0 ${initrd_addr} initrd.img\0",
	"load nvme 0 ${initrd_addr} initrd.img-5.10.110-rockchip-rk3588-taiyi\0",
	"load nvme 0 ${initrd_addr} initrd.img-6.1.115-hanyang-rk35xx\0",
	"load nvme 0 ${initrd_addr} initrd.img-6.1.43-kasan\0",
};

static char* dtb_image_cmd[] = {
	"load nvme 0 ${fdt_addr_r} /dtb/rockchip/yourland.dtb\0",
	"load nvme 0 ${fdt_addr_r} /dtb/rockchip/yourland.dtb\0",
	"load nvme 0 ${fdt_addr_r} /dtb/rockchip/yourland6.dtb\0",
	"load nvme 0 ${fdt_addr_r} /dtb/rockchip/yourland.dtb\0",
};

static int initrd_img_get_again(char dev)
{
	int ret;
	char cmd[128] = { 0 };

	switch (dev) {
	case 'e':
		strncpy(cmd, "load mmc 1:1 ${initrd_addr} initrd.img-5.10.110-rockchip-rk3588\0", 128);
		break;
	case 'c':
		strncpy(cmd, "load mmc 0:2 ${initrd_addr} initrd.img-5.10.110-rockchip-rk3588\0", 128);
		break;
	case 'n':
	default:
		strncpy(cmd, "load nvme 0 ${initrd_addr} initrd.img-5.10.110-rockchip-rk3588\0", 128);
		break;
	}

	printf("%c: [%s]\n", dev, cmd);
	ret = run_command(cmd, 0);

	return ret;
}

static int boot_env_handle(boot_env_info* boot_env)
{
	printf("prepare boot from %s\n", boot_env->device);

	char cmd[128] = { 0 };
	int i, initrd_size, ret;

	initrd_size = 0;
	for (i = 0; i < boot_env->cmdscnt; i++) {
		snprintf(cmd, 127, boot_env->envcmds[i]);
		printf("will run command [%s]\n", cmd);
		ret = run_command(cmd, 0);
		if (i == boot_env->initrd_site && ret != 0) {
			printf("will try other inird image\n");
			ret = initrd_img_get_again(boot_env->device[0]);
		}

		if ((ret != 0) && (boot_env->skip_failed[i] == 0)) {
			printf("run command failed, ret: %d\n", ret);
			goto TAG_FAILED;
		}

		if (i == boot_env->initrd_site) {
			initrd_size = simple_strtoul(env_get("filesize"), NULL, 16);
		}
	}

	if ((initrd_size != 0)) {
		osl_set_boot_mode(*boot_env->device);

		printf("will run boot command\n");
		snprintf(cmd, 127, "booti ${loadaddr} ${initrd_addr}:%x ${fdt_addr_r}", initrd_size);
		printf("boot cmd: [%s]\n", cmd);
		run_command(cmd, 0);

		return 0;
	}

TAG_FAILED:
	printf("boot from %s failed !!!\n", boot_env->device);
	return 1;
}

static int backup_boot_try(int boot_sts, int retry, int type)
{
	if (boot_sts == 0 || retry == UNNEED_RETRY) {
		return boot_sts;
	}

	switch (type) {
	case NVME_BOOT_FAILED:
		return boot_from_emmc(UNNEED_RETRY);
	case EMMC_BOOT_FAILED:
		return boot_from_nvme(UNNEED_RETRY);
	case USB_UPDATE_FAILED:
	case SDC_BOOT_FAILED:
		return boot_from_emmc(NEED_RETRY);
	default:
		return boot_sts;
	}
}

static int boot_from_sdcard(int retry)
{
	int ret;
	char* cmdslist[] = {
		"mmc list", "ls mmc 1", "mmc dev 1", "load mmc 1:1 ${loadaddr} /Image",
		"run sdcargs", "load mmc 1:1 ${initrd_addr} /initrd.img",
		"load mmc 1:1 ${fdt_addr_r} /dtb/rockchip/yourland.dtb"
	};
	int skiplist[7] = { 0 };
	boot_env_info boot_sdc_env = {
		.device = "sdc",
		.envcmds = cmdslist,
		.cmdscnt = sizeof(cmdslist)/sizeof(cmdslist[0]),
		.initrd_site = 5,
		.skip_failed = skiplist
	};

	ret = boot_env_handle(&boot_sdc_env);
    if(ret != 0)
	    ret = backup_boot_try(ret, retry, SDC_BOOT_FAILED);

	return ret;
}
// old usb_update_emmc
static int boot_from_usb(int retry)
{
	int ret;
	char* cmdslist[] = {
		"mw.b 0x10000000 0 0x04000000", "usb reset", "load usb 0:1 ${loadaddr_} /boot/Image.gz",
		"run usbargs", "load usb 0:1 ${initrd_addr} /boot/initrd.img", "setenv fdt_addr_r 0x17000000",
		"unzip ${loadaddr_} ${loadaddr}"
	};
	int skiplist[7] = { 0 };
	boot_env_info boot_usb_env = {
		.device = "usb",
		.envcmds = cmdslist,
		.cmdscnt = sizeof(cmdslist)/sizeof(cmdslist[0]),
		.initrd_site = 4,
		.skip_failed = skiplist
	};

	ret = boot_env_handle(&boot_usb_env);
	if(ret != 0)
		ret = backup_boot_try(ret, retry, USB_UPDATE_FAILED);

	return ret;
}

static int boot_from_emmc(int retry)
{
	int ret;
	char* cmdslist[] = {
		"ls mmc 0:2", "mmc dev 0", 
		"load mmc 0:2 ${loadaddr_} /boot/Image.gz", 
		"run mmcargs",
		"load mmc 0:2 ${initrd_addr} /boot/initrd.img", 
		"setenv fdt_addr_r 0x17000000",
		"load mmc 0:2 ${fdt_addr_r} /boot/x5.dtb", // x5 is used for legacy kernel from ODM. added by Raymond on 2025-7-25		
		"unzip ${loadaddr_} ${loadaddr}"
	};
	int skiplist[7] = { 0 };
	boot_env_info boot_emmc_env = {
		.device = "emmc",
		.envcmds = cmdslist,
		.cmdscnt = sizeof(cmdslist)/sizeof(cmdslist[0]),
		.initrd_site = 4,
		.skip_failed = skiplist
	};

	ret = boot_env_handle(&boot_emmc_env);
    if(ret != 0)
	    ret = backup_boot_try(ret, retry, EMMC_BOOT_FAILED);

	return ret;
}

static int boot_from_nvme(int retry)
{
	int ret;
	char* cmdslist[] = {
		"pci enum", 
		"nvme scan", 
		boot_image_cmd[cur_yl_kernel_ver],  
		"run nvmeargs",
		init_image_cmd[cur_yl_kernel_ver],
		dtb_image_cmd[cur_yl_kernel_ver],
	};
	int skiplist[7] = {
		1, 0, 0, 0,
		0, 0, 0
	};
	boot_env_info boot_nvme_env = {
		.device = "nvme",
		.envcmds = cmdslist,
		.cmdscnt = sizeof(cmdslist)/sizeof(cmdslist[0]),
		.initrd_site = 5,
		.skip_failed = skiplist
	};

	ret = boot_env_handle(&boot_nvme_env);
    if(ret != 0) {
    	ret = backup_boot_try(ret, retry, NVME_BOOT_FAILED);
	}
	return ret;
}

typedef struct _boot_ini_info {
	char* title_name;
	int title_size;
	char** destination;
} boot_ini_info;

char* yl_boot_args[] = {
	"\0",
	"\0"
};

#include <memalign.h>

static int boot_env_get(const char* bootif, const char* devpart)
{
	char cmd[0x100], * site_start, * site_end,dev_info[2][128];
	int read_size, i, ret, ini_size;
	char* ini_buf, * ini_not_exits;
	boot_ini_info boot_ini[] = {
		{
			.title_name = "Image=",
			.title_size = 6,
			.destination = boot_image_cmd
		},
		{
			.title_name = "Initrd=",
			.title_size = 7,
			.destination = init_image_cmd
		},
		{
			.title_name = "BootArgs=",
			.title_size = 9,
			.destination = yl_boot_args
		}
	};

	ini_buf = (char*)0x18000000;
	ret = 0;
	switch(bootif[0]) {
	case 'n':
		goto TAG_NVME;
	case 's':
		strncpy(dev_info[0], "mmc", 128);
		strncpy(dev_info[1], "1:1", 128);
	case 'e':
		strncpy(dev_info[0], "mmc", 128);
		strncpy(dev_info[1], "0:2", 128);
	case 'u':
		strncpy(dev_info[0], "usb", 128);
		strncpy(dev_info[1], "0:1", 128);
	case '5':
	case '6':
		return 1;
	default:
TAG_NVME:
		strncpy(dev_info[0], "nvme", 128);
		strncpy(dev_info[1], "0", 128);
		run_command("pci enum", 0);
		run_command("nvme scan", 0);
	}

	run_command("size nvme 0 /ylboot.ini", 0);
	run_command("setenv ini_not_exits $?", 0);
	ini_not_exits = env_get("ini_not_exits");
	if (ini_not_exits[0] != '0') {
		return 2;
	}

	ini_size = simple_strtoul(env_get("filesize"), NULL, 16);
	if (ini_size >= 0x1000) {
		return 2;
	}

	snprintf(cmd, 0x100, "load %s %s 0x18000000 /ylboot.ini", dev_info[0], dev_info[1]);
	printf("will run command [%s]\n", cmd);
	run_command(cmd, 0);
	read_size = simple_strtoul(env_get("filesize"), NULL, 16);
	if (read_size >= 0x1000 || read_size <= 0) {
		printf("read file error\n");
		return 2;
	}
	ini_buf[read_size - 1] = '\n';
	ini_buf[read_size] = '\0';

	for (i = 0; i < 3; i++) {
		site_start = strstr(ini_buf, boot_ini[i].title_name);
		if (site_start != NULL) {
			site_start += boot_ini[i].title_size;
			site_end = strstr(ini_buf, "\n");
			*site_end = '\0';
			ini_buf = site_start;
			strncpy(boot_ini[i].destination[0], ini_buf, 0x100);
			ini_buf = site_end + 1;
			printf("get config: %s\n", boot_ini[i].destination[0]);
		}
	}

	return ret;
}
static int osl_get_boot_mode_register()
{
	static reg_boot = 0;
	if(reg_boot == 0)
		reg_boot = readl((void *)CONFIG_ROCKCHIP_BOOT_MODE_REG);

	return reg_boot;
}
/*
// check whether it's restore emmc using usb 
// this is triggered by pressing power button 5 times
*/
static bool osl_is_usb_recovery(void)
{
	char* usb_update = (char*)USB_RESTORE_FLAG_ADDRESS;
	int reg_boot = osl_get_boot_mode_register();
	if(reg_boot == BOOT_USB_UPDATE)
		return true;
	
	return (*usb_update == USB_UPDATE_TAG_VAL);
}

static int osl_boot_device_legacy(const char* bootif, const char* devpart, const char* kernel_version)
{
	int ret;
	if(osl_is_usb_recovery()) {
		printf("get usb update tag\n");
		goto TAG_USB_UPDATE;
	}

	ret = boot_env_get(bootif, devpart);
	if (ret != 0) {
		printf("will use default boot info\n");
	}
	if(kernel_version != NULL) {
		if(strchr(kernel_version, '5'))
			cur_yl_kernel_ver = YL_KERNEL_5X;
		else if(strchr(kernel_version, '6'))
			cur_yl_kernel_ver = YL_KERNEL_6X;
		else
			cur_yl_kernel_ver = YL_KERNEL_DEFAULT;
	}
	switch(*bootif) {
	case 's':
		ret = boot_from_sdcard(NEED_RETRY);
        break;
	case 'e':
		ret = boot_from_emmc(NEED_RETRY);
        break;
	case 'u':
TAG_USB_UPDATE:
		ret = boot_from_usb(NEED_RETRY);
        break;
	case '5':
		cur_yl_kernel_ver = YL_KERNEL_5X;
		return boot_from_nvme(NEED_RETRY);
	case '6':
		cur_yl_kernel_ver = YL_KERNEL_6X;
		ret = boot_from_nvme(NEED_RETRY);
        break;
	case 'k':
		cur_yl_kernel_ver = YL_KERNEL_KASAN;
	default:
		ret = boot_from_nvme(NEED_RETRY);
        break;
	}

    return ret;
}
/*
int check_load_version(void)
{
	int ret;
	char* bootdev;

	bootdev = env_get("bootos");
	printf("ulan's current boot device %s\n", bootdev);

	ret = osl_boot_device_legacy(*bootdev);
	if ((ret != 0) && (*bootdev != 's')) {
		printf("last try to boot from sdcard !!!\n");
		ret = boot_from_sdcard(UNNEED_RETRY);
	}

	return ret;
}

static void osl_set_boot_stor(const char* store)
{
	switch(store[0]) {
	case 'e':
		env_set("bootos", "emmc");
		break;
	case 's':
		env_set("bootos", "sdc");
	default:
		env_set("bootos", "nvme");
	}
}
*/
// prepare boot device and assign default settings
static int osl_prepare_bootdev(osl_mgr* om, const char* bootdev)
{
    int ret = 0;

    om->dtb_ = "/dtb/rockchip/yourland.dtb";
    om->total_cmds_ = 0;

    if(strcmp(bootdev, "5-kernel") == 0) {
		om->kernel_ver_ = 5;
		om->ini_section_ = "krnl5";
		goto TAG_NVME;
	}
	else if(strcmp(bootdev, "6-kernel") == 0) { 
		om->kernel_ver_ = 6;
		om->ini_section_ = "krnl6";
		goto TAG_NVME;
	}
	else if(strcmp(bootdev, "kasan") == 0 ) {
		om->ini_section_ = "KASAN";
		goto TAG_NVME;
	}
	else if(strcmp(bootdev, "nvme") == 0 ) {
TAG_NVME:	
		om->bootif_ = "nvme";
		om->devpart_ = om->devpart_ == NULL? "0" : om->devpart_;
		om->fs_type_ = FS_TYPE_ANY;
        om->env_cmd_ = "run nvmeargs";
		om->bootmedia_ = YL_BOOT_MEDIA_NVME;
        run_command("pci enum", 0);
		run_command("nvme scan", 0);   
	}
	else if(strcmp(bootdev, "sdc") == 0 ) { // sdcard
		om->bootif_ = "mmc";
		om->devpart_ = "1:1";
		om->fs_type_ = FS_TYPE_ANY;
        om->env_cmd_ = "run sdcargs";
		om->bootmedia_ = YL_BOOT_MEDIA_SDCARD;
        run_command("mmc list", 0);
		run_command("ls mmc 1", 0);         
		run_command("mmc dev 1", 0);         
    }
	else if(strcmp(bootdev, "emmc") == 0 ) {
		om->bootif_ = "mmc";
		om->devpart_ = "0:2";
		om->fs_type_ = FS_TYPE_ANY;
		om->krnl_ = "/boot/Image.gz";
		om->dtb_ = "/boot/yourland.dtb"; //"/boot/x5.dtb";
        om->env_cmd_ = "run mmcargs";
		om->bootmedia_ = YL_BOOT_MEDIA_EMMC;
        run_command("ls mmc 0:2", 0);
		run_command("mmc dev 0", 0); 
		run_command("setenv fdt_addr_r 0x17000000", 0); 
    }
	else if(strcmp(bootdev, "usb") == 0 ) {
		om->bootif_ = "usb";
		om->devpart_ = "0:1";
		om->bootmedia_ = YL_BOOT_MEDIA_USB;
		if(osl_is_usb_recovery()) {
			run_command("mw.b 0x10000000 0 0x04000000", 0); // clear the usb_update flag
			run_command("usb reset", 0); 
			run_command("setenv fdt_addr_r 0x17000000", 0);         
		} else {
			run_command("usb start", 0); 
			// env_set("bootargs", "root=/dev/sda2 rootfstype=ext4 rw console=ttyFIQ0,1500000 psi=1 rootwait quiet "
			// 	"splash earlyprintk earlyprintk=ttyFIQ0,1500000 verbose nokaslr systemd.debug_shell=1");
		}
		run_command("fstype usb 0:1 ramrootfstype", 0); // detect fstype and set it to env var
		om->env_cmd_ = "run usbargs";
		om->fs_type_ = FS_TYPE_ANY;
	}
	else if(strcmp(bootdev, "mmc") == 0 ) { // alias of emmc for bootmenu TODO, simply the code
		om->bootif_ = bootdev;
		om->devpart_ = "0:2";
		om->fs_type_ = FS_TYPE_ANY;
		om->krnl_ = "/boot/Image";
		om->initramfs_ = "/boot/initrd.img";
		om->dtb_ = "/boot/dtb/rockchip/yourland6.dtb"; //"/boot/x5.dtb";
        om->env_cmd_ = "run mmcargs";
		om->bootmedia_ = YL_BOOT_MEDIA_EMMC;
        run_command("ls mmc 0:2", 0);
		run_command("mmc dev 0", 0); 
		run_command("setenv fdt_addr_r 0x17000000", 0); 
    }
	else {
        printf("unknown bootdev %s\n", bootdev);
        ret = -EFAULT;
    }
    printf("osl-%s: boot dev %s %s fs-type %d is prepared\n", bootdev, om->bootif_, 
		om->devpart_, om->fs_type_);
    return ret;
}

// return command index or -ENOMEM for failure;
static int osl_push_cmd(osl_mgr* om, char* cmd)
{
    int ret;
    if(om->total_cmds_ < OSL_MAX_BOOT_CMDS - 1) {
        om->cmds_[om->total_cmds_] = cmd;
        ret = om->total_cmds_;
        om->total_cmds_++;
    } else {
        puts("osl: commands array is out of usage");
        ret = -ENOMEM;
    }   
    return ret;
}

static int osl_exec_boot_cmds(osl_mgr* om)
{
    int i, ret = 0, initrd_size = 0;
    char cmd[80];
    const char *cmd_ptr;

    for(i = 0; i < om->total_cmds_; i++) {
        printf("osl: fire cmd %s\n", om->cmds_[i]);
        ret = run_command(om->cmds_[i], 0);
        if(ret != 0) {
            printf("osl: cmd failed with %d\n", ret);
            return ret; // return failure so that try other options 2024-11-28
        }
        if(i>0 && om->initramfs_cmd_ == i) {
            initrd_size = env_get_hex("filesize", 0);
        }
    }

    if(strstr(om->dtb_, "x5.dtb")) {
		printf("legacy dtb %s is obsolete\n", om->dtb_);
    }

    osl_set_boot_mode(*om->bootif_);
    if(om->boot_efi_) {
		efi_set_bootdev(om->bootif_, om->devpart_, om->krnl_, NULL, 0);
        cmd_ptr = "bootefi ${loadaddr} ${fdt_addr_r}";
	}
	else if(initrd_size>0) {
        snprintf(cmd, sizeof(cmd), "booti ${loadaddr} ${initrd_addr}:%x ${fdt_addr_r}", initrd_size);
        cmd_ptr = cmd;
    } else {
        cmd_ptr = "booti ${loadaddr} - ${fdt_addr_r}";
    }
    // run the env cmd now;
	if(om->env_cmd_)
    	run_command(om->env_cmd_, 0);

    printf("osl: emitting [%s]\n", cmd_ptr);
    ret = run_command(cmd_ptr, 0);

    return ret;    
}
// check existence of the kerenl image 
static int osl_check_kernel(osl_mgr* om)
{
	int i;
	char * krnl_tries[] = {
		"Image", "Image.gz","/boot/Image.gz","/boot/Image"
	};
	om->krnl_ = om->krnl_ == NULL ? krnl_tries[0] : om->krnl_;

	if(file_exists(om->bootif_, om->devpart_, om->krnl_, om->fs_type_))
		return 0;
	printf("osl: kernel %s not exist, try other names\n", om->krnl_);
	for(i = 0; i<sizeof(krnl_tries)/sizeof(krnl_tries[0]); i++) {
		if(strcmp(krnl_tries[i], om->krnl_) == 0) 
			continue;
		if(file_exists(om->bootif_, om->devpart_, krnl_tries[i], om->fs_type_)) {
			printf("osl: found %s\n", krnl_tries[i]);
			om->krnl_ = krnl_tries[i];
			return 0;
		}
	}
	return -ENOENT;
}
static void osl_set_rootfs(osl_mgr* om, const char* rootfs)
{
	const char* var = NULL;
	switch(om->bootmedia_) {
	case YL_BOOT_MEDIA_NVME:
		var = "nvmeroot";
		break;
	case YL_BOOT_MEDIA_EMMC:
		var = "mmcroot";
		break;
	case YL_BOOT_MEDIA_SDCARD:
		var = "sdroot";
		break;
	case YL_BOOT_MEDIA_USB:
		var = "ramroot";
		break;
	case YL_BOOT_MEDIA_UNKNOWN:
	case YL_BOOT_MEDIA_MAX:
		var = "nvmeroot";
		break;
	}
	if(var)
		env_set(var, rootfs);
}

// boot yourland using ini directed method 
static int osl_boot_by_ini(osl_mgr* om, const char* bootdev) 
{
    int ret, i, j;
    char cmd_buffer[512], *cmd;
    int left = sizeof(cmd_buffer), n;
	bool found = false;
    // step 1: prepare boot device
    ret = osl_prepare_bootdev(om, bootdev);
    if(ret < 0)
        return ret;

    // step 2: try to read the boot ini file from the boot device
	for(i = 0; i < sizeof(ini_paths)/sizeof(ini_paths[0]); i++) {
		if(!file_exists(om->bootif_, om->devpart_, ini_paths[i], om->fs_type_)) {
			printf("ini is not found on %s %s, try searching now\n", om->bootif_, om->devpart_);
			for(j = 0; j < sizeof(devparts)/sizeof(devparts[0]); j++) {
				if(file_exists(om->bootif_, devparts[j], ini_paths[i], om->fs_type_)) {
					printf("found ini on %s %s.\n", om->bootif_,devparts[j]);
					om->devpart_ = devparts[j];
					found = true;
					break;
				}
			}
		}else {
			found = true;
		}
		if(found) {
			om->ini_path_ = ini_paths[i];
			break;
		}
	}
	/* Factory eMMC has no ini; keep its existing legacy boot and LCD logo. */
	if (!found) {
		printf("osl: no ini was found in %s\n", bootdev);
		return -ENOENT;
	}
    ret = osl_read_boot_ini(om, om->bootif_, om->devpart_, om->fs_type_, om->ini_path_);
    if(ret != 0) {
		ndb_enable_lcd("osl: read ini failed, using defaults", ret);
		if(om->boot_efi_)
			om->krnl_ = "/efi/boot/bootaa64.efi";
		// else
		// 	om->krnl_ = om->kernel_ver_ == 6 ? "/Image-6x":"/Image";
		// BUGBUG above line will overwrite settings prepared. 2025-5-23
		// dtb_ is assigned in prepare step, initramfs is unnecessary.
	}
	// step 2.1 check file existence and try adjustment 
	ret = osl_check_kernel(om);
	if(ret < 0) {
		ndb_enable_lcd("osl: missing kernel image, failed", ret);
		return ret;
	}

    // step 3: load kernel
    cmd = cmd_buffer;
    if(strstr(om->krnl_, ".gz")){ 
        n = snprintf(cmd, left, "load %s %s ${loadaddr_} %s", om->bootif_, 
            om->devpart_, om->krnl_);
        osl_push_cmd(om, cmd);
        osl_push_cmd(om, "unzip ${loadaddr_} ${loadaddr}");
    } else {
        n = snprintf(cmd, left, "load %s %s ${loadaddr} %s", om->bootif_, 
            om->devpart_, om->krnl_);
        osl_push_cmd(om, cmd);
    }
    left -= (n+1);
    cmd += n+1;

    // step 4: load initrd
    if(om->initramfs_ != NULL){
        n = snprintf(cmd, left, "load %s %s ${initrd_addr} %s", om->bootif_, 
            om->devpart_, om->initramfs_);
        om->initramfs_cmd_ = osl_push_cmd(om, cmd);         
        left -= (n+1);
        cmd += n+1;        
    }

    // step 5: load device tree
    n = snprintf(cmd, left, "load %s %s ${fdt_addr_r} %s", om->bootif_, 
        om->devpart_, om->dtb_);
    osl_push_cmd(om, cmd);
    left -= (n+1);
    cmd += n+1;        

    // step 6: set rootfs and other env as needed
	if(om->root_)
    	osl_set_rootfs(om, om->root_);

    // step 7: run commad list and booti command now
    ret = osl_exec_boot_cmds(om);

    return ret;
}

static bool osl_menu_entry_exist(osl_mgr* om, const char * mi_value)
{
	int i;
	char mi[16], *mv;

	for (i = 0; i<om->menu_item_count_; i++ ){
		snprintf(mi, sizeof(mi), "bootmenu_%d", i);
		mv = env_get(mi);
		if(mv && strcmp(mv, mi_value) == 0)
			return true;
	}

	return false;
}

static int osl_fed_section(osl_mgr* om, const char* bootentry, const char* ifname, const char* devpart)
{
	char mi[16], mv[200];
	
	snprintf(mv, sizeof(mv), "%s on %s %s=bootyl %s %s %s", bootentry, ifname, devpart, ifname, devpart, bootentry);
	if(osl_menu_entry_exist(om, mv)) 
		return 1;

	snprintf(mi, sizeof(mi), "bootmenu_%d", om->menu_item_count_++);
	
	env_set(mi, mv);
	printf("*mi%d: %s %s\n", om->menu_item_count_ - 1, mi, mv);

	return 0;
}

static int osl_file_menu(osl_mgr* om, const char* ifname, const char* devpart)
{
	int ret;

    ret = osl_read_file(om, ifname, devpart, FS_TYPE_ANY, ini_paths[0]);
    if(ret != 0) {
		ret = osl_read_file(om, ifname, devpart, FS_TYPE_ANY, ini_paths[1]);
	}
	if(ret == 0)
		osl_scan_section_headers(om, om->ini_buffer_, om->ini_buffer_ + om->ini_bytes_, osl_fed_section, ifname, devpart);	

	return 0;
}
static int osl_file_d4d_menu(osl_mgr* om)
{
	char mi[16];

	if(osl_menu_entry_exist(om, "MaskROM=rbrom")) 
		return 1;

	snprintf(mi, sizeof(mi), "bootmenu_%d", om->menu_item_count_++);
	
	env_set(mi, "MaskROM=rbrom");
	
	return 0;
}

/*
 * round 3c: before the autoboot countdown, power the laptop keyboard, start USB once (no usb reset here, a repeated
 * usb reset panicked this U-Boot) and turn the screen console on, so Esc works from the keyboard as well as from
 * serial and the countdown shows on the panel.
 * round 3d: the boot logo stays 1 s before the screen console takes over (the operator: "show logo for 1s is already
 * enough"); the keyboard gets that second to power up before usb start.
 */
void osl_countdown_input(void)
{
	run_command("gpio set 112", 0); // KEY_PWREN GPIO3_C0
	mdelay(1000);
	ndb_enable_lcd("countdown", 0);
	run_command("usb start", 0);
	if (gd->flags & GD_FLG_USBKBD_REGISTERED)
		env_set("stdin", "serial,usbkbd");
	else
		puts("osl: no USB keyboard yet, Esc works from serial\n");
}

// prepare and show boot menu 
static int osl_show_bootmenu(osl_mgr* om, int bootdelay)
{
	int max_usbkbd_retries = 0;

	// turn on usb keyboad firstly 
	run_command("gpio set 112", 0); // KEY_PWREN GPIO3_C0

	// turn on local lcd
	ndb_enable_lcd("bootmenu", 0);

	// show a notice firstly
	puts("Preparing boot menus now. Please wait a moment...");
	
	// prepare menu items for known storage
	run_command_list("pci enum;nvme scan", -1, 0);
	osl_file_menu(om, "nvme", "0:1");
	osl_file_menu(om, "nvme", "0:2");// since hanyang with swap on	
	osl_file_menu(om, "mmc", "0:2");

	// start usb
	run_command("usb start", 0);
	osl_file_menu(om, "usb", "0:1");

	// round 3c: at most one usb reset (was 10); a repeated usb reset panicked this U-Boot on 2026-10-09
	while ( (gd->flags & GD_FLG_USBKBD_REGISTERED) == 0 && max_usbkbd_retries++ < 1) {
		run_command("usb reset", 0);
	}
	if((gd->flags & GD_FLG_USBKBD_REGISTERED) == 0) {
		printf("USB keyboard is absent after %d retries.\n"
			"You may need serial console via NTP or SD-Serial kit.\n", max_usbkbd_retries);
		mdelay(5000);
	} else {
		// set usbkd as stdin
		env_set("stdin", "serial,usbkbd");
	}
	osl_file_d4d_menu(om);

	// 
	//run_command("bootmenu 30", 0);
	bootmenu_show(bootdelay);

	return 0;
}

int menu_show(int bootdelay)
{
	osl_show_bootmenu(&omgr, bootdelay);
	return -1; /* -1 - abort boot and run monitor code */
}
static int do_bootyl(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	int ret = -1;
	const char* bootdev = "nvme"; // default
	const char *ini_section = NULL;
 	int reg_boot = osl_get_boot_mode_register();

	if(osl_is_usb_recovery()) {
		puts("*** OSL detected USB update flag. Booting from usb now...");
		ret = boot_from_usb(NEED_RETRY);
		if(ret == 0)
			goto TAG_RETURN_SUCCESS;
	} else if(reg_boot == BOOT_CONSOLE) { // in case hotkey not work
		cli_loop();
	}

	if( (argc>1 && strcmp(argv[1], "menu")== 0) )  {
		ret = menu_show(30);
		return -1; // return to shell	   
    }
	else if (argc > 1) {
        bootdev = argv[1];
		// osl_set_boot_stor(argv[1]);
	} else {
		 bootdev = env_get("bootos"); // get boot option from reboot modes 
	}
	printf("booting ulan from %s device now\n", bootdev);

	if(argc > 2) {
		/* Keep the factory "bootyl nvme krnl6" form alongside the
		 * current "bootyl nvme 0:1 krnl6" form. Partition selectors
		 * start with a device number, ':' or '#'.
		 */
		if (argc == 3 && (argv[2][0] < '0' || argv[2][0] > '9') &&
		    argv[2][0] != ':' && argv[2][0] != '#') {
			omgr.devpart_ = "0:1";
			ini_section = argv[2];
		} else {
			omgr.devpart_ = argv[2];
		}
	} else 
		omgr.devpart_ = "0:1"; //default

	if(argc > 3)
		ini_section = argv[3];
	if (ini_section) {
		omgr.ini_section_ = ini_section;
		if(strcmp(omgr.ini_section_, "efi") == 0)
			omgr.boot_efi_ = 1;	
		if(strstr(ini_section, "nullp") != NULL) {
			*(int*)(long)ini_section[0] = 0x888; // do a nullp for debug the debugger
		}
	} 
	printf("osl: booting from %s %s\n", bootdev, omgr.devpart_);

    // try the ini directed way firstly since 2024-11-26
#ifdef GD_DEV_OSL 	
	if(*bootdev == 'n')// debug phase, only use for nvme boot
#endif	
    	ret = osl_boot_by_ini(&omgr, bootdev);
    if(ret != 0) {
		if (ret == -ENOENT && !strcmp(bootdev, "emmc"))
			printf("osl: using factory eMMC defaults\n");
		else
			ndb_enable_lcd("osl: iniboot failed", ret);
		printf("osl: try legacy boot from %s now\n", bootdev);
        ret = osl_boot_device_legacy(bootdev, omgr.devpart_, omgr.ini_section_);
        if ((ret != 0) && bootdev!= NULL && (*bootdev != 's') && !swd_on_mmc_enabled) {
            printf("osl: last try to boot from sdcard !!!\n");
            ret = boot_from_sdcard(UNNEED_RETRY);
        }    
    }
	if (ret == 0) {
		goto TAG_RETURN_SUCCESS;
	}

	// let user choose
	ret = osl_show_bootmenu(&omgr, 30);
	if(ret == 0)
		goto TAG_RETURN_SUCCESS;

	printf("Fail to boot!!!Will reset in 30 seconds...\n");
	mdelay(30000); // pause 30 seconds for user to take a snapshot 
	writel(BOOT_MODE_UNDEFINE, (void *)CONFIG_ROCKCHIP_BOOT_MODE_REG);
	run_command("mw.b 0x10000000 0 0x04000000", 0);
	mdelay(1000);
	run_command_list("reset", -1, 0);

	return 1;

TAG_RETURN_SUCCESS:
	return 0;
}

U_BOOT_CMD(bootyl, 4, 1, do_bootyl,
        "boot yourland images",
        "bootyl menu\n"
		"bootyl nvme|emmc|sdc|usb [krnl5|krnl6|efi|other section name of ini]\n"
		"bootyl nvme|emmc|sdc|usb [devpart] [krnl5|krnl6|efi|other section name of ini]\n"
);
