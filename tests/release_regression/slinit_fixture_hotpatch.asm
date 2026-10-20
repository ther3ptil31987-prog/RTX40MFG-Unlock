; slInit with the entry of NVIDIA's sl.interposer.dll (mov [rsp+10h], rdx)
; behind five int3 bytes, so RTXMFG can hotpatch it while the loader lock is
; held. It forwards to the C++ fixture body.
EXTERN FixtureSlInitBody:PROC
; slSetD3DDevice with the Streamline 2.14 interposer entry (mov [rsp+8], rbx),
; also behind int3 padding.
EXTERN FixtureSlSetD3DDeviceBody:PROC

_TEXT SEGMENT ALIGN(16) 'CODE'
    ALIGN 16
    db 16 dup(0CCh)
slInitHotpatchable PROC
    mov [rsp+10h], rdx
    jmp FixtureSlInitBody
slInitHotpatchable ENDP
    ALIGN 16
    db 16 dup(0CCh)
slSetD3DDeviceHotpatchable PROC
    mov [rsp+8], rbx
    jmp FixtureSlSetD3DDeviceBody
slSetD3DDeviceHotpatchable ENDP
_TEXT ENDS
END
