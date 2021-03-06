/*
 * Copyright 2011 Christoph Bumiller
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

extern "C" {
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_scan.h"
#include "tgsi/tgsi_util.h"
}

#include <set>

#include "codegen/nv50_ir.h"
#include "codegen/nv50_ir_util.h"
#include "codegen/nv50_ir_build_util.h"

namespace tgsi {

class Source;

static nv50_ir::operation translateOpcode(uint opcode);
static nv50_ir::DataFile translateFile(uint file);
static nv50_ir::TexTarget translateTexture(uint texTarg);
static nv50_ir::SVSemantic translateSysVal(uint sysval);

class Instruction
{
public:
   Instruction(const struct tgsi_full_instruction *inst) : insn(inst) { }

   class SrcRegister
   {
   public:
      SrcRegister(const struct tgsi_full_src_register *src)
         : reg(src->Register),
           fsr(src)
      { }

      SrcRegister(const struct tgsi_src_register& src) : reg(src), fsr(NULL) { }

      SrcRegister(const struct tgsi_ind_register& ind)
         : reg(tgsi_util_get_src_from_ind(&ind)),
           fsr(NULL)
      { }

      struct tgsi_src_register offsetToSrc(struct tgsi_texture_offset off)
      {
         struct tgsi_src_register reg;
         memset(&reg, 0, sizeof(reg));
         reg.Index = off.Index;
         reg.File = off.File;
         reg.SwizzleX = off.SwizzleX;
         reg.SwizzleY = off.SwizzleY;
         reg.SwizzleZ = off.SwizzleZ;
         return reg;
      }

      SrcRegister(const struct tgsi_texture_offset& off) :
         reg(offsetToSrc(off)),
         fsr(NULL)
      { }

      uint getFile() const { return reg.File; }

      bool is2D() const { return reg.Dimension; }

      bool isIndirect(int dim) const
      {
         return (dim && fsr) ? fsr->Dimension.Indirect : reg.Indirect;
      }

      int getIndex(int dim) const
      {
         return (dim && fsr) ? fsr->Dimension.Index : reg.Index;
      }

      int getSwizzle(int chan) const
      {
         return tgsi_util_get_src_register_swizzle(&reg, chan);
      }

      nv50_ir::Modifier getMod(int chan) const;

      SrcRegister getIndirect(int dim) const
      {
         assert(fsr && isIndirect(dim));
         if (dim)
            return SrcRegister(fsr->DimIndirect);
         return SrcRegister(fsr->Indirect);
      }

      uint32_t getValueU32(int c, const struct nv50_ir_prog_info *info) const
      {
         assert(reg.File == TGSI_FILE_IMMEDIATE);
         assert(!reg.Absolute);
         assert(!reg.Negate);
         return info->immd.data[reg.Index * 4 + getSwizzle(c)];
      }

   private:
      const struct tgsi_src_register reg;
      const struct tgsi_full_src_register *fsr;
   };

   class DstRegister
   {
   public:
      DstRegister(const struct tgsi_full_dst_register *dst)
         : reg(dst->Register),
           fdr(dst)
      { }

      DstRegister(const struct tgsi_dst_register& dst) : reg(dst), fdr(NULL) { }

      uint getFile() const { return reg.File; }

      bool is2D() const { return reg.Dimension; }

      bool isIndirect(int dim) const
      {
         return (dim && fdr) ? fdr->Dimension.Indirect : reg.Indirect;
      }

      int getIndex(int dim) const
      {
         return (dim && fdr) ? fdr->Dimension.Dimension : reg.Index;
      }

      unsigned int getMask() const { return reg.WriteMask; }

      bool isMasked(int chan) const { return !(getMask() & (1 << chan)); }

      SrcRegister getIndirect(int dim) const
      {
         assert(fdr && isIndirect(dim));
         if (dim)
            return SrcRegister(fdr->DimIndirect);
         return SrcRegister(fdr->Indirect);
      }

   private:
      const struct tgsi_dst_register reg;
      const struct tgsi_full_dst_register *fdr;
   };

   inline uint getOpcode() const { return insn->Instruction.Opcode; }

   unsigned int srcCount() const { return insn->Instruction.NumSrcRegs; }
   unsigned int dstCount() const { return insn->Instruction.NumDstRegs; }

   // mask of used components of source s
   unsigned int srcMask(unsigned int s) const;

   SrcRegister getSrc(unsigned int s) const
   {
      assert(s < srcCount());
      return SrcRegister(&insn->Src[s]);
   }

   DstRegister getDst(unsigned int d) const
   {
      assert(d < dstCount());
      return DstRegister(&insn->Dst[d]);
   }

   SrcRegister getTexOffset(unsigned int i) const
   {
      assert(i < TGSI_FULL_MAX_TEX_OFFSETS);
      return SrcRegister(insn->TexOffsets[i]);
   }

   unsigned int getNumTexOffsets() const { return insn->Texture.NumOffsets; }

   bool checkDstSrcAliasing() const;

   inline nv50_ir::operation getOP() const {
      return translateOpcode(getOpcode()); }

   nv50_ir::DataType inferSrcType() const;
   nv50_ir::DataType inferDstType() const;

   nv50_ir::CondCode getSetCond() const;

   nv50_ir::TexInstruction::Target getTexture(const Source *, int s) const;

   inline uint getLabel() { return insn->Label.Label; }

   unsigned getSaturate() const { return insn->Instruction.Saturate; }

   void print() const
   {
      tgsi_dump_instruction(insn, 1);
   }

private:
   const struct tgsi_full_instruction *insn;
};

unsigned int Instruction::srcMask(unsigned int s) const
{
   unsigned int mask = insn->Dst[0].Register.WriteMask;

   switch (insn->Instruction.Opcode) {
   case TGSI_OPCODE_COS:
   case TGSI_OPCODE_SIN:
      return (mask & 0x8) | ((mask & 0x7) ? 0x1 : 0x0);
   case TGSI_OPCODE_DP2:
      return 0x3;
   case TGSI_OPCODE_DP3:
      return 0x7;
   case TGSI_OPCODE_DP4:
   case TGSI_OPCODE_DPH:
   case TGSI_OPCODE_KILL_IF: /* WriteMask ignored */
      return 0xf;
   case TGSI_OPCODE_DST:
      return mask & (s ? 0xa : 0x6);
   case TGSI_OPCODE_EX2:
   case TGSI_OPCODE_EXP:
   case TGSI_OPCODE_LG2:
   case TGSI_OPCODE_LOG:
   case TGSI_OPCODE_POW:
   case TGSI_OPCODE_RCP:
   case TGSI_OPCODE_RSQ:
   case TGSI_OPCODE_SCS:
      return 0x1;
   case TGSI_OPCODE_IF:
   case TGSI_OPCODE_UIF:
      return 0x1;
   case TGSI_OPCODE_LIT:
      return 0xb;
   case TGSI_OPCODE_TEX2:
   case TGSI_OPCODE_TXB2:
   case TGSI_OPCODE_TXL2:
      return (s == 0) ? 0xf : 0x3;
   case TGSI_OPCODE_TEX:
   case TGSI_OPCODE_TXB:
   case TGSI_OPCODE_TXD:
   case TGSI_OPCODE_TXL:
   case TGSI_OPCODE_TXP:
   case TGSI_OPCODE_LODQ:
   {
      const struct tgsi_instruction_texture *tex = &insn->Texture;

      assert(insn->Instruction.Texture);

      mask = 0x7;
      if (insn->Instruction.Opcode != TGSI_OPCODE_TEX &&
          insn->Instruction.Opcode != TGSI_OPCODE_TXD)
         mask |= 0x8; /* bias, lod or proj */

      switch (tex->Texture) {
      case TGSI_TEXTURE_1D:
         mask &= 0x9;
         break;
      case TGSI_TEXTURE_SHADOW1D:
         mask &= 0xd;
         break;
      case TGSI_TEXTURE_1D_ARRAY:
      case TGSI_TEXTURE_2D:
      case TGSI_TEXTURE_RECT:
         mask &= 0xb;
         break;
      case TGSI_TEXTURE_CUBE_ARRAY:
      case TGSI_TEXTURE_SHADOW2D_ARRAY:
      case TGSI_TEXTURE_SHADOWCUBE:
      case TGSI_TEXTURE_SHADOWCUBE_ARRAY:
         mask |= 0x8;
         break;
      default:
         break;
      }
   }
      return mask;
   case TGSI_OPCODE_XPD:
   {
      unsigned int x = 0;
      if (mask & 1) x |= 0x6;
      if (mask & 2) x |= 0x5;
      if (mask & 4) x |= 0x3;
      return x;
   }
   default:
      break;
   }

   return mask;
}

nv50_ir::Modifier Instruction::SrcRegister::getMod(int chan) const
{
   nv50_ir::Modifier m(0);

   if (reg.Absolute)
      m = m | nv50_ir::Modifier(NV50_IR_MOD_ABS);
   if (reg.Negate)
      m = m | nv50_ir::Modifier(NV50_IR_MOD_NEG);
   return m;
}

static nv50_ir::DataFile translateFile(uint file)
{
   switch (file) {
   case TGSI_FILE_CONSTANT:        return nv50_ir::FILE_MEMORY_CONST;
   case TGSI_FILE_INPUT:           return nv50_ir::FILE_SHADER_INPUT;
   case TGSI_FILE_OUTPUT:          return nv50_ir::FILE_SHADER_OUTPUT;
   case TGSI_FILE_TEMPORARY:       return nv50_ir::FILE_GPR;
   case TGSI_FILE_ADDRESS:         return nv50_ir::FILE_ADDRESS;
   case TGSI_FILE_PREDICATE:       return nv50_ir::FILE_PREDICATE;
   case TGSI_FILE_IMMEDIATE:       return nv50_ir::FILE_IMMEDIATE;
   case TGSI_FILE_SYSTEM_VALUE:    return nv50_ir::FILE_SYSTEM_VALUE;
   case TGSI_FILE_RESOURCE:        return nv50_ir::FILE_MEMORY_GLOBAL;
   case TGSI_FILE_SAMPLER:
   case TGSI_FILE_NULL:
   default:
      return nv50_ir::FILE_NULL;
   }
}

static nv50_ir::SVSemantic translateSysVal(uint sysval)
{
   switch (sysval) {
   case TGSI_SEMANTIC_FACE:       return nv50_ir::SV_FACE;
   case TGSI_SEMANTIC_PSIZE:      return nv50_ir::SV_POINT_SIZE;
   case TGSI_SEMANTIC_PRIMID:     return nv50_ir::SV_PRIMITIVE_ID;
   case TGSI_SEMANTIC_INSTANCEID: return nv50_ir::SV_INSTANCE_ID;
   case TGSI_SEMANTIC_VERTEXID:   return nv50_ir::SV_VERTEX_ID;
   case TGSI_SEMANTIC_GRID_SIZE:  return nv50_ir::SV_NCTAID;
   case TGSI_SEMANTIC_BLOCK_ID:   return nv50_ir::SV_CTAID;
   case TGSI_SEMANTIC_BLOCK_SIZE: return nv50_ir::SV_NTID;
   case TGSI_SEMANTIC_THREAD_ID:  return nv50_ir::SV_TID;
   case TGSI_SEMANTIC_SAMPLEID:   return nv50_ir::SV_SAMPLE_INDEX;
   case TGSI_SEMANTIC_SAMPLEPOS:  return nv50_ir::SV_SAMPLE_POS;
   case TGSI_SEMANTIC_SAMPLEMASK: return nv50_ir::SV_SAMPLE_MASK;
   case TGSI_SEMANTIC_INVOCATIONID: return nv50_ir::SV_INVOCATION_ID;
   default:
      assert(0);
      return nv50_ir::SV_CLOCK;
   }
}

#define NV50_IR_TEX_TARG_CASE(a, b) \
   case TGSI_TEXTURE_##a: return nv50_ir::TEX_TARGET_##b;

static nv50_ir::TexTarget translateTexture(uint tex)
{
   switch (tex) {
   NV50_IR_TEX_TARG_CASE(1D, 1D);
   NV50_IR_TEX_TARG_CASE(2D, 2D);
   NV50_IR_TEX_TARG_CASE(2D_MSAA, 2D_MS);
   NV50_IR_TEX_TARG_CASE(3D, 3D);
   NV50_IR_TEX_TARG_CASE(CUBE, CUBE);
   NV50_IR_TEX_TARG_CASE(RECT, RECT);
   NV50_IR_TEX_TARG_CASE(1D_ARRAY, 1D_ARRAY);
   NV50_IR_TEX_TARG_CASE(2D_ARRAY, 2D_ARRAY);
   NV50_IR_TEX_TARG_CASE(2D_ARRAY_MSAA, 2D_MS_ARRAY);
   NV50_IR_TEX_TARG_CASE(CUBE_ARRAY, CUBE_ARRAY);
   NV50_IR_TEX_TARG_CASE(SHADOW1D, 1D_SHADOW);
   NV50_IR_TEX_TARG_CASE(SHADOW2D, 2D_SHADOW);
   NV50_IR_TEX_TARG_CASE(SHADOWCUBE, CUBE_SHADOW);
   NV50_IR_TEX_TARG_CASE(SHADOWRECT, RECT_SHADOW);
   NV50_IR_TEX_TARG_CASE(SHADOW1D_ARRAY, 1D_ARRAY_SHADOW);
   NV50_IR_TEX_TARG_CASE(SHADOW2D_ARRAY, 2D_ARRAY_SHADOW);
   NV50_IR_TEX_TARG_CASE(SHADOWCUBE_ARRAY, CUBE_ARRAY_SHADOW);
   NV50_IR_TEX_TARG_CASE(BUFFER, BUFFER);

   case TGSI_TEXTURE_UNKNOWN:
   default:
      assert(!"invalid texture target");
      return nv50_ir::TEX_TARGET_2D;
   }
}

nv50_ir::DataType Instruction::inferSrcType() const
{
   switch (getOpcode()) {
   case TGSI_OPCODE_UIF:
   case TGSI_OPCODE_AND:
   case TGSI_OPCODE_OR:
   case TGSI_OPCODE_XOR:
   case TGSI_OPCODE_NOT:
   case TGSI_OPCODE_U2F:
   case TGSI_OPCODE_UADD:
   case TGSI_OPCODE_UDIV:
   case TGSI_OPCODE_UMOD:
   case TGSI_OPCODE_UMAD:
   case TGSI_OPCODE_UMUL:
   case TGSI_OPCODE_UMUL_HI:
   case TGSI_OPCODE_UMAX:
   case TGSI_OPCODE_UMIN:
   case TGSI_OPCODE_USEQ:
   case TGSI_OPCODE_USGE:
   case TGSI_OPCODE_USLT:
   case TGSI_OPCODE_USNE:
   case TGSI_OPCODE_USHR:
   case TGSI_OPCODE_UCMP:
   case TGSI_OPCODE_ATOMUADD:
   case TGSI_OPCODE_ATOMXCHG:
   case TGSI_OPCODE_ATOMCAS:
   case TGSI_OPCODE_ATOMAND:
   case TGSI_OPCODE_ATOMOR:
   case TGSI_OPCODE_ATOMXOR:
   case TGSI_OPCODE_ATOMUMIN:
   case TGSI_OPCODE_ATOMUMAX:
   case TGSI_OPCODE_UBFE:
   case TGSI_OPCODE_UMSB:
      return nv50_ir::TYPE_U32;
   case TGSI_OPCODE_I2F:
   case TGSI_OPCODE_IDIV:
   case TGSI_OPCODE_IMUL_HI:
   case TGSI_OPCODE_IMAX:
   case TGSI_OPCODE_IMIN:
   case TGSI_OPCODE_IABS:
   case TGSI_OPCODE_INEG:
   case TGSI_OPCODE_ISGE:
   case TGSI_OPCODE_ISHR:
   case TGSI_OPCODE_ISLT:
   case TGSI_OPCODE_ISSG:
   case TGSI_OPCODE_SAD: // not sure about SAD, but no one has a float version
   case TGSI_OPCODE_MOD:
   case TGSI_OPCODE_UARL:
   case TGSI_OPCODE_ATOMIMIN:
   case TGSI_OPCODE_ATOMIMAX:
   case TGSI_OPCODE_IBFE:
   case TGSI_OPCODE_IMSB:
      return nv50_ir::TYPE_S32;
   default:
      return nv50_ir::TYPE_F32;
   }
}

nv50_ir::DataType Instruction::inferDstType() const
{
   switch (getOpcode()) {
   case TGSI_OPCODE_F2U: return nv50_ir::TYPE_U32;
   case TGSI_OPCODE_F2I: return nv50_ir::TYPE_S32;
   case TGSI_OPCODE_FSEQ:
   case TGSI_OPCODE_FSGE:
   case TGSI_OPCODE_FSLT:
   case TGSI_OPCODE_FSNE:
      return nv50_ir::TYPE_U32;
   case TGSI_OPCODE_I2F:
   case TGSI_OPCODE_U2F:
      return nv50_ir::TYPE_F32;
   default:
      return inferSrcType();
   }
}

nv50_ir::CondCode Instruction::getSetCond() const
{
   using namespace nv50_ir;

   switch (getOpcode()) {
   case TGSI_OPCODE_SLT:
   case TGSI_OPCODE_ISLT:
   case TGSI_OPCODE_USLT:
   case TGSI_OPCODE_FSLT:
      return CC_LT;
   case TGSI_OPCODE_SLE:
      return CC_LE;
   case TGSI_OPCODE_SGE:
   case TGSI_OPCODE_ISGE:
   case TGSI_OPCODE_USGE:
   case TGSI_OPCODE_FSGE:
      return CC_GE;
   case TGSI_OPCODE_SGT:
      return CC_GT;
   case TGSI_OPCODE_SEQ:
   case TGSI_OPCODE_USEQ:
   case TGSI_OPCODE_FSEQ:
      return CC_EQ;
   case TGSI_OPCODE_SNE:
   case TGSI_OPCODE_FSNE:
      return CC_NEU;
   case TGSI_OPCODE_USNE:
      return CC_NE;
   default:
      return CC_ALWAYS;
   }
}

#define NV50_IR_OPCODE_CASE(a, b) case TGSI_OPCODE_##a: return nv50_ir::OP_##b

static nv50_ir::operation translateOpcode(uint opcode)
{
   switch (opcode) {
   NV50_IR_OPCODE_CASE(ARL, SHL);
   NV50_IR_OPCODE_CASE(MOV, MOV);

   NV50_IR_OPCODE_CASE(RCP, RCP);
   NV50_IR_OPCODE_CASE(RSQ, RSQ);

   NV50_IR_OPCODE_CASE(MUL, MUL);
   NV50_IR_OPCODE_CASE(ADD, ADD);

   NV50_IR_OPCODE_CASE(MIN, MIN);
   NV50_IR_OPCODE_CASE(MAX, MAX);
   NV50_IR_OPCODE_CASE(SLT, SET);
   NV50_IR_OPCODE_CASE(SGE, SET);
   NV50_IR_OPCODE_CASE(MAD, MAD);
   NV50_IR_OPCODE_CASE(SUB, SUB);

   NV50_IR_OPCODE_CASE(FLR, FLOOR);
   NV50_IR_OPCODE_CASE(ROUND, CVT);
   NV50_IR_OPCODE_CASE(EX2, EX2);
   NV50_IR_OPCODE_CASE(LG2, LG2);
   NV50_IR_OPCODE_CASE(POW, POW);

   NV50_IR_OPCODE_CASE(ABS, ABS);

   NV50_IR_OPCODE_CASE(COS, COS);
   NV50_IR_OPCODE_CASE(DDX, DFDX);
   NV50_IR_OPCODE_CASE(DDX_FINE, DFDX);
   NV50_IR_OPCODE_CASE(DDY, DFDY);
   NV50_IR_OPCODE_CASE(DDY_FINE, DFDY);
   NV50_IR_OPCODE_CASE(KILL, DISCARD);

   NV50_IR_OPCODE_CASE(SEQ, SET);
   NV50_IR_OPCODE_CASE(SGT, SET);
   NV50_IR_OPCODE_CASE(SIN, SIN);
   NV50_IR_OPCODE_CASE(SLE, SET);
   NV50_IR_OPCODE_CASE(SNE, SET);
   NV50_IR_OPCODE_CASE(TEX, TEX);
   NV50_IR_OPCODE_CASE(TXD, TXD);
   NV50_IR_OPCODE_CASE(TXP, TEX);

   NV50_IR_OPCODE_CASE(CAL, CALL);
   NV50_IR_OPCODE_CASE(RET, RET);
   NV50_IR_OPCODE_CASE(CMP, SLCT);

   NV50_IR_OPCODE_CASE(TXB, TXB);

   NV50_IR_OPCODE_CASE(DIV, DIV);

   NV50_IR_OPCODE_CASE(TXL, TXL);

   NV50_IR_OPCODE_CASE(CEIL, CEIL);
   NV50_IR_OPCODE_CASE(I2F, CVT);
   NV50_IR_OPCODE_CASE(NOT, NOT);
   NV50_IR_OPCODE_CASE(TRUNC, TRUNC);
   NV50_IR_OPCODE_CASE(SHL, SHL);

   NV50_IR_OPCODE_CASE(AND, AND);
   NV50_IR_OPCODE_CASE(OR, OR);
   NV50_IR_OPCODE_CASE(MOD, MOD);
   NV50_IR_OPCODE_CASE(XOR, XOR);
   NV50_IR_OPCODE_CASE(SAD, SAD);
   NV50_IR_OPCODE_CASE(TXF, TXF);
   NV50_IR_OPCODE_CASE(TXQ, TXQ);
   NV50_IR_OPCODE_CASE(TG4, TXG);
   NV50_IR_OPCODE_CASE(LODQ, TXLQ);

   NV50_IR_OPCODE_CASE(EMIT, EMIT);
   NV50_IR_OPCODE_CASE(ENDPRIM, RESTART);

   NV50_IR_OPCODE_CASE(KILL_IF, DISCARD);

   NV50_IR_OPCODE_CASE(F2I, CVT);
   NV50_IR_OPCODE_CASE(FSEQ, SET);
   NV50_IR_OPCODE_CASE(FSGE, SET);
   NV50_IR_OPCODE_CASE(FSLT, SET);
   NV50_IR_OPCODE_CASE(FSNE, SET);
   NV50_IR_OPCODE_CASE(IDIV, DIV);
   NV50_IR_OPCODE_CASE(IMAX, MAX);
   NV50_IR_OPCODE_CASE(IMIN, MIN);
   NV50_IR_OPCODE_CASE(IABS, ABS);
   NV50_IR_OPCODE_CASE(INEG, NEG);
   NV50_IR_OPCODE_CASE(ISGE, SET);
   NV50_IR_OPCODE_CASE(ISHR, SHR);
   NV50_IR_OPCODE_CASE(ISLT, SET);
   NV50_IR_OPCODE_CASE(F2U, CVT);
   NV50_IR_OPCODE_CASE(U2F, CVT);
   NV50_IR_OPCODE_CASE(UADD, ADD);
   NV50_IR_OPCODE_CASE(UDIV, DIV);
   NV50_IR_OPCODE_CASE(UMAD, MAD);
   NV50_IR_OPCODE_CASE(UMAX, MAX);
   NV50_IR_OPCODE_CASE(UMIN, MIN);
   NV50_IR_OPCODE_CASE(UMOD, MOD);
   NV50_IR_OPCODE_CASE(UMUL, MUL);
   NV50_IR_OPCODE_CASE(USEQ, SET);
   NV50_IR_OPCODE_CASE(USGE, SET);
   NV50_IR_OPCODE_CASE(USHR, SHR);
   NV50_IR_OPCODE_CASE(USLT, SET);
   NV50_IR_OPCODE_CASE(USNE, SET);

   NV50_IR_OPCODE_CASE(IMUL_HI, MUL);
   NV50_IR_OPCODE_CASE(UMUL_HI, MUL);

   NV50_IR_OPCODE_CASE(SAMPLE, TEX);
   NV50_IR_OPCODE_CASE(SAMPLE_B, TXB);
   NV50_IR_OPCODE_CASE(SAMPLE_C, TEX);
   NV50_IR_OPCODE_CASE(SAMPLE_C_LZ, TEX);
   NV50_IR_OPCODE_CASE(SAMPLE_D, TXD);
   NV50_IR_OPCODE_CASE(SAMPLE_L, TXL);
   NV50_IR_OPCODE_CASE(SAMPLE_I, TXF);
   NV50_IR_OPCODE_CASE(SAMPLE_I_MS, TXF);
   NV50_IR_OPCODE_CASE(GATHER4, TXG);
   NV50_IR_OPCODE_CASE(SVIEWINFO, TXQ);

   NV50_IR_OPCODE_CASE(ATOMUADD, ATOM);
   NV50_IR_OPCODE_CASE(ATOMXCHG, ATOM);
   NV50_IR_OPCODE_CASE(ATOMCAS, ATOM);
   NV50_IR_OPCODE_CASE(ATOMAND, ATOM);
   NV50_IR_OPCODE_CASE(ATOMOR, ATOM);
   NV50_IR_OPCODE_CASE(ATOMXOR, ATOM);
   NV50_IR_OPCODE_CASE(ATOMUMIN, ATOM);
   NV50_IR_OPCODE_CASE(ATOMUMAX, ATOM);
   NV50_IR_OPCODE_CASE(ATOMIMIN, ATOM);
   NV50_IR_OPCODE_CASE(ATOMIMAX, ATOM);

   NV50_IR_OPCODE_CASE(TEX2, TEX);
   NV50_IR_OPCODE_CASE(TXB2, TXB);
   NV50_IR_OPCODE_CASE(TXL2, TXL);

   NV50_IR_OPCODE_CASE(IBFE, EXTBF);
   NV50_IR_OPCODE_CASE(UBFE, EXTBF);
   NV50_IR_OPCODE_CASE(BFI, INSBF);
   NV50_IR_OPCODE_CASE(BREV, EXTBF);
   NV50_IR_OPCODE_CASE(POPC, POPCNT);
   NV50_IR_OPCODE_CASE(LSB, BFIND);
   NV50_IR_OPCODE_CASE(IMSB, BFIND);
   NV50_IR_OPCODE_CASE(UMSB, BFIND);

   NV50_IR_OPCODE_CASE(END, EXIT);

   default:
      return nv50_ir::OP_NOP;
   }
}

static uint16_t opcodeToSubOp(uint opcode)
{
   switch (opcode) {
   case TGSI_OPCODE_LFENCE:   return NV50_IR_SUBOP_MEMBAR(L, GL);
   case TGSI_OPCODE_SFENCE:   return NV50_IR_SUBOP_MEMBAR(S, GL);
   case TGSI_OPCODE_MFENCE:   return NV50_IR_SUBOP_MEMBAR(M, GL);
   case TGSI_OPCODE_ATOMUADD: return NV50_IR_SUBOP_ATOM_ADD;
   case TGSI_OPCODE_ATOMXCHG: return NV50_IR_SUBOP_ATOM_EXCH;
   case TGSI_OPCODE_ATOMCAS:  return NV50_IR_SUBOP_ATOM_CAS;
   case TGSI_OPCODE_ATOMAND:  return NV50_IR_SUBOP_ATOM_AND;
   case TGSI_OPCODE_ATOMOR:   return NV50_IR_SUBOP_ATOM_OR;
   case TGSI_OPCODE_ATOMXOR:  return NV50_IR_SUBOP_ATOM_XOR;
   case TGSI_OPCODE_ATOMUMIN: return NV50_IR_SUBOP_ATOM_MIN;
   case TGSI_OPCODE_ATOMIMIN: return NV50_IR_SUBOP_ATOM_MIN;
   case TGSI_OPCODE_ATOMUMAX: return NV50_IR_SUBOP_ATOM_MAX;
   case TGSI_OPCODE_ATOMIMAX: return NV50_IR_SUBOP_ATOM_MAX;
   case TGSI_OPCODE_IMUL_HI:
   case TGSI_OPCODE_UMUL_HI:
      return NV50_IR_SUBOP_MUL_HIGH;
   default:
      return 0;
   }
}

bool Instruction::checkDstSrcAliasing() const
{
   if (insn->Dst[0].Register.Indirect) // no danger if indirect, using memory
      return false;

   for (int s = 0; s < TGSI_FULL_MAX_SRC_REGISTERS; ++s) {
      if (insn->Src[s].Register.File == TGSI_FILE_NULL)
         break;
      if (insn->Src[s].Register.File == insn->Dst[0].Register.File &&
          insn->Src[s].Register.Index == insn->Dst[0].Register.Index)
         return true;
   }
   return false;
}

class Source
{
public:
   Source(struct nv50_ir_prog_info *);
   ~Source();

public:
   bool scanSource();
   unsigned fileSize(unsigned file) const { return scan.file_max[file] + 1; }

public:
   struct tgsi_shader_info scan;
   struct tgsi_full_instruction *insns;
   const struct tgsi_token *tokens;
   struct nv50_ir_prog_info *info;

   nv50_ir::DynArray tempArrays;
   nv50_ir::DynArray immdArrays;

   typedef nv50_ir::BuildUtil::Location Location;
   // these registers are per-subroutine, cannot be used for parameter passing
   std::set<Location> locals;

   bool mainTempsInLMem;

   int clipVertexOutput;

   struct TextureView {
      uint8_t target; // TGSI_TEXTURE_*
   };
   std::vector<TextureView> textureViews;

   struct Resource {
      uint8_t target; // TGSI_TEXTURE_*
      bool raw;
      uint8_t slot; // $surface index
   };
   std::vector<Resource> resources;

private:
   int inferSysValDirection(unsigned sn) const;
   bool scanDeclaration(const struct tgsi_full_declaration *);
   bool scanInstruction(const struct tgsi_full_instruction *);
   void scanProperty(const struct tgsi_full_property *);
   void scanImmediate(const struct tgsi_full_immediate *);

   inline bool isEdgeFlagPassthrough(const Instruction&) const;
};

Source::Source(struct nv50_ir_prog_info *prog) : info(prog)
{
   tokens = (const struct tgsi_token *)info->bin.source;

   if (prog->dbgFlags & NV50_IR_DEBUG_BASIC)
      tgsi_dump(tokens, 0);

   mainTempsInLMem = FALSE;
}

Source::~Source()
{
   if (insns)
      FREE(insns);

   if (info->immd.data)
      FREE(info->immd.data);
   if (info->immd.type)
      FREE(info->immd.type);
}

bool Source::scanSource()
{
   unsigned insnCount = 0;
   struct tgsi_parse_context parse;

   tgsi_scan_shader(tokens, &scan);

   insns = (struct tgsi_full_instruction *)MALLOC(scan.num_instructions *
                                                  sizeof(insns[0]));
   if (!insns)
      return false;

   clipVertexOutput = -1;

   textureViews.resize(scan.file_max[TGSI_FILE_SAMPLER_VIEW] + 1);
   resources.resize(scan.file_max[TGSI_FILE_RESOURCE] + 1);

   info->immd.bufSize = 0;

   info->numInputs = scan.file_max[TGSI_FILE_INPUT] + 1;
   info->numOutputs = scan.file_max[TGSI_FILE_OUTPUT] + 1;
   info->numSysVals = scan.file_max[TGSI_FILE_SYSTEM_VALUE] + 1;

   if (info->type == PIPE_SHADER_FRAGMENT) {
      info->prop.fp.writesDepth = scan.writes_z;
      info->prop.fp.usesDiscard = scan.uses_kill;
   } else
   if (info->type == PIPE_SHADER_GEOMETRY) {
      info->prop.gp.instanceCount = 1; // default value
   }

   info->io.viewportId = -1;

   info->immd.data = (uint32_t *)MALLOC(scan.immediate_count * 16);
   info->immd.type = (ubyte *)MALLOC(scan.immediate_count * sizeof(ubyte));

   tgsi_parse_init(&parse, tokens);
   while (!tgsi_parse_end_of_tokens(&parse)) {
      tgsi_parse_token(&parse);

      switch (parse.FullToken.Token.Type) {
      case TGSI_TOKEN_TYPE_IMMEDIATE:
         scanImmediate(&parse.FullToken.FullImmediate);
         break;
      case TGSI_TOKEN_TYPE_DECLARATION:
         scanDeclaration(&parse.FullToken.FullDeclaration);
         break;
      case TGSI_TOKEN_TYPE_INSTRUCTION:
         insns[insnCount++] = parse.FullToken.FullInstruction;
         scanInstruction(&parse.FullToken.FullInstruction);
         break;
      case TGSI_TOKEN_TYPE_PROPERTY:
         scanProperty(&parse.FullToken.FullProperty);
         break;
      default:
         INFO("unknown TGSI token type: %d\n", parse.FullToken.Token.Type);
         break;
      }
   }
   tgsi_parse_free(&parse);

   if (mainTempsInLMem)
      info->bin.tlsSpace += (scan.file_max[TGSI_FILE_TEMPORARY] + 1) * 16;

   if (info->io.genUserClip > 0) {
      info->io.clipDistanceMask = (1 << info->io.genUserClip) - 1;

      const unsigned int nOut = (info->io.genUserClip + 3) / 4;

      for (unsigned int n = 0; n < nOut; ++n) {
         unsigned int i = info->numOutputs++;
         info->out[i].id = i;
         info->out[i].sn = TGSI_SEMANTIC_CLIPDIST;
         info->out[i].si = n;
         info->out[i].mask = info->io.clipDistanceMask >> (n * 4);
      }
   }

   return info->assignSlots(info) == 0;
}

void Source::scanProperty(const struct tgsi_full_property *prop)
{
   switch (prop->Property.PropertyName) {
   case TGSI_PROPERTY_GS_OUTPUT_PRIM:
      info->prop.gp.outputPrim = prop->u[0].Data;
      break;
   case TGSI_PROPERTY_GS_INPUT_PRIM:
      info->prop.gp.inputPrim = prop->u[0].Data;
      break;
   case TGSI_PROPERTY_GS_MAX_OUTPUT_VERTICES:
      info->prop.gp.maxVertices = prop->u[0].Data;
      break;
   case TGSI_PROPERTY_GS_INVOCATIONS:
      info->prop.gp.instanceCount = prop->u[0].Data;
      break;
   case TGSI_PROPERTY_FS_COLOR0_WRITES_ALL_CBUFS:
      info->prop.fp.separateFragData = TRUE;
      break;
   case TGSI_PROPERTY_FS_COORD_ORIGIN:
   case TGSI_PROPERTY_FS_COORD_PIXEL_CENTER:
      // we don't care
      break;
   case TGSI_PROPERTY_VS_PROHIBIT_UCPS:
      info->io.genUserClip = -1;
      break;
   default:
      INFO("unhandled TGSI property %d\n", prop->Property.PropertyName);
      break;
   }
}

void Source::scanImmediate(const struct tgsi_full_immediate *imm)
{
   const unsigned n = info->immd.count++;

   assert(n < scan.immediate_count);

   for (int c = 0; c < 4; ++c)
      info->immd.data[n * 4 + c] = imm->u[c].Uint;

   info->immd.type[n] = imm->Immediate.DataType;
}

int Source::inferSysValDirection(unsigned sn) const
{
   switch (sn) {
   case TGSI_SEMANTIC_INSTANCEID:
   case TGSI_SEMANTIC_VERTEXID:
      return 1;
   case TGSI_SEMANTIC_LAYER:
#if 0
   case TGSI_SEMANTIC_VIEWPORTINDEX:
      return 0;
#endif
   case TGSI_SEMANTIC_PRIMID:
      return (info->type == PIPE_SHADER_FRAGMENT) ? 1 : 0;
   default:
      return 0;
   }
}

bool Source::scanDeclaration(const struct tgsi_full_declaration *decl)
{
   unsigned i, c;
   unsigned sn = TGSI_SEMANTIC_GENERIC;
   unsigned si = 0;
   const unsigned first = decl->Range.First, last = decl->Range.Last;

   if (decl->Declaration.Semantic) {
      sn = decl->Semantic.Name;
      si = decl->Semantic.Index;
   }

   if (decl->Declaration.Local) {
      for (i = first; i <= last; ++i) {
         for (c = 0; c < 4; ++c) {
            locals.insert(
               Location(decl->Declaration.File, decl->Dim.Index2D, i, c));
         }
      }
   }

   switch (decl->Declaration.File) {
   case TGSI_FILE_INPUT:
      if (info->type == PIPE_SHADER_VERTEX) {
         // all vertex attributes are equal
         for (i = first; i <= last; ++i) {
            info->in[i].sn = TGSI_SEMANTIC_GENERIC;
            info->in[i].si = i;
         }
      } else {
         for (i = first; i <= last; ++i, ++si) {
            info->in[i].id = i;
            info->in[i].sn = sn;
            info->in[i].si = si;
            if (info->type == PIPE_SHADER_FRAGMENT) {
               // translate interpolation mode
               switch (decl->Interp.Interpolate) {
               case TGSI_INTERPOLATE_CONSTANT:
                  info->in[i].flat = 1;
                  break;
               case TGSI_INTERPOLATE_COLOR:
                  info->in[i].sc = 1;
                  break;
               case TGSI_INTERPOLATE_LINEAR:
                  info->in[i].linear = 1;
                  break;
               default:
                  break;
               }
               if (decl->Interp.Location || info->io.sampleInterp)
                  info->in[i].centroid = 1;
            }
         }
      }
      break;
   case TGSI_FILE_OUTPUT:
      for (i = first; i <= last; ++i, ++si) {
         switch (sn) {
         case TGSI_SEMANTIC_POSITION:
            if (info->type == PIPE_SHADER_FRAGMENT)
               info->io.fragDepth = i;
            else
            if (clipVertexOutput < 0)
               clipVertexOutput = i;
            break;
         case TGSI_SEMANTIC_COLOR:
            if (info->type == PIPE_SHADER_FRAGMENT)
               info->prop.fp.numColourResults++;
            break;
         case TGSI_SEMANTIC_EDGEFLAG:
            info->io.edgeFlagOut = i;
            break;
         case TGSI_SEMANTIC_CLIPVERTEX:
            clipVertexOutput = i;
            break;
         case TGSI_SEMANTIC_CLIPDIST:
            info->io.clipDistanceMask |=
               decl->Declaration.UsageMask << (si * 4);
            info->io.genUserClip = -1;
            break;
         case TGSI_SEMANTIC_SAMPLEMASK:
            info->io.sampleMask = i;
            break;
         case TGSI_SEMANTIC_VIEWPORT_INDEX:
            info->io.viewportId = i;
            break;
         default:
            break;
         }
         info->out[i].id = i;
         info->out[i].sn = sn;
         info->out[i].si = si;
      }
      break;
   case TGSI_FILE_SYSTEM_VALUE:
      switch (sn) {
      case TGSI_SEMANTIC_INSTANCEID:
         info->io.instanceId = first;
         break;
      case TGSI_SEMANTIC_VERTEXID:
         info->io.vertexId = first;
         break;
      default:
         break;
      }
      for (i = first; i <= last; ++i, ++si) {
         info->sv[i].sn = sn;
         info->sv[i].si = si;
         info->sv[i].input = inferSysValDirection(sn);
      }
      break;
   case TGSI_FILE_RESOURCE:
      for (i = first; i <= last; ++i) {
         resources[i].target = decl->Resource.Resource;
         resources[i].raw = decl->Resource.Raw;
         resources[i].slot = i;
      }
      break;
   case TGSI_FILE_SAMPLER_VIEW:
      for (i = first; i <= last; ++i)
         textureViews[i].target = decl->SamplerView.Resource;
      break;
   case TGSI_FILE_NULL:
   case TGSI_FILE_TEMPORARY:
   case TGSI_FILE_ADDRESS:
   case TGSI_FILE_CONSTANT:
   case TGSI_FILE_IMMEDIATE:
   case TGSI_FILE_PREDICATE:
   case TGSI_FILE_SAMPLER:
      break;
   default:
      ERROR("unhandled TGSI_FILE %d\n", decl->Declaration.File);
      return false;
   }
   return true;
}

inline bool Source::isEdgeFlagPassthrough(const Instruction& insn) const
{
   return insn.getOpcode() == TGSI_OPCODE_MOV &&
      insn.getDst(0).getIndex(0) == info->io.edgeFlagOut &&
      insn.getSrc(0).getFile() == TGSI_FILE_INPUT;
}

bool Source::scanInstruction(const struct tgsi_full_instruction *inst)
{
   Instruction insn(inst);

   if (insn.getOpcode() == TGSI_OPCODE_BARRIER)
      info->numBarriers = 1;

   if (insn.dstCount()) {
      if (insn.getDst(0).getFile() == TGSI_FILE_OUTPUT) {
         Instruction::DstRegister dst = insn.getDst(0);

         if (dst.isIndirect(0))
            for (unsigned i = 0; i < info->numOutputs; ++i)
               info->out[i].mask = 0xf;
         else
            info->out[dst.getIndex(0)].mask |= dst.getMask();

         if (info->out[dst.getIndex(0)].sn == TGSI_SEMANTIC_PSIZE ||
             info->out[dst.getIndex(0)].sn == TGSI_SEMANTIC_PRIMID ||
             info->out[dst.getIndex(0)].sn == TGSI_SEMANTIC_LAYER ||
             info->out[dst.getIndex(0)].sn == TGSI_SEMANTIC_VIEWPORT_INDEX ||
             info->out[dst.getIndex(0)].sn == TGSI_SEMANTIC_FOG)
            info->out[dst.getIndex(0)].mask &= 1;

         if (isEdgeFlagPassthrough(insn))
            info->io.edgeFlagIn = insn.getSrc(0).getIndex(0);
      } else
      if (insn.getDst(0).getFile() == TGSI_FILE_TEMPORARY) {
         if (insn.getDst(0).isIndirect(0))
            mainTempsInLMem = TRUE;
      }
   }

   for (unsigned s = 0; s < insn.srcCount(); ++s) {
      Instruction::SrcRegister src = insn.getSrc(s);
      if (src.getFile() == TGSI_FILE_TEMPORARY) {
         if (src.isIndirect(0))
            mainTempsInLMem = TRUE;
      } else
      if (src.getFile() == TGSI_FILE_RESOURCE) {
         if (src.getIndex(0) == TGSI_RESOURCE_GLOBAL)
            info->io.globalAccess |= (insn.getOpcode() == TGSI_OPCODE_LOAD) ?
               0x1 : 0x2;
      }
      if (src.getFile() != TGSI_FILE_INPUT)
         continue;
      unsigned mask = insn.srcMask(s);

      if (src.isIndirect(0)) {
         for (unsigned i = 0; i < info->numInputs; ++i)
            info->in[i].mask = 0xf;
      } else {
         const int i = src.getIndex(0);
         for (unsigned c = 0; c < 4; ++c) {
            if (!(mask & (1 << c)))
               continue;
            int k = src.getSwizzle(c);
            if (k <= TGSI_SWIZZLE_W)
               info->in[i].mask |= 1 << k;
         }
         switch (info->in[i].sn) {
         case TGSI_SEMANTIC_PSIZE:
         case TGSI_SEMANTIC_PRIMID:
         case TGSI_SEMANTIC_FOG:
            info->in[i].mask &= 0x1;
            break;
         case TGSI_SEMANTIC_PCOORD:
            info->in[i].mask &= 0x3;
            break;
         default:
            break;
         }
      }
   }
   return true;
}

nv50_ir::TexInstruction::Target
Instruction::getTexture(const tgsi::Source *code, int s) const
{
   // XXX: indirect access
   unsigned int r;

   switch (getSrc(s).getFile()) {
   case TGSI_FILE_RESOURCE:
      r = getSrc(s).getIndex(0);
      return translateTexture(code->resources.at(r).target);
   case TGSI_FILE_SAMPLER_VIEW:
      r = getSrc(s).getIndex(0);
      return translateTexture(code->textureViews.at(r).target);
   default:
      return translateTexture(insn->Texture.Texture);
   }
}

} // namespace tgsi

namespace {

using namespace nv50_ir;

class Converter : public BuildUtil
{
public:
   Converter(Program *, const tgsi::Source *);
   ~Converter();

   bool run();

private:
   struct Subroutine
   {
      Subroutine(Function *f) : f(f) { }
      Function *f;
      ValueMap values;
   };

   Value *shiftAddress(Value *);
   Value *getVertexBase(int s);
   DataArray *getArrayForFile(unsigned file, int idx);
   Value *fetchSrc(int s, int c);
   Value *acquireDst(int d, int c);
   void storeDst(int d, int c, Value *);

   Value *fetchSrc(const tgsi::Instruction::SrcRegister src, int c, Value *ptr);
   void storeDst(const tgsi::Instruction::DstRegister dst, int c,
                 Value *val, Value *ptr);

   Value *applySrcMod(Value *, int s, int c);

   Symbol *makeSym(uint file, int fileIndex, int idx, int c, uint32_t addr);
   Symbol *srcToSym(tgsi::Instruction::SrcRegister, int c);
   Symbol *dstToSym(tgsi::Instruction::DstRegister, int c);

   bool handleInstruction(const struct tgsi_full_instruction *);
   void exportOutputs();
   inline Subroutine *getSubroutine(unsigned ip);
   inline Subroutine *getSubroutine(Function *);
   inline bool isEndOfSubroutine(uint ip);

   void loadProjTexCoords(Value *dst[4], Value *src[4], unsigned int mask);

   // R,S,L,C,Dx,Dy encode TGSI sources for respective values (0xSf for auto)
   void setTexRS(TexInstruction *, unsigned int& s, int R, int S);
   void handleTEX(Value *dst0[4], int R, int S, int L, int C, int Dx, int Dy);
   void handleTXF(Value *dst0[4], int R, int L_M);
   void handleTXQ(Value *dst0[4], enum TexQuery);
   void handleLIT(Value *dst0[4]);
   void handleUserClipPlanes();

   Symbol *getResourceBase(int r);
   void getResourceCoords(std::vector<Value *>&, int r, int s);

   void handleLOAD(Value *dst0[4]);
   void handleSTORE();
   void handleATOM(Value *dst0[4], DataType, uint16_t subOp);

   void handleINTERP(Value *dst0[4]);

   Value *interpolate(tgsi::Instruction::SrcRegister, int c, Value *ptr);

   void insertConvergenceOps(BasicBlock *conv, BasicBlock *fork);

   Value *buildDot(int dim);

   class BindArgumentsPass : public Pass {
   public:
      BindArgumentsPass(Converter &conv) : conv(conv) { }

   private:
      Converter &conv;
      Subroutine *sub;

      inline const Location *getValueLocation(Subroutine *, Value *);

      template<typename T> inline void
      updateCallArgs(Instruction *i, void (Instruction::*setArg)(int, Value *),
                     T (Function::*proto));

      template<typename T> inline void
      updatePrototype(BitSet *set, void (Function::*updateSet)(),
                      T (Function::*proto));

   protected:
      bool visit(Function *);
      bool visit(BasicBlock *bb) { return false; }
   };

private:
   const struct tgsi::Source *code;
   const struct nv50_ir_prog_info *info;

   struct {
      std::map<unsigned, Subroutine> map;
      Subroutine *cur;
   } sub;

   uint ip; // instruction pointer

   tgsi::Instruction tgsi;

   DataType dstTy;
   DataType srcTy;

   DataArray tData; // TGSI_FILE_TEMPORARY
   DataArray aData; // TGSI_FILE_ADDRESS
   DataArray pData; // TGSI_FILE_PREDICATE
   DataArray oData; // TGSI_FILE_OUTPUT (if outputs in registers)

   Value *zero;
   Value *fragCoord[4];
   Value *clipVtx[4];

   Value *vtxBase[5]; // base address of vertex in primitive (for TP/GP)
   uint8_t vtxBaseValid;

   Stack condBBs;  // fork BB, then else clause BB
   Stack joinBBs;  // fork BB, for inserting join ops on ENDIF
   Stack loopBBs;  // loop headers
   Stack breakBBs; // end of / after loop

   Value *viewport;
};

Symbol *
Converter::srcToSym(tgsi::Instruction::SrcRegister src, int c)
{
   const int swz = src.getSwizzle(c);

   return makeSym(src.getFile(),
                  src.is2D() ? src.getIndex(1) : 0,
                  src.isIndirect(0) ? -1 : src.getIndex(0), swz,
                  src.getIndex(0) * 16 + swz * 4);
}

Symbol *
Converter::dstToSym(tgsi::Instruction::DstRegister dst, int c)
{
   return makeSym(dst.getFile(),
                  dst.is2D() ? dst.getIndex(1) : 0,
                  dst.isIndirect(0) ? -1 : dst.getIndex(0), c,
                  dst.getIndex(0) * 16 + c * 4);
}

Symbol *
Converter::makeSym(uint tgsiFile, int fileIdx, int idx, int c, uint32_t address)
{
   Symbol *sym = new_Symbol(prog, tgsi::translateFile(tgsiFile));

   sym->reg.fileIndex = fileIdx;

   if (idx >= 0) {
      if (sym->reg.file == FILE_SHADER_INPUT)
         sym->setOffset(info->in[idx].slot[c] * 4);
      else
      if (sym->reg.file == FILE_SHADER_OUTPUT)
         sym->setOffset(info->out[idx].slot[c] * 4);
      else
      if (sym->reg.file == FILE_SYSTEM_VALUE)
         sym->setSV(tgsi::translateSysVal(info->sv[idx].sn), c);
      else
         sym->setOffset(address);
   } else {
      sym->setOffset(address);
   }
   return sym;
}

static inline uint8_t
translateInterpMode(const struct nv50_ir_varying *var, operation& op)
{
   uint8_t mode = NV50_IR_INTERP_PERSPECTIVE;

   if (var->flat)
      mode = NV50_IR_INTERP_FLAT;
   else
   if (var->linear)
      mode = NV50_IR_INTERP_LINEAR;
   else
   if (var->sc)
      mode = NV50_IR_INTERP_SC;

   op = (mode == NV50_IR_INTERP_PERSPECTIVE || mode == NV50_IR_INTERP_SC)
      ? OP_PINTERP : OP_LINTERP;

   if (var->centroid)
      mode |= NV50_IR_INTERP_CENTROID;

   return mode;
}

Value *
Converter::interpolate(tgsi::Instruction::SrcRegister src, int c, Value *ptr)
{
   operation op;

   // XXX: no way to know interpolation mode if we don't know what's accessed
   const uint8_t mode = translateInterpMode(&info->in[ptr ? 0 :
                                                      src.getIndex(0)], op);

   Instruction *insn = new_Instruction(func, op, TYPE_F32);

   insn->setDef(0, getScratch());
   insn->setSrc(0, srcToSym(src, c));
   if (op == OP_PINTERP)
      insn->setSrc(1, fragCoord[3]);
   if (ptr)
      insn->setIndirect(0, 0, ptr);

   insn->setInterpolate(mode);

   bb->insertTail(insn);
   return insn->getDef(0);
}

Value *
Converter::applySrcMod(Value *val, int s, int c)
{
   Modifier m = tgsi.getSrc(s).getMod(c);
   DataType ty = tgsi.inferSrcType();

   if (m & Modifier(NV50_IR_MOD_ABS))
      val = mkOp1v(OP_ABS, ty, getScratch(), val);

   if (m & Modifier(NV50_IR_MOD_NEG))
      val = mkOp1v(OP_NEG, ty, getScratch(), val);

   return val;
}

Value *
Converter::getVertexBase(int s)
{
   assert(s < 5);
   if (!(vtxBaseValid & (1 << s))) {
      const int index = tgsi.getSrc(s).getIndex(1);
      Value *rel = NULL;
      if (tgsi.getSrc(s).isIndirect(1))
         rel = fetchSrc(tgsi.getSrc(s).getIndirect(1), 0, NULL);
      vtxBaseValid |= 1 << s;
      vtxBase[s] = mkOp2v(OP_PFETCH, TYPE_U32, getSSA(4, FILE_ADDRESS),
                          mkImm(index), rel);
   }
   return vtxBase[s];
}

Value *
Converter::fetchSrc(int s, int c)
{
   Value *res;
   Value *ptr = NULL, *dimRel = NULL;

   tgsi::Instruction::SrcRegister src = tgsi.getSrc(s);

   if (src.isIndirect(0))
      ptr = fetchSrc(src.getIndirect(0), 0, NULL);

   if (src.is2D()) {
      switch (src.getFile()) {
      case TGSI_FILE_INPUT:
         dimRel = getVertexBase(s);
         break;
      case TGSI_FILE_CONSTANT:
         // on NVC0, this is valid and c{I+J}[k] == cI[(J << 16) + k]
         if (src.isIndirect(1))
            dimRel = fetchSrc(src.getIndirect(1), 0, 0);
         break;
      default:
         break;
      }
   }

   res = fetchSrc(src, c, ptr);

   if (dimRel)
      res->getInsn()->setIndirect(0, 1, dimRel);

   return applySrcMod(res, s, c);
}

Converter::DataArray *
Converter::getArrayForFile(unsigned file, int idx)
{
   switch (file) {
   case TGSI_FILE_TEMPORARY:
      return &tData;
   case TGSI_FILE_PREDICATE:
      return &pData;
   case TGSI_FILE_ADDRESS:
      return &aData;
   case TGSI_FILE_OUTPUT:
      assert(prog->getType() == Program::TYPE_FRAGMENT);
      return &oData;
   default:
      assert(!"invalid/unhandled TGSI source file");
      return NULL;
   }
}

Value *
Converter::shiftAddress(Value *index)
{
   if (!index)
      return NULL;
   return mkOp2v(OP_SHL, TYPE_U32, getSSA(4, FILE_ADDRESS), index, mkImm(4));
}

Value *
Converter::fetchSrc(tgsi::Instruction::SrcRegister src, int c, Value *ptr)
{
   const int idx2d = src.is2D() ? src.getIndex(1) : 0;
   const int idx = src.getIndex(0);
   const int swz = src.getSwizzle(c);

   switch (src.getFile()) {
   case TGSI_FILE_IMMEDIATE:
      assert(!ptr);
      return loadImm(NULL, info->immd.data[idx * 4 + swz]);
   case TGSI_FILE_CONSTANT:
      return mkLoadv(TYPE_U32, srcToSym(src, c), shiftAddress(ptr));
   case TGSI_FILE_INPUT:
      if (prog->getType() == Program::TYPE_FRAGMENT) {
         // don't load masked inputs, won't be assigned a slot
         if (!ptr && !(info->in[idx].mask & (1 << swz)))
            return loadImm(NULL, swz == TGSI_SWIZZLE_W ? 1.0f : 0.0f);
	 if (!ptr && info->in[idx].sn == TGSI_SEMANTIC_FACE)
            return mkOp1v(OP_RDSV, TYPE_F32, getSSA(), mkSysVal(SV_FACE, 0));
         return interpolate(src, c, shiftAddress(ptr));
      } else
      if (prog->getType() == Program::TYPE_GEOMETRY) {
         if (!ptr && info->in[idx].sn == TGSI_SEMANTIC_PRIMID)
            return mkOp1v(OP_RDSV, TYPE_U32, getSSA(), mkSysVal(SV_PRIMITIVE_ID, 0));
         // XXX: This is going to be a problem with scalar arrays, i.e. when
         // we cannot assume that the address is given in units of vec4.
         //
         // nv50 and nvc0 need different things here, so let the lowering
         // passes decide what to do with the address
         if (ptr)
            return mkLoadv(TYPE_U32, srcToSym(src, c), ptr);
      }
      return mkLoadv(TYPE_U32, srcToSym(src, c), shiftAddress(ptr));
   case TGSI_FILE_OUTPUT:
      assert(!"load from output file");
      return NULL;
   case TGSI_FILE_SYSTEM_VALUE:
      assert(!ptr);
      return mkOp1v(OP_RDSV, TYPE_U32, getSSA(), srcToSym(src, c));
   default:
      return getArrayForFile(src.getFile(), idx2d)->load(
         sub.cur->values, idx, swz, shiftAddress(ptr));
   }
}

Value *
Converter::acquireDst(int d, int c)
{
   const tgsi::Instruction::DstRegister dst = tgsi.getDst(d);
   const unsigned f = dst.getFile();
   const int idx = dst.getIndex(0);
   const int idx2d = dst.is2D() ? dst.getIndex(1) : 0;

   if (dst.isMasked(c) || f == TGSI_FILE_RESOURCE)
      return NULL;

   if (dst.isIndirect(0) ||
       f == TGSI_FILE_SYSTEM_VALUE ||
       (f == TGSI_FILE_OUTPUT && prog->getType() != Program::TYPE_FRAGMENT))
      return getScratch();

   return getArrayForFile(f, idx2d)-> acquire(sub.cur->values, idx, c);
}

void
Converter::storeDst(int d, int c, Value *val)
{
   const tgsi::Instruction::DstRegister dst = tgsi.getDst(d);

   switch (tgsi.getSaturate()) {
   case TGSI_SAT_NONE:
      break;
   case TGSI_SAT_ZERO_ONE:
      mkOp1(OP_SAT, dstTy, val, val);
      break;
   case TGSI_SAT_MINUS_PLUS_ONE:
      mkOp2(OP_MAX, dstTy, val, val, mkImm(-1.0f));
      mkOp2(OP_MIN, dstTy, val, val, mkImm(+1.0f));
      break;
   default:
      assert(!"invalid saturation mode");
      break;
   }

   Value *ptr = NULL;
   if (dst.isIndirect(0))
      ptr = shiftAddress(fetchSrc(dst.getIndirect(0), 0, NULL));

   if (info->io.genUserClip > 0 &&
       dst.getFile() == TGSI_FILE_OUTPUT &&
       !dst.isIndirect(0) && dst.getIndex(0) == code->clipVertexOutput) {
      mkMov(clipVtx[c], val);
      val = clipVtx[c];
   }

   storeDst(dst, c, val, ptr);
}

void
Converter::storeDst(const tgsi::Instruction::DstRegister dst, int c,
                    Value *val, Value *ptr)
{
   const unsigned f = dst.getFile();
   const int idx = dst.getIndex(0);
   const int idx2d = dst.is2D() ? dst.getIndex(1) : 0;

   if (f == TGSI_FILE_SYSTEM_VALUE) {
      assert(!ptr);
      mkOp2(OP_WRSV, TYPE_U32, NULL, dstToSym(dst, c), val);
   } else
   if (f == TGSI_FILE_OUTPUT && prog->getType() != Program::TYPE_FRAGMENT) {

      if (ptr || (info->out[idx].mask & (1 << c))) {
         /* Save the viewport index into a scratch register so that it can be
            exported at EMIT time */
         if (info->out[idx].sn == TGSI_SEMANTIC_VIEWPORT_INDEX &&
             viewport != NULL)
            mkOp1(OP_MOV, TYPE_U32, viewport, val);
         else
            mkStore(OP_EXPORT, TYPE_U32, dstToSym(dst, c), ptr, val);
      }
   } else
   if (f == TGSI_FILE_TEMPORARY ||
       f == TGSI_FILE_PREDICATE ||
       f == TGSI_FILE_ADDRESS ||
       f == TGSI_FILE_OUTPUT) {
      getArrayForFile(f, idx2d)->store(sub.cur->values, idx, c, ptr, val);
   } else {
      assert(!"invalid dst file");
   }
}

#define FOR_EACH_DST_ENABLED_CHANNEL(d, chan, inst) \
   for (chan = 0; chan < 4; ++chan)                 \
      if (!inst.getDst(d).isMasked(chan))

Value *
Converter::buildDot(int dim)
{
   assert(dim > 0);

   Value *src0 = fetchSrc(0, 0), *src1 = fetchSrc(1, 0);
   Value *dotp = getScratch();

   mkOp2(OP_MUL, TYPE_F32, dotp, src0, src1);

   for (int c = 1; c < dim; ++c) {
      src0 = fetchSrc(0, c);
      src1 = fetchSrc(1, c);
      mkOp3(OP_MAD, TYPE_F32, dotp, src0, src1, dotp);
   }
   return dotp;
}

void
Converter::insertConvergenceOps(BasicBlock *conv, BasicBlock *fork)
{
   FlowInstruction *join = new_FlowInstruction(func, OP_JOIN, NULL);
   join->fixed = 1;
   conv->insertHead(join);

   fork->joinAt = new_FlowInstruction(func, OP_JOINAT, conv);
   fork->insertBefore(fork->getExit(), fork->joinAt);
}

void
Converter::setTexRS(TexInstruction *tex, unsigned int& s, int R, int S)
{
   unsigned rIdx = 0, sIdx = 0;

   if (R >= 0)
      rIdx = tgsi.getSrc(R).getIndex(0);
   if (S >= 0)
      sIdx = tgsi.getSrc(S).getIndex(0);

   tex->setTexture(tgsi.getTexture(code, R), rIdx, sIdx);

   if (tgsi.getSrc(R).isIndirect(0)) {
      tex->tex.rIndirectSrc = s;
      tex->setSrc(s++, fetchSrc(tgsi.getSrc(R).getIndirect(0), 0, NULL));
   }
   if (S >= 0 && tgsi.getSrc(S).isIndirect(0)) {
      tex->tex.sIndirectSrc = s;
      tex->setSrc(s++, fetchSrc(tgsi.getSrc(S).getIndirect(0), 0, NULL));
   }
}

void
Converter::handleTXQ(Value *dst0[4], enum TexQuery query)
{
   TexInstruction *tex = new_TexInstruction(func, OP_TXQ);
   tex->tex.query = query;
   unsigned int c, d;

   for (d = 0, c = 0; c < 4; ++c) {
      if (!dst0[c])
         continue;
      tex->tex.mask |= 1 << c;
      tex->setDef(d++, dst0[c]);
   }
   tex->setSrc((c = 0), fetchSrc(0, 0)); // mip level

   setTexRS(tex, c, 1, -1);

   bb->insertTail(tex);
}

void
Converter::loadProjTexCoords(Value *dst[4], Value *src[4], unsigned int mask)
{
   Value *proj = fetchSrc(0, 3);
   Instruction *insn = proj->getUniqueInsn();
   int c;

   if (insn->op == OP_PINTERP) {
      bb->insertTail(insn = cloneForward(func, insn));
      insn->op = OP_LINTERP;
      insn->setInterpolate(NV50_IR_INTERP_LINEAR | insn->getSampleMode());
      insn->setSrc(1, NULL);
      proj = insn->getDef(0);
   }
   proj = mkOp1v(OP_RCP, TYPE_F32, getSSA(), proj);

   for (c = 0; c < 4; ++c) {
      if (!(mask & (1 << c)))
         continue;
      if ((insn = src[c]->getUniqueInsn())->op != OP_PINTERP)
         continue;
      mask &= ~(1 << c);

      bb->insertTail(insn = cloneForward(func, insn));
      insn->setInterpolate(NV50_IR_INTERP_PERSPECTIVE | insn->getSampleMode());
      insn->setSrc(1, proj);
      dst[c] = insn->getDef(0);
   }
   if (!mask)
      return;

   proj = mkOp1v(OP_RCP, TYPE_F32, getSSA(), fetchSrc(0, 3));

   for (c = 0; c < 4; ++c)
      if (mask & (1 << c))
         dst[c] = mkOp2v(OP_MUL, TYPE_F32, getSSA(), src[c], proj);
}

// order of nv50 ir sources: x y z layer lod/bias shadow
// order of TGSI TEX sources: x y z layer shadow lod/bias
//  lowering will finally set the hw specific order (like array first on nvc0)
void
Converter::handleTEX(Value *dst[4], int R, int S, int L, int C, int Dx, int Dy)
{
   Value *val;
   Value *arg[4], *src[8];
   Value *lod = NULL, *shd = NULL;
   unsigned int s, c, d;
   TexInstruction *texi = new_TexInstruction(func, tgsi.getOP());

   TexInstruction::Target tgt = tgsi.getTexture(code, R);

   for (s = 0; s < tgt.getArgCount(); ++s)
      arg[s] = src[s] = fetchSrc(0, s);

   if (texi->op == OP_TXL || texi->op == OP_TXB)
      lod = fetchSrc(L >> 4, L & 3);

   if (C == 0x0f)
      C = 0x00 | MAX2(tgt.getArgCount(), 2); // guess DC src

   if (tgsi.getOpcode() == TGSI_OPCODE_TG4 &&
       tgt == TEX_TARGET_CUBE_ARRAY_SHADOW)
      shd = fetchSrc(1, 0);
   else if (tgt.isShadow())
      shd = fetchSrc(C >> 4, C & 3);

   if (texi->op == OP_TXD) {
      for (c = 0; c < tgt.getDim(); ++c) {
         texi->dPdx[c].set(fetchSrc(Dx >> 4, (Dx & 3) + c));
         texi->dPdy[c].set(fetchSrc(Dy >> 4, (Dy & 3) + c));
      }
   }

   // cube textures don't care about projection value, it's divided out
   if (tgsi.getOpcode() == TGSI_OPCODE_TXP && !tgt.isCube() && !tgt.isArray()) {
      unsigned int n = tgt.getDim();
      if (shd) {
         arg[n] = shd;
         ++n;
         assert(tgt.getDim() == tgt.getArgCount());
      }
      loadProjTexCoords(src, arg, (1 << n) - 1);
      if (shd)
         shd = src[n - 1];
   }

   if (tgt.isCube()) {
      for (c = 0; c < 3; ++c)
         src[c] = mkOp1v(OP_ABS, TYPE_F32, getSSA(), arg[c]);
      val = getScratch();
      mkOp2(OP_MAX, TYPE_F32, val, src[0], src[1]);
      mkOp2(OP_MAX, TYPE_F32, val, src[2], val);
      mkOp1(OP_RCP, TYPE_F32, val, val);
      for (c = 0; c < 3; ++c)
         src[c] = mkOp2v(OP_MUL, TYPE_F32, getSSA(), arg[c], val);
   }

   for (c = 0, d = 0; c < 4; ++c) {
      if (dst[c]) {
         texi->setDef(d++, dst[c]);
         texi->tex.mask |= 1 << c;
      } else {
         // NOTE: maybe hook up def too, for CSE
      }
   }
   for (s = 0; s < tgt.getArgCount(); ++s)
      texi->setSrc(s, src[s]);
   if (lod)
      texi->setSrc(s++, lod);
   if (shd)
      texi->setSrc(s++, shd);

   setTexRS(texi, s, R, S);

   if (tgsi.getOpcode() == TGSI_OPCODE_SAMPLE_C_LZ)
      texi->tex.levelZero = true;
   if (tgsi.getOpcode() == TGSI_OPCODE_TG4 && !tgt.isShadow())
      texi->tex.gatherComp = tgsi.getSrc(1).getValueU32(0, info);

   texi->tex.useOffsets = tgsi.getNumTexOffsets();
   for (s = 0; s < tgsi.getNumTexOffsets(); ++s) {
      for (c = 0; c < 3; ++c) {
         texi->offset[s][c].set(fetchSrc(tgsi.getTexOffset(s), c, NULL));
         texi->offset[s][c].setInsn(texi);
      }
   }

   bb->insertTail(texi);
}

// 1st source: xyz = coordinates, w = lod/sample
// 2nd source: offset
void
Converter::handleTXF(Value *dst[4], int R, int L_M)
{
   TexInstruction *texi = new_TexInstruction(func, tgsi.getOP());
   int ms;
   unsigned int c, d, s;

   texi->tex.target = tgsi.getTexture(code, R);

   ms = texi->tex.target.isMS() ? 1 : 0;
   texi->tex.levelZero = ms; /* MS textures don't have mip-maps */

   for (c = 0, d = 0; c < 4; ++c) {
      if (dst[c]) {
         texi->setDef(d++, dst[c]);
         texi->tex.mask |= 1 << c;
      }
   }
   for (c = 0; c < (texi->tex.target.getArgCount() - ms); ++c)
      texi->setSrc(c, fetchSrc(0, c));
   texi->setSrc(c++, fetchSrc(L_M >> 4, L_M & 3)); // lod or ms

   setTexRS(texi, c, R, -1);

   texi->tex.useOffsets = tgsi.getNumTexOffsets();
   for (s = 0; s < tgsi.getNumTexOffsets(); ++s) {
      for (c = 0; c < 3; ++c) {
         texi->offset[s][c].set(fetchSrc(tgsi.getTexOffset(s), c, NULL));
         texi->offset[s][c].setInsn(texi);
      }
   }

   bb->insertTail(texi);
}

void
Converter::handleLIT(Value *dst0[4])
{
   Value *val0 = NULL;
   unsigned int mask = tgsi.getDst(0).getMask();

   if (mask & (1 << 0))
      loadImm(dst0[0], 1.0f);

   if (mask & (1 << 3))
      loadImm(dst0[3], 1.0f);

   if (mask & (3 << 1)) {
      val0 = getScratch();
      mkOp2(OP_MAX, TYPE_F32, val0, fetchSrc(0, 0), zero);
      if (mask & (1 << 1))
         mkMov(dst0[1], val0);
   }

   if (mask & (1 << 2)) {
      Value *src1 = fetchSrc(0, 1), *src3 = fetchSrc(0, 3);
      Value *val1 = getScratch(), *val3 = getScratch();

      Value *pos128 = loadImm(NULL, +127.999999f);
      Value *neg128 = loadImm(NULL, -127.999999f);

      mkOp2(OP_MAX, TYPE_F32, val1, src1, zero);
      mkOp2(OP_MAX, TYPE_F32, val3, src3, neg128);
      mkOp2(OP_MIN, TYPE_F32, val3, val3, pos128);
      mkOp2(OP_POW, TYPE_F32, val3, val1, val3);

      mkCmp(OP_SLCT, CC_GT, TYPE_F32, dst0[2], TYPE_F32, val3, zero, val0);
   }
}

static inline bool
isResourceSpecial(const int r)
{
   return (r == TGSI_RESOURCE_GLOBAL ||
           r == TGSI_RESOURCE_LOCAL ||
           r == TGSI_RESOURCE_PRIVATE ||
           r == TGSI_RESOURCE_INPUT);
}

static inline bool
isResourceRaw(const struct tgsi::Source *code, const int r)
{
   return isResourceSpecial(r) || code->resources[r].raw;
}

static inline nv50_ir::TexTarget
getResourceTarget(const struct tgsi::Source *code, int r)
{
   if (isResourceSpecial(r))
      return nv50_ir::TEX_TARGET_BUFFER;
   return tgsi::translateTexture(code->resources.at(r).target);
}

Symbol *
Converter::getResourceBase(const int r)
{
   Symbol *sym = NULL;

   switch (r) {
   case TGSI_RESOURCE_GLOBAL:
      sym = new_Symbol(prog, nv50_ir::FILE_MEMORY_GLOBAL, 15);
      break;
   case TGSI_RESOURCE_LOCAL:
      assert(prog->getType() == Program::TYPE_COMPUTE);
      sym = mkSymbol(nv50_ir::FILE_MEMORY_SHARED, 0, TYPE_U32,
                     info->prop.cp.sharedOffset);
      break;
   case TGSI_RESOURCE_PRIVATE:
      sym = mkSymbol(nv50_ir::FILE_MEMORY_LOCAL, 0, TYPE_U32,
                     info->bin.tlsSpace);
      break;
   case TGSI_RESOURCE_INPUT:
      assert(prog->getType() == Program::TYPE_COMPUTE);
      sym = mkSymbol(nv50_ir::FILE_SHADER_INPUT, 0, TYPE_U32,
                     info->prop.cp.inputOffset);
      break;
   default:
      sym = new_Symbol(prog,
                       nv50_ir::FILE_MEMORY_GLOBAL, code->resources.at(r).slot);
      break;
   }
   return sym;
}

void
Converter::getResourceCoords(std::vector<Value *> &coords, int r, int s)
{
   const int arg =
      TexInstruction::Target(getResourceTarget(code, r)).getArgCount();

   for (int c = 0; c < arg; ++c)
      coords.push_back(fetchSrc(s, c));

   // NOTE: TGSI_RESOURCE_GLOBAL needs FILE_GPR; this is an nv50 quirk
   if (r == TGSI_RESOURCE_LOCAL ||
       r == TGSI_RESOURCE_PRIVATE ||
       r == TGSI_RESOURCE_INPUT)
      coords[0] = mkOp1v(OP_MOV, TYPE_U32, getScratch(4, FILE_ADDRESS),
                         coords[0]);
}

static inline int
partitionLoadStore(uint8_t comp[2], uint8_t size[2], uint8_t mask)
{
   int n = 0;

   while (mask) {
      if (mask & 1) {
         size[n]++;
      } else {
         if (size[n])
            comp[n = 1] = size[0] + 1;
         else
            comp[n]++;
      }
      mask >>= 1;
   }
   if (size[0] == 3) {
      n = 1;
      size[0] = (comp[0] == 1) ? 1 : 2;
      size[1] = 3 - size[0];
      comp[1] = comp[0] + size[0];
   }
   return n + 1;
}

// For raw loads, granularity is 4 byte.
// Usage of the texture read mask on OP_SULDP is not allowed.
void
Converter::handleLOAD(Value *dst0[4])
{
   const int r = tgsi.getSrc(0).getIndex(0);
   int c;
   std::vector<Value *> off, src, ldv, def;

   getResourceCoords(off, r, 1);

   if (isResourceRaw(code, r)) {
      uint8_t mask = 0;
      uint8_t comp[2] = { 0, 0 };
      uint8_t size[2] = { 0, 0 };

      Symbol *base = getResourceBase(r);

      // determine the base and size of the at most 2 load ops
      for (c = 0; c < 4; ++c)
         if (!tgsi.getDst(0).isMasked(c))
            mask |= 1 << (tgsi.getSrc(0).getSwizzle(c) - TGSI_SWIZZLE_X);

      int n = partitionLoadStore(comp, size, mask);

      src = off;

      def.resize(4); // index by component, the ones we need will be non-NULL
      for (c = 0; c < 4; ++c) {
         if (dst0[c] && tgsi.getSrc(0).getSwizzle(c) == (TGSI_SWIZZLE_X + c))
            def[c] = dst0[c];
         else
         if (mask & (1 << c))
            def[c] = getScratch();
      }

      const bool useLd = isResourceSpecial(r) ||
         (info->io.nv50styleSurfaces &&
          code->resources[r].target == TGSI_TEXTURE_BUFFER);

      for (int i = 0; i < n; ++i) {
         ldv.assign(def.begin() + comp[i], def.begin() + comp[i] + size[i]);

         if (comp[i]) // adjust x component of source address if necessary
            src[0] = mkOp2v(OP_ADD, TYPE_U32, getSSA(4, off[0]->reg.file),
                            off[0], mkImm(comp[i] * 4));
         else
            src[0] = off[0];

         if (useLd) {
            Instruction *ld =
               mkLoad(typeOfSize(size[i] * 4), ldv[0], base, src[0]);
            for (size_t c = 1; c < ldv.size(); ++c)
               ld->setDef(c, ldv[c]);
         } else {
            mkTex(OP_SULDB, getResourceTarget(code, r), code->resources[r].slot,
                  0, ldv, src)->dType = typeOfSize(size[i] * 4);
         }
      }
   } else {
      def.resize(4);
      for (c = 0; c < 4; ++c) {
         if (!dst0[c] || tgsi.getSrc(0).getSwizzle(c) != (TGSI_SWIZZLE_X + c))
            def[c] = getScratch();
         else
            def[c] = dst0[c];
      }

      mkTex(OP_SULDP, getResourceTarget(code, r), code->resources[r].slot, 0,
            def, off);
   }
   FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
      if (dst0[c] != def[c])
         mkMov(dst0[c], def[tgsi.getSrc(0).getSwizzle(c)]);
}

// For formatted stores, the write mask on OP_SUSTP can be used.
// Raw stores have to be split.
void
Converter::handleSTORE()
{
   const int r = tgsi.getDst(0).getIndex(0);
   int c;
   std::vector<Value *> off, src, dummy;

   getResourceCoords(off, r, 0);
   src = off;
   const int s = src.size();

   if (isResourceRaw(code, r)) {
      uint8_t comp[2] = { 0, 0 };
      uint8_t size[2] = { 0, 0 };

      int n = partitionLoadStore(comp, size, tgsi.getDst(0).getMask());

      Symbol *base = getResourceBase(r);

      const bool useSt = isResourceSpecial(r) ||
         (info->io.nv50styleSurfaces &&
          code->resources[r].target == TGSI_TEXTURE_BUFFER);

      for (int i = 0; i < n; ++i) {
         if (comp[i]) // adjust x component of source address if necessary
            src[0] = mkOp2v(OP_ADD, TYPE_U32, getSSA(4, off[0]->reg.file),
                            off[0], mkImm(comp[i] * 4));
         else
            src[0] = off[0];

         const DataType stTy = typeOfSize(size[i] * 4);

         if (useSt) {
            Instruction *st =
               mkStore(OP_STORE, stTy, base, NULL, fetchSrc(1, comp[i]));
            for (c = 1; c < size[i]; ++c)
               st->setSrc(1 + c, fetchSrc(1, comp[i] + c));
            st->setIndirect(0, 0, src[0]);
         } else {
            // attach values to be stored
            src.resize(s + size[i]);
            for (c = 0; c < size[i]; ++c)
               src[s + c] = fetchSrc(1, comp[i] + c);
            mkTex(OP_SUSTB, getResourceTarget(code, r), code->resources[r].slot,
                  0, dummy, src)->setType(stTy);
         }
      }
   } else {
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         src.push_back(fetchSrc(1, c));

      mkTex(OP_SUSTP, getResourceTarget(code, r), code->resources[r].slot, 0,
            dummy, src)->tex.mask = tgsi.getDst(0).getMask();
   }
}

// XXX: These only work on resources with the single-component u32/s32 formats.
// Therefore the result is replicated. This might not be intended by TGSI, but
// operating on more than 1 component would produce undefined results because
// they do not exist.
void
Converter::handleATOM(Value *dst0[4], DataType ty, uint16_t subOp)
{
   const int r = tgsi.getSrc(0).getIndex(0);
   std::vector<Value *> srcv;
   std::vector<Value *> defv;
   LValue *dst = getScratch();

   getResourceCoords(srcv, r, 1);

   if (isResourceSpecial(r)) {
      assert(r != TGSI_RESOURCE_INPUT);
      Instruction *insn;
      insn = mkOp2(OP_ATOM, ty, dst, getResourceBase(r), fetchSrc(2, 0));
      insn->subOp = subOp;
      if (subOp == NV50_IR_SUBOP_ATOM_CAS)
         insn->setSrc(2, fetchSrc(3, 0));
      insn->setIndirect(0, 0, srcv.at(0));
   } else {
      operation op = isResourceRaw(code, r) ? OP_SUREDB : OP_SUREDP;
      TexTarget targ = getResourceTarget(code, r);
      int idx = code->resources[r].slot;
      defv.push_back(dst);
      srcv.push_back(fetchSrc(2, 0));
      if (subOp == NV50_IR_SUBOP_ATOM_CAS)
         srcv.push_back(fetchSrc(3, 0));
      TexInstruction *tex = mkTex(op, targ, idx, 0, defv, srcv);
      tex->subOp = subOp;
      tex->tex.mask = 1;
      tex->setType(ty);
   }

   for (int c = 0; c < 4; ++c)
      if (dst0[c])
         dst0[c] = dst; // not equal to rDst so handleInstruction will do mkMov
}

void
Converter::handleINTERP(Value *dst[4])
{
   // Check whether the input is linear. All other attributes ignored.
   Instruction *insn;
   Value *offset = NULL, *ptr = NULL, *w = NULL;
   bool linear;
   operation op;
   int c, mode;

   tgsi::Instruction::SrcRegister src = tgsi.getSrc(0);
   assert(src.getFile() == TGSI_FILE_INPUT);

   if (src.isIndirect(0))
      ptr = fetchSrc(src.getIndirect(0), 0, NULL);

   // XXX: no way to know interp mode if we don't know the index
   linear = info->in[ptr ? 0 : src.getIndex(0)].linear;
   if (linear) {
      op = OP_LINTERP;
      mode = NV50_IR_INTERP_LINEAR;
   } else {
      op = OP_PINTERP;
      mode = NV50_IR_INTERP_PERSPECTIVE;
   }

   switch (tgsi.getOpcode()) {
   case TGSI_OPCODE_INTERP_CENTROID:
      mode |= NV50_IR_INTERP_CENTROID;
      break;
   case TGSI_OPCODE_INTERP_SAMPLE:
      insn = mkOp1(OP_PIXLD, TYPE_U32, (offset = getScratch()), fetchSrc(1, 0));
      insn->subOp = NV50_IR_SUBOP_PIXLD_OFFSET;
      mode |= NV50_IR_INTERP_OFFSET;
      break;
   case TGSI_OPCODE_INTERP_OFFSET: {
      // The input in src1.xy is float, but we need a single 32-bit value
      // where the upper and lower 16 bits are encoded in S0.12 format. We need
      // to clamp the input coordinates to (-0.5, 0.4375), multiply by 4096,
      // and then convert to s32.
      Value *offs[2];
      for (c = 0; c < 2; c++) {
         offs[c] = fetchSrc(1, c);
         mkOp2(OP_MIN, TYPE_F32, offs[c], offs[c], loadImm(NULL, 0.4375f));
         mkOp2(OP_MAX, TYPE_F32, offs[c], offs[c], loadImm(NULL, -0.5f));
         mkOp2(OP_MUL, TYPE_F32, offs[c], offs[c], loadImm(NULL, 4096.0f));
         mkCvt(OP_CVT, TYPE_S32, offs[c], TYPE_F32, offs[c]);
      }
      offset = mkOp3v(OP_INSBF, TYPE_U32, getScratch(),
                      offs[1], mkImm(0x1010), offs[0]);
      mode |= NV50_IR_INTERP_OFFSET;
      break;
   }
   }

   if (op == OP_PINTERP) {
      if (offset) {
         w = mkOp2v(OP_RDSV, TYPE_F32, getSSA(), mkSysVal(SV_POSITION, 3), offset);
         mkOp1(OP_RCP, TYPE_F32, w, w);
      } else {
         w = fragCoord[3];
      }
   }


   FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
      insn = mkOp1(op, TYPE_F32, dst[c], srcToSym(src, c));
      if (op == OP_PINTERP)
         insn->setSrc(1, w);
      if (ptr)
         insn->setIndirect(0, 0, ptr);
      if (offset)
         insn->setSrc(op == OP_PINTERP ? 2 : 1, offset);

      insn->setInterpolate(mode);
   }
}

Converter::Subroutine *
Converter::getSubroutine(unsigned ip)
{
   std::map<unsigned, Subroutine>::iterator it = sub.map.find(ip);

   if (it == sub.map.end())
      it = sub.map.insert(std::make_pair(
              ip, Subroutine(new Function(prog, "SUB", ip)))).first;

   return &it->second;
}

Converter::Subroutine *
Converter::getSubroutine(Function *f)
{
   unsigned ip = f->getLabel();
   std::map<unsigned, Subroutine>::iterator it = sub.map.find(ip);

   if (it == sub.map.end())
      it = sub.map.insert(std::make_pair(ip, Subroutine(f))).first;

   return &it->second;
}

bool
Converter::isEndOfSubroutine(uint ip)
{
   assert(ip < code->scan.num_instructions);
   tgsi::Instruction insn(&code->insns[ip]);
   return (insn.getOpcode() == TGSI_OPCODE_END ||
           insn.getOpcode() == TGSI_OPCODE_ENDSUB ||
           // does END occur at end of main or the very end ?
           insn.getOpcode() == TGSI_OPCODE_BGNSUB);
}

bool
Converter::handleInstruction(const struct tgsi_full_instruction *insn)
{
   Instruction *geni;

   Value *dst0[4], *rDst0[4];
   Value *src0, *src1, *src2, *src3;
   Value *val0, *val1;
   int c;

   tgsi = tgsi::Instruction(insn);

   bool useScratchDst = tgsi.checkDstSrcAliasing();

   operation op = tgsi.getOP();
   dstTy = tgsi.inferDstType();
   srcTy = tgsi.inferSrcType();

   unsigned int mask = tgsi.dstCount() ? tgsi.getDst(0).getMask() : 0;

   if (tgsi.dstCount()) {
      for (c = 0; c < 4; ++c) {
         rDst0[c] = acquireDst(0, c);
         dst0[c] = (useScratchDst && rDst0[c]) ? getScratch() : rDst0[c];
      }
   }

   switch (tgsi.getOpcode()) {
   case TGSI_OPCODE_ADD:
   case TGSI_OPCODE_UADD:
   case TGSI_OPCODE_AND:
   case TGSI_OPCODE_DIV:
   case TGSI_OPCODE_IDIV:
   case TGSI_OPCODE_UDIV:
   case TGSI_OPCODE_MAX:
   case TGSI_OPCODE_MIN:
   case TGSI_OPCODE_IMAX:
   case TGSI_OPCODE_IMIN:
   case TGSI_OPCODE_UMAX:
   case TGSI_OPCODE_UMIN:
   case TGSI_OPCODE_MOD:
   case TGSI_OPCODE_UMOD:
   case TGSI_OPCODE_MUL:
   case TGSI_OPCODE_UMUL:
   case TGSI_OPCODE_IMUL_HI:
   case TGSI_OPCODE_UMUL_HI:
   case TGSI_OPCODE_OR:
   case TGSI_OPCODE_SHL:
   case TGSI_OPCODE_ISHR:
   case TGSI_OPCODE_USHR:
   case TGSI_OPCODE_SUB:
   case TGSI_OPCODE_XOR:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         src1 = fetchSrc(1, c);
         geni = mkOp2(op, dstTy, dst0[c], src0, src1);
         geni->subOp = tgsi::opcodeToSubOp(tgsi.getOpcode());
      }
      break;
   case TGSI_OPCODE_MAD:
   case TGSI_OPCODE_UMAD:
   case TGSI_OPCODE_SAD:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         src1 = fetchSrc(1, c);
         src2 = fetchSrc(2, c);
         mkOp3(op, dstTy, dst0[c], src0, src1, src2);
      }
      break;
   case TGSI_OPCODE_MOV:
   case TGSI_OPCODE_ABS:
   case TGSI_OPCODE_CEIL:
   case TGSI_OPCODE_FLR:
   case TGSI_OPCODE_TRUNC:
   case TGSI_OPCODE_RCP:
   case TGSI_OPCODE_IABS:
   case TGSI_OPCODE_INEG:
   case TGSI_OPCODE_NOT:
   case TGSI_OPCODE_DDX:
   case TGSI_OPCODE_DDY:
   case TGSI_OPCODE_DDX_FINE:
   case TGSI_OPCODE_DDY_FINE:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkOp1(op, dstTy, dst0[c], fetchSrc(0, c));
      break;
   case TGSI_OPCODE_RSQ:
      src0 = fetchSrc(0, 0);
      val0 = getScratch();
      mkOp1(OP_ABS, TYPE_F32, val0, src0);
      mkOp1(OP_RSQ, TYPE_F32, val0, val0);
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkMov(dst0[c], val0);
      break;
   case TGSI_OPCODE_ARL:
   case TGSI_OPCODE_ARR:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         const RoundMode rnd =
            tgsi.getOpcode() == TGSI_OPCODE_ARR ? ROUND_N : ROUND_M;
         src0 = fetchSrc(0, c);
         mkCvt(OP_CVT, TYPE_S32, dst0[c], TYPE_F32, src0)->rnd = rnd;
      }
      break;
   case TGSI_OPCODE_UARL:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkOp1(OP_MOV, TYPE_U32, dst0[c], fetchSrc(0, c));
      break;
   case TGSI_OPCODE_POW:
      val0 = mkOp2v(op, TYPE_F32, getScratch(), fetchSrc(0, 0), fetchSrc(1, 0));
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkOp1(OP_MOV, TYPE_F32, dst0[c], val0);
      break;
   case TGSI_OPCODE_EX2:
   case TGSI_OPCODE_LG2:
      val0 = mkOp1(op, TYPE_F32, getScratch(), fetchSrc(0, 0))->getDef(0);
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkOp1(OP_MOV, TYPE_F32, dst0[c], val0);
      break;
   case TGSI_OPCODE_COS:
   case TGSI_OPCODE_SIN:
      val0 = getScratch();
      if (mask & 7) {
         mkOp1(OP_PRESIN, TYPE_F32, val0, fetchSrc(0, 0));
         mkOp1(op, TYPE_F32, val0, val0);
         for (c = 0; c < 3; ++c)
            if (dst0[c])
               mkMov(dst0[c], val0);
      }
      if (dst0[3]) {
         mkOp1(OP_PRESIN, TYPE_F32, val0, fetchSrc(0, 3));
         mkOp1(op, TYPE_F32, dst0[3], val0);
      }
      break;
   case TGSI_OPCODE_SCS:
      if (mask & 3) {
         val0 = mkOp1v(OP_PRESIN, TYPE_F32, getSSA(), fetchSrc(0, 0));
         if (dst0[0])
            mkOp1(OP_COS, TYPE_F32, dst0[0], val0);
         if (dst0[1])
            mkOp1(OP_SIN, TYPE_F32, dst0[1], val0);
      }
      if (dst0[2])
         loadImm(dst0[2], 0.0f);
      if (dst0[3])
         loadImm(dst0[3], 1.0f);
      break;
   case TGSI_OPCODE_EXP:
      src0 = fetchSrc(0, 0);
      val0 = mkOp1v(OP_FLOOR, TYPE_F32, getSSA(), src0);
      if (dst0[1])
         mkOp2(OP_SUB, TYPE_F32, dst0[1], src0, val0);
      if (dst0[0])
         mkOp1(OP_EX2, TYPE_F32, dst0[0], val0);
      if (dst0[2])
         mkOp1(OP_EX2, TYPE_F32, dst0[2], src0);
      if (dst0[3])
         loadImm(dst0[3], 1.0f);
      break;
   case TGSI_OPCODE_LOG:
      src0 = mkOp1v(OP_ABS, TYPE_F32, getSSA(), fetchSrc(0, 0));
      val0 = mkOp1v(OP_LG2, TYPE_F32, dst0[2] ? dst0[2] : getSSA(), src0);
      if (dst0[0] || dst0[1])
         val1 = mkOp1v(OP_FLOOR, TYPE_F32, dst0[0] ? dst0[0] : getSSA(), val0);
      if (dst0[1]) {
         mkOp1(OP_EX2, TYPE_F32, dst0[1], val1);
         mkOp1(OP_RCP, TYPE_F32, dst0[1], dst0[1]);
         mkOp2(OP_MUL, TYPE_F32, dst0[1], dst0[1], src0);
      }
      if (dst0[3])
         loadImm(dst0[3], 1.0f);
      break;
   case TGSI_OPCODE_DP2:
      val0 = buildDot(2);
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkMov(dst0[c], val0);
      break;
   case TGSI_OPCODE_DP3:
      val0 = buildDot(3);
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkMov(dst0[c], val0);
      break;
   case TGSI_OPCODE_DP4:
      val0 = buildDot(4);
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkMov(dst0[c], val0);
      break;
   case TGSI_OPCODE_DPH:
      val0 = buildDot(3);
      src1 = fetchSrc(1, 3);
      mkOp2(OP_ADD, TYPE_F32, val0, val0, src1);
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkMov(dst0[c], val0);
      break;
   case TGSI_OPCODE_DST:
      if (dst0[0])
         loadImm(dst0[0], 1.0f);
      if (dst0[1]) {
         src0 = fetchSrc(0, 1);
         src1 = fetchSrc(1, 1);
         mkOp2(OP_MUL, TYPE_F32, dst0[1], src0, src1);
      }
      if (dst0[2])
         mkMov(dst0[2], fetchSrc(0, 2));
      if (dst0[3])
         mkMov(dst0[3], fetchSrc(1, 3));
      break;
   case TGSI_OPCODE_LRP:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         src1 = fetchSrc(1, c);
         src2 = fetchSrc(2, c);
         mkOp3(OP_MAD, TYPE_F32, dst0[c],
               mkOp2v(OP_SUB, TYPE_F32, getSSA(), src1, src2), src0, src2);
      }
      break;
   case TGSI_OPCODE_LIT:
      handleLIT(dst0);
      break;
   case TGSI_OPCODE_XPD:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         if (c < 3) {
            val0 = getSSA();
            src0 = fetchSrc(1, (c + 1) % 3);
            src1 = fetchSrc(0, (c + 2) % 3);
            mkOp2(OP_MUL, TYPE_F32, val0, src0, src1);
            mkOp1(OP_NEG, TYPE_F32, val0, val0);

            src0 = fetchSrc(0, (c + 1) % 3);
            src1 = fetchSrc(1, (c + 2) % 3);
            mkOp3(OP_MAD, TYPE_F32, dst0[c], src0, src1, val0);
         } else {
            loadImm(dst0[c], 1.0f);
         }
      }
      break;
   case TGSI_OPCODE_ISSG:
   case TGSI_OPCODE_SSG:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         val0 = getScratch();
         val1 = getScratch();
         mkCmp(OP_SET, CC_GT, srcTy, val0, srcTy, src0, zero);
         mkCmp(OP_SET, CC_LT, srcTy, val1, srcTy, src0, zero);
         if (srcTy == TYPE_F32)
            mkOp2(OP_SUB, TYPE_F32, dst0[c], val0, val1);
         else
            mkOp2(OP_SUB, TYPE_S32, dst0[c], val1, val0);
      }
      break;
   case TGSI_OPCODE_UCMP:
   case TGSI_OPCODE_CMP:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         src1 = fetchSrc(1, c);
         src2 = fetchSrc(2, c);
         if (src1 == src2)
            mkMov(dst0[c], src1);
         else
            mkCmp(OP_SLCT, (srcTy == TYPE_F32) ? CC_LT : CC_NE,
                  srcTy, dst0[c], srcTy, src1, src2, src0);
      }
      break;
   case TGSI_OPCODE_FRC:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         val0 = getScratch();
         mkOp1(OP_FLOOR, TYPE_F32, val0, src0);
         mkOp2(OP_SUB, TYPE_F32, dst0[c], src0, val0);
      }
      break;
   case TGSI_OPCODE_ROUND:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkCvt(OP_CVT, TYPE_F32, dst0[c], TYPE_F32, fetchSrc(0, c))
         ->rnd = ROUND_NI;
      break;
   case TGSI_OPCODE_CLAMP:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         src1 = fetchSrc(1, c);
         src2 = fetchSrc(2, c);
         val0 = getScratch();
         mkOp2(OP_MIN, TYPE_F32, val0, src0, src1);
         mkOp2(OP_MAX, TYPE_F32, dst0[c], val0, src2);
      }
      break;
   case TGSI_OPCODE_SLT:
   case TGSI_OPCODE_SGE:
   case TGSI_OPCODE_SEQ:
   case TGSI_OPCODE_SGT:
   case TGSI_OPCODE_SLE:
   case TGSI_OPCODE_SNE:
   case TGSI_OPCODE_FSEQ:
   case TGSI_OPCODE_FSGE:
   case TGSI_OPCODE_FSLT:
   case TGSI_OPCODE_FSNE:
   case TGSI_OPCODE_ISGE:
   case TGSI_OPCODE_ISLT:
   case TGSI_OPCODE_USEQ:
   case TGSI_OPCODE_USGE:
   case TGSI_OPCODE_USLT:
   case TGSI_OPCODE_USNE:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         src1 = fetchSrc(1, c);
         mkCmp(op, tgsi.getSetCond(), dstTy, dst0[c], srcTy, src0, src1);
      }
      break;
   case TGSI_OPCODE_KILL_IF:
      val0 = new_LValue(func, FILE_PREDICATE);
      mask = 0;
      for (c = 0; c < 4; ++c) {
         const int s = tgsi.getSrc(0).getSwizzle(c);
         if (mask & (1 << s))
            continue;
         mask |= 1 << s;
         mkCmp(OP_SET, CC_LT, TYPE_F32, val0, TYPE_F32, fetchSrc(0, c), zero);
         mkOp(OP_DISCARD, TYPE_NONE, NULL)->setPredicate(CC_P, val0);
      }
      break;
   case TGSI_OPCODE_KILL:
      mkOp(OP_DISCARD, TYPE_NONE, NULL);
      break;
   case TGSI_OPCODE_TEX:
   case TGSI_OPCODE_TXB:
   case TGSI_OPCODE_TXL:
   case TGSI_OPCODE_TXP:
   case TGSI_OPCODE_LODQ:
      //              R  S     L     C    Dx    Dy
      handleTEX(dst0, 1, 1, 0x03, 0x0f, 0x00, 0x00);
      break;
   case TGSI_OPCODE_TXD:
      handleTEX(dst0, 3, 3, 0x03, 0x0f, 0x10, 0x20);
      break;
   case TGSI_OPCODE_TG4:
      handleTEX(dst0, 2, 2, 0x03, 0x0f, 0x00, 0x00);
      break;
   case TGSI_OPCODE_TEX2:
      handleTEX(dst0, 2, 2, 0x03, 0x10, 0x00, 0x00);
      break;
   case TGSI_OPCODE_TXB2:
   case TGSI_OPCODE_TXL2:
      handleTEX(dst0, 2, 2, 0x10, 0x0f, 0x00, 0x00);
      break;
   case TGSI_OPCODE_SAMPLE:
   case TGSI_OPCODE_SAMPLE_B:
   case TGSI_OPCODE_SAMPLE_D:
   case TGSI_OPCODE_SAMPLE_L:
   case TGSI_OPCODE_SAMPLE_C:
   case TGSI_OPCODE_SAMPLE_C_LZ:
      handleTEX(dst0, 1, 2, 0x30, 0x30, 0x30, 0x40);
      break;
   case TGSI_OPCODE_TXF:
      handleTXF(dst0, 1, 0x03);
      break;
   case TGSI_OPCODE_SAMPLE_I:
      handleTXF(dst0, 1, 0x03);
      break;
   case TGSI_OPCODE_SAMPLE_I_MS:
      handleTXF(dst0, 1, 0x20);
      break;
   case TGSI_OPCODE_TXQ:
   case TGSI_OPCODE_SVIEWINFO:
      handleTXQ(dst0, TXQ_DIMS);
      break;
   case TGSI_OPCODE_F2I:
   case TGSI_OPCODE_F2U:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkCvt(OP_CVT, dstTy, dst0[c], srcTy, fetchSrc(0, c))->rnd = ROUND_Z;
      break;
   case TGSI_OPCODE_I2F:
   case TGSI_OPCODE_U2F:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi)
         mkCvt(OP_CVT, dstTy, dst0[c], srcTy, fetchSrc(0, c));
      break;
   case TGSI_OPCODE_EMIT:
      /* export the saved viewport index */
      if (viewport != NULL) {
         Symbol *vpSym = mkSymbol(FILE_SHADER_OUTPUT, 0, TYPE_U32,
                                  info->out[info->io.viewportId].slot[0] * 4);
         mkStore(OP_EXPORT, TYPE_U32, vpSym, NULL, viewport);
      }
      /* fallthrough */
   case TGSI_OPCODE_ENDPRIM:
   {
      // get vertex stream (must be immediate)
      unsigned int stream = tgsi.getSrc(0).getValueU32(0, info);
      if (stream && op == OP_RESTART)
         break;
      src0 = mkImm(stream);
      mkOp1(op, TYPE_U32, NULL, src0)->fixed = 1;
      break;
   }
   case TGSI_OPCODE_IF:
   case TGSI_OPCODE_UIF:
   {
      BasicBlock *ifBB = new BasicBlock(func);

      bb->cfg.attach(&ifBB->cfg, Graph::Edge::TREE);
      condBBs.push(bb);
      joinBBs.push(bb);

      mkFlow(OP_BRA, NULL, CC_NOT_P, fetchSrc(0, 0))->setType(srcTy);

      setPosition(ifBB, true);
   }
      break;
   case TGSI_OPCODE_ELSE:
   {
      BasicBlock *elseBB = new BasicBlock(func);
      BasicBlock *forkBB = reinterpret_cast<BasicBlock *>(condBBs.pop().u.p);

      forkBB->cfg.attach(&elseBB->cfg, Graph::Edge::TREE);
      condBBs.push(bb);

      forkBB->getExit()->asFlow()->target.bb = elseBB;
      if (!bb->isTerminated())
         mkFlow(OP_BRA, NULL, CC_ALWAYS, NULL);

      setPosition(elseBB, true);
   }
      break;
   case TGSI_OPCODE_ENDIF:
   {
      BasicBlock *convBB = new BasicBlock(func);
      BasicBlock *prevBB = reinterpret_cast<BasicBlock *>(condBBs.pop().u.p);
      BasicBlock *forkBB = reinterpret_cast<BasicBlock *>(joinBBs.pop().u.p);

      if (!bb->isTerminated()) {
         // we only want join if none of the clauses ended with CONT/BREAK/RET
         if (prevBB->getExit()->op == OP_BRA && joinBBs.getSize() < 6)
            insertConvergenceOps(convBB, forkBB);
         mkFlow(OP_BRA, convBB, CC_ALWAYS, NULL);
         bb->cfg.attach(&convBB->cfg, Graph::Edge::FORWARD);
      }

      if (prevBB->getExit()->op == OP_BRA) {
         prevBB->cfg.attach(&convBB->cfg, Graph::Edge::FORWARD);
         prevBB->getExit()->asFlow()->target.bb = convBB;
      }
      setPosition(convBB, true);
   }
      break;
   case TGSI_OPCODE_BGNLOOP:
   {
      BasicBlock *lbgnBB = new BasicBlock(func);
      BasicBlock *lbrkBB = new BasicBlock(func);

      loopBBs.push(lbgnBB);
      breakBBs.push(lbrkBB);
      if (loopBBs.getSize() > func->loopNestingBound)
         func->loopNestingBound++;

      mkFlow(OP_PREBREAK, lbrkBB, CC_ALWAYS, NULL);

      bb->cfg.attach(&lbgnBB->cfg, Graph::Edge::TREE);
      setPosition(lbgnBB, true);
      mkFlow(OP_PRECONT, lbgnBB, CC_ALWAYS, NULL);
   }
      break;
   case TGSI_OPCODE_ENDLOOP:
   {
      BasicBlock *loopBB = reinterpret_cast<BasicBlock *>(loopBBs.pop().u.p);

      if (!bb->isTerminated()) {
         mkFlow(OP_CONT, loopBB, CC_ALWAYS, NULL);
         bb->cfg.attach(&loopBB->cfg, Graph::Edge::BACK);
      }
      setPosition(reinterpret_cast<BasicBlock *>(breakBBs.pop().u.p), true);
   }
      break;
   case TGSI_OPCODE_BRK:
   {
      if (bb->isTerminated())
         break;
      BasicBlock *brkBB = reinterpret_cast<BasicBlock *>(breakBBs.peek().u.p);
      mkFlow(OP_BREAK, brkBB, CC_ALWAYS, NULL);
      bb->cfg.attach(&brkBB->cfg, Graph::Edge::CROSS);
   }
      break;
   case TGSI_OPCODE_CONT:
   {
      if (bb->isTerminated())
         break;
      BasicBlock *contBB = reinterpret_cast<BasicBlock *>(loopBBs.peek().u.p);
      mkFlow(OP_CONT, contBB, CC_ALWAYS, NULL);
      contBB->explicitCont = true;
      bb->cfg.attach(&contBB->cfg, Graph::Edge::BACK);
   }
      break;
   case TGSI_OPCODE_BGNSUB:
   {
      Subroutine *s = getSubroutine(ip);
      BasicBlock *entry = new BasicBlock(s->f);
      BasicBlock *leave = new BasicBlock(s->f);

      // multiple entrypoints possible, keep the graph connected
      if (prog->getType() == Program::TYPE_COMPUTE)
         prog->main->call.attach(&s->f->call, Graph::Edge::TREE);

      sub.cur = s;
      s->f->setEntry(entry);
      s->f->setExit(leave);
      setPosition(entry, true);
      return true;
   }
   case TGSI_OPCODE_ENDSUB:
   {
      sub.cur = getSubroutine(prog->main);
      setPosition(BasicBlock::get(sub.cur->f->cfg.getRoot()), true);
      return true;
   }
   case TGSI_OPCODE_CAL:
   {
      Subroutine *s = getSubroutine(tgsi.getLabel());
      mkFlow(OP_CALL, s->f, CC_ALWAYS, NULL);
      func->call.attach(&s->f->call, Graph::Edge::TREE);
      return true;
   }
   case TGSI_OPCODE_RET:
   {
      if (bb->isTerminated())
         return true;
      BasicBlock *leave = BasicBlock::get(func->cfgExit);

      if (!isEndOfSubroutine(ip + 1)) {
         // insert a PRERET at the entry if this is an early return
         // (only needed for sharing code in the epilogue)
         BasicBlock *pos = getBB();
         setPosition(BasicBlock::get(func->cfg.getRoot()), false);
         mkFlow(OP_PRERET, leave, CC_ALWAYS, NULL)->fixed = 1;
         setPosition(pos, true);
      }
      mkFlow(OP_RET, NULL, CC_ALWAYS, NULL)->fixed = 1;
      bb->cfg.attach(&leave->cfg, Graph::Edge::CROSS);
   }
      break;
   case TGSI_OPCODE_END:
   {
      // attach and generate epilogue code
      BasicBlock *epilogue = BasicBlock::get(func->cfgExit);
      bb->cfg.attach(&epilogue->cfg, Graph::Edge::TREE);
      setPosition(epilogue, true);
      if (prog->getType() == Program::TYPE_FRAGMENT)
         exportOutputs();
      if (info->io.genUserClip > 0)
         handleUserClipPlanes();
      mkOp(OP_EXIT, TYPE_NONE, NULL)->terminator = 1;
   }
      break;
   case TGSI_OPCODE_SWITCH:
   case TGSI_OPCODE_CASE:
      ERROR("switch/case opcode encountered, should have been lowered\n");
      abort();
      break;
   case TGSI_OPCODE_LOAD:
      handleLOAD(dst0);
      break;
   case TGSI_OPCODE_STORE:
      handleSTORE();
      break;
   case TGSI_OPCODE_BARRIER:
      geni = mkOp2(OP_BAR, TYPE_U32, NULL, mkImm(0), mkImm(0));
      geni->fixed = 1;
      geni->subOp = NV50_IR_SUBOP_BAR_SYNC;
      break;
   case TGSI_OPCODE_MFENCE:
   case TGSI_OPCODE_LFENCE:
   case TGSI_OPCODE_SFENCE:
      geni = mkOp(OP_MEMBAR, TYPE_NONE, NULL);
      geni->fixed = 1;
      geni->subOp = tgsi::opcodeToSubOp(tgsi.getOpcode());
      break;
   case TGSI_OPCODE_ATOMUADD:
   case TGSI_OPCODE_ATOMXCHG:
   case TGSI_OPCODE_ATOMCAS:
   case TGSI_OPCODE_ATOMAND:
   case TGSI_OPCODE_ATOMOR:
   case TGSI_OPCODE_ATOMXOR:
   case TGSI_OPCODE_ATOMUMIN:
   case TGSI_OPCODE_ATOMIMIN:
   case TGSI_OPCODE_ATOMUMAX:
   case TGSI_OPCODE_ATOMIMAX:
      handleATOM(dst0, dstTy, tgsi::opcodeToSubOp(tgsi.getOpcode()));
      break;
   case TGSI_OPCODE_IBFE:
   case TGSI_OPCODE_UBFE:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         src1 = fetchSrc(1, c);
         src2 = fetchSrc(2, c);
         mkOp3(OP_INSBF, TYPE_U32, src1, src2, mkImm(0x808), src1);
         mkOp2(OP_EXTBF, dstTy, dst0[c], src0, src1);
      }
      break;
   case TGSI_OPCODE_BFI:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         src1 = fetchSrc(1, c);
         src2 = fetchSrc(2, c);
         src3 = fetchSrc(3, c);
         mkOp3(OP_INSBF, TYPE_U32, src2, src3, mkImm(0x808), src2);
         mkOp3(OP_INSBF, TYPE_U32, dst0[c], src1, src2, src0);
      }
      break;
   case TGSI_OPCODE_LSB:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         geni = mkOp2(OP_EXTBF, TYPE_U32, src0, src0, mkImm(0x2000));
         geni->subOp = NV50_IR_SUBOP_EXTBF_REV;
         geni = mkOp1(OP_BFIND, TYPE_U32, dst0[c], src0);
         geni->subOp = NV50_IR_SUBOP_BFIND_SAMT;
      }
      break;
   case TGSI_OPCODE_IMSB:
   case TGSI_OPCODE_UMSB:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         mkOp1(OP_BFIND, srcTy, dst0[c], src0);
      }
      break;
   case TGSI_OPCODE_BREV:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         geni = mkOp2(OP_EXTBF, TYPE_U32, dst0[c], src0, mkImm(0x2000));
         geni->subOp = NV50_IR_SUBOP_EXTBF_REV;
      }
      break;
   case TGSI_OPCODE_POPC:
      FOR_EACH_DST_ENABLED_CHANNEL(0, c, tgsi) {
         src0 = fetchSrc(0, c);
         mkOp2(OP_POPCNT, TYPE_U32, dst0[c], src0, src0);
      }
      break;
   case TGSI_OPCODE_INTERP_CENTROID:
   case TGSI_OPCODE_INTERP_SAMPLE:
   case TGSI_OPCODE_INTERP_OFFSET:
      handleINTERP(dst0);
      break;
   default:
      ERROR("unhandled TGSI opcode: %u\n", tgsi.getOpcode());
      assert(0);
      break;
   }

   if (tgsi.dstCount()) {
      for (c = 0; c < 4; ++c) {
         if (!dst0[c])
            continue;
         if (dst0[c] != rDst0[c])
            mkMov(rDst0[c], dst0[c]);
         storeDst(0, c, rDst0[c]);
      }
   }
   vtxBaseValid = 0;

   return true;
}

void
Converter::handleUserClipPlanes()
{
   Value *res[8];
   int n, i, c;

   for (c = 0; c < 4; ++c) {
      for (i = 0; i < info->io.genUserClip; ++i) {
         Symbol *sym = mkSymbol(FILE_MEMORY_CONST, info->io.ucpCBSlot,
                                TYPE_F32, info->io.ucpBase + i * 16 + c * 4);
         Value *ucp = mkLoadv(TYPE_F32, sym, NULL);
         if (c == 0)
            res[i] = mkOp2v(OP_MUL, TYPE_F32, getScratch(), clipVtx[c], ucp);
         else
            mkOp3(OP_MAD, TYPE_F32, res[i], clipVtx[c], ucp, res[i]);
      }
   }

   const int first = info->numOutputs - (info->io.genUserClip + 3) / 4;

   for (i = 0; i < info->io.genUserClip; ++i) {
      n = i / 4 + first;
      c = i % 4;
      Symbol *sym =
         mkSymbol(FILE_SHADER_OUTPUT, 0, TYPE_F32, info->out[n].slot[c] * 4);
      mkStore(OP_EXPORT, TYPE_F32, sym, NULL, res[i]);
   }
}

void
Converter::exportOutputs()
{
   for (unsigned int i = 0; i < info->numOutputs; ++i) {
      for (unsigned int c = 0; c < 4; ++c) {
         if (!oData.exists(sub.cur->values, i, c))
            continue;
         Symbol *sym = mkSymbol(FILE_SHADER_OUTPUT, 0, TYPE_F32,
                                info->out[i].slot[c] * 4);
         Value *val = oData.load(sub.cur->values, i, c, NULL);
         if (val)
            mkStore(OP_EXPORT, TYPE_F32, sym, NULL, val);
      }
   }
}

Converter::Converter(Program *ir, const tgsi::Source *code) : BuildUtil(ir),
     code(code),
     tgsi(NULL),
     tData(this), aData(this), pData(this), oData(this)
{
   info = code->info;

   const DataFile tFile = code->mainTempsInLMem ? FILE_MEMORY_LOCAL : FILE_GPR;

   const unsigned tSize = code->fileSize(TGSI_FILE_TEMPORARY);
   const unsigned pSize = code->fileSize(TGSI_FILE_PREDICATE);
   const unsigned aSize = code->fileSize(TGSI_FILE_ADDRESS);
   const unsigned oSize = code->fileSize(TGSI_FILE_OUTPUT);

   tData.setup(TGSI_FILE_TEMPORARY, 0, 0, tSize, 4, 4, tFile, 0);
   pData.setup(TGSI_FILE_PREDICATE, 0, 0, pSize, 4, 4, FILE_PREDICATE, 0);
   aData.setup(TGSI_FILE_ADDRESS, 0, 0, aSize, 4, 4, FILE_GPR, 0);
   oData.setup(TGSI_FILE_OUTPUT, 0, 0, oSize, 4, 4, FILE_GPR, 0);

   zero = mkImm((uint32_t)0);

   vtxBaseValid = 0;
}

Converter::~Converter()
{
}

inline const Converter::Location *
Converter::BindArgumentsPass::getValueLocation(Subroutine *s, Value *v)
{
   ValueMap::l_iterator it = s->values.l.find(v);
   return it == s->values.l.end() ? NULL : &it->second;
}

template<typename T> inline void
Converter::BindArgumentsPass::updateCallArgs(
   Instruction *i, void (Instruction::*setArg)(int, Value *),
   T (Function::*proto))
{
   Function *g = i->asFlow()->target.fn;
   Subroutine *subg = conv.getSubroutine(g);

   for (unsigned a = 0; a < (g->*proto).size(); ++a) {
      Value *v = (g->*proto)[a].get();
      const Converter::Location &l = *getValueLocation(subg, v);
      Converter::DataArray *array = conv.getArrayForFile(l.array, l.arrayIdx);

      (i->*setArg)(a, array->acquire(sub->values, l.i, l.c));
   }
}

template<typename T> inline void
Converter::BindArgumentsPass::updatePrototype(
   BitSet *set, void (Function::*updateSet)(), T (Function::*proto))
{
   (func->*updateSet)();

   for (unsigned i = 0; i < set->getSize(); ++i) {
      Value *v = func->getLValue(i);
      const Converter::Location *l = getValueLocation(sub, v);

      // only include values with a matching TGSI register
      if (set->test(i) && l && !conv.code->locals.count(*l))
         (func->*proto).push_back(v);
   }
}

bool
Converter::BindArgumentsPass::visit(Function *f)
{
   sub = conv.getSubroutine(f);

   for (ArrayList::Iterator bi = f->allBBlocks.iterator();
        !bi.end(); bi.next()) {
      for (Instruction *i = BasicBlock::get(bi)->getFirst();
           i; i = i->next) {
         if (i->op == OP_CALL && !i->asFlow()->builtin) {
            updateCallArgs(i, &Instruction::setSrc, &Function::ins);
            updateCallArgs(i, &Instruction::setDef, &Function::outs);
         }
      }
   }

   if (func == prog->main && prog->getType() != Program::TYPE_COMPUTE)
      return true;
   updatePrototype(&BasicBlock::get(f->cfg.getRoot())->liveSet,
                   &Function::buildLiveSets, &Function::ins);
   updatePrototype(&BasicBlock::get(f->cfgExit)->defSet,
                   &Function::buildDefSets, &Function::outs);

   return true;
}

bool
Converter::run()
{
   BasicBlock *entry = new BasicBlock(prog->main);
   BasicBlock *leave = new BasicBlock(prog->main);

   prog->main->setEntry(entry);
   prog->main->setExit(leave);

   setPosition(entry, true);
   sub.cur = getSubroutine(prog->main);

   if (info->io.genUserClip > 0) {
      for (int c = 0; c < 4; ++c)
         clipVtx[c] = getScratch();
   }

   if (prog->getType() == Program::TYPE_FRAGMENT) {
      Symbol *sv = mkSysVal(SV_POSITION, 3);
      fragCoord[3] = mkOp1v(OP_RDSV, TYPE_F32, getSSA(), sv);
      mkOp1(OP_RCP, TYPE_F32, fragCoord[3], fragCoord[3]);
   }

   if (info->io.viewportId >= 0)
      viewport = getScratch();
   else
      viewport = NULL;

   for (ip = 0; ip < code->scan.num_instructions; ++ip) {
      if (!handleInstruction(&code->insns[ip]))
         return false;
   }

   if (!BindArgumentsPass(*this).run(prog))
      return false;

   return true;
}

} // unnamed namespace

namespace nv50_ir {

bool
Program::makeFromTGSI(struct nv50_ir_prog_info *info)
{
   tgsi::Source src(info);
   if (!src.scanSource())
      return false;
   tlsSize = info->bin.tlsSpace;

   Converter builder(this, &src);
   return builder.run();
}

} // namespace nv50_ir
