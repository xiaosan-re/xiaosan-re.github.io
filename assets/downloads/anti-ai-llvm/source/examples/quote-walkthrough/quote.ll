; ModuleID = 'anti_ai/examples/quote-walkthrough/quote.c'
source_filename = "anti_ai/examples/quote-walkthrough/quote.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx26.0.0"

@.str = private unnamed_addr constant [18 x i8] c"omvll.business-vm\00", section "llvm.metadata"
@.str.1 = private unnamed_addr constant [43 x i8] c"anti_ai/examples/quote-walkthrough/quote.c\00", section "llvm.metadata"
@llvm.global.annotations = appending global [1 x { ptr, ptr, ptr, i32, ptr }] [{ ptr, ptr, ptr, i32, ptr } { ptr @quote_demo, ptr @.str, ptr @.str.1, i32 6, ptr null }], section "llvm.metadata"

; Function Attrs: mustprogress nofree norecurse nosync nounwind ssp willreturn memory(none) uwtable(sync)
define i32 @quote_demo(i32 noundef %0, i32 noundef %1, i32 noundef %2, i32 noundef %3) #0 {
  %5 = mul i32 %0, 17
  %6 = add i32 %5, 9
  %7 = xor i32 %3, %6
  %8 = icmp eq i32 %7, 324508639
  br i1 %8, label %9, label %17

9:                                                ; preds = %4
  %10 = mul i32 %2, 5
  %11 = add i32 %10, 100
  %12 = mul i32 %11, %1
  %13 = icmp ugt i32 %1, 9
  %14 = udiv i32 %12, 10
  %15 = select i1 %13, i32 %14, i32 0
  %16 = sub i32 %12, %15
  br label %17

17:                                               ; preds = %4, %9
  %18 = phi i32 [ %16, %9 ], [ 0, %4 ]
  ret i32 %18
}

attributes #0 = { mustprogress nofree norecurse nosync nounwind ssp willreturn memory(none) uwtable(sync) "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+altnzcv,+ccdp,+ccidx,+ccpp,+complxnum,+crc,+dit,+dotprod,+flagm,+fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,+perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,+ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8a" }

!llvm.module.flags = !{!0, !1, !2, !3, !4}
!llvm.ident = !{!5}

!0 = !{i32 2, !"SDK Version", [2 x i32] [i32 26, i32 2]}
!1 = !{i32 1, !"wchar_size", i32 4}
!2 = !{i32 8, !"PIC Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 1}
!4 = !{i32 7, !"frame-pointer", i32 1}
!5 = !{!"Homebrew clang version 21.1.8"}
