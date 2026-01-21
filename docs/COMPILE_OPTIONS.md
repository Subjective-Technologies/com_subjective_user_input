# Compilation Options for Windows

You have **two main options** for compiling `input_unified.c` on Windows. Both produce identical working executables.

---

## Option 1: Visual Studio (MSVC) ⭐ You Are Here

**Pros:**
- Native Windows toolchain
- Better debugging in Visual Studio IDE
- Integrates with other MSVC projects

**Cons:**
- Requires vcpkg setup (~15 minutes)
- More complex setup process

**Quick Start:**
See [`VISUAL_STUDIO_QUICK_START.md`](VISUAL_STUDIO_QUICK_START.md)

**Full Guide:**
See [`VISUAL_STUDIO_COMPILE_GUIDE.md`](VISUAL_STUDIO_COMPILE_GUIDE.md)

**Compile Command:**
```powershell
# From Developer Command Prompt for VS
.\compile_vs.ps1
```

---

## Option 2: MinGW (GCC)

**Pros:**
- Simpler setup (~10 minutes)
- Works with standard Linux-style toolchain
- Libraries install automatically

**Cons:**
- Separate toolchain from Visual Studio
- Less integration with VS debugger

**Quick Start:**
See [`COMPILE_INSTRUCTIONS_WINDOWS.md`](COMPILE_INSTRUCTIONS_WINDOWS.md)

**Compile Command:**
```powershell
# From regular PowerShell (after PATH is set)
.\compile.ps1
```

---

## Comparison Table

| Feature | Visual Studio (MSVC) | MinGW (GCC) |
|---------|---------------------|-------------|
| Setup Time | ~15 minutes | ~10 minutes |
| Binary Size | Smaller (~150KB) | Larger (~200KB) |
| Setup Complexity | Medium | Easy |
| VS Debugging | Excellent | Good |
| Library Management | vcpkg | pacman (MSYS2) |
| Executable Works? | ✅ Yes | ✅ Yes |
| Performance | ≈ Same | ≈ Same |

---

## Recommendation

- **If you're new to C compilation on Windows:** Use MinGW (Option 2)
- **If you need Visual Studio integration:** Use MSVC (Option 1)
- **If you just want it to work quickly:** Use MinGW (Option 2)
- **If you're already familiar with vcpkg:** Use MSVC (Option 1)

Both methods produce fully functional executables. The compiled program works identically regardless of which compiler you use.

---

## What You'll Get

After compilation, you'll have:
- `input_unified.exe` - The KVM client executable
- Required DLL files (websockets, OpenSSL, etc.)

You can then use it as described in:
- `WINDOWS_QUICK_START.md` - Usage guide
- `README.md` - Full documentation

---

## Already Have Visual Studio Installed?

Great! Here's your path forward:

1. **Quick**: [`VISUAL_STUDIO_QUICK_START.md`](VISUAL_STUDIO_QUICK_START.md) - Get compiling in 15 minutes
2. **Detailed**: [`VISUAL_STUDIO_COMPILE_GUIDE.md`](VISUAL_STUDIO_COMPILE_GUIDE.md) - Full explanations and IDE setup

---

## Need Help?

- **Visual Studio issues**: See troubleshooting in `VISUAL_STUDIO_COMPILE_GUIDE.md`
- **MinGW issues**: See troubleshooting in `COMPILE_INSTRUCTIONS_WINDOWS.md`
- **General Windows porting status**: See `WINDOWS_QUICK_START.md`







