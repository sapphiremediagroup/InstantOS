# TODO

## ACPI 6.0 Support

- [ ] Audit the current ACPI implementation against the ACPI 6.0 specification, especially RSDP/XSDT/RSDT discovery, table revision handling, and checksum validation.
- [ ] Extend 64-bit GAS preference to any future FADT register consumers instead of reading legacy 32-bit fields directly.
- [ ] Add duplicate ACPI table handling and mapped-address bounds checks.
- [ ] Fill gaps in the AML interpreter needed by ACPI 6.0 DSDT/SSDT device enumeration and power methods.
- [ ] Review platform table support needed by current drivers, including MADT, MCFG, HPET, DSDT, SSDT, and FACS.
- [ ] Add boot/test fixtures for representative ACPI 1.0, 2.0, and 6.0 firmware layouts to prevent regressions.
