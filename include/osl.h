/*
 OS loader routines with boot ini support etc.
 RaymondZ of GSL 2024-11-26
 */
#ifndef __OSL_H__
#define __OSL_H__

#define MAX_INI_BYTES 4096

// bad code for legacy support
#define USB_RESTORE_FLAG_ADDRESS			0x10000000
#define USB_UPDATE_TAG_VAL	0x12
#define UNNEED_RETRY		0
#define NEED_RETRY			1
#define NVME_BOOT_FAILED	1
#define EMMC_BOOT_FAILED	2
#define USB_UPDATE_FAILED	3
#define SDC_BOOT_FAILED		4

// boot mode, can be changed by powerkey
extern int yl_pwr_btn_click_count; // defined and set in rk8xx_pwrkey.c
#define YL_BOOT_FAST  1 //  - normal boot flow
// 2 - reserved
#define YL_BOOT_DEBUG 3 // - advanced debug mode, enable usb and show boot menu 
// 4 - reserved
#define YL_BOOT_UPDATE 5 // - usb update
//

#define OSL_MAX_BOOT_CMDS 15
enum YL_BOOT_MEDIA  {   
    YL_BOOT_MEDIA_UNKNOWN = 0,
    YL_BOOT_MEDIA_EMMC,
    YL_BOOT_MEDIA_USB,
    YL_BOOT_MEDIA_NVME,
    YL_BOOT_MEDIA_SDCARD,
    YL_BOOT_MEDIA_MAX
};

typedef struct _osl_mgr {
    int tries_; // tries counter
    enum YL_BOOT_MEDIA bootmedia_; // boot media
    const char* bootif_; // boot device
    const char* devpart_; // partition string 
    char* ini_buffer_;
    loff_t ini_bytes_;
    int fs_type_;
    int kernel_ver_; // 5 6, etc
    char* krnl_;
    char* initramfs_;
    char* cmdline_;
    char* dtb_;
    char* root_; // rootfs
    const char* ini_path_; // full ini file path is being used to boot
    const char* env_cmd_; // the run nvmeargs cmd
    const char* ini_section_; // the ini section used for kernel options    
    char* cmds_[OSL_MAX_BOOT_CMDS];
    unsigned int boot_efi_:1;// booting uefi now 
    int total_cmds_;
    int initramfs_cmd_; // index of the initramfs command, need record its size
    int menu_item_count_;
} osl_mgr;

typedef struct _boot_env_info {
	char* device;
	char** envcmds;
	int cmdscnt;
	int initrd_site;
	int* skip_failed;
} boot_env_info;
typedef int (*PFN_INI_FEED_VPAIR)(osl_mgr* om, char* name, char* value);
typedef int (*PFN_INI_FEED_SECTION)(osl_mgr* om, const char* name, const char* ifname, const char* devpart);

#endif // __OSL_H__