// Frozen diagnostic inputs shared by the GL reference and Deko frame replay.
// Generated arrays contain private game data and live only in scratch.
#pragma once
struct FrameSpan { unsigned serial,first,count,presentSurface=~0u,presentMapping=0x688; };
struct FrameSurface { unsigned base,width,height,depth; };
struct FrameGpuView { unsigned surface,mapping; };
struct FrameVertexCapture { unsigned record,count; const char* names[32]; };
struct FrameTexture {
  unsigned width,height,mips,mapping,format; // DXGI format
  unsigned upload[16],stride[16],raw[16],rows[16],rowBytes[16];
};
struct FrameRange { unsigned address,offset,bytes; };
struct FrameBinding {
  unsigned stage,slot,original,image,sampler;
  unsigned view=~0u;
};
struct FrameSampler {
  unsigned mag,min,mip,address[3]; float aniso,border[4];
};
struct FrameRecord {
  unsigned kind,vs,ps,gs,topology,indexCount,index32,indexOffset,vertexCount;
  unsigned rangeFirst,rangeCount,bindingFirst,bindingCount;
  unsigned cbOffset[6],cbSize[6];
  unsigned scaleX,scaleY,blend[4],colorMask,depthControl,stencilRef,cull,rtMask,edramMode;
  float viewport[6]; unsigned scissor[4];
  unsigned resolveRect[4],destinationOffset[2],destinationSwap;
  unsigned resolveSurface=~0u;
};
