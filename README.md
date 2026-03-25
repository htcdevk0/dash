# Dash Programming Language™

<img src="./assets/logo.svg" width="400">

Dash is a compiled programming language based on LLVM, designed for portability, small binaries, and low-level control.

Version: v5.2.0LL, Repository Version: v2.0.5

Patches:

- Added samples for the language

---

## What is Dash

Dash focuses on:

- portable binaries across architectures  
- minimal runtime and small output size  
- explicit control over linking (static and dynamic)  
- direct interoperability with C and native code  

Supported targets:

x86, x86_64, ARM, AArch64, RISC-V, WebAssembly, GPU backends, and embedded platforms.

---

## Example

```dash
import [std/io];

fn main(): int {
    let x: int = 10;
    let y: int = 20;

    io.println($"result: {x + y}");
    return 0;
}
```

## How to install

Check: https://github.com/htcdevk0/dashtup here you can find the dashtup binary and install dash on your Linux.

### Check the docs/DASH_<language\>.md or .txt for the full documentation