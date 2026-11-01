// Caller orders the preceding image copy and following use with GPU barriers.
// Write the requested constant alpha while preserving all three color channels.
static void lsw_deko_clear_present_alpha(DkCmdBuf commands,const DkImageView& image,unsigned width,unsigned height){
  const DkImageView* colors[]={&image};dkCmdBufBindRenderTargets(commands,colors,1,nullptr);
  DkScissor scissor={0,0,width,height};dkCmdBufSetScissors(commands,0,&scissor,1);
  dkCmdBufClearColorFloat(commands,0,DkColorMask_A,0,0,0,1);
}
