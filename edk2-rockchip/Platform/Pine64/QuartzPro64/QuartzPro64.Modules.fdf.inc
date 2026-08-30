## @file
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
#
##

  # ACPI Support
  INF RuleOverride = ACPITABLE $(PLATFORM_DIRECTORY)/AcpiTables/AcpiTables.inf

  # Device Tree Support
  INF RuleOverride = DTB $(PLATFORM_DIRECTORY)/DeviceTree/Vendor.inf
  INF RuleOverride = DTB $(PLATFORM_DIRECTORY)/DeviceTree/Mainline.inf

  # Splash screen logo
  INF $(VENDOR_DIRECTORY)/Drivers/LogoDxe/LogoDxe.inf
