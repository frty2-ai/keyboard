#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 frty2.ai and contributors
# SPDX-License-Identifier: GPL-2.0-or-later
"""Minimal HID report-descriptor decoder (no deps). Usage: rdesc_decode.py <file-with-raw-bytes>"""
import sys
PAGES={0x01:'GenericDesktop',0x07:'Keyboard/Keypad',0x08:'LED',0x09:'Button',0x0C:'Consumer',0x0D:'Digitizer',0xFF00:'Vendor(FF00)'}
GD={0x01:'Pointer',0x02:'Mouse',0x06:'Keyboard',0x30:'X',0x31:'Y',0x37:'Dial',0x38:'Wheel',0x80:'SystemControl',0x81:'SysPowerDown',0x82:'SysSleep',0x83:'SysWakeUp'}
CONS={0x01:'ConsumerControl',0x30:'Power',0x6F:'DisplayBrightnessInc',0x70:'DisplayBrightnessDec',0xB5:'ScanNext',0xB6:'ScanPrev',0xB7:'Stop',0xCD:'PlayPause',0xE0:'Volume',0xE2:'Mute',0xE9:'VolumeInc',0xEA:'VolumeDec',0x183:'AL_ConsumerControlConfig',0x18A:'AL_Email',0x192:'AL_Calculator',0x194:'AL_LocalBrowser',0x221:'AC_Search',0x223:'AC_Home',0x224:'AC_Back',0x225:'AC_Forward',0x22D:'AC_ZoomIn',0x22E:'AC_ZoomOut',0x238:'AC_Pan'}
MAIN={8:'Input',9:'Output',0xB:'Feature',0xA:'Collection',0xC:'EndCollection'}
GLOB={0:'UsagePage',1:'LogicalMin',2:'LogicalMax',3:'PhysicalMin',4:'PhysicalMax',5:'UnitExp',6:'Unit',7:'ReportSize',8:'ReportID',9:'ReportCount',0xA:'Push',0xB:'Pop'}
LOC={0:'Usage',1:'UsageMin',2:'UsageMax',3:'DesignatorIdx',4:'DesignatorMin',5:'DesignatorMax',7:'StringIdx',8:'StringMin',9:'StringMax',0xA:'Delimiter'}
COLL={0:'Physical',1:'Application',2:'Logical',3:'Report',4:'NamedArray',5:'UsageSwitch',6:'UsageModifier'}
def uname(page,u):
    if page==0x01: return GD.get(u,'')
    if page==0x0C: return CONS.get(u,'')
    return ''
def main(path):
    b=open(path,'rb').read(); i=0; depth=0; page=0; rsize=0; rcount=0; rid=0
    sizes={}  # (type,rid)->bits
    out=[]
    while i<len(b):
        h=b[i]; i+=1
        if h==0xFE:  # long item
            n=b[i]; i+=2+n; out.append('  '*depth+'LongItem'); continue
        bsz=[0,1,2,4][h&3]; typ=(h>>2)&3; tag=h>>4
        raw=b[i:i+bsz]; i+=bsz
        v=int.from_bytes(raw,'little') if raw else 0
        sv=int.from_bytes(raw,'little',signed=True) if raw else 0
        if typ==0:
            name=MAIN.get(tag,'Main?%x'%tag)
            if tag==0xC: depth=max(depth-1,0)
            extra=''
            if tag in (8,9,0xB):
                flags=[]
                flags.append('Const' if v&1 else 'Data'); flags.append('Var' if v&2 else 'Array'); flags.append('Rel' if v&4 else 'Abs')
                if v&0x40: flags.append('Null')
                if v&0x100: flags.append('Buf')
                extra=' (%s) bits=%dx%d'%(','.join(flags),rcount,rsize)
                sizes[(name,rid)]=sizes.get((name,rid),0)+rcount*rsize
            elif tag==0xA: extra=' '+COLL.get(v,'0x%x'%v)
            out.append('  '*depth+'%s%s'%(name,extra))
            if tag==0xA: depth+=1
        elif typ==1:
            name=GLOB.get(tag,'Global?%x'%tag)
            if tag==0: page=v; out.append('  '*depth+'UsagePage 0x%04x %s'%(v,PAGES.get(v,'')))
            else:
                if tag==7: rsize=v
                if tag==8: rid=v
                if tag==9: rcount=v
                out.append('  '*depth+'%s %d'%(name, sv if tag in (1,2,3,4) else v))
        elif typ==2:
            name=LOC.get(tag,'Local?%x'%tag)
            if tag in (0,1,2):
                p=page; u=v
                if bsz==4: p=v>>16; u=v&0xFFFF
                out.append('  '*depth+'%s 0x%04x %s'%(name,u,uname(p,u)))
            else: out.append('  '*depth+'%s %d'%(name,v))
    print('\n'.join(out))
    print('\n== report sizes (excluding report-ID byte) ==')
    for (t,r),bits in sorted(sizes.items()):
        print('  %-8s reportID=%-3d %4d bits = %d bytes'%(t,r,bits,(bits+7)//8))
if __name__=='__main__': main(sys.argv[1])
