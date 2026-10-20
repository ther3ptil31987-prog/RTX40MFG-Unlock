; The wrapper's maximum clamp: mov edx, 5 / cmp ecx, edx / cmovb edx, ecx,
; emitted as bytes so the encoding is exactly the recognized profile.
_TEXT SEGMENT ALIGN(16) 'CODE'
FixtureClamp PROC
    db 0BAh, 05h, 00h, 00h, 00h, 3Bh, 0CAh, 0Fh, 42h, 0D1h
    mov eax, edx
    ret
FixtureClamp ENDP
_TEXT ENDS
END
