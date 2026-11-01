from pathlib import Path
import argparse,hashlib,json,os,shutil,subprocess,tempfile
parser=argparse.ArgumentParser(description="Actual before/after tile identity reproducer; no game/SDK/network required")
parser.add_argument('--compiler',default='clang++',help='Already installed C++20 compiler executable')
args=parser.parse_args()
base=Path(__file__).resolve().parents[1];port=base/'source'
temporary=tempfile.TemporaryDirectory(prefix='tcs360-draw-identity-');out=Path(temporary.name)
def function(text,name):
 p=text.index(name+'(');start=text.rfind('\n',0,p)+1;a=text.index('{',p);depth=1;b=a+1
 while depth:
  depth+=(text[b]=='{')-(text[b]=='}');b+=1
 return text[start:b]
current=(port/'switch/source/gl_backend.cpp').read_text(encoding='utf-8')
before=(base/'tests/fixtures/gl_backend_mesa011.cpp').read_text(encoding='utf-8')
def planner(s):
 start=s.find('uint64_t draw_base_key(')
 return s[s.index('uint64_t draw_content_key(') if start<0 else start:s.index('void plan_dead_work(')]
schema=(base/'tests/fixtures/spirv_shader_translator.h').read_text()
p=schema.index('  struct SystemConstants {');a=schema.index('{',p);depth=1;b=a+1
while depth:
 depth+=(schema[b]=='{')-(schema[b]=='}');b+=1
sysconst=schema[p:b+1].replace('struct SystemConstants','struct TestSysConst',1)
bridge=(port/'switch/source/gl_bridge.cc').read_text()
helper=function(bridge,'lsw_gl_tile_system_key')
prefix=r'''
#include "lsw_gpu_frame.h"
#include "lsw_gl_shader.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <chrono>
extern "C" void abort(){std::_Exit(86);}
#define XXH_INLINE_ALL
#include "xxhash.h"
namespace xenos {enum class Endian:uint32_t{kNone=0};}
uint64_t g_replays=0;
bool env_on(const char*,bool d){return d;}
void msaa_scale(uint32_t s,uint32_t&x,uint32_t&y){x=1;y=(s>>16)&1?2:1;}
'''
test=r'''
LswGpuFrame fixture(unsigned count=8,unsigned ranges=1,unsigned tiles=2,bool clear=false){
 LswGpuFrame f;f.arena.resize(16384,0);
 for(unsigned tile=0;tile<tiles;++tile){
  for(unsigned q=0;q<ranges;++q){
   unsigned off=4096+(tile*ranges+q)*128;
   std::memset(f.arena.data()+off,42+q,128);f.ranges.push_back({0x4000+q*4096,128,off});
  }
  if(clear){LswGpuDrawRec r{};r.kind=0;r.viewport[2]=8192;r.viewport[3]=8192;r.scissor[2]=1280;r.scissor[3]=320;f.draws.push_back(r);}
  for(unsigned q=0;q<count;++q){LswGpuDrawRec r{};
   r.kind=0;r.vs_key=101+q;r.ps_key=202;r.vertex_count=6;r.topology=4;
   r.cb_sys_off=tile*1024;r.cb_sys_size=kLswGlClipPlaneOffset+kLswGlClipPlaneSize;
   r.cb_vsf_off=2048+tile*32;r.cb_vsf_size=32;r.cb_psf_off=2112+tile*32;r.cb_psf_size=32;
   r.cb_bool_off=2304;r.cb_fetch_off=2560;r.first_range=tile*ranges;r.range_count=ranges;
   r.viewport[1]=-float(tile*288);r.viewport[2]=1280;r.viewport[3]=320;r.viewport[5]=1;
   r.scissor[2]=1280;r.scissor[3]=320;f.draws.push_back(r);}
  LswGpuDrawRec resolve{};resolve.kind=1;resolve.rs_rect[2]=1280;resolve.rs_rect[3]=320;f.draws.push_back(resolve);
 }
 return f;
}
void mutate(LswGpuFrame&f,const std::string&m){
 for(size_t i=9;i<17;++i){auto&r=f.draws[i];
  // GetHostViewportInfo clamps y to 0 and shrinks the extent: a later tile's real viewport is y 0, height 32.
  if(m=="clamped-height"){r.viewport[1]=0;r.viewport[3]=32;}
  if(m=="viewport-height-mixed"&&i==10)r.viewport[3]=300;
  if(m=="topology")r.topology=5;
  if(m=="index-width")r.ib_32bit=1;
  if(m=="color-mask")r.color_mask=7;
  if(m=="stencil")r.stencil_ref_bf=9;
  if(m=="mrt")r.color_info_mrt[3]=32;
  if(m=="mrt-blend")r.blend_mrt[2]=42;
  if(m=="mode")r.edram_mode=5;
  if(m=="raster")r.sc_mode_cntl=16;
  if(m=="output-mask")r.ps_rt_mask=3;
  if(m=="writes-depth")r.ps_writes_depth=1;
  if(m=="gs")r.gs_dxbc=reinterpret_cast<void*>(4096);
  if(m=="viewport-x")r.viewport[0]=4;
  if(m=="viewport-width")r.viewport[2]=640;
  if(m=="viewport-depth")r.viewport[4]=0.5;
  if(m=="sampler"){r.ps_smp_count=1;r.ps_smp[0].min_filter=2;}
  if(m=="binding"){r.ps_tex_count=1;r.ps_tex_fetch[0]=3;}
 }
 auto byte=[&](size_t off){f.arena[1024+off]^=1;};
 if(m=="system-flags")byte(offsetof(SysConst,flags));
 if(m=="base-index")byte(offsetof(SysConst,vertex_base_index));
 if(m=="index-endian")byte(offsetof(SysConst,vertex_index_endian));
 if(m=="alpha-test")byte(offsetof(SysConst,alpha_test_reference));
 if(m=="clip-plane")byte(kLswGlClipPlaneOffset+12);
 if(m=="point-diameter")byte(offsetof(SysConst,point_vertex_diameter_min));
 if(m=="ordinary-point-radius")byte(offsetof(SysConst,point_screen_diameter_to_ndc_radius));
 if(m=="tile-system"){
  byte(offsetof(SysConst,ndc_scale)+4);byte(offsetof(SysConst,ndc_offset)+4);
 }
 if(m=="point-tile-system"){
  for(auto&r:f.draws)if(r.kind==0)r.gs_type=1;
  byte(offsetof(SysConst,ndc_scale)+4);byte(offsetof(SysConst,ndc_offset)+4);
  byte(offsetof(SysConst,point_screen_diameter_to_ndc_radius));
 }
 if(m=="vertex")f.arena[f.ranges[1].arena_off]^=1;
 if(m=="fifth-upload")f.arena[f.ranges[9].arena_off]^=1;
 if(m=="fifth-address")f.ranges[9].phys+=4096;
 if(m=="inline-index"){
  for(size_t i=0;i<f.draws.size();++i)if(f.draws[i].kind==0){auto&r=f.draws[i];r.ib_count=3;r.ib_off=i<9?8192:8208;}
  f.arena[8208]=1;
 }
}
template<class A,class B>bool same_plan(const A&a,const B&b){return a.act==b.act&&a.clear==b.clear&&a.arg==b.arg&&a.arg2==b.arg2&&a.order==b.order&&a.groups==b.groups&&a.skipped==b.skipped;}
int main(){
 const char*changed[]={"viewport-height-mixed","vertex","topology","fifth-upload","fifth-address","index-width","color-mask","stencil","mrt","mrt-blend","mode","raster","output-mask","writes-depth","gs","viewport-x","viewport-width","viewport-depth","sampler","binding","system-flags","base-index","index-endian","alpha-test","clip-plane","point-diameter","ordinary-point-radius","inline-index"};
 unsigned old_bad=0;
 for(const char*mode:changed){auto f=fixture(8,std::string(mode).find("fifth")==0?5:1);mutate(f,mode);
  Old::UntilePlan a;New::UntilePlan b;Old::plan_untile(f,a);New::plan_untile(f,b);
  assert(b.groups==0&&b.skipped==0);for(size_t i=0;i<b.order.size();++i)assert(b.order[i]==i&&b.act[i]==New::UT_NONE);
  if(a.skipped)++old_bad;printf("changed=%s old_skipped=%u new_skipped=%u\n",mode,a.skipped,b.skipped);
 }
 for(const char*mode:{"identical","tile-system","point-tile-system","clamped-height"}){auto f=fixture();mutate(f,mode);
  Old::UntilePlan a;New::UntilePlan b;Old::plan_untile(f,a);New::plan_untile(f,b);
  assert(a.groups==1&&a.skipped==8&&same_plan(a,b));printf("preserved=%s groups=%u skipped=%u\n",mode,b.groups,b.skipped);
 }
 // Walk-time copies: tile_skip_copy points them at tile 0's data, and the plan reuses the source's base key.
 auto copies=[](LswGpuFrame&f){for(size_t i=9;i<17;++i){auto&r=f.draws[i];const auto&s=f.draws[i-9];
  r.tile_repeat=uint32_t(i-9+1);r.first_range=s.first_range;r.cb_vsf_off=s.cb_vsf_off;r.cb_psf_off=s.cb_psf_off;
  r.cb_bool_off=s.cb_bool_off;r.cb_fetch_off=s.cb_fetch_off;}};
 {auto f=fixture();auto g=fixture();copies(g);New::UntilePlan a;New::UntilePlan b;New::plan_untile(f,a);New::plan_untile(g,b);
  assert(a.groups==1&&same_plan(a,b));
  // The reused key must equal the key a full hash gives, or a later comparison could disagree with the old planner.
  for(size_t i=9;i<17;++i)assert(b.key[i]==New::draw_content_key(g,g.draws[i]));
  printf("preserved=walk-copy groups=%u skipped=%u\n",b.groups,b.skipped);}
 {auto g=fixture();copies(g);g.arena[2112+32]^=1;g.draws[10].cb_psf_off=2112+32;New::UntilePlan b;New::plan_untile(g,b);
  assert(b.groups==0&&b.skipped==0);printf("changed=walk-copy-damaged-link new_skipped=%u\n",b.skipped);}
 {auto g=fixture();copies(g);g.ranges[0].arena_off=kLswRangeResident;New::UntilePlan b;New::plan_untile(g,b);
  assert(b.groups==0&&b.skipped==0);printf("unsupported=walk-copy-resident fallback=all-records\n");}
 // Unsupported or malformed inputs must never let two zero keys authorize a merge.
 for(unsigned mode=0;mode<7;++mode){auto f=fixture();
  for(auto&r:f.draws)if(r.kind==0){
   if(mode==0)r.cb_sys_size=0;if(mode==1)r.cb_sys_off=UINT32_MAX;
   if(mode==2)r.first_range=UINT32_MAX;if(mode==3)r.cb_vsf_off=UINT32_MAX;
   if(mode==4){r.ib_count=UINT32_MAX;r.ib_32bit=1;}if(mode==5)r.ps_smp_count=33;
  }
  if(mode==6)f.ranges[0].arena_off=f.ranges[1].arena_off=kLswRangeResident;
  New::UntilePlan b;New::plan_untile(f,b);assert(b.groups==0&&b.skipped==0);
  assert(New::draw_content_key(f,f.draws[0])==0);printf("unsupported=%u fallback=all-records\n",mode);
 }
 // Same snapshots relocated in arena, 2/3 tiles, clear moves, MSAA, late scissor-only geometry.
 for(unsigned run=0;run<300;++run){unsigned count=8+run%23;auto f=fixture(count,run%6,2+run%2,run%3==0);
  for(auto&r:f.draws)if(r.kind==0){r.surf_info=(run%2)<<16;r.scissor[0]=run%10;r.scissor[2]-=run%10;}
  Old::UntilePlan a;New::UntilePlan b;Old::plan_untile(f,a);New::plan_untile(f,b);assert(a.groups==1&&same_plan(a,b));
 }
 // Representative zero-upload warm plan, not a console benchmark.
 auto f=fixture(1000,0,3);auto t0=std::chrono::steady_clock::now();
 for(unsigned i=0;i<100;++i){New::UntilePlan b;New::plan_untile(f,b);assert(b.skipped==2000);}
 auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-t0).count();
 printf("summary changed=28 old_bad=%u supported=4 unsupported=7 equivalent_plans=300 host_warm_3000draw_us=%lld\n",old_bad,(long long)us);
 assert(old_bad>=24);
}
'''
cpp=out/'actual-identity.cpp';exe=out/'actual-identity.exe'
cpp.write_text(prefix+'\n'+sysconst+'\nusing SysConst=TestSysConst;\n'+helper+'\nnamespace Old {\n'+planner(before)+'\n}\nnamespace New {\n'+planner(current)+'\n}\n'+test,encoding='utf-8')
compiler=Path(shutil.which(args.compiler) or args.compiler)
if not compiler.is_file():raise SystemExit('Compiler not found; pass --compiler with an already installed compiler path. Nothing is installed automatically.')
env=dict(os.environ,PATH=str(compiler.parent)+os.pathsep+os.environ['PATH'])
command=[str(compiler),'-std=c++20','-O2','-I'+str(port/'LSWTCSRuntime/include'),'-I'+str(port/'switch/include'),'-I'+str(base/'tests/fixtures'),str(cpp),'-o',str(exe)]
r=subprocess.run(command,capture_output=True,env=env);(out/'host-compile.log').write_bytes(r.stdout+r.stderr)
if r.returncode:print((r.stdout+r.stderr).decode(errors='replace'));raise SystemExit(r.returncode)
r=subprocess.run([str(exe)],capture_output=True,env=env,timeout=60);(out/'host-output.log').write_bytes(r.stdout+r.stderr)
report={'passed':r.returncode==0,'exit_code':r.returncode,'output':r.stdout.decode(errors='replace'),'stderr':r.stderr.decode(errors='replace'),
 'scope':'Actual before/after draw keys/planners and new system-key helper. SystemConstants declaration extracted verbatim from installed project schema; Endian mocked with uint32 enum, env/default and MSAA helpers mocked. Actual record/frame/XXH3 code. No driver/shader execution, pixel attribution or console performance proof.',
 'new_geometry_snapshot_or_cache':False,'production_source':[{'path':str(p.relative_to(port)),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in [port/'switch/source/gl_backend.cpp',port/'switch/source/gl_bridge.cc',port/'switch/include/lsw_gl_shader.h']],
 'schema_sha256':hashlib.sha256(schema.encode()).hexdigest()}
(out/'host-validation.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2));assert report['passed']

temporary.cleanup()
