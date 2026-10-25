; ngxforward.1: entries for EntryOverlayTests. Each starts with exactly one five-byte
; instruction (a relocated detour, never the hotpatch form), has five padding bytes
; above it for a short-jump patch, and returns (a + b) * 3 xor 5Ah.
.code

OVERLAY_TARGET MACRO name
    ALIGN 16
    db 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh
name PROC
    mov qword ptr [rsp+8], rbx
    mov eax, ecx
    add eax, edx
    imul eax, eax, 3
    xor eax, 5Ah
    mov rbx, qword ptr [rsp+8]
    ret
name ENDP
    db 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh, 0CCh
ENDM

OVERLAY_TARGET OverlayTargetCurrent
OVERLAY_TARGET OverlayTargetRel32
OVERLAY_TARGET OverlayTargetShort
OVERLAY_TARGET OverlayTargetAbsolute
OVERLAY_TARGET OverlayTargetRestored
OVERLAY_TARGET OverlayTargetOurs
OVERLAY_TARGET OverlayTargetTail
OVERLAY_TARGET OverlayTargetHotpatch

END
