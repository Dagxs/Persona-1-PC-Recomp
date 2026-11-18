"""Load Persona.exe (Win32 PE, MSVC, no .reloc) as a flat image at its preferred base."""
import struct
import pefile


class Image:
    def __init__(self, path):
        self.path = path
        pe = pefile.PE(path)
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.mem = bytes(pe.get_memory_mapped_image())
        self.size = len(self.mem)
        # (name, start, end, characteristics)
        self.sections = []
        for s in pe.sections:
            nm = s.Name.rstrip(b'\0').decode('latin1')
            a = self.base + s.VirtualAddress
            self.sections.append((nm, a, a + max(s.Misc_VirtualSize, s.SizeOfRawData), s.Characteristics))
        self.code = [(a, e) for (nm, a, e, ch) in self.sections if ch & 0x20000000]
        # Import address table slots: address -> (dll, name)
        self.imports = {}
        for e in pe.DIRECTORY_ENTRY_IMPORT:
            dll = e.dll.decode().upper()
            for i in e.imports:
                nm = i.name.decode() if i.name else '#%d' % i.ordinal
                self.imports[i.address] = (dll, nm)

    def in_code(self, a):
        return any(b <= a < e for b, e in self.code)

    def in_image(self, a):
        return self.base <= a < self.base + self.size

    def section_of(self, a):
        for s in self.sections:
            if s[1] <= a < s[2]:
                return s
        return None

    def bytes_at(self, a, n):
        o = a - self.base
        return self.mem[o:o + n]

    def u8(self, a):
        return self.mem[a - self.base]

    def u32(self, a):
        return struct.unpack_from('<I', self.mem, a - self.base)[0]
