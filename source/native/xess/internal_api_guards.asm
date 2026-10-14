option casemap:none
EXTERN XessInternalApiTarget:PROC
.code

; Opaque Microsoft x64 forwarding: preserve the original argument registers,
; SIMD arguments, caller stack arguments and return address. The C++ selector
; never calls an unpublished SDK API with a guessed signature.
INTERNAL_GUARD MACRO number
XessInternalGuard&number PROC FRAME
    sub rsp, 0B8h
    .allocstack 0B8h
    .endprolog
    mov [rsp+20h], rcx
    mov [rsp+28h], rdx
    mov [rsp+30h], r8
    mov [rsp+38h], r9
    movdqu [rsp+40h], xmm0
    movdqu [rsp+50h], xmm1
    movdqu [rsp+60h], xmm2
    movdqu [rsp+70h], xmm3
    mov rdx, rcx
    mov ecx, number
    mov r8, [rsp+0B8h]
    call XessInternalApiTarget
    mov [rsp+80h], rax
    mov rcx, [rsp+20h]
    mov rdx, [rsp+28h]
    mov r8, [rsp+30h]
    mov r9, [rsp+38h]
    movdqu xmm0, [rsp+40h]
    movdqu xmm1, [rsp+50h]
    movdqu xmm2, [rsp+60h]
    movdqu xmm3, [rsp+70h]
    mov rax, [rsp+80h]
    add rsp, 0B8h
    jmp rax
XessInternalGuard&number ENDP
ENDM
INTERNAL_GUARD 0
INTERNAL_GUARD 1
INTERNAL_GUARD 2
INTERNAL_GUARD 3
INTERNAL_GUARD 4
INTERNAL_GUARD 5
INTERNAL_GUARD 6
INTERNAL_GUARD 7
INTERNAL_GUARD 8
INTERNAL_GUARD 9
INTERNAL_GUARD 10
INTERNAL_GUARD 11
END
