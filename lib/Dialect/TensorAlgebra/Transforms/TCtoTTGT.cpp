//===- TCtoTTGT.cpp ------===//
//
// Copyright 2022 Battelle Memorial Institute
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions
// and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
// and the following disclaimer in the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//===----------------------------------------------------------------------===//
//
// This file implements reformulation of tensor contraction operations as Transpose-Transpose-GEMM-Transpose
//===----------------------------------------------------------------------===//

#include "comet/Dialect/TensorAlgebra/IR/TADialect.h"
#include "comet/Dialect/TensorAlgebra/Passes.h"
#include "comet/Dialect/Utils/Utils.h"

#include "mlir/Dialect/Linalg/EDSC/Intrinsics.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Linalg/IR/LinalgTypes.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"

#include "mlir/Dialect/StandardOps/EDSC/Intrinsics.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"

#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/Sequence.h"

#include "mlir/EDSC/Builders.h"
#include "mlir/Dialect/StandardOps/EDSC/Intrinsics.h"

#include <limits>
#include <map>
#include <set>
#include <unordered_map>

#include "mlir/Dialect/MemRef/IR/MemRef.h"

using namespace mlir;
using namespace mlir::edsc;
using namespace mlir::edsc::intrinsics;
using namespace mlir::linalg;

using namespace mlir::tensorAlgebra;

// *********** For debug purpose *********//
// #ifndef DEBUG_MODE_TTGT
// #define DEBUG_MODE_TTGT
// #endif

#ifdef DEBUG_MODE_TTGT
#define comet_debug() llvm::errs() << __FILE__ << " " << __LINE__ << " "
#define comet_pdump(n)                                \
  llvm::errs() << __FILE__ << " " << __LINE__ << " "; \
  n->dump()
#define comet_vdump(n)                                \
  llvm::errs() << __FILE__ << " " << __LINE__ << " "; \
  n.dump()
#else
#define comet_debug() llvm::nulls()
#define comet_pdump(n)
#define comet_vdump(n)
#endif
// *********** For debug purpose *********//

const StringLiteral tensorAlgMarker = "__with_tiling__";

template <typename T>
static bool arePermutations(const std::vector<T> &vec1,
                            const std::vector<T> &vec2)
{
  if (vec1.size() != vec2.size())
  {
    return false;
  }
  std::vector<bool> taken(vec1.size(), false);
  for (size_t i = 0; i < vec1.size(); i++)
  {
    auto it = std::find(vec2.begin(), vec2.end(), vec1[i]);
    if (it == vec2.end())
    {
      return false;
    }
    if (taken[std::distance(vec2.begin(), it)] == true)
    {
      return false;
    }
    taken[std::distance(vec2.begin(), it)] = true;
  }
  return true;
}

/// Return true if the reassociation specification is valid, false otherwise.
/// When false, the `invalidIndex` integer pointer is optionally filled with the
/// index of the offending reassociation map.
static bool isReassociationValid(ArrayRef<AffineMap> reassociation,
                                 int *invalidIndex = nullptr)
{
  if (reassociation.empty())
    return true;
  unsigned nDims = reassociation[0].getNumDims();
  unsigned nextExpectedDim = 0;
  for (auto it : llvm::enumerate(reassociation))
  {
    auto m = it.value();
    if (m.getNumDims() != nDims || m.getNumSymbols() != 0)
    {
      if (invalidIndex)
        *invalidIndex = it.index();
      return false;
    }
    for (auto e : m.getResults())
    {
      auto d = e.dyn_cast<AffineDimExpr>();
      if (!d || d.getPosition() != nextExpectedDim++)
      {
        if (invalidIndex)
          *invalidIndex = it.index();
        return false;
      }
    }
  }
  if (nextExpectedDim != nDims)
  {
    if (invalidIndex)
      *invalidIndex = reassociation.size() - 1;
    return false;
  }
  return true;
}

/// Detect whether memref dims [dim, dim + extent) can be reshaped without
/// copies.
static bool isReshapableDimBand(unsigned dim, unsigned extent,
                                ArrayRef<int64_t> sizes,
                                ArrayRef<AffineExpr> strides)
{
  assert(sizes.size() == strides.size() && "mismatched ranks");
  // off by 1 indexing to avoid out of bounds
  //                       V
  for (auto idx = dim, e = dim + extent; idx + 1 < e; ++idx)
  {
    // Only bands of static shapes are reshapable. This is due to the fact that
    // there is no relation between dynamic sizes and dynamic strides: we do not
    // have enough information to know whether a "-1" size corresponds to the
    // proper symbol in the AffineExpr of a stride.
    if (ShapedType::isDynamic(sizes[dim + 1]))
      return false;
    // simplify on the fly and catch more reshapable cases.
    if (strides[idx] != strides[idx + 1] * sizes[idx + 1])
      return false;
  }
  return true;
}

static IndexVector getIndexRange(unsigned lo, unsigned hi, unsigned step = 1)
{
  IndexVector result;
  for (unsigned i = lo; i < hi; i += step)
  {
    result.push_back(i);
  }
  return result;
}

/// Compute the MemRefType obtained by applying the `reassociation` (which is
/// expected to be valid) to `type`.
/// If `type` is Contiguous MemRefType, this always produce a contiguous
/// MemRefType.
static MemRefType
computeReshapeCollapsedType(MemRefType type,
                            ArrayRef<AffineMap> reassociation)
{
  auto sizes = type.getShape();
  AffineExpr offset;
  SmallVector<AffineExpr, 4> strides;
  auto status = getStridesAndOffset(type, strides, offset);
  (void)status;
  assert(succeeded(status) && "expected strided memref");

  SmallVector<int64_t, 4> newSizes;
  newSizes.reserve(reassociation.size());
  SmallVector<AffineExpr, 4> newStrides;
  newStrides.reserve(reassociation.size());

  // Use the fact that reassociation is valid to simplify the logic: only use
  // each map's rank.
  assert(isReassociationValid(reassociation) && "invalid reassociation");
  unsigned currentDim = 0;
  for (AffineMap m : reassociation)
  {
    unsigned dim = m.getNumResults();
    int64_t size = 1;
    AffineExpr stride = strides[currentDim + dim - 1];
    if (!isReshapableDimBand(currentDim, dim, sizes, strides))
    {
      size = ShapedType::kDynamicSize;
      stride = AffineExpr();
    }
    else
    {
      for (unsigned d = 0; d < dim; ++d)
        size *= sizes[currentDim + d];
    }
    newSizes.push_back(size);
    newStrides.push_back(stride);
    currentDim += dim;
  }

  // Early-exit: if `type` is contiguous, the result must be contiguous.
  if (canonicalizeStridedLayout(type).getAffineMaps().empty())
    return MemRefType::Builder(type).setShape(newSizes).setAffineMaps({});

  // Convert back to int64_t because we don't have enough information to create
  // new strided layouts from AffineExpr only. This corresponds to a case where
  // copies may be necessary.
  int64_t intOffset = ShapedType::kDynamicStrideOrOffset;
  if (auto o = offset.dyn_cast<AffineConstantExpr>())
    intOffset = o.getValue();
  SmallVector<int64_t, 4> intStrides;
  intStrides.reserve(strides.size());
  for (auto stride : newStrides)
  {
    if (auto cst = stride.dyn_cast_or_null<AffineConstantExpr>())
      intStrides.push_back(cst.getValue());
    else
      intStrides.push_back(ShapedType::kDynamicStrideOrOffset);
  }
  auto layout =
      makeStridedLinearLayoutMap(intStrides, intOffset, type.getContext());
  return canonicalizeStridedLayout(
      MemRefType::Builder(type).setShape(newSizes).setAffineMaps({layout}));
}

//===----------------------------------------------------------------------===//
// TAEarlyLoweringTTGTPass
//===----------------------------------------------------------------------===//

/// This is a partial lowering to linear algebra of the tensor algebra
/// operations that are computationally intensive (like matmul for example...)
/// while keeping the rest of the code in the TA dialect.
namespace
{

  struct TensorContractionOpLoweringTTGT : public ConversionPattern
  {
    TensorContractionOpLoweringTTGT(MLIRContext *ctx, bool isSelectBestPerm, int whatPerm, bool printFlops)
        : ConversionPattern(tensorAlgebra::TensorMultOp::getOperationName(), 1, ctx),
          isSelectBestPerm(isSelectBestPerm), whatPerm(whatPerm), printFlops{printFlops} {}

    /**
     * @brief Latest implementation with following optimizations:
     *        - if no transpose is required there won't be any copy operations
     *        - if any operand is 2 dimensional no reshape
     *        - does not copy C
     */
    LogicalResult
    matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                    ConversionPatternRewriter &rewriter) const final
    {
      comet_pdump(op);
      assert(isa<tensorAlgebra::TensorMultOp>(op));

      auto ctx = rewriter.getContext();
      auto loc = op->getLoc();
      auto multop = cast<tensorAlgebra::TensorMultOp>(op);
      auto alphaAttr = multop.getOperation()->getAttr("__alpha__");
      auto betaAttr = multop.getOperation()->getAttr("__beta__");

      Operation *startTime;
      std::string getTimeStr = "getTime";
      auto f64Type = rewriter.getF64Type();
      if (printFlops)
      {
        startTime = rewriter.create<mlir::CallOp>(
            op->getLoc(), getTimeStr, SmallVector<Type, 2>{f64Type});
      }

      ArrayAttr indexMaps = multop.indexing_maps();
      std::vector<std::vector<unsigned>> allPerms;
      // Find summation indices
      for (const auto &map : indexMaps)
      {
        auto affineMap = map.cast<AffineMapAttr>().getValue();
        std::vector<unsigned> perm;
        for (size_t i = 0; i < affineMap.getNumResults(); i++)
        {
          auto expr = affineMap.getResult(i);
          perm.push_back(expr.cast<AffineDimExpr>().getPosition());
        }

        allPerms.push_back(perm);
      }

      comet_pdump(op);
      comet_debug() << "\n";
      auto rhs1Tensor = cast<memref::TensorLoadOp>(operands[0].getDefiningOp());
      auto rhs2Tensor = cast<memref::TensorLoadOp>(operands[1].getDefiningOp());
      comet_debug() << "\n";
      Value lhsDef;
      tensorAlgebra::TensorSetOp setnewop;
      for (auto u : multop.getOperation()->getResult(0).getUsers())
      {

        comet_pdump(u);
        if (isa<tensorAlgebra::TensorSetOp>(u))
        {
          setnewop = cast<tensorAlgebra::TensorSetOp>(u);
          Value dstTensor = u->getOperand(1);
          if (isa<tensorAlgebra::LabeledTensorOp>(dstTensor.getDefiningOp()))
          {
            Value dstTensor_labeledTensor = cast<tensorAlgebra::LabeledTensorOp>(dstTensor.getDefiningOp());
            lhsDef = dstTensor_labeledTensor.getDefiningOp()->getOperand(0);
          }
          else
          { // if(isa<memref::TensorLoadOp>(dstTensor.getOperation())){
            lhsDef = dstTensor;
          }
          comet_vdump(lhsDef);
        }
      }
      auto lhsTensor = cast<memref::TensorLoadOp>(lhsDef.getDefiningOp());

      comet_vdump(setnewop);
      comet_debug() << "\n";

      Value rhs1Memref = rhs1Tensor.memref();
      Value rhs2Memref = rhs2Tensor.memref();
      Value lhsMemref = lhsTensor.memref();

      auto rhs1MemrefType = rhs1Memref.getType().cast<MemRefType>();
      auto rhs2MemrefType = rhs2Memref.getType().cast<MemRefType>();
      auto lhsMemrefType = lhsMemref.getType().cast<MemRefType>();

      std::vector<TensorShape> allShapes{rhs1MemrefType.getShape(),
                                         rhs2MemrefType.getShape(),
                                         lhsMemrefType.getShape()};

      ContractionPlan plan{allPerms[0], allShapes[0], allPerms[1],
                           allShapes[1], allPerms[2], allShapes[2]};

      // computeBestPermutations identifies the optimal index permutation for TTGT
      // it should enable and disable to heuristic
      IndexVector rhs1Perm, rhs2Perm, lhsPerm;
      std::tie(rhs1Perm, rhs2Perm, lhsPerm) = plan.computePermutations(isSelectBestPerm, whatPerm);

      comet_debug() << "Best permutation : " << plan.bestPermStr_ << "\n";

      std::set<unsigned> rhsIndices(allPerms[0].begin(), allPerms[0].end());
      rhsIndices.insert(allPerms[1].begin(), allPerms[1].end());
      std::set<unsigned> lhsIndices(allPerms[2].begin(), allPerms[2].end());

      std::vector<unsigned> sumIndices;

      std::set_difference(rhsIndices.begin(), rhsIndices.end(),
                          lhsIndices.begin(), lhsIndices.end(),
                          std::inserter(sumIndices, sumIndices.begin()));

      AffineMapAttr rhs1OutMapAttr = AffineMapAttr::get(AffineMap::getPermutationMap(rhs1Perm, ctx));
      AffineMap rhs1InMap = AffineMap::getPermutationMap(getIdentityPermutation(allPerms[0].size()), ctx);
      AffineMap rhs1OutMap = AffineMap::getPermutationMap(rhs1Perm, ctx);

      AffineMapAttr rhs2OutMapAttr =
          AffineMapAttr::get(AffineMap::getPermutationMap(rhs2Perm, ctx));
      AffineMap rhs2InMap = AffineMap::getPermutationMap(getIdentityPermutation(allPerms[1].size()), ctx);
      AffineMap rhs2OutMap = AffineMap::getPermutationMap(rhs2Perm, ctx);

      AffineMapAttr lhsOutMapAttr =
          AffineMapAttr::get(AffineMap::getPermutationMap(lhsPerm, ctx));
      AffineMap lhsInMap = AffineMap::getPermutationMap(
          getIdentityPermutation(allPerms[2].size()), ctx);
      AffineMap lhsOutMap = AffineMap::getPermutationMap(lhsPerm, ctx);

      Value rhs1Alloc = rhs1Memref;
      Value rhs2Alloc = rhs2Memref;
      Value lhsAlloc = lhsMemref;

      // Do transpose if needed
      if (!rhs1OutMapAttr.getValue().isIdentity())
      {
        std::vector<int64_t> rhs1Dims;
        for (auto idx : rhs1Perm)
        {
          auto shape = rhs1MemrefType.getShape();
          rhs1Dims.push_back(shape[idx]);
        }

        rhs1Alloc = insertAllocAndDealloc(
            MemRefType::get(rhs1Dims, rhs1MemrefType.getElementType()), loc,
            rewriter);

        #ifdef DEBUG_MODE_TTGT
        auto rhs1LinalgCopy = rewriter.create<linalg::CopyOp>(loc, rhs1Memref, rhs1Alloc, rhs1InMap, rhs1OutMap);
        comet_debug() << "\n";
        comet_vdump(rhs1LinalgCopy);
        #else
        rewriter.create<linalg::CopyOp>(loc, rhs1Memref, rhs1Alloc, rhs1InMap, rhs1OutMap);
        #endif
      }

      if (!rhs2OutMapAttr.getValue().isIdentity())
      {
        std::vector<int64_t> rhs2Dims;
        for (auto idx : rhs2Perm)
        {
          auto shape = rhs2MemrefType.getShape();
          rhs2Dims.push_back(shape[idx]);
        }

        rhs2Alloc = insertAllocAndDealloc(
            MemRefType::get(rhs2Dims, rhs2MemrefType.getElementType()), loc,
            rewriter);
        #ifdef DEBUG_MODE_TTGT
        auto rhs2LinalgCopy = rewriter.create<linalg::CopyOp>(loc, rhs2Memref, rhs2Alloc, rhs2InMap, rhs2OutMap);
        comet_debug() << " rhs2LinalgCopy op: " << __LINE__ << "\n";
        comet_vdump(rhs2LinalgCopy);
        #else
        rewriter.create<linalg::CopyOp>(loc, rhs2Memref, rhs2Alloc, rhs2InMap, rhs2OutMap);
        #endif
      }

      bool useLHSTranspose = false;
      if (!lhsOutMapAttr.getValue().isIdentity())
      {
        std::vector<int64_t> lhsDims;
        for (auto idx : lhsPerm)
        {
          auto shape = lhsMemrefType.getShape();
          lhsDims.push_back(shape[idx]);
        }

        lhsAlloc = insertAllocAndDealloc(
            MemRefType::get(lhsDims, lhsMemrefType.getElementType()), loc,
            rewriter);
        useLHSTranspose = true;
        // TODO(gkestor): we might need this copy if we support update C[] += A[] * B[]
        rewriter.create<linalg::CopyOp>(loc, lhsMemref, lhsAlloc, lhsInMap, lhsOutMap);
      }

      MemRefType collapsedMemrefType;

      Value rhs1Reshape = rhs1Alloc;
      Value rhs2Reshape = rhs2Alloc;
      Value lhsReshape = lhsAlloc;

      unsigned mIdxSize = plan.m_indices_.size();
      unsigned nIdxSize = plan.n_indices_.size();
      unsigned kIdxSize = plan.k_indices_.size();

      bool isRHS1SumPermutation = arePermutations(allPerms[0], sumIndices);
      bool isRHS2SumPermutation = arePermutations(allPerms[1], sumIndices);

      comet_debug() << __LINE__ << "mIdxSize, nIdxSize, kIdxSize: " << mIdxSize << ", " << nIdxSize << ", " << kIdxSize << " isRHS1SumPermutation, isRHS2SumPermutation: " << isRHS1SumPermutation << ", " << isRHS2SumPermutation << "\n";

      // Do reshape if needed
      if (isRHS1SumPermutation)
      {
        auto resultShape = rhs1MemrefType.getShape();

        auto rhs1AffineMap = AffineMap::getPermutationMap(
            getIdentityPermutation(resultShape.size()), ctx);

        SmallVector<AffineMap, 2> rhs1IndexingMap{rhs1AffineMap};

        collapsedMemrefType = computeReshapeCollapsedType(
            rhs1Alloc.getType().cast<MemRefType>(), rhs1IndexingMap);
        SmallVector<ReassociationIndices> reassociationIndices =
            getReassociationIndices(rhs1IndexingMap);
        comet_debug() << "\n";
        rhs1Reshape = rewriter.create<linalg::ReshapeOp>(
            loc, collapsedMemrefType, rhs1Alloc, reassociationIndices);
        comet_vdump(rhs1Reshape);
      }
      else if (rhs1MemrefType.getShape().size() != 2)
      {
        auto resultShape = rhs1MemrefType.getShape();
        // Construct combined shape of 2D memrefc
        std::vector<unsigned> rhs1_0, rhs1_1;

        if (plan.swapAB_)
        {
          rhs1_0 = getIndexRange(0, kIdxSize);
          rhs1_1 = getIndexRange(kIdxSize, kIdxSize + mIdxSize);
        }
        else
        {
          rhs1_0 = getIndexRange(0, mIdxSize);
          rhs1_1 = getIndexRange(mIdxSize, mIdxSize + kIdxSize);
        }

        auto rhs1AffineMap = AffineMap::getPermutationMap(
            getIdentityPermutation(resultShape.size()), ctx);
        auto rhs1Subset0 = rhs1AffineMap.getSubMap(rhs1_0);
        auto rhs1Subset1 = rhs1AffineMap.getSubMap(rhs1_1);

        SmallVector<AffineMap, 2> rhs1IndexingMap;

        rhs1IndexingMap.push_back(rhs1Subset0);
        rhs1IndexingMap.push_back(rhs1Subset1);
        collapsedMemrefType = computeReshapeCollapsedType(
            rhs1Alloc.getType().cast<MemRefType>(), rhs1IndexingMap);
        SmallVector<ReassociationIndices> reassociationIndices =
            getReassociationIndices(rhs1IndexingMap);
        comet_debug() << " collapsedMemrefType:"
                     << "\n";
        comet_vdump(collapsedMemrefType);
        comet_debug() << "\n";
        comet_debug() << " rhs1Alloc: \n";
        comet_vdump(rhs1Alloc);
        comet_vdump(rhs1MemrefType);

        rhs1Reshape = rewriter.create<linalg::ReshapeOp>(
            loc, collapsedMemrefType, rhs1Alloc, reassociationIndices);
        comet_debug() << " Before rhs1Reshape: \n";
        comet_vdump(rhs1Reshape);
        comet_debug() << " After rhs1Reshape: \n";
      }

      // if (isRHS2SumPermutation) {
      if (isRHS2SumPermutation && rhs2MemrefType.getShape().size() != 1)
      {
        auto resultShape = rhs2MemrefType.getShape();

        auto rhs2AffineMap = AffineMap::getPermutationMap(
            getIdentityPermutation(resultShape.size()), ctx);

        SmallVector<AffineMap, 2> rhs2IndexingMap{rhs2AffineMap};

        collapsedMemrefType = computeReshapeCollapsedType(
            rhs2Alloc.getType().cast<MemRefType>(), rhs2IndexingMap);
        SmallVector<ReassociationIndices> reassociationIndices =
            getReassociationIndices(rhs2IndexingMap);
        rhs2Reshape = rewriter.create<linalg::ReshapeOp>(
            loc, collapsedMemrefType, rhs2Alloc, reassociationIndices);

        comet_debug() << "\n";
        comet_vdump(rhs2Reshape);
        // } else if (rhs2MemrefType.getShape().size() != 2) {
      }
      else if (rhs2MemrefType.getShape().size() != 2 && rhs2MemrefType.getShape().size() != 1)
      {
        auto resultShape = rhs2MemrefType.getShape();

        // Construct combined shape of 2D memref
        std::vector<unsigned> rhs2_0, rhs2_1;

        if (plan.swapAB_)
        {
          rhs2_0 = getIndexRange(0, nIdxSize);
          rhs2_1 = getIndexRange(nIdxSize, nIdxSize + kIdxSize);
        }
        else
        {
          rhs2_0 = getIndexRange(0, kIdxSize);
          rhs2_1 = getIndexRange(kIdxSize, kIdxSize + nIdxSize);
        }

        auto rhs2AffineMap = AffineMap::getPermutationMap(
            getIdentityPermutation(resultShape.size()), ctx);
        auto rhs2Subset0 = rhs2AffineMap.getSubMap(rhs2_0);
        auto rhs2Subset1 = rhs2AffineMap.getSubMap(rhs2_1);

        SmallVector<AffineMap, 2> rhs2IndexingMap;

        rhs2IndexingMap.push_back(rhs2Subset0);
        rhs2IndexingMap.push_back(rhs2Subset1);

        collapsedMemrefType = computeReshapeCollapsedType(
            rhs2Alloc.getType().cast<MemRefType>(), rhs2IndexingMap);
        SmallVector<ReassociationIndices> reassociationIndices =
            getReassociationIndices(rhs2IndexingMap);
        rhs2Reshape = rewriter.create<linalg::ReshapeOp>(
            loc, collapsedMemrefType, rhs2Alloc, reassociationIndices);
        comet_debug() << "\n";
        comet_vdump(rhs2Reshape);
      }

      comet_debug() << "\n";
      // if (isRHS1SumPermutation || isRHS2SumPermutation) {
      if (isRHS1SumPermutation || (isRHS2SumPermutation && rhs2MemrefType.getShape().size() != 1))
      {
        comet_debug() << "\n";
        auto resultShape = lhsMemrefType.getShape();

        auto lhsAffineMap = AffineMap::getPermutationMap(
            getIdentityPermutation(resultShape.size()), ctx);

        SmallVector<AffineMap, 2> lhsIndexingMap{lhsAffineMap};

        collapsedMemrefType = computeReshapeCollapsedType(
            lhsAlloc.getType().cast<MemRefType>(), lhsIndexingMap);
        SmallVector<ReassociationIndices> reassociationIndices =
            getReassociationIndices(lhsIndexingMap);
        lhsReshape = rewriter.create<linalg::ReshapeOp>(
            loc, collapsedMemrefType, lhsAlloc, reassociationIndices);
        comet_debug() << "\n";
        comet_vdump(lhsReshape);
      }
      else if (lhsMemrefType.getShape().size() != 2 && lhsMemrefType.getShape().size() != 1)
      {
        comet_debug() << "\n";
        auto resultShape = lhsMemrefType.getShape();
        // Construct combined shape of 2D memref
        std::vector<unsigned> lhs_0, lhs_1;
        if (plan.swapAB_)
        {
          lhs_0 = getIndexRange(0, nIdxSize);
          lhs_1 = getIndexRange(nIdxSize, nIdxSize + mIdxSize);
        }
        else
        {
          lhs_0 = getIndexRange(0, mIdxSize);
          lhs_1 = getIndexRange(mIdxSize, mIdxSize + nIdxSize);
        }

        auto lhsAffineMap = AffineMap::getPermutationMap(
            getIdentityPermutation(resultShape.size()), ctx);
        auto lhsSubset0 = lhsAffineMap.getSubMap(lhs_0);
        auto lhsSubset1 = lhsAffineMap.getSubMap(lhs_1);

        SmallVector<AffineMap, 2> lhsIndexingMap;

        lhsIndexingMap.push_back(lhsSubset0);
        lhsIndexingMap.push_back(lhsSubset1);

        collapsedMemrefType = computeReshapeCollapsedType(
            lhsAlloc.getType().cast<MemRefType>(), lhsIndexingMap);
        SmallVector<ReassociationIndices> reassociationIndices =
            getReassociationIndices(lhsIndexingMap);
        lhsReshape = rewriter.create<linalg::ReshapeOp>(
            loc, collapsedMemrefType, lhsAlloc, reassociationIndices);
        comet_debug() << "\n";
        comet_vdump(lhsReshape);
      }

      comet_debug() << "\n";
      // Create linalg matmul op
      linalg::MatmulOp matmulop;
      linalg::MatvecOp matvecop;
      Value res_value;
      if (isRHS1SumPermutation)
      {
        comet_debug() << "\n";
        matvecop = rewriter.create<linalg::MatvecOp>(
            loc, ValueRange{rhs2Reshape, rhs1Reshape},
            ValueRange{lhsReshape});
        comet_debug() << "\n";
        comet_vdump(matvecop);

        matvecop.getOperation()->setAttr("__alpha__", alphaAttr);
        matvecop.getOperation()->setAttr("__beta__", betaAttr);

        // Add attribute to the linalg.matvec operations
        // matvecop.setAttr(LinalgTransforms::kLinalgTransformMarker,
        // rewriter.getStringAttr(tensorAlgMarker));
      }
      else if (isRHS2SumPermutation)
      {
        comet_debug() << "\n";
        matvecop = rewriter.create<linalg::MatvecOp>(
            loc, ValueRange{rhs1Reshape, rhs2Reshape},
            ValueRange{lhsReshape});
        comet_debug() << "\n";
        comet_vdump(rhs1Reshape);
        comet_vdump(rhs2Reshape);
        comet_vdump(lhsReshape);
        comet_vdump(matvecop);

        matvecop.getOperation()->setAttr("__alpha__", alphaAttr);
        matvecop.getOperation()->setAttr("__beta__", betaAttr);

        // Add attribute to the linalg.matvec operations
        // matvecop.setAttr(LinalgTransforms::kLinalgTransformMarker,
        // rewriter.getStringAttr(tensorAlgMarker));
      }
      else
      {
        comet_debug() << "\n";
        if (plan.swapAB_)
        {
          // TODO(gkestor) - there is error with the building process
          matmulop = rewriter.create<linalg::MatmulOp>(
              loc, ValueRange{rhs2Reshape, rhs1Reshape},
              ValueRange{lhsReshape});
          comet_debug() << "\n";
          comet_vdump(matmulop);
        }
        else
        {
          matmulop = rewriter.create<linalg::MatmulOp>(
              loc, ValueRange{rhs1Reshape, rhs2Reshape},
              ValueRange{lhsReshape});
          comet_debug() << "\n";
          comet_vdump(rhs1Reshape);
          comet_vdump(rhs2Reshape);
          comet_vdump(lhsReshape);
          comet_vdump(matmulop);
        }
        comet_debug() << "\n";
        // Add attribute to the linalg.matmul operations
        matmulop.getOperation()->setAttr(LinalgTransforms::kLinalgTransformMarker,
                                         rewriter.getStringAttr(tensorAlgMarker));
        matmulop.getOperation()->setAttr("__alpha__", alphaAttr);
        matmulop.getOperation()->setAttr("__beta__", betaAttr);
      }

      // Copy back the result if needed
      if (lhsAlloc != lhsMemref && useLHSTranspose)
      {
        #ifdef DEBUG_MODE_TTGT
        auto lhsFinalCopy =
            rewriter.create<linalg::CopyOp>(loc, lhsAlloc, lhsMemref, lhsOutMap, lhsInMap);
        comet_debug() << "\n";
        comet_vdump(lhsFinalCopy);
        #else
        rewriter.create<linalg::CopyOp>(loc, lhsAlloc, lhsMemref, lhsOutMap, lhsInMap);
        #endif
      }

      if (printFlops)
      {
        auto endTime = rewriter.create<mlir::CallOp>(
            loc, getTimeStr, SmallVector<Type, 2>{f64Type});

        auto start = startTime->getResult(0);
        auto end = endTime.getResult(0);

        Value totalTimeValue =
            rewriter.create<mlir::SubFOp>(loc, f64Type, end, start);

        double opNums = 2.0 * plan.m_size_ * plan.n_size_ * plan.k_size_;

        Value numFlopsOp =
            rewriter.create<ConstantOp>(loc, FloatAttr::get(f64Type, opNums));

        Value flopsOp =
            rewriter.create<DivFOp>(loc, f64Type, numFlopsOp, totalTimeValue);

        //   call @print_flops(%flops) : (f64) -> ()
        std::string printFlopsStr = "print_flops";
        // auto printFlopsCall =
        rewriter.create<mlir::CallOp>(
            loc, printFlopsStr, SmallVector<Type, 2>{}, ValueRange{flopsOp});
      }

      rewriter.eraseOp(setnewop);
      rewriter.eraseOp(op);
      return success();
    }

  private:
    bool isSelectBestPerm;
    int whatPerm;
    bool printFlops;
  }; // namespace

  struct TALoweringTTGTPass
      : public PassWrapper<TALoweringTTGTPass, FunctionPass>
  {

    TALoweringTTGTPass(bool isSelectBestPerm, int whatPerm, bool printFlops) : 
                      isSelectBestPerm(isSelectBestPerm), whatPerm(whatPerm), printFlops{printFlops} {};
    void runOnFunction() final;

  private:
    bool isSelectBestPerm;
    int whatPerm;
    bool printFlops;
  };

} // end anonymous namespace.

void TALoweringTTGTPass::runOnFunction()
{
  auto function = getFunction();
  auto module = function.getOperation()->getParentOfType<ModuleOp>();
  auto *ctx = &getContext();

  auto getTimeFunc = FunctionType::get(ctx, {}, {FloatType::getF64(ctx)});
  auto printFlopFunc = FunctionType::get(ctx, {FloatType::getF64(ctx)}, {});

  // func @getTime() -> f64
  if (!hasFuncDeclaration(module, "getTime"))
  {
    FuncOp func1 = FuncOp::create(function.getLoc(), "getTime", getTimeFunc,
                                  ArrayRef<NamedAttribute>{});
    func1.setPrivate();
    module.push_back(func1);
  }

  // func @print_flops(%flops) : (f64) -> ()
  if (!hasFuncDeclaration(module, "print_flops"))
  {
    FuncOp func1 = FuncOp::create(function.getLoc(), "print_flops",
                                  printFlopFunc, ArrayRef<NamedAttribute>{});
    func1.setPrivate();
    module.push_back(func1);
  }

  OwningRewritePatternList patterns(&getContext());
  patterns.insert<TensorContractionOpLoweringTTGT>(&getContext(), isSelectBestPerm, whatPerm, printFlops);

  ConversionTarget target(getContext());
  target.addLegalDialect<LinalgDialect, StandardOpsDialect, memref::MemRefDialect>();

  if (failed(applyPartialConversion(function, target, std::move(patterns))))
  {
    llvm::errs() << "Failed to applyPartialConversion in TALoweringTTGTPass\n";
    signalPassFailure();
  }
}

/// Create a pass for lowering operations in the `LinAlg` and `Std` dialects,
/// for a subset of the TA IR (e.g. matmul).
/// ordering of permutation starts with one
std::unique_ptr<Pass> mlir::tensorAlgebra::createLoweringTTGTPass(bool isSelectBestPerm, int whatPerm, bool printFlops)
{
  return std::make_unique<TALoweringTTGTPass>(isSelectBestPerm, whatPerm, printFlops);
}
