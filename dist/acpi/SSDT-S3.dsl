/*
 * SSDT-S3: give macOS an ACPI S3 sleep state (Darwin only) + report the firmware's S3 switches.
 *
 * Why (2026-11-12, macOS 25G229): AppleACPIPlatform logs "ACPI: sleep states S4 S5" and every
 * sleep attempt ends in "PMRD: System sleep prevented by kPMPCIUnsupported" (idle sleep reaches
 * DarkWake, never S3). The DSDT (CB-01) defines _S3 only when the BIOS runs without Modern Standby:
 *     If (LEqual (CNSB, Zero)) { If (LEqual (DAS3, One)) { Name (_S3, Package () {3, 3, 0, 0}) } }
 * (FADT "Low Power S0 Idle" = 1). macOS on x86 has no S0ix, only S3.
 *
 * What: on Darwin only, if the DSDT did not create \_S3, create it with the DSDT's own values
 * (SLP_TYP 3/3). Windows/Linux see nothing (_OSI ("Darwin") is false there).
 *
 * Diagnostics: \_SB.RMS3 (Darwin only) has _STA = 0x0B | DAS3 << 8 | CNSB << 16, published by
 * AppleACPIPlatform as the "_STA" property (ioreg -p IOACPIPlane -n RMS3). DAS3 = 1 means the
 * firmware's S3 support is on (only Modern Standby hides _S3); DAS3 = 0 means S3 is also off in
 * firmware, so the S3 resume may not exist (wake would cold-boot or hang).
 *
 * Built table: dist/acpi/SSDT-S3.aml. To build it yourself: iasl -vx 6142 SSDT-S3.dsl (see below).
 * Remove = disable the ACPI>Add entry in config.plist.
 *
 * iasl-expect: 6142
 * iasl checks a static namespace and rejects CondRefOf (\_S3) before Name (\_S3) as an "Illegal
 * forward reference". At table load, module-level code runs in order: CondRefOf runs first and sees
 * only the DSDT's _S3, which is the intent (and the installed AML works: S3 was entered on
 * 2026-11-12). -vx 6142 makes iasl accept exactly this error.
 */
DefinitionBlock ("", "SSDT", 2, "RMB660", "S3", 0x00000000)
{
    External (CNSB, FieldUnitObj)
    External (DAS3, FieldUnitObj)

    If (_OSI ("Darwin"))
    {
        If (LNot (CondRefOf (\_S3)))
        {
            Name (\_S3, Package (0x04)
            {
                0x03,
                0x03,
                Zero,
                Zero
            })
        }
    }

    Device (\_SB.RMS3)
    {
        Name (_HID, "RMBS0003")  // _HID: Hardware ID
        Method (_STA, 0, NotSerialized)  // _STA: Status
        {
            If (_OSI ("Darwin"))
            {
                Return (Or (0x0B, Or (ShiftLeft (\DAS3, 0x08), ShiftLeft (\CNSB, 0x10))))
            }

            Return (Zero)
        }
    }
}
