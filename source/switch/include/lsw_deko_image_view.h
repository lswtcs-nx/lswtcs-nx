// 2D transfers use format interpretation rather than texture swizzles.
#pragma once
inline unsigned lsw_deko_blit_format_index(unsigned mapping){
  switch(mapping&0xfff){
    case 0x688:return 1; // RGBA
    case 0x60a:return 2; // BGRA
    case 0xa88:return 3; // RGB, opaque alpha
    case 0xa0a:return 4; // BGR, opaque alpha
    default:return 0;
  }
}
