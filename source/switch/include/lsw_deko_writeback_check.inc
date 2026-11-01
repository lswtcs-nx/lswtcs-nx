// Opt-in contract check in the production executor, before accepting game jobs.
// It uses native image clears/resolves/copies/fences and the real guest encoder.
// This is not a game route or a performance test.
static bool validateWriteback(){
  log("WRITEBACK_CHECK begin cases=36 presentation=0 game_route=0\n");
  uint64_t waited=0;if(!beginCommands(waited))return false;
  // Seed disjoint physical EDRAM tiles through the production clear path.
  // Unorm and float24 depth also have distinct backing: preserving the same
  // native float while changing its guest encoding is not reinterpretation.
  for(unsigned mode=0;mode<3;++mode){
    LswGpuDrawRec seed{};seed.kind=1;seed.surf_info=Width;seed.rs_copy_cmd=3;
    seed.rs_rect[2]=80;seed.rs_rect[3]=16;seed.rs_clear_color=mode==0;seed.rs_clear_depth=mode!=0;
    seed.depth_info=mode==2?(1536u|(1u<<16)):1024u;seed.rs_color_clear=0xff0000ff;
    seed.rs_depth_clear=((mode==2?0xe00000u:0x400000u)<<8)|0x5b;
    lsw_deko::ResolvePlan plan;std::string error;
    if(!lsw_deko::normalize_resolve(seed,plan,error)||!clearResolve(seed,plan))return fail("writeback fixture EDRAM seed failed");
  }
  lsw_deko::EdramView depthView;std::string depthError;
  if(!lsw_deko::edram_view(Width,1024,true,depthView,depthError))return fail(depthError.c_str());
  DkImageView dv;dkImageViewDefaults(&dv,&edramImages.at(depthView).image);
  if(!submitSlot(0))return false;
  // Observe native clear output before any resolve can transform it. This is
  // a diagnostic boundary, not a guessed adjustment to the guest byte encoder.
  if(!beginCommands(waited))return false;
  DkImageRect pixel={0,0,0,1,1,1};DkCopyBuf destination={dkMemBlockGetGpuAddr(readbackMem),32,0};
  barrier();dkCmdBufCopyImageToBuffer(cmds,&dv,&pixel,&destination,0);barrier();
  if(!submitSlot(0)||!waitSlot(*activeSlot,waited))return false;
  uint32_t native[2];memcpy(native,dkMemBlockGetCpuAddr(readbackMem),8);
  log("DEPTH_NATIVE_INITIAL low=%08X high=%08X expected_depth=3E800000 expected_stencil=5B\n",native[0],native[1]);
  const bool nativeClearMatches=native[0]==0x3e800000&&(native[1]&255)==0x5b;
  LswGpuFrame frame;
  std::vector<uint32_t> expected;
  uint32_t color=0xff0000ff,stencil=0x5b;
  for(unsigned mode=0;mode<3;++mode)for(unsigned endian=0;endian<6;++endian)for(unsigned array=0;array<2;++array){
    unsigned index=unsigned(frame.draws.size());
    LswGpuDrawRec r{};r.kind=1;r.surf_info=Width;r.rs_copy_cmd=1;r.rs_src=mode?4:0;
    r.depth_info=mode==2?(1536u|(1u<<16)):1024u;r.rs_rect[2]=16;r.rs_rect[3]=8;
    r.rs_dest=0x100000;r.rs_dest_pitch=64;r.rs_dest_height=64;r.rs_dest_fmt=6;
    r.rs_dest_x=8;r.rs_dest_y=16;r.rs_dest_swap=mode?0:1;
    r.rs_clear_color=1;r.rs_clear_depth=1;
    r.rs_color_clear=(index&1)?0xff0000ff:0xffff0000;
    r.rs_depth_clear=((mode==2?0xe00000u:0x400000u)<<8)|((index^0xa7)&255);
    // Independent expected constant bytes; no CPU encoder creates this oracle.
    if(mode==2&&endian==0&&array==0)stencil=0x5b;
    expected.push_back(mode?((mode==2?0xe00000u:0x400000u)<<8)|stencil:
        (color&0xff00ff00u)|((color&255)<<16)|((color>>16)&255));
    color=r.rs_color_clear;stencil=r.rs_depth_clear&255;
    frame.draws.push_back(r);
    frame.resolve_registers.push_back({index,(1u<<20)|(1u<<8)|(1u<<9)|r.rs_src,r.rs_dest,
        64|(64<<16),(6<<7)|endian|(array<<3)|(3<<4)|(r.rs_dest_swap<<24)});
    // Append more records after an actual prefix submission. Earlier records and
    // their images stay intact, including results sharing this guest base.
    if(index==17&&!render(frame,18,false))return false;
  }
  if(!render(frame,uint32_t(frame.draws.size()),false))return false;
  if(!frame.deko_execution||frame.deko_execution->executed!=36||presentations)return fail("writeback check prefix execution count");
  std::vector<uint8_t> guest(2u<<20,0x5a);
  unsigned colorPassed=0,depthPassed=0;
  for(unsigned index=0;index<36;++index){
    std::fill(guest.begin(),guest.end(),0x5a);
    MaterializeJob request;request.frame=&frame;request.end=index+1;request.record=index;
    request.physical=guest.data();request.physicalBytes=guest.size();request.wanted={0,uint32_t(guest.size())};
    uint64_t before=submissions.submitted();if(!materialize(request))return false;
    if(submissions.submitted()!=before+1)return fail("readback re-executed an earlier prefix");
    const auto& producer=*frame.deko_execution->resolves[index];
    bool same=lsw_deko::visit_resolve_words(producer.memory,[&](const lsw_deko::ResolveWord& word){
      for(unsigned b=0;b<4;++b){
        unsigned address=word.physical+(b^word.byteXor);uint8_t want=uint8_t(expected[index]>>(8*b));
        if(guest[address]!=want){log("WRITEBACK_MISMATCH record=%u depth=%u format=%u address=%08X byte=%u got=%02X expected=%02X\n",index,unsigned(producer.depth),producer.memory.guestFormat,address,b,guest[address],want);return false;}
      }return true;
    });
    if(same){if(producer.depth)++depthPassed;else ++colorPassed;}
  }
  // Logical release cannot destroy a retained version or an unobserved GPU use.
  auto held=frame.deko_execution->resolves[0];unsigned heldId=held->id;
  surfaces.erase(lsw_deko::resolve_view_key(held->memory.resolve.destination,held->depth,held->width,held->height,&held->memory));
  frame.clear();retireResolveImages();
  if(resolvedImages.size()!=surfaces.size()+1)return fail("frame release did not retire completed resolve versions");
  if(!beginCommands(waited))return false;
  auto temporary=surface(0x180000,false,64,64);if(!temporary)return false;unsigned temporaryId=temporary->id;
  if(!submitSlot(0))return false;
  temporary.reset();surfaces.erase(lsw_deko::resolve_view_key(0x180000,false,64,64));retireResolveImages();
  auto retained=[&](unsigned id){return std::any_of(resolvedImages.begin(),resolvedImages.end(),[&](const auto& image){return image->id==id;});};
  if(!retained(temporaryId)||!retained(heldId))return fail("resolve storage retired before completion/logical release");
  if(!waitSlot(*activeSlot,waited))return false;retireResolveImages();
  if(retained(temporaryId)||!retained(heldId))return fail("resolve completion retirement wrong");
  held.reset();retireResolveImages();
  if(retained(heldId)||resolvedImages.size()!=surfaces.size())return fail("retained resolve reference did not release storage");
  bool ok=nativeClearMatches&&colorPassed==12&&depthPassed==24;
  log("WRITEBACK_CHECK passed=%u cases=36 color_passed=%u depth_passed=%u native_clear_matches=%u endian_modes=6 array_slices=1 retained_old_versions=1 prefix_append_resume=1 fenced_image_retirement=1 presentation=0 submissions=%llu completed=%llu\n",unsigned(ok),colorPassed,depthPassed,unsigned(nativeClearMatches),(unsigned long long)submissions.submitted(),(unsigned long long)submissions.completed());
  return ok?true:fail("native depth/stencil clear/readback contract did not pass");
}
