; XplorerLite's JavaScript environment (src/js/prelude.js), embedded as a string
section .rodata
global js_prelude, js_prelude_end
js_prelude:
    incbin "src/js/prelude.js"
js_prelude_end:
    db 0
