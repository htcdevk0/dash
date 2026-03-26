# Dash Programming Language™

Official syntax and other documentation for the ***Dash Programming Language™***.

Made by: htcdevk0

Doc Version: v3.1.0
Dash Version: v6.0.0LL
Dash Repository Version: v2.0.6

---

## Get-started

To start using the ***Dash Programming Language™***, it is necessary to download and install the language. This can be done using *dashtup* or by manually compiling the binary and the standard library.

**Dashtup**:

- Access the official *dashtup* repository: https://github.com/htcdevk0/dashtup
- Download the installer release.
- Execute the installer with:

```bash
chmod +x ./dashtup
sudo ./dashtup install
```

* After installation, test the following code:

main.ds:

```dash
import [std/io];

fn main(): int {
    io.println("Hello, World!");
    return 0;
}
```

Run:

```bash
dash run main.ds # Or use build if you want to execute the final binary directly
```

---

## Overview:

The ***Dash Programming Language™*** aims to be a compiled programming language (LLVM) whose final binary must always be as portable as possible. The language seeks to function across all available architectures, including:
- x86 / x86_64
- ARM / AArch64
- RISC-V (32/64)
- WebAssembly (wasm32/wasm64)
- GPU (NVPTX, AMDGPU)
- Embedded (AVR, MSP430, Xtensa)
- Others (MIPS, PowerPC, SystemZ, BPF, Hexagon, VE)

The language offers libraries programmed in C (currently supporting 90% of GTK4) by making use of C FFI:

```dash
extern("c") <function name>(<params>): <return type>;
```

It also possesses Dash FFI (for dynamic and static libraries):

```dash
extern <function name>(<params>): <return type>;
```

It is very important to differentiate these two keywords, as each utilizes a different ABI (C & DASH).

---

## Types:

The ***Dash Programming Language™*** offers 6 different variable and function types, which are:

```dash
int      -> i32
uint     -> i64

float    -> float
double   -> double

bool     -> i1
char     -> i8

string   -> ptr (i8*)
```

Each type has its specific uses and was designed to fit current standards without the need for explicit types like u8, i8, i16, etc.

---

## Variables:

Variables in the ***Dash Programming Language*** have exposed mutability and typing; they feature lists and arrays for each type.

**Mutability:**
- `let` -> mutable
- `const` -> immutable

**Declarations:**
- `<mut> <name>: <type> = <value>;`

**Example:**
```dash
let x: int = 10;
let y: int = 20;
let xy: int = x + y;
```

---

## Modules:

Modules in the ***Dash Programming Language™*** are separated via classes or style imports. Standard libraries are separated by their name and their import:

**See:**
- **Stdio:** `io.<func>(<param>);`
- **Stdstr:** `str.<func>(<param>);`
- **Stros:** `os.<func>(<param>);` or `system.<func>(<param>);`

**IMPORTS:**
```dash
import [std/io];
import [std/str];
import [std/os];
import [std/fs];
```

All *standard* modules are created using static or dynamic implementations (*Smart Linking*) and they originate from:
- `/home/<user>/.dash/imports/<folder or module>`

Static objects come from:
- `/home/<user>/.dash/static/<lib<module>.o>`

Dynamic objects come from:
- `/home/<user>/.dash/lib/<lib<module>.so>`

You can access the STDLIB implementation repository at: ***[https://github.com/htcdevk0/Dash-Language-STDLIB](https://github.com/htcdevk0/Dash-Language-STDLIB)***

---

## Smart linking:

*Smart Linking* is the method of filtering static objects to dynamic ones at compile time using the ***Dash Programming Language™***. See:

```bash
dash build <file.ds> -sl # Smart Linking
```

It will search in the folder `/home/<user>/.dash/lib/<lib<same static module name>.so>`

**Diagram:**
> If the flag '-sl' is used, search for functions in the static modules, compare the static module name...
> Now search in the dynamic modules and compare the filenames; if they are the same, link the dynamic instead of the static.

---

## Groups:

*Groups* in the ***Dash Programming Language™*** are equivalent to:
```c
typedef struct . . .
```
in C.

They function as a definition of vector types:

```dash
group Vec3 {
    x: float,
    y: float,
    z: float
}

fn main(): int {
    let pos: Vec3 = {1.0, 2.0, 1.0};
    // Access
    let posX: float = pos.x;
    return 0;
}
```

---

## Lists and Arrays:

Arrays are famous for storing data in a way where a single variable can carry several of them, but they suffer from being entirely static in most cases. Lists solve this by storing the same data but with mutability.

**Full Example:**
```dash
import [std/io];

fn main() {
    // A List is created when the array is let/mutable
    let lista: string[/*optional max size*/] = {/*starts empty*/};
    
    // An Array is created when the variable is const and receives brackets
    const array: string[/*optional max size*/] = {"ABC", "DEF", "GHI"};

    // Add to the last index of the list
    lista::push("ABC"); // Insere at the last available index
    lista::push("DEF");
    
    // Remove
    lista::rem(1); // Removes the content of the specified index
    
    // Now only index 0 with "ABC" remains
    
    // Insert
    lista::insert(0, "DEF");
    // Now index 0 is "DEF"
    
    io.println(lista[0]);
    // Output: DEF
    
    // Array access:
    io.println($"0: {array[0]}, 1: {array[1]}, 2: {array[2]}");
}
```

---

## Builtin Functions:

Builtin functions in the ***Dash Programming Language™*** are used with the symbol ***::***

**Example:**
```dash
fn main(): int {
    let list: string[] = {/*starts empty*/};
    list::push("ABC"); // Pushes ABC at index 0 using the builtin function
    return 0;
}
```

---

## Templates:

Templates in the ***Dash Programming Language™*** are implemented via builtin.

```dash
env <<target>, <param>> -> env<"target", "linux">
    // Returns true if the operating system running the program matches the specified OS ("linux", "windows", "mac", "bsd", "unknown").

is<<type>, <var>> -> is<string, x>
    // Returns true if the variable type matches the specified type.

exdt<var>
    // Returns the data extracted from the variable.
```

---

## Pointers:

Pointers in the ***Dash Programming Language™*** are used in the same manner as the C language.

**Example:**
```dash
fn main(): int {
    let a: int = 10;
    let aPtr: int* = &a;
    // Create an integer pointer and receive the `a` address.
    return 0;
}
```

To send function pointers as parameters (usually with libraries imported via C FFI), it is done with:

```dash
function((function_thats_sends_the_pointer())); // It does NOT call the function!
```

---

## Classes:

Classes in the ***Dash Programming Language™*** are used with the keyword *class* and they possess the *static* type.

**Example:**
```dash
class My_class {
    let x: int;
    let y: int->10; // The operator -> initializes the variable. `=` is not accepted!

    fn getX(): int {
        return self.x; // The keyword self searches for the variable or function inside the parent class
    }

    fn updateY(a: int): void {
        self.y = a;
    }
}

// Usage:
fn main(): int {
    let my_class: My_class;
    let x: int = my_class.getX();
    my_class.updateY(20);
}
```

**Static classes:**
```dash
class My_class: static {
    let x: int;
    let y: int->10; // The operator -> initializes the variable. `=` is not accepted!

    fn getX(): int {
        return self.x; 
    }

    fn updateY(a: int): void {
        self.y = a;
    }
}

// Usage:
fn main(): int {
    let x: int = My_class.getX();
    My_class.updateY(20);

    // No instance needed
}
```

---

## String Interpolation:

String interpolation is usable **anywhere** that accepts double quotes `"` by using `$` before them and using `{variable, function, method, operation}` to insert values.

**Example:**
```dash
fn retString(str: string): string {
    return str;
}

fn main(): int {
    const x: int = 10;
    let str: string = $"X value: {x}";
    retString($"X value: {x}");
}
```

---

## Enums:

Enums are used to standardize values in a clear and readable manner. In the ***Dash Programming Language™***, they can be utilized as follows:

```dash
enum <Enum_name> {
    // Body
    Content1,
    Content2_With_Value = 1
}
```

**Example:**

```dash
import [std/io];

enum Colors {
    RED = 1,
    BLUE = 2,
    GREEN = 3
}

fn main(): int {
    let color: int;
    io.print("Insert the color [RED = 1, GREEN = 2, BLUE = 3]: ");
    io.sinp(&color);
    if (color == Colors.RED) {
        io.println("Red color!");
    } else if (color == Colors.BLUE) {
        io.println("Blue color!");
    } else if (color == Colors.GREEN) {
        io.println("Green color!");
    } else {
        io.println("Unknown color");
    }
    return 0;
}
```

---

## Loops:

Loops in ***Dash Programming Language™*** are used to repeat code based on conditions or ranges.

Dash supports:

- for loops (range-based iteration)
- while loops (condition-based)
- do-while loops (runs at least once)

**Example:**

*For loop*
```dash
import [std/io];

fn func(): void {
    const arr: string[5] = {"ABC", "DEF", "GHI", "JKL", "MNO"};
    for (let i: int = 0; i < 5; i++) {
        io.println($"Array content: {arr[i]}");
    }
}

fn main(): int {
    func();
    return 0;
}
```

*While loop*
```dash
import [std/io];

fn main(): int {
    let n: int = 0;
    while (n <= 50) {
        n++;
        io.println(n);
    }
    return 0;
}
```

*Do While loop*
```dash
import [std/io];

fn main(): int {
    let choice: int = 1000;
    do {
        io.println("Enter your choice (0 quit, 1.. stay): ");
        io.sinp(&choice);
    } while (choice != 0);
    return 0;
}
```

---

## Match:

```dash
import [std/io];

fn main(): int {
    let number: int = 1000;

    match (number) {
        0..10 -> io.println("Between 0 and 10")
        1000 -> io.println("Exactly 1000")
        _ -> io.println("Other value")
    }

    return 0;
}
```

---

## Switch:

```dash
import [std/io];

fn main(): int {
    let number: int = 1000;

    switch (number) {
        case 1:
            io.println("One");
            break;
        case 2:
            io.println("Two");
            break;
        default:
            io.println("Other");
            break;
    }

    return 0;
}
```

---

## Conditionals:

Conditionals in Dash are used to control program flow based on conditions.

**Example:**

```dash
import [std/io];

fn main(): int {
    let value: int = 10;

    if (value > 10) {
        io.println("Greater than 10");
    } else if (value == 10) {
        io.println("Equal to 10");
    } else {
        io.println("Less than 10");
    }

    return 0;
}
```
*Body:*
```dash
if (condition) {
    // code
} else if (condition) {
    // code
} else {
    // code
}
```

---

## `use`:

The `use` keyword allows you to access functions, classes, variables,
and other symbols from a module **without importing the entire
module**.\
This mechanism is designed to prevent **cyclic imports** and reduce
unnecessary module dependencies during compilation.

Instead of loading the full module into scope, `use` selectively exposes
only the requested symbols.

**Example:**

``` dash
use [std/io] {io}; // accesses only the 'io' class from std/io

fn main(): int {
    io.println("Hello, World");
    return 0;
}
```

**Forms:**

``` dash
use [module] {symbol1, symbol2}; // selective symbols
use [module] {*};               // all symbols (full exposure)
use "./local" {symbol};        // local module access
```

**Behavior:** - Does not perform a full module import - Avoids cyclic
dependency issues - Reduces compile-time overhead - Provides
fine-grained control over symbol visibility

------------------------------------------------------------------------

## Annotations:

Annotations are an advanced feature that provide **additional metadata
and compile-time control** over functions (and potentially other
constructs).\
They do not directly affect runtime behavior but influence compiler
diagnostics and symbol organization.

Currently supported annotations:

``` dash
@Deprecated
<function>
    - Emits a compile-time warning when the function is used.
    - Indicates that the function is outdated and may be removed in future versions.

@Risky
<function>
    - Emits a compile-time warning indicating that the function may be unsafe,
      unstable, or prone to errors.

@Warning("warning here")
<function>
    - Emits a custom compile-time warning message when the function is used.
```

**Example:**

``` dash
@Deprecated
fn oldFunc(): void {}

@Risky
fn unstable(): void {}

@Warning("This function is experimental")
fn experimental(): void {}

fn main(): int {
    oldFunc();
    unstable();
    experimental();
    return 0;
}
```

---

## Namespaces

Namespaces in Dash are used to organize code in a clean, structured, and scalable way.
They help avoid naming conflicts and can be used as an alternative to classes for logical grouping and hierarchy.

**Example:**

```dash
import [std/io];

namespace "math" {
    namespace "sum" {
        fn add(n: int, z: int): int {
            return n + z;
        }
    }
}

fn main(): int {
    io.println(math::sum::add(10, 20));
    return 0;
}
```

---

## CLI:

**Usage:**
- `dash <input.ds> [options]`
- `dash build <input.ds> [options]`
- `dash run <input.ds> [options]`

**General:**
- `--version, -v`   Show compiler version
- `--license, -lic` Show license
- `--author, -aut`  Show author (htcdevk0)

**Build:**
- `-o <path>`          Output path
- `--emit-llvm`        Emit LLVM IR (.ll)
- `-obj / -c`          Emit object file
- `--shared`           Build shared library (.so/.dll/.dylib)
- `--clang <path>`     Use custom clang for linking

**Linking:**
- `-L=<profile>`      Extra link profile (e.g., gtk4)
- `-cl <file>`        Statically link object/archive
- `-ld<path>`         Add linker search directory
- `-l<name>`          Link native library

**Runtime:**
- `-d`                Link shared libs from ~/.dash/lib
- `-sl`               Smart static/dynamic linking

**Native:**
- `<file.so|.dll|.dylib|.a|.o>`  Add native object/library to link

**Repositories:**
- [https://github.com/htcdevk0/dash](https://github.com/htcdevk0/dash)
- [https://github.com/htcdevk0/Dash-Language-STDLIB](https://github.com/htcdevk0/dash-stdlib)
- [https://github.com/htcdevk0/dashtup](https://github.com/htcdevk0/dashtup)

---

## License:

The ***Dash Programming Language™*** uses the ***GNU GPLv3*** license, as do all of its other modules.

---

## Author:

Currently, the ***Dash Programming Language™*** is created and maintained by htcdevk0 (Brazilian Developer, Pietro De Paiva).