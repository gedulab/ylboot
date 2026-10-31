/*
 * Copyright (C) 2012 Samsung Electronics
 * Rajeshwari Shinde <rajeshwari.s@samsung.com>
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <common.h>
#include <command.h>
#include <fdtdec.h>
#include <sound.h>

#define YL_TRY_OLD_CODE

DECLARE_GLOBAL_DATA_PTR;

/* Initilaise sound subsystem */
static int do_init(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
	int ret;
    struct udevice *dev;	
#ifdef YL_TRY_OLD_CODE
	ret = sound_init(gd->fdt_blob);
	if (ret) {
		printf("Initialise Audio driver failed %d\n", ret);
		return CMD_RET_FAILURE;
	}
	// ret = i2s_sound_init(gd->fdt_blob);
	if (ret) {
		printf("Initialise i2s driver failed\n");
		return CMD_RET_FAILURE;
	}	
#else
    ret = uclass_first_device_err(UCLASS_SOUND, &dev);
    if (ret) {
        printf("Initialise Audio driver failed (ret=%d)\n", ret);
        return CMD_RET_FAILURE;
    }

    // 调用 sound_setup 完成驱动的初始化配置
    ret = sound_setup(dev);
    if (ret) {
        printf("Sound setup failed (ret=%d)\n", ret);
        return CMD_RET_FAILURE;
    }
#endif	
	return 0;
}
static int do_play(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
	int ret;

	ret = sound_play(0, 0);
	if (ret) {
		printf("play wav failed\n");
		ret = CMD_RET_FAILURE;
	}	
	return ret;
}
/* play sound from buffer */
static int do_beep(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
	int ret = 0;
    struct udevice *dev;

	int msec = 1000;
	int freq = 400;

	if (argc > 1)
		msec = simple_strtoul(argv[1], NULL, 10);
	if (argc > 2)
		freq = simple_strtoul(argv[2], NULL, 10);
#ifdef YL_TRY_OLD_CODE
    ret = sound_play(msec, freq);
	// ret = i2s_sound_play(msec, freq);
	if (ret) {
		printf("play failed %d\n", ret);
		return CMD_RET_FAILURE;
	}
#else
    ret = uclass_first_device_err(UCLASS_SOUND, &dev);
    if (ret) {
        printf("Cannot find sound device (err=%d)\n", ret);
        return CMD_RET_FAILURE;
    }

    // 使用驱动模型提供的 sound_beep 接口
    ret = sound_beep(dev, msec, freq);
    if (ret) {
        printf("Sound device failed to play (err=%d)\n", ret);
        return CMD_RET_FAILURE;
    }
#endif

	return 0;
}

static cmd_tbl_t cmd_sound_sub[] = {
	U_BOOT_CMD_MKENT(init, 0, 1, do_init, "", ""),
	U_BOOT_CMD_MKENT(beep, 2, 1, do_beep, "", ""),
	U_BOOT_CMD_MKENT(play, 0, 1, do_play, "", ""),
};

/* process sound command */
static int do_sound(cmd_tbl_t *cmdtp, int flag, int argc, char *const argv[])
{
	cmd_tbl_t *c;

	if (argc < 1)
		return CMD_RET_USAGE;

	/* Strip off leading 'sound' command argument */
	argc--;
	argv++;

	c = find_cmd_tbl(argv[0], &cmd_sound_sub[0], ARRAY_SIZE(cmd_sound_sub));

	if (c)
		return c->cmd(cmdtp, flag, argc, argv);
	else
		return CMD_RET_USAGE;
}

U_BOOT_CMD(
	sound, 4, 1, do_sound,
	"sound sub-system",
	"init - initialise the sound driver\n"
	"beep [len] [freq] - play a sound for len ms at freq hz\n"
	"play - play a wav file\n"
);
