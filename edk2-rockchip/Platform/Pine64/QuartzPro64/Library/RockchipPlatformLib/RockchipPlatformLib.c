/** @file
*
*  Copyright (c) 2021, Rockchip Limited. All rights reserved.
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

#include <Base.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/GpioLib.h>
#include <Library/RK806.h>
#include <Library/Rk3588Pcie.h>
#include <Library/PWMLib.h>
#include <Soc.h>
#include <VarStoreData.h>

// vdd_cpu_lit_mem_s0 floor and regulator-coupled-max-spread, both from the dts.
#define VDD_CPU_LIT_MEM_MIN_UV       675000
#define RK806_COUPLED_MAX_SPREAD_UV  10000

// From DTB as starting point, &spi2/pmic@0 (master) and pmic@1 (slave).
static struct regulator_init_data  rk806_init_data[] = {
  /* Master PMIC */
  // Range rails - SPL leaves these at nominal 750000; forcing the floor hangs Linux.
  // RK8XX_VOLTAGE_INIT (MASTER_BUCK1,  675000),  // vdd_gpu_s0
  // RK8XX_VOLTAGE_INIT (MASTER_BUCK2,  550000),  // vdd_npu_s0
  // RK8XX_VOLTAGE_INIT (MASTER_BUCK3,  675000),  // vdd_log_s0
  // RK8XX_VOLTAGE_INIT (MASTER_BUCK4,  550000),  // vdd_vdenc_s0
  // RK8XX_VOLTAGE_INIT (MASTER_BUCK5,  675000),  // vdd_gpu_mem_s0
  // RK8XX_VOLTAGE_INIT (MASTER_BUCK6,  675000),  // vdd_npu_mem_s0
  // RK8XX_VOLTAGE_INIT (MASTER_BUCK8,  675000),  // vdd_vdenc_mem_s0
  RK8XX_VOLTAGE_INIT (MASTER_BUCK7,  2000000), // vcc_2v0_pldo_s3
  RK8XX_VOLTAGE_INIT (MASTER_BUCK10, 1100000), // vcc_1v1_nldo_s3

  RK8XX_VOLTAGE_INIT (MASTER_PLDO1, 1800000), // avcc_1v8_s0
  RK8XX_VOLTAGE_INIT (MASTER_PLDO2, 1800000), // vdd1_1v8_ddr_s3
  RK8XX_VOLTAGE_INIT (MASTER_PLDO3, 1800000), // avcc_1v8_codec_s0
  RK8XX_VOLTAGE_INIT (MASTER_PLDO4, 3300000), // vcc_3v3_s3
  RK8XX_VOLTAGE_INIT (MASTER_PLDO5, 3300000), // vccio_sd_s0 (switched 1.8V/3.3V by the SD driver, default high)
  RK8XX_VOLTAGE_INIT (MASTER_PLDO6, 1800000), // vcc_1v8_s3

  RK8XX_VOLTAGE_INIT (MASTER_NLDO1, 750000), // vdd_0v75_s3
  RK8XX_VOLTAGE_INIT (MASTER_NLDO2, 900000), // vdd2l_0v9_ddr_s3
  RK8XX_VOLTAGE_INIT (MASTER_NLDO3, 750000), // vdd_0v75_hdmi_edp_s0
  RK8XX_VOLTAGE_INIT (MASTER_NLDO4, 750000), // avdd_0v75_s0
  RK8XX_VOLTAGE_INIT (MASTER_NLDO5, 850000), // vdd_0v85_s0

  // *_mem_s0 rails are coupled to their core rail (10mV max spread); raise them first.
  RK8XX_VOLTAGE_INIT (SLAVER_BUCK5, 1000000), // vdd_cpu_big1_mem_s0
  RK8XX_VOLTAGE_INIT (SLAVER_BUCK6, 1000000), // vdd_cpu_big0_mem_s0
  RK8XX_VOLTAGE_INIT (SLAVER_BUCK1, 1000000), // vdd_cpu_big1_s0
  RK8XX_VOLTAGE_INIT (SLAVER_BUCK2, 1000000), // vdd_cpu_big0_s0
  RK8XX_VOLTAGE_INIT (SLAVER_BUCK4, 3300000), // vcc_3v3_s0
  RK8XX_VOLTAGE_INIT (SLAVER_BUCK7, 1800000), // vcc_1v8_s0
  // RK8XX_VOLTAGE_INIT (SLAVER_BUCK10, 675000), // vdd_ddr_s0 - range rail, u-boot never touches, so SKIP

  RK8XX_VOLTAGE_INIT (SLAVER_PLDO1, 1800000), // vcc_1v8_cam_s0
  RK8XX_VOLTAGE_INIT (SLAVER_PLDO2, 1800000), // avdd1v8_ddr_pll_s0
  RK8XX_VOLTAGE_INIT (SLAVER_PLDO3, 1800000), // vdd_1v8_pll_s0
  RK8XX_VOLTAGE_INIT (SLAVER_PLDO4, 3300000), // vcc_3v3_sd_s0
  RK8XX_VOLTAGE_INIT (SLAVER_PLDO5, 2800000), // vcc_2v8_cam_s0
  RK8XX_VOLTAGE_INIT (SLAVER_PLDO6, 1800000), // pldo6_s3

  RK8XX_VOLTAGE_INIT (SLAVER_NLDO1, 750000), // vdd_0v75_pll_s0
  RK8XX_VOLTAGE_INIT (SLAVER_NLDO2, 850000), // vdd_ddr_pll_s0
  RK8XX_VOLTAGE_INIT (SLAVER_NLDO3, 850000), // avdd_0v85_s0
  RK8XX_VOLTAGE_INIT (SLAVER_NLDO4, 1200000), // avdd_1v2_cam_s0
  RK8XX_VOLTAGE_INIT (SLAVER_NLDO5, 1200000), // avdd_1v2_s0
};

VOID
EFIAPI
SdmmcIoMux (
  VOID
  )
{
  /* sdmmc0 iomux (microSD socket) */
  BUS_IOC->GPIO4D_IOMUX_SEL_L  = (0xFFFFUL << 16) | (0x1111); // SDMMC_D0,SDMMC_D1,SDMMC_D2,SDMMC_D3
  BUS_IOC->GPIO4D_IOMUX_SEL_H  = (0x00FFUL << 16) | (0x0011); // SDMMC_CLK,SDMMC_CMD
  PMU1_IOC->GPIO0A_IOMUX_SEL_H = (0x000FUL << 16) | (0x0001); // SDMMC_DET
}

VOID
EFIAPI
SdhciEmmcIoMux (
  VOID
  )
{
  /* sdhci0 iomux (eMMC socket) */
  BUS_IOC->GPIO2A_IOMUX_SEL_L = (0xFFFFUL << 16) | (0x1111); // EMMC_CMD,EMMC_CLKOUT,EMMC_DATASTROBE,EMMC_RSTN
  BUS_IOC->GPIO2D_IOMUX_SEL_L = (0xFFFFUL << 16) | (0x1111); // EMMC_D0,EMMC_D1,EMMC_D2,EMMC_D3
  BUS_IOC->GPIO2D_IOMUX_SEL_H = (0xFFFFUL << 16) | (0x1111); // EMMC_D4,EMMC_D5,EMMC_D6,EMMC_D7
}

VOID
EFIAPI
EdpEnableBacklight (
  IN UINT32   Id,
  IN BOOLEAN  Enable
  )
{
}

#define NS_CRU_BASE       0xFD7C0000
#define CRU_CLKSEL_CON59  0x03EC
#define CRU_CLKSEL_CON78  0x0438

VOID
EFIAPI
Rk806SpiIomux (
  VOID
  )
{
  /* io mux */
  PMU1_IOC->GPIO0A_IOMUX_SEL_H = (0x0FF0UL << 16) | 0x0110;
  PMU1_IOC->GPIO0B_IOMUX_SEL_L = (0xF0FFUL << 16) | 0x1011;
  MmioWrite32 (NS_CRU_BASE + CRU_CLKSEL_CON59, (0x00C0UL << 16) | 0x0080);
}

VOID
EFIAPI
Rk806Configure (
  VOID
  )
{
  UINTN  RegCfgIndex;

  RK806Init ();

  RK806PinSetFunction (MASTER, 1, 2); // rk806_dvs1_pwrdn

  for (RegCfgIndex = 0; RegCfgIndex < ARRAY_SIZE (rk806_init_data); RegCfgIndex++) {
    RK806RegulatorInit (rk806_init_data[RegCfgIndex]);
  }
}

VOID
EFIAPI
SetCPULittleVoltage (
  IN UINT32  Microvolts
  )
{
  UINT32  MemMicrovolts = MAX (Microvolts, VDD_CPU_LIT_MEM_MIN_UV);

  if (Microvolts + RK806_COUPLED_MAX_SPREAD_UV < MemMicrovolts) {
    Microvolts = MemMicrovolts - RK806_COUPLED_MAX_SPREAD_UV;
  }

  struct regulator_init_data  Rk806CpuLittleMemSupply =
    RK8XX_VOLTAGE_INIT (SLAVER_BUCK8, MemMicrovolts);
  struct regulator_init_data  Rk806CpuLittleSupply =
    RK8XX_VOLTAGE_INIT (SLAVER_BUCK3, Microvolts);

  RK806RegulatorInit (Rk806CpuLittleMemSupply);
  RK806RegulatorInit (Rk806CpuLittleSupply);
}

VOID
EFIAPI
NorFspiIomux (
  VOID
  )
{
}

VOID
EFIAPI
NorFspiEnableClock (
  UINT32  *CruBase
  )
{
  UINTN  BaseAddr = (UINTN)CruBase;

  MmioWrite32 (BaseAddr + 0x087C, 0x0E000000);
}

VOID
EFIAPI
GmacIomux (
  IN UINT32  Id
  )
{
  switch (Id) {
    case 0:
      /* gmac0 iomux (RGMII to RTL8211F) */
      BUS_IOC->GPIO2A_IOMUX_SEL_H = (0xFF00UL << 16) | 0x1100; // GMAC0_RXD2, GMAC0_RXD3
      BUS_IOC->GPIO2B_IOMUX_SEL_L = (0xFFFFUL << 16) | 0x1111; // GMAC0_RXCLK, GMAC0_TXD2, GMAC0_TXD3, GMAC0_TXCLK
      BUS_IOC->GPIO2B_IOMUX_SEL_H = (0xFF00UL << 16) | 0x1100; // GMAC0_TXD0, GMAC0_TXD1
      BUS_IOC->GPIO2C_IOMUX_SEL_L = (0x0FFFUL << 16) | 0x0111; // GMAC0_TXEN, GMAC0_RXD0, GMAC0_RXD1
      BUS_IOC->GPIO4C_IOMUX_SEL_L = (0xFF00UL << 16) | 0x1100; // GMAC0_RXDV_CRS, GMAC0_MCLKINOUT
      BUS_IOC->GPIO4C_IOMUX_SEL_H = (0x00FFUL << 16) | 0x0011; // GMAC0_MDC, GMAC0_MDIO

      /* phy0 reset */
      GpioPinSetDirection (4, GPIO_PIN_PB3, GPIO_PIN_OUTPUT);
      break;
    default:
      break;
  }
}

VOID
EFIAPI
GmacIoPhyReset (
  IN UINT32   Id,
  IN BOOLEAN  Enable
  )
{
  switch (Id) {
    case 0:
      /* phy0 reset - active low, gpio4 PB3 */
      GpioPinWrite (4, GPIO_PIN_PB3, !Enable);
      break;
    default:
      break;
  }
}

VOID
EFIAPI
I2cIomux (
  UINT32  id
  )
{
  switch (id) {
    case 2:
      /* HYM8563 RTC */
      GpioPinSetFunction (0, GPIO_PIN_PB7, 9); // i2c2_scl_m0
      GpioPinSetFunction (0, GPIO_PIN_PC0, 9); // i2c2_sda_m0
      break;
    case 7:
      /* ES8388 audio codec */
      GpioPinSetFunction (1, GPIO_PIN_PD0, 9); // i2c7_scl_m0
      GpioPinSetFunction (1, GPIO_PIN_PD1, 9); // i2c7_sda_m0
      break;
    default:
      break;
  }
}

VOID
EFIAPI
UsbPortPowerEnable (
  VOID
  )
{
  DEBUG ((DEBUG_INFO, "UsbPortPowerEnable called\n"));
  /* vcc5v0_host - gpio4 PB0 output high to power USB host ports */
  GpioPinWrite (4, GPIO_PIN_PB0, TRUE);
  GpioPinSetDirection (4, GPIO_PIN_PB0, GPIO_PIN_OUTPUT);
}

VOID
EFIAPI
Usb2PhyResume (
  VOID
  )
{
  MmioWrite32 (0xfd5d0008, 0x20000000);
  MmioWrite32 (0xfd5d4008, 0x20000000);
  MmioWrite32 (0xfd5d8008, 0x20000000);
  MmioWrite32 (0xfd5dc008, 0x20000000);
  MmioWrite32 (0xfd7f0a10, 0x07000700);
  MmioWrite32 (0xfd7f0a10, 0x07000000);
}

VOID
EFIAPI
PcieIoInit (
  UINT32  Segment
  )
{
  /* Set reset and power IO to gpio output mode */
  switch (Segment) {
    case PCIE_SEGMENT_PCIE30X4:
      GpioPinSetDirection (4, GPIO_PIN_PB6, GPIO_PIN_OUTPUT); // reset
      GpioPinSetDirection (3, GPIO_PIN_PC3, GPIO_PIN_OUTPUT); // vcc3v3_pcie30 enable
      break;
    case PCIE_SEGMENT_PCIE20L0: // wifi/bt (AP6275PR3)
      GpioPinSetDirection (4, GPIO_PIN_PA5, GPIO_PIN_OUTPUT); // reset
      GpioPinSetDirection (3, GPIO_PIN_PB1, GPIO_PIN_OUTPUT); // vcc3v3_wf enable
      break;
    case PCIE_SEGMENT_PCIE20L1: // rtl8111hs
      GpioPinSetDirection (4, GPIO_PIN_PA2, GPIO_PIN_OUTPUT); // reset
      GpioPinSetFunction (1, GPIO_PIN_PA4, 0); // isolateb, active low
      GpioPinSetDirection (1, GPIO_PIN_PA4, GPIO_PIN_OUTPUT);
      break;
    default:
      break;
  }
}

VOID
EFIAPI
PciePowerEn (
  UINT32   Segment,
  BOOLEAN  Enable
  )
{
  /* output high to enable power */
  switch (Segment) {
    case PCIE_SEGMENT_PCIE30X4:
      GpioPinWrite (3, GPIO_PIN_PC3, Enable);
      break;
    case PCIE_SEGMENT_PCIE20L0:
      GpioPinWrite (3, GPIO_PIN_PB1, Enable);
      break;
    case PCIE_SEGMENT_PCIE20L1:
      GpioPinWrite (1, GPIO_PIN_PA4, Enable);
      break;
    default:
      break;
  }
}

VOID
EFIAPI
PciePeReset (
  UINT32   Segment,
  BOOLEAN  Enable
  )
{
  switch (Segment) {
    case PCIE_SEGMENT_PCIE30X4:
      GpioPinWrite (4, GPIO_PIN_PB6, !Enable);
      break;
    case PCIE_SEGMENT_PCIE20L0:
      GpioPinWrite (4, GPIO_PIN_PA5, !Enable);
      break;
    case PCIE_SEGMENT_PCIE20L1:
      GpioPinWrite (4, GPIO_PIN_PA2, !Enable);
      break;
    default:
      break;
  }
}

VOID
EFIAPI
HdmiTxIomux (
  IN UINT32  Id
  )
{
  switch (Id) {
    case 0:
      GpioPinSetFunction (4, GPIO_PIN_PC1, 5); // hdmim0_tx0_cec
      GpioPinSetPull (4, GPIO_PIN_PC1, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (1, GPIO_PIN_PA5, 5); // hdmim0_tx0_hpd
      GpioPinSetPull (1, GPIO_PIN_PA5, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (4, GPIO_PIN_PB7, 5); // hdmim0_tx0_scl
      GpioPinSetPull (4, GPIO_PIN_PB7, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (4, GPIO_PIN_PC0, 5); // hdmim0_tx0_sda
      GpioPinSetPull (4, GPIO_PIN_PC0, GPIO_PIN_PULL_NONE);
      break;
    case 1:
      GpioPinSetFunction (3, GPIO_PIN_PC4, 5); // hdmim2_tx1_cec
      GpioPinSetPull (3, GPIO_PIN_PC4, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (1, GPIO_PIN_PA6, 5); // hdmim0_tx1_hpd
      GpioPinSetPull (1, GPIO_PIN_PA6, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (3, GPIO_PIN_PC6, 5); // hdmim1_tx1_scl
      GpioPinSetPull (3, GPIO_PIN_PC6, GPIO_PIN_PULL_NONE);
      GpioPinSetFunction (3, GPIO_PIN_PC5, 5); // hdmim1_tx1_sda
      GpioPinSetPull (3, GPIO_PIN_PC5, GPIO_PIN_PULL_NONE);
      break;
  }
}

VOID
EFIAPI
PwmFanIoSetup (
  VOID
  )
{
  // Deferred to Phase 2 (PWM fan header bring-up).
}

VOID
EFIAPI
PwmFanSetSpeed (
  IN UINT32  Percentage
  )
{
  // Deferred to Phase 2 (PWM fan header bring-up).
}

VOID
EFIAPI
PlatformInitLeds (
  VOID
  )
{
  /* Status indicator - gpio3 PB7, active high */
  GpioPinWrite (3, GPIO_PIN_PB7, FALSE);
  GpioPinSetDirection (3, GPIO_PIN_PB7, GPIO_PIN_OUTPUT);
}

VOID
EFIAPI
PlatformSetStatusLed (
  IN BOOLEAN  Enable
  )
{
  GpioPinWrite (3, GPIO_PIN_PB7, Enable);
}

CONST EFI_GUID *
EFIAPI
PlatformGetDtbFileGuid (
  IN UINT32  CompatMode
  )
{
  STATIC CONST EFI_GUID  VendorDtbFileGuid = {
    // DeviceTree/Vendor.inf
    0xd97bc13d, 0x7f3d, 0x48ba, { 0xa4, 0xd1, 0x18, 0xd1, 0xe0, 0x2b, 0xd0, 0x60 }
  };
  STATIC CONST EFI_GUID  MainlineDtbFileGuid = {
    // DeviceTree/Mainline.inf
    0x5b66776e, 0x52e2, 0x4f23, { 0x8e, 0x85, 0x32, 0x7f, 0xa4, 0x6f, 0xc9, 0xa8 }
  };

  switch (CompatMode) {
    case FDT_COMPAT_MODE_VENDOR:
      return &VendorDtbFileGuid;
    case FDT_COMPAT_MODE_MAINLINE:
      return &MainlineDtbFileGuid;
  }

  return NULL;
}

VOID
EFIAPI
PlatformEarlyInit (
  VOID
  )
{
}
