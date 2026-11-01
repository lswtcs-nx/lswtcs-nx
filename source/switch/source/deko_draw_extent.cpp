// Reuse the vendored BSD Xenia interpreter; no GPU API or guest threads here.
// Compiled with a private namespace and checked physical-word reads.
#include "lsw_deko_draw_extent.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/shader_interpreter.h"
#include <cstring>

namespace xe::gpu {RegisterFile::RegisterFile(){std::memset(values,0,sizeof(values));}}
namespace {
using namespace xe::gpu;
bool bounded_program(const std::vector<uint32_t>& code){
  // Restrict this small CPU task to finite straight-line vertex programs. The
  // full interpreter's loops/calls/textures need separate bounds before use.
  for(unsigned cf=0;cf<128;++cf){size_t base=3*(cf/2);if(base+2>=code.size())return false;
    ucode::ControlFlowInstruction instruction;
    if(cf&1){instruction.dword_0=(code[base+1]>>16)|(code[base+2]<<16);instruction.dword_1=code[base+2]>>16;}
    else{instruction.dword_0=code[base];instruction.dword_1=code[base+1]&65535;}
    auto op=instruction.opcode();
    if(op==ucode::ControlFlowOpcode::kNop||op==ucode::ControlFlowOpcode::kAlloc||op==ucode::ControlFlowOpcode::kMarkVsFetchDone)continue;
    if(op!=ucode::ControlFlowOpcode::kExec&&op!=ucode::ControlFlowOpcode::kExecEnd)return false;
    const auto& exec=*reinterpret_cast<const ucode::ControlFlowExecInstruction*>(&instruction);
    if((uint64_t(exec.address())+exec.count())*3>code.size())return false;
    for(unsigned i=0;i<exec.count();++i)if((exec.sequence()>>(i*2))&1){
      const auto& fetch=*reinterpret_cast<const ucode::FetchInstruction*>(&code[(exec.address()+i)*3]);
      if(fetch.opcode()!=ucode::FetchOpcode::kVertexFetch)return false;
    }
    if(op==ucode::ControlFlowOpcode::kExecEnd)return true;
  }
  return false;
}
class Positions:public ShaderInterpreter::ExportSink {
 public:
  LswDekoExtentPosition* out=nullptr;
  void Export(ucode::ExportRegister reg,const float* values,uint32_t mask)override{
    if(reg==ucode::ExportRegister::kVSPosition){for(unsigned i=0;i<4;++i)if(mask&(1u<<i))out->value[i]=values[i];out->mask|=mask;}
    else if(reg==ucode::ExportRegister::kVSPointSizeEdgeFlagKillVertex&&(mask&4)&&values[2]!=0)out->killed=true;
  }
};
}
bool lsw_deko_extent_positions(const uint32_t* regs,const uint32_t* codeBE,unsigned words,
                              int first,unsigned count,LswDekoReadWord read,void* context,
                              LswDekoExtentPosition* positions){
  if(!regs||!codeBE||!words||words>4096||!count||count>3||!read||!positions)return false;
  std::vector<uint32_t> code(words);for(unsigned i=0;i<words;++i)code[i]=__builtin_bswap32(codeBE[i]);
  if(!bounded_program(code))return false;
  xe::gpu::RegisterFile file;memcpy(file.values,regs,sizeof(file.values));
  xe::Memory memory;memory.read_word=read;memory.read_context=context;
  xe::gpu::ShaderInterpreter interpreter(file,memory);interpreter.SetShader(xe::gpu::xenos::ShaderType::kVertex,code.data());
  Positions sink;interpreter.SetExportSink(&sink);
  for(unsigned i=0;i<count;++i){
    positions[i]={};sink.out=&positions[i];
    std::fill_n(interpreter.temp_registers(),xe::gpu::xenos::kMaxShaderTempRegisters*4,0.0f);
    interpreter.temp_registers()[0]=float(int64_t(first)+i);interpreter.Execute();
    if(memory.read_failed)return false;
  }
  return true;
}
