// Shared by the frozen diagnostic and live renderer. Caller owns cmds/errors/log.
// deko3d 0.5.0 computes level-zero storage with adjustTileSize, but its
// descriptor retains the original tile height. Match them before allocation.
static void imageLayoutFix(DkImageLayoutMaker& maker,unsigned blockHeight){
  if(maker.flags&(DkImageFlags_PitchLinear|DkImageFlags_CustomTileSize|DkImageFlags_UsageVideo))return;
  if(maker.type!=DkImageType_2D&&maker.type!=DkImageType_2DArray&&maker.type!=DkImageType_Cubemap&&maker.type!=DkImageType_CubemapArray)return;
  unsigned rows=(maker.dimensions[1]+blockHeight-1)/blockHeight;
  unsigned gobs=(rows+rows/2+7)/8;
  unsigned selected=gobs>=16?4:gobs>=8?3:gobs>=4?2:gobs>=2?1:0,levelZero=selected;
  while(levelZero&&rows<=(8u<<(levelZero-1)))--levelZero;
  if(levelZero!=selected){maker.flags|=DkImageFlags_CustomTileSize;maker.tileSize=DkTileSize(levelZero);
    log("IMAGE_LAYOUT height=%u block_height=%u tile=%u corrected_tile=%u\n",maker.dimensions[1],blockHeight,selected,levelZero);}
}
static DkBlendFactor blendFactor(unsigned v) {
  constexpr DkBlendFactor m[]={DkBlendFactor_Zero,DkBlendFactor_One,DkBlendFactor_Zero,DkBlendFactor_Zero,
    DkBlendFactor_SrcColor,DkBlendFactor_InvSrcColor,DkBlendFactor_SrcAlpha,DkBlendFactor_InvSrcAlpha,
    DkBlendFactor_DstColor,DkBlendFactor_InvDstColor,DkBlendFactor_DstAlpha,DkBlendFactor_InvDstAlpha,
    DkBlendFactor_ConstColor,DkBlendFactor_InvConstColor,DkBlendFactor_ConstAlpha,DkBlendFactor_InvConstAlpha,DkBlendFactor_SrcAlphaSaturate};
  if(v>=std::size(m)){++errors;return DkBlendFactor_Zero;}return m[v];
}
static DkBlendOp blendOp(unsigned v){constexpr DkBlendOp m[]={DkBlendOp_Add,DkBlendOp_Sub,DkBlendOp_Min,DkBlendOp_Max,DkBlendOp_RevSub};if(v>=std::size(m)){++errors;return DkBlendOp_Add;}return m[v];}
static DkPrimitive primitive(unsigned v){switch(v){case 1:return DkPrimitive_Points;case 2:return DkPrimitive_Lines;case 3:return DkPrimitive_LineStrip;case 4:return DkPrimitive_Triangles;case 5:return DkPrimitive_TriangleStrip;case 10:return DkPrimitive_LinesAdjacency;}++errors;return DkPrimitive_Points;}
static DkImageFormat textureFormat(unsigned dxgi){switch(dxgi){
  case 24:return DkImageFormat_RGB10A2_Unorm;case 28:return DkImageFormat_RGBA8_Unorm;
  case 41:return DkImageFormat_R32_Float;case 49:return DkImageFormat_RG8_Unorm;case 61:return DkImageFormat_R8_Unorm;
  case 71:return DkImageFormat_RGBA_BC1;case 74:return DkImageFormat_RGBA_BC2;case 77:return DkImageFormat_RGBA_BC3;
  default:++errors;log("UNSUPPORTED texture DXGI=%u\n",dxgi);return DkImageFormat_None;}}
static void swizzle(DkImageView& view,unsigned mapping,bool depth=false){
  for(unsigned c=0;c<4;++c){unsigned s=(mapping>>(3*c))&7;
    if(s>5){++errors;continue;}
    // D3D depth SRVs expose (D,0,0,1), while Deko depth views replicate D.
    view.swizzle[c]=depth?(s==0?DkImageSwizzle_Red:s==3||s==5?DkImageSwizzle_One:DkImageSwizzle_Zero):
      s<4?DkImageSwizzle(s+2):s==5?DkImageSwizzle_One:DkImageSwizzle_Zero;}}
static void state(const FrameRecord& r) {
  DkRasterizerState raster;dkRasterizerStateDefaults(&raster);raster.cullMode=DkFace(r.cull&3);raster.frontFace=r.cull&4?DkFrontFace_CW:DkFrontFace_CCW;dkCmdBufBindRasterizerState(cmds,&raster);
  DkColorState color;dkColorStateDefaults(&color);DkColorWriteState masks;dkColorWriteStateDefaults(&masks);
  for(unsigned rt=0;rt<4;++rt){unsigned bc=r.blend[rt],cs=bc&31,cd=(bc>>8)&31,cop=(bc>>5)&7,as=(bc>>16)&31,ad=(bc>>24)&31,aop=(bc>>21)&7;
    unsigned mask=r.ps!=~0u&&r.edramMode==4&&(r.rtMask&(1u<<rt))?(r.colorMask>>(4*rt))&15:0;
    dkColorStateSetBlendEnable(&color,rt,mask&&!(cs==1&&cd==0&&cop==0&&as==1&&ad==0&&aop==0));
    dkColorWriteStateSetMask(&masks,rt,mask);
    if(mask){DkBlendState blend;dkBlendStateDefaults(&blend);dkBlendStateSetOps(&blend,blendOp(cop),blendOp(aop));dkBlendStateSetFactors(&blend,blendFactor(cs),blendFactor(cd),blendFactor(as),blendFactor(ad));dkCmdBufBindBlendState(cmds,rt,&blend);}}
  dkCmdBufBindColorState(cmds,&color);dkCmdBufBindColorWriteState(cmds,&masks);
  unsigned dc=r.depthControl;DkDepthStencilState ds;dkDepthStencilStateDefaults(&ds);ds.depthTestEnable=(dc&6)!=0;ds.depthWriteEnable=(dc&4)!=0;
  ds.depthCompareOp=(dc&2)?DkCompareOp(1+((dc>>4)&7)):DkCompareOp_Always;ds.stencilTestEnable=dc&1;
  constexpr DkStencilOp ops[]={DkStencilOp_Keep,DkStencilOp_Zero,DkStencilOp_Replace,DkStencilOp_IncrWrap,DkStencilOp_DecrWrap,DkStencilOp_Invert,DkStencilOp_Incr,DkStencilOp_Decr};
  unsigned front=8,back=(dc&128)?20:8;
  ds.stencilFrontCompareOp=DkCompareOp(1+((dc>>front)&7));ds.stencilFrontFailOp=ops[(dc>>(front+3))&7];ds.stencilFrontPassOp=ops[(dc>>(front+6))&7];ds.stencilFrontDepthFailOp=ops[(dc>>(front+9))&7];
  ds.stencilBackCompareOp=DkCompareOp(1+((dc>>back)&7));ds.stencilBackFailOp=ops[(dc>>(back+3))&7];ds.stencilBackPassOp=ops[(dc>>(back+6))&7];ds.stencilBackDepthFailOp=ops[(dc>>(back+9))&7];
  dkCmdBufBindDepthStencilState(cmds,&ds);dkCmdBufSetStencil(cmds,DkFace_FrontAndBack,(r.stencilRef>>16)&255,r.stencilRef&255,(r.stencilRef>>8)&255);
  DkViewport vp={r.viewport[0]*r.scaleX,r.viewport[1]*r.scaleY,r.viewport[2]*r.scaleX,r.viewport[3]*r.scaleY,r.viewport[4],r.viewport[5]};
  DkScissor sc={r.scissor[0]*r.scaleX,r.scissor[1]*r.scaleY,r.scissor[2]*r.scaleX,r.scissor[3]*r.scaleY};dkCmdBufSetViewports(cmds,0,&vp,1);dkCmdBufSetScissors(cmds,0,&sc,1);
}
