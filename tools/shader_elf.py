# Wrap a raw AMD GPU code blob in a minimal ELF64 relocatable (EM_AMDGPU) so llvm-objdump can disassemble it.
# usage: shader_elf.py <in.bin> <out.o>
import sys, struct
code = open(sys.argv[1], "rb").read()
shstr = b"\0.text\0.shstrtab\0"
ehsize, shentsize = 64, 64
text_off = ehsize
shstr_off = text_off + len(code)
sh_off = (shstr_off + len(shstr) + 7) & ~7
EF_AMDGPU_MACH_AMDGCN_GFX1030 = 0x36
hdr = b"\x7fELF" + bytes([2, 1, 1, 64, 3]) + b"\0" * 7          # ELFCLASS64, LSB, v1, OSABI AMDGPU_HSA, ABI v3 (code object v5)
hdr += struct.pack("<HHIQQQIHHHHHH", 1, 224, 1, 0, 0, sh_off, EF_AMDGPU_MACH_AMDGCN_GFX1030, ehsize, 0, 0, shentsize, 3, 2)
def sh(name, typ, flags, off, size, align):
    return struct.pack("<IIQQQQIIQQ", name, typ, flags, 0, off, size, 0, 0, align, 0)
shdrs = sh(0, 0, 0, 0, 0, 0) + sh(1, 1, 6, text_off, len(code), 256) + sh(7, 3, 0, shstr_off, len(shstr), 1)
pad = b"\0" * (sh_off - shstr_off - len(shstr))
open(sys.argv[2], "wb").write(hdr + code + shstr + pad + shdrs)
