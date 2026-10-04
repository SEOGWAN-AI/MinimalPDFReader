#!/usr/bin/env python3
"""Inspect the actual Windows PE resources, not just build input files."""
import hashlib
import json
from pathlib import Path
import struct
import tempfile

class PE:
    def __init__(self,path):
        self.path=Path(path); self.data=self.path.read_bytes()
        p=struct.unpack_from('<I',self.data,0x3c)[0]
        assert self.data[p:p+4]==b'PE\0\0'
        assert struct.unpack_from('<H',self.data,p+4)[0]==0x8664
        count=struct.unpack_from('<H',self.data,p+6)[0]
        size=struct.unpack_from('<H',self.data,p+20)[0]
        optional=p+24
        assert struct.unpack_from('<H',self.data,optional)[0]==0x20b
        assert struct.unpack_from('<H',self.data,optional+68)[0]==2
        sections=optional+size
        self.sections=[]
        for i in range(count):
            s=sections+i*40
            virtualSize,va,rawSize,raw=struct.unpack_from('<IIII',self.data,s+8)
            self.sections.append((va,max(virtualSize,rawSize),raw))
        resourceRva,_=struct.unpack_from('<II',self.data,optional+112+16)
        self.resourceBase=self.offset(resourceRva)
        self.resources={}
        self.walk(0,())

    def offset(self,rva):
        for va,size,raw in self.sections:
            if va<=rva<va+size: return raw+rva-va
        raise AssertionError('RVA outside sections')

    def walk(self,relative,path):
        assert len(path)<8
        start=self.resourceBase+relative
        named,numbered=struct.unpack_from('<HH',self.data,start+12)
        for i in range(named+numbered):
            name,entry=struct.unpack_from('<II',self.data,start+16+i*8)
            assert not name&0x80000000, 'Numeric resources expected'
            current=path+(name,)
            if entry&0x80000000: self.walk(entry&0x7fffffff,current)
            else:
                rva,length=struct.unpack_from('<II',self.data,self.resourceBase+entry)
                offset=self.offset(rva)
                self.resources[current]=self.data[offset:offset+length]

    def resource(self,type_,id_):
        found=[v for (t,i,*_),v in self.resources.items() if t==type_ and i==id_]
        assert len(found)==1,(self.path,type_,id_,len(found))
        return found[0]

    def verify_icons(self):
        group=self.resource(14,101)
        reserved,type_,count=struct.unpack_from('<HHH',group)
        assert reserved==0 and type_==1 and count>=7
        sizes=set()
        for i in range(count):
            w,h,_,_,planes,bpp,length,id_=struct.unpack_from('<BBBBHHIH',group,6+i*14)
            sizes.add((w or 256,h or 256))
            assert len(self.resource(3,id_))==length
        assert {(16,16),(32,32),(48,48),(256,256)}<=sizes
        return sorted(sizes)

def main():
    project=Path(__file__).resolve().parents[1]
    setup=PE(project.parent/'MinimalPDFReader-Setup-Win11-v1.0.exe')
    reader=PE(project/'PDFReader.exe')
    uninstall=PE(project/'installer-build/Uninstall.exe')
    record=json.loads((project/'installer-build/payload.json').read_text())
    assert len({r['name'] for r in record})==len(record)
    with tempfile.TemporaryDirectory() as temporary:
        root=Path(temporary)
        for r in record:
            data=setup.resource(10,r['id'])
            assert len(data)==r['bytes']
            assert hashlib.sha256(data).hexdigest()==r['sha256']
            destination=root/r['name']
            assert destination.resolve().is_relative_to(root)
            destination.parent.mkdir(parents=True,exist_ok=True)
            destination.write_bytes(data)
        assert (root/'PDFReader.exe').read_bytes()==reader.data
        assert (root/'Uninstall.exe').read_bytes()==uninstall.data
        assert (root/'PDFReader.ico').read_bytes()==(project/'assets/PDFReader.ico').read_bytes()
        assert (root/'install-id.txt').read_text().startswith('MinimalPDFReader:{7C063285-4FE0-4BA4-AAC8-918DF65D943A}')
    result={
        'setup_size_bytes':len(setup.data),'setup_sha256':hashlib.sha256(setup.data).hexdigest(),
        'reader_sha256':hashlib.sha256(reader.data).hexdigest(),
        'payload_hashes':'PASS: all embedded payloads match originals',
        'pe_architecture':'PASS: setup, reader and uninstaller are x64 Windows GUI',
        'icon_resources':{p.path.name:p.verify_icons() for p in (setup,reader,uninstall)},
        'dpi_manifest':'PASS: PerMonitorV2 and asInvoker are embedded',
        'native_windows_install_and_uninstall':'NOT TESTED: build host is Linux',
        'native_windows_pdf_and_touch_gestures':'NOT TESTED'
    }
    for p in (setup,reader,uninstall):
        manifest=p.resource(24,1)
        assert b'PerMonitorV2' in manifest and b'asInvoker' in manifest
    (project/'tests/installer-validation.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2))

if __name__=='__main__': main()
