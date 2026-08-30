## @file
#
#  Copyright (c) 2014-2018, Linaro Limited. All rights reserved.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = QuartzPro64
  PLATFORM_VENDOR                = Pine64
  PLATFORM_GUID                  = d5b8ad10-febe-4de5-8c95-fc93b5aaaacd
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010019
  OUTPUT_DIRECTORY               = Build/$(PLATFORM_NAME)
  VENDOR_DIRECTORY               = Platform/$(PLATFORM_VENDOR)
  PLATFORM_DIRECTORY             = $(VENDOR_DIRECTORY)/$(PLATFORM_NAME)
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = Silicon/Rockchip/RK3588/RK3588.fdf
  RK_PLATFORM_FVMAIN_MODULES     = $(PLATFORM_DIRECTORY)/$(PLATFORM_NAME).Modules.fdf.inc

  #
  # GMAC0 is exposed via RTL8211F RGMII PHY
  #
  DEFINE RK3588_GMAC_ENABLE = TRUE

  #
  # HYM8563 RTC support
  #
  DEFINE RK_RTC8563_ENABLE = TRUE

  #
  # RK3588-based platform
  #
!include Silicon/Rockchip/RK3588/RK3588Platform.dsc.inc

################################################################################
#
# Library Class section - list of all Library Classes needed by this Platform.
#
################################################################################

[LibraryClasses.common]
  RockchipPlatformLib|$(PLATFORM_DIRECTORY)/Library/RockchipPlatformLib/RockchipPlatformLib.inf

################################################################################
#
# Pcd Section - list of all EDK II PCD Entries defined by this Platform.
#
################################################################################

[PcdsFixedAtBuild.common]
  # SMBIOS platform config
  gRockchipTokenSpaceGuid.PcdPlatformName|"QuartzPro64"
  gRockchipTokenSpaceGuid.PcdPlatformVendorName|"Pine64"
  gRockchipTokenSpaceGuid.PcdFamilyName|"QuartzPro64"
  gRockchipTokenSpaceGuid.PcdProductUrl|"https://wiki.pine64.org/wiki/QuartzPro64_Development"
  gRockchipTokenSpaceGuid.PcdDeviceTreeName|"rk3588-quartzpro64"

  # I2C
  # HYM8563 RTC on i2c2 (no I2C-based voltage regulators on this platform - dual RK806 is SPI-only)
  gRockchipTokenSpaceGuid.PcdI2cSlaveAddresses|{ 0x51 }
  gRockchipTokenSpaceGuid.PcdI2cSlaveBuses|{ 0x2 }
  gRockchipTokenSpaceGuid.PcdI2cSlaveBusesRuntimeSupport|{ TRUE }
  gPcf8563RealTimeClockLibTokenSpaceGuid.PcdI2cSlaveAddress|0x51
  gRockchipTokenSpaceGuid.PcdRtc8563Bus|0x2

  #
  # PCIe/SATA/USB Combo PIPE PHY support flags and default values
  # combphy0=SATA, combphy1=PCIe(WiFi), combphy2=PCIe(RTL8111HS)
  gRK3588TokenSpaceGuid.PcdComboPhy0Switchable|FALSE
  gRK3588TokenSpaceGuid.PcdComboPhy1Switchable|FALSE
  gRK3588TokenSpaceGuid.PcdComboPhy2Switchable|FALSE
  gRK3588TokenSpaceGuid.PcdComboPhy0ModeDefault|$(COMBO_PHY_MODE_SATA)
  gRK3588TokenSpaceGuid.PcdComboPhy1ModeDefault|$(COMBO_PHY_MODE_PCIE)
  gRK3588TokenSpaceGuid.PcdComboPhy2ModeDefault|$(COMBO_PHY_MODE_PCIE)

  #
  # USB-DP PHYs (USB3 only)
  #
  gRK3588TokenSpaceGuid.PcdUsbDpPhy0Supported|TRUE
  gRK3588TokenSpaceGuid.PcdUsbDpPhy1Supported|TRUE

  #
  # GMAC
  #
  gRK3588TokenSpaceGuid.PcdGmac0Supported|TRUE
  gRK3588TokenSpaceGuid.PcdGmac0TxDelay|0x43
  gRK3588TokenSpaceGuid.PcdGmac0RxDelay|0x00

  #
  # I2S
  #
  gRK3588TokenSpaceGuid.PcdI2S0Supported|TRUE

  #
  # Display support flags and default values
  #
  gRK3588TokenSpaceGuid.PcdDisplayConnectors|{CODE({
    VOP_OUTPUT_IF_HDMI0,
    VOP_OUTPUT_IF_HDMI1
  })}

################################################################################
#
# Components Section - list of all EDK II Modules needed by this Platform.
#
################################################################################
[Components.common]
  # ACPI Support
  $(PLATFORM_DIRECTORY)/AcpiTables/AcpiTables.inf

  # Device Tree Support
  $(PLATFORM_DIRECTORY)/DeviceTree/Vendor.inf
  $(PLATFORM_DIRECTORY)/DeviceTree/Mainline.inf

  # Splash screen logo
  $(VENDOR_DIRECTORY)/Drivers/LogoDxe/LogoDxe.inf
