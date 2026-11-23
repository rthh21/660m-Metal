/*
 * SSDT-XHCWAKE: XHC0 and XHC1 (GP17 functions 3, 4; GPE 0x19) may not wake macOS from S3. Other OSes keep the firmware _PRW.
 *
 * Why (2026-11-19, macOS 25G229, S3 via SSDT-S3): every S3 ends after 6-8 s in "DarkWake ... due to
 * XHC0 XHC1", 46 s awake, sleep, again (awake ~87 % of the "sleep"). At all 9 wakes of that boot
 * XHC1's PMCSR read 0x8000 (PME_Status) and XHC0/2/3/4 read 0. The same happens with the Logitech
 * receiver unplugged and with Bluetooth switched off: the remaining device on XHC1 is the MediaTek
 * MT7922 Bluetooth (USB 0489:e0d8, port 3), which drops off the bus in S3 ("lost power", firmware
 * reloaded by SimpleMtkBtFw at every wake). XHC0 and XHC1 share GPE 0x19, hence "XHC0 XHC1".
 *
 * What: the firmware _PRW of XHC1 (SSDT "CB-01", SSDK, 20411 bytes) is renamed to XPRW by an OpenCore
 * ACPI patch (Find 14 45 06 5F 50 52 57 08, Skip 1: the first match is XHC0's). On Darwin the new _PRW
 * returns {0x19, 0}: deepest wake state S0, so macOS does not arm XHC1's PME for S3 (PMCSR write 0x3
 * instead of 0x103 at sleep). XHC0, the EC, the power button and the RTC are untouched.
 * Cost: devices on XHC1 (Bluetooth, the USB-A port with the Logitech receiver) cannot wake from S3.
 *
 * 11-19 23:44, XHC1 only (SSDT-XHC1WAKE): the wake reason became "XHC0" but the loop stayed. The USB driver
 * still writes PMCSR 0x103 (PME_En) on XHC1, and XHC0 keeps GPE 0x19 armed, so XHC1's PME still wakes.
 * Hence both devices on GPE 0x19 get {0x19, 0}: nothing arms GPE 0x19 for S3. Patch: Skip 0, Count 2
 * (both _PRW methods in SSDK). XHC2/3/4 (GPE 0x1A) keep their wake. Cost: no S3 wake from XHC0/XHC1 ports.
 *
 * Built table: dist/acpi/SSDT-XHCWAKE.aml (Lenovo 82TL, BIOS CB-01; the patch Find bytes are table-specific).
 * Remove = disable both the ACPI>Add entry and the ACPI>Patch entry in config.plist.
 */
DefinitionBlock ("", "SSDT", 2, "RMB660", "XHCWAKE", 0x00000000)
{
    External (\_SB.PCI0.GP17.XHC0, DeviceObj)
    External (\_SB.PCI0.GP17.XHC1, DeviceObj)
    External (\_SB.PCI0.GP17.XHC0.XPRW, MethodObj)    // 0 Arguments
    External (\_SB.PCI0.GP17.XHC1.XPRW, MethodObj)    // 0 Arguments

    Scope (\_SB.PCI0.GP17.XHC0)
    {
        Method (_PRW, 0, NotSerialized)  // _PRW: Power Resources for Wake
        {
            If (_OSI ("Darwin"))
            {
                Return (Package (0x02)
                {
                    0x19,
                    Zero
                })
            }

            Return (\_SB.PCI0.GP17.XHC0.XPRW ())
        }
    }

    Scope (\_SB.PCI0.GP17.XHC1)
    {
        Method (_PRW, 0, NotSerialized)  // _PRW: Power Resources for Wake
        {
            If (_OSI ("Darwin"))
            {
                Return (Package (0x02)
                {
                    0x19,
                    Zero
                })
            }

            Return (\_SB.PCI0.GP17.XHC1.XPRW ())
        }
    }
}
