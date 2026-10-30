#pragma once
/*
 * Board selection: build.sh passes -DBOARD_BERYLLIUM (Poco F1, SDM845) or
 * -DBOARD_SM6375 (OnePlus Nord CE 3 Lite 5G, SM6375 "blair"); the default is
 * sweet (SM7150).
 * Every value comes from the board's live device tree; the SM6375 numbers are
 * captured in docs/SM6375_PORT.md (M0).
 */
#if defined(BOARD_BERYLLIUM)
#define BOARD_NAME		"beryllium"
/* reserved-memory cont_splash_mem@9d400000, simple-framebuffer 1080x2246 */
#define FB_BASE			0x9d400000UL
#define FB_W			1080
#define FB_H			2246
/* sdm845 imem@146bf000, restart_reason@65c */
#define IMEM_RESTART_REASON	0x146bf65cUL
#define PSHOLD			0x0c264000UL
#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define GICR_SIZE		0x100000UL
#define UFS_BASE		0x01d84000UL	/* ufshc@1d84000 */
#elif defined(BOARD_SM6375)
#define BOARD_NAME		"sm6375"
/* reserved-memory splash_region@0x85200000 ("cont_splash_region"), 12 MiB */
#define FB_BASE			0x85200000UL
#define FB_W			1080
#define FB_H			2400
/* blair imem@c125000, restart_reason@65c */
#define IMEM_RESTART_REASON	0xc12565cUL
#define PSHOLD			0x440b000UL	/* restart@440b000, qcom,pshold */
#define GICD_BASE		0xf200000UL	/* interrupt-controller@f200000 */
#define GICR_BASE		0xf240000UL
#define GICR_SIZE		0x100000UL
#define UFS_BASE		0x04804000UL	/* ufshc@4804000 */
#else
#define BOARD_NAME		"sweet"
/* reserved-memory cont_splash_region@9c000000 */
#define FB_BASE			0x9c000000UL
#define FB_W			1080
#define FB_H			2400
/* qcom,msm-imem restart_reason@65c */
#define IMEM_RESTART_REASON	0x146aa65cUL
#define PSHOLD			0x0c264000UL
#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define GICR_SIZE		0x100000UL
#define UFS_BASE		0x01d84000UL	/* ufshc@1d84000 */
#endif

#define FB_STRIDE		(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */
#define FB_PHYS			FB_BASE
