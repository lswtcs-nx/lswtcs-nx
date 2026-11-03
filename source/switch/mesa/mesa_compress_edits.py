"""LSWTCS: lets render targets use compressed colour kinds, for states Z and J of the A/B cycle.

lsw_nvc0_rt_compress is set only around a render target's allocation: bit 0 asks for 128 KB alignment (__wrap_nouveau_bo_new,
bo_check.cpp), bit 1 also picks kind 0xdb (C32_2CRA, what deko3d uses at one sample) for 32-bit colour. Mesa requests compressed depth
kinds already, but with 4 KB alignment; deko3d aligns compressed images to a big page, so those requests may be falling back to no
compression. With the flag at 0 nothing changes.

Mesa's comment says 0xdb "makes things blurry". That is from other hardware and nothing here proves it applies, so J is compared as its
own state. Applied after mesa_cbdedupe_edits.py. Usage: python mesa_compress_edits.py <mesa-src-dir>"""
import sys, os
root = sys.argv[1]

def edit(rel, pairs):
    p = os.path.join(root, rel)
    s = open(p, newline='').read()
    for old, new in pairs:
        if s.count(new) == 1:
            continue   # already applied
        assert s.count(old) == 1, (rel, old[:70])
        s = s.replace(old, new)
    open(p, 'w', newline='').write(s)

edit('src/gallium/drivers/nouveau/nvc0/nvc0_miptree.c', [
    ('static uint32_t\nnvc0_mt_choose_storage_type(struct nv50_miptree *mt, bool compressed)\n',
     '/* LSWTCS (mesa_compress_edits.py) */\nvolatile int lsw_nvc0_rt_compress = 0;\n\n'
     'static uint32_t\nnvc0_mt_choose_storage_type(struct nv50_miptree *mt, bool compressed)\n'),
    ('         if (compressed && ms) {\n            switch (ms) {\n               /* This one makes things blurry:\n'
     '            case 0: tile_flags = 0xdb; break;\n               */\n',
     '         if (compressed && (ms || (lsw_nvc0_rt_compress & 2))) {\n            switch (ms) {\n'
     '            case 0: tile_flags = 0xdb; break;   /* LSWTCS: only with the flag; Mesa: "makes things blurry" */\n'),
])
print('compress edits applied')
