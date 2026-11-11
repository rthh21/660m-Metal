// Optional local fallback (Makefile: only when fw/lenovo_660m-73ff-aug.rom exists, adds -DRMB_EMBED_VBIOS).
// Lenovo 660M ATY,bin_image = tools/make_lenovo_vbios.py output (sha256 0b64dda94ef0e0270988dd33681c0268c4c0bb3a
// db9ecc3f7171e1953a8a0e52) from this laptop's own ACPI VFCT. Lenovo/AMD firmware: never committed or shipped.
	.section __DATA,__const
	.globl _rmb_vbios_aug, _rmb_vbios_aug_end
	.p2align 4
_rmb_vbios_aug:
	.incbin "fw/lenovo_660m-73ff-aug.rom"
_rmb_vbios_aug_end:
