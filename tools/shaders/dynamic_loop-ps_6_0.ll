;
; Input signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; SV_Position              0   xyzw        0      POS   float
; COLOR                    0   xyzw        1     NONE   float   xyzw
; TEXCOORD                 0   xy          2     NONE   float   x
;
;
; Output signature:
;
; Name                 Index   Mask Register SysValue  Format   Used
; -------------------- ----- ------ -------- -------- ------- ------
; SV_Target                0   xyzw        0   TARGET   float   xyzw
;
; shader hash: cfb00ff3a0b5bcffae05b228001d14cd
;
; Pipeline Runtime Information:
;
;PSVRuntimeInfo:
; Pixel Shader
; DepthOutput=0
; SampleFrequency=0
; MinimumExpectedWaveLaneCount: 0
; MaximumExpectedWaveLaneCount: 4294967295
; UsesViewID: false
; SigInputElements: 3
; SigOutputElements: 1
; SigPatchConstOrPrimElements: 0
; SigInputVectors: 3
; SigOutputVectors[0]: 1
; SigOutputVectors[1]: 0
; SigOutputVectors[2]: 0
; SigOutputVectors[3]: 0
; EntryFunctionName: dynamic_loop
;
;
; Input signature:
;
; Name                 Index             InterpMode DynIdx
; -------------------- ----- ---------------------- ------
; SV_Position              0          noperspective
; COLOR                    0                 linear
; TEXCOORD                 0                 linear
;
; Output signature:
;
; Name                 Index             InterpMode DynIdx
; -------------------- ----- ---------------------- ------
; SV_Target                0
;
; Buffer Definitions:
;
;
; Resource Bindings:
;
; Name                                 Type  Format         Dim      ID      HLSL Bind  Count
; ------------------------------ ---------- ------- ----------- ------- -------------- ------
;
;
; ViewId state:
;
; Number of inputs: 10, outputs: 4
; Outputs dependent on ViewId: {  }
; Inputs contributing to computation of Outputs:
;   output 0 depends on inputs: { 4, 8 }
;   output 1 depends on inputs: { 5, 8 }
;   output 2 depends on inputs: { 6, 8 }
;   output 3 depends on inputs: { 7, 8 }
;
target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

define void @dynamic_loop() {
  %1 = call float @dx.op.loadInput.f32(i32 4, i32 2, i32 0, i8 0, i32 undef)  ; LoadInput(inputSigId,rowIndex,colIndex,gsVertexAxis)
  %2 = call float @dx.op.loadInput.f32(i32 4, i32 1, i32 0, i8 0, i32 undef)  ; LoadInput(inputSigId,rowIndex,colIndex,gsVertexAxis)
  %3 = call float @dx.op.loadInput.f32(i32 4, i32 1, i32 0, i8 1, i32 undef)  ; LoadInput(inputSigId,rowIndex,colIndex,gsVertexAxis)
  %4 = call float @dx.op.loadInput.f32(i32 4, i32 1, i32 0, i8 2, i32 undef)  ; LoadInput(inputSigId,rowIndex,colIndex,gsVertexAxis)
  %5 = call float @dx.op.loadInput.f32(i32 4, i32 1, i32 0, i8 3, i32 undef)  ; LoadInput(inputSigId,rowIndex,colIndex,gsVertexAxis)
  %6 = fptosi float %1 to i32
  %7 = icmp sgt i32 %6, 0
  br i1 %7, label %8, label %26

; <label>:8                                       ; preds = %0
  br label %9

; <label>:9                                       ; preds = %9, %8
  %10 = phi float [ %19, %9 ], [ %2, %8 ]
  %11 = phi float [ %20, %9 ], [ %3, %8 ]
  %12 = phi float [ %21, %9 ], [ %4, %8 ]
  %13 = phi float [ %22, %9 ], [ %5, %8 ]
  %14 = phi i32 [ %23, %9 ], [ 0, %8 ]
  %15 = fmul fast float %10, 7.500000e-01
  %16 = fmul fast float %11, 7.500000e-01
  %17 = fmul fast float %12, 7.500000e-01
  %18 = fmul fast float %13, 7.500000e-01
  %19 = fadd fast float %15, 0x3F847AE140000000
  %20 = fadd fast float %16, 0x3F847AE140000000
  %21 = fadd fast float %17, 0x3F847AE140000000
  %22 = fadd fast float %18, 0x3F847AE140000000
  %23 = add nuw nsw i32 %14, 1
  %24 = icmp slt i32 %23, %6
  br i1 %24, label %9, label %25, !llvm.loop !16

; <label>:25                                      ; preds = %9
  br label %26

; <label>:26                                      ; preds = %25, %0
  %27 = phi float [ %2, %0 ], [ %19, %25 ]
  %28 = phi float [ %3, %0 ], [ %20, %25 ]
  %29 = phi float [ %4, %0 ], [ %21, %25 ]
  %30 = phi float [ %5, %0 ], [ %22, %25 ]
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float %27)  ; StoreOutput(outputSigId,rowIndex,colIndex,value)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float %28)  ; StoreOutput(outputSigId,rowIndex,colIndex,value)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float %29)  ; StoreOutput(outputSigId,rowIndex,colIndex,value)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float %30)  ; StoreOutput(outputSigId,rowIndex,colIndex,value)
  ret void
}

; Function Attrs: nounwind readnone
declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) #0

; Function Attrs: nounwind
declare void @dx.op.storeOutput.f32(i32, i32, i32, i8, float) #1

attributes #0 = { nounwind readnone }
attributes #1 = { nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.viewIdState = !{!4}
!dx.entryPoints = !{!5}

!0 = !{!"dxcoob 1.9.2609.5 (01b62ad47)"}
!1 = !{i32 1, i32 0}
!2 = !{i32 1, i32 9}
!3 = !{!"ps", i32 6, i32 0}
!4 = !{[12 x i32] [i32 10, i32 4, i32 0, i32 0, i32 0, i32 0, i32 1, i32 2, i32 4, i32 8, i32 15, i32 0]}
!5 = !{void ()* @dynamic_loop, !"dynamic_loop", !6, null, null}
!6 = !{!7, !14, null}
!7 = !{!8, !10, !12}
!8 = !{i32 0, !"SV_Position", i8 9, i8 3, !9, i8 4, i32 1, i8 4, i32 0, i8 0, null}
!9 = !{i32 0}
!10 = !{i32 1, !"COLOR", i8 9, i8 0, !9, i8 2, i32 1, i8 4, i32 1, i8 0, !11}
!11 = !{i32 3, i32 15}
!12 = !{i32 2, !"TEXCOORD", i8 9, i8 0, !9, i8 2, i32 1, i8 2, i32 2, i8 0, !13}
!13 = !{i32 3, i32 1}
!14 = !{!15}
!15 = !{i32 0, !"SV_Target", i8 9, i8 16, !9, i8 0, i32 1, i8 4, i32 0, i8 0, !11}
!16 = distinct !{!16, !17}
!17 = !{!"llvm.loop.unroll.disable"}
