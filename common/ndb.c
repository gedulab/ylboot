#ifndef CONFIG_SANDBOX
#include <common.h>
#include <console.h>
#include <asm-generic/gpio.h>
#include <asm/io.h>
#include <version.h>
#include <asm/arch/boot_mode.h>
#include "ndb.h"
#include <stdio.h>

bool swd_on_mmc_enabled = false;

static int ndb_enable_sdmmc_uart2(void)
{
	int value;

	value = readl(RK3588_BUS_IOC_BASE + RK3588_BUS_IOC_GPIO4D_IOMUX_SEL_L);
	printf("Current value of RK3588_BUS_IOC_GPIO4D_IOMUX_SEL_L: %x\n", value);
	// gpio4d1u: 4'h1
	value |= GRF_HIWORD_UPDATE(0, 1, 4);	// bit 4 0
	value |= GRF_HIWORD_UPDATE(1, 1, 5);	// bit 5 1
	value |= GRF_HIWORD_UPDATE(0, 1, 6);	// bit 6 0
	value |= GRF_HIWORD_UPDATE(1, 1, 7);	// bit 7 1
	// gpio4d0u: 4'h1
	value |= GRF_HIWORD_UPDATE(0, 1, 0);	// bit 0 0
	value |= GRF_HIWORD_UPDATE(1, 1, 1);	// bit 1 1
	value |= GRF_HIWORD_UPDATE(0, 1, 2);	// bit 2 0
	value |= GRF_HIWORD_UPDATE(1, 1, 3);	// bit 3 1
	printf("IOMUX is set to: %x\n", value);
	writel(value, RK3588_BUS_IOC_BASE + RK3588_BUS_IOC_GPIO4D_IOMUX_SEL_L);

	value = readl(RK3588_BUS_IOC_BASE + RK3588_BUS_IOC_GPIO4D_IOMUX_SEL_L);
	printf("RK3588_BUS_IOC_GPIO4D_IOMUX_SEL_L = %x\n", value);

	return value;
}

static int ndb_check_sdmmc_cd_via_gpio(void)
{
	int value, ret;

	ret = gpio_request(RK_GPIO0_A4, "gpio0_a4");
	if (ret < 0) {
		printf("Request gpio0_a4 failed: %d\n", ret);

		return 1;
	}

	value = gpio_get_value(RK_GPIO0_A4);
	printf("sdmmc_cd gpio0_a4_u get value: %d\n", value);
	gpio_free(RK_GPIO0_A4);

	return value;
}

// enable console output on local display
void ndb_enable_lcd(const char* msg, int errcode)
{
	if(0 == (gd->flags & GD_FLG_SERIAL_READY)) {
		console_show_early_log(); // round 3d: first what U-Boot printed so far, on the screen only
		gd->flags |= GD_FLG_SERIAL_READY; // turn on local display via a bug of u-boot, see putc in console.c
		printf("Hi geuder, local console is enabled for %s (err %d).\n", msg, errcode);
	} else {
		// printf("%s (err %d).\n", msg, errcode);
		// already enabled, ignore. Raymond 2026-10-29
	}
}

/*static void ndb_ndb_tag_check(void)
{
	int value;

	value = readl(RK3588_IRAM_BASE + YL_NDB_NOTE_OFFSET);
	printf("NDB tag = 0x%x\n", value);
	if(value >= NDB_NOTE_KERNEL_BASE && value < NDB_NOTE_KERNEL_BASE + 0x10000) {
		if(ndb_breakin(NDB_BRK_IMM_MANUAL, 0, 1) != 0) {
            printf("ndb is waiting for kernel debugger (%d)......\n", value - NDB_NOTE_KERNEL_BASE);
            mdelay(value - NDB_NOTE_KERNEL_BASE);
        }
	}
}*/

/*
 * set the HDE bit of EDSCR
 * HDE, bit [14] Halting debug enable.
 * EDSCR can't be access directly in the PE. According ARM:
 * When OSLSR_EL1.OSLK is 1, this bit can be indirectly read and written through the following System registers:
 * 	MDSCR_EL1.
 *  DBGDSCRext.
 */
#define OSLSR_EL1_OSLK (1<<1)
#define MDSCR_EL1_HDE (1<<14)

static int ndb_enable_halt(void)
{
	unsigned long oslsr, mdscr;
	bool oset = false, mset = false;

	asm volatile("mrs %0, oslsr_el1" : "=r" (oslsr));
	// set the oslk bit(bit 1)
	if( (oslsr & OSLSR_EL1_OSLK) == 0) {
		oslsr |= OSLSR_EL1_OSLK;
		oset = true;
		asm volatile("msr oslsr_el1, %0" : : "r" (oslsr) : "memory");
	}
	asm volatile("mrs %0, mdscr_el1" : "=r" (mdscr));
	if( (mdscr &MDSCR_EL1_HDE) == 0) {
		mdscr |= MDSCR_EL1_HDE;
		mset = true;
		asm volatile("msr mdscr_el1, %0" : : "r" (mdscr) : "memory");		
	}
	if(oset) {
		oslsr &= ~OSLSR_EL1_OSLK;
		asm volatile("msr oslsr_el1, %0" : : "r" (oslsr) : "memory");
	}
	printf("ndb: %lx set %d %lx set %d\n", oslsr, oset, mdscr, mset);

	return 0;
}

static void ndb_enable_debug(void)
{
	int value;

	swd_on_mmc_enabled = false;
	value = ndb_check_sdmmc_cd_via_gpio();

	if (value == 0) {
		value = ndb_enable_sdmmc_uart2();
		swd_on_mmc_enabled = true;

		printf("UART and NTP debug via SD slot are enabled(%x).\n%s\n"
			"You can enter u-boot shell by typing [11] repeatedly.\n", value, version_string);
		
		// ndb_enable_halt(); // cause exception when reboot
		// printf("NDB flag is being checked\n");
		// ndb_ndb_tag_check();
	} else {
		printf("ulan SD slot is empty(0x%x).\n", value);	
	}
}

char* ulan_krnl_breakin_cmd[] = {
	"",
	"kd=start "//"ylkrnl_breakin=1 "
};
bool  krnl_need_breakin;

static void ndb_boot_mode_check(void)
{
	uint32_t boot_mode;

	boot_mode = readl((void *)CONFIG_ROCKCHIP_BOOT_MODE_REG);
	printf("ulan boot mode 0x%x\n", boot_mode);
	krnl_need_breakin = false;
	gd->reg_boot = boot_mode - REBOOT_FLAG;

	switch (boot_mode) {
	case BOOT_CONSOLE:
		printf("received console request %d.\n", gd->reg_boot);
		gd->console_evt = 1; /* CTRL_A not work yet */
		break;
	case BOOT_PANIC:
		panic("good code is achived by debug");
		break;
	case BOOT_UDBG:
		printf("breakin on uboot %x\n", boot_mode);
		if (ndb_breakin(NDB_BRK_UIMM_INIT, 0, 1) != 0) {
			puts("ndb auto breakin failed");
		}
		puts("ndb auto breakin succeed");
		break;
	default:
		break;
	}
}

int ndb_debug_init(void)
{
	ndb_enable_debug();

	ndb_boot_mode_check();

	return 0;
}


#endif // !CONFIG_SANDBOX
