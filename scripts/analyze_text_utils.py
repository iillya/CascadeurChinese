"""Read-only PE/Qt meta-object investigation, never loads the target DLL."""
import argparse
import bisect
import json
import hashlib
from pathlib import Path
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

HOST = Path('C:/Program Files/Cascadeur/presenter_lib.dll')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--rva', type=lambda x:int(x,0))
    p.add_argument('--size', type=lambda x:int(x,0), default=0x300)
    p.add_argument('--xref', type=lambda x:int(x,0))
    p.add_argument('--data', type=lambda x:int(x,0))
    p.add_argument('--report', type=Path, help='Save hash-locked function disassembly and metadata evidence')
    args = p.parse_args()
    pe = pefile.PE(str(HOST))
    data = HOST.read_bytes()
    base = pe.OPTIONAL_HEADER.ImageBase
    cs = Cs(CS_ARCH_X86, CS_MODE_64)
    cs.detail = True
    functions = [(e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION]
    starts = [f[0] for f in functions]
    print('ImageBase', hex(base))
    imports = {imp.address-base: entry.dll.decode()+':'+(imp.name.decode() if imp.name else str(imp.ordinal))
               for entry in pe.DIRECTORY_ENTRY_IMPORT for imp in entry.imports}
    if args.report:
        from native_text_names import DLL_SHA256
        digest = hashlib.sha256(data).hexdigest().upper()
        if digest != DLL_SHA256:
            raise ValueError('Host DLL changed; offsets must be re-analyzed')
        ranges = {'static_metacall': (0x50810, 0x508c8),
                  'fromAnyCaseString': (0xd37c0, 0xd38d9),
                  'fromCamelCaseString': (0xd38e0, 0xd3af4),
                  'fromDomainObjectTypeString': (0xd3b00, 0xd3c64),
                  'fromSnakeCaseString': (0xd3c70, 0xd3d4d)}
        recovered = {}
        for name, (start, end) in ranges.items():
            lines = []
            for ins in cs.disasm(pe.get_data(start, end-start), base+start):
                refs = []
                for op in ins.operands:
                    if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                        target = ins.address+ins.size+op.mem.disp-base
                        refs.append({'rva':hex(target), 'import':imports.get(target),
                                     'data_hex':pe.get_data(target, 64).hex()})
                lines.append({'rva':hex(ins.address-base), 'bytes':ins.bytes.hex(),
                              'instruction':ins.mnemonic+' '+ins.op_str, 'rip_references':refs})
            recovered[name] = {'start_rva':hex(start), 'end_rva_exclusive':hex(end), 'instructions':lines}
        report = {'dll':str(HOST), 'sha256':digest, 'image_base':hex(base),
                  'address_kind':'RVA, not file offset',
                  'class_name_rva':'0xba4c18', 'static_metacall_pointer_rva':'0xba4860',
                  'meta_strings_hex':pe.get_data(0xba4be0, 0x100).hex(),
                  'meta_object_hex':pe.get_data(0xba4848, 0x40).hex(),
                  'meta_methods_hex':pe.get_data(0xba56d0, 0x100).hex(), 'functions':recovered}
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
        print('Saved', args.report)
        return
    if args.data is not None:
        raw = pe.get_data(args.data,args.size)
        for off in range(0,len(raw)-7,8):
            value = struct.unpack_from('<Q',raw,off)[0]
            print(hex(args.data+off),hex(value), 'RVA='+hex(value-base) if base <= value < base+pe.OPTIONAL_HEADER.SizeOfImage else '', repr(raw[off:off+8]))
        return
    if args.rva is not None:
        for ins in cs.disasm(pe.get_data(args.rva,args.size), base+args.rva):
            note = ''
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    rva = ins.address+ins.size+op.mem.disp-base
                    raw = pe.get_data(rva, 100)
                    note += f' ; [{rva:#x}] '+imports.get(rva,repr(raw[:64]))
            print(f'{ins.address-base:08x} {ins.mnemonic:8s} {ins.op_str}{note}')
        return
    needles = [b'utils::TextUtils\0', b'fromAnyCaseString\0', b'fromDomainObjectTypeString\0']
    targets = set()
    if args.xref is not None:
        targets.add(args.xref)
    else:
        for needle in needles:
            start = 0
            while (pos := data.find(needle, start)) >= 0:
                rva = pe.get_rva_from_offset(pos)
                targets.add(rva)
                print('STRING', hex(pos),hex(rva),repr(data[max(0,pos-80):pos+180]))
                start = pos+1
    # Pointer xrefs in data (vtable/staticMetaObject), and RIP references in code.
    for rva in targets:
        needle = struct.pack('<Q',base+rva)
        start = 0
        while (pos := data.find(needle,start)) >= 0:
            print('POINTER', hex(pe.get_rva_from_offset(pos)), '->',hex(rva))
            start=pos+1
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        # Fast prefilter before expensive operand decoding: RIP-relative LEA/MOV.
        block = section.get_data()
        for off in range(len(block)-7):
            if block[off] not in (0x48,0x4c) or block[off+1] not in (0x8d,0x8b):
                continue
            if block[off+2] & 0xc7 != 0x05:
                continue
            at = section.VirtualAddress+off
            target = at+7+struct.unpack_from('<i',block,off+3)[0]
            if target not in targets:
                continue
            f = functions[max(0,bisect.bisect_right(starts,at)-1)]
            bounds = tuple(hex(x) for x in f) if f[0] <= at < f[1] else 'leaf/unlisted'
            print('CODE',hex(at),'->',hex(target),'FUNCTION',bounds)


if __name__ == '__main__':
    main()
