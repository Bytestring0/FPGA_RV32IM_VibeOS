#!/usr/bin/env python3
"""Rasterize a user-selected font to the board's BMP8x16 glyph asset."""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--font',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--ascii',action='store_true')
    a=p.parse_args()
    font=ImageFont.truetype(str(a.font),13 if a.ascii else 16)
    glyphs=bytearray((128 if a.ascii else 65536)*16)
    count=0
    for cp in range(32,128 if a.ascii else 65536):
        if 0xd800<=cp<=0xdfff: continue
        char=chr(cp)
        box=font.getbbox(char)
        if not box:continue
        w,h=box[2]-box[0],box[3]-box[1]
        if w<=0 or h<=0:continue
        im=Image.new('L',(w,h));ImageDraw.Draw(im).text((-box[0],-box[1]),char,font=font,fill=255)
        # Retain readable baseline and fit wide glyphs to one hardware cell.
        if a.ascii and w>8:raise ValueError("Font exceeds eight-pixel cell")
        if w>8:im=im.resize((8,h),Image.Resampling.LANCZOS)
        if h>16:im=im.resize((im.width,16),Image.Resampling.LANCZOS)
        tile=Image.new('L',(8,16));tile.paste(im,((8-im.width)//2,max(0,min(16-im.height,box[1]))))
        for y in range(16):
            value=0
            for x in range(8):value=(value<<1)|int(tile.getpixel((x,y))>=96)
            glyphs[cp*16+y]=value
        count+=1
    a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_bytes(glyphs)
    assert len(glyphs)==(2048 if a.ascii else 1048576) and not any(glyphs[32*16:33*16])
    for cp in map(ord,'Agent0123456789' if a.ascii else 'Agent中文任务确认取消'):assert any(glyphs[cp*16:(cp+1)*16])
    print('DISPLAY_FONT_PASS bytes='+str(len(glyphs))+' rasterized='+str(count))
if __name__=='__main__':main()
