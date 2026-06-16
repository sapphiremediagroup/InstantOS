/* Placeholder body for the empty crti.o / crtn.o compatibility objects.
 *
 * tcc's default link flow pulls in BOTH crti.o and crtn.o (the ELF _init/_fini
 * bracket). InstantOS has no init/fini sections to bracket, so these objects
 * carry no real code -- but the two inputs must not share a symbol name:
 * tcc's internal linker collides on a repeated name even when the binding is
 * local (STB_LOCAL), unlike lld. The symbol name is therefore parameterized so
 * crti.o and crtn.o get distinct names; the build passes
 * -D__INSTANT_CRT_SYM=<name> per object. */
#ifndef __INSTANT_CRT_SYM
#define __INSTANT_CRT_SYM __instant_tcc_empty_crt
#endif
__attribute__((used)) static void __INSTANT_CRT_SYM(void) {}


