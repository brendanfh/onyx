window.ONYX_MODULES = window.ONYX_MODULES || [];

function push_event_to_buffer(esp, event_size, event_kind, data) {
    let WASM_U32 = new Uint32Array(ONYX_MEMORY.buffer);

    if (WASM_U32[esp] >= WASM_U32[esp + 1]) {
        console.log("Event buffer full!");
        return;
    }

    WASM_U32[esp] += 1;

    let event_idx = esp + (WASM_U32[esp] - 1) * (event_size / 4) + 2;
    WASM_U32[event_idx] = event_kind;
    WASM_U32[event_idx + 1] = Date.now();

    for (let i = 0; i < data.length; i++) {
        WASM_U32[event_idx + 2 + i] = data[i];
    }
}

window.ONYX_MODULES.push({
    module_name: "js_events",

    setup: function(esp, event_size) {
        // Indicies into a Uint32Array are not based on bytes,
        // but on the index.
        esp /= 4;

        document.addEventListener("keydown", function (ev) {
            if (ev.isComposing || ev.keyCode === 229) return;
            ev.preventDefault();

            // NOTE: These modifiers need to match in js_events.onyx.
            var modifiers = 0x0000;
            if (ev.ctrlKey)  modifiers |= 0x01;
            if (ev.altKey)   modifiers |= 0x02;
            if (ev.metaKey)  modifiers |= 0x04;
            if (ev.shiftKey) modifiers |= 0x08;

            push_event_to_buffer(esp, event_size, 0x04, [ ev.keyCode, modifiers ]);

            var keyname = ev.code;
            let WASM_U32 = new Uint32Array(ONYX_MEMORY.buffer);
            let event_idx = esp + (WASM_U32[esp] - 1) * (event_size / 4) + 2;

            let WASM_U8 = new Uint8Array(ONYX_MEMORY.buffer);

            for (var i = 0; i < keyname.length; i++) {
                WASM_U8[event_idx * 4 + (4 * 4) + i] = keyname.charCodeAt(i);
            }

            WASM_U8[event_idx * 4 + (4 * 4) + 15] = keyname.length;
            return false;
        });

        document.addEventListener("keyup", function (ev) {
            if (ev.isComposing || ev.keyCode === 229) return;
            ev.preventDefault();

            // NOTE: These modifiers need to match in js_events.onyx.
            var modifiers = 0x0000;
            if (ev.ctrlKey)  modifiers |= 0x01;
            if (ev.altKey)   modifiers |= 0x02;
            if (ev.metaKey)  modifiers |= 0x04;
            if (ev.shiftKey) modifiers |= 0x08;
            
            push_event_to_buffer(esp, event_size, 0x05, [ ev.keyCode, modifiers ]);

            var keyname = ev.code;
            let WASM_U32 = new Uint32Array(ONYX_MEMORY.buffer);
            let event_idx = esp + (WASM_U32[esp] - 1) * (event_size / 4) + 2;

            let WASM_U8 = new Uint8Array(ONYX_MEMORY.buffer);

            for (var i = 0; i < keyname.length; i++) {
                WASM_U8[event_idx * 4 + (4 * 4) + i] = keyname.charCodeAt(i);
            }

            WASM_U8[event_idx * 4 + (4 * 4) + 15] = keyname.length;

            return false;
        });

        document.addEventListener("mousedown", function (ev) {
            push_event_to_buffer(esp, event_size, 0x01, [ ev.clientX, ev.clientY, ev.button ]);
        });

        document.addEventListener("mouseup", function (ev) {
            push_event_to_buffer(esp, event_size, 0x02, [ ev.clientX, ev.clientY, ev.button ]);
        });

        document.addEventListener("mousemove", function (ev) {
            push_event_to_buffer(esp, event_size, 0x03, [ ev.clientX, ev.clientY, -1 ]);
        });

        document.addEventListener("wheel", function (ev) {
            push_event_to_buffer(esp, event_size, 0x07, [ ev.clientX, ev.clientY, ev.deltaY >= 0 ? 0x04 : 0x03 ]);
        });

        window.addEventListener("resize", function (ev) {
            push_event_to_buffer(esp, event_size, 0x06, [ window.innerWidth, window.innerHeight ]);
        });

        push_event_to_buffer(esp, event_size, 0x06, [ window.innerWidth, window.innerHeight ]);

        document.oncontextmenu = (e) => {
            e.preventDefault = true;
            return false;
        };
    },
});
