//===- LinalgTransforms.cpp - Linalg transformation patterns -----===//
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
// This file implements logic for Linalg transformations after lowering TA to Linalg operations
//
//===----------------------------------------------------------------------===//

#include "comet/Dialect/TensorAlgebra/IR/TADialect.h"
#include "comet/Dialect/TensorAlgebra/Passes.h"
#include "comet/Dialect/Utils/Utils.h"

#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Conversion/LinalgToStandard/LinalgToStandard.h"
#include "mlir/Dialect/Linalg/Analysis/DependenceAnalysis.h"
#include "mlir/Dialect/Linalg/Utils/Utils.h"
#include "mlir/Transforms/LoopUtils.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

using namespace mlir;
using namespace mlir::edsc;
using namespace mlir::edsc::intrinsics;
using namespace mlir::linalg;
using namespace mlir::tensorAlgebra;

// *********** For debug purpose *********//
// #ifndef DEBUG_MODE_LINALGTRANSFORMS
// #define DEBUG_MODE_LINALGTRANSFORMS
// #endif

#ifdef DEBUG_MODE_LINALGTRANSFORMS
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

namespace
{
  class LinAlgMatmulTilingPass : public PassWrapper<LinAlgMatmulTilingPass, FunctionPass>
  {
  public:
    void runOnFunction() override
    {
      // OwningRewritePatternList patterns;

      auto funcOp = getFunction();
      MLIRContext *ctx = funcOp.getContext();

      OwningRewritePatternList patterns(&getContext());

      // Add the matmul tiling patterns to the list.
      //===----------------------------------------------------------------------===//
      // BLIS HASWELL
      //===----------------------------------------------------------------------===//
      //#define BLIS_DGEMM_UKERNEL         bli_dgemm_asm_8x6
      //#define BLIS_DEFAULT_MC_D          72
      //#define BLIS_DEFAULT_KC_D          256
      //#define BLIS_DEFAULT_NC_D          4080
      //#define BLIS_DEFAULT_MR_D          8
      //#define BLIS_DEFAULT_NR_D          6

      patterns.insert<LinalgTilingPattern<MatmulOp>>(
          ctx,
          LinalgTilingOptions()
              .setTileSizes({72, 4080, 256})
              .setInterchange({1, 2, 0})
              .setLoopType(LinalgTilingLoopType::Loops),
          LinalgTransformationFilter(Identifier::get("__with_tiling__", ctx),
                                     Identifier::get("L2__with_tiling__", ctx)));

      patterns.insert<LinalgTilingPattern<MatmulOp>>(
          ctx,
          LinalgTilingOptions()
              .setTileSizes({6, 8, 256})
              .setInterchange({1, 0, 2})
              .setLoopType(LinalgTilingLoopType::Loops),
          LinalgTransformationFilter(Identifier::get("L2__with_tiling__", ctx),
                                     Identifier::get("__micro_kernel__", ctx)));

      (void)applyPatternsAndFoldGreedily(funcOp, std::move(patterns));
    }
  };
} // end anonymous namespace

namespace
{
  class LinalgMatMulOpToLibraryCallRewrite : public OpRewritePattern<MatmulOp>
  {
  public:
    using OpRewritePattern<MatmulOp>::OpRewritePattern;
    LogicalResult matchAndRewrite(MatmulOp op,
                                  PatternRewriter &rewriter) const override;
  };
}

/// Helper function to extract the operand types that are passed to the
/// generated CallOp. MemRefTypes have their layout canonicalized since the
/// information is not used in signature generation.
/// Note that static size information is not modified.
static SmallVector<Type, 4> extractOperandTypes(Operation *op)
{
  SmallVector<Type, 4> result;
  result.reserve(op->getNumOperands());
  if (auto indexedGenericOp = dyn_cast<IndexedGenericOp>(op))
  {
    auto *ctx = op->getContext();
    auto numLoops = indexedGenericOp.getNumLoops();
    result.reserve(op->getNumOperands() + numLoops);
    result.assign(numLoops, IndexType::get(ctx));
  }
  for (auto type : op->getOperandTypes())
  {
    // The underlying descriptor type (e.g. LLVM) does not have layout
    // information. Canonicalizing the type at the level of std when going into
    // a library call avoids needing to introduce DialectCastOp.
    if (auto memrefType = type.dyn_cast<MemRefType>())
      result.push_back(eraseStridedLayout(memrefType));
    else
      result.push_back(type);
  }
  return result;
}

static SmallVector<Value, 4>
createTypeCanonicalizedMemRefOperands(OpBuilder &b, Location loc,
                                      ValueRange operands)
{
  SmallVector<Value, 4> res;
  res.reserve(operands.size());
  for (auto op : operands)
  {
    auto memrefType = op.getType().dyn_cast<MemRefType>();
    if (!memrefType)
    {
      res.push_back(op);
      continue;
    }
    Value cast =
        b.create<memref::CastOp>(loc, eraseStridedLayout(memrefType), op);
    res.push_back(cast);
  }
  return res;
}

// Get a SymbolRefAttr containing the library function name for the LinalgOp.
// If the library function does not exist, insert a declaration.
static FlatSymbolRefAttr getLibraryCallSymbolRef(Operation *op,
                                                 PatternRewriter &rewriter)
{
  auto linalgOp = cast<LinalgOp>(op);
  auto fnName = linalgOp.getLibraryCallName();
  if (fnName.empty())
  {
    op->emitWarning("No library call defined for: ") << *op;
    return {};
  }

  // fnName is a dynamic std::string, unique it via a SymbolRefAttr.
  FlatSymbolRefAttr fnNameAttr = rewriter.getSymbolRefAttr(fnName);
  auto module = op->getParentOfType<ModuleOp>();
  if (module.lookupSymbol(fnName))
  {
    return fnNameAttr;
  }

  SmallVector<Type, 4> inputTypes(extractOperandTypes(op));
  assert(op->getNumResults() == 0 &&
         "Library call for linalg operation can be generated only for ops that "
         "have void return types");
  auto libFnType = rewriter.getFunctionType(inputTypes, {});

  OpBuilder::InsertionGuard guard(rewriter);
  // Insert before module terminator.
  rewriter.setInsertionPoint(module.getBody(),
                             std::prev(module.getBody()->end()));
  FuncOp funcOp =
      rewriter.create<FuncOp>(op->getLoc(), fnNameAttr.getValue(), libFnType);
  // Insert a function attribute that will trigger the emission of the
  // corresponding `_mlir_ciface_xxx` interface so that external libraries see
  // a normalized ABI. This interface is added during std to llvm conversion.
  funcOp->setAttr("llvm.emit_c_interface", UnitAttr::get(op->getContext()));
  funcOp.setPrivate();
  return fnNameAttr;
}

LogicalResult LinalgMatMulOpToLibraryCallRewrite::matchAndRewrite(
    MatmulOp op, PatternRewriter &rewriter) const
{
  if (!isa<MatmulOp>(op))
    return failure();

  auto libraryCallName = getLibraryCallSymbolRef(op, rewriter);
  if (!libraryCallName)
    return failure();

  // TODO(gkestor): Add support for more complex library call signatures that include
  // indices or captured values.
  rewriter.replaceOpWithNewOp<mlir::CallOp>(
      op, libraryCallName.getValue(), TypeRange(),
      createTypeCanonicalizedMemRefOperands(rewriter, op->getLoc(),
                                            op->getOperands()));
  return success();
}

namespace
{
  class LinAlgMatmulMicroKernelPass : public PassWrapper<LinAlgMatmulMicroKernelPass, FunctionPass>
  {
  public:
    void runOnFunction() override
    {
      auto funcOp = getFunction();
      MLIRContext *ctx = funcOp.getContext();

      OwningRewritePatternList patterns(&getContext());

      // Replace the inner linalg.matmul with the blis microkernel
      patterns.insert<LinalgMatMulOpToLibraryCallRewrite>(ctx);
      (void)applyPatternsAndFoldGreedily(funcOp, std::move(patterns));
    }
  };
} // end anonymous namespace

struct OptDenseTranspose : public ConversionPattern
{
  OptDenseTranspose(MLIRContext *ctx, uint64_t tile_size, bool seperate_tiles)
      : ConversionPattern(CopyOp::getOperationName(), 1, ctx),
        tile_size(tile_size), seperate_tiles(seperate_tiles) {}

  LogicalResult
  matchAndRewrite(Operation *input_op, ArrayRef<Value> operands,
                  ConversionPatternRewriter &rewriter) const final
  {
    comet_debug() << " OptDenseTranspose : public ConversionPattern\n";
    auto op = dyn_cast<linalg::CopyOp>(input_op);
    comet_debug() << " Lowering dense transpose\n";
    assert(isa<linalg::CopyOp>(op) &&
           "this operation is not CopyOp");

    assert(op.inputPermutation().hasValue() && op.outputPermutation().hasValue() &&
           "this copy operation does not have input and/or output permutation");

    // auto module = op->getParentOfType<ModuleOp>();
    Location loc = op.getLoc();
    comet_vdump(op);

    auto inputType = op->getOperand(0).getType();
    comet_debug() << " Input Type\n";
    comet_vdump(inputType);
    auto inputMemref = op->getOperand(0);
    auto outputMemref = op->getOperand(1);

    auto step = rewriter.create<ConstantIndexOp>(loc, 1);
    std::vector<AffineForOp> loops;
    std::vector<int64_t> indexIterateOrder;
    for (int64_t rank = 0; rank < inputType.cast<mlir::MemRefType>().getRank(); rank++)
    {
      indexIterateOrder.push_back(rank);
      auto upperBound = inputType.cast<mlir::MemRefType>().getDimSize(rank);
      if (upperBound == ShapedType::kDynamicSize)
      {
        assert(false && "TODO: This dimension is a dynamic size");
      }
      // create for loops
      auto loop = rewriter.create<AffineForOp>(loc, 0, upperBound, step);
      loops.push_back(loop);
      comet_vdump(loop);
      rewriter.setInsertionPointToStart(loop.getBody());
    }

    AffineMap inputIndexingMap = op.inputPermutation().getValue();
    auto inputIndices = getReassociationIndices(inputIndexingMap);
    auto inputIVs = createInductionVarAffine(loops, indexIterateOrder, inputIndices);

    AffineMap outputIndexingMap = op.outputPermutation().getValue();
    SmallVector<ReassociationIndices> outputIndices =
        getReassociationIndices(outputIndexingMap);
    auto outputIVs = createInductionVarAffine(loops, indexIterateOrder, outputIndices);

    // Build loop body
    auto load_rhs = rewriter.create<memref::LoadOp>(loc, inputMemref, inputIVs);
    #ifdef DEBUG_MODE_LINALGTRANSFORMS
    comet_vdump(load_rhs);
    auto store_lhs = rewriter.create<memref::StoreOp>(loc, load_rhs, outputMemref, outputIVs);
    comet_vdump(store_lhs);
    #else
    rewriter.create<memref::StoreOp>(loc, load_rhs, outputMemref, outputIVs);
    #endif

    // CopyOp index permutation
    AffineMap invmap = op.inputPermutation().getValue();
    ArrayRef<AffineExpr> invresults = invmap.getResults();
    std::vector<unsigned> sourceOrder;
    for (auto a : invresults)
    {
      if (a.getKind() == AffineExprKind::DimId)
      {
        AffineDimExpr *b = (AffineDimExpr *)&a; // down_casting
        sourceOrder.push_back(b->getPosition());
        comet_debug() << "Source order: " << b->getPosition() << "\n";
      }
    }

    AffineMap outvmap = op.outputPermutation().getValue();
    ArrayRef<AffineExpr> outvresults = outvmap.getResults();
    // From outer to inner, the destOrder[size -1] is the most important,
    std::vector<unsigned> destOrder;
    for (auto a : outvresults)
    {
      if (a.getKind() == AffineExprKind::DimId)
      {
        AffineDimExpr *b = (AffineDimExpr *)&a; // down_casting
        destOrder.push_back(b->getPosition());
        comet_debug() << "destination order: " << b->getPosition() << "\n";
      }
    }

    if (loops.size() > 0)
    {
      /* Suppose Given best order: a0, a3, a1, a2
      ** Then first step: a0, a1, a3, a2 (exchange loop index 1 and 2)
      ** Then second step: a0, a1, a2, a3 (exchange loop order index 2 and 3)
      */
      std::vector<unsigned> optimalOrder = destOrder;
      // Call an getLoopOrder algorithm to get the best order
      std::vector<std::vector<unsigned>> loopOrders;
      getLoopOrders(loopOrders, destOrder.size(), sourceOrder, destOrder);
      optimalOrder = loopOrders[0];

      std::vector<unsigned> currentOrder;
      for (unsigned i = 0; i < destOrder.size(); i++)
      {
        currentOrder.push_back(i);
      }

      for (unsigned i = 0; i < optimalOrder.size(); i++)
      {
        comet_debug() << "currentOrder[i]: " << currentOrder[i] << " optimalOrder[i]: " << optimalOrder[i] << "\n";
        // This loop index is the correct loop index, no loop interchange
        if (optimalOrder[i] == currentOrder[i])
        {
          continue;
        }
        else
        { // Get the location of the right loop index
          for (unsigned j = i + 1; j < currentOrder.size(); j++)
          {
            if (optimalOrder[i] == currentOrder[j])
            { // loop j and i exchange
              unsigned k = j;
              // k = (i,j]. k is unsigned, should be >= 0. use k-1, so k>=1
              while (k > 0 && k > i)
              {
                mlir::interchangeLoops(loops[currentOrder[k - 1]], loops[currentOrder[k]]);
                std::swap(currentOrder[k - 1], currentOrder[k]);
                k--;
              }
              break;
            }
          }
        }
      }

      std::vector<AffineForOp> newLoops;
      for (unsigned i = 0; i < currentOrder.size(); i++)
      {
        newLoops.push_back(loops[currentOrder[i]]);
      }
      loops.clear();

      // Possible to assign different tile size based on the dimension
      if (tile_size > 1)
      {
        std::vector<unsigned> tileSizes;
        for (unsigned i = 0; i < currentOrder.size(); i++)
        {
          tileSizes.push_back(tile_size);
        }
        // comet_vdump(newLoops[0]);
        SmallVector<AffineForOp, 6> tiledNest;
        if (failed(mlir::tilePerfectlyNested(newLoops, tileSizes, &tiledNest)))
          return failure();

        // llvm::errs() << __FILE__ << " " << __LINE__ << " after tiling\n";
        comet_vdump(tiledNest[0]);

        // Separate full and partial tiles.
        if (seperate_tiles)
        {
          auto intraTileLoops =
              MutableArrayRef<AffineForOp>(tiledNest).drop_front(newLoops.size());
          if (failed(separateFullTiles(intraTileLoops)))
            return failure();
        }
      } // end if (tilesize > 1)

    } // end loops.size() < 0

    rewriter.eraseOp(op);
    return success();
  }

private:
  uint64_t tile_size;
  bool seperate_tiles;
}; // Lower Dense Transpose to loops after optimizations

namespace
{
  class OptDenseTransposePass : public PassWrapper<OptDenseTransposePass, FunctionPass>
  {
  public:
    OptDenseTransposePass(uint64_t tile_size, bool seperate_tiles) : tile_size(tile_size), seperate_tiles(seperate_tiles){};
    void runOnFunction() final
    {
      comet_debug() << "OptDenseTransposePass : public PassWrapper<OptDenseTransposePass, FunctionPass>\n";
      ConversionTarget target(getContext());
      target.addLegalDialect<StandardOpsDialect, AffineDialect, memref::MemRefDialect>();
      OwningRewritePatternList patterns(&getContext());
      patterns.insert<OptDenseTranspose>(&getContext(), tile_size, seperate_tiles);

      if (failed(applyPartialConversion(getFunction(), target, std::move(patterns))))
      {
        llvm::errs() << "Failed to Lower dense transpose operation\n";
        signalPassFailure();
      }
      comet_debug() << "OptDenseTransposePass done\n";
    }

  private:
    uint64_t tile_size;
    bool seperate_tiles;
  };
} // end anonymous namespace

namespace
{
  class LowerLinAlgFillOpPass : public PassWrapper<LowerLinAlgFillOpPass, FunctionPass>
  {
  public:
    void runOnFunction() override
    {
      auto funcOp = getFunction();
      MLIRContext *ctx = funcOp.getContext();

      OwningRewritePatternList patterns(&getContext());

      // Add the patterns to the list lower linalg fill operation
      patterns.insert<LinalgLoweringPattern<FillOp>>(ctx, LinalgLoweringType::Loops);
      (void)applyPatternsAndFoldGreedily(funcOp, std::move(patterns));
    }
  };
} // end anonymous namespace

/// Create a pass to optimize LinAlg Matmul Op with tiling
std::unique_ptr<mlir::Pass> mlir::tensorAlgebra::createLinAlgMatmulTilingPass()
{
  return std::make_unique<LinAlgMatmulTilingPass>();
}

/// Create a pass to call a blis micro kernel for the inner linalg.matmul after tiling
std::unique_ptr<mlir::Pass> mlir::tensorAlgebra::createLinAlgMatmulMicroKernelPass()
{
  return std::make_unique<LinAlgMatmulMicroKernelPass>();
}

/// Create a pass to optimize LinAlg Copy Op - follow in HPTT paper
/// HPTT: A High-Performance Tensor Transposition C++ Library
/// https://arxiv.org/abs/1704.04374
std::unique_ptr<mlir::Pass> mlir::tensorAlgebra::createOptDenseTransposePass(uint64_t tile_size,
                                                                             bool seperate_tiles)
{
  comet_debug() << "LinAlgTransforms createOptDenseTransposePass\n";
  return std::make_unique<OptDenseTransposePass>(tile_size, seperate_tiles);
}

/// Create a pass to convert linalg.fill to loops
std::unique_ptr<mlir::Pass> mlir::tensorAlgebra::createLowerLinAlgFillPass()
{
  return std::make_unique<LowerLinAlgFillOpPass>();
}
