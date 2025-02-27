//===- LateLowering.cpp------===//
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
// This file implements the lowering of some TA operations such as print, gettime, etc.
//===----------------------------------------------------------------------===//

#include "comet/Dialect/TensorAlgebra/IR/TADialect.h"
#include "comet/Dialect/TensorAlgebra/Passes.h"
#include "comet/Dialect/Utils/Utils.h"

#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Linalg/IR/LinalgTypes.h"
#include "mlir/Dialect/Linalg/EDSC/Intrinsics.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/StandardOps/EDSC/Intrinsics.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/Sequence.h"

#include "mlir/EDSC/Builders.h"
#include "mlir/IR/BuiltinOps.h"

#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"

using namespace mlir;
using namespace mlir::linalg;
using namespace mlir::edsc;
using namespace mlir::edsc::intrinsics;
using namespace mlir::tensorAlgebra;

// *********** For debug purpose *********//
// #ifndef DEBUG_MODE_LateLoweringPass
// #define DEBUG_MODE_LateLoweringPass
// #endif

#ifdef DEBUG_MODE_LateLoweringPass
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

//===----------------------------------------------------------------------===//
// Late lowering RewritePatterns
//===----------------------------------------------------------------------===//

namespace
{

  //===----------------------------------------------------------------------===//
  // Late Lowering to Standard Dialect RewritePatterns: Return operations
  //===----------------------------------------------------------------------===//
  struct ReturnOpLowering : public OpRewritePattern<tensorAlgebra::TAReturnOp>
  {
    using OpRewritePattern<tensorAlgebra::TAReturnOp>::OpRewritePattern;

    LogicalResult matchAndRewrite(tensorAlgebra::TAReturnOp op,
                                  PatternRewriter &rewriter) const final
    {
      // During this lowering, we expect that all function calls have been
      // inlined.
      if (op.hasOperand())
        return failure();

      // We lower "ta.return" directly to "std.return".
      rewriter.replaceOpWithNewOp<ReturnOp>(op);
      return success();
    }
  };

  /// Lowers `ta.print` to a loop nest calling `printf` on each of the individual
  /// elements of the array.
  class PrintOpLowering : public ConversionPattern
  {
  public:
    explicit PrintOpLowering(MLIRContext *context)
        : ConversionPattern(tensorAlgebra::PrintOp::getOperationName(), 1, context) {}

    LogicalResult
    matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                    ConversionPatternRewriter &rewriter) const override
    {

      Location loc = op->getLoc();
      auto module = op->getParentOfType<ModuleOp>();
      auto *ctx = op->getContext();
      FloatType f64Type = FloatType::getF64(ctx);
      IndexType indexType = IndexType::get(ctx);
      Type unrankedMemrefType_f64 = UnrankedMemRefType::get(f64Type, 0);

      auto printTensorF64Func = FunctionType::get(ctx, {mlir::UnrankedMemRefType::get(f64Type, 0)}, {});
      auto printTensorIndexFunc = FunctionType::get(ctx, {mlir::UnrankedMemRefType::get(indexType, 0)}, {});
      auto printScalarFunc = FunctionType::get(ctx, {FloatType::getF64(ctx)}, {});

      FuncOp print_func;
      auto inputType = op->getOperand(0).getType();

      // If the Input type is scalar (F64)
      if (inputType.isa<FloatType>())
      {
        std::string print_scalar_f64Str = "printF64";
        std::string print_newline_Str = "printNewline";
        if (isFuncInMod("printF64", module) == false)
        {
          print_func = FuncOp::create(loc, print_scalar_f64Str, printScalarFunc, ArrayRef<NamedAttribute>{});
          print_func.setPrivate();
          module.push_back(print_func);

          if (isFuncInMod("printNewline", module) == false)
          {
            auto printNewLineFunc = FunctionType::get(ctx, {}, {});
            FuncOp print_newline = FuncOp::create(loc, print_newline_Str, printNewLineFunc, ArrayRef<NamedAttribute>{});
            print_newline.setPrivate();
            module.push_back(print_newline);
          }
        }
        rewriter.create<mlir::CallOp>(loc, print_scalar_f64Str, SmallVector<Type, 2>{}, ValueRange{op->getOperand(0)});
        rewriter.create<mlir::CallOp>(loc, print_newline_Str, SmallVector<Type, 2>{}, ValueRange{});
      }
      else
      {
        std::string comet_print_f64Str = "comet_print_memref_f64";
        if (isFuncInMod(comet_print_f64Str, module) == false)
        {
          print_func = FuncOp::create(loc, comet_print_f64Str, printTensorF64Func, ArrayRef<NamedAttribute>{});
          print_func.setPrivate();
          module.push_back(print_func);
        }

        if (inputType.isa<MemRefType>())
        {
          auto alloc_op = cast<memref::AllocOp>(op->getOperand(0).getDefiningOp());
          auto u = rewriter.create<memref::CastOp>(loc, alloc_op, unrankedMemrefType_f64);
          rewriter.create<mlir::CallOp>(loc, comet_print_f64Str, SmallVector<Type, 2>{}, ValueRange{u});
        }
        else
        {
          // If the Input type is tensor
          if (inputType.isa<TensorType>())
          {
            auto rhs = op->getOperand(0).getDefiningOp();
            auto alloc_op = cast<memref::AllocOp>(rhs->getOperand(0).getDefiningOp());

            auto u = rewriter.create<memref::CastOp>(loc, alloc_op, unrankedMemrefType_f64);
            rewriter.create<mlir::CallOp>(loc, comet_print_f64Str, SmallVector<Type, 2>{}, ValueRange{u});
          }
          else if (inputType.isa<SparseTensorType>())
          {
            std::string comet_print_i64Str = "comet_print_memref_i64";

            if (isFuncInMod(comet_print_i64Str, module) == false)
            {
              print_func = FuncOp::create(loc, comet_print_i64Str, printTensorIndexFunc, ArrayRef<NamedAttribute>{});
              print_func.setPrivate();
              module.push_back(print_func);
            }

            // SparseTensorType includes 5 metadata per dimension. Additionally, 2 elements for value array, value array size.
            // TODO(gkestor): get tensor ranks by functions
            int tensorRanks = (op->getOperand(0).getDefiningOp()->getNumOperands() - 2) / 5;
            Type unrankedMemref_index = mlir::UnrankedMemRefType::get(indexType, 0);

            auto rhs = op->getOperand(0).getDefiningOp();
            for (int rsize = 0; rsize < tensorRanks; rsize++)
            {
              // accessing xD_pos array and creating cast op for its alloc
              auto xD_pos = rhs->getOperand(rsize * 2).getDefiningOp();
              auto alloc_rhs = cast<memref::AllocOp>(xD_pos->getOperand(0).getDefiningOp());
              auto u = rewriter.create<memref::CastOp>(loc, alloc_rhs, unrankedMemref_index);
              rewriter.create<mlir::CallOp>(loc, comet_print_i64Str, SmallVector<Type, 2>{}, ValueRange{u});

              // accessing xD_crd array and creating cast op for its alloc
              auto xD_crd = rhs->getOperand((rsize * 2) + 1).getDefiningOp();
              alloc_rhs = cast<memref::AllocOp>(xD_crd->getOperand(0).getDefiningOp());
              u = rewriter.create<memref::CastOp>(loc, alloc_rhs, unrankedMemref_index);
              rewriter.create<mlir::CallOp>(loc, comet_print_i64Str, SmallVector<Type, 2>{}, ValueRange{u});
            }

            auto xD_value = rhs->getOperand(tensorRanks * 2).getDefiningOp();
            auto alloc_rhs = cast<memref::AllocOp>(xD_value->getOperand(0).getDefiningOp());
            auto u = rewriter.create<memref::CastOp>(loc, alloc_rhs, unrankedMemrefType_f64);
            rewriter.create<mlir::CallOp>(loc, comet_print_f64Str, SmallVector<Type, 2>{}, ValueRange{u});
          }
          else
            llvm::errs() << __FILE__ << " " << __LINE__ << "Unknown Data type\n";
        }
      }

      // Notify the rewriter that this operation has been removed.
      rewriter.eraseOp(op);
      return success();
    }
  };

  class GetTimeLowering : public ConversionPattern
  {
  public:
    explicit GetTimeLowering(MLIRContext *ctx)
        : ConversionPattern(tensorAlgebra::GetTimeOp::getOperationName(), 1,
                            ctx) {}
    LogicalResult
    matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                    ConversionPatternRewriter &rewriter) const final
    {
      auto ctx = rewriter.getContext();
      auto module = op->getParentOfType<ModuleOp>();
      std::string getTimeStr = "getTime";
      auto f64Type = rewriter.getF64Type();

      if (!hasFuncDeclaration(module, getTimeStr))
      {
        auto getTimeFunc = FunctionType::get(ctx, {}, {FloatType::getF64(ctx)});
        // func @getTime() -> f64
        FuncOp func1 = FuncOp::create(op->getLoc(), getTimeStr,
                                      getTimeFunc, ArrayRef<NamedAttribute>{});
        func1.setPrivate();
        module.push_back(func1);
      }

      rewriter.replaceOpWithNewOp<mlir::CallOp>(op, getTimeStr, SmallVector<Type, 2>{f64Type});

      return success();
    }
  };

  class PrintElapsedTimeLowering : public ConversionPattern
  {
  public:
    explicit PrintElapsedTimeLowering(MLIRContext *ctx)
        : ConversionPattern(tensorAlgebra::PrintElapsedTimeOp::getOperationName(), 1, ctx) {}

    LogicalResult
    matchAndRewrite(Operation *op, ArrayRef<Value> operands,
                    ConversionPatternRewriter &rewriter) const override
    {
      auto ctx = rewriter.getContext();
      auto module = op->getParentOfType<ModuleOp>();

      auto start = operands[0];
      auto end = operands[1];
      std::string printElapsedTimeStr = "printElapsedTime";
      auto f64Type = rewriter.getF64Type();

      if (!hasFuncDeclaration(module, printElapsedTimeStr))
      {
        auto printElapsedTimeFunc = FunctionType::get(ctx, {f64Type, f64Type}, {});
        // func @printElapsedTime(f64, f64) -> ()
        FuncOp func1 = FuncOp::create(op->getLoc(), printElapsedTimeStr,
                                      printElapsedTimeFunc, ArrayRef<NamedAttribute>{});
        func1.setPrivate();
        module.push_back(func1);
      }

      rewriter.replaceOpWithNewOp<mlir::CallOp>(op, printElapsedTimeStr, SmallVector<Type, 2>{}, ValueRange{start, end});

      return success();
    }
  };

} // end anonymous namespace.

/// This is a partial lowering to linear algebra of the tensor algebra operations that are
/// computationally intensive (like matmul for example...) while keeping the
/// rest of the code in the TA dialect.
namespace
{
  struct LateLoweringPass
      : public PassWrapper<LateLoweringPass, FunctionPass>
  {
    void runOnFunction() final;
  };
} // end anonymous namespace.

void LateLoweringPass::runOnFunction()
{

  auto function = getFunction();

  // llvm::outs() << "Late lower input:\n" <<  function << "\n";
  //  Verify that the given main has no inputs and results.
  if (function.getNumArguments() || function.getType().getNumResults())
  {
    function.emitError("expected 'main' to have 0 inputs and 0 results");
    return signalPassFailure();
  }

  // The first thing to define is the conversion target. This will define the
  // final target for this lowering.
  ConversionTarget target(getContext());

  // We define the specific operations, or dialects, that are legal targets for
  // this lowering. In our case, we are lowering to a combination of the
  // `LinAlg` and `Standard` dialects.
  target.addLegalDialect<AffineDialect,
                         scf::SCFDialect,
                         StandardOpsDialect,
                         memref::MemRefDialect>();

  // Now that the conversion target has been defined, we just need to provide
  // the set of patterns that will lower the TA operations.
  // OwningRewritePatternList patterns;
  OwningRewritePatternList patterns(&getContext());
  patterns.insert<ReturnOpLowering,
                  PrintOpLowering,
                  GetTimeLowering,
                  PrintElapsedTimeLowering>(&getContext());

  // With the target and rewrite patterns defined, we can now attempt the
  // conversion. The conversion will signal failure if any of our `illegal`
  // operations were not converted successfully.
  
  if (failed(applyPartialConversion(getFunction(), target, std::move(patterns))))
  {
    signalPassFailure();
  }
}

/// Create a pass for lowering utility operations in tensor algebra to lower level dialects
std::unique_ptr<Pass> mlir::tensorAlgebra::createLateLoweringPass()
{
  return std::make_unique<LateLoweringPass>();
}
