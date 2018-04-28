//===- IntrinsicConversion.h ------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains a mapping between equivalent intrinsics, to allow
// automatic widening of vector operations in the Revectorizer pass. The
// equivalences are generated by testing intrinsics on various inputs.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"

#ifndef LLVM_TRANSFORMS_VECTORIZE_REVECTORIZER_INTRINSICCONVERSION_H
#define LLVM_TRANSFORMS_VECTORIZE_REVECTORIZER_INTRINSICCONVERSION_H

namespace llvm {
namespace revectorizer {

// A map encoding lane-widening intrinsic conversions
static SmallDenseMap<unsigned, std::pair<unsigned, int>> intrinsicWideningMap;

static void initializeIntrinsicWideningMap() {
  intrinsicWideningMap[Intrinsic::x86_avx2_psllv_d] = {Intrinsic::x86_avx2_psllv_d_256, 2};
  intrinsicWideningMap[Intrinsic::x86_avx2_psllv_q] = {Intrinsic::x86_avx2_psllv_q_256, 2};
  intrinsicWideningMap[Intrinsic::x86_avx2_psrav_d] = {Intrinsic::x86_avx2_psrav_d_256, 2};
  intrinsicWideningMap[Intrinsic::x86_avx2_psrlv_d] = {Intrinsic::x86_avx2_psrlv_d_256, 2};
  intrinsicWideningMap[Intrinsic::x86_avx2_psrlv_q] = {Intrinsic::x86_avx2_psrlv_q_256, 2};
  intrinsicWideningMap[Intrinsic::x86_avx_vpermilvar_pd] = {Intrinsic::x86_avx_vpermilvar_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_avx_vpermilvar_ps] = {Intrinsic::x86_avx_vpermilvar_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfmadd_pd] = {Intrinsic::x86_fma_vfmadd_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfmadd_ps] = {Intrinsic::x86_fma_vfmadd_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfmaddsub_pd] = {Intrinsic::x86_fma_vfmaddsub_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfmaddsub_ps] = {Intrinsic::x86_fma_vfmaddsub_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfmsub_pd] = {Intrinsic::x86_fma_vfmsub_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfmsub_ps] = {Intrinsic::x86_fma_vfmsub_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfmsubadd_pd] = {Intrinsic::x86_fma_vfmsubadd_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfmsubadd_ps] = {Intrinsic::x86_fma_vfmsubadd_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfnmadd_pd] = {Intrinsic::x86_fma_vfnmadd_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfnmadd_ps] = {Intrinsic::x86_fma_vfnmadd_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfnmsub_pd] = {Intrinsic::x86_fma_vfnmsub_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_fma_vfnmsub_ps] = {Intrinsic::x86_fma_vfnmsub_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_cvtdq2ps] = {Intrinsic::x86_avx_cvtdq2_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_cvtps2dq] = {Intrinsic::x86_avx_cvt_ps2dq_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_cvttps2dq] = {Intrinsic::x86_avx_cvtt_ps2dq_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_max_pd] = {Intrinsic::x86_avx_max_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_min_pd] = {Intrinsic::x86_avx_min_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_packssdw_128] = {Intrinsic::x86_avx2_packssdw, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_packsswb_128] = {Intrinsic::x86_avx2_packsswb, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_packuswb_128] = {Intrinsic::x86_avx2_packuswb, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_padds_b] = {Intrinsic::x86_avx2_padds_b, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_padds_w] = {Intrinsic::x86_avx2_padds_w, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_paddus_b] = {Intrinsic::x86_avx2_paddus_b, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_paddus_w] = {Intrinsic::x86_avx2_paddus_w, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_pmadd_wd] = {Intrinsic::x86_avx2_pmadd_wd, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_pmulh_w] = {Intrinsic::x86_avx2_pmulh_w, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_pmulhu_w] = {Intrinsic::x86_avx2_pmulhu_w, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_pmulu_dq] = {Intrinsic::x86_avx2_pmulu_dq, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psad_bw] = {Intrinsic::x86_avx2_psad_bw, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psll_d] = {Intrinsic::x86_avx2_psll_d, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psll_q] = {Intrinsic::x86_avx2_psll_q, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psll_w] = {Intrinsic::x86_avx2_psll_w, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psra_d] = {Intrinsic::x86_avx2_psra_d, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psra_w] = {Intrinsic::x86_avx2_psra_w, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psrl_d] = {Intrinsic::x86_avx2_psrl_d, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psrl_q] = {Intrinsic::x86_avx2_psrl_q, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psrl_w] = {Intrinsic::x86_avx2_psrl_w, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psubs_b] = {Intrinsic::x86_avx2_psubs_b, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psubs_w] = {Intrinsic::x86_avx2_psubs_w, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psubus_b] = {Intrinsic::x86_avx2_psubus_b, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_psubus_w] = {Intrinsic::x86_avx2_psubus_w, 2};
  intrinsicWideningMap[Intrinsic::x86_sse2_sqrt_pd] = {Intrinsic::x86_avx_sqrt_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse3_addsub_pd] = {Intrinsic::x86_avx_addsub_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse3_addsub_ps] = {Intrinsic::x86_avx_addsub_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse3_hadd_pd] = {Intrinsic::x86_avx_hadd_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse3_hadd_ps] = {Intrinsic::x86_avx_hadd_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse3_hsub_pd] = {Intrinsic::x86_avx_hsub_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse3_hsub_ps] = {Intrinsic::x86_avx_hsub_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse41_blendvpd] = {Intrinsic::x86_avx_blendv_pd_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse41_blendvps] = {Intrinsic::x86_avx_blendv_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse41_packusdw] = {Intrinsic::x86_avx2_packusdw, 2};
  intrinsicWideningMap[Intrinsic::x86_sse41_pblendvb] = {Intrinsic::x86_avx2_pblendvb, 2};
  intrinsicWideningMap[Intrinsic::x86_sse41_pmuldq] = {Intrinsic::x86_avx2_pmul_dq, 2};
  intrinsicWideningMap[Intrinsic::x86_sse_max_ps] = {Intrinsic::x86_avx_max_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse_min_ps] = {Intrinsic::x86_avx_min_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse_rcp_ps] = {Intrinsic::x86_avx_rcp_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse_rsqrt_ps] = {Intrinsic::x86_avx_rsqrt_ps_256, 2};
  intrinsicWideningMap[Intrinsic::x86_sse_sqrt_ps] = {Intrinsic::x86_avx_sqrt_ps_256, 2};
}

} // end namespace revectorizer
} // end namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_REVECTORIZER_INTRINSICCONVERSION_H
