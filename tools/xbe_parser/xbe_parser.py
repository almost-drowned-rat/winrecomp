"""winrecomp: Win32 PE stand-in for xboxrecomp's XBEParser.

The lifter stages (disasm / func_id / recomp) read the section layout through
``XBEParser(path).parse()``. winrecomp only takes 32-bit PE images, so this
module answers that interface from the PE headers. Section flags use the XBE
bit layout the stages test (0x4 = executable, 0x1 = writable).
"""
from dataclasses import dataclass
from typing import List

XBE_SEC_WRITABLE = 0x00000001
XBE_SEC_EXECUTABLE = 0x00000004


@dataclass
class Section:
    name: str
    virtual_addr: int
    virtual_size: int
    raw_addr: int
    raw_size: int
    flags: int


@dataclass
class Header:
    entry_point: int
    kernel_thunk_addr: int
    image_size: int
    base_address: int


@dataclass
class Image:
    header: Header
    sections: List[Section]


class XBEParser:
    def __init__(self, path):
        self.path = str(path)

    def parse(self) -> Image:
        from tools.disasm.loader import load_image
        im = load_image(self.path)
        secs = [Section(s.name, s.virtual_addr, s.virtual_size, s.raw_addr,
                        s.raw_size,
                        (XBE_SEC_EXECUTABLE if s.executable else 0)
                        | (XBE_SEC_WRITABLE if s.writable else 0))
                for s in im.sections]
        return Image(Header(im.entry_point, im.kernel_thunk_addr,
                            im.image_size, im.base_address), secs)
